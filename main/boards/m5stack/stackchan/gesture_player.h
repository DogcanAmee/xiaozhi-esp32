#pragma once

// Plays one head gesture (a list of keyframes) one frame at a time.
// No ESP-IDF dependency: compiled and tested on the host.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "head_gestures.h"

namespace anan {

// Not thread-safe: drive it from a single task (the behavior director's).
class GesturePlayer {
public:
    // Called once per keyframe: ease the neck to `pose` over `duration_ms`.
    using StageMove = std::function<void(Pose pose, uint32_t duration_ms)>;

    // Extra time past the sum of keyframe durations before a gesture is finished anyway.
    static constexpr int64_t kWatchdogSlackMs = 500;

    explicit GesturePlayer(StageMove stage_move);

    // Replace whatever is playing (the old gesture never reports finished) and stage the
    // first keyframe now. An empty list reports finished on the next Tick().
    void Start(std::vector<Keyframe> frames, int64_t now_ms);

    // Stop playing. The cancelled gesture never reports finished.
    void Cancel();

    // Advance at most one keyframe. Returns true exactly once per started gesture: when its
    // last keyframe's time is up, or when the watchdog (sum of durations + kWatchdogSlackMs
    // after Start) expires first.
    bool Tick(int64_t now_ms);

    bool playing() const { return playing_; }

private:
    StageMove stage_move_;
    std::vector<Keyframe> frames_;
    size_t index_ = 0;
    int64_t frame_started_ms_ = 0;
    int64_t deadline_ms_ = 0;
    bool playing_ = false;
};

}  // namespace anan
