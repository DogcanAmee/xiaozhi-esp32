#ifdef NDEBUG
#error "asserts must be enabled in host tests"
#endif

#include <cassert>
#include <cstdio>

#include "tap_detector.h"

using namespace anan;

namespace {

void short_touch_taps_once_on_release() {
    TapDetector d;
    assert(!d.Update(false, 0));
    assert(!d.Update(true, 100));
    assert(!d.Update(true, 120));
    assert(d.Update(false, 200));
    assert(!d.Update(false, 220));
}

void touch_held_half_a_second_is_not_a_tap() {
    TapDetector d;
    assert(!d.Update(true, 1000));
    assert(!d.Update(true, 1499));
    assert(!d.Update(false, 1500));
}

void touch_just_under_the_limit_is_a_tap() {
    TapDetector d;
    assert(!d.Update(true, 1000));
    assert(d.Update(false, 1499));
}

void a_long_press_does_not_block_the_next_tap() {
    TapDetector d;
    assert(!d.Update(true, 0));
    assert(!d.Update(false, 800));
    assert(!d.Update(true, 820));
    assert(d.Update(false, 900));
}

void two_taps_200ms_apart_give_only_the_first() {
    TapDetector d;
    assert(!d.Update(true, 1000));
    assert(d.Update(false, 1050));
    assert(!d.Update(true, 1100));
    assert(!d.Update(false, 1250));
}

void two_taps_400ms_apart_give_both() {
    TapDetector d;
    assert(!d.Update(true, 1000));
    assert(d.Update(false, 1050));
    assert(!d.Update(true, 1300));
    assert(d.Update(false, 1450));
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
    assert(d.Update(false, 1050));  // accepted
    assert(!d.Update(true, 1100));
    assert(!d.Update(false, 1200));  // rejected, 150 ms after the first
    assert(!d.Update(true, 1300));
    assert(d.Update(false, 1360));  // 310 ms after the accepted one: accepted
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
    std::puts("test_tap_detector: all passed");
    return 0;
}
