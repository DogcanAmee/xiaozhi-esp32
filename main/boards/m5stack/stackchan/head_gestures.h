#pragma once

// Keyframe tables for head gestures (nod / shake / tilt / bow).
// No ESP-IDF dependency: compiled and tested on the host.

#include <cstdint>
#include <vector>

#include "behavior_core.h"

namespace anan {

struct Keyframe {
    Pose pose;
    uint32_t duration_ms;
};

int ClampGestureCount(int count);  // clamp to 1..3

// Keyframes for one gesture, relative to `base` (clamped first). The last
// keyframe is always the clamped base. Gesture::None -> empty.
// Tilt and Bow ignore `count`.
std::vector<Keyframe> BuildGesture(Gesture g, Pose base, int count);

}  // namespace anan
