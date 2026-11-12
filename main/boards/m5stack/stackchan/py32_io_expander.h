#pragma once

// The PY32 companion chip on the StackChan base (GPIO expander + WS2812 ring engine).

#include <cstddef>
#include <cstdint>

#include <driver/i2c_master.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>


// The PY32 companion microcontroller on the internal bus (Marco, D-440 era). It owns the
// base LED ring through its own WS2812 engine - the ESP32 writes packed RGB565 into the
// chip's LED RAM and asks it to latch - and it also provides the GPIO registers the
// diagnostic tools use. Upstream has no equivalent for either half.
//
// It adds its own I2C device at 100 kHz rather than inheriting I2cDevice, because the PY32
// wants 100 kHz while the rest of this bus runs at 400 kHz; the IDF driver carries the
// speed per device, so both live on one bus.
class Py32IoExpander {
public:
    static constexpr uint8_t  DEFAULT_ADDR = 0x6F;
    static constexpr uint32_t I2C_FREQ_HZ  = 100000;  // 100 kHz (M5 default)
    static constexpr uint8_t  REG_GPIO_O_L_PUBLIC = 0x05;  // exposed for verify

    Py32IoExpander(i2c_master_bus_handle_t i2c_bus, uint8_t addr = DEFAULT_ADDR) {
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = addr,
            .scl_speed_hz    = I2C_FREQ_HZ,
            .scl_wait_us     = 0,
            .flags           = { .disable_ack_check = 0 },
        };
        ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &cfg, &i2c_device_));
    }

    // Probe the chip. On success, returns true and (if non-null) writes the
    // version byte to out_version. Internal reads use SafeReadReg, which
    // already retries on transient I2C errors — so the chip is genuinely
    // unreachable / not yet ready when this returns false.
    bool Begin(uint8_t* out_version = nullptr) {
        uint8_t version = 0;
        if (!SafeReadReg(REG_VERSION, &version)) {
            return false;
        }
        if (version == 0x00 || version == 0xFF) {
            return false;
        }
        if (out_version != nullptr) {
            *out_version = version;
        }
        return true;
    }

    // direction: false=input, true=output. Accepts pin 0..15 (PY32 has 14
    // GPIOs; the WS2812 data line is on pin 13, in the high byte).
    bool SetDirection(uint8_t pin, bool output) {
        return WriteBitWideSafe(REG_GPIO_M_L, REG_GPIO_M_H, pin, output);
    }

    // mode: false=pull down, true=pull up. Accepts pin 0..15.
    bool SetPullMode(uint8_t pin, bool up) {
        if (up) {
            bool a = WriteBitWideSafe(REG_GPIO_PD_L, REG_GPIO_PD_H, pin, false);
            bool b = WriteBitWideSafe(REG_GPIO_PU_L, REG_GPIO_PU_H, pin, true);
            return a && b;
        } else {
            bool a = WriteBitWideSafe(REG_GPIO_PU_L, REG_GPIO_PU_H, pin, false);
            bool b = WriteBitWideSafe(REG_GPIO_PD_L, REG_GPIO_PD_H, pin, true);
            return a && b;
        }
    }

    bool DigitalWrite(uint8_t pin, bool level) {
        return WriteBitSafe(REG_GPIO_O_L, pin, level);
    }

    // Read back the current output low-byte register (pins 0..7) for
    // verification after DigitalWrite. Returns false if the read failed.
    bool ReadOutputLow(uint8_t* out) {
        return SafeReadReg(REG_GPIO_O_L, out);
    }

    // Drive mode for any pin (0..15). false=push-pull, true=open-drain.
    // The WS2812 data line on pin 13 must be push-pull.
    bool SetDriveMode(uint8_t pin, bool open_drain) {
        return WriteBitWideSafe(REG_GPIO_DRV_L, REG_GPIO_DRV_H, pin, open_drain);
    }

    // Show `count` LEDs (1..LED_MAX_COUNT). rgb565 holds one packed RGB565 value per LED.
    // Two steps, in this order: burst-write the colours into LED RAM (little-endian, 2 bytes
    // per LED, from REG_LED_RAM_START), then write REG_LED_CFG with the LED count in bits
    // [5:0] and the latch bit [6] set, which makes the chip push the frame out to the ring.
    // Returns false if `count` is out of range or either write failed.
    bool WriteLeds(const uint16_t* rgb565, size_t count) {
        if (rgb565 == nullptr || count == 0 || count > LED_MAX_COUNT) {
            return false;
        }
        uint8_t buf[1 + 2 * LED_MAX_COUNT];
        buf[0] = REG_LED_RAM_START;
        for (size_t i = 0; i < count; i++) {
            buf[1 + 2 * i] = (uint8_t)(rgb565[i] & 0xFF);
            buf[2 + 2 * i] = (uint8_t)(rgb565[i] >> 8);
        }
        if (!SafeWriteRaw(buf, 1 + 2 * count)) {
            return false;
        }
        return SafeWriteReg(REG_LED_CFG, (uint8_t)(count | LED_CFG_LATCH));
    }


