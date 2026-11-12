// Host tests for DoaEstimator (P4 spike) with synthetic two-microphone signals.

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "doa_estimator.h"

using anan::DoaConfig;
using anan::DoaEstimator;
using anan::DoaResult;

namespace {

constexpr double kPi = 3.14159265358979;

struct Lcg {
    uint32_t s;
    explicit Lcg(uint32_t seed) : s(seed) {}
    double Uniform() {  // 0..1
        s = s * 1664525u + 1013904223u;
        return (s >> 8) / 16777216.0;
    }
};

// A sum of sinusoids, so a fractional delay is exact: value at time t (in samples).
struct Source {
    std::vector<double> f, amp, ph;
    double Sample(double t, int fs) const {
        double v = 0;
        for (size_t i = 0; i < f.size(); ++i) {
            v += amp[i] * std::sin(2 * kPi * f[i] * t / fs + ph[i]);
        }
        return v;
    }
};

// Broadband noise-like source, 200..5000 Hz.
Source Noise(uint32_t seed) {
    Lcg r(seed);
    Source s;
    for (int i = 0; i < 120; ++i) {
        s.f.push_back(200 + 4800 * r.Uniform());
        s.amp.push_back(0.5 + r.Uniform());
        s.ph.push_back(2 * kPi * r.Uniform());
    }
    return s;
}

// Voice-like: harmonics of a 140 Hz fundamental with a falling spectrum.
Source Voice(uint32_t seed) {
    Lcg r(seed);
    Source s;
    for (int h = 1; h * 140 < 4500; ++h) {
        s.f.push_back(140.0 * h);
        s.amp.push_back(3.0 / h);
        s.ph.push_back(2 * kPi * r.Uniform());
    }
    return s;
}

// Fills `windows` windows: A = src(t - delay) (mic 1 later when delay > 0), B = src(t),
// scaled to `rms_target` int16 units, plus independent noise of `noise_rms`. `start` lets a
// stream continue across calls.
std::vector<int16_t> Make(const Source& src, double delay, int frames, double start,
                          double rms_target, double noise_rms, uint32_t noise_seed,
                          bool independent_b = false, const Source* src_b = nullptr) {
    const int fs = 24000;
    double norm = 0;
    for (double a : src.amp) {
        norm += a * a / 2;
    }
    const double scale = rms_target / std::sqrt(norm);
    Lcg r(noise_seed);
    std::vector<int16_t> out(2 * frames);
    for (int i = 0; i < frames; ++i) {
        const double t = start + i;
        double a = scale * src.Sample(t - delay, fs);
        double b = independent_b ? scale * src_b->Sample(t, fs) : scale * src.Sample(t, fs);
        // Approximately Gaussian noise (sum of uniforms).
        double na = 0, nb = 0;
        for (int k = 0; k < 4; ++k) {
            na += r.Uniform() - 0.5;
            nb += r.Uniform() - 0.5;
        }
        a += noise_rms * na * std::sqrt(3.0);
        b += noise_rms * nb * std::sqrt(3.0);
        out[2 * i] = static_cast<int16_t>(std::lround(std::fmax(-32768, std::fmin(32767, a))));
        out[2 * i + 1] =
            static_cast<int16_t>(std::lround(std::fmax(-32768, std::fmin(32767, b))));
    }
    return out;
}

struct Fixture {
    DoaConfig cfg;
    std::vector<float> ws;
    DoaEstimator est;
    Fixture() : ws(DoaEstimator::WorkspaceFloats(cfg)), est(cfg, ws.data()) {}

