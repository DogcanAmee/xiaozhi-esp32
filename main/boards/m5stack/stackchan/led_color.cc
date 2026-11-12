#include "led_color.h"

#include <algorithm>
#include <cmath>

namespace anan {

namespace {

constexpr Rgb kGreen{0, 200, 60};
constexpr Rgb kBlue{30, 90, 255};
constexpr Rgb kWhite{180, 180, 180};
constexpr Rgb kRed{255, 40, 30};
constexpr Rgb kYellow{255, 190, 40};
constexpr Rgb kDeepBlue{40, 60, 200};
constexpr Rgb kPink{255, 90, 150};

// Colour for a strong emotion; the default blue for anything weak or unknown.
Rgb SpeakingColor(std::string_view emotion) {
    if (emotion == "angry")
        return kRed;
    if (emotion == "happy" || emotion == "laughing")
        return kYellow;
    if (emotion == "sad" || emotion == "crying")
        return kDeepBlue;
    if (emotion == "loving")
        return kPink;
    return kBlue;
}

}  // namespace

LedCmd LedForState(Phase phase, std::string_view emotion) {
    switch (phase) {
        case Phase::Idle:
            return {LedEffect::Off, {0, 0, 0}};
        case Phase::Listening:
            return {LedEffect::Solid, kGreen};
        case Phase::Speaking:
            return {LedEffect::Breathe, SpeakingColor(emotion)};
        case Phase::Other:
            break;
    }
    return {LedEffect::Breathe, kWhite};
}

Rgb ApplyBrightness(Rgb c, int pct) {
    pct = std::clamp(pct, 0, kMaxBrightnessPct);
    auto scale = [pct](uint8_t v) { return static_cast<uint8_t>((v * pct + 50) / 100); };
    return {scale(c.r), scale(c.g), scale(c.b)};
}

uint16_t PackRgb565(Rgb c) {
    return static_cast<uint16_t>(((c.r & 0xF8) << 8) | ((c.g & 0xFC) << 3) | (c.b >> 3));
}

int BreatheLevelPct(int64_t t_ms) {
    int64_t phase = t_ms % kBreathePeriodMs;
    if (phase < 0)
        phase += kBreathePeriodMs;
    // Raised cosine: starts and ends at the minimum, peaks mid-period.
    const double kTwoPi = 6.283185307179586;
    double x = (1.0 - std::cos(kTwoPi * static_cast<double>(phase) /
                               static_cast<double>(kBreathePeriodMs))) /
               2.0;
    int level =
        kBreatheMinPct + static_cast<int>(std::lround(x * (kMaxBrightnessPct - kBreatheMinPct)));
    return std::clamp(level, kBreatheMinPct, kMaxBrightnessPct);
}

}  // namespace anan
