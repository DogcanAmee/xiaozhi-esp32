#include "wifi_board.h"
#include "../core-s3/cores3_audio_codec.h"
#include "display/emote_display.h"
#include "display/lcd_display.h"
#include "application.h"
#include "config.h"
#include "power_save_timer.h"
#include "i2c_device.h"
#include "axp2101.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_ili9341.h>
#include <esp_timer.h>
#include "esp_video.h"
#include <cmath>           // lroundf, for the neck easing in ServoNeck::Advance
#include "feetech_scs.h"   // the MIT SCS bus driver
#include "mcp_server.h"    // the device-side tool surface
#include <atomic>          // the power-save pin, written by tool calls and read on app tasks
#include "py32_io_expander.h"
#include "servo_neck.h"
#include "anan_display.h"
#include "behavior_director.h"
#include "led_ring.h"

#define TAG "StackChanBoard"

class Pmic : public Axp2101 {
public:
    // Power Init
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        uint8_t data = ReadReg(0x90);
        data |= 0b10110100;
        WriteReg(0x90, data);
        WriteReg(0x99, (0b11110 - 5));
        WriteReg(0x97, (0b11110 - 2));
        WriteReg(0x69, 0b00110101);
        WriteReg(0x30, 0b111111);
        WriteReg(0x90, 0xBF);
        WriteReg(0x94, 33 - 5);
        WriteReg(0x95, 33 - 5);
    }

    void SetBrightness(uint8_t brightness) {
        brightness = ((brightness + 641) >> 5);
        WriteReg(0x99, brightness);
    }

    // The die temperature, read from where the die temperature actually lives.
    //
    // The shared Axp2101 helper returns the raw byte of register 0xA5, which is not a
    // temperature; on this board it reads a flat zero. So every board using that helper reports
    // a number that looks like Celsius and is not one.
    //
    // Registers, read order and scaling follow the AXP2101 datasheet V1.0, sections 6.10 and
    // 6.13.2.38-39: the high six bits must be read before the low eight, and bits 7:6 of the
    // high byte are undocumented, so they are masked off. The ADC channel itself is already
    // switched on by the constructor above - 0x30 bit 4, which powers up disabled.
    //
    // The conversion to Celsius is NOT in the datasheet. It is vendor convention, written
    // identically in XPowersLib and M5Unified, so it is followed here and labelled as
    // convention: 7274 counts is 22 degrees, and each further count is a twentieth of a degree
    // colder. Deliberately in floating point - M5Unified does this subtraction in unsigned
    // integers, which wraps into the hundreds for every temperature below 22 degrees, that is,
    // for most rooms.
    bool ReadDieTemperature(float& celsius) {
        uint16_t high = ReadReg(0x3C);
        uint16_t low = ReadReg(0x3D);
        uint16_t raw = (uint16_t)(((high & 0x3F) << 8) | low);
        float value = 22.0f + (7274.0f - (float)raw) / 20.0f;
        // A silent I2C failure reads as zero, which converts to 386 degrees. Report nothing
        // rather than nonsense: the caller then drops the field, and a missing reading is honest.
        if (raw == 0 || value < -40.0f || value > 125.0f) {
            return false;
        }
        celsius = value;
        return true;
    }
};

class CustomBacklight : public Backlight {
public:
    CustomBacklight(Pmic *pmic) : pmic_(pmic) {}

    void SetBrightnessImpl(uint8_t brightness) override {
        pmic_->SetBrightness(target_brightness_);
        brightness_ = target_brightness_;
    }

private:
    Pmic *pmic_;
};

class Aw9523 : public I2cDevice {
public:
    // Exanpd IO Init
    Aw9523(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        WriteReg(0x02, 0b00000111);  // P0
        WriteReg(0x03, 0b10001111);  // P1
        WriteReg(0x04, 0b00011000);  // CONFIG_P0
        WriteReg(0x05, 0b00001100);  // CONFIG_P1
        WriteReg(0x11, 0b00010000);  // GCR P0 port is Push-Pull mode.
        WriteReg(0x12, 0b11111111);  // LEDMODE_P0
        WriteReg(0x13, 0b11111111);  // LEDMODE_P1
    }

