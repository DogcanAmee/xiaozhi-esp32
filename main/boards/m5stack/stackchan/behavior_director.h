#pragma once

// The one task that drives BehaviorCore and turns its outputs into neck moves, gestures and
// LED ring commands. Every public Notify*/Request* method only posts to this task (a queue,
// or an atomic flag for device-state changes), so they are safe to call from any task.

#include <atomic>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "behavior_core.h"
#include "gesture_player.h"
#include "led_color.h"
#include "led_ring.h"
#include "servo_neck.h"

namespace anan {

// Lives for the whole life of the firmware (the board owns it and never destroys it).
class BehaviorDirector {
public:
    static constexpr int kQueueLength = 16;
    static constexpr uint32_t kTaskStack = 4096;
    static constexpr UBaseType_t kTaskPriority = 3;
    static constexpr uint32_t kLoopMs = 20;
    // Outputs::move_ms == 0 means "the caller's default speed".
    static constexpr uint32_t kManualMoveMs = 600;
    static constexpr uint32_t kTrackingMoveMs = 250;

    // Either may be null: without a neck no head command is sent (the core still runs, so the
    // LED ring works); without a ring no LED command is sent.
    BehaviorDirector(ServoNeck* neck, LedRing* led);
    BehaviorDirector(const BehaviorDirector&) = delete;
    BehaviorDirector& operator=(const BehaviorDirector&) = delete;

    // Create the queue and the task. Call once.
    void Start();

    // Reads Application::GetDeviceState() on the director task. Never lost: a flag, not a
    // queued event.
    void NotifyStateChanged();
    // Null-safe. Dropped (with a log line) if the queue is full.
    void NotifyEmotion(const char* emotion);
    // For MCP tools. Dropped (with a warning) if the queue is full.
    void RequestGesture(Gesture g, int count);
    void RequestManualMove(int yaw_deg, int pitch_deg);

    // Latest level outputs, for later phases (P4 face tracking, P5 idle actions).
    bool tracking_enabled() const { return tracking_enabled_.load(std::memory_order_relaxed); }
    bool idle_actions_enabled() const {
        return idle_actions_enabled_.load(std::memory_order_relaxed);
    }

private:
    enum class EventType : uint8_t { Emotion, Gesture, ManualMove };
    // Plain data, copied into the queue: no allocation per event.
    struct Event {
        EventType type;
        int a;  // gesture id / yaw
        int b;  // gesture count / pitch
        char emotion[24];
    };

    static void TaskEntry(void* self);
    void Run();
    void Post(const Event& ev);
    void HandleEvent(const Event& ev, int64_t now_ms);
    void ApplyOutputs(const Outputs& out, int64_t now_ms);
    // True when head commands can do anything: a neck that is holding torque. A position write
    // to a released servo would quietly take the head out of the user's hand.
    bool HeadUsable() const;

    ServoNeck* neck_;
    LedRing* led_;
    QueueHandle_t queue_ = nullptr;
    std::atomic<bool> state_dirty_{true};  // read the device state once at start
    std::atomic<bool> tracking_enabled_{false};
    std::atomic<bool> idle_actions_enabled_{false};

    // Everything below is touched only by the director task.
    BehaviorCore core_;
    GesturePlayer player_;
    bool led_sent_ = false;
    LedCmd last_led_{LedEffect::Off, {0, 0, 0}};
};

}  // namespace anan
