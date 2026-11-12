#pragma once

// Standby face animations: while the robot is idle with nobody owning the head, every
// kIdleGapMinMs..kIdleGapMaxMs play one short one-shot face animation, then go back to
// "neutral". Decides what and when only; the caller puts it on the screen.
// No ESP-IDF dependency: compiled and tested on the host.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace anan {

struct IdleAction {
    const char* name;  // emotion name in the asset pack; must outlive IdleActions
    uint32_t duration_ms;
};

inline constexpr int64_t kIdleGapMinMs = 20000;
inline constexpr int64_t kIdleGapMaxMs = 45000;

// Not thread-safe: drive it from a single task.
class IdleActions {
public:
    // `actions` may be empty (nothing ever plays) and must outlive this object. `rng(n)`
    // returns an integer in [0, n); tests inject a fixed sequence. An empty rng counts as
    // always 0.
    IdleActions(std::span<const IdleAction> actions, std::function<uint32_t(uint32_t n)> rng);

    struct Step {
        const char* play = nullptr;  // start this animation now
        bool restore = false;        // the action is over: show "neutral" again
    };

    // Once per director iteration. `enabled` = Outputs::idle_actions_enabled.
    Step Tick(bool enabled, int64_t now_ms);
    // Someone else just set a face (server, state change): drop the action in progress
    // without a restore, and start the gap again from now.
    void OnExternalEmotion(int64_t now_ms);
    bool playing() const { return playing_; }

private:
    uint32_t Random(uint32_t n);
    void StartGap(int64_t now_ms);
    size_t Pick();

    std::span<const IdleAction> actions_;
    std::function<uint32_t(uint32_t)> rng_;
    bool enabled_ = false;  // `enabled` at the previous Tick
    int64_t next_play_ms_ = 0;
    bool playing_ = false;
    int64_t play_end_ms_ = 0;
    std::optional<size_t> last_index_;
};

}  // namespace anan
