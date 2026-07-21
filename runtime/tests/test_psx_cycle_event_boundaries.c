/*
 * Pin cross-device causality at a scheduler deadline.
 *
 * Build from runtime/tests:
 *   gcc -std=c99 -Wall -Wextra -ffunction-sections -fdata-sections \
 *     -Wl,--gc-sections -I../include -o test_psx_cycle_event_boundaries \
 *     test_psx_cycle_event_boundaries.c ../src/psx_cycles.c
 */

#include "psx_cycles.h"

#include <stdint.h>
#include <stdio.h>

int g_ls_replay_active = 0;
int g_ls_mode = 0;
int g_precise_mode = 0;
int g_psx_call_bail = 0;
uint32_t i_mask = 0;
uint64_t g_guest_store_count = 0;
uint64_t g_mmio_access_count = 0;

static uint32_t s_cd_cycles_remaining = 5;
static int s_cd_ready = 0;
static uint32_t s_dma_ready_cycles = 0;

void sio_advance(uint32_t cycles) { (void)cycles; }

void cdrom_advance(uint32_t cycles) {
    if (s_cd_ready) return;
    if (cycles >= s_cd_cycles_remaining) {
        s_cd_cycles_remaining = 0;
        s_cd_ready = 1;
    } else {
        s_cd_cycles_remaining -= cycles;
    }
}

void dma_advance(uint32_t cycles) {
    if (s_cd_ready) s_dma_ready_cycles += cycles;
}

void timers_advance(uint32_t cycles) { (void)cycles; }
void interrupts_advance_cycles(uint32_t cycles) { (void)cycles; }
void interrupts_service_scheduled_events(void) {}

uint32_t interrupts_cycles_to_vblank(void) { return UINT32_MAX; }
uint32_t timers_cycles_to_irq(uint32_t mask) { (void)mask; return UINT32_MAX; }
uint32_t cdrom_cycles_to_irq(uint32_t mask) {
    (void)mask;
    return s_cd_ready ? UINT32_MAX : s_cd_cycles_remaining;
}
uint32_t dma_cycles_to_internal_event(void) { return UINT32_MAX; }
uint32_t dma_cycles_to_deliverable_irq(uint32_t mask) {
    (void)mask;
    return UINT32_MAX;
}
uint32_t sio_cycles_to_irq(uint32_t mask) { (void)mask; return UINT32_MAX; }
int psx_get_in_exception(void) { return 0; }

void starvation_watchdog_check(void) {}
void starvation_ring_pc_sample(void) {}

int main(void) {
    psx_cycles_resync_after_restore();
    psx_advance_cycles(5);

    if (psx_get_cycle_count() != 5) {
        fprintf(stderr, "FAIL cycle count: expected 5 got %llu\n",
                (unsigned long long)psx_get_cycle_count());
        return 1;
    }
    if (!s_cd_ready) {
        fprintf(stderr, "FAIL CD event did not fire at cycle 5\n");
        return 1;
    }
    if (s_dma_ready_cycles != 1) {
        fprintf(stderr,
                "FAIL retroactive DMA credit: expected 1 boundary cycle got %u\n",
                s_dma_ready_cycles);
        return 1;
    }

    fprintf(stderr, "PASS cross-device deadline preserves D-1 + 1 causality\n");
    return 0;
}
