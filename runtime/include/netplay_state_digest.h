#ifndef PSX_NETPLAY_STATE_DIGEST_H
#define PSX_NETPLAY_STATE_DIGEST_H

/*
 * Compact state digests for rollback hash_confirm / FRAME_COMMIT / POST.
 *
 * Core = CPU + RAM + IRQ/timers/clock + dirty bitmap — invent hash_confirm.
 * AV = GPU regs + VRAM — baseline/POST also require this (GL/VK readback forks
 * VRAM while core still matches; pin zlib sizes were the symptom).
 * CDROM digest: live dig audit + folded into baseline dig_c with aux
 * (matched core/av/aux with divergent CD was loading doomed Replay snaps).
 * Baseline dig_c (ext) also folds scratchpad + DMA + SIO — pin zlib skew
 * with matched core/av/aux/cd was ungated bus state.
 * Master = crc(core, cd) for combined logging only.
 */

#include <stdint.h>
#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Partition CRCs folded into netplay_core_digest — for first-diverge diags. */
typedef struct NetplayCoreParts {
    uint32_t cpu;       /* gpr + pc + hi/lo + status/cause/epc + GTE */
    uint32_t clock_irq; /* cycle count + i_stat/i_mask */
    uint32_t timers;    /* timer snapshot */
    uint32_t ram;       /* 2 MiB main RAM */
    uint32_t dirty;     /* dirty-RAM bitmap words */
    uint32_t core;      /* full core (same as netplay_core_digest) */
} NetplayCoreParts;

uint32_t netplay_core_digest(const CPUState* cpu);
void     netplay_core_digest_parts(const CPUState* cpu, NetplayCoreParts* out);
uint32_t netplay_master_digest(const CPUState* cpu);
uint32_t netplay_cdrom_digest(void);
uint32_t netplay_av_digest(void); /* GPU + VRAM */
/* SPU regs+RAM and MDEC — in snaps but not core/av; pin zlib skew with matched
 * core/av was this. Baseline dig_c / live dig carry aux.
 * MDEC snap age is guest-cycle relative (not host s_frame_count).
 * PSX_RB_SPU_DIG_REGS_ONLY=1: netplay_spu_digest skips 512 KiB SPU RAM (A/B
 * whether Win↔Linux aux forks in reverb/capture RAM vs voice regs). */
uint32_t netplay_spu_regs_digest(void); /* regs/voices/tail only — no SPU RAM */
uint32_t netplay_spu_digest(void);
uint32_t netplay_mdec_digest(void);
uint32_t netplay_aux_digest(void); /* crc(spu, mdec) */
uint32_t netplay_spad_digest(void); /* 1 KiB scratchpad */
uint32_t netplay_dma_digest(void);
uint32_t netplay_sio_digest(void);

/* SIO snapshot section CRCs — which subsystem forked a resim (a single SIO
 * digest could not say). Sections mirror sio_snap_emit order.
 * netplay_sio_digest / baseline_ext fold only through fsm_pace (not meta). */
typedef struct NetplaySioParts {
    uint32_t regs; /* tx/rx/stat/mode/ctrl/baud */
    uint32_t pads; /* pad_analog/connected/state/response/config/type_req */
    uint32_t mc;   /* memcard working vars + both slot FSMs + active_device */
    uint32_t pace; /* shift/ack/irq countdown — guest-visible timing */
    uint32_t meta; /* irq source/slot/delay/byte_seq (host audit only) */
} NetplaySioParts;
void netplay_sio_digest_parts(NetplaySioParts *out);
/* Baseline dig_c: crc(aux, cd, spad, dma, sio). Refuse doomed Replay. */
uint32_t netplay_baseline_ext_digest(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_NETPLAY_STATE_DIGEST_H */
