#include "led_ring.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <utility>

namespace anan {

namespace {

constexpr char TAG[] = "LedRing";
constexpr int64_t kFadeOutMs = 600;
constexpr int64_t kFrameMs = 40;
constexpr int64_t kRetryMs = 1000;         // re-try a failed static frame this often
constexpr int64_t kWarnIntervalMs = 5000;  // at most one write-failure warning per interval

int64_t NowMs() { return esp_timer_get_time() / 1000; }

}  // namespace

LedRing::LedRing(Py32IoExpander* py32, std::function<void()> on_state_changed)
    : py32_(py32), on_state_changed_(std::move(on_state_changed)) {
    if (py32_ == nullptr) {
        return;
    }
    queue_ = xQueueCreate(1, sizeof(LedCmd));
    if (queue_ == nullptr) {
        ESP_LOGE(TAG, "queue allocation failed; the ring stays dark");
        return;
    }
    if (xTaskCreate(&LedRing::TaskEntry, "led_ring", 3072, this, 2, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "task creation failed; the ring stays dark");
        vQueueDelete(queue_);
        queue_ = nullptr;
    }
}

void LedRing::OnStateChanged() {
    if (on_state_changed_) {
        on_state_changed_();
    }
}

void LedRing::Apply(const LedCmd& cmd) {
    if (py32_ == nullptr || queue_ == nullptr) {
        return;
    }
    xQueueOverwrite(queue_, &cmd);
}

void LedRing::TaskEntry(void* self) { static_cast<LedRing*>(self)->Run(); }

void LedRing::Run() {
    for (;;) {
        LedCmd cmd;
        if (xQueueReceive(queue_, &cmd, pdMS_TO_TICKS(kFrameMs)) == pdTRUE) {
            HandleCommand(cmd, NowMs());
        }
        Step(NowMs());
    }
}

void LedRing::HandleCommand(const LedCmd& cmd, int64_t now_ms) {
    switch (cmd.effect) {
        case LedEffect::Off:
        case LedEffect::FadeOut:
            if (effect_ == LedEffect::FadeOut) {
                break;  // already on its way down
            }
            if (shown_pct_ > 0) {
                fade_from_pct_ = shown_pct_;
                effect_ = LedEffect::FadeOut;
                effect_start_ms_ = now_ms;
            } else {
                effect_ = LedEffect::Off;
                dirty_ = true;
            }
            break;
        case LedEffect::Solid:
            if (effect_ != LedEffect::Solid || color_.r != cmd.color.r || color_.g != cmd.color.g ||
                color_.b != cmd.color.b) {
                effect_ = LedEffect::Solid;
                color_ = cmd.color;
                dirty_ = true;
            }
            break;
        case LedEffect::Breathe:
            if (effect_ != LedEffect::Breathe) {
                effect_ = LedEffect::Breathe;
                effect_start_ms_ = now_ms;  // a repeated Breathe keeps its phase
            }
            color_ = cmd.color;
            break;
    }
}

void LedRing::Step(int64_t now_ms) {
    int level = 0;
    bool animating = false;
    switch (effect_) {
        case LedEffect::Off:
            break;
        case LedEffect::Solid:
            level = kMaxBrightnessPct;
            break;
        case LedEffect::Breathe:
            level = BreatheLevelPct(now_ms - effect_start_ms_);
            animating = true;
            break;
        case LedEffect::FadeOut: {
            int64_t elapsed = now_ms - effect_start_ms_;
            animating = true;  // also covers the last frame, which writes zero
            if (elapsed >= kFadeOutMs) {
                effect_ = LedEffect::Off;
            } else {
                level = static_cast<int>(fade_from_pct_ * (kFadeOutMs - elapsed) / kFadeOutMs);
            }
            break;
        }
    }

    if (now_ms < next_retry_ms_) {
        return;  // the last write failed; do not hammer the bus
    }
    if (animating ? (level == shown_pct_ && !dirty_) : !dirty_) {
        return;  // nothing new to show
    }

    uint16_t px = PackRgb565(ApplyBrightness(color_, level));
    uint16_t frame[kLedCount];
    for (uint16_t& p : frame) {
        p = px;
    }
    if (py32_->WriteLeds(frame, kLedCount)) {
        shown_pct_ = level;
        dirty_ = false;
        return;
    }
    dirty_ = true;  // try this frame again
    next_retry_ms_ = now_ms + kRetryMs;
    if (!warned_ || now_ms - last_warn_ms_ >= kWarnIntervalMs) {
        warned_ = true;
        last_warn_ms_ = now_ms;
        ESP_LOGW(TAG, "LED frame write failed; will keep retrying quietly");
    }
}

}  // namespace anan
