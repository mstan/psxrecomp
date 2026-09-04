#include "psx_vblank_clock.h"

#include <limits.h>

void psx_vblank_clock_next(PSXVBlankClock *clock) {
    uint64_t fraction;
    if (!clock || clock->period_denominator == 0u) return;
    fraction = (uint64_t)clock->fraction +
               (uint64_t)clock->period_remainder;
    clock->current_cycles = clock->period_floor;
    if (fraction >= clock->period_denominator) {
        fraction -= clock->period_denominator;
        clock->current_cycles++;
    }
    clock->fraction = (uint32_t)fraction;
}

int psx_vblank_clock_configure(PSXVBlankClock *clock,
                               uint64_t period_numerator,
                               uint32_t period_denominator) {
    uint64_t floor64;
    if (!clock || period_denominator == 0u) return 0;
    floor64 = period_numerator / period_denominator;
    if (floor64 == 0u || floor64 >= UINT32_MAX) return 0;
    clock->period_floor = (uint32_t)floor64;
    clock->period_remainder =
        (uint32_t)(period_numerator % period_denominator);
    clock->period_denominator = period_denominator;
    clock->fraction = 0u;
    psx_vblank_clock_next(clock);
    return 1;
}

int psx_vblank_clock_restore_fraction(PSXVBlankClock *clock,
                                      uint32_t fraction) {
    if (!clock || clock->period_denominator == 0u ||
        fraction >= clock->period_denominator) {
        return 0;
    }
    clock->fraction = fraction;
    clock->current_cycles = clock->period_floor;
    /* If the most recent addition wrapped, the post-add remainder lies below
     * period_remainder and the selected period was floor+1. */
    if (clock->period_remainder != 0u &&
        fraction < clock->period_remainder) {
        clock->current_cycles++;
    }
    return 1;
}

uint64_t psx_vblank_clock_cycles_for_frames_ceil(
    const PSXVBlankClock *clock, uint32_t frames) {
    uint64_t whole, fractional;
    if (!clock || clock->period_denominator == 0u) return 0u;
    whole = (uint64_t)frames * (uint64_t)clock->period_floor;
    fractional =
        ((uint64_t)frames * (uint64_t)clock->period_remainder +
         (uint64_t)clock->period_denominator - 1u) /
        (uint64_t)clock->period_denominator;
    return whole > UINT64_MAX - fractional
        ? UINT64_MAX : whole + fractional;
}

uint64_t psx_vblank_clock_frames_for_cycles_floor(
    const PSXVBlankClock *clock, uint64_t cycles) {
    uint64_t lo, hi;
    if (!clock || clock->period_floor == 0u ||
        clock->period_denominator == 0u) {
        return 0u;
    }
    lo = 0u;
    hi = cycles / (uint64_t)clock->period_floor;
    while (lo < hi) {
        const uint64_t mid = lo + (hi - lo + 1u) / 2u;
        const uint64_t whole = mid * (uint64_t)clock->period_floor;
        const uint64_t groups = mid / clock->period_denominator;
        const uint64_t tail = mid % clock->period_denominator;
        uint64_t fractional =
            groups * (uint64_t)clock->period_remainder;
        fractional += (tail * (uint64_t)clock->period_remainder) /
                      clock->period_denominator;
        if (whole <= cycles && fractional <= cycles - whole)
            lo = mid;
        else
            hi = mid - 1u;
    }
    return lo;
}
