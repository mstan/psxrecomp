/* psx_icache.c — R3000A instruction-cache FETCH cost model (faithful).
 *
 * FAITHFUL_TIMING_PLAN.md axis-2 (I-cache). Transcribed from the in-tree Beetle
 * oracle PS_CPU::ReadInstruction (psxrecomp/beetle-psx/mednafen/psx/cpu.cpp:534-601).
 *
 * HIT path is inlined in psx_icache.h; this file owns reset + MISS refill.
 */
#include "psx_icache.h"
#include "cpu_state.h"
#include <stdint.h>
#include <stdlib.h>

extern void psx_advance_cycles(uint32_t cycles);

int psx_icache_enabled(void) {
    static int s = -1;
    if (s < 0) {
        const char* e = getenv("PSX_ICACHE");
        s = (e == NULL || e[0] == '\0') ? 1 : (e[0] != '0');
    }
    return s;
}

uint32_t g_psx_icache_tv[1024];
int      g_psx_icache_on = -1;

void psx_icache_reset(void) {
    for (int i = 0; i < 1024; i++) g_psx_icache_tv[i] = 0x1u; /* bit0 => never matches */
}

void psx_icache_fetch_miss(CPUState* cpu, uint32_t addr) {
#ifdef PSX_ENABLE_BLOCK_CYCLES
    /* MISS — clear the pending load give-back (Beetle cpu.cpp:542-543). */
    cpu->read_absorb[cpu->read_absorb_which] = 0u;
    cpu->read_absorb_which = 0u;

    if (addr >= 0xA0000000u) { /* KSEG1 / uncached (BIOS ROM) */
        psx_advance_cycles(4u);
        return;
    }

    /* Cached refill (KSEG0/KUSEG). */
    uint32_t line = addr & 0xFFFFFFF0u;
    uint32_t bidx = (addr & 0xFF0u) >> 2;
    g_psx_icache_tv[bidx + 0] = line | 0x0u | 0x2u;
    g_psx_icache_tv[bidx + 1] = line | 0x4u | 0x2u;
    g_psx_icache_tv[bidx + 2] = line | 0x8u | 0x2u;
    g_psx_icache_tv[bidx + 3] = line | 0xCu | 0x2u;
    uint32_t cost = 3u;
    for (uint32_t i = (addr & 0xCu) >> 2; i < 4u; i++) {
        g_psx_icache_tv[bidx + i] &= ~0x2u;
        cost++;
    }
    psx_advance_cycles(cost);
#else
    (void)cpu;
    (void)addr;
#endif
}

void psx_icache_fetch_fn(CPUState* cpu, uint32_t addr) {
    psx_icache_fetch(cpu, addr);
}
