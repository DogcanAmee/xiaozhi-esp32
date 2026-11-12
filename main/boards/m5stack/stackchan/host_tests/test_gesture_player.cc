#ifdef NDEBUG
#error "asserts must be enabled in host tests"
#endif

#include <cassert>
#include <cstdio>
#include <vector>

#include "gesture_player.h"

using namespace anan;

namespace {

struct Staged {
    Pose pose;
    uint32_t duration_ms;
};

struct Recorder {
    std::vector<Staged> moves;
    GesturePlayer::StageMove fn() {
        return [this](Pose p, uint32_t ms) { moves.push_back({p, ms}); };
    }
};

bool Same(Pose a, Pose b) { return a.yaw_deg == b.yaw_deg && a.pitch_deg == b.pitch_deg; }

const std::vector<Keyframe> kThree = {{{0, 15}, 100}, {{0, 5}, 200}, {{10, 5}, 300}};

// Ticks every 20 ms from `from` to `to` inclusive; returns how many times finished was reported.
int TickRange(GesturePlayer& p, int64_t from, int64_t to) {
    int finished = 0;
    for (int64_t t = from; t <= to; t += 20) {
        if (p.Tick(t)) {
            ++finished;
        }
    }
    return finished;
}

void plays_all_keyframes_in_order_with_timing() {
    Recorder rec;
    GesturePlayer p(rec.fn());
    p.Start(kThree, 1000);
    // First keyframe is staged immediately.
    assert(rec.moves.size() == 1);
    assert(Same(rec.moves[0].pose, {0, 15}) && rec.moves[0].duration_ms == 100);
    assert(p.playing());

    // Not before its duration is up.
    assert(!p.Tick(1099));
    assert(rec.moves.size() == 1);
    // Second keyframe at 1100.
    assert(!p.Tick(1100));
    assert(rec.moves.size() == 2);
    assert(Same(rec.moves[1].pose, {0, 5}) && rec.moves[1].duration_ms == 200);
    // Third at 1300.
    assert(!p.Tick(1299));
    assert(!p.Tick(1300));
    assert(rec.moves.size() == 3);
    assert(Same(rec.moves[2].pose, {10, 5}) && rec.moves[2].duration_ms == 300);
    // Finished when the last keyframe's time is up.
    assert(!p.Tick(1599));
    assert(p.Tick(1600));
    assert(!p.playing());
    assert(rec.moves.size() == 3);
}

void reports_finished_exactly_once() {
    Recorder rec;
    GesturePlayer p(rec.fn());
    p.Start(kThree, 0);
    assert(TickRange(p, 0, 5000) == 1);
    assert(!p.Tick(9000));
    assert(rec.moves.size() == 3);
}

void cancel_stops_and_never_reports_finished() {
    Recorder rec;
    GesturePlayer p(rec.fn());
    p.Start(kThree, 0);
    assert(!p.Tick(100));  // second keyframe staged
    p.Cancel();
    assert(!p.playing());
    assert(TickRange(p, 120, 5000) == 0);
    assert(rec.moves.size() == 2);  // nothing staged after the cancel
}

void cancel_when_idle_is_harmless() {
    Recorder rec;
    GesturePlayer p(rec.fn());
    p.Cancel();
    assert(!p.playing());
    assert(!p.Tick(0));
    assert(rec.moves.empty());
}

void new_gesture_replaces_old_and_only_new_finishes() {
    Recorder rec;
    GesturePlayer p(rec.fn());
    p.Start(kThree, 0);
    assert(!p.Tick(100));  // old: second keyframe staged
    const std::vector<Keyframe> other = {{{-12, 5}, 150}, {{0, 5}, 150}};
    p.Start(other, 120);
    assert(rec.moves.size() == 3);
    assert(Same(rec.moves[2].pose, {-12, 5}) && rec.moves[2].duration_ms == 150);
    // Old gesture would have finished at 600. The new one stages its second keyframe on the
    // first tick at or after 270 (280) and finishes on the first tick at or after 430.
    int finished_at = -1;
    int finished = 0;
    for (int64_t t = 140; t <= 5000; t += 20) {
        if (p.Tick(t)) {
            ++finished;
            finished_at = static_cast<int>(t);
        }
    }
    assert(finished == 1);
    assert(finished_at == 440);
    // Only the new gesture's frames were staged after the replacement.
    assert(rec.moves.size() == 4);
    assert(Same(rec.moves[3].pose, {0, 5}));
}

void empty_list_finishes_on_next_tick() {
    Recorder rec;
    GesturePlayer p(rec.fn());
    p.Start({}, 500);
    assert(rec.moves.empty());
    assert(p.Tick(500));
    assert(!p.Tick(520));
    assert(!p.playing());
}

void empty_list_then_cancel_never_finishes() {
    Recorder rec;
    GesturePlayer p(rec.fn());
    p.Start({}, 500);
    p.Cancel();
    assert(!p.Tick(500));
}

void watchdog_finishes_a_stuck_gesture() {
    Recorder rec;
    GesturePlayer p(rec.fn());
    p.Start(kThree, 0);    // sum 600, watchdog at 1100
    assert(!p.Tick(100));  // second keyframe staged at 100
    // The driving task stalls; the next tick is past the watchdog.
    assert(p.Tick(1100));
    assert(!p.playing());
    assert(rec.moves.size() == 2);  // the remaining keyframe is dropped, not staged late
    assert(!p.Tick(1120));
}

void watchdog_not_triggered_by_normal_tick_jitter() {
    // Each keyframe staged one tick late (20 ms) still finishes normally, well within the slack.
    Recorder rec;
    GesturePlayer p(rec.fn());
    p.Start(kThree, 0);
    int finished = 0;
    for (int64_t t = 10; t <= 2000; t += 20) {
        if (p.Tick(t)) {
            ++finished;
            assert(t < 600 + GesturePlayer::kWatchdogSlackMs);
            break;
        }
    }
    assert(finished == 1);
    assert(rec.moves.size() == 3);
}

}  // namespace

int main() {
    plays_all_keyframes_in_order_with_timing();
    reports_finished_exactly_once();
    cancel_stops_and_never_reports_finished();
    cancel_when_idle_is_harmless();
    new_gesture_replaces_old_and_only_new_finishes();
    empty_list_finishes_on_next_tick();
    empty_list_then_cancel_never_finishes();
    watchdog_finishes_a_stuck_gesture();
    watchdog_not_triggered_by_normal_tick_jitter();
    std::puts("test_gesture_player: all passed");
    return 0;
}
