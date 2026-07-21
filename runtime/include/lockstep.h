/* lockstep.h — #2 lockstep "unit-test interp" comparator.
 *
 * Per-basic-block differential check: the compiled block runs (RECORD mode,
 * its memory ops captured), then the SAME block is re-interpreted from the
 * same entry state (REPLAY mode, memory ops fed from the recorded trace so the
 * replay is side-effect-free and MMIO-safe). If the interp replay reads a
 * different address, writes a different value, or ends with different
 * registers, that block's compiled output is wrong -> the exact codegen defect.
 * If NO block ever diverges, the regression is timing/IRQ-phase, not codegen.
 *
 * Dev tooling only: the per-block hook is debug_server_cyc_observe, which is
 * PSX_NO_DEBUG_TOOLS-stripped in release builds. Window-gated (frame range) so
 * it's off (full speed) outside the region of interest.
 */
#pragma once
#include <stdint.h>
#include "cpu_state.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 0=off, 1=record (compiled block running), 2=replay (interp re-run). */
extern int g_ls_mode;
/* 1 while replaying: cycle/icache accounting no-ops so the replay never
 * perturbs real global timing state. */
extern int g_ls_replay_active;

/* Side-effect-free whole-call trace used by the overlay differential gate.
 * RECORD executes the interpreter authoritatively and records every memory
 * operation. REPLAY feeds the recorded reads to native code and verifies
 * writes without touching RAM, scratchpad, or MMIO a second time. These calls
 * are deliberately single-owner/non-nestable; a false return fails the
 * comparison closed. */
int      ls_shadow_record_begin(void);
int      ls_shadow_record_end(uint32_t *ops, int *saw_exception);
int      ls_shadow_replay_begin(void);
int      ls_shadow_replay_end(uint32_t *ops, int *mismatch_kind,
                              uint32_t *pc, uint32_t *addr,
                              uint32_t *expected, uint32_t *actual);
void     ls_shadow_abort(void);

/* Memory chokepoint hooks, called from the psx_read_ and psx_write_ funcs. */
uint32_t ls_read_hook(uint32_t addr, int size, uint32_t real_val);
void     ls_write_hook(uint32_t addr, int size, uint32_t val);

/* Per-basic-block-leader entry, called from debug_server_cyc_observe. */
void     ls_at_leader(uint32_t leader_phys, CPUState *cpu);

/* Dispatch-segment comparator: wraps one clean psx_dispatch_game_compiled()
 * invocation and replays the same callable through exec_one until it reaches
 * the same returned cpu->pc. This measures the native CPS unit, not just one
 * basic block. */
void     ls_func_enter(uint32_t entry_pc, CPUState *cpu);
void     ls_func_exit(uint32_t entry_pc, CPUState *cpu, int handled);
void     ls_func_set_window(uint32_t frame_lo, uint32_t frame_hi);
void     ls_func_set_record_only(int on);

/* Called at real IRQ exception entry. Function-scope comparisons skip segments
 * that delivered an interrupt, keeping codegen/tooling checks separate from
 * event-phase behavior. */
void     ls_note_exception_entry(void);

/* Suppress lockstep memory recording while runtime/debug observers read guest
 * RAM for diagnostics. These reads are not guest instructions and must not
 * become part of the compiled-vs-interp trace. */
void     ls_suppress_begin(void);
void     ls_suppress_end(void);

/* Arm the comparator over a guest-frame window [lo,hi]. hi==0 => disabled. */
void     ls_set_window(uint32_t frame_lo, uint32_t frame_hi);

/* Diagnostic: record blocks but skip the inline replay (to test whether the
 * inline replay, not recording, is what perturbs the run). */
void     ls_set_record_only(int on);

/* Drain the first-divergence record as JSON into buf. Returns bytes written. */
int      ls_get_diverge_json(char *buf, int buflen);
int      ls_get_func_json(char *buf, int buflen);

#ifdef __cplusplus
}
#endif
