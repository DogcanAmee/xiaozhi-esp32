#ifdef NDEBUG
#error "asserts must be enabled in host tests"
#endif

#include <cassert>
#include <cstdio>
#include <optional>
#include <vector>

#include "director_logic.h"

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
// close is asked for once, when Listening has settled again. A missing neck changes nothing.
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
        assert(d->end_conversation());
        assert(CountEnds(*d, 3180, 40000) == 0);
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
    std::puts("test_director_logic: all passed");
    return 0;
}
