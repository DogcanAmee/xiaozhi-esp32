#pragma once

// A pending "go rest" request from the self.conversation.end tool. It decides the moment the
// conversation should be closed: after the goodbye has been spoken, while the device listens.
// No ESP-IDF dependency: compiled and tested on the host.

#include <cstdint>

#include "behavior_core.h"

namespace anan {

// With no speech since the request, close this long after it (the LLM called the tool without
// saying goodbye).
inline constexpr int64_t kRestSpeechWaitMs = 4000;
// A pending request this old is dropped without closing.
inline constexpr int64_t kRestExpireMs = 30000;

// Not thread-safe: drive it from a single task.
class RestRequest {
public:
    // Arm a pending close. While one is pending, a second request keeps the first's timing.
    void Request(int64_t now_ms);

    // Call every iteration with the settled phase. Returns true exactly once per request, and
    // only while `phase` is Listening: on the first Listening after a Speaking phase seen since
    // the request, or kRestSpeechWaitMs after the request if no speech began. Idle or Other
    // drops the request (the conversation ended some other way); so does kRestExpireMs.
    bool Step(Phase phase, int64_t now_ms);

    bool pending() const { return pending_; }

private:
    bool pending_ = false;
    int64_t requested_ms_ = 0;
    bool saw_speaking_ = false;
};

}  // namespace anan
