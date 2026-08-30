/*
 * Pin cross-device causality at a scheduler deadline.
 *
 * Build/run: ctest -R psx_cycle_event_boundaries_test
 */

#include "psx_cycles.h"
#include "cpu_state.h"

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
    if (g_psx_cycle_fast_limit <= psx_cycle_count) {
        fprintf(stderr, "FAIL device service did not publish a safe fast limit\n");
        return 1;
    }
    psx_devices_mmio_sync();
    if (g_psx_cycle_fast_limit != 0u || psx_next_service_cycle != 0u) {
        fprintf(stderr, "FAIL MMIO boundary retained a pre-write deadline\n");
        return 1;
    }

    /* GTE/muldiv deadline helpers read the absolute guest clock. A generated
     * block may have deferred its preceding instructions; each observer must
     * publish them before setting or comparing a completion timestamp. */
    {
        CPUState cpu = {0};
        g_psx_cyc_batch = 7u;
        psx_muldiv_set(&cpu, 3u);
        if (psx_cycle_count != 12u || cpu.muldiv_ts_done != 15u) {
            fprintf(stderr, "FAIL muldiv set observed a deferred clock\n");
            return 1;
        }
        g_psx_cyc_batch = 2u;
        psx_muldiv_stall(&cpu);
        if (psx_cycle_count != 14u || cpu.muldiv_ts_done != 14u) {
            fprintf(stderr, "FAIL muldiv stall lost exact +1 ownership\n");
            return 1;
        }

        g_psx_cyc_batch = 4u;
        psx_gte_set(&cpu, 5u);
        if (psx_cycle_count != 18u || cpu.gte_ts_done != 23u) {
            fprintf(stderr, "FAIL GTE set observed a deferred clock\n");
            return 1;
        }
        g_psx_cyc_batch = 2u;
        psx_gte_read(&cpu, 9u);
        if (psx_cycle_count != 23u || cpu.ld_absorb != 3u ||
            cpu.ld_which_t != 9u) {
            fprintf(stderr, "FAIL GTE read delay-slot ownership changed\n");
            return 1;
        }
        cpu.gte_ts_done = 30u;
        g_psx_cyc_batch = 2u;
        psx_gte_stall(&cpu);
        if (psx_cycle_count != 30u) {
            fprintf(stderr, "FAIL GTE stall observed a deferred clock\n");
            return 1;
        }
    }

    fprintf(stderr, "PASS cross-device deadline preserves D-1 + 1 causality\n");
    return 0;
}
