#include <assert.h>
#include <stdint.h>

#define PSX_OVERLAY_DLL_BUILD 1
#define PSX_ENABLE_BLOCK_CYCLES 1
#include "cpu_state.h"

static uint32_t callback_cycles;

/* Overlay DLLs provide this through their callback shim.  Defining only the
 * callback-backed sink also makes the link fail if generated_base regresses
 * to importing any host cycle-batching globals. */
void psx_advance_cycles(uint32_t cycles) {
    callback_cycles += cycles;
}

int main(void) {
    CPUState cpu = {0};

    /* A pending load give-back replaces base retirement until exhausted. */
    cpu.read_absorb_which = 7u;
    cpu.read_absorb[7] = 2u;
    cpu.ld_which_t = 0x20u;
    psx_cyc_step_0(&cpu);
    assert(cpu.read_absorb[7] == 1u);
    assert(callback_cycles == 0u);
    psx_cyc_step_0(&cpu);
    assert(cpu.read_absorb[7] == 0u);
    assert(callback_cycles == 0u);

    /* Once no give-back remains, the base cycle reaches the overlay shim. */
    psx_cyc_step_0(&cpu);
    assert(callback_cycles == 1u);

    /* DO_LDS still commits the pending load after that base retirement. */
    cpu.read_absorb_which = 0u;
    cpu.ld_which_t = 3u;
    cpu.ld_absorb = 5u;
    psx_cyc_step_0(&cpu);
    assert(callback_cycles == 2u);
    assert(cpu.read_absorb[3] == 5u);
    assert(cpu.read_absorb_which == 3u);
    assert(cpu.read_fudge == 3u);
    assert(cpu.ld_which_t == 0x20u);

    /* The newly committed give-back suppresses the following base cycle. */
    psx_cyc_step_0(&cpu);
    assert(cpu.read_absorb[3] == 4u);
    assert(callback_cycles == 2u);
    return 0;
}
