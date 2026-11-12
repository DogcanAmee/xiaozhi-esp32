#include "director_logic.h"

#include <cstring>
#include <string_view>
#include <utility>

#include "head_gestures.h"

namespace anan {

DirectorInput DirectorInput::Emotion(const char* emotion) {
    DirectorInput in{};
    in.type = Type::Emotion;
    if (emotion != nullptr) {
        std::strncpy(in.emotion, emotion, sizeof(in.emotion) - 1);
    }
    in.emotion[sizeof(in.emotion) - 1] = '\0';
    return in;
}

DirectorInput DirectorInput::GestureRequest(Gesture g, int count) {
    DirectorInput in{};
    in.type = Type::Gesture;
    in.a = static_cast<int>(g);
    in.b = count;
    return in;
}

DirectorInput DirectorInput::FaceDelta(int dyaw_deg, int dpitch_deg) {
    DirectorInput in{};
    in.type = Type::FaceDelta;
    in.a = dyaw_deg;
    in.b = dpitch_deg;
    return in;
}

DirectorInput DirectorInput::ManualMove(int yaw_deg, int pitch_deg) {
    DirectorInput in{};
    in.type = Type::ManualMove;
    in.a = yaw_deg;
    in.b = pitch_deg;
    return in;
}

DirectorInput DirectorInput::EndConversation() {
    DirectorInput in{};
    in.type = Type::EndConversation;
    return in;
}

DirectorLogic::DirectorLogic(HeadPort* head, std::span<const IdleAction> idle_actions,
                             std::function<uint32_t(uint32_t n)> rng)
    : head_(head),
      player_([this](Pose pose, uint32_t duration_ms) {
          if (HeadUsable()) {
              head_->StageMove(pose, duration_ms);
          }
      }),
      idle_(idle_actions, std::move(rng)) {}

bool DirectorLogic::HeadUsable() const { return head_ != nullptr && head_->Usable(); }

Outputs DirectorLogic::Step(int64_t now_ms, std::optional<Phase> phase,
                            std::span<const DirectorInput> inputs) {
    // The finish goes first. OnGestureFinished carries no gesture id, so it must reach the
    // core before any input that could start or replace a gesture, or it would end that one.
    if (player_.Tick(now_ms)) {
        gesture_base_.reset();
        core_.OnGestureFinished(now_ms);
    }
    // Torque back on: the core's one-shot return-to-rest may have been spent while the head
    // could not move, so let it re-arm.
    const bool usable = HeadUsable();
    if (head_was_usable_ && !*head_was_usable_ && usable) {
        core_.OnHeadAvailable(now_ms);
    }
    head_was_usable_ = usable;
    if (phase) {
        core_.OnPhase(*phase, now_ms);
    }
    for (const DirectorInput& in : inputs) {
        Feed(in, now_ms);
    }
    Outputs out = core_.Poll(now_ms);
    ApplyHead(out, now_ms);
    // After Poll, so a phase that settled in this iteration counts.
    end_conversation_ = rest_.Step(core_.phase(), now_ms);
    // After Poll too: idle_actions_enabled is false the moment the head is taken or the
    // device leaves Idle, and an emotion fed above has already restarted the timer.
    // Any face other than "neutral" (e.g. a network-error alert that the application keeps
    // visible in Idle) is left alone; DismissAlert and the next Idle entry send "neutral".
    idle_step_ = idle_.Tick(out.idle_actions_enabled && last_external_neutral_, now_ms);
    return out;
}

void DirectorLogic::Feed(const DirectorInput& in, int64_t now_ms) {
    switch (in.type) {
        case DirectorInput::Type::Emotion:
            core_.OnEmotion(std::string_view(in.emotion), now_ms);
            idle_.OnExternalEmotion(now_ms);
            last_external_neutral_ = std::string_view(in.emotion) == "neutral";
            break;
        case DirectorInput::Type::Gesture:
            core_.OnGestureRequest(static_cast<Gesture>(in.a), in.b, now_ms);
            break;
        case DirectorInput::Type::ManualMove:
            core_.OnManualMove(Pose{in.a, in.b}, now_ms);
            break;
        case DirectorInput::Type::EndConversation:
            rest_.Request(now_ms);
            break;
        case DirectorInput::Type::FaceDelta: {
            // Relative to the last commanded target (clamped there), so no wind-up past the
            // limits. Ignored by the core unless the tracker owns the head.
            Pose base = head_ != nullptr ? head_->Target() : kRestPose;
            core_.OnFaceTarget(ClampPose(Pose{base.yaw_deg + in.a, base.pitch_deg + in.b}),
                               now_ms);
            break;
        }
    }
}

void DirectorLogic::ApplyHead(const Outputs& out, int64_t now_ms) {
    // Order: cancel, then move_to, then start_gesture.
    //
    // A cancelled gesture leaves the neck's target on whatever keyframe was in flight, so the
    // pose it interrupted is remembered here instead: a replacement gesture is built on it, and
    // with nothing else moving the head, the head goes back to it.
    std::optional<Pose> interrupted_base;
    if (out.cancel_gesture || (out.start_gesture != Gesture::None && player_.playing())) {
        if (player_.playing()) {
            interrupted_base = gesture_base_;
        }
        player_.Cancel();
        gesture_base_.reset();
        if (head_ != nullptr) {
            head_->Invalidate();
        }
    }

    std::optional<Pose> staged;
    if (out.move_to && HeadUsable()) {
        uint32_t ms = out.move_ms;
        if (ms == 0) {
            ms = core_.owner() == HeadOwner::Tracking ? kTrackingMoveMs : kManualMoveMs;
        }
        head_->StageMove(*out.move_to, ms);
        staged = *out.move_to;
    }

    if (out.start_gesture != Gesture::None) {
        if (HeadUsable()) {
            // A move staged in this Poll wins; then the interrupted gesture's base; then
            // wherever the head was last sent.
            Pose base = staged ? *staged : (interrupted_base ? *interrupted_base : head_->Target());
            gesture_base_ = base;
            player_.Start(BuildGesture(out.start_gesture, base, out.gesture_count), now_ms);
        } else {
            // Nothing can play it; finish now so the core does not stay in Gesture.
            core_.OnGestureFinished(now_ms);
        }
    } else if (interrupted_base && !out.move_to && HeadUsable()) {
        head_->StageMove(*interrupted_base, kReturnToBaseMs);
    }
}

}  // namespace anan
