#ifndef PSXRECOMP_V4_PSX_CYCLES_H
#define PSXRECOMP_V4_PSX_CYCLES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PSX guest CPU cycle clock — the single source of truth for guest-
 * visible time. Peripherals derive all schedules from this counter.
 *
 * Phase 1.0e-a (scaffold): the counter exists and psx_advance_cycles is
 * defined, but no caller invokes psx_advance_cycles yet. Validation:
 * counter remains 0 throughout a normal session. */
extern uint64_t psx_cycle_count;

/* Advance guest time. Negative or zero is a no-op. Phase 1.0e-a body
 * just updates the counter; 1.0e-b adds peripheral *_advance() calls. */
void psx_advance_cycles(uint32_t cycles);

/* Read accessor for telemetry. */
uint64_t psx_get_cycle_count(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_V4_PSX_CYCLES_H */
