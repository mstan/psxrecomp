/* psx_icache.h — R3000A I-cache fetch (HIT inlined for VLC/decode hot paths). */
#ifndef PSXRECOMP_PSX_ICACHE_H
#define PSXRECOMP_PSX_ICACHE_H

#include <stdint.h>

struct CPUState;

#ifdef __cplusplus
extern "C" {
#endif

/* Tag array + enable gate (psx_icache.c). */
extern uint32_t g_psx_icache_tv[1024];
extern int      g_psx_icache_on; /* -1 unread, 0/1 after resolve */
extern int      g_ls_replay_active;

int  psx_icache_enabled(void);
void psx_icache_reset(void);
void psx_icache_fetch_miss(struct CPUState* cpu, uint32_t addr);

/* Out-of-line entry for function pointers (overlay CPS callbacks). */
void psx_icache_fetch_fn(struct CPUState* cpu, uint32_t addr);

/* HIT is the MotK VLC steady state (+0). Miss falls to fetch_miss. */
static inline void psx_icache_fetch(struct CPUState* cpu, uint32_t addr) {
#ifdef PSX_ENABLE_BLOCK_CYCLES
    if (g_ls_replay_active) return;
    if (g_psx_icache_on < 0) g_psx_icache_on = psx_icache_enabled() ? 1 : 0;
    if (!g_psx_icache_on) return;
    uint32_t idx = (addr & 0xFFCu) >> 2;
    if (g_psx_icache_tv[idx] == addr) return; /* HIT */
    psx_icache_fetch_miss(cpu, addr);
#else
    (void)cpu;
    (void)addr;
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_PSX_ICACHE_H */
