#include "doa_estimator.h"

#include <algorithm>
#include <cmath>

namespace anan {

namespace {
constexpr float kPi = 3.14159265358979f;
constexpr float kLagStep = 0.1f;  // grid for the direct correlation evaluation
}  // namespace

DoaEstimator::DoaEstimator(const DoaConfig& cfg, float* workspace) : cfg_(cfg) {
    const int n = cfg_.window;
    re_ = workspace;
    im_ = re_ + n;
    cos_ = im_ + n;
    sin_ = cos_ + n / 2;
    hann_ = sin_ + n / 2;
    for (int i = 0; i < n / 2; ++i) {
        cos_[i] = std::cos(2 * kPi * i / n);
        sin_[i] = -std::sin(2 * kPi * i / n);  // forward transform: e^{-i...}
    }
    for (int i = 0; i < n; ++i) {
        hann_[i] = 0.5f - 0.5f * std::cos(2 * kPi * i / n);
    }
    const float bin_hz = static_cast<float>(cfg_.sample_rate) / n;
    k_lo_ = std::max(1, static_cast<int>(std::ceil(cfg_.band_lo_hz / bin_hz)));
    k_hi_ = std::min(n / 2 - 1, static_cast<int>(std::floor(cfg_.band_hi_hz / bin_hz)));
    cfg_.history = std::clamp(cfg_.history, 1, kMaxHistory);
}

void DoaEstimator::Reset() {
    hist_count_ = 0;
    hist_next_ = 0;
}

float DoaEstimator::AngleForLag(float lag) const {
    float s = lag * kSpeedOfSoundMps / (cfg_.sample_rate * cfg_.mic_spacing_m);
    s = std::clamp(s, -1.0f, 1.0f);
    return std::asin(s) * 180.0f / kPi;
}

void DoaEstimator::Fft(float* re, float* im) const {
    const int n = cfg_.window;
    // Bit-reversal permutation.
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len / 2;
        const int stride = n / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; ++k) {
                const float wr = cos_[k * stride];
                const float wi = sin_[k * stride];
                const int a = i + k;
                const int b = a + half;
                const float xr = re[b] * wr - im[b] * wi;
                const float xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
            }
        }
    }
}

