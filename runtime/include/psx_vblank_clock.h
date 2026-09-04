#ifndef PSXRECOMP_PSX_VBLANK_CLOCK_H
#define PSXRECOMP_PSX_VBLANK_CLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exact rational native-cycle period generator. `fraction` is the remainder
 * after selecting `current_cycles`, which makes the five fields complete
 * deterministic save-state data once the configured ratio is fixed. */
typedef struct PSXVBlankClock {
    uint32_t period_floor;
    uint32_t period_remainder;
    uint32_t period_denominator;
    uint32_t fraction;
    uint32_t current_cycles;
} PSXVBlankClock;

int      psx_vblank_clock_configure(PSXVBlankClock *clock,
                                    uint64_t period_numerator,
                                    uint32_t period_denominator);
void     psx_vblank_clock_next(PSXVBlankClock *clock);
int      psx_vblank_clock_restore_fraction(PSXVBlankClock *clock,
                                           uint32_t fraction);
uint64_t psx_vblank_clock_cycles_for_frames_ceil(
             const PSXVBlankClock *clock, uint32_t frames);
uint64_t psx_vblank_clock_frames_for_cycles_floor(
             const PSXVBlankClock *clock, uint64_t cycles);

#ifdef __cplusplus
}
#endif

#endif
