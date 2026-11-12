#include "servo_neck.h"

#include <cmath>           // lroundf, for the neck easing in ServoNeck::Advance

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/task.h>

bool ServoNeck::Begin() {
    bus_mutex_ = xSemaphoreCreateMutex();
    motion_mutex_ = xSemaphoreCreateMutex();
    if (bus_mutex_ == nullptr || motion_mutex_ == nullptr) {
        ESP_LOGE(TAG_NECK, "could not create mutexes");
        return false;
    }
    bus_.begin(SERVO_UART_NUM, SERVO_BAUDRATE, SERVO_TX_PIN, SERVO_RX_PIN);
    // ACK reading stays on (SCS::Level defaults to 1): with it off, a WritePos arriving while
    // the servo is still processing the previous one is silently dropped.
    ready_ = true;

    // Hold whatever position the servos are already in, before anything else.
    //
    // The SCS0009 remembers its last commanded set-point across a reset of the ESP32 but not
    // across its own power cycle, so on boot the head can drop or snap into an end stop
    // (issues #121/#123). Reading the current position and immediately writing it back with
    // time=0 pins it. Pitch first, because pitch is the axis with a stop to snap into.
    HoldCurrent(SERVO_PITCH_ID, pitch_, &ServoNeck::PitchDegToPos, true);
    HoldCurrent(SERVO_YAW_ID, yaw_, &ServoNeck::YawDegToPos, false);
    return true;
}

void ServoNeck::StartMove(int yaw_deg, int pitch_deg, uint32_t duration_ms) {
    if (!ready_) {
        return;
    }
    if (pitch_deg < SAFE_PITCH_MIN) pitch_deg = SAFE_PITCH_MIN;
    if (pitch_deg > SAFE_PITCH_MAX) pitch_deg = SAFE_PITCH_MAX;
    if (duration_ms == 0) duration_ms = MOTION_TICK_MS;
    uint32_t now_ms = NowMs();
    xSemaphoreTake(motion_mutex_, portMAX_DELAY);
    uint64_t token = ++next_token_;
    Stage(yaw_, yaw_deg, now_ms, duration_ms, token);
    Stage(pitch_, pitch_deg, now_ms, duration_ms, token);
    xSemaphoreGive(motion_mutex_);
}

void ServoNeck::InvalidateRequests() {
    if (motion_mutex_ == nullptr) {
        return;
    }
    xSemaphoreTake(motion_mutex_, portMAX_DELAY);
    ++next_token_;
    yaw_.moving = false;
    pitch_.moving = false;
    xSemaphoreGive(motion_mutex_);
}

bool ServoNeck::IsMoving() const {
    if (motion_mutex_ == nullptr) {
        return false;
    }
    xSemaphoreTake(motion_mutex_, portMAX_DELAY);
    bool moving = yaw_.moving || pitch_.moving;
    xSemaphoreGive(motion_mutex_);
    return moving;
}

void ServoNeck::GetTarget(int& yaw_deg, int& pitch_deg) const {
    if (motion_mutex_ == nullptr) {
        yaw_deg = yaw_.target_deg;
        pitch_deg = pitch_.target_deg;
        return;
    }
    xSemaphoreTake(motion_mutex_, portMAX_DELAY);
    yaw_deg = yaw_.target_deg;
    pitch_deg = pitch_.target_deg;
    xSemaphoreGive(motion_mutex_);
}

bool ServoNeck::SetTorque(bool on) {
    if (!ready_) {
        return false;
    }
    if (!on) {
        // Anything in flight would fight the release.
        InvalidateRequests();
    }
    xSemaphoreTake(bus_mutex_, portMAX_DELAY);
    // EnableTorque takes uint8_t, not bool - made explicit rather than left to an
    // implicit conversion.
    uint8_t flag = on ? 1 : 0;
    bool ok = bus_.EnableTorque(SERVO_YAW_ID, flag) == 0 &&
              bus_.EnableTorque(SERVO_PITCH_ID, flag) == 0;
    xSemaphoreGive(bus_mutex_);
    if (ok) {
        torque_on_ = on;
    }
    return ok;
}

