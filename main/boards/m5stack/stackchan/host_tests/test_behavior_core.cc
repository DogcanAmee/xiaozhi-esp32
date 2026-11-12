#ifdef NDEBUG
#error "asserts must be enabled in host tests"
#endif

#include <cassert>
#include <cstdio>

#include "behavior_core.h"

using namespace anan;

namespace {

bool Is(const std::optional<Pose>& p, Pose want) {
    return p.has_value() && p->yaw_deg == want.yaw_deg && p->pitch_deg == want.pitch_deg;
}

// Request a phase at `t` and let it settle; returns the time after settling.
int64_t SetPhase(BehaviorCore& c, Phase p, int64_t t) {
    c.OnPhase(p, t);
    c.Poll(t + kPhaseSettleMs);
    return t + kPhaseSettleMs;
}

bool Silent(const Outputs& o) {
    return !o.move_to.has_value() && o.start_gesture == Gesture::None && !o.cancel_gesture;
}

void clamp_pose_limits_pitch_and_yaw() {
    Pose a = ClampPose({90, 100});
    assert(a.yaw_deg == 45 && a.pitch_deg == 85);
    Pose b = ClampPose({-90, 0});
    assert(b.yaw_deg == -45 && b.pitch_deg == 5);
    Pose c = ClampPose({10, 40});
    assert(c.yaw_deg == 10 && c.pitch_deg == 40);
}

void emotion_table_maps_known_names() {
    assert(GestureForEmotion("happy") == Gesture::Nod);
    assert(GestureForEmotion("laughing") == Gesture::Nod);
    assert(GestureForEmotion("confident") == Gesture::Nod);
    assert(GestureForEmotion("confused") == Gesture::Tilt);
    assert(GestureForEmotion("thinking") == Gesture::Tilt);
    assert(GestureForEmotion("sad") == Gesture::Bow);
    assert(GestureForEmotion("crying") == Gesture::Bow);
    assert(GestureForEmotion("angry") == Gesture::QuickShake);
    assert(GestureForEmotion("shocked") == Gesture::QuickShake);
}

void unknown_or_empty_emotion_maps_to_none() {
    assert(GestureForEmotion("") == Gesture::None);
    assert(GestureForEmotion("banana") == Gesture::None);
    assert(GestureForEmotion("neutral") == Gesture::None);
}

void idle_returns_to_rest_once_then_is_silent() {
    BehaviorCore c;
    c.OnPhase(Phase::Idle, 0);
    Outputs before = c.Poll(100);  // not settled yet
    assert(!before.move_to.has_value());
    Outputs first = c.Poll(150);
    assert(Is(first.move_to, kRestPose));
    assert(first.move_ms == kReturnToRestMs);
    for (int64_t t = 170; t <= 10150; t += 20) {
        assert(Silent(c.Poll(t)));
    }
}

void idle_enables_idle_actions_only() {
    BehaviorCore c;
    SetPhase(c, Phase::Idle, 0);
    Outputs o = c.Poll(1000);
    assert(o.idle_actions_enabled);
    assert(!o.tracking_enabled);
    assert(c.owner() == HeadOwner::Hold);
}

void listening_enables_tracking_and_follows_face() {
    BehaviorCore c;
    int64_t t = SetPhase(c, Phase::Listening, 0);
    Outputs o = c.Poll(t + 20);
    assert(o.tracking_enabled);
    assert(!o.idle_actions_enabled);
    assert(c.owner() == HeadOwner::Tracking);
    c.OnFaceTarget({20, 30}, t + 40);
    o = c.Poll(t + 40);
    assert(Is(o.move_to, (Pose{20, 30})));
    // one-shot
    assert(!c.Poll(t + 60).move_to.has_value());
    // clamped
    c.OnFaceTarget({100, 100}, t + 80);
    o = c.Poll(t + 80);
    assert(Is(o.move_to, (Pose{45, 85})));
}

void speaking_disables_tracking_and_holds() {
    BehaviorCore c;
    int64_t t = SetPhase(c, Phase::Speaking, 0);
    c.OnFaceTarget({20, 30}, t + 20);
    Outputs o = c.Poll(t + 20);
    assert(!o.tracking_enabled);
    assert(!o.move_to.has_value());
    assert(c.owner() == HeadOwner::Hold);
    assert(Silent(c.Poll(t + 40)));
}

void speaking_emotion_starts_gesture() {
    BehaviorCore c;
    int64_t t = SetPhase(c, Phase::Speaking, 0);
    c.OnEmotion("happy", t + 10);
    Outputs o = c.Poll(t + 20);
    assert(o.start_gesture == Gesture::Nod);
    assert(o.gesture_count == 1);
    assert(c.owner() == HeadOwner::Gesture);
    // one-shot
    assert(c.Poll(t + 40).start_gesture == Gesture::None);
    c.OnGestureFinished(t + 500);
    c.Poll(t + 500);
    assert(c.owner() == HeadOwner::Hold);
}

void auto_gestures_are_throttled_4s() {
    BehaviorCore c;
    SetPhase(c, Phase::Speaking, -150);
    c.OnEmotion("happy", 0);
    assert(c.Poll(0).start_gesture == Gesture::Nod);
    c.OnGestureFinished(1000);
    c.OnEmotion("happy", 3999);
    assert(c.Poll(3999).start_gesture == Gesture::None);
    c.OnEmotion("happy", 4000);
    assert(c.Poll(4000).start_gesture == Gesture::Nod);
}

void tool_gesture_ignores_throttle() {
    BehaviorCore c;
    SetPhase(c, Phase::Speaking, -150);
    c.OnEmotion("happy", 0);
    assert(c.Poll(0).start_gesture == Gesture::Nod);
    c.OnGestureFinished(500);
    c.OnGestureRequest(Gesture::Shake, 3, 600);
    Outputs o = c.Poll(600);
    assert(o.start_gesture == Gesture::Shake);
    assert(o.gesture_count == 3);
    assert(!o.cancel_gesture);
    // a tool request replaces a running gesture
    c.OnGestureRequest(Gesture::Tilt, 1, 700);
    o = c.Poll(700);
    assert(o.cancel_gesture);
    assert(o.start_gesture == Gesture::Tilt);
    // None is ignored
    c.OnGestureRequest(Gesture::None, 1, 800);
    assert(Silent(c.Poll(800)));
    assert(c.owner() == HeadOwner::Gesture);
}

void manual_move_cancels_gesture_and_wins() {
    BehaviorCore c;
    SetPhase(c, Phase::Speaking, -150);
    c.OnEmotion("sad", 0);
    assert(c.Poll(0).start_gesture == Gesture::Bow);
    c.OnManualMove({30, 20}, 500);
    Outputs o = c.Poll(500);
    assert(o.cancel_gesture);
    assert(Is(o.move_to, (Pose{30, 20})));
    assert(c.owner() == HeadOwner::Manual);
}

void manual_hold_blocks_auto_gesture_and_tracking() {
    BehaviorCore c;
    SetPhase(c, Phase::Listening, -150);
    c.OnManualMove({10, 10}, 0);
    assert(Is(c.Poll(0).move_to, (Pose{10, 10})));
    c.OnFaceTarget({20, 30}, 1000);
    assert(Silent(c.Poll(1000)));
    assert(!c.Poll(1020).tracking_enabled);
    assert(c.owner() == HeadOwner::Manual);
    c.OnFaceTarget({20, 30}, kManualHoldMs - 1);
    assert(Silent(c.Poll(kManualHoldMs - 1)));
    // hold expires at 8 s; tracking resumes
    c.Poll(kManualHoldMs);
    assert(c.owner() == HeadOwner::Tracking);
    c.OnFaceTarget({20, 30}, kManualHoldMs + 20);
    assert(Is(c.Poll(kManualHoldMs + 20).move_to, (Pose{20, 30})));

    // auto gesture blocked during the hold, allowed after
    BehaviorCore d;
    SetPhase(d, Phase::Speaking, -150);
    d.OnManualMove({10, 10}, 0);
    d.Poll(0);
    d.OnEmotion("happy", 2000);
    assert(d.Poll(2000).start_gesture == Gesture::None);
    d.OnEmotion("happy", kManualHoldMs);
    assert(d.Poll(kManualHoldMs).start_gesture == Gesture::Nod);
}

void manual_move_in_idle_returns_to_rest_after_hold() {
    BehaviorCore c;
    int64_t t = SetPhase(c, Phase::Idle, 0);
    c.Poll(t + 20);
    c.OnManualMove({30, 20}, 1000);
    assert(Is(c.Poll(1000).move_to, (Pose{30, 20})));
    assert(c.owner() == HeadOwner::Manual);
    assert(!c.Poll(1020).idle_actions_enabled);
    assert(Silent(c.Poll(1000 + kManualHoldMs - 20)));
    Outputs o = c.Poll(1000 + kManualHoldMs);
    assert(Is(o.move_to, kRestPose));
    assert(o.move_ms == kReturnToRestMs);
    assert(Silent(c.Poll(1000 + kManualHoldMs + 20)));
    assert(c.owner() == HeadOwner::Hold);
}

void leaving_to_idle_cancels_gesture_before_rest() {
    BehaviorCore c;
    SetPhase(c, Phase::Speaking, -150);
    c.OnEmotion("happy", 0);
    assert(c.Poll(0).start_gesture == Gesture::Nod);
    c.OnPhase(Phase::Idle, 100);
    assert(Silent(c.Poll(200)));
    Outputs o = c.Poll(250);
    assert(o.cancel_gesture);
    assert(Is(o.move_to, kRestPose));
    assert(c.owner() == HeadOwner::Hold);
}

void phase_flapping_within_150ms_is_ignored() {
    BehaviorCore c;
    SetPhase(c, Phase::Speaking, -150);
    c.OnPhase(Phase::Listening, 0);
    assert(Silent(c.Poll(20)));
    c.OnPhase(Phase::Speaking, 50);
    assert(Silent(c.Poll(60)));
    c.OnPhase(Phase::Listening, 100);
    c.OnFaceTarget({20, 30}, 110);
    assert(Silent(c.Poll(120)));
    c.OnPhase(Phase::Speaking, 150);
    for (int64_t t = 170; t <= 500; t += 20) {
        Outputs o = c.Poll(t);
        assert(Silent(o));
        assert(!o.tracking_enabled);
        assert(o.led_phase == Phase::Speaking);
    }
    assert(c.phase() == Phase::Speaking);
}

void emotion_outside_speaking_does_not_gesture() {
    BehaviorCore c;
    SetPhase(c, Phase::Idle, -150);
    c.Poll(0);
    c.OnEmotion("happy", 10);
    Outputs o = c.Poll(20);
    assert(o.start_gesture == Gesture::None);
    assert(o.led_emotion.empty());
    SetPhase(c, Phase::Listening, 100);
    c.OnEmotion("happy", 300);
    o = c.Poll(300);
    assert(o.start_gesture == Gesture::None);
    assert(o.led_emotion.empty());
}

void led_phase_and_emotion_follow_inputs() {
    BehaviorCore c;
    assert(c.phase() == Phase::Other);
    assert(c.owner() == HeadOwner::Hold);
    assert(c.Poll(0).led_phase == Phase::Other);
    c.OnPhase(Phase::Speaking, 0);
    assert(c.Poll(100).led_phase == Phase::Other);  // not settled
    assert(c.Poll(150).led_phase == Phase::Speaking);
    c.OnEmotion("angry", 200);
    Outputs o = c.Poll(200);
    assert(o.led_phase == Phase::Speaking);
    assert(o.led_emotion == "angry");
    assert(c.Poll(220).led_emotion == "angry");  // level output
    c.OnPhase(Phase::Listening, 300);
    assert(c.Poll(400).led_emotion == "angry");  // still Speaking until settled
    o = c.Poll(450);
    assert(o.led_phase == Phase::Listening);
    assert(o.led_emotion.empty());
}

}  // namespace

int main() {
    clamp_pose_limits_pitch_and_yaw();
    emotion_table_maps_known_names();
    unknown_or_empty_emotion_maps_to_none();
    idle_returns_to_rest_once_then_is_silent();
    idle_enables_idle_actions_only();
    listening_enables_tracking_and_follows_face();
    speaking_disables_tracking_and_holds();
    speaking_emotion_starts_gesture();
    auto_gestures_are_throttled_4s();
    tool_gesture_ignores_throttle();
    manual_move_cancels_gesture_and_wins();
    manual_hold_blocks_auto_gesture_and_tracking();
    manual_move_in_idle_returns_to_rest_after_hold();
    leaving_to_idle_cancels_gesture_before_rest();
    phase_flapping_within_150ms_is_ignored();
    emotion_outside_speaking_does_not_gesture();
    led_phase_and_emotion_follow_inputs();
    std::puts("test_behavior_core: all passed");
    return 0;
}
