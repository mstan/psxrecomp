#include <assert.h>
#include <stdint.h>

#define PSX_OVERLAY_DLL_BUILD 1
#include "cpu_state.h"

static uint32_t pending_cycles;
static uint32_t published_cycles;
static uint32_t read_observed_cycles;
static uint32_t write_observed_cycles;

void overlay_flush_cycles(void) {
    published_cycles += pending_cycles;
    pending_cycles = 0;
}

static uint32_t observed_read_word(uint32_t addr) {
    (void)addr;
    read_observed_cycles = published_cycles;
    return 0x11223344u;
}

static void observed_write_word(uint32_t addr, uint32_t value) {
    (void)addr;
    (void)value;
    write_observed_cycles = published_cycles;
}

int main(void) {
    CPUState cpu = {0};
    cpu.read_word = observed_read_word;
    cpu.write_word = observed_write_word;

    /* Direct store: its host callback sees the newly published timestamp. */
    pending_cycles = 7;
    psx_store_cycle_barrier();
    cpu.write_word(0x1F801810u, 0);
    assert(pending_cycles == 0);
    assert(write_observed_cycles == 7);

    /* SWL/SWR: the raw read is itself an observation boundary, so it must see
     * the publication too; the following write remains on the same timestamp. */
    pending_cycles = 5;
    psx_store_cycle_barrier();
    uint32_t value = cpu.read_word(0x1F801810u);
    cpu.write_word(0x1F801810u, value);
    assert(pending_cycles == 0);
    assert(read_observed_cycles == 12);
    assert(write_observed_cycles == 12);
    return 0;
}
