#ifdef NDEBUG
#error "asserts must be enabled in host tests"
#endif

#include <cassert>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

#include "director_logic.h"
#include "idle_actions.h"

using namespace anan;

namespace {

struct Staged {
    Pose pose;
    uint32_t duration_ms;
};

// Mirrors ServoNeck: Target() is the last staged pose, and Invalidate() leaves it there.
class FakeHead : public HeadPort {
public:
    explicit FakeHead(Pose start) : target_(start) {}
    bool Usable() const override { return usable; }
    void StageMove(Pose pose, uint32_t duration_ms) override {
        moves.push_back({pose, duration_ms});
        target_ = pose;
    }
    void Invalidate() override { ++invalidates; }
    Pose Target() const override { return target_; }

    bool usable = true;
    std::vector<Staged> moves;
    int invalidates = 0;

private:
    Pose target_;
};

bool Same(Pose a, Pose b) { return a.yaw_deg == b.yaw_deg && a.pitch_deg == b.pitch_deg; }

Outputs StepIn(DirectorLogic& d, int64_t now, std::vector<DirectorInput> in = {},
               std::optional<Phase> phase = std::nullopt) {
    return d.Step(now, phase, in);
}

// Steps every 20 ms over [from, to] with no inputs.
void Run(DirectorLogic& d, int64_t from, int64_t to) {
    for (int64_t t = from; t <= to; t += 20) {
        StepIn(d, t);
    }
}

// (a) A tool gesture request arrives in the same iteration in which the previous gesture's
// last keyframe expires. The finish must be reported for the old gesture only: the new one
// still owns the head afterwards, and a manual move cancels it.
void request_in_same_iteration_as_old_finish() {
    FakeHead head({0, 5});
    DirectorLogic d(&head);
    // Three nods (6 keyframes x 220 ms), so the old gesture ends after the tool-gesture gap.
    StepIn(d, 0, {DirectorInput::GestureRequest(Gesture::Nod, 3)});
    assert(d.gesture_playing());
    assert(head.moves.size() == 1);  // nod keyframe 0 (220 ms)
    // Keyframe i is staged on the tick at 220 * i; the last keyframe's time is up at 1320.
    Run(d, 20, 1300);
    assert(d.gesture_playing());
    assert(head.moves.size() == 6);

    StepIn(d, 1320, {DirectorInput::GestureRequest(Gesture::Shake, 1)});
    assert(d.core().owner() == HeadOwner::Gesture);
    assert(d.gesture_playing());
    assert(head.moves.size() == 7);
    assert(Same(head.moves[6].pose, {-22, 5}));  // shake's first keyframe, from base (0, 5)

    const int invalidates_before = head.invalidates;
    StepIn(d, 1340, {DirectorInput::ManualMove(30, 40)});
    assert(head.invalidates == invalidates_before + 1);
    assert(!d.gesture_playing());
    assert(d.core().owner() == HeadOwner::Manual);
    const size_t after_manual = head.moves.size();
    assert(Same(head.moves.back().pose, {30, 40}));
    assert(head.moves.back().duration_ms == DirectorLogic::kManualMoveMs);
    Run(d, 1360, 4000);
    assert(head.moves.size() == after_manual);  // no stale shake keyframes over the target
}

// (b) Replacing a nod mid-keyframe with a shake: the shake is built on the nod's base, not on
// the in-flight keyframe, so the head ends where it started.
void replacement_mid_keyframe_ends_at_original_base() {
    FakeHead head({0, 5});
    DirectorLogic d(&head);
    // Three nods; at 1000 keyframe 4 (0, 15), staged at 880, is in flight. The replacement
    // comes after the tool-gesture gap.
    StepIn(d, 0, {DirectorInput::GestureRequest(Gesture::Nod, 3)});
    Run(d, 20, 980);
    assert(Same(head.Target(), {0, 15}));
    const size_t before_shake = head.moves.size();
    StepIn(d, 1000, {DirectorInput::GestureRequest(Gesture::Shake, 1)});
    assert(head.invalidates == 1);
    assert(d.gesture_playing());
    Run(d, 1020, 4000);
    assert(!d.gesture_playing());
    assert(Same(head.moves.back().pose, {0, 5}));
    // Every shake keyframe is at the base pitch.
    for (size_t i = before_shake; i < head.moves.size(); ++i) {
        assert(head.moves[i].pose.pitch_deg == 5);
    }
}

// (c) A cancel with no replacement and no move_to returns the head to the gesture's base.
// Real path: a tool nod during a manual hold, then the device goes Idle. Idle cancels the
// gesture, and the return-to-rest is withheld while the manual hold lasts.
void cancel_without_replacement_returns_to_base() {
    FakeHead head({0, 5});
    DirectorLogic d(&head);
    StepIn(d, 0, {DirectorInput::ManualMove(20, 30)});
    assert(Same(head.Target(), {20, 30}));
    StepIn(d, 100, {DirectorInput::GestureRequest(Gesture::Nod, 1)});
    assert(Same(head.Target(), {20, 40}));  // nod keyframe 0 from base (20, 30)
    StepIn(d, 120, {}, Phase::Idle);        // settles on the first step at or after 270
    Run(d, 140, 280);                       // nod keyframe 1 would only be staged at 320
    assert(d.core().phase() == Phase::Idle);
    assert(!d.gesture_playing());
    assert(head.invalidates == 1);
    assert(Same(head.moves.back().pose, {20, 30}));
    assert(head.moves.back().duration_ms == DirectorLogic::kReturnToBaseMs);
    const size_t after_cancel = head.moves.size();
    Run(d, 300, 1000);  // still inside the manual hold: nothing else moves
    assert(head.moves.size() == after_cancel);
}

// (d) A cancel that comes with a manual move: the manual target alone decides the pose.
void cancel_with_manual_move_ends_at_manual_target() {
    FakeHead head({0, 5});
    DirectorLogic d(&head);
    StepIn(d, 0, {DirectorInput::GestureRequest(Gesture::Nod, 1)});
    StepIn(d, 100, {DirectorInput::ManualMove(30, 40)});
    assert(head.invalidates == 1);
    assert(!d.gesture_playing());
    assert(Same(head.moves.back().pose, {30, 40}));
    assert(head.moves.back().duration_ms == DirectorLogic::kManualMoveMs);
    // No return-to-base move was staged before or after the manual move.
    for (const Staged& m : head.moves) {
        assert(m.duration_ms != DirectorLogic::kReturnToBaseMs);
    }
    Run(d, 120, 3000);
    assert(Same(head.moves.back().pose, {30, 40}));
}

// A gesture with no usable head finishes at once, so the core does not stay in Gesture.
void unusable_head_finishes_gesture_at_once() {
    FakeHead head({0, 5});
    head.usable = false;
    DirectorLogic d(&head);
    StepIn(d, 0, {DirectorInput::GestureRequest(Gesture::Nod, 1)});
    assert(d.core().owner() != HeadOwner::Gesture);
    assert(head.moves.empty());

    DirectorLogic none(nullptr);
    StepIn(none, 0, {DirectorInput::GestureRequest(Gesture::Shake, 2)});
    assert(none.core().owner() != HeadOwner::Gesture);
}

// A gesture that plays to the end leaves the head at its base and owner back to Hold.
void finished_gesture_releases_head() {
    FakeHead head({10, 20});
    DirectorLogic d(&head);
    StepIn(d, 0, {DirectorInput::GestureRequest(Gesture::Shake, 1)});
    Run(d, 20, 2000);
    assert(!d.gesture_playing());
    assert(d.core().owner() == HeadOwner::Hold);
    assert(Same(head.moves.back().pose, {10, 20}));
    assert(head.invalidates == 0);
}

// Torque released while Idle settles: the one-shot return-to-rest is consumed with nothing to
// move. When the head is usable again, the rest move is re-issued exactly once.
void head_usable_again_in_idle_returns_to_rest_once() {
    FakeHead head({20, 30});
    head.usable = false;
    DirectorLogic d(&head);
    StepIn(d, 0, {}, Phase::Idle);
    Run(d, 20, 1000);
    assert(d.core().phase() == Phase::Idle);
    assert(head.moves.empty());
    head.usable = true;
    Run(d, 1020, 4000);
    assert(head.moves.size() == 1);
    assert(Same(head.moves[0].pose, kRestPose));
    assert(head.moves[0].duration_ms == kReturnToRestMs);
}

void head_usable_again_while_speaking_does_not_move() {
    FakeHead head({20, 30});
    head.usable = false;
    DirectorLogic d(&head);
    StepIn(d, 0, {}, Phase::Speaking);
    Run(d, 20, 1000);
    head.usable = true;
    Run(d, 1020, 4000);
    assert(head.moves.empty());
}

// Steps every 20 ms over [from, to] with no inputs; returns how many steps asked to end the
// conversation.
int CountEnds(DirectorLogic& d, int64_t from, int64_t to) {
    int ends = 0;
    for (int64_t t = from; t <= to; t += 20) {
        StepIn(d, t);
        ends += d.end_conversation() ? 1 : 0;
    }
    return ends;
}

// self.conversation.end: the request comes while Listening, the goodbye is spoken, and the
// close is asked for once, kRestGraceMs after Listening has settled again. A missing neck
// changes nothing.
void end_conversation_after_goodbye() {
    FakeHead head({0, 5});
    DirectorLogic with_neck(&head);
    DirectorLogic no_neck(nullptr);
    for (DirectorLogic* d : {&with_neck, &no_neck}) {
        StepIn(*d, 0, {}, Phase::Listening);
        assert(CountEnds(*d, 20, 380) == 0);
        StepIn(*d, 400, {DirectorInput::EndConversation()});
        assert(!d->end_conversation());
        assert(CountEnds(*d, 420, 980) == 0);
        StepIn(*d, 1000, {}, Phase::Speaking);
        assert(CountEnds(*d, 1020, 2980) == 0);
        StepIn(*d, 3000, {}, Phase::Listening);
        assert(!d->end_conversation());
        // The phase settles on the first step at or after 3150.
        assert(CountEnds(*d, 3020, 3140) == 0);
        StepIn(*d, 3160);
        assert(d->core().phase() == Phase::Listening);
        assert(!d->end_conversation());
        // Then it must stay Listening for the grace period.
        assert(CountEnds(*d, 3180, 3160 + kRestGraceMs - 20) == 0);
        StepIn(*d, 3160 + kRestGraceMs);
        assert(d->end_conversation());
        assert(CountEnds(*d, 3180 + kRestGraceMs, 40000) == 0);
    }
}

// The close follows the settled phase: a Listening blip shorter than the settle time in the
// middle of the goodbye does not close.
void end_conversation_ignores_unsettled_listening() {
    DirectorLogic d(nullptr);
    StepIn(d, 0, {}, Phase::Speaking);
    Run(d, 20, 180);  // Speaking settles at 160
    StepIn(d, 200, {DirectorInput::EndConversation()});
    assert(CountEnds(d, 220, 1000) == 0);
    StepIn(d, 1020, {}, Phase::Listening);
    assert(!d.end_conversation());
    StepIn(d, 1080, {}, Phase::Speaking);
    assert(!d.end_conversation());
    assert(CountEnds(d, 1100, 2000) == 0);
    assert(d.core().phase() == Phase::Speaking);
    StepIn(d, 2020, {}, Phase::Listening);
    assert(CountEnds(d, 2040, 2160) == 0);
    StepIn(d, 2180);
    assert(!d.end_conversation());
    assert(CountEnds(d, 2200, 2180 + kRestGraceMs - 20) == 0);
    StepIn(d, 2180 + kRestGraceMs);
    assert(d.end_conversation());
}

// Idle while the request waits: the conversation already ended, so nothing is closed, not
// even when a new conversation starts listening later.
void end_conversation_dropped_by_idle() {
    DirectorLogic d(nullptr);
    StepIn(d, 0, {}, Phase::Listening);
    Run(d, 20, 400);
    StepIn(d, 420, {DirectorInput::EndConversation()});
    StepIn(d, 1000, {}, Phase::Idle);
    assert(CountEnds(d, 1020, 3000) == 0);
    StepIn(d, 3020, {}, Phase::Listening);
    assert(CountEnds(d, 3040, 40000) == 0);
}

// --- Idle actions -------------------------------------------------------------------------

constexpr IdleAction kIdleTwo[] = {{"idle_wink", 1800}, {"idle_sleepy", 3200}};

// Always 0: every gap is kIdleGapMinMs, and the pick is the first action not just played.
uint32_t ZeroRng(uint32_t) { return 0; }

struct IdleSeen {
    std::vector<const char*> plays;
    std::vector<int64_t> play_times;
    std::vector<int64_t> restore_times;
};

// Steps every 20 ms over [from, to] with no inputs; collects idle_play / idle_restore.
void RunIdle(DirectorLogic& d, int64_t from, int64_t to, IdleSeen& seen) {
    for (int64_t t = from; t <= to; t += 20) {
        StepIn(d, t);
        if (d.idle_play() != nullptr) {
            seen.plays.push_back(d.idle_play());
            seen.play_times.push_back(t);
        }
        if (d.idle_restore()) {
            seen.restore_times.push_back(t);
        }
    }
}

// (a) Idle with the head free: one play after the gap, one restore after its duration.
void idle_action_plays_and_restores() {
    FakeHead head({20, 30});
    DirectorLogic d(&head, kIdleTwo, ZeroRng);
    StepIn(d, 0, {DirectorInput::Emotion("neutral")}, Phase::Idle);  // as the app does
    assert(d.idle_play() == nullptr && !d.idle_restore());
    IdleSeen seen;
    RunIdle(d, 20, 20000 + 3000, seen);  // Idle settles at 160; the gap ends at 20160
    assert(seen.plays.size() == 1);
    assert(std::strcmp(seen.plays[0], "idle_wink") == 0);
    assert(seen.play_times[0] == 160 + kIdleGapMinMs);
    assert(seen.restore_times.size() == 1);
    assert(seen.restore_times[0] == seen.play_times[0] + 1800);
    // The only head move is the return to rest on entering Idle (started off rest on
    // purpose); the idle action itself moves nothing.
    assert(head.moves.size() == 1);
    assert(Same(head.moves[0].pose, kRestPose));
    assert(head.moves[0].duration_ms == kReturnToRestMs);
}

// (b) Woken while an action plays: Listening stops it, and no restore is ever asked for.
void idle_action_stopped_by_listening_never_restores() {
    DirectorLogic d(nullptr, kIdleTwo, ZeroRng);
    StepIn(d, 0, {DirectorInput::Emotion("neutral")}, Phase::Idle);  // as the app does
    IdleSeen seen;
    RunIdle(d, 20, 160 + kIdleGapMinMs, seen);
    assert(seen.plays.size() == 1);
    StepIn(d, 160 + kIdleGapMinMs + 20, {}, Phase::Listening);
    RunIdle(d, 160 + kIdleGapMinMs + 40, 120000, seen);
    assert(d.core().phase() == Phase::Listening);
    assert(seen.plays.size() == 1);
    assert(seen.restore_times.empty());
}

// (c) An emotion that reaches the director in Idle (e.g. the app's own "neutral") restarts
// the timer: no play until a full gap after it.
void external_emotion_in_idle_restarts_idle_timer() {
    DirectorLogic d(nullptr, kIdleTwo, ZeroRng);
    StepIn(d, 0, {DirectorInput::Emotion("neutral")}, Phase::Idle);  // as the app does
    IdleSeen seen;
    RunIdle(d, 20, 10000, seen);
    StepIn(d, 10020, {DirectorInput::Emotion("neutral")});
    RunIdle(d, 10040, 10020 + kIdleGapMinMs - 20, seen);
    assert(seen.plays.empty());
    RunIdle(d, 10020 + kIdleGapMinMs, 10020 + kIdleGapMinMs, seen);
    assert(seen.plays.size() == 1);
}

// (d) No actions: nothing is ever emitted.
void empty_idle_list_emits_nothing() {
    DirectorLogic d(nullptr);
    StepIn(d, 0, {DirectorInput::Emotion("neutral")}, Phase::Idle);  // as the app does
    IdleSeen seen;
    RunIdle(d, 20, 300000, seen);
    assert(seen.plays.empty() && seen.restore_times.empty());
}

// (e) A manual head turn in Idle owns the head: nothing plays during the hold.
void manual_hold_in_idle_blocks_idle_actions() {
    FakeHead head({0, 5});
    DirectorLogic d(&head, kIdleTwo, ZeroRng);
    StepIn(d, 0, {DirectorInput::Emotion("neutral")}, Phase::Idle);  // as the app does
    IdleSeen seen;
    RunIdle(d, 20, 19000, seen);
    // Keep turning the head every few seconds, past the point the gap would have ended.
    for (int64_t t = 19020; t <= 60000; t += 20) {
        std::vector<DirectorInput> in;
        if ((t - 19020) % 5000 == 0) {
            in.push_back(DirectorInput::ManualMove(10, 20));
        }
        Outputs out = StepIn(d, t, in);
        assert(!out.idle_actions_enabled);
        assert(d.idle_play() == nullptr && !d.idle_restore());
    }
    assert(seen.plays.empty());
}

// (f) A face other than "neutral" in Idle (e.g. a network-error alert the app keeps visible)
// is left alone; the next "neutral" (DismissAlert, next Idle entry) lets idle actions resume
// after a full gap.
void non_neutral_face_in_idle_blocks_idle_actions() {
    DirectorLogic d(nullptr, kIdleTwo, ZeroRng);
    StepIn(d, 0, {DirectorInput::Emotion("neutral")}, Phase::Idle);
    IdleSeen seen;
    RunIdle(d, 20, 5000, seen);
    StepIn(d, 5020, {DirectorInput::Emotion("sad")});
    RunIdle(d, 5040, 65040, seen);
    assert(seen.plays.empty() && seen.restore_times.empty());
    StepIn(d, 65060, {DirectorInput::Emotion("neutral")});
    RunIdle(d, 65080, 65060 + kIdleGapMinMs - 20, seen);
    assert(seen.plays.empty());
    RunIdle(d, 65060 + kIdleGapMinMs, 65060 + kIdleGapMinMs, seen);
    assert(seen.plays.size() == 1);
}

// (g) Before the app has set any face, nothing plays.
void no_idle_actions_before_first_neutral() {
    DirectorLogic d(nullptr, kIdleTwo, ZeroRng);
    StepIn(d, 0, {}, Phase::Idle);
    IdleSeen seen;
    RunIdle(d, 20, 120000, seen);
    assert(seen.plays.empty() && seen.restore_times.empty());
}

// (h) A non-neutral face arriving while an idle action plays cancels it with no restore,
// and nothing more plays until "neutral" again.
void non_neutral_face_cancels_playing_action_without_restore() {
    DirectorLogic d(nullptr, kIdleTwo, ZeroRng);
    StepIn(d, 0, {DirectorInput::Emotion("neutral")}, Phase::Idle);
    IdleSeen seen;
    RunIdle(d, 20, 160 + kIdleGapMinMs, seen);
    assert(seen.plays.size() == 1);
    StepIn(d, 160 + kIdleGapMinMs + 200, {DirectorInput::Emotion("triangle_exclamation")});
    assert(d.idle_play() == nullptr && !d.idle_restore());
    RunIdle(d, 160 + kIdleGapMinMs + 220, 200000, seen);
    assert(seen.plays.size() == 1);
    assert(seen.restore_times.empty());
}

// P4 spike: a face delta moves the head relative to its target while Listening, clamped, and
// is ignored outside Listening.
void face_delta_moves_relative_and_clamps() {
    FakeHead head({40, 5});
    DirectorLogic d(&head);
    StepIn(d, 0, {}, Phase::Listening);
    Run(d, 20, 200);  // settle Listening
    assert(d.core().owner() == HeadOwner::Tracking);
    size_t before = head.moves.size();
    StepIn(d, 220, {DirectorInput::FaceDelta(10, -3)});
    assert(head.moves.size() == before + 1);
    assert(Same(head.moves.back().pose, {kYawMax, kPitchMin}));
    assert(head.moves.back().duration_ms == DirectorLogic::kTrackingMoveMs);
    StepIn(d, 240, {DirectorInput::FaceDelta(-5, 7)});
    assert(Same(head.moves.back().pose, {kYawMax - 5, kPitchMin + 7}));

    StepIn(d, 260, {}, Phase::Speaking);
    Run(d, 280, 500);
    before = head.moves.size();
    StepIn(d, 520, {DirectorInput::FaceDelta(-5, 0)});
    assert(head.moves.size() == before);
}

}  // namespace

int main() {
    request_in_same_iteration_as_old_finish();
    replacement_mid_keyframe_ends_at_original_base();
    cancel_without_replacement_returns_to_base();
    cancel_with_manual_move_ends_at_manual_target();
    unusable_head_finishes_gesture_at_once();
    finished_gesture_releases_head();
    head_usable_again_in_idle_returns_to_rest_once();
    head_usable_again_while_speaking_does_not_move();
    end_conversation_after_goodbye();
    end_conversation_ignores_unsettled_listening();
    end_conversation_dropped_by_idle();
    idle_action_plays_and_restores();
    idle_action_stopped_by_listening_never_restores();
    external_emotion_in_idle_restarts_idle_timer();
    empty_idle_list_emits_nothing();
    manual_hold_in_idle_blocks_idle_actions();
    non_neutral_face_in_idle_blocks_idle_actions();
    no_idle_actions_before_first_neutral();
    non_neutral_face_cancels_playing_action_without_restore();
    face_delta_moves_relative_and_clamps();
    std::puts("test_director_logic: all passed");
    return 0;
}
