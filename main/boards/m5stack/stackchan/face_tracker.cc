// P4 SPIKE ONLY (not kept). See face_tracker.h.
//
// The loop, while Listening: grab a frame, detect faces (r0, upright), let HeadAim decide a head
// move, then run the sound direction estimator over the audio the codec tapped since the last
// pass (all DOA math runs here, at priority 1, never in the audio input task).
//
// Frames come straight from EspVideo's own V4L2 fd and mmap buffer (the stream EspVideo starts
// at boot and never stops). EspVideo keeps those private and lives in boards/common, which this
// spike must not change, so this one translation unit opens it up with the classic
// `#define private public` trick. Every standard / C header esp_video.h pulls in is included
// first so only EspVideo's own class body sees the macro.

#include "sdkconfig.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <expected>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <errno.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <sys/ioctl.h>

// Same order as esp_video.cc: the real V4L2 header before jpg/image_to_jpeg.h, which defines
// fallback V4L2_PIX_FMT_* macros otherwise.
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "linux/videodev2.h"
#include "camera.h"
#include "jpg/image_to_jpeg.h"

#define private public
#include "esp_video.h"
#undef private

#include "human_face_detect.hpp"

#include "application.h"
#include "behavior_director.h"
#include "display.h"
#include "doa_estimator.h"
#include "face_tracker.h"
#include "head_aim.h"
#include "mic_tap.h"

namespace anan {

namespace {

constexpr char TAG[] = "FaceTracker";

// --- Tunables (spike) -----------------------------------------------------------------------
// Face: see AimConfig (head_aim.h) for FOV, signs, gain, deadband, settle times.
//
// Sound direction sign: DoaEstimator's angle is positive when the sound reaches mic 2 (ES7210
// MIC2, U13) first. +1 turns the head to positive yaw for that. Which side of the head U13 is on
// is not documented (see the report); if the head turns AWAY from a voice when no face is in
// view, set this to -1.
constexpr float kDoaYawSign = +1.0f;
// Distance between the two microphones. NOT documented by M5Stack; best estimate (see the
// report). Only the angle uses it; the lag on the status line does not, so the real spacing
// can be measured: spacing = |lag at 90 deg| * 343 / 24000 m.
constexpr float kMicSpacingM = 0.040f;
constexpr int kAudioSampleRate = 24000;  // AUDIO_INPUT_SAMPLE_RATE (config.h)
constexpr uint32_t kTapFrames = 16384;   // 683 ms of stereo audio, 64 KB in PSRAM
constexpr int kMaxWindowsPerPass = 24;   // ~1 s of audio per tracker loop at most
constexpr int kDoaWindow = 1024;         // 42.7 ms

constexpr int64_t kReportMs = 5000;
constexpr UBaseType_t kPriority = 1;  // below opus_codec (2), audio_afe (3), audio_output (4)
constexpr BaseType_t kCore = 1;       // audio_input is pinned to core 0
constexpr uint32_t kStackBytes = 16 * 1024;

std::mutex g_cam_mutex;

int64_t NowMs() { return esp_timer_get_time() / 1000; }

class TrackedVideo : public EspVideo {
public:
    using EspVideo::EspVideo;

    bool Capture() override {
        std::lock_guard<std::mutex> lock(g_cam_mutex);
        return EspVideo::Capture();
    }