bool DoaEstimator::Process(const int16_t* lr) {
    const int n = cfg_.window;
    const int64_t index = window_index_++;

    double sa = 0, sb = 0;
    for (int i = 0; i < n; ++i) {
        sa += lr[2 * i];
        sb += lr[2 * i + 1];
    }
    const float ma = static_cast<float>(sa / n);
    const float mb = static_cast<float>(sb / n);
    double e = 0;
    for (int i = 0; i < n; ++i) {
        const float a = lr[2 * i] - ma;
        const float b = lr[2 * i + 1] - mb;
        e += a * a + b * b;
        re_[i] = a * hann_[i];
        im_[i] = b * hann_[i];
    }
    const float energy = static_cast<float>(e / (2.0 * n));

    // Noise floor: falls at once to any quieter window, creeps up 6 % per window (x4 in ~1 s), so steady
    // noise stops counting within ~1 s once the floor is near it (from deep silence to a loud fan:
    // ~6 s). Speech keeps counting because the gaps between words pull the floor back down.
    bool loud;
    if (!floor_init_) {
        floor_init_ = true;
        floor_ = std::max(energy, 1.0f);
        loud = false;
    } else {
        loud = energy >= cfg_.gate_ratio * floor_ && energy >= cfg_.min_mean_square;
        floor_ = energy < floor_ ? energy : std::min(energy, floor_ * 1.06f);
        floor_ = std::max(floor_, 1.0f);
    }
    last_gated_ = !loud;
    if (!loud) {
        return false;
    }

    Fft(re_, im_);

    // Split the two real spectra out of the packed one and keep the PHAT-weighted cross
    // spectrum G = A conj(B) / |A conj(B)| for the band bins, in place (bins k_lo_..k_hi_ < n/2,
    // their mirrors n-k are read before anything in the upper half is written - nothing is).
    int bins = 0;
    for (int k = k_lo_; k <= k_hi_; ++k) {
        const float zr = re_[k], zi = im_[k];
        const float wr = re_[n - k], wi = im_[n - k];
        const float ar = 0.5f * (zr + wr), ai = 0.5f * (zi - wi);
        const float br = 0.5f * (zi + wi), bi = -0.5f * (zr - wr);
        float gr = ar * br + ai * bi;  // A * conj(B)
        float gi = ai * br - ar * bi;
        const float mag = std::sqrt(gr * gr + gi * gi);
        if (mag > 1e-9f) {
            gr /= mag;
            gi /= mag;
            ++bins;
        } else {
            gr = gi = 0;
        }
        re_[k] = gr;
        im_[k] = gi;
    }
    if (bins == 0) {
        last_coherence_ = 0;
        return false;
    }

    // r(tau) = sum_k Re(G[k] e^{+i 2 pi k tau / n}) / bins, peaks at tau = delay of A after B.
    const int steps = static_cast<int>(std::lround(cfg_.max_lag / kLagStep));
    float best = -2, prev_val = 0, best_prev = 0, best_next = 0;
    int best_i = 0;
    bool want_next = false;
    for (int i = -steps; i <= steps; ++i) {
        const float tau = i * kLagStep;
        const float phi = 2 * kPi * tau / n;
        const float sr = std::cos(phi), si = std::sin(phi);
        float cr = std::cos(phi * k_lo_), ci = std::sin(phi * k_lo_);
        float acc = 0;
        for (int k = k_lo_; k <= k_hi_; ++k) {
            acc += re_[k] * cr - im_[k] * ci;
            const float t = cr * sr - ci * si;
            ci = cr * si + ci * sr;
            cr = t;
        }
        const float val = acc / bins;
        if (want_next) {
            best_next = val;
            want_next = false;
        }
        if (val > best) {
            best = val;
            best_i = i;
            best_prev = (i == -steps) ? val : prev_val;
            want_next = true;
        }
        prev_val = val;
    }
    if (want_next) {
        best_next = best;  // the peak was the last grid point
    }
    float lag = best_i * kLagStep;
    const float denom = best_prev - 2 * best + best_next;
    if (denom < 0) {
        lag += 0.5f * (best_prev - best_next) / denom * kLagStep;
    }
    last_coherence_ = best;
    last_lag_ = lag;

    // A peak on the edge of the searched range is outside it: no physical delay.
    if (best < cfg_.min_coherence || std::fabs(best_i) >= steps - 1) {
        return false;
    }
    hist_[hist_next_] = Entry{lag, best, index};
    hist_next_ = (hist_next_ + 1) % kMaxHistory;
    hist_count_ = std::min(hist_count_ + 1, kMaxHistory);
    return true;
}

DoaResult DoaEstimator::Current() const {
    DoaResult r;
    float lags[kMaxHistory], cohs[kMaxHistory];
    int m = 0;
    // Newest first, at most cfg_.history, none older than max_age_windows.
    for (int j = 0; j < hist_count_ && m < cfg_.history; ++j) {
        const Entry& e = hist_[(hist_next_ - 1 - j + kMaxHistory) % kMaxHistory];
        if (window_index_ - 1 - e.index > cfg_.max_age_windows) {
            break;
        }
        lags[m] = e.lag;
        cohs[m] = e.coherence;
        ++m;
    }
    if (m < cfg_.min_history) {
        return r;
    }
    float sorted[kMaxHistory];
    std::copy(lags, lags + m, sorted);
    std::sort(sorted, sorted + m);
    const float med = (m % 2) ? sorted[m / 2] : 0.5f * (sorted[m / 2 - 1] + sorted[m / 2]);
    std::copy(cohs, cohs + m, sorted);
    std::sort(sorted, sorted + m);
    const float med_coh = (m % 2) ? sorted[m / 2] : 0.5f * (sorted[m / 2 - 1] + sorted[m / 2]);
    int agree = 0;
    for (int i = 0; i < m; ++i) {
        if (std::fabs(lags[i] - med) <= 0.75f) {
            ++agree;
        }
    }
    const float q = std::min(1.0f, med_coh / cfg_.full_coherence) * agree / m;
    r.valid = true;
    r.lag = med;
    r.angle_deg = AngleForLag(med);
    r.confidence = std::clamp(static_cast<int>(std::lround(9 * q)), 0, 9);
    return r;
}

}  // namespace anan
