#ifdef NDEBUG
#error "asserts must be enabled in host tests"
#endif

#include <cassert>
#include <cstdio>

#include "rest_request.h"

using namespace anan;

namespace {

// Steps every 20 ms over [from, to] in `phase`; returns how many steps returned true.
int Run(RestRequest& r, Phase phase, int64_t from, int64_t to) {
    int closes = 0;
    for (int64_t t = from; t <= to; t += 20) {
        closes += r.Step(phase, t) ? 1 : 0;
    }
    return closes;
}

void nothing_pending_never_closes() {
    RestRequest r;
    assert(Run(r, Phase::Listening, 0, 40000) == 0);
    assert(!r.pending());
}

// The usual order: the tool call comes while Listening, then the goodbye is spoken.
void request_in_listening_closes_after_the_goodbye() {
    RestRequest r;
    r.Request(0);
    assert(r.pending());
    assert(Run(r, Phase::Listening, 0, 500) == 0);
    assert(Run(r, Phase::Speaking, 520, 3000) == 0);
    assert(r.Step(Phase::Listening, 3020));
    assert(!r.pending());
    assert(Run(r, Phase::Listening, 3040, 40000) == 0);
}

// The goodbye can run past the speech wait: once speech began, only Listening closes.
void long_goodbye_waits_for_listening() {
    RestRequest r;
    r.Request(0);
    assert(Run(r, Phase::Listening, 0, 1000) == 0);
    assert(Run(r, Phase::Speaking, 1020, 9000) == 0);
    assert(r.Step(Phase::Listening, 9020));
}

void request_while_speaking_closes_when_listening_comes() {
    RestRequest r;
    assert(!r.Step(Phase::Speaking, 0));
    r.Request(100);
    assert(Run(r, Phase::Speaking, 100, 2000) == 0);
    assert(r.Step(Phase::Listening, 2020));
    assert(Run(r, Phase::Listening, 2040, 10000) == 0);
}

// The LLM called the tool and said nothing.
void no_speech_closes_at_the_speech_wait() {
    RestRequest r;
    r.Request(1000);
    assert(Run(r, Phase::Listening, 1000, 1000 + kRestSpeechWaitMs - 20) == 0);
    assert(!r.Step(Phase::Listening, 1000 + kRestSpeechWaitMs - 1));
    assert(r.Step(Phase::Listening, 1000 + kRestSpeechWaitMs));
    assert(Run(r, Phase::Listening, 1000 + kRestSpeechWaitMs + 20, 40000) == 0);
}

void idle_while_pending_drops_the_request() {
    for (Phase ended : {Phase::Idle, Phase::Other}) {
        RestRequest r;
        r.Request(0);
        assert(Run(r, Phase::Speaking, 0, 1000) == 0);
        assert(!r.Step(ended, 1020));
        assert(!r.pending());
        assert(Run(r, Phase::Listening, 1040, 40000) == 0);
    }
    // Dropped before any speech, too.
    RestRequest r;
    r.Request(0);
    assert(!r.Step(Phase::Idle, 20));
    assert(Run(r, Phase::Listening, 40, 40000) == 0);
}

void never_closes_outside_listening() {
    RestRequest r;
    r.Request(0);
    assert(Run(r, Phase::Speaking, 0, kRestExpireMs + 1000) == 0);
}

void pending_request_expires() {
    RestRequest r;
    r.Request(0);
    assert(Run(r, Phase::Listening, 0, 200) == 0);
    assert(Run(r, Phase::Speaking, 220, kRestExpireMs - 20) == 0);
    assert(r.pending());
    assert(!r.Step(Phase::Listening, kRestExpireMs));
    assert(!r.pending());
    assert(Run(r, Phase::Listening, kRestExpireMs + 20, kRestExpireMs + 10000) == 0);
}

// A second request while pending keeps the first one's timing.
void double_request_keeps_first_timing() {
    RestRequest r;
    r.Request(0);
    assert(Run(r, Phase::Listening, 0, 3000) == 0);
    r.Request(3000);
    assert(Run(r, Phase::Listening, 3000, kRestSpeechWaitMs - 20) == 0);
    assert(r.Step(Phase::Listening, kRestSpeechWaitMs));  // not 7000

    // The first request's speech is kept as well.
    RestRequest s;
    s.Request(0);
    assert(Run(s, Phase::Speaking, 0, 1000) == 0);
    s.Request(1020);
    assert(s.Step(Phase::Listening, 1040));
}

// After a close or a drop, a new request starts fresh.
void later_request_works_again() {
    RestRequest r;
    r.Request(0);
    assert(Run(r, Phase::Speaking, 0, 1000) == 0);
    assert(r.Step(Phase::Listening, 1020));

    // Fresh timing and no carried-over speech: the next close needs speech or the full wait.
    r.Request(50000);
    assert(Run(r, Phase::Listening, 50000, 50000 + kRestSpeechWaitMs - 20) == 0);
    assert(r.Step(Phase::Listening, 50000 + kRestSpeechWaitMs));

    // After a drop.
    r.Request(60000);
    assert(!r.Step(Phase::Idle, 60020));
    r.Request(70000);
    assert(Run(r, Phase::Speaking, 70000, 71000) == 0);
    assert(r.Step(Phase::Listening, 71020));

    // After an expiry.
    r.Request(100000);
    assert(Run(r, Phase::Speaking, 100000, 100000 + kRestExpireMs) == 0);
    assert(!r.pending());
    r.Request(200000);
    assert(Run(r, Phase::Speaking, 200000, 201000) == 0);
    assert(r.Step(Phase::Listening, 201020));
}

}  // namespace

int main() {
    nothing_pending_never_closes();
    request_in_listening_closes_after_the_goodbye();
    long_goodbye_waits_for_listening();
    request_while_speaking_closes_when_listening_comes();
    no_speech_closes_at_the_speech_wait();
    idle_while_pending_drops_the_request();
    never_closes_outside_listening();
    pending_request_expires();
    double_request_keeps_first_timing();
    later_request_works_again();
    std::puts("test_rest_request: all passed");
    return 0;
}
