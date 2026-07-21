/* fntrace.h — always-on call ring for recomp psx_dispatch.
 *
 * Records every entry into psx_dispatch with the caller's argument
 * registers and return address. Mirrors beetle_libretro.cpp's fntrace
 * ring (same wire commands: fntrace_arm / fntrace_dump / fntrace_clear)
 * so cross-process tools that already speak Beetle's protocol work
 * unchanged against psx-runtime.
 *
 * Why "always-on" rather than arm-then-record: see CLAUDE.md global
 * rule "Never time/attach for observability". The ring captures every
 * dispatch from boot; arming only narrows what the dump command
 * reports, never what is recorded. To investigate a window of
 * interest, fntrace_dump it after the fact.
 *
 * Coverage: every psx_dispatch entry — both static-recompiled targets
 * (ROM functions, shell-relocated functions) and dirty-RAM dispatches.
 * dirty_ram_block_log overlaps with the dirty-RAM subset; this ring is
 * the superset.
 */
#ifndef PSXRECOMP_FNTRACE_H
#define PSXRECOMP_FNTRACE_H

#include <stdint.h>
#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 4M entries × 32 bytes = 128 MB.  At ~580K dispatches/s peak (boot)
 * that's ~7s; at ~10K/s during modal idle, ~400s.  Sized for press-
 * window retrospectives without per-second eviction. */
#define FNTRACE_RING_CAP (1u << 22)

typedef struct {
    uint32_t frame;     /* s_frame_count at dispatch */
    uint32_t target;    /* psx_dispatch addr argument (virtual) */
    uint32_t ra;        /* cpu->gpr[31] at dispatch — caller's return PC */
    uint32_t a0;        /* cpu->gpr[4]  */
    uint32_t a1;        /* cpu->gpr[5]  */
    uint32_t a2;        /* cpu->gpr[6]  */
    uint32_t a3;        /* cpu->gpr[7]  */
    uint32_t s3;        /* cpu->gpr[19] — callee-saved, useful for arg-passing chains */
    uint32_t sp;        /* cpu->gpr[29] — for func_8001A954 SP/RA-lifecycle oracle */
} FntraceEntry;

extern FntraceEntry g_fntrace_ring[FNTRACE_RING_CAP];
extern uint64_t     g_fntrace_seq;     /* monotonic; index into ring = seq % CAP */

/* ── Stack-domain transition ring (ALWAYS-ON, every build) ──────────────────
 * One entry per dispatch whose guest SP crossed a 64 KB domain since the
 * previous dispatch. Quiet in normal execution; fires at genuine stack
 * switches (green-thread/coroutine restores, longjmp, kernel ChangeThread,
 * crt0 stack init) — the provenance record for "who installed this SP".
 * Dumped via the `sp_ring` TCP command. */
#define SPDOM_RING_CAP 512u
typedef struct {
    uint64_t seq;
    uint64_t cycle;     /* guest cycle at the recording dispatch */
    uint32_t prev_sp;   /* sp domain execution came FROM */
    uint32_t new_sp;    /* sp observed at this dispatch */
    uint32_t target;    /* dispatch target (the code running on the new stack) */
    uint32_t ra;        /* caller's return PC at the dispatch */
    uint32_t tcb;       /* kernel current-TCB (PCB[0]) at the dispatch */
    uint32_t frame;     /* s_frame_count */
} SpDomainEntry;
extern SpDomainEntry g_spdom_ring[SPDOM_RING_CAP];
extern uint64_t      g_spdom_seq;

/* Always-on dispatch tail ring: last N dispatches (target/ra/sp/cycle),
 * recorded unconditionally so the final control-flow sequence before a death
 * is always reconstructable. Dumped via `disp_ring`. */
#define DISP_TAIL_CAP 128u
typedef struct {
    uint64_t cycle;
    uint32_t target;
    uint32_t ra;
    uint32_t sp;
    uint32_t pad;
} DispTailEntry;
extern DispTailEntry g_disp_tail[DISP_TAIL_CAP];
extern uint64_t      g_disp_tail_seq;

/* Hot path. Called from psx_dispatch on every entry. Inlinable. */
void fntrace_record(CPUState* cpu, uint32_t target);
/* Register the game's text range for one-shot game-start detection.
 * First dispatch into [lo, hi) calls cdrom_notify_game_started(). */
void fntrace_set_game_range(uint32_t lo, uint32_t hi);
int  fntrace_is_game_started(void);

/* Arm a target filter. arm_count == 0 means "record all" (default).
 * When arm_count > 0, only dispatches whose target matches one of the
 * armed entries are recorded. Use for noise reduction on a hot ring. */
#define FNTRACE_ARM_MAX 32
void fntrace_arm(uint32_t target);     /* add to arm list, or 0 to clear */
void fntrace_arm_clear(void);
uint32_t fntrace_arm_count(void);
uint32_t fntrace_arm_get(uint32_t i);
void fntrace_arm_from_env(const char *env_name);

/* Reset ring (clears seq, leaves storage in place). */
void fntrace_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_FNTRACE_H */
