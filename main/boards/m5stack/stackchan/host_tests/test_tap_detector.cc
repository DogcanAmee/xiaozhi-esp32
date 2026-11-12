#ifdef NDEBUG
#error "asserts must be enabled in host tests"
#endif

#include <cassert>
#include <cstdio>

#include "tap_detector.h"

using namespace anan;

namespace {

// Samples arrive every 20 ms. A release is confirmed by the second not-touched sample, so the
// tap comes back 20 ms after the first one.

void short_touch_taps_once_on_release() {
    TapDetector d;
    assert(!d.Update(false, 0));
    assert(!d.Update(true, 100));
    assert(!d.Update(true, 120));
    assert(!d.Update(false, 200));
    assert(d.Update(false, 220));
    assert(!d.Update(false, 240));
}

void touch_held_half_a_second_is_not_a_tap() {
    TapDetector d;
    for (int64_t t = 1000; t < 1500; t += 20) {
        assert(!d.Update(true, t));
    }
    assert(!d.Update(false, 1500));  // 500 ms after the touch began
    assert(!d.Update(false, 1520));
}

void touch_just_under_the_limit_is_a_tap() {
    TapDetector d;
    for (int64_t t = 1000; t < 1480; t += 20) {
        assert(!d.Update(true, t));
    }
    assert(!d.Update(false, 1480));  // 480 ms
    assert(d.Update(false, 1500));
}

void a_long_press_does_not_block_the_next_tap() {
    TapDetector d;
    for (int64_t t = 0; t < 800; t += 20) {
        assert(!d.Update(true, t));
    }
    assert(!d.Update(false, 800));
    assert(!d.Update(false, 820));
    assert(!d.Update(true, 840));
    assert(!d.Update(false, 900));
    assert(d.Update(false, 920));
}

void two_taps_200ms_apart_give_only_the_first() {
    TapDetector d;
    assert(!d.Update(true, 1000));
    assert(!d.Update(false, 1040));
    assert(d.Update(false, 1060));
    assert(!d.Update(true, 1100));
    assert(!d.Update(false, 1240));  // 200 ms after the first release
    assert(!d.Update(false, 1260));
}

void two_taps_400ms_apart_give_both() {
    TapDetector d;
    assert(!d.Update(true, 1000));
    assert(!d.Update(false, 1040));
    assert(d.Update(false, 1060));
    assert(!d.Update(true, 1300));
    assert(!d.Update(false, 1440));  // 400 ms after the first release
    assert(d.Update(false, 1460));
}

void a_touch_still_down_gives_nothing() {
    TapDetector d;
    for (int64_t t = 0; t < 400; t += 20) {
        assert(!d.Update(true, t));
    }
}

void release_without_ever_being_down_gives_nothing() {
    TapDetector d;
    assert(!d.Update(false, 0));
    assert(!d.Update(false, 20));
    assert(!d.Update(false, 5000));
}

void a_rejected_tap_does_not_extend_the_debounce() {
    TapDetector d;
    assert(!d.Update(true, 1000));
    assert(!d.Update(false, 1040));
    assert(d.Update(false, 1060));  // accepted, release at 1040
    assert(!d.Update(true, 1100));
    assert(!d.Update(false, 1180));
    assert(!d.Update(false, 1200));  // rejected, 140 ms after the accepted release
    assert(!d.Update(true, 1300));
    assert(!d.Update(false, 1360));
    assert(d.Update(false, 1380));  // 320 ms after the accepted release: accepted
}

void a_single_missed_sample_inside_a_touch_is_ignored() {
    TapDetector d;
    for (int64_t t = 1000; t < 1200; t += 20) {
        assert(!d.Update(true, t));
    }
    assert(!d.Update(false, 1200));  // glitch
    for (int64_t t = 1220; t < 1700; t += 20) {
        assert(!d.Update(true, t));  // the touch carries on
    }
    // 700 ms in total: not a tap, even though the first half is under 500 ms.
    assert(!d.Update(false, 1700));
    assert(!d.Update(false, 1720));
    assert(!d.Update(false, 1740));
}

void a_touch_with_a_one_sample_glitch_released_early_is_one_tap() {
    TapDetector d;
    assert(!d.Update(true, 1000));
    assert(!d.Update(true, 1020));
    assert(!d.Update(false, 1040));  // glitch
    assert(!d.Update(true, 1060));
    assert(!d.Update(true, 1080));
    assert(!d.Update(false, 1100));  // real release: 100 ms
    assert(d.Update(false, 1120));
    assert(!d.Update(false, 1140));
}

}  // namespace

int main() {
    short_touch_taps_once_on_release();
    touch_held_half_a_second_is_not_a_tap();
    touch_just_under_the_limit_is_a_tap();
    a_long_press_does_not_block_the_next_tap();
    two_taps_200ms_apart_give_only_the_first();
    two_taps_400ms_apart_give_both();
    a_touch_still_down_gives_nothing();
    release_without_ever_being_down_gives_nothing();
    a_rejected_tap_does_not_extend_the_debounce();
    a_single_missed_sample_inside_a_touch_is_ignored();
    a_touch_with_a_one_sample_glitch_released_early_is_one_tap();
    std::puts("test_tap_detector: all passed");
    return 0;
}
