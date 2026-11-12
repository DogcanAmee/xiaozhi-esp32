#include "behavior_director.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <cstring>

#include "application.h"
#include "head_gestures.h"

namespace anan {

namespace {

constexpr char TAG[] = "BehaviorDirector";

int64_t NowMs() { return esp_timer_get_time() / 1000; }

Phase PhaseForState(DeviceState state) {
    switch (state) {
        case kDeviceStateIdle:
            return Phase::Idle;
        case kDeviceStateListening:
            return Phase::Listening;
        case kDeviceStateSpeaking:
            return Phase::Speaking;
        default:
            return Phase::Other;
    }
}

bool SameLed(const LedCmd& a, const LedCmd& b) {
    return a.effect == b.effect && a.color.r == b.color.r && a.color.g == b.color.g &&
           a.color.b == b.color.b;
}

}  // namespace

BehaviorDirector::BehaviorDirector(ServoNeck* neck, LedRing* led)
    : neck_(neck), led_(led), player_([this](Pose pose, uint32_t duration_ms) {
          // Only called from the director task, inside ApplyOutputs / the loop.
          if (HeadUsable()) {
              neck_->StartMove(pose.yaw_deg, pose.pitch_deg, duration_ms);
          }
      }) {}

void BehaviorDirector::Start() {
    queue_ = xQueueCreate(kQueueLength, sizeof(Event));
    if (queue_ == nullptr) {
        ESP_LOGE(TAG, "queue allocation failed; head and LED behavior are off");
        return;
    }
    if (xTaskCreate(&BehaviorDirector::TaskEntry, "behavior", kTaskStack, this, kTaskPriority,
                    nullptr) != pdPASS) {
        ESP_LOGE(TAG, "task creation failed; head and LED behavior are off");
        vQueueDelete(queue_);
        queue_ = nullptr;
    }
}

void BehaviorDirector::NotifyStateChanged() { state_dirty_.store(true, std::memory_order_release); }

void BehaviorDirector::NotifyEmotion(const char* emotion) {
    if (emotion == nullptr || queue_ == nullptr) {
        return;
    }
    Event ev{};
    ev.type = EventType::Emotion;
    std::strncpy(ev.emotion, emotion, sizeof(ev.emotion) - 1);
    ev.emotion[sizeof(ev.emotion) - 1] = '\0';
    if (xQueueSend(queue_, &ev, 0) != pdTRUE) {
        // A dropped emotion costs at most one recolour or one automatic gesture.
        ESP_LOGD(TAG, "queue full; emotion \"%s\" dropped", ev.emotion);
    }
}

void BehaviorDirector::RequestGesture(Gesture g, int count) {
    Event ev{};
    ev.type = EventType::Gesture;
    ev.a = static_cast<int>(g);
    ev.b = count;
    Post(ev);
}

void BehaviorDirector::RequestManualMove(int yaw_deg, int pitch_deg) {
    Event ev{};
    ev.type = EventType::ManualMove;
    ev.a = yaw_deg;
    ev.b = pitch_deg;
    Post(ev);
}

void BehaviorDirector::Post(const Event& ev) {
    if (queue_ == nullptr) {
        ESP_LOGW(TAG, "not started; request dropped");
        return;
    }
    if (xQueueSend(queue_, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full; request dropped");
    }
}

void BehaviorDirector::TaskEntry(void* self) { static_cast<BehaviorDirector*>(self)->Run(); }

void BehaviorDirector::Run() {
    for (;;) {
        int64_t now = NowMs();
        if (state_dirty_.exchange(false, std::memory_order_acq_rel)) {
            core_.OnPhase(PhaseForState(Application::GetInstance().GetDeviceState()), now);
        }
        Event ev;
        while (xQueueReceive(queue_, &ev, 0) == pdTRUE) {
            HandleEvent(ev, now);
        }
        if (player_.Tick(now)) {
            core_.OnGestureFinished(now);
        }
        ApplyOutputs(core_.Poll(now), now);
        vTaskDelay(pdMS_TO_TICKS(kLoopMs));
    }
}

void BehaviorDirector::HandleEvent(const Event& ev, int64_t now_ms) {
    switch (ev.type) {
        case EventType::Emotion:
            core_.OnEmotion(ev.emotion, now_ms);
            break;
        case EventType::Gesture:
            core_.OnGestureRequest(static_cast<Gesture>(ev.a), ev.b, now_ms);
            break;
        case EventType::ManualMove:
            core_.OnManualMove(Pose{ev.a, ev.b}, now_ms);
            break;
    }
}

bool BehaviorDirector::HeadUsable() const { return neck_ != nullptr && neck_->torque_on(); }

void BehaviorDirector::ApplyOutputs(const Outputs& out, int64_t now_ms) {
    // Order matters: cancel, then move_to, then start_gesture (whose base is the move target).
    if (out.cancel_gesture) {
        player_.Cancel();
        if (neck_ != nullptr) {
            neck_->InvalidateRequests();
        }
    }
    if (out.move_to && HeadUsable()) {
        uint32_t ms = out.move_ms;
        if (ms == 0) {
            ms = core_.owner() == HeadOwner::Tracking ? kTrackingMoveMs : kManualMoveMs;
        }
        neck_->StartMove(out.move_to->yaw_deg, out.move_to->pitch_deg, ms);
    }
    if (out.start_gesture != Gesture::None) {
        if (HeadUsable()) {
            int yaw = 0, pitch = 0;
            neck_->GetTarget(yaw, pitch);
            player_.Start(BuildGesture(out.start_gesture, Pose{yaw, pitch}, out.gesture_count),
                          now_ms);
        } else {
            // Nothing can play it; finish now so the core does not stay in Gesture.
            player_.Cancel();
            core_.OnGestureFinished(now_ms);
        }
    }

    tracking_enabled_.store(out.tracking_enabled, std::memory_order_relaxed);
    idle_actions_enabled_.store(out.idle_actions_enabled, std::memory_order_relaxed);

    if (led_ != nullptr) {
        LedCmd cmd = LedForState(out.led_phase, out.led_emotion);
        if (!led_sent_ || !SameLed(cmd, last_led_)) {
            led_->Apply(cmd);
            last_led_ = cmd;
            led_sent_ = true;
        }
    }
}

}  // namespace anan