void ServoNeck::Tick() {
    if (!ready_) {
        return;
    }
    uint32_t now_ms = NowMs();

    Axis yaw_local, pitch_local;
    uint64_t token;
    xSemaphoreTake(motion_mutex_, portMAX_DELAY);
    if (!yaw_.moving && !pitch_.moving) {
        xSemaphoreGive(motion_mutex_);
        return;
    }
    yaw_local = yaw_;
    pitch_local = pitch_;
    token = next_token_;
    int yaw_next = yaw_.current_deg;
    bool yaw_moving = yaw_.moving;
    Advance(yaw_local, now_ms, yaw_next, yaw_moving);
    int pitch_next = pitch_.current_deg;
    bool pitch_moving = pitch_.moving;
    Advance(pitch_local, now_ms, pitch_next, pitch_moving);
    xSemaphoreGive(motion_mutex_);

    // The bus is slow relative to the mutex, so it is used without holding motion_mutex_.
    // A request arriving in this window would make the writes below stale, which is what the
    // token re-check after them is for.
    xSemaphoreTake(bus_mutex_, portMAX_DELAY);
    if (yaw_local.moving) {
        bus_.WritePos(SERVO_YAW_ID, YawDegToPos(yaw_next), MOTION_PER_WRITE_TIME_MS, 0);
    }
    if (pitch_local.moving) {
        bus_.WritePos(SERVO_PITCH_ID, PitchDegToPos(pitch_next), MOTION_PER_WRITE_TIME_MS, 0);
    }
    xSemaphoreGive(bus_mutex_);

    xSemaphoreTake(motion_mutex_, portMAX_DELAY);
    // Only commit if this is still the request we computed from.
    if (next_token_ == token) {
        yaw_.current_deg = yaw_next;
        yaw_.moving = yaw_moving;
        pitch_.current_deg = pitch_next;
        pitch_.moving = pitch_moving;
    }
    xSemaphoreGive(motion_mutex_);
}

int ServoNeck::Ping(uint8_t id) {
    if (!ready_) {
        return -1;
    }
    xSemaphoreTake(bus_mutex_, portMAX_DELAY);
    int pos = bus_.ReadPos(id);
    xSemaphoreGive(bus_mutex_);
    return pos;
}

uint32_t ServoNeck::NowMs() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

void ServoNeck::Stage(Axis& axis, int target_deg, uint32_t now_ms,
                      uint32_t duration_ms, uint64_t token) {
    axis.start_deg = axis.current_deg;
    axis.target_deg = target_deg;
    axis.move_start_ms = now_ms;
    axis.move_duration_ms = duration_ms;
    axis.moving = (target_deg != axis.current_deg);
    axis.request_token = token;
}

void ServoNeck::Advance(const Axis& axis, uint32_t now_ms,
                        int& next_deg, bool& still_moving) {
    if (!axis.moving) {
        return;
    }
    uint32_t elapsed = now_ms - axis.move_start_ms;
    if (axis.move_duration_ms == 0 || elapsed >= axis.move_duration_ms) {
        next_deg = axis.target_deg;
        still_moving = false;
        return;
    }
    float t = static_cast<float>(elapsed) / static_cast<float>(axis.move_duration_ms);
    float eased = EaseOutCubic(t);
    int delta = axis.target_deg - axis.start_deg;
    next_deg = axis.start_deg + static_cast<int>(lroundf(delta * eased));
}

void ServoNeck::HoldCurrent(uint8_t id, Axis& axis, int (*to_pos)(int), bool clamp_pitch) {
    int pos = -1;
    for (int i = 0; i < BOOT_READPOS_MAX_ATTEMPTS; ++i) {
        xSemaphoreTake(bus_mutex_, portMAX_DELAY);
        pos = bus_.ReadPos(id);
        xSemaphoreGive(bus_mutex_);
        if (pos >= 0) {
            break;
        }
        // The chip needs roughly 200 ms after power-up before it answers (issue #138).
        vTaskDelay(pdMS_TO_TICKS(BOOT_READPOS_RETRY_MS));
    }
    if (pos < 0) {
        ESP_LOGW(TAG_NECK, "servo %u did not report its position; not holding it", id);
        return;
    }
    // Convert back to degrees using the same scale the write path uses.
    int centre = clamp_pitch ? 620 : 460;
    int deg = (pos - centre) * 5 / 16;
    if (clamp_pitch) {
        if (deg < SAFE_PITCH_MIN) deg = SAFE_PITCH_MIN;
        if (deg > SAFE_PITCH_MAX) deg = SAFE_PITCH_MAX;
    }
    xSemaphoreTake(bus_mutex_, portMAX_DELAY);
    // time=0: hold it exactly here, right now.
    bus_.WritePos(id, to_pos(deg), 0, 0);
    xSemaphoreGive(bus_mutex_);
    axis.current_deg = deg;
    axis.target_deg = deg;
    axis.moving = false;
    ESP_LOGI(TAG_NECK, "servo %u held at raw %d (%d deg)", id, pos, deg);
}