    // Some quiet room first so the noise floor is known, then `n` windows of `make(i)`.
    template <typename F>
    DoaResult Run(int n, F make) {
        const int w = cfg.window;
        for (int i = 0; i < 10; ++i) {
            auto q = Make(Noise(99), 0, w, i * w, 0, 20, 1000 + i);
            est.Process(q.data());
        }
        for (int i = 0; i < n; ++i) {
            auto buf = make(i);
            est.Process(buf.data());
        }
        return est.Current();
    }
};

void CheckDelay(const Source& src, double delay, double tol) {
    Fixture f;
    const int w = f.cfg.window;
    DoaResult r = f.Run(6, [&](int i) { return Make(src, delay, w, i * w, 3000, 20, 7 + i); });
    std::printf("  delay %+.2f -> valid %d lag %+.2f angle %+.1f conf %d\n", delay, r.valid, r.lag,
                r.angle_deg, r.confidence);
    assert(r.valid);
    assert(std::fabs(r.lag - delay) <= tol);
    assert(r.confidence >= 6);
    if (delay > 0.5) {
        assert(r.angle_deg > 10);
    } else if (delay < -0.5) {
        assert(r.angle_deg < -10);
    } else {
        assert(std::fabs(r.angle_deg) < 10);
    }
}

void noise_delays_left_right_centre() {
    CheckDelay(Noise(1), +2.0, 0.3);
    CheckDelay(Noise(2), -2.0, 0.3);
    CheckDelay(Noise(3), 0.0, 0.3);
    CheckDelay(Noise(4), +1.4, 0.3);   // fractional
    CheckDelay(Noise(5), -2.7, 0.3);   // fractional, near the end of a 40 mm array
}

void voice_delays_left_right_centre() {
    CheckDelay(Voice(11), +2.5, 0.4);
    CheckDelay(Voice(12), -2.5, 0.4);
    CheckDelay(Voice(13), 0.0, 0.4);
}

void silence_gives_no_estimate() {
    Fixture f;
    const int w = f.cfg.window;
    DoaResult r = f.Run(10, [&](int i) {
        return Make(Noise(1), 0, w, i * w, 0, 0, 5 + i);  // digital silence
    });
    assert(!r.valid);
    // Room noise only (same level as the floor).
    Fixture g;
    r = g.Run(10, [&](int i) { return Make(Noise(1), 0, w, i * w, 0, 20, 50 + i); });
    assert(!r.valid);
}

void steady_noise_stops_counting() {
    Fixture f;
    const int w = f.cfg.window;
    // A steady, coherent source (a fan) at a fixed delay, running for ~13 s.
    Source fan = Noise(21);
    int counted_late = 0;
    f.Run(0, [&](int) { return std::vector<int16_t>(); });
    for (int i = 0; i < 300; ++i) {
        auto buf = Make(fan, 1.0, w, i * w, 2000, 20, 300 + i);
        bool counted = f.est.Process(buf.data());
        if (i >= 180 && counted) {
            ++counted_late;
        }
    }
    std::printf("  steady noise: counted in the last 120 windows: %d\n", counted_late);
    assert(counted_late == 0);
    assert(!f.est.Current().valid);
}

void uncorrelated_noise_low_confidence() {
    Fixture f;
    const int w = f.cfg.window;
    Source a = Noise(31), b = Noise(32);
    DoaResult r = f.Run(6, [&](int i) {
        return Make(a, 0, w, i * w, 3000, 20, 400 + i, /*independent_b=*/true, &b);
    });
    std::printf("  uncorrelated -> valid %d lag %+.2f conf %d (last coherence %.2f)\n", r.valid,
                r.lag, r.confidence, f.est.last_coherence());
    assert(!r.valid || r.confidence <= 3);
}

void reset_drops_history() {
    Fixture f;
    const int w = f.cfg.window;
    DoaResult r = f.Run(5, [&](int i) { return Make(Noise(41), 2.0, w, i * w, 3000, 20, i); });
    assert(r.valid);
    f.est.Reset();
    assert(!f.est.Current().valid);
}

void angle_mapping() {
    Fixture f;
    // 40 mm at 24 kHz: full-side lag = 0.04 * 24000 / 343 = 2.80 samples.
    assert(std::fabs(f.est.AngleForLag(0)) < 1e-4);
    assert(std::fabs(f.est.AngleForLag(2.80f) - 90) < 3);
    assert(std::fabs(f.est.AngleForLag(-1.40f) + 30) < 1);
    assert(f.est.AngleForLag(10) == 90.0f);
}

}  // namespace

int main() {
    noise_delays_left_right_centre();
    voice_delays_left_right_centre();
    silence_gives_no_estimate();
    steady_noise_stops_counting();
    uncorrelated_noise_low_confidence();
    reset_drops_history();
    angle_mapping();
    std::puts("test_doa_estimator: all passed");
    return 0;
}
