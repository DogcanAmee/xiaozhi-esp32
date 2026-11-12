#pragma once

// P4 spike: a single-producer / single-consumer ring of interleaved stereo int16 frames, filled
// by the audio input task (StackChanAudioCodec::Read) and drained by the face tracker task for
// the DOA estimator. Lock-free: the producer copies, then publishes its frame counter with a
// release store; the consumer copies, then re-checks the counter to detect an overrun.
//
// The producer's whole cost is one memcpy of the block (two at the wrap) plus two atomic stores,
// and nothing at all while the tap is disabled.
//
// No ESP-IDF dependency (the caller provides the buffer and the clock).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace anan {

class MicTap {
public:
    // `buffer` holds `frames` stereo frames (2 * frames int16); `frames` must be a power of two.
    // Call before Enable(true); never again.
    void Attach(int16_t* buffer, uint32_t frames, int sample_rate) {
        buf_ = buffer;
        mask_ = frames - 1;
        frames_ = frames;
        sample_rate_ = sample_rate;
        attached_.store(true, std::memory_order_release);
    }

    void Enable(bool on) { enabled_.store(on, std::memory_order_release); }

    // Producer (audio input task). `lr`: `n` interleaved frames; `now_us`: when the last one
    // arrived.
    void Write(const int16_t* lr, uint32_t n, int64_t now_us) {
        if (!enabled_.load(std::memory_order_acquire) ||
            !attached_.load(std::memory_order_acquire)) {
            return;
        }
        if (n > frames_) {
            lr += 2 * (n - frames_);
            n = frames_;
        }
        uint32_t w = written_.load(std::memory_order_relaxed);
        uint32_t pos = w & mask_;
        uint32_t first = n < frames_ - pos ? n : frames_ - pos;
        std::memcpy(buf_ + 2 * pos, lr, first * 2 * sizeof(int16_t));
        if (n > first) {
            std::memcpy(buf_, lr + 2 * first, (n - first) * 2 * sizeof(int16_t));
        }
        end_time_us_.store(now_us, std::memory_order_relaxed);
        written_.store(w + n, std::memory_order_release);
    }

    // Consumer. Frames written so far (wraps at 2^32; use unsigned differences).
    uint32_t written() const { return written_.load(std::memory_order_acquire); }

    // Copies frames [start, start + n) into `out` (2 * n int16). False if any of them has been
    // overwritten (or is not written yet).
    bool Read(uint32_t start, uint32_t n, int16_t* out) const {
        uint32_t w = written();
        if (w - start < n || w - start > safe()) {
            return false;
        }
        uint32_t pos = start & mask_;
        uint32_t first = n < frames_ - pos ? n : frames_ - pos;
        std::memcpy(out, buf_ + 2 * pos, first * 2 * sizeof(int16_t));
        if (n > first) {
            std::memcpy(out + 2 * first, buf_, (n - first) * 2 * sizeof(int16_t));
        }
        // Overrun check: the producer may have lapped `start` while we copied. A block the
        // producer is copying right now is not published yet, so keep a quarter of the ring
        // (far more than one producer block) as margin.
        uint32_t w2 = written();
        return w2 - start <= safe();
    }

    // Approximate capture time (ms) of frame `index`, from the last write's end time. Error up to
    // one producer block (~30 ms) when a write is in progress.
    int64_t TimeOfMs(uint32_t index) const {
        uint32_t w = written();
        int64_t end_us = end_time_us_.load(std::memory_order_relaxed);
        int32_t behind = static_cast<int32_t>(w - index);
        return (end_us - static_cast<int64_t>(behind) * 1000000 / sample_rate_) / 1000;
    }

    uint32_t capacity() const { return frames_; }
    // The oldest frame still safe to read is written() - safe().
    uint32_t safe() const { return frames_ - frames_ / 4; }
    bool attached() const { return attached_.load(std::memory_order_acquire); }

private:
    int16_t* buf_ = nullptr;
    uint32_t mask_ = 0;
    uint32_t frames_ = 0;
    int sample_rate_ = 24000;
    std::atomic<bool> attached_{false};
    std::atomic<bool> enabled_{false};
    std::atomic<uint32_t> written_{0};
    std::atomic<int64_t> end_time_us_{0};
};

// The one tap shared by the codec and the tracker (defined in stackchan_audio_codec.cc).
MicTap& GetMicTap();

}  // namespace anan
