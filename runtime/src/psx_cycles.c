/* psx_cycles.c — PSX guest CPU cycle clock.
 *
 * Phase 1.0e-a: scaffold only. psx_advance_cycles updates the counter
 * but invokes no peripheral hooks. 1.0e-b will dispatch to
 * sio_advance(cycles); 1.0e-c will add gpu_advance for VBlank; later
 * slices add timers/CDROM. */

#include "psx_cycles.h"

uint64_t psx_cycle_count = 0;

void psx_advance_cycles(uint32_t cycles) {
    if (cycles == 0) return;
    psx_cycle_count += (uint64_t)cycles;
    /* Phase 1.0e-a: peripheral hooks empty.
     * 1.0e-b will call: sio_advance(cycles), gpu_advance(cycles), etc. */
}

uint64_t psx_get_cycle_count(void) {
    return psx_cycle_count;
}
