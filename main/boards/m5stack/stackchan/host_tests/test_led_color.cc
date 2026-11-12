#ifdef NDEBUG
#error "asserts must be enabled in host tests"
#endif

#include <cassert>
#include <cstdio>

#include "led_color.h"

using namespace anan;

namespace {

bool SameColor(Rgb a, Rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

void idle_is_off() {
    LedCmd c = LedForState(Phase::Idle, "");
    assert(c.effect == LedEffect::Off);
}

void listening_is_solid_green() {
    LedCmd c = LedForState(Phase::Listening, "");
    assert(c.effect == LedEffect::Solid);
    assert(SameColor(c.color, {0, 200, 60}));
}

void speaking_default_is_breathing_blue() {
    LedCmd c = LedForState(Phase::Speaking, "");
    assert(c.effect == LedEffect::Breathe);
    assert(SameColor(c.color, {30, 90, 255}));
}

void other_is_breathing_white() {
    LedCmd c = LedForState(Phase::Other, "");
    assert(c.effect == LedEffect::Breathe);
    assert(SameColor(c.color, {180, 180, 180}));
}

void speaking_strong_emotions_use_emotion_color() {
    struct Case {
        const char* emotion;
        Rgb color;
    };
    const Case cases[] = {
        {"angry", {255, 40, 30}}, {"happy", {255, 190, 40}}, {"laughing", {255, 190, 40}},
        {"sad", {40, 60, 200}},   {"crying", {40, 60, 200}}, {"loving", {255, 90, 150}},
    };
    for (const Case& k : cases) {
        LedCmd c = LedForState(Phase::Speaking, k.emotion);
        assert(c.effect == LedEffect::Breathe);
        assert(SameColor(c.color, k.color));
    }
}

void speaking_weak_or_unknown_emotion_uses_blue() {
    for (const char* e : {"thinking", "", "banana"}) {
        LedCmd c = LedForState(Phase::Speaking, e);
        assert(c.effect == LedEffect::Breathe);
        assert(SameColor(c.color, {30, 90, 255}));
    }
}

void emotion_ignored_outside_speaking() {
    LedCmd c = LedForState(Phase::Listening, "angry");
    assert(c.effect == LedEffect::Solid);
    assert(SameColor(c.color, {0, 200, 60}));
    LedCmd d = LedForState(Phase::Idle, "angry");
    assert(d.effect == LedEffect::Off);
    LedCmd o = LedForState(Phase::Other, "angry");
    assert(SameColor(o.color, {180, 180, 180}));
}

void brightness_never_exceeds_40pct() {
    Rgb c = ApplyBrightness({255, 255, 255}, 100);
    assert(c.r <= 102 && c.g <= 102 && c.b <= 102);
    assert(c.r == 102);
    Rgb z = ApplyBrightness({255, 255, 255}, -5);
    assert(z.r == 0 && z.g == 0 && z.b == 0);
    Rgb h = ApplyBrightness({200, 100, 0}, 20);
    assert(h.r == 40 && h.g == 20 && h.b == 0);
}

void breathe_stays_within_8_to_40() {
    int lo = 1000, hi = -1;
    for (int64_t t = 0; t <= 4800; t += 20) {
        int p = BreatheLevelPct(t);
        assert(p >= 8 && p <= kMaxBrightnessPct);
        if (p < lo)
            lo = p;
        if (p > hi)
            hi = p;
    }
    assert(lo == 8 && hi == kMaxBrightnessPct);  // it actually sweeps the range
    assert(BreatheLevelPct(0) == BreatheLevelPct(2400));
    int p = BreatheLevelPct(-100);  // negative time must not escape the range
    assert(p >= 8 && p <= kMaxBrightnessPct);
}

void pack_rgb565_known_values() {
    assert(PackRgb565({255, 0, 0}) == 0xF800);
    assert(PackRgb565({0, 255, 0}) == 0x07E0);
    assert(PackRgb565({0, 0, 255}) == 0x001F);
    assert(PackRgb565({0, 0, 0}) == 0x0000);
    assert(PackRgb565({255, 255, 255}) == 0xFFFF);
}

}  // namespace

int main() {
    idle_is_off();
    listening_is_solid_green();
    speaking_default_is_breathing_blue();
    other_is_breathing_white();
    speaking_strong_emotions_use_emotion_color();
    speaking_weak_or_unknown_emotion_uses_blue();
    emotion_ignored_outside_speaking();
    brightness_never_exceeds_40pct();
    breathe_stays_within_8_to_40();
    pack_rgb565_known_values();
    std::puts("test_led_color: all tests passed");
    return 0;
}
