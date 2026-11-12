#include "rest_request.h"

namespace anan {

void RestRequest::Request(int64_t now_ms) {
    if (pending_) {
        return;
    }
    pending_ = true;
    requested_ms_ = now_ms;
    saw_speaking_ = false;
    listening_since_.reset();
}

bool RestRequest::Step(Phase phase, int64_t now_ms) {
    if (!pending_) {
        return false;
    }
    const int64_t elapsed = now_ms - requested_ms_;
    if (elapsed >= kRestExpireMs) {
        pending_ = false;
        return false;
    }
    switch (phase) {
        case Phase::Idle:
        case Phase::Other:
            pending_ = false;  // the conversation already ended
            return false;
        case Phase::Speaking:
            saw_speaking_ = true;
            listening_since_.reset();  // she is still talking: wait for the next Listening
            return false;
        case Phase::Listening:
            if (saw_speaking_) {
                if (!listening_since_) {
                    listening_since_ = now_ms;
                }
                if (now_ms - *listening_since_ < kRestGraceMs) {
                    return false;  // give the server time to end the conversation itself
                }
            } else if (elapsed < kRestSpeechWaitMs) {
                return false;
            }
            pending_ = false;
            return true;
    }
    return false;
}

}  // namespace anan
