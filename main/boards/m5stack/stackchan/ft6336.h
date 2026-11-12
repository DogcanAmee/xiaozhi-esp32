#pragma once

// FT6336 capacitive touch controller (I2C 0x38) on the CoreS3 front panel.
//
// The Ft6336 class is copied from the core-s3 board (m5stack_core_s3.cc) without behaviour
// changes, in the global namespace like the original, and logs through the TAG macro of the
// file that includes it - define TAG before including this header.
//
// The one addition is TryUpdateTouchPoint(). I2cDevice::ReadRegs wraps the transfer in
// ESP_ERROR_CHECK, so a single failed read on the shared bus would abort the firmware; the
// polling on this board must treat a failed read as "not touched" instead.

#include <esp_log.h>

#include "i2c_device.h"

class Ft6336 : public I2cDevice {
public:
    struct TouchPoint_t {
        int num = 0;
        int x = -1;
        int y = -1;
    };

    Ft6336(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        uint8_t chip_id = ReadReg(0xA3);
        ESP_LOGI(TAG, "Get chip ID: 0x%02X", chip_id);
        read_buffer_ = new uint8_t[6];
    }

    ~Ft6336() { delete[] read_buffer_; }

    void UpdateTouchPoint() {
        ReadRegs(0x02, read_buffer_, 6);
        tp_.num = read_buffer_[0] & 0x0F;
        tp_.x = ((read_buffer_[1] & 0x0F) << 8) | read_buffer_[2];
        tp_.y = ((read_buffer_[3] & 0x0F) << 8) | read_buffer_[4];
    }

    // Same read as UpdateTouchPoint, but a failed transfer is returned instead of aborting. On
    // failure the touch point is reset to "no touch" and false is returned.
    bool TryUpdateTouchPoint() {
        uint8_t reg = 0x02;
        esp_err_t err = i2c_master_transmit_receive(i2c_device_, &reg, 1, read_buffer_, 6, 100);
        if (err != ESP_OK) {
            tp_ = TouchPoint_t();
            return false;
        }
        tp_.num = read_buffer_[0] & 0x0F;
        tp_.x = ((read_buffer_[1] & 0x0F) << 8) | read_buffer_[2];
        tp_.y = ((read_buffer_[3] & 0x0F) << 8) | read_buffer_[4];
        return true;
    }

    inline const TouchPoint_t& GetTouchPoint() { return tp_; }

private:
    uint8_t* read_buffer_ = nullptr;
    TouchPoint_t tp_;
};