    void ResetAw88298() {
        ESP_LOGI(TAG, "Reset AW88298");
        WriteReg(0x02, 0b00000011);
        vTaskDelay(pdMS_TO_TICKS(10));
        WriteReg(0x02, 0b00000111);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    void ResetIli9342() {
        ESP_LOGI(TAG, "Reset IlI9342");
        WriteReg(0x03, 0b10000001);
        vTaskDelay(pdMS_TO_TICKS(20));
        WriteReg(0x03, 0b10000011);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
};

class StackChanBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Pmic* pmic_;
    Aw9523* aw9523_;
    // The PY32 on the StackChan base. Null when it did not answer; the board still boots,
    // but the neck will not move - see EnableServoPower().
    Py32IoExpander* py32_ = nullptr;
    // VM EN, the StackChan base's servo power switch, and whether it read back high.
    static constexpr uint8_t SERVO_POWER_PIN = 0;
    static constexpr uint32_t SERVO_POWER_WAKE_MS = 250;
    // The WS2812 data line of the base ring, driven by the PY32's own LED engine.
    static constexpr uint8_t LED_DATA_PIN = 13;
    bool servo_power_on_ = false;
    // The power-save pin. Written by a tool call, read inside SetPowerSaveLevel, which the
    // application calls from twelve places on its own tasks.
    std::atomic<bool> power_save_pinned_{true};
    std::atomic<int> power_save_pinned_level_{(int)PowerSaveLevel::PERFORMANCE};
    // How many times the pin has overruled the application. Reported rather than logged: the
    // decline logs at debug, which the default log level strips, so without this the one
    // mechanism carried over BECAUSE it was verified by observation would be unobservable.
    std::atomic<uint32_t> power_save_declines_{0};
    // Idle shutdowns declined because the robot was on external power. Counted since boot and
    // reported in the device status, so the gate can be checked without a serial cable.
    std::atomic<uint32_t> idle_shutdown_declines_{0};
    // Logged once per idle period rather than once per second: the shutdown request repeats on
    // every tick while the robot sits idle on external power. Atomic because the decline runs on
    // the esp_timer task while the reset runs on whoever woke the device.
    std::atomic<bool> idle_shutdown_logged_{false};

    // The neck. Null when the bus did not come up: a robot that cannot turn its head should
    // still talk.
    ServoNeck* neck_ = nullptr;
    TaskHandle_t neck_task_ = nullptr;
    // Always created, even without the PY32, so GetLed() is never null and every device-state
    // change still reaches the director.
    anan::LedRing* led_ring_ = nullptr;
    // The only driver of head, gesture and ring behavior. Created after the neck and the PY32.
    anan::BehaviorDirector* director_ = nullptr;
    // Set only in an emote build; the same object as display_.
    anan::AnanDisplay* anan_display_ = nullptr;
    // Display*, not LcdDisplay*: in an emote build this holds an emote::EmoteDisplay, which is
    // not an LcdDisplay. Only ever read through GetDisplay(), and nothing LcdDisplay-specific is
    // called on it.
    Display* display_;
    EspVideo* camera_;
    PowerSaveTimer* power_save_timer_;

    void InitializePowerSaveTimer() {
        // 180 to dim, not 60. The face never stops animating - EmoteDisplay::SetPowerSaveMode is
        // an empty function - so what "no idle expression" actually meant was the backlight
        // dropping to an eighth of normal one minute in. Three minutes is longer than a pause in
        // a conversation, and the floor below is high enough to read from across a desk.
        //
        // The shutdown threshold stays at 300 s and still only fires on battery (see
        // OnShutdownRequest), so nothing about this changes when the robot switches off.
        power_save_timer_ = new PowerSaveTimer(-1, 180, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            // 35 of 100, against a normal 75. Dim enough to be obviously resting, bright enough
            // that the robot still has a face. 10 was a guess and it read as a dead screen.
            GetBacklight()->SetBrightness(35);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
            idle_shutdown_logged_.store(false, std::memory_order_relaxed);
        });
        power_save_timer_->OnShutdownRequest([this]() {
            // Only on battery. Four upstream boards gate this the same way
            // (alientek atk-dnesp32s3-box0 / box2-4g / box2-wifi / box3, each with
            // `if (power_status_ == kDeviceBatterySupply)`), and PowerSaveTimer's own default
            // for seconds_to_shutdown is -1 - upstream does not switch a board off unless the
            // board asks for it.
            //
            // The PMIC is asked here rather than relied on through GetBatteryLevel, which also
            // disables the timer when charging but only runs when something requests the device
            // status. While the gateway was attached that happened constantly; with nothing
            // polling, the gate never ran and a robot on USB power switched itself off after
            // five idle minutes. A check that only works while someone is watching is not a
            // check.
            //
            // Dimming still happens on USB power - that is what power saving should look like
            // for a robot that sits on a desk. Switching off is not power saving when the only
            // thing that can undo it is a person pressing a button.
            //
            // Said once per idle period, not once per second. PowerSaveCheck has no one-shot
            // guard and does not reset ticks_, so once the count passes seconds_to_shutdown
            // this callback is invoked on every tick. The four alientek boards avoid the
            // problem by logging nothing on this path; a count plus one line keeps the gate
            // observable without flooding the log.
            if (pmic_ == nullptr || !pmic_->IsDischarging()) {
                idle_shutdown_declines_.fetch_add(1, std::memory_order_relaxed);
                if (!idle_shutdown_logged_.exchange(true, std::memory_order_relaxed)) {
                    ESP_LOGI(TAG, "idle shutdown declined: running on external power");
                }
                return;
            }
            ESP_LOGW(TAG, "idle on battery; powering off");
            pmic_->PowerOff();
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void I2cDetect() {
        uint8_t address;
        printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\r\n");
        for (int i = 0; i < 128; i += 16) {
            printf("%02x: ", i);
            for (int j = 0; j < 16; j++) {
                fflush(stdout);
                address = i + j;
                esp_err_t ret = i2c_master_probe(i2c_bus_, address, pdMS_TO_TICKS(200));
                if (ret == ESP_OK) {
                    printf("%02x ", address);
                } else if (ret == ESP_ERR_TIMEOUT) {
                    printf("UU ");
                } else {
                    printf("-- ");
                }
            }
            printf("\r\n");
        }
    }

    void InitializeAxp2101() {
        ESP_LOGI(TAG, "Init AXP2101");
        pmic_ = new Pmic(i2c_bus_, 0x34);
    }

    void InitializeAw9523() {
        ESP_LOGI(TAG, "Init AW9523");
        aw9523_ = new Aw9523(i2c_bus_, 0x58);
        vTaskDelay(pdMS_TO_TICKS(50));
    }


    // ---- device status ---------------------------------------------------------------------
    //
    // Upstream's WifiBoard fills this with audio_speaker, screen, battery, network and
    // chip.temperature - the last via Board::GetTemperature, which this board overrides, so the
    // AXP2101 die temperature is already answered here and needs no tool of its own.
    //
    // Adding readings here rather than giving each one a tool is upstream's own shape: one call
    // answers "what is going on", and the model does not have to know which reading lives behind
    // which tool name. Five other upstream boards override this the same way.
    //
    // Absent on purpose, each checked against upstream rather than discovered at compile time:
    // room_audio (TakeRoomAudio is a fork addition to audio_service, inside upstream's AudioEngine
    // rewrite), clock (upstream has no RTC driver at all and this board has not ported one),
    // display flush counters and listen_auto_stop_ms (no upstream source), and the servo
    // auto-release and wobble flags (behaviours this neck does not have - a flag describing
    // machinery that is not there would be a lie rather than a port).
    static const char* PowerSaveLevelName(PowerSaveLevel level) {
        switch (level) {
            case PowerSaveLevel::LOW_POWER:   return "low_power";
            case PowerSaveLevel::BALANCED:    return "balanced";
            case PowerSaveLevel::PERFORMANCE: return "performance";
        }
        return "unknown";
    }

    virtual std::string GetDeviceStatusJson() override {
        std::string base = WifiBoard::GetDeviceStatusJson();
        cJSON* root = cJSON_Parse(base.c_str());
        if (root == nullptr) {
            // Never observed. A parse failure must still not cost the caller the base status.
            ESP_LOGW(TAG, "could not parse base device status; returning it unextended");
            return base;
        }

        cJSON_AddNumberToObject(root, "uptime_seconds",
                                (double)(esp_timer_get_time() / 1000000));

        if (cJSON* servo = BuildServoJson()) {
            cJSON_AddItemToObject(root, "servo", servo);
        }
        cJSON_AddItemToObject(root, "audio", BuildAudioJson());
        // Onto the base status's own network object: whether the radio is being held awake is a
        // fact about the network, and the reason audio does or does not break up.
        if (cJSON* network = cJSON_GetObjectItem(root, "network")) {
            bool pin = power_save_pinned_.load(std::memory_order_acquire);
            cJSON_AddStringToObject(network, "power_save", pin
                ? PowerSaveLevelName((PowerSaveLevel)power_save_pinned_level_.load(
                      std::memory_order_acquire))
                : "auto");
            // Not cleared by reading, unlike the touch and IMU counters: this one is a
            // since-boot total, and two callers asking should get the same answer.
            cJSON_AddNumberToObject(network, "power_save_declines",
                (double)power_save_declines_.load(std::memory_order_relaxed));
        }

        // Charge-complete is distinct from "charging": the base status reports the latter, and a
        // battery sitting full on the dock reads as not charging, same as one on no power at all.
        if (pmic_ != nullptr) {
            cJSON* battery = cJSON_GetObjectItem(root, "battery");
            if (battery != nullptr) {
                cJSON_AddBoolToObject(battery, "charge_done", pmic_->IsChargingDone());
                // A since-boot total, not cleared by reading: nonzero means the robot reached
                // its idle shutdown threshold and stayed on because it was plugged in.
                cJSON_AddNumberToObject(battery, "idle_shutdown_declines",
                    (double)idle_shutdown_declines_.load(std::memory_order_relaxed));
            }
        }

        char* text = cJSON_PrintUnformatted(root);
        std::string result = text ? std::string(text) : base;
        if (text) {
            cJSON_free(text);
        }
        cJSON_Delete(root);
        return result;
    }


    // Whether the neck is holding, and where. The board has always known; until there was a way
    // to ask, a torque release issued by one process was invisible to the next - and a caller
    // that had not issued it would drive the head straight out of somebody's hand.
    cJSON* BuildServoJson() {
        if (neck_ == nullptr) {
            return nullptr;
        }
        cJSON* servo = cJSON_CreateObject();
        cJSON_AddBoolToObject(servo, "torque", neck_->torque_on());
        // False means the base never switched the servo supply on, which looks from the
        // outside exactly like a dead bus or a jammed head.
        cJSON_AddBoolToObject(servo, "power", servo_power_on_);
        cJSON_AddBoolToObject(servo, "moving", neck_->moving());
        cJSON_AddNumberToObject(servo, "yaw", neck_->yaw_deg());
        cJSON_AddNumberToObject(servo, "pitch", neck_->pitch_deg());
        return servo;
    }


    // "It did not wake up" is otherwise diagnosed by reasoning about logs after the fact.
    // AudioService has known the answer all along; nothing could ask it.
    //
    // All four come from upstream accessors (audio_service.h:125/128/131,
    // device_state_machine.h:64). The two the fork never surfaced - the VAD flag and the
    // audio-processor bit - separate "deaf" from "hearing nothing".
    cJSON* BuildAudioJson() {
        cJSON* audio = cJSON_CreateObject();
        auto& app = Application::GetInstance();
        auto& audio_service = app.GetAudioService();
        // Named for what the bit means. The fork called this one "requested" and reported a
        // second flag beside it, but IsWakeWordRunning reads an event-group bit: it is the
        // detector's actual state, not a record of what was asked for.
        cJSON_AddBoolToObject(audio, "wake_word_running", audio_service.IsWakeWordRunning());
        cJSON_AddBoolToObject(audio, "processor_running",
                              audio_service.IsAudioProcessorRunning());
        cJSON_AddBoolToObject(audio, "voice_detected", audio_service.IsVoiceDetected());
        cJSON_AddStringToObject(audio, "device_state",
                                DeviceStateMachine::GetStateName(app.GetDeviceState()));
        return audio;
    }

    // ---- MCP tools -------------------------------------------------------------------------
    //
    // Registered in upstream's shape: McpServer::GetInstance(), declarative Property ranges, and
    // std::unexpected for failures. The error channel is the point - the fork returned cJSON
    // with ok:false because the older callback signature had nowhere to put a failure, and that
    // is how a refused face reported success for weeks (D-476).
    void InitializeNeckTools() {
        auto& mcp_server = McpServer::GetInstance();


        mcp_server.AddTool(
            "self.neck.get_angles",
            "Where the head is now: yaw (horizontal, negative is left) and pitch (vertical, "
            "larger is further up), both in degrees, plus whether a move is still in progress.",
            PropertyList(),
            [this](const PropertyList&) -> ToolResult {
                if (neck_ == nullptr) {
                    return std::unexpected("the servo neck did not start; the head cannot move");
                }
                cJSON* root = cJSON_CreateObject();
                cJSON_AddNumberToObject(root, "yaw", neck_->yaw_deg());
                cJSON_AddNumberToObject(root, "pitch", neck_->pitch_deg());
                cJSON_AddBoolToObject(root, "moving", neck_->moving());
                cJSON_AddBoolToObject(root, "torque", neck_->torque_on());
                return root;
            });

        mcp_server.AddTool(
            "self.neck.set_angles",
            "Turn the head to an absolute yaw and pitch and hold it there. When idle, the head "
            "returns to rest after about 8 seconds; during a conversation it stays until the "
            "conversation ends. Non-blocking: this returns as soon as the move is accepted. "
            "Pitch is limited to the range the neck can reach without hitting a stop. "
            "duration_ms is accepted but ignored: the head turns at its own natural speed.",
            PropertyList({
                // The range the behavior director applies (anan::kYawMin..kYawMax).
                Property("yaw", kPropertyTypeInteger, anan::kYawMin, anan::kYawMax),
                // The same bounds the write path clamps to, declared here so an out-of-range
                // request is refused with a message instead of being silently clamped.
                Property("pitch", kPropertyTypeInteger, SAFE_PITCH_MIN, SAFE_PITCH_MAX),
                // 120 ms is about as fast as the bus and the servo will usefully go; 5 s is a
                // slow deliberate sweep. Default chosen to read as a natural turn.
                Property("duration_ms", kPropertyTypeInteger, 400, 120, 5000),
            }),
            [this](const PropertyList& properties) -> ToolResult {
                if (neck_ == nullptr) {
                    return std::unexpected("the servo neck did not start; the head cannot move");
                }
                if (!neck_->torque_on()) {
                    return std::unexpected("torque is off; enable it with self.neck.set_torque "
                                           "before asking the head to move");
                }
                int yaw = properties["yaw"].value<int>();
                int pitch = properties["pitch"].value<int>();
                int duration = properties["duration_ms"].value<int>();
                if (director_ != nullptr) {
                    // The director owns the head: a manual turn holds for a while, then the
                    // normal behavior takes over again. It moves at its own default speed.
                    anan::Pose used = anan::ClampPose({yaw, pitch});
                    if (!director_->RequestManualMove(used.yaw_deg, used.pitch_deg)) {
                        return std::unexpected("busy, try again");
                    }
                    cJSON* root = cJSON_CreateObject();
                    cJSON_AddNumberToObject(root, "yaw", used.yaw_deg);
                    cJSON_AddNumberToObject(root, "pitch", used.pitch_deg);
                    return root;
                }
                neck_->StartMove(yaw, pitch, static_cast<uint32_t>(duration));
                cJSON* root = cJSON_CreateObject();
                cJSON_AddNumberToObject(root, "yaw", yaw);
                cJSON_AddNumberToObject(root, "pitch", pitch);
                cJSON_AddNumberToObject(root, "duration_ms", duration);
                return root;
            });

        mcp_server.AddTool(
            "self.neck.set_torque",
            "Hold the head in place, or let go of it. With torque off the head can be posed by "
            "hand and will stay where it is put; any move in progress is abandoned. Turning it "
            "back on holds wherever the head currently is.",
            PropertyList({
                Property("on", kPropertyTypeBoolean),
            }),
            [this](const PropertyList& properties) -> ToolResult {
                if (neck_ == nullptr) {
                    return std::unexpected("the servo neck did not start");
                }
                bool on = properties["on"].value<bool>();
                if (!neck_->SetTorque(on)) {
                    return std::unexpected("the servos did not acknowledge the torque change");
                }
                return on;
            });
    }

    // Gesture tools, plus self.conversation.end. Registered whether or not the neck started, so
    // a missing neck is reported as an error rather than as an unknown tool.
    void InitializeHeadTools() {
        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool(
            "self.head.nod",
            "Nod the head. Call this when you agree or say yes.",
            PropertyList({
                Property("count", kPropertyTypeInteger, 1, 1, 3),
            }),
            [this](const PropertyList& properties) -> ToolResult {
                return RequestHeadGesture(anan::Gesture::Nod, properties["count"].value<int>());
            });

        mcp_server.AddTool(
            "self.head.shake",
            "Shake the head. Call this when you disagree or say no.",
            PropertyList({
                Property("count", kPropertyTypeInteger, 1, 1, 3),
            }),
            [this](const PropertyList& properties) -> ToolResult {
                return RequestHeadGesture(anan::Gesture::Shake, properties["count"].value<int>());
            });

        // Nothing to do with the neck: it only needs the director, so it works without one.
        mcp_server.AddTool(
            "self.conversation.end",
            "End the current conversation and return to standby. Call this when the user tells "
            "you to rest, sleep, stop, be quiet, or says goodbye. Say a short goodbye first, "
            "then call this tool.",
            PropertyList(),
            [this](const PropertyList&) -> ToolResult {
                if (director_ == nullptr || !director_->RequestEndConversation()) {
                    return std::unexpected("busy, try again");
                }
                return true;
            });
    }

    ToolResult RequestHeadGesture(anan::Gesture gesture, int count) {
        if (neck_ == nullptr || director_ == nullptr) {
            return std::unexpected("the neck is unavailable");
        }
        if (!neck_->torque_on()) {
            return std::unexpected("torque is off; enable it with self.neck.set_torque "
                                   "before asking the head to move");
        }
        if (!director_->RequestGesture(gesture, anan::ClampGestureCount(count))) {
            return std::unexpected("busy, try again");
        }
        return true;
    }

    // After the neck and the PY32: the director needs both (either may be null).
    void InitializeBehavior() {
        led_ring_ = new anan::LedRing(py32_, [this]() {
            if (director_ != nullptr) {
                director_->NotifyStateChanged();
            }
        });
        director_ = new anan::BehaviorDirector(neck_, led_ring_);
        director_->Start();
        if (anan_display_ != nullptr) {
            anan_display_->SetDirector(director_);
        }
        InitializeHeadTools();
    }

    void InitializeServoNeck() {
        neck_ = new ServoNeck();
        if (!neck_->Begin()) {
            delete neck_;
            neck_ = nullptr;
            ESP_LOGW(TAG, "servo neck unavailable; the head will not move");
            return;
        }
        // Tick on its own task. StartMove is non-blocking precisely so that speaking, expressions
        // and touch keep running while the head is in motion.
        xTaskCreate([](void* self) {
            auto* board = static_cast<StackChanBoard*>(self);
            while (true) {
                board->neck_->Tick();
                vTaskDelay(pdMS_TO_TICKS(ServoNeck::MOTION_TICK_MS));
            }
        }, "servo_neck", 3072, this, 4, &neck_task_);
        // Now that the boot hold has secured wherever the servos were, move to the resting pose.
        neck_->StartMove(BOOT_INIT_YAW_DEG, BOOT_INIT_PITCH_DEG, 1500);
        ESP_LOGI(TAG, "servo neck ready; centring to yaw %d pitch %d",
                 BOOT_INIT_YAW_DEG, BOOT_INIT_PITCH_DEG);
        InitializeNeckTools();
    }

    void InitializePy32() {
        ESP_LOGI(TAG, "init PY32 IO expander (I2C 0x%02X)", Py32IoExpander::DEFAULT_ADDR);
        py32_ = new Py32IoExpander(i2c_bus_);
        uint8_t version = 0;
        if (!py32_->Begin(&version)) {
            delete py32_;
            py32_ = nullptr;
            // Same rule as the sensors: a missing base ring costs the robot a light, not a boot.
            ESP_LOGW(TAG, "PY32 unavailable; base LEDs and GPIO diagnostics are off");
            return;
        }
        ESP_LOGI(TAG, "PY32 ready (version 0x%02X)", version);
        EnableServoPower();
        ConfigureLedDataPin();
    }

    // The PY32's WS2812 engine drives the ring through pin 13, which must be a push-pull output.
    // Nothing else sets it up. A failure costs the ring, not the boot.
    void ConfigureLedDataPin() {
        if (py32_ == nullptr) {
            return;
        }
        if (!py32_->SetDirection(LED_DATA_PIN, true)) {
            ESP_LOGW(TAG, "LED data pin %d: SetDirection failed; the ring may stay dark",
                     LED_DATA_PIN);
        }
        if (!py32_->SetDriveMode(LED_DATA_PIN, false)) {  // false = push-pull
            ESP_LOGW(TAG, "LED data pin %d: SetDriveMode failed; the ring may stay dark",
                     LED_DATA_PIN);
        }
    }

    // The StackChan base gates servo power behind PY32 pin 0 - VM EN. Until this runs, the two
    // SCS0009 have no supply at all: they answer no reads, ignore every write, and can be
    // pushed around by hand because they are not holding torque. That is exactly what this
    // board did from u1 until now, and the boot warning "servo N did not report its position"
    // was it saying so every time.
    //
    // Each step is reported separately rather than left to a combined bool, because "direction
    // set but the level did not latch" and "the chip never answered" need different next moves.
    void EnableServoPower() {
        if (py32_ == nullptr) {
            return;
        }
        bool ok_dir = py32_->SetDirection(SERVO_POWER_PIN, true);
        bool ok_pull = py32_->SetPullMode(SERVO_POWER_PIN, true);
        bool ok_write = py32_->DigitalWrite(SERVO_POWER_PIN, true);
        if (!ok_dir || !ok_pull || !ok_write) {
            ESP_LOGE(TAG, "servo power: failed at %s; the head will not move",
                     !ok_dir ? "SetDirection" : (!ok_pull ? "SetPullMode" : "DigitalWrite"));
            return;
        }
        // The SCS0009 needs roughly 200 ms after its supply comes up before it answers. Waiting
        // here rather than letting the neck's own boot read fail is the whole point: that read
        // happens a few hundred milliseconds from now and is what pins the head where it
        // already is, so it must not be the one paying for the wake-up.
        vTaskDelay(pdMS_TO_TICKS(SERVO_POWER_WAKE_MS));

        // Read the output register back. The chip can acknowledge a write whose level never
        // latched, and the difference matters: one means the servos are live, the other means
        // every move from here on is silent.
        uint8_t out_low = 0;
        if (!py32_->ReadOutputLow(&out_low)) {
            ESP_LOGW(TAG, "servo power: asserted, but the readback failed - cannot confirm");
            servo_power_on_ = true;  // the writes were acknowledged; assume they took
            return;
        }
        servo_power_on_ = (out_low & (1u << SERVO_POWER_PIN)) != 0;
        if (servo_power_on_) {
            ESP_LOGI(TAG, "servo power on (VM EN high, REG_GPIO_O_L=0x%02X)", out_low);
        } else {
            ESP_LOGE(TAG, "servo power: write acknowledged but pin %d reads low "
                          "(REG_GPIO_O_L=0x%02X) - the head will not move",
                     SERVO_POWER_PIN, out_low);
        }
    }


    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = GPIO_NUM_37;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = GPIO_NUM_36;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeIli9342Display() {
        ESP_LOGI(TAG, "Init IlI9342");

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_3;
        io_config.dc_gpio_num = GPIO_NUM_35;
        io_config.spi_mode = 2;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
        
        esp_lcd_panel_reset(panel);
        aw9523_->ResetIli9342();

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        // Turn the panel on here, not in the display. LcdDisplay does this itself (its
        // "Turning display on" line) and EmoteDisplay does not, so while this board created an
        // lvgl display the omission was invisible - and the moment it created an emote display
        // instead, the screen went black while SetEmotion and SetChatMessage carried on being
        // delivered perfectly. The three upstream emote boards all call it right here, before
        // the display exists, which is why it works for either one.
        esp_lcd_panel_disp_on_off(panel, true);

        // The emote build has to actually build the emote display. config.json sets
        // CONFIG_USE_EMOTE_MESSAGE_STYLE=y and this used to create a SpiLcdDisplay regardless,
        // so the firmware believed it was an emote build while running an lvgl display - and
        // because Display::MountAssets only does anything on an EmoteDisplay, the assets
        // partition was marked invalid, which in turn meant srmodels.bin was never read and the
        // device could not hear its own name. The faces and the wake word were one bug.
        //
        // This is the shape six upstream boards use (esp32-s3-box, esp32-s3-box-3, esp-vocat,
        // esp-sensairshuttle, lckfb/szpi-esp32s3, rymcu/bigsmart).
        //
        // Mind the argument order: EmoteDisplay takes the panel first, SpiLcdDisplay takes the
        // panel_io first. Both are opaque handle typedefs, so getting it wrong compiles cleanly
        // and fails on the device.
#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        // AnanDisplay is EmoteDisplay plus the emotion hook for the behavior director.
        anan_display_ = new anan::AnanDisplay(panel, panel_io, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        display_ = anan_display_;
#else
        display_ = new SpiLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
#endif
    }

     void InitializeCamera() {
        static esp_cam_ctlr_dvp_pin_config_t dvp_pin_config = {
            .data_width = CAM_CTLR_DATA_WIDTH_8,
            .data_io = {
                [0] = CAMERA_PIN_D0,
                [1] = CAMERA_PIN_D1,
                [2] = CAMERA_PIN_D2,
                [3] = CAMERA_PIN_D3,
                [4] = CAMERA_PIN_D4,
                [5] = CAMERA_PIN_D5,
                [6] = CAMERA_PIN_D6,
                [7] = CAMERA_PIN_D7,
            },
            .vsync_io = CAMERA_PIN_VSYNC,
            .de_io = CAMERA_PIN_HREF,
            .pclk_io = CAMERA_PIN_PCLK,
            .xclk_io = CAMERA_PIN_XCLK,
        };

        esp_video_init_sccb_config_t sccb_config = {
            .init_sccb = false,
            .i2c_handle = i2c_bus_,
            .freq = 100000,
        };

        esp_video_init_dvp_config_t dvp_config = {
            .sccb_config = sccb_config,
            .reset_pin = CAMERA_PIN_RESET,
            .pwdn_pin = CAMERA_PIN_PWDN,
            .dvp_pin = dvp_pin_config,
            .xclk_freq = XCLK_FREQ_HZ,
        };

        esp_video_init_config_t video_config = {
            .dvp = &dvp_config,
        };

        camera_ = new EspVideo(video_config);
    }

public:
    StackChanBoard() {
        InitializePowerSaveTimer();
        InitializeI2c();
        InitializeAxp2101();
        InitializeAw9523();
        I2cDetect();
        InitializeSpi();
        InitializeIli9342Display();
        InitializeCamera();
        InitializePy32();
        InitializeServoNeck();
        InitializeBehavior();
        GetBacklight()->RestoreBrightness();
    }

    // Was NoLed from the base class until now, so every OnStateChanged() upstream makes went
    // nowhere and the ring only lit when a tool asked it to by name. Now every state change
    // reaches the behavior director, which picks the ring's colour.
    virtual Led* GetLed() override {
        return led_ring_;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static CoreS3AudioCodec audio_codec(i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_AW88298_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }

    // Board::GetTemperature defaults to returning false, and core-s3 does not override it. This
    // board can answer, so it does - and the answer comes from 0x3C/0x3D rather than the shared
    // helper's 0xA5, which is not a temperature at all.
    virtual bool GetTemperature(float& esp32temp) override {
        if (pmic_ == nullptr) {
            return false;
        }
        return pmic_->ReadDieTemperature(esp32temp);
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = pmic_->IsCharging();
        discharging = pmic_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }

        level = pmic_->GetBatteryLevel();
        return true;
    }

    // application.cc sets the power-save level from twelve places, dropping to LOW_POWER every
    // time it goes idle, so a level set from outside survives only until the application next
    // changes state - which is what made push-frame audio stutter on this board. No upstream
    // board overrides this to hold a level.
    //
    // Pinned at PERFORMANCE by default. There is no tool to release the pin in this PR; the
    // atomics are here because the tool that sets them belongs with the rest of the device
    // controls rather than with the board bring-up.
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        // Two different things arrive through this one call, and they must stay separate.
        //
        // The level the application ASKED for is an activity signal: it asks for PERFORMANCE when
        // something is happening, which is the cue for the screen-dim timer. That stays driven by
        // the request, whatever the radio ends up doing - otherwise pinning the radio awake would
        // pin the screen on too, and declining a LOW_POWER request would stop the display ever
        // dimming.
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }

        // The radio is the pin's business (wn9-29). application.cc sets the level from twelve
        // places and drops to LOW_POWER at every idle, so a level set from outside survives only
        // until the next state change unless it is held here.
        if (power_save_pinned_.load(std::memory_order_acquire)) {
            PowerSaveLevel pinned =
                (PowerSaveLevel)power_save_pinned_level_.load(std::memory_order_acquire);
            if (level != pinned) {
                // Debug rather than info: the application asks a dozen times a conversation, and
                // at info this would bury everything else in the log.
                power_save_declines_.fetch_add(1, std::memory_order_relaxed);
                ESP_LOGD(TAG, "declining power save %s; pinned at %s",
                         PowerSaveLevelName(level), PowerSaveLevelName(pinned));
                // Re-asserted, not merely refused. The companion process needed its own periodic
                // re-assert for the same reason: the level is not reliably sticky just because
                // nothing visible changed it.
                WifiBoard::SetPowerSaveLevel(pinned);
                return;
            }
        }
        WifiBoard::SetPowerSaveLevel(level);
    }

    virtual Backlight *GetBacklight() override {
        static CustomBacklight backlight(pmic_);
        return &backlight;
    }
};

DECLARE_BOARD(StackChanBoard);
