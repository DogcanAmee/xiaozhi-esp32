#ifndef _STACKCHAN_AUDIO_CODEC_H
#define _STACKCHAN_AUDIO_CODEC_H

// P4 spike: a copy of m5stack/core-s3's CoreS3AudioCodec (shared with that board, so it is not
// edited) that captures BOTH on-board microphones for sound direction, while handing the audio
// pipeline exactly what CoreS3AudioCodec hands it.
//
// CoreS3 wiring (schematic Sch_M5_CoreS3_v1.0): ES7210 MIC1 = MEMS mic U12, MIC2 = MEMS mic U13
// (both MSM381A3729H9BPC), MIC3 = AEC_P/N, the speaker output through 150k resistors, MIC4 not
// connected. ES7210 runs in TDM (three mics selected) and the I2S RX frame has 2 slots of 16 bit:
// slot 0 = MIC1, slot 1 = MIC2.
//
// CoreS3AudioCodec (input_reference = false) opens the input as 2 channels with channel mask 0x1:
// I2S receives slot 0 only, so Read() gets mono MIC1 samples. This class opens it as 2 channels
// with mask 0x3 - same slot count, slot width, bit depth, sample rate and clocks, so slot 0 carries
// the same bits as before - reads both slots, and copies slot 0 into the caller's buffer (mono,
// same length, same format). input_channels() stays 1. The interleaved pair goes to the MicTap
// (one memcpy) for the face tracker task, which runs the DOA math.
//
// With input_reference = true it behaves exactly like CoreS3AudioCodec and taps nothing.

#include <vector>

#include "audio_codec.h"

#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>

class StackChanAudioCodec : public AudioCodec {
private:
    const audio_codec_data_if_t* data_if_ = nullptr;
    const audio_codec_ctrl_if_t* out_ctrl_if_ = nullptr;
    const audio_codec_if_t* out_codec_if_ = nullptr;
    const audio_codec_ctrl_if_t* in_ctrl_if_ = nullptr;
    const audio_codec_if_t* in_codec_if_ = nullptr;
    const audio_codec_gpio_if_t* gpio_if_ = nullptr;

    esp_codec_dev_handle_t output_dev_ = nullptr;
    esp_codec_dev_handle_t input_dev_ = nullptr;

    // Interleaved MIC1/MIC2 frames of the current Read(); grows once to the largest read.
    std::vector<int16_t> stereo_;

    void CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din);

    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

public:
    StackChanAudioCodec(void* i2c_master_handle, int input_sample_rate, int output_sample_rate,
        gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
        uint8_t aw88298_addr, uint8_t es7210_addr, bool input_reference);
    virtual ~StackChanAudioCodec();

    virtual void SetOutputVolume(int volume) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;
};

#endif // _STACKCHAN_AUDIO_CODEC_H
