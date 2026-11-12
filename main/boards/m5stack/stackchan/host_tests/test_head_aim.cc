// Host tests for HeadAim (P4 spike).

#include <cassert>
#include <cmath>
#include <cstdio>
#include <optional>

#include "head_aim.h"

using anan::AimConfig;
using anan::AimDelta;
using anan::HeadAim;
using Face = anan::HeadAim::Face;

namespace {

void settled_frames_only() {
    HeadAim aim;
    aim.Begin(0);
    auto d = aim.OnFrame(5000, 5300, Face{0.2f, 0}, true);
    assert(d && d->dyaw == -6 && d->dpitch == 0);  // default yaw sign -1, gain 0.5 of 12 deg
    // The next frame was queued before the move had finished and settled: ignored.
    assert(!aim.OnFrame(5300 + 250, 5600, Face{0.2f, 0}, true));
    assert(!aim.OnFrame(5300 + 399, 5900, Face{0.2f, 0}, true));
    // Settled: used.
    assert(aim.OnFrame(5300 + 400, 6200, Face{0.1f, 0}, true));
}

void deadband_and_pitch() {
    HeadAim aim;
    aim.Begin(0);
    assert(!aim.OnFrame(5000, 5300, Face{0.04f, 0.05f}, true));  // 2.4 and 2.25 deg
    auto d = aim.OnFrame(5000, 5300, Face{0.0f, -0.3f}, true);   // face high in the image
    assert(d && d->dyaw == 0 && d->dpitch == 7);                 // 13.5 * 0.5 -> look up
}

void not_tracking_does_nothing() {
    HeadAim aim;
    aim.Begin(0);
    assert(!aim.OnFrame(5000, 5300, Face{0.3f, 0.3f}, false));
    assert(!aim.OnDoa(9000, true, 40, 9, false));
}

void nudge_once_without_face() {
    HeadAim aim;
    aim.Begin(1000);
    assert(!aim.OnFrame(3000, 3500, std::nullopt, true));
    auto d = aim.OnFrame(4100, 4100, std::nullopt, true);
    assert(d && d->dyaw == 0 && d->dpitch == 20);
    assert(!aim.OnFrame(9000, 9000, std::nullopt, true));
    aim.Begin(20000);  // a new session may nudge again
    assert(!aim.OnFrame(21000, 21000, std::nullopt, true));
    assert(aim.OnFrame(23100, 23100, std::nullopt, true));
}

// Closed loop: a person at world yaw `person`, head at `head`; the camera reports
// cx = cam_sign * (person - head) / hfov. Returns the final error.
float Simulate(float cam_sign, float person, int frames, int* flips_out) {
    AimConfig cfg;
    HeadAim aim(cfg);
    float head = 0;
    int64_t t = 1000;
    aim.Begin(t);
    for (int i = 0; i < frames; ++i) {
        int64_t frame_ms = t;
        t += 280;  // one inference
        float cx = cam_sign * (person - head) / cfg.hfov_deg;
        std::optional<Face> face;
        if (std::fabs(cx) < 0.5f) {
            face = Face{cx, 0};
        }
        auto d = aim.OnFrame(frame_ms, t, face, true);
        if (d) {
            head = std::fmax(-45, std::fmin(45, head + d->dyaw));
        }
    }
    *flips_out = aim.flips();
    return person - head;
}

void converges_with_either_camera_sign() {
    int flips = 0;
    // Default sign matches a camera with cam_sign = -1 relation: converges, no flip.
    float err = Simulate(-1, 20, 40, &flips);
    std::printf("  matching sign: err %.1f flips %d\n", err, flips);
    assert(std::fabs(err) <= 7 && flips == 0);
    err = Simulate(-1, -25, 40, &flips);
    assert(std::fabs(err) <= 7 && flips == 0);
    // Wrong sign: the head turns away twice, flips, then converges.
    err = Simulate(+1, 15, 60, &flips);
    std::printf("  wrong sign: err %.1f flips %d\n", err, flips);
    assert(std::fabs(err) <= 7 && flips == 1);
    err = Simulate(+1, -12, 60, &flips);
    assert(std::fabs(err) <= 7 && flips == 1);
}

void one_bad_measurement_does_not_flip() {
    HeadAim aim;
    aim.Begin(0);
    int64_t t = 5000;
    assert(aim.OnFrame(t, t, Face{0.2f, 0}, true));  // -6
    t += 500;
    assert(aim.OnFrame(t, t, Face{0.3f, 0}, true));  // the person moved: worse once
    t += 500;
    assert(aim.OnFrame(t, t, Face{0.1f, 0}, true));  // better: counter reset
    t += 500;
    assert(aim.OnFrame(t, t, Face{0.2f, 0}, true));  // worse once more
    assert(aim.flips() == 0);
    assert(aim.yaw_sign() < 0);
}

void doa_after_face_lost_only() {
    HeadAim aim;
    aim.Begin(0);
    // A face at t=5000 (no move: centred).
    assert(!aim.OnFrame(5000, 5000, Face{0.0f, 0}, true));
    assert(!aim.OnDoa(5800, true, 40, 9, true));  // face seen 0.8 s ago
    auto d = aim.OnDoa(6100, true, 40, 9, true);
    assert(d && d->dyaw == 10 && d->dpitch == 0);  // clamped step, doa_yaw_sign +1
    assert(aim.doa_moves() == 1);
    // Rate limit and settle.
    assert(!aim.OnDoa(6300, true, 40, 9, true));
    assert(aim.AudioUsableFrom() == 6100 + 250 + 300);
    assert(!aim.OnDoa(6600, true, -40, 9, true));
    d = aim.OnDoa(6700, true, -22, 9, true);
    assert(d && d->dyaw == -10);
    // Gates: confidence, small angle.
    assert(!aim.OnDoa(8000, true, 40, 4, true));
    assert(!aim.OnDoa(8000, true, 14, 9, true));
    assert(!aim.OnDoa(8000, false, 40, 9, true));
    // A face again: the camera takes over.
    aim.OnFrame(8000, 8000, Face{0.0f, 0}, true);
    assert(!aim.OnDoa(8500, true, 40, 9, true));
}

}  // namespace

int main() {
    settled_frames_only();
    deadband_and_pitch();
    not_tracking_does_nothing();
    nudge_once_without_face();
    converges_with_either_camera_sign();
    one_bad_measurement_does_not_flip();
    doa_after_face_lost_only();
    std::puts("test_head_aim: all passed");
    return 0;
}
