#pragma once

// The emote face with one hook: every emotion is also handed to the behavior director, and an
// emotion the asset pack does not carry falls back to "neutral" instead of leaving the old face.

#include <atomic>
#include <cstring>

#include <esp_log.h>

#include "behavior_director.h"
#include "display/emote_display.h"

namespace anan {

class AnanDisplay : public emote::EmoteDisplay {
public:
    using EmoteDisplay::EmoteDisplay;

    void SetDirector(BehaviorDirector* d) { director_.store(d, std::memory_order_release); }

    bool SetEmotion(const char* emotion) override {
        if (emotion == nullptr) {
            return EmoteDisplay::SetEmotion("neutral");
        }
        if (BehaviorDirector* d = director_.load(std::memory_order_acquire)) {
            d->NotifyEmotion(emotion);
        }
        if (EmoteDisplay::SetEmotion(emotion)) {
            return true;
        }
        if (std::strcmp(emotion, "neutral") == 0) {
            return false;
        }
        ESP_LOGW("AnanDisplay", "no face for emotion \"%.23s\"; showing neutral", emotion);
        return EmoteDisplay::SetEmotion("neutral");
    }

private:
    std::atomic<BehaviorDirector*> director_{nullptr};
};

}  // namespace anan
