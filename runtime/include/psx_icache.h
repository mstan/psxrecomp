#ifndef PSXRECOMP_PSX_ICACHE_H
#define PSXRECOMP_PSX_ICACHE_H

#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

extern uint32_t g_psx_icache_tv[1024];
extern int g_psx_icache_active;
extern int g_ls_replay_active;
void psx_icache_reset(void);
void psx_icache_fetch(CPUState *cpu, uint32_t addr);
void psx_icache_fetch_miss(CPUState *cpu, uint32_t addr);

/* Keep the interpreter's steady-state tag hit inside its translation unit.
 * Misses use the shared slow path, preserving exact cache evolution/timing. */
static inline void psx_icache_fetch_interp(CPUState *cpu, uint32_t addr) {
#ifdef PSX_ENABLE_BLOCK_CYCLES
    if (g_ls_replay_active) return;
    if (g_psx_icache_active < 0) {
        psx_icache_fetch(cpu, addr);
        return;
    }
    if (!g_psx_icache_active) return;
    uint32_t idx = (addr & 0xFFCu) >> 2;
    if (g_psx_icache_tv[idx] == addr) return;
    psx_icache_fetch_miss(cpu, addr);
#else
    (void)cpu;
    (void)addr;
#endif
}

#ifdef __cplusplus
}
#endif

#endif
