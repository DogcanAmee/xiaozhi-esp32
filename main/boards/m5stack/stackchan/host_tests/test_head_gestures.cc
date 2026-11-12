#ifdef NDEBUG
#error "asserts must be enabled in host tests"
#endif

#include <cassert>
#include <cstdio>

#include "head_gestures.h"

using namespace anan;

namespace {

constexpr Gesture kAll[] = {Gesture::Nod, Gesture::Shake, Gesture::QuickShake, Gesture::Tilt,
                            Gesture::Bow};

bool Same(Pose a, Pose b) { return a.yaw_deg == b.yaw_deg && a.pitch_deg == b.pitch_deg; }

void none_gesture_is_empty() {
    assert(BuildGesture(Gesture::None, {0, 5}, 1).empty());
    assert(BuildGesture(Gesture::None, {0, 5}, 3).empty());
}

void every_gesture_ends_at_base() {
    const Pose bases[] = {{0, 5}, {30, 40}};
    for (Gesture g : kAll) {
        for (Pose base : bases) {
            auto frames = BuildGesture(g, base, 2);
            assert(!frames.empty());
            assert(Same(frames.back().pose, ClampPose(base)));
        }
    }
}

void all_keyframes_within_limits() {
    const int yaws[] = {-45, 45};
    const int pitches[] = {5, 85};
    for (Gesture g : kAll) {
        for (int yaw : yaws) {
            for (int pitch : pitches) {
                for (const Keyframe& k : BuildGesture(g, {yaw, pitch}, 3)) {
                    assert(k.pose.yaw_deg >= kYawMin && k.pose.yaw_deg <= kYawMax);
                    assert(k.pose.pitch_deg >= kPitchMin && k.pose.pitch_deg <= kPitchMax);
                }
            }
        }
    }
}

void nod_repeats_by_count() {
    auto one = BuildGesture(Gesture::Nod, {0, 5}, 1);
    auto two = BuildGesture(Gesture::Nod, {0, 5}, 2);
    assert(!one.empty());
    assert(two.size() == 2 * one.size());
}

void count_is_clamped_1_to_3() {
    assert(ClampGestureCount(0) == 1);
    assert(ClampGestureCount(-5) == 1);
    assert(ClampGestureCount(1) == 1);
    assert(ClampGestureCount(2) == 2);
    assert(ClampGestureCount(3) == 3);
    assert(ClampGestureCount(99) == 3);
    assert(BuildGesture(Gesture::Shake, {0, 5}, 99).size() ==
           BuildGesture(Gesture::Shake, {0, 5}, 3).size());
    assert(BuildGesture(Gesture::Shake, {0, 5}, 0).size() ==
           BuildGesture(Gesture::Shake, {0, 5}, 1).size());
}

void tilt_and_bow_ignore_count() {
    for (Gesture g : {Gesture::Tilt, Gesture::Bow}) {
        assert(BuildGesture(g, {0, 5}, 3).size() == BuildGesture(g, {0, 5}, 1).size());
    }
}

void durations_are_nonzero() {
    for (Gesture g : kAll) {
        for (const Keyframe& k : BuildGesture(g, {0, 5}, 3)) {
            assert(k.duration_ms > 0);
        }
    }
}

void exact_tables_at_rest_base() {
    const Pose base{0, 5};
    auto nod = BuildGesture(Gesture::Nod, base, 1);
    assert(nod.size() == 2);
    assert(Same(nod[0].pose, {0, 15}) && nod[0].duration_ms == 220);
    assert(Same(nod[1].pose, base) && nod[1].duration_ms == 220);

    auto shake = BuildGesture(Gesture::Shake, base, 2);
    assert(shake.size() == 5);
    assert(Same(shake[0].pose, {-12, 5}) && shake[0].duration_ms == 200);
    assert(Same(shake[1].pose, {12, 5}) && shake[1].duration_ms == 300);
    assert(Same(shake[4].pose, base) && shake[4].duration_ms == 200);

    auto quick = BuildGesture(Gesture::QuickShake, base, 1);
    assert(quick.size() == 3);
    assert(Same(quick[0].pose, {-8, 5}) && quick[0].duration_ms == 120);
    assert(Same(quick[1].pose, {8, 5}) && quick[1].duration_ms == 160);
    assert(Same(quick[2].pose, base) && quick[2].duration_ms == 120);

    auto tilt = BuildGesture(Gesture::Tilt, base, 3);
    assert(tilt.size() == 3);
    assert(Same(tilt[0].pose, {10, 11}) && tilt[0].duration_ms == 400);
    assert(Same(tilt[1].pose, {10, 11}) && tilt[1].duration_ms == 600);
    assert(Same(tilt[2].pose, base) && tilt[2].duration_ms == 400);

    auto bow = BuildGesture(Gesture::Bow, base, 3);
    assert(bow.size() == 3);
    assert(Same(bow[0].pose, {0, 13}) && bow[0].duration_ms == 600);
    assert(Same(bow[1].pose, {0, 13}) && bow[1].duration_ms == 500);
    assert(Same(bow[2].pose, base) && bow[2].duration_ms == 600);
}

void pitch_delta_flips_near_max() {
    // base pitch 85: +10 would exceed kPitchMax, so the frame uses -10.
    auto nod = BuildGesture(Gesture::Nod, {0, 85}, 1);
    assert(Same(nod[0].pose, {0, 75}));
    // base pitch 80: +10 -> 90 > 85 flips to 70; Tilt +6 -> 86 flips to 74.
    auto tilt = BuildGesture(Gesture::Tilt, {0, 80}, 1);
    assert(Same(tilt[0].pose, {10, 74}));
    // base pitch 75: +10 -> 85 is allowed (not greater than max).
    auto nod2 = BuildGesture(Gesture::Nod, {0, 75}, 1);
    assert(Same(nod2[0].pose, {0, 85}));
    // out-of-range base is clamped first, and the end is the clamped base.
    auto shake = BuildGesture(Gesture::Shake, {99, -9}, 1);
    assert(Same(shake.back().pose, {45, 5}));
}

}  // namespace

int main() {
    none_gesture_is_empty();
    every_gesture_ends_at_base();
    all_keyframes_within_limits();
    nod_repeats_by_count();
    count_is_clamped_1_to_3();
    tilt_and_bow_ignore_count();
    durations_are_nonzero();
    exact_tables_at_rest_base();
    pitch_delta_flips_near_max();
    std::puts("test_head_gestures: all passed");
    return 0;
}