    // Copies the next frame into `buf` (grown in PSRAM as needed). `queued_ms`: when the V4L2
    // buffer the frame came in was last queued, i.e. no part of the frame is older than that
    // (the driver fills a buffer only while it is queued).
    bool Grab(uint8_t*& buf, size_t& cap, int& w, int& h, uint32_t& fmt, int64_t& queued_ms) {
        std::lock_guard<std::mutex> lock(g_cam_mutex);
        if (!streaming_on_ || video_fd_ < 0) {
            return false;
        }
        struct v4l2_buffer b = {};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (ioctl(video_fd_, VIDIOC_DQBUF, &b) != 0) {
            return false;
        }
        const size_t slot = b.index < kSlots ? b.index : kSlots - 1;
        queued_ms = queued_ms_[slot];
        size_t n = b.bytesused ? b.bytesused : mmap_buffers_[b.index].length;
        n = std::min(n, mmap_buffers_[b.index].length);
        if (n > cap || buf == nullptr) {
            heap_caps_free(buf);
            buf = static_cast<uint8_t*>(heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            cap = buf ? n : 0;
        }
        if (buf != nullptr) {
#ifdef CONFIG_XIAOZHI_ENABLE_CAMERA_ENDIANNESS_SWAP
            auto src16 = static_cast<const uint16_t*>(mmap_buffers_[b.index].start);
            auto dst16 = reinterpret_cast<uint16_t*>(buf);
            for (size_t i = 0; i < n / 2; i++) {
                dst16[i] = __builtin_bswap16(src16[i]);
            }
#else
            memcpy(buf, mmap_buffers_[b.index].start, n);
#endif
        }
        if (ioctl(video_fd_, VIDIOC_QBUF, &b) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF failed");
        }
        queued_ms_[slot] = NowMs();
        w = frame_.width;
        h = frame_.height;
        fmt = sensor_format_;
        return buf != nullptr && n >= static_cast<size_t>(w) * h * 2;
    }

private:
    // Before the tracker's first QBUF the buffer was queued at stream start (0 = very old).
    static constexpr size_t kSlots = 4;
    int64_t queued_ms_[kSlots] = {};
};

// The sensor data is UYVY (measured on hardware) and upright as is (r0, measured on hardware).
// Converted here to a half-size RGB888 image for esp-dl.
constexpr int kSmallW = 160;
constexpr int kSmallH = 120;

inline uint8_t Clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// src: 320x240 UYVY. dst: 160x120 RGB888 (every other pixel and line).
void ToSmallRgb(const uint8_t* src, uint8_t* dst) {
    for (int y = 0; y < kSmallH; ++y) {
        const uint8_t* line = src + static_cast<size_t>(y) * 2 * 320 * 2;
        uint8_t* d = dst + static_cast<size_t>(y) * kSmallW * 3;
        for (int x = 0; x < kSmallW; ++x) {
            const uint8_t* m = line + x * 4;  // one UYVY macropixel per output pixel
            int u = m[0] - 128, yy = m[1], v = m[2] - 128;
            d[0] = Clamp8(yy + ((359 * v) >> 8));
            d[1] = Clamp8(yy - ((88 * u + 183 * v) >> 8));
            d[2] = Clamp8(yy + ((454 * u) >> 8));
            d += 3;
        }
    }
}

struct Ctx {
    TrackedVideo* camera;
    BehaviorDirector* director;
    Display* display;
};

void Report(Display* display, const char* line) {
    ESP_LOGI(TAG, "%s", line);
    if (display == nullptr) {
        return;
    }
    std::string text(line);
    // The display belongs to the main task.
    Application::GetInstance().Schedule([display, text]() {
        if (Application::GetInstance().GetDeviceState() == kDeviceStateListening) {
            display->ShowNotification(text.c_str(), 4000);
        }
    });
}

char OwnerLetter(HeadOwner o) {
    switch (o) {
        case HeadOwner::Tracking: return 'T';
        case HeadOwner::Gesture: return 'G';
        case HeadOwner::Manual: return 'M';
        default: return 'H';
    }
}

void Send(BehaviorDirector* director, const AimDelta& d) {
    if (director != nullptr) {
        director->RequestFaceDelta(d.dyaw, d.dpitch);
    }
}

// Sound direction state, owned by the tracker task.
struct Doa {
    float* workspace = nullptr;
    int16_t* window = nullptr;
    DoaEstimator* est = nullptr;
    uint32_t read = 0;          // next tap frame to process
    uint32_t seen_commands = 0;
    // Status window.
    bool have = false;
    DoaResult last;             // last valid estimate in the window
    int overruns = 0;
};

void ProcessAudio(Doa& doa, HeadAim& aim, BehaviorDirector* director, bool tracking,
                  int64_t now) {
    MicTap& tap = GetMicTap();
    if (doa.est == nullptr || !tap.attached()) {
        return;
    }
    const uint32_t win = static_cast<uint32_t>(kDoaWindow);
    // The head moved: older delays describe another pose.
    if (aim.command_count() != doa.seen_commands) {
        doa.seen_commands = aim.command_count();
        doa.est->Reset();
    }
    uint32_t w = tap.written();
    if (w - doa.read > tap.safe()) {
        doa.read = w - tap.safe() + win;  // fell behind: skip to what is still safe
        ++doa.overruns;
    }
    const int64_t usable_from = aim.AudioUsableFrom();
    int processed = 0;
    while (w - doa.read >= win && processed < kMaxWindowsPerPass) {
        // Servo noise and the old pose: no DOA during a move and kAudioQuietMs after.
        if (tap.TimeOfMs(doa.read) < usable_from) {
            doa.read += win;
            continue;
        }
        if (!tap.Read(doa.read, win, doa.window)) {
            ++doa.overruns;
            doa.read = tap.written();
            break;
        }
        doa.read += win;
        doa.est->Process(doa.window);
        ++processed;
    }
    if (processed == 0) {
        return;
    }
    DoaResult r = doa.est->Current();
    if (r.valid) {
        doa.have = true;
        doa.last = r;
    }
    if (auto d = aim.OnDoa(now, r.valid, r.angle_deg, r.confidence, tracking)) {
        Send(director, *d);
    }
}

void Run(Ctx* ctx) {
    // Load on the first Listening so the load messages land in a serial capture that starts
    // after the USB port re-enumerates.
    while (Application::GetInstance().GetDeviceState() != kDeviceStateListening) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "loading model; internal free %u KB, psram free %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    int64_t t0 = NowMs();
    auto* detector = new HumanFaceDetect(HumanFaceDetect::MSRMNP_S8_V1, false);
    ESP_LOGI(TAG, "model loaded in %lld ms; internal free %u KB, psram free %u KB",
             (long long)(NowMs() - t0),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    // Sound direction: everything in PSRAM.
    Doa doa;
    {
        DoaConfig cfg;
        cfg.sample_rate = kAudioSampleRate;
        cfg.window = kDoaWindow;
        cfg.mic_spacing_m = kMicSpacingM;
        auto* ring = static_cast<int16_t*>(heap_caps_malloc(
            kTapFrames * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        doa.workspace = static_cast<float*>(heap_caps_malloc(
            DoaEstimator::WorkspaceFloats(cfg) * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        doa.window = static_cast<int16_t*>(heap_caps_malloc(
            kDoaWindow * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (ring != nullptr && doa.workspace != nullptr && doa.window != nullptr) {
            GetMicTap().Attach(ring, kTapFrames, kAudioSampleRate);
            doa.est = new DoaEstimator(cfg, doa.workspace);
            ESP_LOGI(TAG, "sound direction on (spacing guess %.0f mm, window %d)",
                     kMicSpacingM * 1000, kDoaWindow);
        } else {
            ESP_LOGE(TAG, "sound direction off: PSRAM allocation failed");
        }
    }

    AimConfig aim_cfg;
    aim_cfg.doa_yaw_sign = kDoaYawSign;
    HeadAim aim(aim_cfg);

    uint8_t* buf = nullptr;
    size_t cap = 0;
    uint8_t* small = nullptr;
    bool active = false;
    bool format_logged = false;
    int64_t window_start = 0, active_since_ms = 0;
    int frames = 0, face_frames = 0;
    int last_flips = 0;

    for (;;) {
        if (Application::GetInstance().GetDeviceState() != kDeviceStateListening) {
            if (active) {
                ESP_LOGI(TAG, "tracking off");
                active = false;
                GetMicTap().Enable(false);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        int64_t now = NowMs();
        if (!active) {
            ESP_LOGI(TAG, "tracking on");
            active = true;
            window_start = now;
            frames = face_frames = 0;
            active_since_ms = now;
            aim.Begin(now);
            aim.ClearWindowCounters();
            doa.have = false;
            doa.overruns = 0;
            if (doa.est != nullptr) {
                doa.est->Reset();
                doa.read = GetMicTap().written();
                GetMicTap().Enable(true);
            }
        }
        const bool tracking = ctx->director != nullptr && ctx->director->tracking_enabled();

        int w = 0, h = 0;
        uint32_t fmt = 0;
        int64_t queued_ms = 0;
        bool ok = ctx->camera->Grab(buf, cap, w, h, fmt, queued_ms);
        // Packed 4:2:2 in any of the names esp_video uses for it; the bytes are UYVY.
        bool usable = ok && w == 320 && h == 240 &&
                      (fmt == V4L2_PIX_FMT_YUV422P || fmt == V4L2_PIX_FMT_YUYV ||
                       fmt == V4L2_PIX_FMT_UYVY);
        if (ok && !format_logged) {
            ESP_LOGI(TAG, "frames: %dx%d fourcc 0x%08lx%s", w, h, (unsigned long)fmt,
                     usable ? "" : " - not the expected 320x240 YUV422; no face detection");
            format_logged = true;
        }
        if (usable && small == nullptr) {
            small = static_cast<uint8_t*>(
                heap_caps_malloc(kSmallW * kSmallH * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            usable = small != nullptr;
        }
        // A frame queued before this Listening session began may be minutes old.
        if (usable && queued_ms >= active_since_ms) {
            ToSmallRgb(buf, small);
            dl::image::img_t img = {.data = small,
                                    .width = static_cast<uint16_t>(kSmallW),
                                    .height = static_cast<uint16_t>(kSmallH),
                                    .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888};
            auto& results = detector->run(img);
            now = NowMs();
            ++frames;

            const dl::detect::result_t* best = nullptr;
            int best_area = 0;
            for (const auto& r : results) {
                if (r.box.size() < 4) {
                    continue;
                }
                int area = (r.box[2] - r.box[0]) * (r.box[3] - r.box[1]);
                if (best == nullptr || area > best_area) {
                    best = &r;
                    best_area = area;
                }
            }
            std::optional<HeadAim::Face> face;
            if (best != nullptr) {
                ++face_frames;
                face = HeadAim::Face{(best->box[0] + best->box[2]) * 0.5f / kSmallW - 0.5f,
                                     (best->box[1] + best->box[3]) * 0.5f / kSmallH - 0.5f};
            }
            if (auto d = aim.OnFrame(queued_ms, now, face, tracking)) {
                Send(ctx->director, *d);
            }
        } else if (!ok || !usable) {
            vTaskDelay(pdMS_TO_TICKS(100));  // no camera frames: still run the sound side
            now = NowMs();
        }

        ProcessAudio(doa, aim, ctx->director, tracking, now);

        if (aim.flips() != last_flips) {
            last_flips = aim.flips();
            ESP_LOGW(TAG, "yaw sign flipped at run time (now %+d); set AimConfig::yaw_sign",
                     (int)aim.yaw_sign());
        }

        if (now - window_start >= kReportMs) {
            const float secs = (now - window_start) / 1000.0f;
            Pose target{0, 0};
            const bool have_target =
                ctx->director != nullptr && ctx->director->HeadTarget(target);
            const char owner =
                ctx->director != nullptr ? OwnerLetter(ctx->director->owner()) : '-';
            char face_txt[20] = "c--";
            if (aim.has_face()) {
                snprintf(face_txt, sizeof(face_txt), "c%+.2f%+.2f", aim.last_face().cx,
                         aim.last_face().cy);
            }
            char delta_txt[16] = "d--";
            if (auto d = aim.last_face_delta()) {
                snprintf(delta_txt, sizeof(delta_txt), "d%+d%+d", d->dyaw, d->dpitch);
            }
            char doa_txt[24] = "L--";
            if (doa.have) {
                snprintf(doa_txt, sizeof(doa_txt), "L%+.1fq%d", doa.last.lag,
                         doa.last.confidence);
            }
            char line[192];  // ~60 used; sized for the worst case of every field
            snprintf(line, sizeof(line), "%c f%.1f %d/%d %s %s Y%dP%d m%d %s s%d h%u%s", owner,
                     frames / secs, face_frames, frames, face_txt, delta_txt,
                     have_target ? target.yaw_deg : 0, have_target ? target.pitch_deg : 0,
                     aim.face_moves(), doa_txt, aim.doa_moves(),
                     (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     aim.flips() > 0 ? " flip" : "");
            Report(ctx->display, line);
            window_start = now;
            frames = face_frames = 0;
            aim.ClearWindowCounters();
            doa.have = false;
        }
        // Let IDLE1 run (task watchdog) and lower-priority work breathe.
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

}  // namespace

Camera* CreateTrackedCamera(const esp_video_init_config_t& config) {
    return new TrackedVideo(config);
}

void StartFaceTracker(Camera* camera, BehaviorDirector* director, Display* display) {
    if (camera == nullptr) {
        ESP_LOGW(TAG, "no camera; face tracking off");
        return;
    }
    auto* ctx = new Ctx{static_cast<TrackedVideo*>(camera), director, display};
    auto entry = [](void* arg) {
        Run(static_cast<Ctx*>(arg));
        vTaskDelete(nullptr);
    };
    if (xTaskCreatePinnedToCoreWithCaps(entry, "face_track", kStackBytes, ctx, kPriority, nullptr,
                                        kCore, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
        ESP_LOGI(TAG, "task started (PSRAM stack %u, prio %u, core %d)", (unsigned)kStackBytes,
                 (unsigned)kPriority, (int)kCore);
        return;
    }
    ESP_LOGW(TAG, "PSRAM stack failed; trying internal");
    if (xTaskCreatePinnedToCore(entry, "face_track", kStackBytes, ctx, kPriority, nullptr,
                                kCore) != pdPASS) {
        ESP_LOGE(TAG, "task creation failed; face tracking off");
    }
}

}  // namespace anan
