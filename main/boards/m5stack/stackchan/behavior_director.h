#pragma once

// The one task that drives BehaviorCore and turns its outputs into neck moves, gestures and
// LED ring commands. Every public Notify*/Request* method only posts to this task (a queue,
// or an atomic flag for device-state changes), so they are safe to call from any task.

#include <atomic>
#include <cstdint>
#include <optional>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "behavior_core.h"
#include "director_logic.h"
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
    // ServoNeck as the director's HeadPort. Called only from the director task.
    class NeckHead : public HeadPort {
    public:
        explicit NeckHead(ServoNeck* neck) : neck_(neck) {}
        // A position write to a released servo would quietly take the head out of the user's
        // hand, so a neck without torque counts as unusable.
        bool Usable() const override { return neck_->torque_on(); }
        void StageMove(Pose pose, uint32_t duration_ms) override {
            neck_->StartMove(pose.yaw_deg, pose.pitch_deg, duration_ms);
        }
        void Invalidate() override { neck_->InvalidateRequests(); }
        Pose Target() const override {
            Pose p{0, 0};
            neck_->GetTarget(p.yaw_deg, p.pitch_deg);
            return p;
        }

    private:
        ServoNeck* neck_;
    };

    static void TaskEntry(void* self);
    void Run();
    void Post(const DirectorInput& in);
    void ApplyLevelsAndLed(const Outputs& out);

    std::optional<NeckHead> head_;  // empty when there is no neck
    LedRing* led_;
    QueueHandle_t queue_ = nullptr;
    std::atomic<bool> state_dirty_{true};  // read the device state once at start
    std::atomic<bool> tracking_enabled_{false};
    std::atomic<bool> idle_actions_enabled_{false};

    // Everything below is touched only by the director task.
    DirectorLogic logic_;
    bool led_sent_ = false;
    LedCmd last_led_{LedEffect::Off, {0, 0, 0}};
};

}  // namespace anan
