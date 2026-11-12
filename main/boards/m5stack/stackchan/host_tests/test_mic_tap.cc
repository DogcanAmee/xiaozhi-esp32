// Host tests for MicTap (P4 spike): wrap, overrun, timing.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "mic_tap.h"

namespace anan {
MicTap& GetMicTap() {
    static MicTap tap;
    return tap;
}
}  // namespace anan

using anan::MicTap;

namespace {

std::vector<int16_t> Block(uint32_t first_frame, uint32_t n) {
    std::vector<int16_t> b(2 * n);
    for (uint32_t i = 0; i < n; ++i) {
        b[2 * i] = static_cast<int16_t>(first_frame + i);
        b[2 * i + 1] = static_cast<int16_t>(-(int)(first_frame + i));
    }
    return b;
}

void disabled_writes_nothing() {
    std::vector<int16_t> mem(2 * 64);
    MicTap tap;
    tap.Attach(mem.data(), 64, 24000);
    auto b = Block(0, 10);
    tap.Write(b.data(), 10, 1000);
    assert(tap.written() == 0);
}

void wrap_and_read() {
    std::vector<int16_t> mem(2 * 64);
    MicTap tap;
    tap.Attach(mem.data(), 64, 24000);
    tap.Enable(true);
    uint32_t f = 0;
    for (int i = 0; i < 9; ++i) {  // 9 * 10 frames: wraps
        auto b = Block(f, 10);
        tap.Write(b.data(), 10, 1000000 + i * 1000);
        f += 10;
    }
    assert(tap.written() == 90);
    int16_t out[2 * 20];
    assert(tap.Read(60, 20, out));  // crosses the wrap (60..79 -> slots 60..63, 0..15)
    for (int i = 0; i < 20; ++i) {
        assert(out[2 * i] == 60 + i && out[2 * i + 1] == -(60 + i));
    }
    // Overwritten / beyond the safe margin (64 - 16 = 48 frames back).
    assert(!tap.Read(30, 10, out));
    assert(tap.Read(42, 10, out));
    // Not written yet.
    assert(!tap.Read(85, 10, out));
    // Time of the last frame = last write's end; 24 frames back = 1 ms earlier.
    assert(tap.TimeOfMs(90) == 1008);
    assert(tap.TimeOfMs(66) == 1007);
}

}  // namespace

int main() {
    disabled_writes_nothing();
    wrap_and_read();
    std::puts("test_mic_tap: all passed");
    return 0;
}
