#ifdef NDEBUG
#error "asserts must be enabled in host tests"
#endif

#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

#include "idle_actions.h"

using namespace anan;

namespace {

constexpr IdleAction kThree[] = {{"a", 1000}, {"b", 2000}, {"c", 3000}};
constexpr IdleAction kOne[] = {{"solo", 1500}};

// Returns the scripted values in order (0 once they run out) and records every n it was
// asked for.
struct ScriptedRng {
    std::deque<uint32_t> values;
    std::vector<uint32_t> asked;

    std::function<uint32_t(uint32_t)> Fn() {
        return [this](uint32_t n) {
            asked.push_back(n);
            uint32_t v = 0;
            if (!values.empty()) {
                v = values.front();
                values.pop_front();
            }
            return v;
        };
    }
};

bool Is(const char* got, const char* want) { return got != nullptr && std::strcmp(got, want) == 0; }

// Ticks every 20 ms over [from, to]; collects what was played and how many restores came.
struct Run {
    std::vector<const char*> plays;
    std::vector<int64_t> play_times;
    int restores = 0;
    std::vector<int64_t> restore_times;
};

Run TickRange(IdleActions& ia, bool enabled, int64_t from, int64_t to) {
    Run r;
    for (int64_t t = from; t <= to; t += 20) {
        IdleActions::Step s = ia.Tick(enabled, t);
        if (s.play != nullptr) {
            r.plays.push_back(s.play);
            r.play_times.push_back(t);
        }
        if (s.restore) {
            ++r.restores;
            r.restore_times.push_back(t);
        }
    }
    return r;
}

void no_action_before_min_gap() {
    ScriptedRng rng;
    rng.values = {0};  // gap = kIdleGapMinMs
    IdleActions ia(kThree, rng.Fn());
    Run r = TickRange(ia, true, 0, kIdleGapMinMs - 20);
    assert(r.plays.empty());
    assert(r.restores == 0);
    assert(!ia.playing());
}

void plays_after_gap_and_restores_after_duration() {
    ScriptedRng rng;
    rng.values = {0, 1};  // gap 20 s, then pick "b" (2000 ms)
    IdleActions ia(kThree, rng.Fn());
    Run r = TickRange(ia, true, 0, kIdleGapMinMs);
    assert(r.plays.size() == 1);
    assert(Is(r.plays[0], "b"));
    assert(r.play_times[0] == kIdleGapMinMs);
    assert(ia.playing());
    assert(rng.asked.size() == 2);
    assert(rng.asked[0] == static_cast<uint32_t>(kIdleGapMaxMs - kIdleGapMinMs + 1));
    assert(rng.asked[1] == 3);

    r = TickRange(ia, true, kIdleGapMinMs + 20, kIdleGapMinMs + 1980);
    assert(r.plays.empty() && r.restores == 0);
    assert(ia.playing());
    IdleActions::Step s = ia.Tick(true, kIdleGapMinMs + 2000);
    assert(s.restore && s.play == nullptr);
    assert(!ia.playing());
    // A new gap starts at the restore: nothing before it is over.
    rng.values = {0};
    r = TickRange(ia, true, kIdleGapMinMs + 2020, kIdleGapMinMs + 2000 + kIdleGapMinMs - 20);
    assert(r.plays.empty() && r.restores == 0);
    s = ia.Tick(true, kIdleGapMinMs + 2000 + kIdleGapMinMs);
    assert(s.play != nullptr);
}

void does_not_repeat_same_action_twice_in_a_row() {
    ScriptedRng rng;
    // gap, pick 2 ("c"); gap, pick 2 again -> next one, wrapping to "a"; gap, pick 0 -> "a"
    // again -> "b".
    rng.values = {0, 2, 0, 2, 0, 0};
    IdleActions ia(kThree, rng.Fn());
    Run r = TickRange(ia, true, 0, 120000);
    assert(r.plays.size() >= 3);
    assert(Is(r.plays[0], "c"));
    assert(Is(r.plays[1], "a"));
    assert(Is(r.plays[2], "b"));
    for (size_t i = 1; i < r.plays.size(); ++i) {
        assert(std::strcmp(r.plays[i], r.plays[i - 1]) != 0);
    }
}

void single_action_list_may_repeat() {
    ScriptedRng rng;
    IdleActions ia(kOne, rng.Fn());
    Run r = TickRange(ia, true, 0, 60000);
    assert(r.plays.size() == 2);
    assert(Is(r.plays[0], "solo") && Is(r.plays[1], "solo"));
    assert(r.restores == 2);
    assert(r.restore_times[0] == r.play_times[0] + 1500);
}

void empty_list_never_plays() {
    ScriptedRng rng;
    IdleActions ia({}, rng.Fn());
    Run r = TickRange(ia, true, 0, 600000);
    assert(r.plays.empty() && r.restores == 0);
    assert(!ia.playing());

    IdleActions no_rng({}, nullptr);
    r = TickRange(no_rng, true, 0, 100000);
    assert(r.plays.empty() && r.restores == 0);
}

void disabled_while_playing_stops_without_restore() {
    ScriptedRng rng;
    IdleActions ia(kThree, rng.Fn());  // gap 20 s, "a" (1000 ms)
    Run r = TickRange(ia, true, 0, kIdleGapMinMs);
    assert(r.plays.size() == 1 && ia.playing());
    IdleActions::Step s = ia.Tick(false, kIdleGapMinMs + 20);
    assert(s.play == nullptr && !s.restore);
    assert(!ia.playing());
    r = TickRange(ia, false, kIdleGapMinMs + 40, 200000);
    assert(r.plays.empty() && r.restores == 0);
}

void timer_restarts_when_reenabled() {
    ScriptedRng rng;
    IdleActions ia(kThree, rng.Fn());  // every gap 20 s
    Run r = TickRange(ia, true, 0, 15000);
    assert(r.plays.empty());
    r = TickRange(ia, false, 15020, 17000);  // disabled: no timing
    assert(r.plays.empty());
    // Re-enabled at 17020: a full gap from there, not the 5 s left over.
    r = TickRange(ia, true, 17020, 17020 + kIdleGapMinMs - 20);
    assert(r.plays.empty());
    IdleActions::Step s = ia.Tick(true, 17020 + kIdleGapMinMs);
    assert(s.play != nullptr);
}

void external_emotion_cancels_and_restarts_timer() {
    ScriptedRng rng;
    IdleActions ia(kThree, rng.Fn());
    // Not playing: the timer restarts at the emotion.
    Run r = TickRange(ia, true, 0, 15000);
    ia.OnExternalEmotion(15000);
    r = TickRange(ia, true, 15020, 15000 + kIdleGapMinMs - 20);
    assert(r.plays.empty());
    IdleActions::Step s = ia.Tick(true, 15000 + kIdleGapMinMs);
    assert(s.play != nullptr && ia.playing());
    // Playing: cancelled without a restore, and a full gap again.
    const int64_t t = 15000 + kIdleGapMinMs + 200;
    ia.OnExternalEmotion(t);
    assert(!ia.playing());
    r = TickRange(ia, true, t, t + kIdleGapMinMs - 20);
    assert(r.plays.empty() && r.restores == 0);
    s = ia.Tick(true, t + kIdleGapMinMs);
    assert(s.play != nullptr);
}

void gap_is_within_20_to_45_seconds() {
    {
        ScriptedRng rng;
        rng.values = {0};
        IdleActions ia(kThree, rng.Fn());
        Run r = TickRange(ia, true, 0, 50000);
        assert(!r.play_times.empty() && r.play_times[0] == 20000);
    }
    {
        ScriptedRng rng;
        rng.values = {static_cast<uint32_t>(kIdleGapMaxMs - kIdleGapMinMs)};
        IdleActions ia(kThree, rng.Fn());
        Run r = TickRange(ia, true, 0, 50000);
        assert(r.play_times.size() == 1 && r.play_times[0] == 45000);
    }
}

}  // namespace

int main() {
    no_action_before_min_gap();
    plays_after_gap_and_restores_after_duration();
    does_not_repeat_same_action_twice_in_a_row();
    single_action_list_may_repeat();
    empty_list_never_plays();
    disabled_while_playing_stops_without_restore();
    timer_restarts_when_reenabled();
    external_emotion_cancels_and_restarts_timer();
    gap_is_within_20_to_45_seconds();
    std::puts("test_idle_actions: all passed");
    return 0;
}
