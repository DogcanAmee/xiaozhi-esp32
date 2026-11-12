// P4 SPIKE ONLY (not kept). See face_tracker.h.
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
#include "face_tracker.h"

namespace anan {

namespace {

constexpr char TAG[] = "FaceTracker";

// --- Tunables (spike) -----------------------------------------------------------------------
// Camera field of view (GC0308 on CoreS3, approximate).
constexpr float kHFovDeg = 60.0f;
constexpr float kVFovDeg = 45.0f;  // 60 * 240 / 320
// SIGN ASSUMPTIONS (flip here after the hardware test):
// yaw: the head's yaw is "negative = left" (robot's own left). With camera_hmirror=false a
// face in the right half of the image is assumed to be on the robot's right -> yaw increases.
constexpr float kYawSign = +1.0f;
// pitch: "larger = further up". A face in the upper half of the image (small y) -> pitch
// increases.
constexpr float kPitchSign = -1.0f;
constexpr float kGain = 0.5f;         // fraction of the measured angular error per step
constexpr float kLowPass = 0.6f;      // weight of the newest error sample
constexpr float kDeadbandDeg = 3.0f;  // ignore smaller errors
constexpr float kMaxStepDeg = 8.0f;   // per command
// After a move is sent the head needs DirectorLogic::kTrackingMoveMs (250) to get there;
// detections taken while it moves are counted but not acted on.
constexpr int64_t kSettleMs = 350;
constexpr int64_t kLostMs = 2000;     // no face this long -> stop sending (head holds)
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

