#include "idle_actions.h"

#include <utility>

namespace anan {

IdleActions::IdleActions(std::span<const IdleAction> actions,
                         std::function<uint32_t(uint32_t n)> rng)
    : actions_(actions), rng_(std::move(rng)) {}

uint32_t IdleActions::Random(uint32_t n) {
    if (n == 0 || !rng_) {
        return 0;
    }
    return rng_(n) % n;  // tolerate an rng that strays out of range
}

void IdleActions::StartGap(int64_t now_ms) {
    const auto span = static_cast<uint32_t>(kIdleGapMaxMs - kIdleGapMinMs + 1);
    next_play_ms_ = now_ms + kIdleGapMinMs + Random(span);
}

size_t IdleActions::Pick() {
    const auto n = static_cast<uint32_t>(actions_.size());
    size_t i = Random(n);
    if (n > 1 && last_index_ == i) {
        i = (i + 1) % n;
    }
    last_index_ = i;
    return i;
}

IdleActions::Step IdleActions::Tick(bool enabled, int64_t now_ms) {
    Step step;
    if (!enabled) {
        // Woken, head turned, ...: whoever disabled us sets the face; no restore.
        enabled_ = false;
        playing_ = false;
        return step;
    }
    if (!enabled_) {
        enabled_ = true;
        StartGap(now_ms);
        return step;
    }
    if (playing_) {
        if (now_ms >= play_end_ms_) {
            playing_ = false;
            step.restore = true;
            StartGap(now_ms);
        }
        return step;
    }
    if (actions_.empty() || now_ms < next_play_ms_) {
        return step;
    }
    const IdleAction& action = actions_[Pick()];
    playing_ = true;
    play_end_ms_ = now_ms + action.duration_ms;
    step.play = action.name;
    return step;
}

void IdleActions::OnExternalEmotion(int64_t now_ms) {
    playing_ = false;
    if (enabled_) {
        StartGap(now_ms);
    }
}

}  // namespace anan