private:
    // Owned device handle (NOT inherited from I2cDevice — see class comment).
    // Registered at 100 kHz in the constructor so this transport runs slower
    // than the rest of the bus.
    i2c_master_dev_handle_t i2c_device_ = nullptr;

    static constexpr uint8_t REG_VERSION  = 0x02;
    static constexpr uint8_t REG_GPIO_M_L = 0x03;  // Direction (mode) low byte
    static constexpr uint8_t REG_GPIO_M_H = 0x04;  // Direction (mode) high byte
    static constexpr uint8_t REG_GPIO_O_L = 0x05;  // Output low byte
    static constexpr uint8_t REG_GPIO_O_H = 0x06;  // Output high byte
    static constexpr uint8_t REG_GPIO_PU_L = 0x09; // Pull-up low byte
    static constexpr uint8_t REG_GPIO_PU_H = 0x0A; // Pull-up high byte
    static constexpr uint8_t REG_GPIO_PD_L = 0x0B; // Pull-down low byte
    static constexpr uint8_t REG_GPIO_PD_H = 0x0C; // Pull-down high byte
    static constexpr uint8_t REG_GPIO_DRV_L = 0x13; // Drive mode low byte
    static constexpr uint8_t REG_GPIO_DRV_H = 0x14; // Drive mode high byte
    static constexpr uint8_t REG_LED_CFG       = 0x24;  // count[5:0] + latch[6]
    static constexpr uint8_t REG_LED_RAM_START = 0x30;  // 2 bytes per LED, RGB565 LE
    static constexpr uint8_t LED_CFG_LATCH     = 0x40;  // REG_LED_CFG bit 6
    static constexpr size_t  LED_MAX_COUNT     = 12;    // the base ring

    // I2C op transient-retry parameters. Total budget per failed op is
    // (I2C_INNER_RETRIES - 1) * I2C_RETRY_DELAY_MS, i.e. ~30 ms here.
    static constexpr int I2C_INNER_RETRIES  = 3;
    static constexpr int I2C_RETRY_DELAY_MS = 15;

    // Safe I2C read — retries up to I2C_INNER_RETRIES on transient errors,
    // logs WARN only on the final failure to keep the log readable.
    bool SafeReadReg(uint8_t reg, uint8_t* out) {
        esp_err_t err = ESP_FAIL;
        for (int i = 0; i < I2C_INNER_RETRIES; i++) {
            err = i2c_master_transmit_receive(i2c_device_, &reg, 1, out, 1, 100);
            if (err == ESP_OK) {
                return true;
            }
            if (i + 1 < I2C_INNER_RETRIES) {
                vTaskDelay(pdMS_TO_TICKS(I2C_RETRY_DELAY_MS));
            }
        }
        ESP_LOGW("Py32IoExpander", "I2C read reg 0x%02X failed after %d tries: 0x%X",
                 reg, I2C_INNER_RETRIES, err);
        return false;
    }

    // Safe I2C write — same retry semantics as SafeReadReg.
    bool SafeWriteReg(uint8_t reg, uint8_t value) {
        uint8_t buffer[2] = {reg, value};
        esp_err_t err = ESP_FAIL;
        for (int i = 0; i < I2C_INNER_RETRIES; i++) {
            err = i2c_master_transmit(i2c_device_, buffer, 2, 100);
            if (err == ESP_OK) {
                return true;
            }
            if (i + 1 < I2C_INNER_RETRIES) {
                vTaskDelay(pdMS_TO_TICKS(I2C_RETRY_DELAY_MS));
            }
        }
        ESP_LOGW("Py32IoExpander", "I2C write reg 0x%02X failed after %d tries: 0x%X",
                 reg, I2C_INNER_RETRIES, err);
        return false;
    }

    // Read-Modify-Write a single bit using safe I2C. Pin 0..7 only.
    // Returns false if either the read or the write step ultimately failed.
    bool WriteBitSafe(uint8_t reg, uint8_t pin, bool value) {
        if (pin >= 8) {
            return false;
        }
        uint8_t v = 0;
        if (!SafeReadReg(reg, &v)) {
            return false;
        }
        if (value) {
            v |= (uint8_t)(1u << pin);
        } else {
            v &= (uint8_t)~(1u << pin);
        }
        return SafeWriteReg(reg, v);
    }

    // 16-bit RMW: pin 0..7 -> reg_l, pin 8..15 -> reg_h. Used for any pin
    // beyond pin 7 (LED data line is on pin 13, so all the LED setup goes
    // through this path).
    bool WriteBitWideSafe(uint8_t reg_l, uint8_t reg_h, uint8_t pin, bool value) {
        if (pin >= 16) return false;
        uint8_t reg = (pin < 8) ? reg_l : reg_h;
        uint8_t bit = (uint8_t)(pin & 0x07);
        uint8_t v = 0;
        if (!SafeReadReg(reg, &v)) return false;
        if (value) v |= (uint8_t)(1u << bit);
        else       v &= (uint8_t)~(1u << bit);
        return SafeWriteReg(reg, v);
    }

    // Burst write: ship a pre-built {reg, ...payload} buffer in a single
    // i2c_master_transmit. Used by the LED RAM writes which would otherwise
    // require dozens of individual register writes. Same retry semantics
    // as SafeWriteReg.
    bool SafeWriteRaw(const uint8_t* buf, size_t len) {
        esp_err_t err = ESP_FAIL;
        for (int i = 0; i < I2C_INNER_RETRIES; i++) {
            err = i2c_master_transmit(i2c_device_, buf, len, 100);
            if (err == ESP_OK) {
                return true;
            }
            if (i + 1 < I2C_INNER_RETRIES) {
                vTaskDelay(pdMS_TO_TICKS(I2C_RETRY_DELAY_MS));
            }
        }
        ESP_LOGW("Py32IoExpander", "I2C raw write (len=%u) failed after %d tries: 0x%X",
                 (unsigned)len, I2C_INNER_RETRIES, err);
        return false;
    }
};
