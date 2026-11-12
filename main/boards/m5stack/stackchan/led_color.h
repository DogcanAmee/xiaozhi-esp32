#pragma once

// Pure colour / effect decisions for the Stack-chan base LED ring.
// No ESP-IDF dependency: compiled and tested on the host.

#include <cstdint>
#include <string_view>

#include "behavior_core.h"

namespace anan {

enum class LedEffect { Off, Solid, Breathe, FadeOut };
struct Rgb {
    uint8_t r, g, b;
};
struct LedCmd {
    LedEffect effect;
    Rgb color;
};

inline constexpr int kLedCount = 12;
inline constexpr int kMaxBrightnessPct = 40;  // hard cap for the whole ring
inline constexpr int kBreatheMinPct = 8;
inline constexpr int64_t kBreathePeriodMs = 2400;

// Phase -> effect + colour. A strong emotion only recolours Speaking; every
// other phase ignores it. Idle -> Off (LedRing fades out before going dark).
LedCmd LedForState(Phase phase, std::string_view emotion);

// Scale a colour to `pct` percent; pct is clamped to 0..kMaxBrightnessPct first.
Rgb ApplyBrightness(Rgb c, int pct);

uint16_t PackRgb565(Rgb c);

// Breathing brightness at time t: 2400 ms period, kBreatheMinPct..kMaxBrightnessPct.
int BreatheLevelPct(int64_t t_ms);

}  // namespace anan
