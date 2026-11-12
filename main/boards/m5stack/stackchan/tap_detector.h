#pragma once

// Tap detection for the touch screen: pure logic, no ESP-IDF headers, host-tested.
//
// Fed one sample per poll (is anything touching the panel, and when). A release only counts
// after kReleaseSamples consecutive not-touched samples, so one failed read or a one-sample
// glitch in the middle of a finger-down does not end the touch. A tap is a touch whose first
// not-touched sample comes in under kTapMaxMs after it started; Update() returns true exactly
// once for it, when the release is confirmed. A second tap within kDebounceMs of the previous
// ACCEPTED tap is ignored - a rejected tap does not push the window out.

#include <cstdint>

namespace anan {

class TapDetector {
public:
    static constexpr int64_t kTapMaxMs = 500;
    static constexpr int64_t kDebounceMs = 300;

    static constexpr int kReleaseSamples = 2;

    bool Update(bool touched, int64_t now_ms) {
        if (touched) {
            if (!down_) {
                down_ = true;
                down_ms_ = now_ms;
            }
            misses_ = 0;
            return false;
        }
        if (!down_) {
            return false;
        }
        if (misses_ == 0) {
            first_up_ms_ = now_ms;
        }
        if (++misses_ < kReleaseSamples) {
            return false;
        }
        down_ = false;
        misses_ = 0;
        if (first_up_ms_ - down_ms_ >= kTapMaxMs) {
            return false;
        }
        if (has_accepted_ && first_up_ms_ - last_accepted_ms_ < kDebounceMs) {
            return false;
        }
        has_accepted_ = true;
        last_accepted_ms_ = first_up_ms_;
        return true;
    }

private:
    bool down_ = false;
    int64_t down_ms_ = 0;
    int misses_ = 0;           // consecutive not-touched samples while down_
    int64_t first_up_ms_ = 0;  // time of the first of them
    bool has_accepted_ = false;
    int64_t last_accepted_ms_ = 0;
};

}  // namespace anan
