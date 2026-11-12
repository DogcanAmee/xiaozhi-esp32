#include "head_gestures.h"

#include <algorithm>

namespace anan {

namespace {

// Pose = base offset by (dyaw, dpitch), clamped. A positive pitch delta that
// would exceed kPitchMax is flipped to the same magnitude, negative.
Pose Offset(Pose base, int dyaw, int dpitch) {
    if (base.pitch_deg + dpitch > kPitchMax) {
        dpitch = -dpitch;
    }
    return ClampPose({base.yaw_deg + dyaw, base.pitch_deg + dpitch});
}

void AddSwing(std::vector<Keyframe>& out, Pose base, int count, int amp, uint32_t first_ms,
              uint32_t second_ms) {
    for (int i = 0; i < count; ++i) {
        out.push_back({Offset(base, -amp, 0), first_ms});
        out.push_back({Offset(base, amp, 0), second_ms});
    }
    out.push_back({base, first_ms});
}

}  // namespace

int ClampGestureCount(int count) { return std::clamp(count, 1, 3); }

std::vector<Keyframe> BuildGesture(Gesture g, Pose base, int count) {
    std::vector<Keyframe> out;
    base = ClampPose(base);
    count = ClampGestureCount(count);
    switch (g) {
        case Gesture::None:
            break;
        case Gesture::Nod:
            for (int i = 0; i < count; ++i) {
                out.push_back({Offset(base, 0, 10), 220});
                out.push_back({base, 220});
            }
            break;
        case Gesture::Shake:
            AddSwing(out, base, count, 22, 240, 380);
            break;
        case Gesture::QuickShake:
            AddSwing(out, base, count, 14, 140, 200);
            break;
        case Gesture::Tilt: {
            Pose tilted = Offset(base, 10, 6);
            out.push_back({tilted, 400});
            out.push_back({tilted, 600});
            out.push_back({base, 400});
            break;
        }
        case Gesture::Bow: {
            Pose raised = Offset(base, 0, 8);
            out.push_back({raised, 600});
            out.push_back({raised, 500});
            out.push_back({base, 600});
            break;
        }
    }
    return out;
}

}  // namespace anan