    // Copies the next frame into `buf` (grown in PSRAM as needed).
    bool Grab(uint8_t*& buf, size_t& cap, int& w, int& h, uint32_t& fmt) {
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
        w = frame_.width;
        h = frame_.height;
        fmt = sensor_format_;
        return buf != nullptr && n >= static_cast<size_t>(w) * h * 2;
    }
};


// P4 spike v3: the sensor data is UYVY (odd bytes follow the light; measured on hardware).
// Convert it ourselves to a half-size RGB888 image and probe 4 orientations.
struct ProbeMode {
    int orient;  // 0 = as is, 1 = 180, 2 = 90 cw, 3 = 90 ccw
    const char* name;
};
const ProbeMode kProbeModes[] = {{0, "r0"}, {1, "r180"}, {2, "r90"}, {3, "r270"}};
constexpr int kProbeModeCount = sizeof(kProbeModes) / sizeof(kProbeModes[0]);
constexpr int kSmallW = 160;
constexpr int kSmallH = 120;

inline uint8_t Clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// src: 320x240 UYVY. dst: RGB888, 160x120 (orient 0/1) or 120x160 (orient 2/3).
void ToSmallRgb(const uint8_t* src, uint8_t* dst, int orient, int& ow, int& oh) {
    ow = (orient >= 2) ? kSmallH : kSmallW;
    oh = (orient >= 2) ? kSmallW : kSmallH;
    for (int y = 0; y < oh; ++y) {
        for (int x = 0; x < ow; ++x) {
            int sx, sy;
            switch (orient) {
                case 1: sx = kSmallW - 1 - x; sy = kSmallH - 1 - y; break;
                case 2: sx = y; sy = kSmallH - 1 - x; break;
                case 3: sx = kSmallW - 1 - y; sy = x; break;
                default: sx = x; sy = y; break;
            }
            const uint8_t* m = src + (static_cast<size_t>(sy) * 2 * 320 + sx * 2) * 2;
            int u = m[0] - 128, yy = m[1], v = m[2] - 128;
            uint8_t* d = dst + (static_cast<size_t>(y) * ow + x) * 3;
            d[0] = Clamp8(yy + ((359 * v) >> 8));
            d[1] = Clamp8(yy - ((88 * u + 183 * v) >> 8));
            d[2] = Clamp8(yy + ((454 * u) >> 8));
        }
    }
}

struct Ctx {
    TrackedVideo* camera;
    BehaviorDirector* director;
    Display* display;
};

bool PixType(uint32_t fmt, dl::image::pix_type_t& out) {
    switch (fmt) {
        case V4L2_PIX_FMT_YUYV:
        case V4L2_PIX_FMT_YUV422P:  // esp_video's DVP YUV422 is packed YUYV (see EspVideo)
            out = dl::image::DL_IMAGE_PIX_TYPE_YUYV;
            return true;
        case V4L2_PIX_FMT_UYVY:
            out = dl::image::DL_IMAGE_PIX_TYPE_UYVY;
            return true;
        case V4L2_PIX_FMT_RGB565:
            out = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;
            return true;
        case V4L2_PIX_FMT_RGB24:
            out = dl::image::DL_IMAGE_PIX_TYPE_RGB888;
            return true;
        default:
            return false;
    }
}

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

void Run(Ctx* ctx) {
    // P4 spike: load on the first Listening so the load messages land in a serial capture
    // that starts after the USB port re-enumerates.
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

    uint8_t* buf = nullptr;
    size_t cap = 0;
    bool active = false;
    bool format_logged = false;
    int64_t window_start = 0, last_face_ms = 0, last_send_ms = 0;
    int frames = 0, face_frames = 0, faces = 0, sends = 0;
    int64_t infer_ms_sum = 0;
    float lp_yaw = 0, lp_pitch = 0;
    bool lp_valid = false;
    uint8_t* small = nullptr;
    bool dumped = false;
    int dump_countdown = 0;
    int raw_results = 0;
    bool nudged = false;
    int64_t active_since_ms = 0;
    int probe_mode = 0;
    bool probe_locked = false;
    int probe_misses = 0;
    uint32_t mean_even = 0, mean_odd = 0;

    for (;;) {
        if (Application::GetInstance().GetDeviceState() != kDeviceStateListening) {
            if (active) {
                ESP_LOGI(TAG, "tracking off");
                active = false;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        int64_t now = NowMs();
        if (!active) {
            ESP_LOGI(TAG, "tracking on");
            active = true;
            window_start = now;
            frames = face_frames = faces = sends = 0;
            raw_results = 0;
            infer_ms_sum = 0;
            lp_valid = false;
            last_face_ms = 0;
            nudged = false;
            active_since_ms = now;
        }

        int w = 0, h = 0;
        uint32_t fmt = 0;
        dl::image::pix_type_t pix;
        if (!ctx->camera->Grab(buf, cap, w, h, fmt) || !PixType(fmt, pix)) {
            if (!format_logged) {
                ESP_LOGE(TAG, "no usable frame (fmt 0x%08lx, %dx%d)", (unsigned long)fmt, w, h);
                format_logged = true;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (!format_logged) {
            ESP_LOGI(TAG, "frames: %dx%d fourcc 0x%08lx -> dl pix %d", w, h, (unsigned long)fmt,
                     (int)pix);
            format_logged = true;
        }

        bool packed422 = (w == 320 && h == 240) &&
                         (pix == dl::image::DL_IMAGE_PIX_TYPE_YUYV ||
                          pix == dl::image::DL_IMAGE_PIX_TYPE_UYVY);
        dl::image::img_t img = {.data = buf,
                                .width = static_cast<uint16_t>(w),
                                .height = static_cast<uint16_t>(h),
                                .pix_type = pix};
        int iw = w, ih = h;
        if (packed422) {
            uint32_t se = 0, so = 0, cnt = 0;
            for (size_t i = 0; i + 1 < static_cast<size_t>(w) * h * 2; i += 64) {
                se += buf[i];
                so += buf[i + 1];
                ++cnt;
            }
            mean_even = cnt ? se / cnt : 0;
            mean_odd = cnt ? so / cnt : 0;
            if (small == nullptr) {
                small = static_cast<uint8_t*>(heap_caps_malloc(
                    kSmallW * kSmallH * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            }
            if (small != nullptr) {
                ToSmallRgb(buf, small, kProbeModes[probe_mode].orient, iw, ih);
                img.data = small;
                img.width = static_cast<uint16_t>(iw);
                img.height = static_cast<uint16_t>(ih);
                img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;
            } else {
                packed422 = false;
            }
        }
        if (packed422 && !dumped && ++dump_countdown >= 20) {
            // P4 spike: dump one frame (sensor orientation, luma only, 160x120) as base64 so
            // it can be looked at off the device.
            dumped = true;
            static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            printf("\nIMGBEGIN %d %d\n", kSmallW, kSmallH);
            char line[81];
            int col = 0;
            for (int i = 0; i < kSmallW * kSmallH; i += 3) {
                // luma of 3 consecutive half-res pixels straight from the UYVY buffer
                auto Y = [&](int k) -> uint32_t {
                    int px = k % kSmallW, py = k / kSmallW;
                    return buf[(static_cast<size_t>(py) * 2 * 320 + px * 2) * 2 + 1];
                };
                uint32_t v = (Y(i) << 16) | (Y(i + 1 < kSmallW * kSmallH ? i + 1 : i) << 8) |
                             Y(i + 2 < kSmallW * kSmallH ? i + 2 : i);
                line[col++] = b64[(v >> 18) & 63];
                line[col++] = b64[(v >> 12) & 63];
                line[col++] = b64[(v >> 6) & 63];
                line[col++] = b64[v & 63];
                if (col >= 76) {
                    line[col] = 0;
                    printf("%s\n", line);
                    col = 0;
                    vTaskDelay(1);
                }
            }
            if (col > 0) {
                line[col] = 0;
                printf("%s\n", line);
            }
            printf("IMGEND\n");
        }
        int64_t ti = NowMs();
        auto& results = detector->run(img);
        raw_results += static_cast<int>(results.size());
        now = NowMs();
        infer_ms_sum += now - ti;
        ++frames;

        const dl::detect::result_t* best = nullptr;
        int best_area = 0;
        for (const auto& r : results) {
            if (r.box.size() < 4) {
                continue;
            }
            ++faces;
            int area = (r.box[2] - r.box[0]) * (r.box[3] - r.box[1]);
            if (best == nullptr || area > best_area) {
                best = &r;
                best_area = area;
            }
        }

        if (packed422) {
            if (best != nullptr) {
                if (!probe_locked) {
                    ESP_LOGI(TAG, "probe locked on %s", kProbeModes[probe_mode].name);
                }
                probe_locked = true;
                probe_misses = 0;
            } else if (!probe_locked) {
                probe_mode = (probe_mode + 1) % kProbeModeCount;
            } else if (++probe_misses > 200) {
                probe_locked = false;  // long without a face: search again
                probe_misses = 0;
            }
        }

        if (best != nullptr) {
            ++face_frames;
            if (last_face_ms == 0 || now - last_face_ms > kLostMs) {
                lp_valid = false;  // re-acquired: start the filter fresh
            }
            last_face_ms = now;
            if (now - last_send_ms >= kSettleMs) {
                float u = (best->box[0] + best->box[2]) * 0.5f / iw - 0.5f;  // -0.5..0.5
                float v = (best->box[1] + best->box[3]) * 0.5f / ih - 0.5f;
                float cx = u, cy = v;
                if (packed422) {  // map back to the sensor's orientation
                    switch (kProbeModes[probe_mode].orient) {
                        case 1: cx = -u; cy = -v; break;
                        case 2: cx = v; cy = -u; break;
                        case 3: cx = -v; cy = u; break;
                        default: break;
                    }
                }
                float ey = kYawSign * cx * kHFovDeg;
                float ep = kPitchSign * cy * kVFovDeg;
                if (!lp_valid) {
                    lp_yaw = ey;
                    lp_pitch = ep;
                    lp_valid = true;
                } else {
                    lp_yaw += kLowPass * (ey - lp_yaw);
                    lp_pitch += kLowPass * (ep - lp_pitch);
                }
                int dy = 0, dp = 0;
                if (std::fabs(lp_yaw) > kDeadbandDeg) {
                    dy = (int)std::lround(std::clamp(kGain * lp_yaw, -kMaxStepDeg, kMaxStepDeg));
                }
                if (std::fabs(lp_pitch) > kDeadbandDeg) {
                    dp = (int)std::lround(
                        std::clamp(kGain * lp_pitch, -kMaxStepDeg, kMaxStepDeg));
                }
                if ((dy != 0 || dp != 0) && ctx->director != nullptr &&
                    ctx->director->tracking_enabled()) {
                    ctx->director->RequestFaceDelta(dy, dp);
                    last_send_ms = now;
                    ++sends;
                    // What was just corrected is no longer in front of us.
                    lp_yaw -= dy;
                    lp_pitch -= dp;
                }
            }
        }
        else if (!nudged && now - active_since_ms > 3000 && ctx->director != nullptr &&
                 ctx->director->tracking_enabled()) {
            // P4 spike v3: the rest pose looks level; a seated user's face is usually above
            // the frame. Look up once per Listening session when nothing was found.
            ctx->director->RequestFaceDelta(0, 20);
            nudged = true;
            last_send_ms = now;
            ++sends;
        }

        if (now - window_start >= kReportMs) {
            float secs = (now - window_start) / 1000.0f;
            char line[128];
            snprintf(line, sizeof(line),
                     "fps %.1f face %d/%d raw %d mv %d %s%s e%u o%u heap %u/%uK",
                     frames / secs, face_frames, frames, raw_results, sends,
                     kProbeModes[probe_mode].name, probe_locked ? "*" : "?",
                     (unsigned)mean_even, (unsigned)mean_odd,
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
            Report(ctx->display, line);
            window_start = now;
            frames = face_frames = faces = sends = 0;
            raw_results = 0;
            infer_ms_sum = 0;
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
