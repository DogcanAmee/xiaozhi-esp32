#include "behavior_director.h"

#include <esp_log.h>
#include <esp_timer.h>

#include "application.h"

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
    : head_(neck != nullptr ? std::optional<NeckHead>(std::in_place, neck) : std::nullopt),
      led_(led),
      logic_(head_ ? &*head_ : nullptr) {}

void BehaviorDirector::Start() {
    queue_ = xQueueCreate(kQueueLength, sizeof(DirectorInput));
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
    DirectorInput in = DirectorInput::Emotion(emotion);
    if (xQueueSend(queue_, &in, 0) != pdTRUE) {
        // A dropped emotion costs at most one recolour or one automatic gesture.
        ESP_LOGD(TAG, "queue full; emotion \"%s\" dropped", in.emotion);
    }
}

bool BehaviorDirector::RequestGesture(Gesture g, int count) {
    return Post(DirectorInput::GestureRequest(g, count));
}

bool BehaviorDirector::RequestManualMove(int yaw_deg, int pitch_deg) {
    return Post(DirectorInput::ManualMove(yaw_deg, pitch_deg));
}

bool BehaviorDirector::Post(const DirectorInput& in) {
    if (queue_ == nullptr) {
        ESP_LOGW(TAG, "not started; request dropped");
        return false;
    }
    if (xQueueSend(queue_, &in, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full; request dropped");
        return false;
    }
    return true;
}

void BehaviorDirector::TaskEntry(void* self) { static_cast<BehaviorDirector*>(self)->Run(); }

void BehaviorDirector::Run() {
    // At most one queue's worth per iteration; anything left waits 20 ms.
    DirectorInput inputs[kQueueLength];
    for (;;) {
        int64_t now = NowMs();
        std::optional<Phase> phase;
        if (state_dirty_.exchange(false, std::memory_order_acq_rel)) {
            phase = PhaseForState(Application::GetInstance().GetDeviceState());
        }
        size_t n = 0;
        while (n < static_cast<size_t>(kQueueLength) &&
               xQueueReceive(queue_, &inputs[n], 0) == pdTRUE) {
            ++n;
        }
        ApplyLevelsAndLed(logic_.Step(now, phase, std::span<const DirectorInput>(inputs, n)));
        vTaskDelay(pdMS_TO_TICKS(kLoopMs));
    }
}

void BehaviorDirector::ApplyLevelsAndLed(const Outputs& out) {
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
