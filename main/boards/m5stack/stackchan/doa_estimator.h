#pragma once

// P4 spike: direction of arrival from the CoreS3's two microphones.
//
// Pure logic, no ESP-IDF dependency: compiled and tested on the host.
//
// Per window (default 1024 frames, 42.7 ms at 24 kHz) it estimates the time delay between the two
// channels with GCC-PHAT (generalised cross-correlation with phase transform), band-limited to
// speech (300..4000 Hz). PHAT is used rather than plain cross-correlation because the possible
// lag range is only a few samples: speech energy sits mostly below 1 kHz, so a plain correlation
// peak is tens of samples wide and nearly flat over that range; whitening the spectrum makes the
// peak about fs / bandwidth wide (~6 samples) and keeps room reverberation from dominating.
// The correlation is evaluated directly at 0.1-sample steps over the searched lag range (the
// range is tiny, so this is cheaper than an inverse FFT) and refined with a parabola.
//
// A window counts only when it is louder than the tracked noise floor (silence and steady noise
// give nothing) and its PHAT peak is coherent enough. The reported estimate is the median of
// the last few counted windows, with a 0..9 confidence.
//
// Sign: lag > 0 means channel A (mic 1) hears the sound later than channel B (mic 2), i.e. the
// source is nearer mic 2. angle_deg has the same sign (positive = towards mic 2).

#include <cstddef>
#include <cstdint>

namespace anan {

// Speed of sound at room temperature.
inline constexpr float kSpeedOfSoundMps = 343.0f;

struct DoaConfig {
    int sample_rate = 24000;
    int window = 1024;           // frames per channel per window; power of two
    float mic_spacing_m = 0.040f;  // see stackchan_audio_codec.h for where this estimate comes from
    float max_lag = 6.0f;        // searched lag range, +-samples (independent of the spacing guess)
    float band_lo_hz = 300.0f;
    float band_hi_hz = 4000.0f;
    // A window counts when its energy is this many times the tracked noise floor ...
    float gate_ratio = 4.0f;
    // ... and its mean square (per channel, int16 units) is at least this (absolute silence).
    float min_mean_square = 400.0f;  // ~ -38 dBFS rms 20
    // Minimum PHAT peak (0..1) for a window to count.
    float min_coherence = 0.15f;
    // PHAT peak that maps to full confidence.
    float full_coherence = 0.45f;
    int history = 5;           // windows in the median
    int min_history = 3;       // fewer counted windows: no estimate
    int max_age_windows = 15;  // counted windows older than this are dropped (~640 ms)
};

struct DoaResult {
    bool valid = false;
    float lag = 0;        // samples, see sign above
    float angle_deg = 0;  // asin(lag * c / (fs * d)), clamped to +-90
    int confidence = 0;   // 0..9
};

class DoaEstimator {
public:
    // Floats of scratch memory the caller must provide (so the caller decides where it lives).
    static size_t WorkspaceFloats(const DoaConfig& cfg) { return 4u * cfg.window; }

    // `workspace` must hold WorkspaceFloats(cfg) floats and outlive this object.
    DoaEstimator(const DoaConfig& cfg, float* workspace);
    DoaEstimator(const DoaEstimator&) = delete;
    DoaEstimator& operator=(const DoaEstimator&) = delete;

    // One window: `lr` holds cfg.window interleaved frames (A, B, A, B, ...).
    // Returns true when the window counted (loud and coherent enough).
    bool Process(const int16_t* lr);

    // Forget the history (e.g. the head moved, so old delays describe another pose). The noise
    // floor is kept.
    void Reset();

    DoaResult Current() const;

    // Diagnostics of the last processed window.
    float last_coherence() const { return last_coherence_; }
    float last_lag() const { return last_lag_; }
    bool last_gated() const { return last_gated_; }  // true: too quiet / steady
    float noise_floor() const { return floor_; }

    // Angle for a lag with this configuration (clamped).
    float AngleForLag(float lag) const;

private:
    void Fft(float* re, float* im) const;  // in place, size cfg_.window

    DoaConfig cfg_;
    float* re_;
    float* im_;
    float* cos_;   // twiddles, window/2 each
    float* sin_;
    float* hann_;  // window
    int k_lo_, k_hi_;

    struct Entry {
        float lag;
        float coherence;
        int64_t index;
    };
    static constexpr int kMaxHistory = 16;
    Entry hist_[kMaxHistory];
    int hist_count_ = 0;
    int hist_next_ = 0;
    int64_t window_index_ = 0;

    bool floor_init_ = false;
    float floor_ = 0;
    float last_coherence_ = 0;
    float last_lag_ = 0;
    bool last_gated_ = true;
};

}  // namespace anan
