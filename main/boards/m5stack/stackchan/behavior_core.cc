#include "behavior_core.h"

#include <algorithm>

namespace anan {

Pose ClampPose(Pose p) {
    return {std::clamp(p.yaw_deg, kYawMin, kYawMax), std::clamp(p.pitch_deg, kPitchMin, kPitchMax)};
}

Gesture GestureForEmotion(std::string_view emotion) {
    if (emotion == "happy" || emotion == "laughing" || emotion == "confident") {
        return Gesture::Nod;
    }
    if (emotion == "confused" || emotion == "thinking") {
        return Gesture::Tilt;
    }
    if (emotion == "sad" || emotion == "crying") {
        return Gesture::Bow;
    }
    if (emotion == "angry" || emotion == "shocked") {
        return Gesture::QuickShake;
    }
    return Gesture::None;
}

void BehaviorCore::EnterPhase(Phase next) {
    const Phase prev = phase_;
    phase_ = next;
    if (prev == Phase::Speaking) {
        led_emotion_.clear();
    }
    if (next == Phase::Idle) {
        if (gesture_active_) {
            gesture_active_ = false;
            pending_gesture_ = Gesture::None;
            pending_cancel_ = true;
        }
        pending_rest_ = true;
    }
}

void BehaviorCore::Advance(int64_t now_ms) {
    if (manual_active_ && now_ms >= manual_until_ms_) {
        manual_active_ = false;
        if (phase_ == Phase::Idle) {
            pending_rest_ = true;
        }
    }
    if (pending_phase_ && now_ms - pending_since_ms_ >= kPhaseSettleMs) {
        const Phase next = *pending_phase_;
        pending_phase_.reset();
        EnterPhase(next);
    }
}

void BehaviorCore::OnPhase(Phase phase, int64_t now_ms) {
    Advance(now_ms);
    if (phase == phase_) {
        pending_phase_.reset();  // flapped back before settling
    } else if (!pending_phase_ || *pending_phase_ != phase) {
        pending_phase_ = phase;
        pending_since_ms_ = now_ms;
    }
}

void BehaviorCore::OnEmotion(std::string_view emotion, int64_t now_ms) {
    Advance(now_ms);
    if (phase_ != Phase::Speaking) {
        return;
    }
    led_emotion_ = std::string(emotion);
    const Gesture g = GestureForEmotion(emotion);
    if (g == Gesture::None || manual_active_ || gesture_active_) {
        return;
    }
    if (last_auto_gesture_ms_ && now_ms - *last_auto_gesture_ms_ < kAutoGestureGapMs) {
        return;
    }
    last_auto_gesture_ms_ = now_ms;
    gesture_active_ = true;
    pending_gesture_ = g;
    pending_gesture_count_ = 1;
}

void BehaviorCore::OnManualMove(Pose target, int64_t now_ms) {
    Advance(now_ms);
    if (gesture_active_) {
        gesture_active_ = false;
        pending_gesture_ = Gesture::None;
        pending_cancel_ = true;
    }
    manual_active_ = true;
    manual_until_ms_ = now_ms + kManualHoldMs;
    pending_rest_ = false;
    pending_face_.reset();
    pending_move_ = ClampPose(target);
    pending_move_ms_ = 0;
}

void BehaviorCore::OnGestureRequest(Gesture g, int count, int64_t now_ms) {
    Advance(now_ms);
    // Tool gestures are allowed during a manual hold; the hold timer is unchanged.
    if (g == Gesture::None) {
        return;
    }
    if (gesture_active_) {
        pending_cancel_ = true;
    }
    gesture_active_ = true;
    pending_gesture_ = g;
    pending_gesture_count_ = count;
}

void BehaviorCore::OnFaceTarget(Pose target, int64_t now_ms) {
    Advance(now_ms);
    pending_face_ = target;
}

void BehaviorCore::OnGestureFinished(int64_t now_ms) {
    Advance(now_ms);
    gesture_active_ = false;
}

HeadOwner BehaviorCore::owner() const {
    // A gesture started by a tool request during a manual hold owns the head until it
    // finishes (OnManualMove itself cancels any running gesture).
    if (gesture_active_) {
        return HeadOwner::Gesture;
    }
    if (manual_active_) {
        return HeadOwner::Manual;
    }
    if (phase_ == Phase::Listening) {
        return HeadOwner::Tracking;
    }
    return HeadOwner::Hold;
}

Phase BehaviorCore::phase() const { return phase_; }

Outputs BehaviorCore::Poll(int64_t now_ms) {
    Advance(now_ms);
    Outputs out;
    const HeadOwner who = owner();

    out.cancel_gesture = pending_cancel_;
    out.start_gesture = pending_gesture_;
    out.gesture_count = pending_gesture_count_;

    if (pending_move_) {
        out.move_to = pending_move_;
        out.move_ms = pending_move_ms_;
    } else if (pending_rest_ && !manual_active_ && !gesture_active_) {
        out.move_to = kRestPose;
        out.move_ms = kReturnToRestMs;
        pending_rest_ = false;
    } else if (pending_face_ && who == HeadOwner::Tracking) {
        out.move_to = ClampPose(*pending_face_);
    }

    out.tracking_enabled = who == HeadOwner::Tracking;
    out.idle_actions_enabled = phase_ == Phase::Idle && who == HeadOwner::Hold;
    out.led_phase = phase_;
    out.led_emotion = led_emotion_;

    pending_move_.reset();
    pending_face_.reset();
    pending_gesture_ = Gesture::None;
    pending_gesture_count_ = 1;
    pending_cancel_ = false;
    return out;
}

}  // namespace anan
