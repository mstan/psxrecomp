/* psx_icache.c — R3000A instruction-cache FETCH cost model (faithful).
 *
 * FAITHFUL_TIMING_PLAN.md axis-2 (I-cache). Transcribed from the in-tree Beetle
 * oracle PS_CPU::ReadInstruction (psxrecomp/beetle-psx/mednafen/psx/cpu.cpp:534-601).
 *
 * HIT path is inlined in psx_icache.h; this file owns reset + MISS refill.
 */
#include "psx_icache.h"
#include "cpu_state.h"
#include "psx_icache.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern void psx_advance_cycles(uint32_t cycles);

int psx_icache_enabled(void) {
    static int s = -1;
    if (s < 0) {
        const char* e = getenv("PSX_ICACHE");
        s = (e == NULL || e[0] == '\0') ? 1 : (e[0] != '0');
    }
    return s;
}

/* Per-word tag+validity, mirroring Beetle ICache[idx].TV. Init to a value that can
 * never equal an aligned fetch address (aligned addrs have bits 0-1 = 0), so every
 * line starts cold (miss). */
uint32_t g_psx_icache_tv[1024];
int g_psx_icache_active = -1;

/* Whole-call shadow: a transactional view of the cache tags so an AOT/diff
 * shadow replay can evolve the cache from the recorded entry state and then
 * restore the live view (master's shadow machinery, retargeted at the global
 * tag array the interpreter fast path exports). */
static uint32_t s_icache_shadow_entry[1024];
static uint32_t s_icache_shadow_live[1024];
static int      s_icache_shadow_state = 0; /* 0 none, 1 recorded, 2 replay */

int psx_icache_shadow_record_begin(void) {
    if (s_icache_shadow_state != 0) return 0;
    memcpy(s_icache_shadow_entry, g_psx_icache_tv, sizeof g_psx_icache_tv);
    s_icache_shadow_state = 1;
    return 1;
}

int psx_icache_shadow_replay_begin(void) {
    if (s_icache_shadow_state != 1) return 0;
    memcpy(s_icache_shadow_live, g_psx_icache_tv, sizeof g_psx_icache_tv);
    memcpy(g_psx_icache_tv, s_icache_shadow_entry, sizeof g_psx_icache_tv);
    s_icache_shadow_state = 2;
    return 1;
}

void psx_icache_shadow_replay_end(void) {
    if (s_icache_shadow_state != 2) return;
    memcpy(g_psx_icache_tv, s_icache_shadow_live, sizeof g_psx_icache_tv);
    s_icache_shadow_state = 0;
}

void psx_icache_shadow_abort(void) {
    if (s_icache_shadow_state == 2)
        memcpy(g_psx_icache_tv, s_icache_shadow_live, sizeof g_psx_icache_tv);
    s_icache_shadow_state = 0;
}

void psx_icache_reset(void) {
    g_psx_icache_active = psx_icache_enabled();
    for (int i = 0; i < 1024; i++) g_psx_icache_tv[i] = 0x1u;
}

/* Charge the fetch cost for the instruction at `addr` and update the cache tags.
 * `addr` is the runtime guest virtual PC (aligned). Gated on PSX_ENABLE_BLOCK_CYCLES. */
void psx_icache_fetch_miss(CPUState* cpu, uint32_t addr) {
    { extern int g_ls_replay_active;
      /* Ordinary lockstep replay must not perturb the shared cache. The whole-
       * call shadow owns a private transactional cache view and may evolve it. */
      if (g_ls_replay_active && s_icache_shadow_state != 2) return;
    }
#ifdef PSX_ENABLE_BLOCK_CYCLES
    if (g_psx_icache_active < 0) g_psx_icache_active = psx_icache_enabled();
    if (!g_psx_icache_active) return;
    uint32_t idx = (addr & 0xFFCu) >> 2;
    if (g_psx_icache_tv[idx] == addr) return;        /* HIT: +0, no give-back clear */

    /* MISS — clear the pending load give-back (Beetle cpu.cpp:542-543). */
    cpu->read_absorb[cpu->read_absorb_which] = 0u;
    cpu->read_absorb_which = 0u;

    if (addr >= 0xA0000000u) { /* KSEG1 / uncached (BIOS ROM) */
        psx_advance_cycles(4u);
        return;
    }

    /* Cached refill (KSEG0/KUSEG). */
    uint32_t line = addr & 0xFFFFFFF0u;
    uint32_t bidx = (addr & 0xFF0u) >> 2;            /* first word index of the line */
    g_psx_icache_tv[bidx + 0] = line | 0x0u | 0x2u;
    g_psx_icache_tv[bidx + 1] = line | 0x4u | 0x2u;
    g_psx_icache_tv[bidx + 2] = line | 0x8u | 0x2u;
    g_psx_icache_tv[bidx + 3] = line | 0xCu | 0x2u;
    uint32_t cost = 3u;
    for (uint32_t i = (addr & 0xCu) >> 2; i < 4u; i++) {
        g_psx_icache_tv[bidx + i] &= ~0x2u;          /* valid: TV == that word's addr */
        cost++;
    }
    psx_advance_cycles(cost);
#else
    (void)cpu;
    (void)addr;
#endif
}

void psx_icache_fetch(CPUState* cpu, uint32_t addr) {
    psx_icache_fetch_miss(cpu, addr);
}
