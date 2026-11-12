#pragma once

// The StackChan base LED ring as a repo Led. A low-priority task owns every I2C write to the
// ring: Apply() only hands it a command, so it is safe to call from any task.

#include <cstdint>
#include <functional>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "led/led.h"
#include "led_color.h"
#include "py32_io_expander.h"

namespace anan {

// Lives for the whole life of the firmware (the board owns it and never destroys it).
class LedRing : public Led {
public:
    // `py32` may be null (chip missing): Apply() then does nothing and no task is created.
    LedRing(Py32IoExpander* py32, std::function<void()> on_state_changed);
    LedRing(const LedRing&) = delete;
    LedRing& operator=(const LedRing&) = delete;

    // Only invokes the callback; the coordinator decides the colour and calls Apply().
    void OnStateChanged() override;

    // Latest command wins (a length-1 overwrite queue). Off while the ring is lit fades out
    // first (600 ms from the current brightness), then stays off.
    void Apply(const LedCmd& cmd);

private:
    static void TaskEntry(void* self);
    void Run();
    void HandleCommand(const LedCmd& cmd, int64_t now_ms);
    void Step(int64_t now_ms);

    Py32IoExpander* py32_;
    std::function<void()> on_state_changed_;
    QueueHandle_t queue_ = nullptr;

    // Everything below is touched only by the effect task.
    LedEffect effect_ = LedEffect::Off;
    Rgb color_{0, 0, 0};
    int64_t effect_start_ms_ = 0;
    int fade_from_pct_ = 0;
    int shown_pct_ = 0;  // brightness of the last frame that was written successfully
    bool dirty_ = true;  // a static frame still has to be written (the first one clears the ring)
    int64_t next_retry_ms_ = 0;
    int64_t last_warn_ms_ = 0;
    bool warned_ = false;
};

}  // namespace anan
