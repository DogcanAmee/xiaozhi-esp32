#pragma once

// Tap detection for the touch screen: pure logic, no ESP-IDF headers, host-tested.
//
// Fed one sample per poll (is anything touching the panel, and when). A tap is a touch that is
// released in under kTapMaxMs; Update() returns true exactly once for it, at the release. A
// second tap within kDebounceMs of the previous ACCEPTED tap is ignored - a rejected tap does
// not push the window out.

#include <cstdint>

namespace anan {

class TapDetector {
public:
    static constexpr int64_t kTapMaxMs = 500;
    static constexpr int64_t kDebounceMs = 300;

    bool Update(bool touched, int64_t now_ms) {
        if (touched) {
            if (!down_) {
                down_ = true;
                down_ms_ = now_ms;
            }
            return false;
        }
        if (!down_) {
            return false;
        }
        down_ = false;
        if (now_ms - down_ms_ >= kTapMaxMs) {
            return false;
        }
        if (has_accepted_ && now_ms - last_accepted_ms_ < kDebounceMs) {
            return false;
        }
        has_accepted_ = true;
        last_accepted_ms_ = now_ms;
        return true;
    }

private:
    bool down_ = false;
    int64_t down_ms_ = 0;
    bool has_accepted_ = false;
    int64_t last_accepted_ms_ = 0;
};

}  // namespace anan
