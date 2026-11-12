#pragma once

// The servo neck of the StackChan base: two Feetech SCS0009 on a half-duplex serial bus.

#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "config.h"
#include "feetech_scs.h"   // the MIT SCS bus driver

// The MIT clean-room driver (necobit/feetech_scs_esp_idf), vendored beside this
// file with its two measured fixes. See feetech_scs_LICENSE.txt.
using ScsBus = FeetechScs;

// The servo neck: two Feetech SCS0009 on a half-duplex serial bus, interpolated on this side.
//
// Non-blocking by design. StartMove stages a request and returns; a task calls Tick() every
// MOTION_TICK_MS to advance it. That matters because this robot moves its head WHILE doing other
// things - it poses before speaking, wanders when idle, and moves to the beat - so a MoveHead
// that blocked for the duration of the move would stall speech and expressions with it.
// Upstream's nearest equivalent, electron-bot's Otto::MoveServos, blocks in a vTaskDelay loop,
// which is fine for a dancing robot and not for this one.
//
// Linear advance with a cubic ease-out, and no spring: the fork's spring mode needs
// smooth_ui_toolkit (110 files, 13570 lines, a git submodule) for one interpolator, and upstream
// has no submodules. The curve below is upstream's own, from electron-bot/movements.cc.
class ServoNeck {
public:
    struct Axis {
        int target_deg = 0;
        int start_deg = 0;
        int current_deg = 0;
        uint32_t move_start_ms = 0;
        uint32_t move_duration_ms = 0;
        bool moving = false;
        // Monotonic request id. Tick() snapshots the axis, drops the mutex to use the bus, then
        // re-checks this before trusting what it computed. move_start_ms has millisecond
        // resolution, which cannot separate two rapid requests; a counter can.
        uint64_t request_token = 0;
    };

    static constexpr uint32_t MOTION_TICK_MS = 20;
    // Each WritePos carries its own small move time, so a dropped tick glides rather than steps.
    static constexpr int MOTION_PER_WRITE_TIME_MS = 30;
    static constexpr int BOOT_READPOS_MAX_ATTEMPTS = 8;
    static constexpr int BOOT_READPOS_RETRY_MS = 40;

    // Upstream's curve, from main/boards/electron-bot/movements.cc. Fast at the start, settling
    // into the target without overshooting it.
    static float EaseOutCubic(float t) {
        float inv = 1.0f - t;
        return 1.0f - inv * inv * inv;
    }

    static int YawDegToPos(int deg) {
        int pos = 460 + deg * 16 / 5;
        if (pos < 0) pos = 0;
        if (pos > 1000) pos = 1000;
        return pos;
    }

    static int PitchDegToPos(int deg) {
        // Issue #80: defence in depth - clamp at the servo-write boundary too, so interpolation
        // and any future caller cannot bypass the input-layer clamp.
        if (deg < SAFE_PITCH_MIN) deg = SAFE_PITCH_MIN;
        if (deg > SAFE_PITCH_MAX) deg = SAFE_PITCH_MAX;
        int pos = 620 + deg * 16 / 5;
        if (pos < 0) pos = 0;
        if (pos > 1000) pos = 1000;
        return pos;
    }

    bool Begin();

    bool ready() const { return ready_; }

    // Stage a move. Returns immediately; Tick() does the work.
    void StartMove(int yaw_deg, int pitch_deg, uint32_t duration_ms);

    // Invalidate in-flight requests without staging a new one - used when torque is dropped or
    // the position is re-synced from the servo, neither of which goes through StartMove.
    void InvalidateRequests();

    int yaw_deg() const { return yaw_.current_deg; }
    int pitch_deg() const { return pitch_.current_deg; }
    bool moving() const { return yaw_.moving || pitch_.moving; }

    // True while an interpolated move is still in progress on either axis. Read under the same
    // mutex that stages requests, so it is safe to call from a task other than the Tick() one.
    bool IsMoving() const;

    // The most recently commanded target, in degrees, after StartMove's own clamping. Before
    // any StartMove it is the pose the neck was holding at boot. Read under the request mutex.
    void GetTarget(int& yaw_deg, int& pitch_deg) const;

    bool SetTorque(bool on);

    bool torque_on() const { return torque_on_; }

    // One step of interpolation for both axes. Call every MOTION_TICK_MS from a task.
    void Tick();

    // Ask a servo where it actually is, rather than where we last told it to go. The only way
    // to tell "the head is at zero" from "the head is not plugged in": every other position
    // report on this board is the commanded value echoed back.
    //
    // Takes the same mutex the motion task does - the bus is half duplex, so a read interleaved
    // with a write returns a corrupted frame rather than an error.
    int Ping(uint8_t id);

private:
    static uint32_t NowMs();

    static void Stage(Axis& axis, int target_deg, uint32_t now_ms,
                      uint32_t duration_ms, uint64_t token);

    // Wall-clock linear progress through the eased curve. Wall clock rather than accumulated
    // ticks so bus latency or mutex contention cannot stretch a move indefinitely.
    static void Advance(const Axis& axis, uint32_t now_ms,
                        int& next_deg, bool& still_moving);

    // Read where an axis actually is and immediately command it to stay there.
    void HoldCurrent(uint8_t id, Axis& axis, int (*to_pos)(int), bool clamp_pitch);

    static constexpr const char* TAG_NECK = "ServoNeck";

    ScsBus bus_;
    SemaphoreHandle_t bus_mutex_ = nullptr;
    SemaphoreHandle_t motion_mutex_ = nullptr;
    Axis yaw_;
    Axis pitch_;
    uint64_t next_token_ = 0;
    bool ready_ = false;
    bool torque_on_ = true;
};
