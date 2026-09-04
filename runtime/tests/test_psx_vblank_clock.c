#include "psx_vblank_clock.h"

#include <stdint.h>
#include <stdio.h>

#define PSX_CPU_HZ 33868800u
#define PAL_VIDEO_CLOCK_HZ 53203425u
#define PAL_TICKS_PER_FRAME (3406u * 314u)

int main(void) {
    PSXVBlankClock clock;
    const uint64_t numerator =
        (uint64_t)PSX_CPU_HZ * (uint64_t)PAL_TICKS_PER_FRAME;
    const uint32_t denominator = PAL_VIDEO_CLOCK_HZ * 2u;
    const uint64_t frames = 1000000u;
    uint64_t sum = 0u;

    if (!psx_vblank_clock_configure(&clock, numerator, denominator)) {
        fputs("FAIL PALx2 clock configuration\n", stderr);
        return 1;
    }
    for (uint64_t i = 0; i < frames; ++i) {
        if (clock.current_cycles != 340411u &&
            clock.current_cycles != 340412u) {
            fputs("FAIL PALx2 emitted an invalid period\n", stderr);
            return 1;
        }
        sum += clock.current_cycles;
        psx_vblank_clock_next(&clock);
    }
    {
        const uint64_t expected =
            frames * (numerator / denominator) +
            (frames * (numerator % denominator)) / denominator;
        const uint64_t scaled_residual =
            (frames * (numerator % denominator)) % denominator;
        if (sum != expected || scaled_residual >= denominator) {
            fprintf(stderr,
                    "FAIL PALx2 drift sum=%llu expected=%llu residual=%llu\n",
                    (unsigned long long)sum,
                    (unsigned long long)expected,
                    (unsigned long long)scaled_residual);
            return 1;
        }
    }
    {
        const PSXVBlankClock saved = clock;
        psx_vblank_clock_next(&clock);
        if (!psx_vblank_clock_restore_fraction(&clock, saved.fraction) ||
            clock.current_cycles != saved.current_cycles) {
            fputs("FAIL PALx2 fractional phase restore\n", stderr);
            return 1;
        }
    }
    {
        const uint64_t ten_frames =
            psx_vblank_clock_cycles_for_frames_ceil(&clock, 10u);
        if (psx_vblank_clock_frames_for_cycles_floor(&clock, ten_frames) < 10u ||
            psx_vblank_clock_frames_for_cycles_floor(&clock,
                ten_frames - clock.current_cycles) >= 10u) {
            fputs("FAIL PALx2 frame/native conversion\n", stderr);
            return 1;
        }
    }
    puts("PASS: PALx2 rational clock stays below one native-cycle drift");
    return 0;
}
