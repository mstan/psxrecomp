#include "psx_cyc.h"

#include <assert.h>
#include <stdint.h>

uint64_t psx_cycle_count = 0;
uint64_t psx_next_service_cycle = 0;
uint32_t g_psx_cyc_batch = 0;
uint32_t g_psx_cyc_batch_limit = 0;
int g_psx_cyc_bb_defer = 0;
uint32_t *g_psx_cyc_local_acc = 0;
int psx_in_device_service = 0;
int g_event_step_conservative = 0;
int g_ls_replay_active = 0;
int g_ls_mode = 0;
volatile int g_ds_recording = 0;
uint8_t *g_psx_ram = 0;
int g_psx_load_delay = 1;

static int service_count;

void psx_devices_service_to_now(void) {
    service_count++;
    psx_next_service_cycle = psx_cycle_count + 1000u;
}

void psx_advance_cycles_slow(uint32_t cycles) {
    psx_cycle_count += cycles;
}

static void reset_clock(uint64_t deadline) {
    psx_cycle_count = 0;
    psx_next_service_cycle = deadline;
    g_psx_cyc_batch = 0;
    g_psx_cyc_batch_limit = 0;
    g_psx_cyc_bb_defer = 0;
    g_psx_cyc_local_acc = 0;
    service_count = 0;
}

int main(void) {
    reset_clock(5u);
    for (int i = 0; i < 4; i++) psx_cyc_charge(1u);
    assert(psx_cycle_count == 0u);
    assert(g_psx_cyc_batch == 4u);
    assert(service_count == 0);

    psx_cyc_charge(1u);
    assert(psx_cycle_count == 5u);
    assert(g_psx_cyc_batch == 0u);
    assert(service_count == 1);

    reset_clock(1000u);
    for (int i = 0; i < 63; i++) psx_cyc_charge(1u);
    assert(psx_cycle_count == 0u);
    assert(g_psx_cyc_batch == 63u);
    psx_cyc_charge(1u);
    assert(psx_cycle_count == 64u);
    assert(g_psx_cyc_batch == 0u);
    assert(service_count == 0);

    reset_clock(5u);
    psx_cyc_bb_defer_begin();
    for (int i = 0; i < 10; i++) psx_cyc_charge(1u);
    assert(psx_cycle_count == 0u);
    assert(g_psx_cyc_batch == 10u);
    psx_cyc_bb_defer_flush();
    assert(psx_cycle_count == 10u);
    assert(service_count == 1);
    assert(g_psx_cyc_bb_defer == 1);
    psx_cyc_bb_defer_end();
    assert(g_psx_cyc_bb_defer == 0);

    reset_clock(0u);
    psx_cyc_charge(1u);
    assert(psx_cycle_count == 1u);
    assert(service_count == 1);

    /* Emitter-level local accumulator: charges stay off the published clock
     * until local_publish / batch_flush; guest total at the barrier matches. */
    reset_clock(1000u);
    {
        uint32_t acc = 0;
        psx_cyc_local_begin(&acc);
        for (int i = 0; i < 100; i++) psx_cyc_charge(5u);
        assert(psx_cycle_count == 0u);
        assert(g_psx_cyc_batch == 0u);
        assert(acc == 500u);
        psx_cyc_bb_defer_flush();
        assert(psx_cycle_count == 500u);
        assert(acc == 0u);
        psx_cyc_local_end();
        assert(g_psx_cyc_local_acc == 0);
    }
    return 0;
}
