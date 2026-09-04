/*
 * Pin cross-device causality at a scheduler deadline.
 *
 * Build/run: ctest -R psx_cycle_event_boundaries_test
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
/* SPU sample-event scheduler (golden 1a973806): psx_cycles.c consults it; stub here. */
static uint32_t s_spu_next_sample = UINT32_MAX;
uint32_t psx_spu_sample_event_cycles_to_next(void) { return s_spu_next_sample; }
void psx_spu_sample_event_service(void) {}
int psx_get_in_exception(void) { return 0; }

void starvation_watchdog_check(void) {}
void starvation_ring_pc_sample(void) {}

int  psx_netplay_active(void) { return 0; }
int  psx_selfcheck_enabled(void) { return 0; }
void dirty_ram_ld_delay_discard(void) {}
void dirty_ram_irq_ambient_resync_after_restore(void) {}

int main(void) {
    /* NULL cpu: this test pins the scheduler boundary, not the CPU-state
     * rewind. psx_cycles_resync_after_restore guards that block on `if (cpu)`. */
    psx_cycles_resync_after_restore(NULL);
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

    /* Idle-skip observation boundary vs the SPU sample scheduler (mstan/psxrecomp#239
     * review): with IRQ9 unmasked, a wait loop must stop at the FIRST 768-cycle
     * sample boundary, not be skipped across several; with IRQ9 masked the
     * sample deadline is not observable and must not shorten the skip. */
    s_cd_ready = 1;                       /* no CD event ahead */
    s_spu_next_sample = 300;              /* next sample boundary in 300 cycles */
    i_mask = 0;
    if (psx_idle_cycles_to_next_observable_event() == 300) {
        fprintf(stderr, "FAIL masked SPU IRQ9 must not bound the idle skip\n");
        return 1;
    }
    i_mask = 1u << 9;                     /* IRQ_SPU unmasked */
    if (psx_idle_cycles_to_next_observable_event() != 300) {
        fprintf(stderr, "FAIL idle skip must stop at the first SPU sample boundary (300), got %u\n",
                psx_idle_cycles_to_next_observable_event());
        return 1;
    }
    s_spu_next_sample = 768 * 4;          /* several boundaries away: still the nearest event */
    if (psx_idle_cycles_to_next_observable_event() != 768 * 4) {
        fprintf(stderr, "FAIL idle skip must not cross a later SPU sample boundary either\n");
        return 1;
    }
    s_spu_next_sample = UINT32_MAX; i_mask = 0;

    fprintf(stderr, "PASS cross-device deadline preserves D-1 + 1 causality; "
                    "idle skip bounded by the SPU sample deadline\n");
    return 0;
}
