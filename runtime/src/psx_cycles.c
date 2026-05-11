/* psx_cycles.c — PSX guest CPU cycle clock. */

#include "psx_cycles.h"
#include "cdrom.h"
#include "sio.h"
#include "starvation_ring.h"
#include "timers.h"

uint64_t psx_cycle_count = 0;

/* Throttle watchdog check to once per ~64K cycles to keep hot-path cost
 * negligible (most blocks emit 5-30 cycles, so the check fires every
 * ~2K-12K blocks). */
static uint32_t s_watchdog_throttle = 0;
static uint32_t s_pc_sample_throttle = 0;

void psx_advance_cycles(uint32_t cycles) {
    if (cycles == 0) return;
    psx_cycle_count += (uint64_t)cycles;
    sio_advance(cycles);
    cdrom_advance(cycles);
    timers_advance(cycles);
    s_watchdog_throttle += cycles;
    if (s_watchdog_throttle >= 65536u) {
        s_watchdog_throttle = 0;
        starvation_watchdog_check();
    }
    /* PC sample every ~1M cycles (~30us PSX, ~333Hz) — small enough to
     * localize a busy-wait loop, sparse enough to not flood the 16K ring
     * during normal SIO traffic (~3000 samples/sec vs >10K SIO events/sec
     * during card transactions). */
    s_pc_sample_throttle += cycles;
    if (s_pc_sample_throttle >= 1048576u) {
        s_pc_sample_throttle = 0;
        starvation_ring_pc_sample();
    }
}

uint64_t psx_get_cycle_count(void) {
    return psx_cycle_count;
}
