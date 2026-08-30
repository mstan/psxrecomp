/* dirty_ram_interp.c — small MIPS interpreter for install-at-runtime RAM.
 *
 * See CLAUDE.md Rule 18, docs/dynamic_handler_install.md, and the inline
 * note in memory.c (search for "Option B") for the architectural rationale.
 *
 * Scope: only fires when psx_dispatch lands on a PC whose page has been
 * written-to since boot.  Runs one basic block (terminator: jr/jalr/j/jal
 * or branch) and returns; dispatch trampoline re-enters for the next block.
 *
 * Strict policy: any opcode not implemented here aborts fatally.  This
 * surfaces unknown install patterns immediately so we expand the support
 * set deliberately, never silently.
 *
 * Future option (Option B, see docs/dynamic_handler_install.md): JIT-compile
 * dirty pages via the existing StrictTranslator instead of interpreting.
 * Pros: single source of MIPS semantics shared with the build-time path,
 * native-speed install stubs, generalizes to game JIT cases.  Cons: gcc-at-
 * runtime build dep, ~200 ms compile latency stall on first dispatch, file
 * I/O on hot path, cache-invalidation complexity, Windows MinGW + dlopen
 * friction.  Today install stubs are cold-path glue (~4k instructions per
 * directory-load); interpretation is sub-microsecond and the right fit.
 * Revisit if measurement shows install-stub instructions becoming a
 * meaningful fraction of total runtime work.
 */

#include "dirty_ram_interp.h"
#include "cpu_state.h"
#include "debug_server.h"
#include "interrupts.h"
#include "psx_cycles.h"
#include "psx_icache.h"
#include "psx_instr_cost.h"  /* psx_instr_base_cycles — single-source cycle cost */
#include "gpu.h"   /* psx_ws_is_backdrop_site / psx_ws_backdrop_x (interp hook) */
#include "ws_backdrop_detect.h"  /* shared backdrop-window detector (auto_backdrop) */
#include "lockstep.h"
#include "starvation_ring.h"
#include "fntrace.h"  /* fntrace_is_game_started / fntrace_mark_game_started */
#include "psx_ram.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

uint64_t g_dirty_ram_blocks_run = 0;
uint64_t g_dirty_ram_insns_run  = 0;
uint64_t g_dirty_window_dispatches = 0;  /* capture-window interp dispatches */
uint64_t g_dirty_ram_aborts     = 0;
uint64_t g_dirty_ram_guard_yields = 0;
uint64_t g_dirty_ram_native_handoffs = 0;
/* Scheduling-contract pump telemetry (see dirty_ram_dispatch). Always-on:
 * g_dirty_pump_max_gap_insns is the largest interpreted-insn gap ever seen
 * between two interrupt pumps — must stay bounded (~4096 + one block) once the
 * region-independent pump is in place; a runaway value means a dirty path is
 * advancing cycles without surfacing to psx_check_interrupts (the softlock). */
uint64_t g_dirty_pump_max_gap_insns = 0;
uint64_t g_dirty_pump_count         = 0;
static uint64_t s_last_dirty_irq_pump_insns = 0;
/* Host-only stride for the no-pending IRQ entry throttle. NOT in the snap —
 * peers that drifted apart through FMV entered the post-FMV dirty wait with
 * different phases and forked tip+1 (Win↔Linux ±1 cyc / swapped cores). Reset
 * on restore; when an IRQ is already deliverable, poll every entry instead. */
static uint32_t s_interp_entry_poll = 0;

/* EPC de-overload signal (Tomba 2 frame-1997 fix). Set to the committed guest PC
 * immediately around a dirty-pump psx_check_interrupts() call, 0 otherwise. When
 * non-zero at exception entry, the interrupt is delivered at a dirty-interp safe point
 * with a precise guest resume PC, so psx_check_interrupts sets COP0_EPC to the REAL PC
 * (architectural RFE resume) instead of the host sentinel 0x80000048. The TCB then
 * saves the real PC, so a game-installed handler that drives ReturnFromException
 * itself (sync OR async) resumes correctly. Compiled-code block-boundary pumps leave
 * this 0 -> the sentinel + host-GPR-restore + longjmp path (unchanged: T1/MMX6/Ape). */
uint32_t g_dirty_safe_resume_pc = 0;

/* Cycle-budgeted precise event slicing (PRECISE_IRQ_SLICE.md). Set while the
 * block-leader slice guard is running a block through the per-instruction
 * interpreter so the interrupt is taken at the EXACT architectural instruction
 * (not the coarse block edge). While set, exec_one's jal/jalr short-circuit to a
 * plain transfer (cpu->pc = target; return 1) so precise-mode steps INTO callees
 * per-instruction instead of running them compiled — the "ignore native
 * availability" invariant (ChatGPT-validated option b). Default 0 => every other
 * path is byte-for-byte unchanged. */
int g_precise_mode = 0;
/* #2 lockstep: set ONLY around the dirty-interp loop's per-instruction
 * cyc_observe so the lockstep comparator can tell a real COMPILED block leader
 * (g_ls_dirty_observe==0) from an interpreted per-instruction sample (==1). */
int g_ls_dirty_observe = 0;
extern int g_ls_replay_active;     /* defined in the lockstep section; used by exec_one's jal/jalr guard */

/* Interpreter-only production fast path.  Do not put this in psx_cyc.h:
 * generated overlay DLLs deliberately batch psx_advance_cycles() through a
 * DLL-local accumulator and must never bind directly to runtime cycle state. */
#if defined(PSX_NO_DEBUG_TOOLS) && !defined(PSX_COSIM) && !STARVATION_RING_ENABLED
static inline void interp_cyc_step(CPUState *cpu, uint32_t reg_mask) {
    uint8_t w = cpu->read_absorb_which;
    if (cpu->read_absorb[w]) {
        cpu->read_absorb[w]--;
    } else if (!psx_cycles_try_fast_charge(1u)) {
        psx_advance_cycles(1u);
    }
    psx_cyc_deps(cpu, reg_mask);
    psx_cyc_lds(cpu);
}
#else
static inline void interp_cyc_step(CPUState *cpu, uint32_t reg_mask) {
    psx_cyc_step(cpu, reg_mask);
}
#endif

/* ===== Minimal exception delivery from the interpreter path =====
 * Sets COP0 registers (BadVAddr, Cause, EPC, Status) and redirects PC to the
 * hardware exception vector.  Used for alignment errors caught by the
 * interpreter.  Returns 1 (control transferred). */
static int interp_exception(CPUState *cpu, uint32_t exc_code,
                            uint32_t badvaddr, uint32_t epc_pc) {
    uint32_t sr = cpu->cop0[12];
    /* BadVAddr */
    cpu->cop0[8] = badvaddr;
    /* Cause: ExcCode, clear BD (not tracking delay-slot exception here) */
    cpu->cop0[13] = (cpu->cop0[13] & ~0x8000007Cu) | (exc_code << 2);
    /* Push SR exception stack: shift bits [5:0] left by 2 */
    cpu->cop0[12] = (sr & ~0x3Fu) | ((sr & 0x0Fu) << 2);
    /* EPC */
    cpu->cop0[14] = epc_pc;
    /* Vector: BEV selects between KSEG1 (BIOS ROM) and KSEG0 (RAM) */
    cpu->pc = (sr & 0x00400000u) ? 0xBFC00180u : 0x80000080u;
    return 1;
}

#ifdef PSX_COSIM
static int g_cosim_exec_one_hooked = 0;
static void cosim_exec_one_begin(void) { g_cosim_exec_one_hooked = 0; }
static int cosim_exec_one_did_hook(void) { return g_cosim_exec_one_hooked; }
static void cosim_exec_one_transfer_hook(uint32_t pc) {
    extern void cosim_instr(uint32_t);
    cosim_instr(pc);
    g_cosim_exec_one_hooked = 1;
}
#else
static void cosim_exec_one_begin(void) {}
static int cosim_exec_one_did_hook(void) { return 0; }
static void cosim_exec_one_transfer_hook(uint32_t pc) { (void)pc; }
#endif
uint64_t g_slice_fired = 0;        /* diagnostic: slices actually run */
uint64_t g_slice_irq_taken = 0;    /* diagnostic: IRQs taken inside precise-mode */
/* First-divergence trace for the precise slice (PRECISE_IRQ_SLICE.md Task #4). */
uint32_t g_slice_last_block     = 0;  /* block_addr the guard fired on            */
uint32_t g_slice_last_first_pc  = 0;  /* pc of the first interp instruction       */
uint32_t g_slice_last_first_insn= 0;  /* raw word of that first instruction       */
uint32_t g_slice_last_committed = 0;  /* committed PC handed back as resume/EPC   */
uint32_t g_slice_last_istat     = 0;  /* i_stat at the take                        */
uint32_t g_slice_last_imask     = 0;  /* i_mask at the take                        */
uint32_t g_slice_last_sr        = 0;  /* COP0 SR at the take                       */
uint32_t g_slice_entry_deliverable = 0; /* was IRQ deliverable at slice entry?    */
uint32_t g_slice_exit_pc        = 0;  /* PC published by the last precise slice    */
uint32_t g_slice_exit_reason    = 0;  /* 1=safe, 2=unsupported, 3=bail, 4=guard    */
uint32_t g_slice_exit_iter      = 0;  /* loop iteration at exit                    */
uint32_t g_slice_exit_dispatchable = 0; /* precise_pc_dispatchable(exit_pc)        */
uint32_t g_slice_exit_dirty     = 0;  /* dirty_ram_is_dirty(exit_pc phys)          */
uint32_t g_slice_exit_in_text   = 0;  /* psx_game_address_in_text(exit_pc)         */
uint32_t g_slice_exit_want      = 0;  /* want_exit value at exit                   */

/* Persistent async-RFE resume PC (Tomba 2 frame-1997 fix). psx_check_interrupts latches
 * this from g_dirty_safe_resume_pc at each dirty-safe-point exception entry, so it holds
 * the real guest PC of the most recent dirty-interp interruption. When a game-installed
 * handler drives ReturnFromException ASYNCHRONOUSLY (sentinel RFE with in_exception==0),
 * the sentinel gates resume the guest here instead of resolving to pc=0 (abnormal exit).
 * Unlike g_dirty_safe_resume_pc (transient, scoped to the pump call) this persists across
 * the gap between the interrupt and the game's later ReturnFromException. */
uint32_t g_async_rfe_resume_pc = 0;
/* Diagnostics for the async-RFE fix: times the resume PC was latched at a dirty-safe
 * exception entry, and times the sentinel gates actually redirected to it. */
uint64_t g_async_rfe_set_count  = 0;
uint64_t g_async_rfe_fire_count = 0;
/* Reach diagnostics: which gate the sentinel dispatch lands in, and what
 * g_async_rfe_resume_pc held there. */
uint64_t g_sentinel_reach_dirty = 0;
uint64_t g_sentinel_reach_traps = 0;
uint32_t g_sentinel_reach_async = 0;

/* One-shot pc=0 producer tripwire (MMX6 boot-wedge investigation, Task #4 family).
 * Latches the FIRST time dirty_ram_dispatch returns "handled" (r==1) but leaves
 * cpu->pc == 0 — i.e. the dirty path produced the abnormal-exit null PC rather
 * than psx_unknown_dispatch (which has its own fail-fast). Captures the dispatch
 * addr + interrupt-return context so the exact producing path is identifiable
 * without printf. Surfaced in freeze_check (pczero_*). */
int      g_cosim_dirty_pump_site = 0; /* PSX_COSIM diagnostic: current dirty IRQ pump site */
int      g_pczero_latched     = 0;
uint32_t g_pczero_addr        = 0;
uint32_t g_pczero_ra          = 0;
uint32_t g_pczero_in_exc      = 0;
uint32_t g_pczero_async_rfe   = 0;
uint32_t g_pczero_dirty_safe  = 0;
uint64_t g_pczero_count       = 0;
/* Which dirty_ram_dispatch_inner sub-path produced the last handled dispatch:
 * 1=sentinel 2=AOT-compiled 3=static-overlay 4=DLL-shard 5=interp. Read by the
 * pc0 tripwire so a null-PC return names its producer. */
uint32_t g_dd_last_route      = 0;

/* Mid-block unsupported-opcode counters. Bumped instead of fprintf-spamming
 * stderr (CLAUDE.md §3). Read via dirty_ram_get_unsupported(). The "last_*"
 * fields capture the most recent occurrence so a TCP query can see what
 * opcode is missing without needing log scraping. */
uint64_t g_dirty_ram_unsupported_midblock = 0;
uint32_t g_dirty_ram_last_unsupported_entry = 0;
uint32_t g_dirty_ram_last_unsupported_entry_ra = 0;
uint32_t g_dirty_ram_last_unsupported_entry_sp = 0;
uint32_t g_dirty_ram_last_unsupported_insns = 0;
uint32_t g_dirty_ram_last_unsupported_pc  = 0;
uint32_t g_dirty_ram_last_unsupported_insn = 0;
const char *g_dirty_ram_last_unsupported_reason = NULL;

DirtyRamPcEntry g_dirty_ram_pc_table[DIRTY_RAM_PC_TABLE_SIZE] = {0};
uint32_t g_dirty_ram_exec_pc_bitmap[DIRTY_RAM_EXEC_BITMAP_WORDS] = {0};
uint32_t g_dirty_ram_exec_page_bitmap[DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS] = {0};
uint32_t g_dirty_ram_dispatch_pc_bitmap[DIRTY_RAM_EXEC_BITMAP_WORDS] = {0};

DirtyRamBlockLogEntry g_dirty_ram_block_log[DIRTY_RAM_BLOCK_LOG_CAP] = {0};
uint64_t              g_dirty_ram_block_log_seq = 0;
DirtyRamFlowLogEntry  g_dirty_ram_flow_log[DIRTY_RAM_FLOW_LOG_CAP] = {0};
uint64_t              g_dirty_ram_flow_log_seq = 0;
DirtyRamInsnLogEntry  g_dirty_ram_insn_log[DIRTY_RAM_INSN_LOG_CAP] = {0};
uint64_t              g_dirty_ram_insn_log_seq = 0;

/* Current frame counter, defined in debug_server.c. */
extern uint64_t s_frame_count;

/* Linear-probed insert/lookup keyed on entry PC.  Probe length is HARD
 * BOUNDED: a saturated table must degrade to "stop tracking" (NULL) at O(1)
 * cost, never to an O(table) scan per lookup.  Tomba2's Trolley attract demo
 * overflowed the old unbounded probe (working set > table size) and burned
 * ~99% of the host emu thread rescanning 64K slots per executed instruction
 * (0.3 fps).  128 probes keeps misses vanishingly rare below ~85% fill with
 * this hash; past that the tracking data was already degrading anyway. */
static DirtyRamPcEntry *pc_table_get_or_insert_in(DirtyRamPcEntry *table, uint32_t pc) {
    uint32_t h = (pc * 2654435761u) & (DIRTY_RAM_PC_TABLE_SIZE - 1);
    for (uint32_t i = 0; i < 128u; i++) {
        uint32_t idx = (h + i) & (DIRTY_RAM_PC_TABLE_SIZE - 1);
        DirtyRamPcEntry *e = &table[idx];
        if (e->pc == pc) return e;
        if (e->pc == 0) { e->pc = pc; return e; }
    }
    return NULL;
}

static DirtyRamPcEntry *pc_table_get_or_insert(uint32_t pc) {
    return pc_table_get_or_insert_in(g_dirty_ram_pc_table, pc);
}

/* Record every PC the interpreter executes (not just block entries) so
 * overlay_capture can report execution-verified seeds for the region. A PSX
 * instruction is aligned, making this a single direct bitmap OR rather than a
 * cache-unfriendly open-addressed lookup on every guest instruction. */
static inline void exec_pc_table_record(uint32_t pc) {
    uint32_t phys = pc & 0x1FFFFFFFu;
    /* Full 8 MiB capacity, matching DIRTY_RAM_EXEC_WORD_COUNT: high-bank
     * enhancement code (8 MB mod) must leave coverage evidence too. */
    if (phys < 8u * 1024u * 1024u && (phys & 3u) == 0u) {
        uint32_t word = phys >> 2;
        uint32_t mask = 1u << (word & 31u);
        uint32_t *slot = &g_dirty_ram_exec_pc_bitmap[word >> 5];
        if ((*slot & mask) == 0u) {
            *slot |= mask;
            uint32_t page = phys >> 12;
            g_dirty_ram_exec_page_bitmap[page >> 5] |= 1u << (page & 31u);
        }
    }
}

/* From debug_server.c — keep our outer-frame attribution coherent. */
extern uint32_t g_debug_current_func_addr;
extern uint32_t g_debug_last_store_pc;

/* Execution-mode tag for the event-timeline ring: 1 while a dirty_ram_dispatch
 * call is on the stack. Cleared defensively at the psx_check_interrupts longjmp
 * landing (interrupts.c) so the EPC-sentinel longjmp at dirty_ram_dispatch
 * can't leave it stuck on. NATIVE_OVERLAY (inprogress!=0) takes precedence in
 * the ring's mode resolution, so this only distinguishes INTERP from STATIC. */
int g_dirty_interp_active = 0;
int dirty_ram_interp_is_active(void) { return g_dirty_interp_active; }

/* Innermost execution phase, for the wall-time sampler (phase_profile).
 * g_dirty_interp_active is NOT this: it stays 1 across a native overlay call
 * made from the interpreter's jal/jalr contract (overlay_loader_call_native),
 * so sampling it reads "inside the dispatch tree", not "interpreting". This
 * variable is save/set/restored at EVERY backend boundary, so a sample reads
 * the backend actually executing at that instant:
 *   0 = host/other (SDL, GPU, top-level dispatch glue, idle)
 *   1 = dirty-RAM interpreter (exec_one body / precise slice)
 *   2 = native overlay shard (gcc/tcc DLL)
 *   3 = compiled static text (game EXE / recompiled BIOS, incl. IRQ handler)
 *   4 = GPU GP0 command processing (gpu.c — raster/batch/VRAM-transfer work)
 * Longjmp contract mirrors g_dirty_interp_active: exception delivery saves it
 * at entry and restores after its setjmp loop (interrupts.c), so a skipped
 * inner restore self-heals at the next bracket. */
int g_exec_phase = 0;
int psx_exec_phase(void) { return g_exec_phase; }

/* TCP-armed instruction-window capture (native↔interp divergence drill).
 * g_insn_gate_*: an extra runtime-settable PC range that the per-insn log
 * records (on top of the hardwired kernel ranges). Freeze latch: on the Nth
 * dispatch of candidate g_insn_freeze_addr the insn ring stops recording, so
 * its tail preserves the window immediately BEFORE that dispatch — query the
 * ring afterward at leisure (ring-first; no arm-then-hope). */
/* ── R3000A load-delay VALUE semantics ──────────────────────────────────────
 * On MIPS-I there is no load interlock: a load's target register is NOT
 * visible to the instruction in the load-delay slot, which still sees the
 * PREVIOUS value. The writeback lands one instruction later.
 *
 * We already modelled the load-delay *interlock timing* (psx_cyc_load_*),
 * but wrote the destination register immediately — so the slot observed the
 * loaded value. That silently diverged from BOTH real hardware and our own
 * compiled backend (full_function_emitter defers the writeback past the
 * successor for dependent pairs), and hand-written kernel asm depends on it.
 *
 * The concrete casualty: OpenBIOS's cardfasttrack.s stashes the pointer
 * variable's BASE with `or $at,$k0,$zero` in the delay slot of
 * `lw $k0,off($k0)`, then writes the advanced pointer back through $at. With
 * an immediate writeback $at became the pointer itself, the write-back landed
 * in unrelated memory, the pointer never advanced, and every byte of a
 * 128-byte memory-card frame was stored to one address (cards read as
 * permanently unformatted; the send side transmitted one byte 128x).
 *
 * Contract: on a same-register conflict the LOAD wins (it retires later),
 * matching the compiled emitter. Pending state is flushed on interpreter exit
 * and before exception delivery, where the pipeline would have drained. */
static uint32_t s_ld_pend_rt    = 0;
static uint32_t s_ld_pend_val   = 0;
static uint32_t s_ld_pend_age   = 0;  /* 0 = armed; 1 = delay slot has run */
static int      s_ld_pend_armed = 0;

/* Retire a deferred load writeback. Call wherever the interpreter stops
 * stepping instructions (hand-off to compiled code, exception entry), since
 * nothing downstream knows about the pending register write. */
void dirty_ram_ld_delay_flush(CPUState *cpu) {
    if (!s_ld_pend_armed) return;
    s_ld_pend_armed = 0;
    s_ld_pend_age   = 0;
    if (s_ld_pend_rt != 0u) cpu->gpr[s_ld_pend_rt] = s_ld_pend_val;
    cpu->gpr[0] = 0;
}

void dirty_ram_ld_delay_discard(void) {
    s_ld_pend_armed = 0;
    s_ld_pend_age   = 0;
    s_ld_pend_rt    = 0;
    s_ld_pend_val   = 0;
}

void dirty_ram_irq_ambient_resync_after_restore(void) {
    /* Re-anchor host-only IRQ pump ambient at the restored timeline so both
     * peers take the first post-load dirty entry poll from the same phase. */
    s_last_dirty_irq_pump_insns = g_dirty_ram_insns_run;
    s_interp_entry_poll = 0;
}

uint32_t g_insn_gate_lo = 0;       /* extra always-log phys range [lo,hi)      */
uint32_t g_insn_gate_hi = 0;       /* 0 = extra range disabled                 */
uint32_t g_insn_freeze_addr  = 0;  /* candidate phys entry to watch            */
uint32_t g_insn_freeze_nth   = 0;  /* freeze before its Nth dispatch           */
uint32_t g_insn_freeze_count = 0;
int      g_insn_log_frozen   = 0;
/* Tomba2 wild-jump capture (Confirm-(b)): freeze the insn ring the instant an
 * interpreted instruction's transfer TARGET (or next_pc) equals this watched
 * value, so the offending jr/jalr is preserved as the ring's last entry with its
 * full register snapshot. Set via the `insn_freeze_target` debug command (0 =
 * disabled). Matches on full 32-bit value (wild PCs are not phys-normalizable). */
uint32_t g_insn_freeze_on_target = 0;
/* Register snapshot captured at the instruction that hits g_insn_freeze_on_target
 * (the offending jr/jalr). g_freeze_snap_valid latches 1; pc/insn identify the
 * branch, gpr[] is the full guest register file at that moment (the source
 * register still holds the wild target; scan for which reg == the target). */
int      g_freeze_snap_valid = 0;
uint32_t g_freeze_snap_pc    = 0;
uint32_t g_freeze_snap_insn  = 0;
uint32_t g_freeze_snap_tcb   = 0;
uint32_t g_freeze_snap_gpr[32] = {0};
/* ra-load watch (Confirm-(b)): capture the instruction that sets $ra to this
 * value. 0 = disabled. */
uint32_t g_ra_load_watch        = 0;
/* Callee-smear tripwire (see the interp-loop probe): first $s3 change
 * inside [lo,hi) that isn't the walk's own advance, with the call target. */
uint32_t g_s3_smear_lo = 0, g_s3_smear_hi = 0;
/* Optional exact-encoding exclusion so a watched loop's OWN $s3 advance
 * (e.g. an `addi s3,s3,8` list walk) doesn't trip the latch. 0 = none. */
uint32_t g_s3_smear_excl = 0;
uint32_t g_s3_smear_pc = 0, g_s3_smear_insn = 0, g_s3_smear_old = 0,
         g_s3_smear_new = 0, g_s3_smear_tgt = 0, g_s3_smear_frame = 0;
int      g_s3_smear_valid = 0;
int      g_ra_load_snap_valid   = 0;
uint32_t g_ra_load_snap_pc      = 0;
uint32_t g_ra_load_snap_insn    = 0;
uint32_t g_ra_load_snap_before_ra = 0;
uint32_t g_ra_load_snap_srcaddr = 0;
uint32_t g_ra_load_snap_gpr[32] = {0};

/* Call-resolution ring (armed via `callret_watch lo=.. hi=..`).
 * For every interp JALR whose call PC lies in [lo,hi), record which resolution
 * tier ran the callee and the FULL host-side outcome: post-call guest state,
 * bail/escape flags, and engine-attribution deltas (static-dispatch hits vs
 * interp blocks across the call). This is the piece the s3 tripwire lacks:
 * the tripwire names the callee that came back smeared; this ring names the
 * RETURN PATH that let it come back. Zero-cost when disarmed. */
uint32_t g_callret_lo = 0, g_callret_hi = 0;
/* MUST stay field-for-field identical to the local mirror `E` in
 * debug_server.c handle_callret_watch() (which dumps this ring through an
 * opaque extern; a divergence is silent garbage, not a compile error). */
typedef struct {
    uint64_t cycle; uint32_t frame;
    uint32_t pc, target, sp_b, ra_b, s0_b, s3_b;
    uint32_t path;                  /* CRES_* code, |0x100 if finish() escaped */
    uint32_t pc_a, ra_a, sp_a, s0_a, s3_a, v0_a;
    uint32_t bail_a, rfe_a, esc_a, in_exc_a;
    uint32_t dstatic, dblocks;      /* engine deltas across the call */
    uint32_t dexc;                  /* exception entries across the call */
    uint32_t last_func_a;           /* g_debug_current_func_addr after */
} CallRetEnt;
#define CALLRET_CAP 64u
CallRetEnt g_callret_ring[CALLRET_CAP];
uint64_t   g_callret_seq = 0;
/* CRES path codes (JALR tiers, in consult order). */
enum { CRES_PLAIN = 1,
       CRES_EC_BAIL = 2, CRES_EC_PC = 3, CRES_EC_CONTRACT = 4, CRES_EC_RET = 5,
       CRES_OVERRIDE = 6,
       CRES_OV_BAIL = 7, CRES_OV_PC = 8, CRES_OV_CONTRACT = 9, CRES_OV_RET = 10,
       CRES_NL_BAIL = 11, CRES_NL_PC = 12, CRES_NL_RET = 13,
       CRES_PCCHAIN = 14 };
extern uint64_t g_dispatch_static_hits;   /* debug_server.c; bumped by generated dispatch */
extern uint64_t psx_cycle_count;
static uint32_t callret_begin(CPUState *cpu, uint32_t pc, uint32_t target) {
    if (!g_callret_lo || pc < g_callret_lo || pc >= g_callret_hi)
        return 0xFFFFFFFFu;
    uint32_t idx = (uint32_t)(g_callret_seq++ & (CALLRET_CAP - 1u));
    CallRetEnt *e = &g_callret_ring[idx];
    e->cycle = psx_cycle_count; e->frame = (uint32_t)s_frame_count;
    e->pc = pc; e->target = target;
    e->sp_b = cpu->gpr[29]; e->ra_b = cpu->gpr[31];
    e->s0_b = cpu->gpr[16]; e->s3_b = cpu->gpr[19];
    e->path = 0;
    e->dstatic = (uint32_t)g_dispatch_static_hits;
    e->dblocks = (uint32_t)g_dirty_ram_blocks_run;
    { extern void psx_get_freeze_diag(uint64_t*,uint32_t*,int*,int*,uint64_t*,uint64_t*);
      uint64_t exc = 0; psx_get_freeze_diag(NULL, NULL, NULL, NULL, &exc, NULL);
      e->dexc = (uint32_t)exc; }
    return idx;
}
static void callret_end(uint32_t idx, CPUState *cpu, uint32_t path) {
    if (idx == 0xFFFFFFFFu) return;
    CallRetEnt *e = &g_callret_ring[idx];
    e->path = path;
    e->pc_a = cpu->pc; e->ra_a = cpu->gpr[31]; e->sp_a = cpu->gpr[29];
    e->s0_a = cpu->gpr[16]; e->s3_a = cpu->gpr[19]; e->v0_a = cpu->gpr[2];
    e->bail_a = (uint32_t)g_psx_call_bail;
    { extern int g_rfe_escape_pending; extern int g_exc_escape_reason;
      e->rfe_a = (uint32_t)g_rfe_escape_pending;
      e->esc_a = (uint32_t)g_exc_escape_reason; }
    e->in_exc_a = (uint32_t)psx_get_in_exception();
    e->dstatic = (uint32_t)g_dispatch_static_hits - e->dstatic;
    e->dblocks = (uint32_t)g_dirty_ram_blocks_run - e->dblocks;
    { extern void psx_get_freeze_diag(uint64_t*,uint32_t*,int*,int*,uint64_t*,uint64_t*);
      uint64_t exc = 0; psx_get_freeze_diag(NULL, NULL, NULL, NULL, &exc, NULL);
      e->dexc = (uint32_t)exc - e->dexc; }
    e->last_func_a = g_debug_current_func_addr;
}

/* Overlay-region floor (phys) = the loaded game's main-EXE text end. Defaults to
 * the BIOS-only value; main.cpp pins it to (load_address + text_size) at game
 * load. See dirty_ram_interp.h for the per-game rationale. */
uint32_t g_overlay_region_floor = OVERLAY_REGION_FLOOR_DEFAULT;

/* Text-image base (phys) = the loaded game's main-EXE load address. Defaults to
 * the kernel-window end so the below-text overlay clause is empty until a
 * high-loading game pins it; main.cpp sets it at game load. See the header. */
uint32_t g_text_image_lo = DIRTY_RAM_KERNEL_WINDOW_END;

#ifdef PSX_HAS_GAME_DISPATCH
extern int psx_dispatch_game_compiled(CPUState* cpu, uint32_t addr);
extern int psx_game_address_in_text(uint32_t addr);
extern int psx_game_is_function_entry(uint32_t addr);  /* non-destructive entry test */
extern int psx_game_text_native_ok(uint32_t addr);
#endif
extern void psx_dispatch_call(CPUState* cpu, uint32_t addr, uint32_t return_addr);

/* Forward decls from memory.c — used to read instruction bytes. */
extern uint8_t *memory_get_ram_ptr(void);
extern void dirty_ram_mark_executable_range(uint32_t phys, uint32_t len);

/* MIPS instruction field decoders. */
static inline uint32_t op_field    (uint32_t i) { return (i >> 26) & 0x3Fu; }
static inline uint32_t rs_field    (uint32_t i) { return (i >> 21) & 0x1Fu; }
static inline uint32_t rt_field    (uint32_t i) { return (i >> 16) & 0x1Fu; }
static inline uint32_t rd_field    (uint32_t i) { return (i >> 11) & 0x1Fu; }
static inline uint32_t shamt_field (uint32_t i) { return (i >>  6) & 0x1Fu; }
static inline uint32_t funct_field (uint32_t i) { return  i        & 0x3Fu; }
static inline uint32_t imm16_field (uint32_t i) { return  i        & 0xFFFFu; }
static inline int32_t  simm16_field(uint32_t i) { return (int32_t)(int16_t)imm16_field(i); }
static inline uint32_t target26    (uint32_t i) { return  i        & 0x03FFFFFFu; }

/* Read a 32-bit instruction word from kernel RAM at the given physical addr.
 * Caller has already verified the address is in dirty kernel RAM. */
static inline uint32_t fetch_word(uint32_t phys) {
    /* Main RAM is a process-lifetime static allocation. Cache its address so
     * instruction fetch does not cross translation units for every guest op. */
    static const uint8_t *ram;
    if (!ram) ram = memory_get_ram_ptr();
    return  (uint32_t)ram[phys]
         | ((uint32_t)ram[phys + 1] <<  8)
         | ((uint32_t)ram[phys + 2] << 16)
         | ((uint32_t)ram[phys + 3] << 24);
}

/* Widescreen render-funnel cull detection for the interpreter ([widescreen.cull]
 * auto_screen_x). A `sltiu …,0x140/0x141` is widened ONLY when its enclosing
 * render function also carries a `sltiu …,0xE0/0xF1` (the GTE per-vertex trivial-
 * reject signature) — a lone 0x140 elsewhere stays vanilla. The interp has no
 * function boundaries, so we scan a +/-512-byte window around the site (clamped
 * to main RAM) via the shared psx_ws_func_has_screen_cull, cached per-PC so a
 * hot render loop pays the scan once. Single-threaded interp => static buffers ok. */
/* Per-PC site caches: FULL-PC tagged, sized for a whole overlay's working set
 * (the old 256-slot direct map thrashed on any two hot PCs 1 KB apart — the
 * ±512-byte rescan then ran per EXECUTED INSTRUCTION and collapsed native-wide
 * 16:9 to ~12fps, entirely CPU-side). Entries carry the dirty-RAM code
 * generation (memory.c g_dirty_ram_code_gen) so an overlay reload re-derives
 * every classification instead of trusting a stale kind. */
extern uint32_t g_dirty_ram_code_gen;
#define WS_SITE_CACHE_SLOTS 8192u             /* 32 KB of code coverage per cache */

static int ws_cull_site(uint32_t pc) {
    enum { WIN = 128 };                       /* +/- 128 words = +/- 512 bytes */
    static struct { uint32_t pc; uint32_t gen; uint32_t word; int8_t flag; } cache[WS_SITE_CACHE_SLOTS];
    uint32_t slot = (pc >> 2) & (WS_SITE_CACHE_SLOTS - 1u);
    uint32_t phys = pc & 0x1FFFFFFFu;
    /* Entry valid iff pc + generation + the SITE'S OWN INSTRUCTION WORD all
     * match. The word tag catches code replaced by plain CPU stores (loader
     * memcpy), which never routes through the page-marking hooks the
     * generation counter sees — a stale verdict here REWRITES a live GPR and
     * corrupts the guest (wild-jump fatal). */
    if (cache[slot].pc == pc && cache[slot].gen == g_dirty_ram_code_gen &&
        cache[slot].word == fetch_word(phys))
        return cache[slot].flag;
    uint32_t lo = (phys > (uint32_t)(WIN * 4)) ? phys - (uint32_t)(WIN * 4) : 0u;
    uint32_t hi = phys + (uint32_t)(WIN * 4);
    if (hi > 0x800000u) hi = 0x800000u;       /* 8 MB capacity */
    static uint32_t words[2 * WIN + 1];
    int n = 0;
    for (uint32_t a = lo; a + 4u <= hi && n < (int)(2 * WIN + 1); a += 4u)
        words[n++] = fetch_word(a);
    int flag = psx_ws_func_has_screen_cull(words, n);
    cache[slot].pc = pc; cache[slot].gen = g_dirty_ram_code_gen;
    cache[slot].word = fetch_word(phys); cache[slot].flag = (int8_t)flag;
    return flag;
}

/* Widescreen LEFT-edge funnel-bltz classification (auto_screen_x, signed
 * min/max + center±halfwidth idioms — ws_cull_detect.h). Same ±512-byte window
 * qualification + per-PC cache discipline as ws_cull_site above; additionally
 * classifies THIS bltz structurally (delay-slot width compare / addu-subu
 * pair), so an unrelated bltz in a qualifying window stays vanilla. */
static int ws_cull_bltz_site(uint32_t pc) {
    enum { WIN = 128 };                       /* +/- 128 words = +/- 512 bytes */
    static struct { uint32_t pc; uint32_t gen; uint32_t word; int8_t flag; } cache[WS_SITE_CACHE_SLOTS];
    uint32_t slot = (pc >> 2) & (WS_SITE_CACHE_SLOTS - 1u);
    uint32_t phys = pc & 0x1FFFFFFFu;
    if (cache[slot].pc == pc && cache[slot].gen == g_dirty_ram_code_gen &&
        cache[slot].word == fetch_word(phys))
        return cache[slot].flag;
    uint32_t lo = (phys > (uint32_t)(WIN * 4)) ? phys - (uint32_t)(WIN * 4) : 0u;
    uint32_t hi = phys + (uint32_t)(WIN * 4);
    if (hi > 0x800000u) hi = 0x800000u;       /* 8 MB capacity */
    static uint32_t words[2 * WIN + 1];
    int n = 0;
    for (uint32_t a = lo; a + 4u <= hi && n < (int)(2 * WIN + 1); a += 4u)
        words[n++] = fetch_word(a);
    int idx = (int)((phys - lo) / 4u);
    int flag = psx_ws_func_has_screen_cull(words, n) &&
               psx_ws_cull_bltz_at(words, n, idx);
    cache[slot].pc = pc; cache[slot].gen = g_dirty_ram_code_gen;
    cache[slot].word = fetch_word(phys); cache[slot].flag = (int8_t)flag;
    return flag;
}

/* Widescreen far-backdrop column-PRELOAD site classification for the interpreter
 * ([widescreen.cull] auto_backdrop). The scrolling-backdrop column-window
 * generators run interpreted in the dev build, so the recompiler emit can't
 * reach them; the interp re-derives the same START/END rewrite sites via the
 * shared detector over a +/-512-byte window around the PC (physical space, so
 * the detector's absolute-PC math matches the masked PC), cached per-PC so a hot
 * generator pays the scan once. Returns WS_BD_NONE / WS_BD_START_ZERO /
 * WS_BD_END_WIDEN. Single-threaded interp => static buffers are safe. */
static int ws_backdrop_site_kind(uint32_t pc, int *out_cols) {
    enum { WIN = 128 };                          /* +/- 128 words = +/- 512 bytes */
    static struct { uint32_t pc; uint32_t gen; uint32_t word; int8_t kind; int16_t cols; } cache[WS_SITE_CACHE_SLOTS];
    uint32_t slot = (pc >> 2) & (WS_SITE_CACHE_SLOTS - 1u);
    uint32_t phys = pc & 0x1FFFFFFFu;
    /* pc + generation + site instruction word — see ws_cull_site: the word tag
     * is what protects against CPU-store code reloads the generation counter
     * cannot see. A stale KIND here is worse than a stale cull flag: it
     * substitutes a GPR value and skips the real instruction. */
    if (cache[slot].pc == pc && cache[slot].gen == g_dirty_ram_code_gen &&
        cache[slot].word == fetch_word(phys)) {
        if (out_cols) *out_cols = cache[slot].cols;
        return cache[slot].kind;
    }
    uint32_t lo = (phys > (uint32_t)(WIN * 4)) ? phys - (uint32_t)(WIN * 4) : 0u;
    uint32_t hi = phys + (uint32_t)(WIN * 4 + 4);
    if (hi > 0x800000u) hi = 0x800000u;          /* 8 MB capacity */
    static uint32_t words[2 * WIN + 2];
    int n = 0;
    for (uint32_t a = lo; a + 4u <= hi && n < (int)(2 * WIN + 2); a += 4u)
        words[n++] = fetch_word(a);
    int cols = 0;
    int kind = psx_ws_backdrop_kind_at(words, n, lo, phys, &cols);  /* all physical-space */
    cache[slot].pc = pc; cache[slot].gen = g_dirty_ram_code_gen;
    cache[slot].word = fetch_word(phys);
    cache[slot].kind = (int8_t)kind; cache[slot].cols = (int16_t)cols;
    if (out_cols) *out_cols = cols;
    return kind;
}

static void dirty_ram_log_instruction(CPUState *cpu, uint32_t pc, uint32_t insn,
                                      uint32_t before_s0, uint32_t next_pc,
                                      uint32_t target, int transferred) {
    if (g_insn_log_frozen) return;
    /* Freeze-on-target (Confirm-(b)): checked BEFORE the gate filter so it fires
     * for ANY interpreted instruction transferring to the watched wild PC, even
     * if that jr/jalr sits outside the recorded gate range. We do NOT record this
     * out-of-gate entry (the gate still governs what's stored); we just stop the
     * ring so the in-gate window leading up to the wild jump is preserved. */
    if (g_insn_freeze_on_target != 0u &&
        (target == g_insn_freeze_on_target || next_pc == g_insn_freeze_on_target)) {
        if (!g_freeze_snap_valid) {
            g_freeze_snap_pc   = pc;
            g_freeze_snap_insn = insn;
            uint32_t tcb_ptr = cpu->read_word(0x00000108u);
            g_freeze_snap_tcb = tcb_ptr ? cpu->read_word(tcb_ptr) : 0;
            for (int r = 0; r < 32; r++) g_freeze_snap_gpr[r] = cpu->gpr[r];
            g_freeze_snap_valid = 1;
        }
        g_insn_log_frozen = 1;
        return;
    }
    uint32_t phys = pc & 0x1FFFFFFFu;
    if (!((g_insn_gate_hi != 0u && phys >= g_insn_gate_lo && phys < g_insn_gate_hi) ||
          (phys >= 0x000E0000u && phys < 0x000F0000u) ||
          (phys >= 0x000000A0u && phys < 0x000000C0u) ||
          (phys >= 0x000005C0u && phys < 0x00000620u) ||
          (phys >= 0x00001E00u && phys < 0x00002000u))) {
        return;
    }

    uint32_t tcb_ptr_addr = cpu->read_word(0x00000108u);
    uint32_t current_tcb = tcb_ptr_addr ? cpu->read_word(tcb_ptr_addr) : 0;
    uint32_t task_ptr = cpu->read_word(0x1F8001D4u);

    uint64_t s = g_dirty_ram_insn_log_seq++;
    DirtyRamInsnLogEntry *e =
        &g_dirty_ram_insn_log[s & (DIRTY_RAM_INSN_LOG_CAP - 1u)];
    e->seq          = s;
    e->pc           = pc;
    e->insn         = insn;
    e->next_pc      = next_pc;
    e->target       = target;
    e->before_s0    = before_s0;
    e->after_s0     = cpu->gpr[16];
    e->sp           = cpu->gpr[29];
    e->ra           = cpu->gpr[31];
    e->v0           = cpu->gpr[2];
    e->v1           = cpu->gpr[3];
    e->a0           = cpu->gpr[4];
    e->a1           = cpu->gpr[5];
    e->a2           = cpu->gpr[6];
    e->a3           = cpu->gpr[7];
    e->t0           = cpu->gpr[8];
    e->t1           = cpu->gpr[9];
    e->t2           = cpu->gpr[10];
    e->at           = cpu->gpr[1];
    e->k0           = cpu->gpr[26];
    e->k1           = cpu->gpr[27];
    e->current_tcb  = current_tcb;
    e->task_ptr     = task_ptr;
    e->task_mode    = task_ptr ? cpu->read_half(task_ptr + 72u) : 0;
    e->task_submode = task_ptr ? cpu->read_half(task_ptr + 74u) : 0;
    e->frame        = (uint32_t)s_frame_count;
    e->transferred  = (uint8_t)(transferred ? 1u : 0u);
}

/* Marker entries delimit NATIVE candidate executions in the insn ring (native
 * code emits no per-insn entries; markers keep the two runs' timelines
 * alignable). transferred: 200 = native entry, 201 = native exit. */
void dirty_ram_log_marker(uint32_t addr, uint32_t tag, int kind) {
    if (g_insn_log_frozen) return;
    uint64_t s = g_dirty_ram_insn_log_seq++;
    DirtyRamInsnLogEntry *e =
        &g_dirty_ram_insn_log[s & (DIRTY_RAM_INSN_LOG_CAP - 1u)];
    memset(e, 0, sizeof *e);
    e->seq         = s;
    e->pc          = addr;
    e->target      = tag;
    e->frame       = (uint32_t)s_frame_count;
    e->transferred = (uint8_t)(200 + kind);
}

/* LWL/LWR are merge-only here; the timed aligned-word read is done by the caller via
 * psx_cyc_load_word (Beetle reads the aligned word; GPR_DEP rs only, arms LDWhich=rt). */
static inline uint32_t lwl_merge(uint32_t addr, uint32_t word, uint32_t rt_value) {
    switch (addr & 3u) {
        case 0: return (rt_value & 0x00FFFFFFu) | (word << 24);
        case 1: return (rt_value & 0x0000FFFFu) | (word << 16);
        case 2: return (rt_value & 0x000000FFu) | (word << 8);
        default: return word;
    }
}

static inline uint32_t lwr_merge(uint32_t addr, uint32_t word, uint32_t rt_value) {
    switch (addr & 3u) {
        case 0: return word;
        case 1: return (rt_value & 0xFF000000u) | (word >> 8);
        case 2: return (rt_value & 0xFFFF0000u) | (word >> 16);
        default: return (rt_value & 0xFFFFFF00u) | (word >> 24);
    }
}

static inline void interp_swl(CPUState *cpu, uint32_t addr, uint32_t value) {
    uint32_t aligned = addr & ~3u;
    uint32_t word = cpu->read_word(aligned);
    switch (addr & 3u) {
        case 0: word = (word & 0xFFFFFF00u) | (value >> 24); break;
        case 1: word = (word & 0xFFFF0000u) | (value >> 16); break;
        case 2: word = (word & 0xFF000000u) | (value >> 8); break;
        default: word = value; break;
    }
    cpu->write_word(aligned, word);
}

static inline void interp_swr(CPUState *cpu, uint32_t addr, uint32_t value) {
    uint32_t aligned = addr & ~3u;
    uint32_t word = cpu->read_word(aligned);
    switch (addr & 3u) {
        case 0: word = value; break;
        case 1: word = (word & 0x000000FFu) | (value << 8); break;
        case 2: word = (word & 0x0000FFFFu) | (value << 16); break;
        default: word = (word & 0x00FFFFFFu) | (value << 24); break;
    }
    cpu->write_word(aligned, word);
}

/* Soft-fail thread-local flag.  When the interpreter encounters an opcode
 * it doesn't implement, it sets this flag and returns instead of aborting,
 * letting the caller (psx_dispatch via dirty_ram_dispatch) fall back to
 * psx_unknown_dispatch — which has its own ad-hoc resolver for known
 * trampoline patterns (jr-based vector dispatch, etc.).
 *
 * This is a deliberate retreat from "always pick the most complete option"
 * for ONE narrow case: dispatch into pages that have been written-to but
 * don't actually contain valid stub code at the dispatched PC (e.g.
 * stale data, return-target addresses that point to non-code areas).  The
 * pre-existing psx_unknown_dispatch already handles those — we just need
 * to let it.  If a true install stub uses an opcode we don't have, this
 * will silently route it to psx_unknown_dispatch, which will likely return
 * a no-op cpu->pc=0.  When that happens, we'll see "card protocol stalls"
 * in measurement and add the missing opcode here. */
static int g_unsupported_seen = 0;
static uint32_t g_unsupported_pc = 0;
static uint32_t g_unsupported_insn = 0;
static const char *g_unsupported_reason = NULL;

static int abort_unsupported(uint32_t pc, uint32_t insn, const char *reason) {
    g_dirty_ram_aborts++;
    g_unsupported_seen   = 1;
    g_unsupported_pc     = pc;
    g_unsupported_insn   = insn;
    g_unsupported_reason = reason;
    return 1; /* signal "control transferred" so the caller stops */
}

/* A dirty page that should run as a locally-chained overlay: anything ABOVE the
 * kernel window. The kernel window [0, DIRTY_RAM_KERNEL_WINDOW_END) intentionally
 * stays per-block (it runs in exception context, where interrupt-check cadence and
 * EPC handling are delicate — see dirty_ram_interp.h window note; kernel native
 * coverage comes from overlay_loader candidates, not interp chaining).
 *
 * NOTE this is the kernel-window end, NOT OVERLAY_REGION_FLOOR. The floor is the
 * end of the boot-EXE text, and the original design assumed [0x10000, FLOOR) is
 * immutable statically-recompiled text. That assumption is FALSE for games that
 * discard their boot/init code and load gameplay overlays OVER the boot-text region
 * (Tomba 2: a START.BIN-loader overlay at 0x8001Dxxx, well below its 0x38800 floor).
 * Such a page is dirty (RAM != compiled image), so dispatching to it must interpret
 * the live overlay and chain locally — exactly like the [FLOOR, RAM_SIZE) overlay
 * region — NOT run the stale compiled image or route through the bail-prone
 * non-local-call contract (the Whoopee-Camp splash wild-jr). dirty_ram_is_dirty()
 * keeps clean boot text on the fast compiled path; only overwritten pages divert. */
static inline int phys_is_overlay_flow_region(uint32_t phys) {
    return phys >= DIRTY_RAM_KERNEL_WINDOW_END;
}

static int is_local_dirty_target(uint32_t target) {
    uint32_t phys = target & 0x1FFFFFFFu;
    return phys_is_overlay_flow_region(phys) && dirty_ram_is_dirty(phys);
}

/* Target the last interp run handed back to the dispatch loop (chained
 * continuation). Consumed by the next dirty dispatch to tell apart
 * external entries (from native code) from the interpreter's own
 * block-to-block chaining. */
static uint32_t g_dirty_interp_chain_target = 0;

/* A dirty-RAM target only deserves interpretation if its first word decodes
 * as a plausible MIPS instruction.  A scatter-loaded overlay leaves data
 * bytes in dirty pages; jumping into those and interpreting them as code is
 * how the original cache build produced black/blue screens.  When the target
 * word doesn't look like code, fall back to the normal dispatch path. */
static int dirty_ram_word_looks_decodable(uint32_t insn) {
    if (insn == 0xFFFFFFFFu || insn == 0xFFFFFFFDu) return 0;

    uint32_t op = op_field(insn);
    uint32_t fn = funct_field(insn);
    uint32_t rt = rt_field(insn);

    if (op == 0x00u) {
        switch (fn) {
        case 0x00u: case 0x02u: case 0x03u: case 0x04u:
        case 0x06u: case 0x07u: case 0x08u: case 0x09u:
        case 0x0Cu: case 0x0Du:
        case 0x10u: case 0x11u: case 0x12u: case 0x13u:
        case 0x18u: case 0x19u: case 0x1Au: case 0x1Bu:
        case 0x20u: case 0x21u: case 0x22u: case 0x23u:
        case 0x24u: case 0x25u: case 0x26u: case 0x27u:
        case 0x2Au: case 0x2Bu:
            return 1;
        default:
            return 0;
        }
    }
    if (op == 0x01u) {
        return rt == 0x00u || rt == 0x01u || rt == 0x10u || rt == 0x11u;
    }

    switch (op) {
    case 0x02u: case 0x03u: case 0x04u: case 0x05u:
    case 0x06u: case 0x07u:
    case 0x08u: case 0x09u: case 0x0Au: case 0x0Bu:
    case 0x0Cu: case 0x0Du: case 0x0Eu: case 0x0Fu:
    case 0x10u: case 0x12u:
    case 0x20u: case 0x21u: case 0x22u: case 0x23u:
    case 0x24u: case 0x25u: case 0x26u:
    case 0x28u: case 0x29u: case 0x2Au: case 0x2Bu:
    case 0x2Eu:
    case 0x30u: case 0x32u: case 0x38u: case 0x3Au:
        return 1;
    default:
        return 0;
    }
}

static int dirty_ram_same_pc(uint32_t a, uint32_t b) {
    return (((a ^ b) & 0x1FFFFFFFu) == 0);
}

static int dirty_ram_pump_boundary(CPUState *cpu, uint32_t committed_pc, int site) {
    uint32_t prev_safe = g_dirty_safe_resume_pc;
    int prev_site = g_cosim_dirty_pump_site;

    s_last_dirty_irq_pump_insns = g_dirty_ram_insns_run;
    if (!psx_interrupts_checked_at_current_cycle(committed_pc)) {
        g_dirty_safe_resume_pc = committed_pc;
        g_cosim_dirty_pump_site = site;
        psx_check_interrupts(cpu);
        g_dirty_safe_resume_pc = prev_safe;
        g_cosim_dirty_pump_site = prev_site;
    }

    if (g_psx_call_bail) return 1;
    if (cpu->pc == 0u && committed_pc != 0u) {
        cpu->pc = committed_pc;
        g_async_rfe_fire_count++;
    }
    if (cpu->pc != 0u && !dirty_ram_same_pc(cpu->pc, committed_pc)) {
        return 1;
    }

    cpu->pc = committed_pc;
    return 0;
}

static int dirty_ram_finish_call_return(CPUState *cpu, uint32_t return_pc,
                                        uint32_t *next_pc_out) {
    uint32_t prev_pc = cpu->pc;
    cpu->pc = return_pc;
    if (dirty_ram_pump_boundary(cpu, return_pc, 5)) return 1;
    cpu->pc = prev_pc;
    *next_pc_out = return_pc;
    return 0;
}

/* Sub-outcome of the last dispatch_nonlocal_call, for the callret ring. */
static uint32_t g_nl_exit_code = 0;
static int dispatch_nonlocal_call(CPUState *cpu, uint32_t target,
                                  uint32_t return_pc,
                                  uint32_t *next_pc_out) {
    cpu->pc = 0;
    {
        int prev_phase = g_exec_phase;
        g_exec_phase = 3;   /* compiled route; dirty/native callees re-tag inside */
        psx_dispatch_call(cpu, target, return_pc);
        g_exec_phase = prev_phase;
    }
    /* psx_dispatch_call validated the (return_pc, sp) contract; a bail
     * unwind in progress surfaces with cpu->pc = the guest's true target. */
    if (g_psx_call_bail) { g_nl_exit_code = CRES_NL_BAIL; return 1; }
    if (cpu->pc != 0)    { g_nl_exit_code = CRES_NL_PC;   return 1; }
    g_nl_exit_code = CRES_NL_RET;
    return dirty_ram_finish_call_return(cpu, return_pc, next_pc_out);
}

/* ── Mixed interp<->compiled dispatch owner (host-stack recursion fix) ──────
 * The dirty interpreter nests psx_dispatch_game_compiled when an interpreted
 * block transfers into compiled code. A guest tail-dispatch loop that crosses the
 * boundary (an interpreted overlay <-> the main-EXE per-frame loop func_8001A954)
 * grows the HOST stack unboundedly while the GUEST stack stays flat — the long-run
 * freeze (docs/RECURSION_BUG.md). The compiled side keeps tail-call loops flat via
 * the psx_dispatch_impl trampoline + g_psx_call_bail unwind; the interpreter
 * bypassed it by nesting directly.
 *
 * Fix: when an interp->compiled crossing would push the HOST stack past a safe
 * watermark, publish the target as a wild-flow bail instead of nesting. The
 * existing contract (compiled frames honor `if (g_psx_call_bail) return;`) unwinds
 * it to the OUTERMOST psx_dispatch_impl, whose flatten path clears the bail and
 * re-dispatches cpu->pc in its loop — so the mutual tail-recursion iterates with
 * bounded native depth. Runtime-only (reuses the existing bail + flatten; no regen).
 *
 * The trigger is the ACTUAL host-stack usage (TEB StackBase - rsp), NOT a nesting
 * counter. A counter LEAKS across the fiber exception path (psx_exception_longjmp
 * never returns, so interp_enter_compiled's post-call decrement is skipped) and
 * creeps up until it falsely trips on a benign call — observed as the FIRST dialogue
 * freezing. Stack usage can't leak and is the real resource we protect: legitimate
 * code (boot, dialogue, deep-but-bounded recursion) stays far below the watermark;
 * only the runaway approaches it. Toggle off with PSX_MIXED_OWNER=0; watermark via
 * PSX_MIXED_STACK_KB (default 700; fiber ~1MB, native-stack guard ~768KB used). */
static int psx_mixed_owner_enabled(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PSX_MIXED_OWNER"); v = (e && e[0] == '0') ? 0 : 1; }
    return v;
}

#ifdef _WIN32
#include <intrin.h>   /* __readgsqword — fiber TEB StackBase */
static size_t interp_host_stack_used(void) {
    char probe;
    uintptr_t base = (uintptr_t)__readgsqword(0x08);   /* TEB StackBase (high end) */
    uintptr_t sp   = (uintptr_t)&probe;
    return (base > sp) ? (size_t)(base - sp) : 0;
}
#else
static size_t interp_host_stack_used(void) { return 0; }
#endif

static size_t psx_mixed_stack_watermark(void) {
    static long v = -1;
    if (v < 0) { const char *e = getenv("PSX_MIXED_STACK_KB");
                 v = (e && *e) ? atol(e) : 700; if (v < 64) v = 64; }
    return (size_t)v * 1024u;
}

/* ── Flight recorder: per-frame count of the re-entry edge interp->0x8001A954 ──
 * (RECURSION_BUG.md §15). The long-run freeze is a per-frame update that, at
 * ~frame 50k, re-enters func_8001A954 (the per-frame loop head) unboundedly via
 * an interpreted overlay. Record, per emulated frame, how many times that edge
 * fires + the cycle counter, in a ring the crash report dumps. Answers: is the
 * re-entry ORDINARY bounded per-frame behavior that stops TERMINATING at ~50k
 * (count goes unbounded), or a brand-new edge — and whether cycles advance.
 * Only the INTERP->compiled re-entry passes through here; the normal compiled
 * per-frame entry to func_8001A954 does not, so this isolates the suspect edge. */
extern uint64_t psx_cycle_count;
#define SITE_CAP 32u   /* 3 sites x 256 overflowed crash_trace's 8KB sub-buffer; 32 frames of lead-up is plenty */
typedef struct { uint32_t frame; uint32_t count; uint64_t cycle; } SiteEntry;
typedef struct { SiteEntry ring[SITE_CAP]; uint64_t seq; uint32_t cur;
                 uint32_t last_frame; uint32_t maxf; } SiteRec;
/* Per-frame dispatch-count recorders for the suspect loop sites (RECURSION_BUG.md
 * §15). g_site_interp (interp->0x8001A954) was found ORDINARY (1/frame); the
 * recursion re-dispatches the compiled loop via dirty_ram_dispatch's entry because
 * the CD-DMA dirty bitmap is saturated. So record dirty_ram_dispatch(0x8001A954)
 * = loop head and dirty_ram_dispatch(0x80046264) = loop tail. Bounded normally,
 * spike at the freeze => "the per-frame loop stopped terminating." */
static SiteRec g_site_interp = { .last_frame = 0xFFFFFFFFu };  /* interp -> 0x8001A954 */
static SiteRec g_site_dd954  = { .last_frame = 0xFFFFFFFFu };  /* dirty_ram_dispatch(0x8001A954) */
static SiteRec g_site_dd264  = { .last_frame = 0xFFFFFFFFu };  /* dirty_ram_dispatch(0x80046264) */

static void site_note(SiteRec *s) {
    uint32_t f = (uint32_t)s_frame_count;
    if (f != s->last_frame) {
        if (s->last_frame != 0xFFFFFFFFu) {
            SiteEntry *e = &s->ring[s->seq++ & (SITE_CAP - 1u)];
            e->frame = s->last_frame; e->count = s->cur; e->cycle = psx_cycle_count;
            if (s->cur > s->maxf) s->maxf = s->cur;
        }
        s->cur = 0; s->last_frame = f;
    }
    s->cur++;
}

static int site_json(char *out, int cap, const char *name, const SiteRec *s) {
    int n = snprintf(out, cap, "  \"%s\": {\"max_per_frame\":%u,\"cur_frame\":%u,\"cur_count\":%u,\"history\":[",
                     name, s->maxf, s->last_frame, s->cur);
    uint64_t total = s->seq;
    int avail = (total < SITE_CAP) ? (int)total : (int)SITE_CAP;
    uint64_t start = total - (uint64_t)avail;
    for (int i = 0; i < avail && n < cap - 96; i++) {
        const SiteEntry *e = &s->ring[(start + (uint64_t)i) & (SITE_CAP - 1u)];
        n += snprintf(out + n, cap - n, "%s{\"f\":%u,\"n\":%u,\"cyc\":%llu}",
                      i ? "," : "", e->frame, e->count, (unsigned long long)e->cycle);
    }
    n += snprintf(out + n, cap - n, "]},\n");
    return n;
}

/* Emit all three loop-site recorders as JSON (called by crash_trace). */
int dirty_ram_re954_json(char *out, int cap) {
    int n = 0;
    n += site_json(out + n, cap - n, "reentry_interp_8001A954", &g_site_interp);
    n += site_json(out + n, cap - n, "dispatch_8001A954",       &g_site_dd954);
    n += site_json(out + n, cap - n, "dispatch_80046264",       &g_site_dd264);
    return n;
}

/* ── Boundary control-flow flight recorder (RECURSION_BUG.md §18) ──────────────
 * The long-run freeze is a runaway interp↔compiled re-entry. To choose among the
 * four shapes (gradual boundary leak / sudden same-frame spin / interpreter
 * fabrication / real guest wait-loop) WITHOUT assuming any of them, record every
 * interp→compiled crossing at BOTH nesting sites (interp_enter_compiled for guest
 * JAL/JALR, and dirty_ram_dispatch_inner's psx_dispatch_game_compiled) plus the
 * watched-set tail-transfers (J/JR to the loop addresses). Two always-on rings:
 *   - per-FRAME summary  -> the time-series (does depth/crossings grow across
 *                            frames, or explode in one?). Answers Q1/Q2/Q3.
 *   - per-CROSSING detail -> src_pc, decoded op, target, delay-slot insn, sp/ra,
 *                            mixed depth, host-stack KB, I_STAT/I_MASK. Answers Q4.
 * Trips EARLY (per-frame crossings or host-stack KB over a low threshold) so the
 * detail ring still holds the ONSET (the transition frame's first crossings), not
 * the 23k steady state that the terminal native-stack guard would capture. The
 * dump routes through the normal crash report (xprobe_json), reusing the proven
 * flush+halt+serve path. Watched set is flagged, but ALL crossings are recorded
 * so the real driver address is not pre-assumed. NB: there is no due-cycle event
 * scheduler in this build (psx_cycles.c), so "next scheduled event cycle" is N/A;
 * cycles-advanced-per-frame + I_STAT/I_MASK cover event progress instead. */
extern uint32_t i_stat;   /* owned by memory.c */
extern uint32_t i_mask;

enum { XOP_JAL = 0, XOP_JALR = 1, XOP_JR = 2, XOP_J = 3, XOP_DD = 4, XOP_BR = 5,
       XOP_RES = 6 /* watched-target call RESOLUTION: site = path code (XRES_*),
                      ds_insn = v0 after the path ran */ };
enum { XSITE_INTERP = 0, XSITE_DD = 1 };
/* XOP_RES path codes (in the `site` field). */
enum { XRES_EC_BAIL = 2, XRES_EC_PC = 3, XRES_EC_CONTRACT = 4, XRES_EC_RET = 5,
       XRES_OV_BAIL = 6, XRES_OV_PC = 7, XRES_OV_CONTRACT = 8, XRES_OV_RET = 9,
       XRES_NONLOCAL = 10, XRES_PCCHAIN = 11, XRES_UNDECODABLE = 12 };

int g_mixed_depth = 0;   /* best-effort interp→compiled nesting depth; reset per frame */

typedef struct {
    uint32_t frame; uint64_t cycle; uint32_t src_pc; uint32_t target;
    uint32_t ds_insn; uint32_t sp; uint32_t ra; uint32_t stk_kb;
    uint32_t istat; uint32_t imask; uint16_t depth; uint8_t op; uint8_t site;
} XDetail;
typedef struct {
    uint32_t frame; uint32_t crossings; uint32_t a954; int32_t depth_max;
    uint32_t stk_max_kb; uint64_t cyc_advanced;
} XSum;

#define XDET_CAP 16384u
#define XSUM_CAP 512u
static XDetail g_xdet[XDET_CAP];
static XSum    g_xsum[XSUM_CAP];
static uint64_t g_xdet_seq = 0, g_xsum_seq = 0;

/* per-frame accumulators */
static uint32_t s_xf_frame = 0xFFFFFFFFu;
static uint32_t s_xf_crossings = 0, s_xf_a954 = 0;
static int32_t  s_xf_depth_max = 0;
static uint32_t s_xf_stk_max_kb = 0;
static uint64_t s_xf_cyc_start = 0;

/* first crossing where mixed depth became > 0 this run (Q1) */
static uint32_t s_xf_first_depth_frame = 0xFFFFFFFFu;
static uint64_t s_xf_first_depth_cycle = 0;
static int      s_xprobe_tripped = 0;

/* Trip thresholds — DISABLED by default (0). Deep but LEGITIMATE boot stacks
 * reach ~300KB host at crossing points (mixed-depth small) and would spuriously
 * trip an absolute threshold. So arm at RUNTIME via the `xprobe_arm` TCP command
 * once the game is idling and the normal per-frame baseline is known (idle stack
 * is a flat ~5KB, so a threshold ~3x the idle crossing baseline / a stack KB well
 * above 5KB then fires only on the runaway, with the onset still in the ring).
 * warmup suppresses any trip before that frame regardless. Env overrides allow
 * arming at launch (PSX_XPROBE_FRAME_TRIP / _STK_KB / _WARMUP). */
int g_xprobe_frame_trip = 0;   /* per-frame crossings; 0 = disabled */
int g_xprobe_stk_kb     = 0;   /* host-stack KB; 0 = disabled */
int g_xprobe_warmup     = 0;   /* no trip before this frame */
static int g_xprobe_env_done = 0;
static void xprobe_env_init(void) {
    if (g_xprobe_env_done) return;
    g_xprobe_env_done = 1;
    const char *a = getenv("PSX_XPROBE_FRAME_TRIP"); if (a && *a) g_xprobe_frame_trip = atoi(a);
    const char *b = getenv("PSX_XPROBE_STK_KB");     if (b && *b) g_xprobe_stk_kb     = atoi(b);
    const char *c = getenv("PSX_XPROBE_WARMUP");     if (c && *c) g_xprobe_warmup     = atoi(c);
}
void dirty_ram_xprobe_arm(int frame_trip, int stk_kb, int warmup) {
    g_xprobe_frame_trip = frame_trip; g_xprobe_stk_kb = stk_kb; g_xprobe_warmup = warmup;
    s_xprobe_tripped = 0;   /* (re-)arm */
}

static void xprobe_flush_frame(void) {
    if (s_xf_frame == 0xFFFFFFFFu) return;
    XSum *s = &g_xsum[g_xsum_seq++ & (XSUM_CAP - 1u)];
    s->frame = s_xf_frame; s->crossings = s_xf_crossings; s->a954 = s_xf_a954;
    s->depth_max = s_xf_depth_max; s->stk_max_kb = s_xf_stk_max_kb;
    s->cyc_advanced = psx_cycle_count - s_xf_cyc_start;
}

/* Record one boundary crossing. want_detail=1 for interp-site guest transfers
 * (rich context), 0 for the dd-site (count + depth only). Runs the per-frame
 * flush on frame change (leak-proof reset of g_mixed_depth) and the early trip. */
static int g_xprobe_watch(uint32_t t) {
    return t == 0x8001A954u || t == 0x80046264u || t == 0x8004630Cu || t == 0x8004DFA0u
        /* MMX6 card-load firstfile flow (mmx6_card_load_regression_state):
         * the mount 0x8001C1AC (works) vs firstfile flow 0x8001C4C0 (its body
         * never runs) — record every call event + resolution for both. */
        || t == 0x8001C1ACu || t == 0x8001C4C0u;
}
static void xprobe_event(uint32_t src_pc, uint8_t op, uint8_t site, uint32_t target,
                         uint32_t ds_insn, uint32_t sp, uint32_t ra, int want_detail);
/* Watched-target call note for NON-interp call sites (overlay shard call-outs
 * via dispatch_call). phase 0 = before the call (ds = a0),
 * phase 1 = after it returned (ds = v0). Recorded as XOP_RES with
 * site = 20+phase so the `watched` dump shows the shard-side call + result. */
void dirty_ram_xprobe_call_note(CPUState *cpu, uint32_t target, uint32_t ra, uint8_t phase) {
    if (!g_xprobe_watch(target)) return;
    xprobe_event(ra, XOP_RES, (uint8_t)(20u + phase), target,
                 phase ? cpu->gpr[2] : cpu->gpr[4], cpu->gpr[29], cpu->gpr[31], 1);
}
static void xprobe_event(uint32_t src_pc, uint8_t op, uint8_t site, uint32_t target,
                         uint32_t ds_insn, uint32_t sp, uint32_t ra, int want_detail) {
    uint32_t f = (uint32_t)s_frame_count;
    if (f != s_xf_frame) {
        xprobe_flush_frame();
        s_xf_frame = f; s_xf_crossings = 0; s_xf_a954 = 0; s_xf_depth_max = 0;
        s_xf_stk_max_kb = 0; s_xf_cyc_start = psx_cycle_count;
        g_mixed_depth = 0;   /* shallow at frame top (proven ~5KB) — leak-proof reset */
    }
    s_xf_crossings++;
    if (target == 0x8001A954u) s_xf_a954++;
    if (g_mixed_depth > s_xf_depth_max) s_xf_depth_max = g_mixed_depth;
    if (g_mixed_depth > 0 && s_xf_first_depth_frame == 0xFFFFFFFFu) {
        s_xf_first_depth_frame = f; s_xf_first_depth_cycle = psx_cycle_count;
    }
    uint32_t stk = (uint32_t)(interp_host_stack_used() >> 10);
    if (stk > s_xf_stk_max_kb) s_xf_stk_max_kb = stk;

    if (want_detail || g_xprobe_watch(target)) {
        XDetail *e = &g_xdet[g_xdet_seq++ & (XDET_CAP - 1u)];
        e->frame = f; e->cycle = psx_cycle_count; e->src_pc = src_pc; e->target = target;
        e->ds_insn = ds_insn; e->sp = sp; e->ra = ra; e->stk_kb = stk;
        e->istat = i_stat; e->imask = i_mask;
        e->depth = (uint16_t)(g_mixed_depth > 0xFFFF ? 0xFFFF : g_mixed_depth);
        e->op = op; e->site = site;
    }

    xprobe_env_init();
    if (!s_xprobe_tripped && f >= (uint32_t)g_xprobe_warmup &&
        ((g_xprobe_frame_trip > 0 && (int)s_xf_crossings > g_xprobe_frame_trip) ||
         (g_xprobe_stk_kb     > 0 && (int)stk            > g_xprobe_stk_kb))) {
        s_xprobe_tripped = 1;
        extern void psx_fatal_halt(const char *reason);
        psx_fatal_halt("xprobe: interp<->compiled boundary onset trip "
                       "(per-frame crossings/host-stack over armed threshold — see xprobe rings)");
    }
}

/* Emit the flight-recorder rings as JSON (called by crash_trace). The detail dump
 * is OLDEST-first so the transition (last normal frame -> first runaway crossings)
 * is visible; the trip fires early enough that the onset is still in the ring. */
int dirty_ram_xprobe_json(char *out, int cap) {
    xprobe_flush_frame();   /* fold the in-progress frame into the summary */
    int n = snprintf(out, cap,
        "{\n"
        "    \"tripped\": %d, \"trip_frame\": %u, \"mixed_depth_now\": %d,\n"
        "    \"first_depth_frame\": %u, \"first_depth_cycle\": %llu,\n"
        "    \"frame_trip\": %d, \"stk_kb_trip\": %d, \"warmup\": %d,\n",
        s_xprobe_tripped, s_xf_frame, g_mixed_depth,
        s_xf_first_depth_frame, (unsigned long long)s_xf_first_depth_cycle,
        g_xprobe_frame_trip, g_xprobe_stk_kb, g_xprobe_warmup);

    /* watched-target events FIRST (sparse, the highest-value content): the WHOLE
     * ring filtered to g_xprobe_watch targets + every XOP_RES resolution record.
     * Emitted before summary/detail so the 56KB response cap can never truncate
     * it (it truncated to [] when this section trailed the detail dump). */
    n += snprintf(out + n, cap - n, "    \"watched\": [");
    {
        static const char *opn2[] = {"JAL","JALR","JR","J","DD","BR","RES"};
        uint64_t total = g_xdet_seq;
        uint32_t avail = total < XDET_CAP ? (uint32_t)total : XDET_CAP;
        uint64_t base  = total - avail;
        int firstw = 1;
        for (uint32_t i = 0; i < avail && n < cap - 240; i++) {
            const XDetail *e = &g_xdet[(base + i) & (XDET_CAP - 1u)];
            if (e->op != XOP_RES && !g_xprobe_watch(e->target)) continue;
            n += snprintf(out + n, cap - n,
                "%s{\"f\":%u,\"cyc\":%llu,\"op\":\"%s\",\"site\":%u,\"src\":\"0x%08X\","
                "\"tgt\":\"0x%08X\",\"ds\":\"0x%08X\",\"sp\":\"0x%08X\",\"ra\":\"0x%08X\","
                "\"d\":%u,\"stk\":%u}",
                firstw ? "" : ",", e->frame, (unsigned long long)e->cycle,
                e->op < 7 ? opn2[e->op] : "?", e->site, e->src_pc, e->target, e->ds_insn,
                e->sp, e->ra, e->depth, e->stk_kb);
            firstw = 0;
        }
    }
    n += snprintf(out + n, cap - n, "],\n");

    /* per-frame summary (whole window in the ring) */
    n += snprintf(out + n, cap - n, "    \"summary\": [");
    {
        uint64_t total = g_xsum_seq;
        uint32_t avail = total < XSUM_CAP ? (uint32_t)total : XSUM_CAP;
        uint64_t start = total - avail;
        for (uint32_t i = 0; i < avail && n < cap - 160; i++) {
            const XSum *s = &g_xsum[(start + i) & (XSUM_CAP - 1u)];
            n += snprintf(out + n, cap - n,
                "%s{\"f\":%u,\"cr\":%u,\"a954\":%u,\"dmax\":%d,\"stk\":%u,\"cyc\":%llu}",
                i ? "," : "", s->frame, s->crossings, s->a954, s->depth_max,
                s->stk_max_kb, (unsigned long long)s->cyc_advanced);
        }
    }
    n += snprintf(out + n, cap - n, "],\n");

    /* per-crossing detail, CENTERED on the focus frame (trip frame, or the current
     * frame when live): dump from ~24 crossings before the focus frame's first
     * crossing forward, so the transition (last normal frame -> runaway onset) is
     * always captured regardless of ring fill. */
    n += snprintf(out + n, cap - n, "    \"detail\": [");
    {
        static const char *opn[] = {"JAL","JALR","JR","J","DD","BR"};
        uint64_t total = g_xdet_seq;
        uint32_t avail = total < XDET_CAP ? (uint32_t)total : XDET_CAP;
        uint64_t base  = total - avail;             /* oldest live entry */
        uint32_t focus = s_xf_frame;
        uint32_t first = avail;                     /* first index of the focus frame */
        for (uint32_t i = 0; i < avail; i++) {
            if (g_xdet[(base + i) & (XDET_CAP - 1u)].frame == focus) { first = i; break; }
        }
        uint32_t starto = (first == avail) ? (avail > 24u ? avail - 24u : 0u)
                                           : (first > 24u ? first - 24u : 0u);
        int firstdump = 1;
        for (uint32_t i = starto; i < avail && n < cap - 240; i++) {
            const XDetail *e = &g_xdet[(base + i) & (XDET_CAP - 1u)];
            n += snprintf(out + n, cap - n,
                "%s{\"f\":%u,\"cyc\":%llu,\"op\":\"%s\",\"site\":%u,\"src\":\"0x%08X\","
                "\"tgt\":\"0x%08X\",\"ds\":\"0x%08X\",\"sp\":\"0x%08X\",\"ra\":\"0x%08X\","
                "\"d\":%u,\"stk\":%u,\"ist\":\"0x%X\",\"imk\":\"0x%X\"}",
                firstdump ? "" : ",", e->frame, (unsigned long long)e->cycle,
                e->op < 6 ? opn[e->op] : "?", e->site, e->src_pc, e->target, e->ds_insn,
                e->sp, e->ra, e->depth, e->stk_kb, e->istat, e->imask);
            firstdump = 0;
        }
    }
    n += snprintf(out + n, cap - n, "]\n  }");
    return n;
}

/* Enter compiled code from the interpreter for a guest control transfer. Returns
 * like psx_dispatch_game_compiled (1 = handled, OR a stack-watermark bail was
 * surfaced; 0 = not a compiled target — caller falls through to overlay/dirty/
 * nonlocal paths). On surface: cpu->pc = target and g_psx_call_bail set.
 *
 * Only present when a game dispatch table is linked (PSX_HAS_GAME_DISPATCH).
 * In the BIOS-only build psx_dispatch_game_compiled does not exist, and the
 * two callers below are themselves gated behind the same macro. */
#ifdef PSX_HAS_GAME_DISPATCH
static int interp_enter_compiled(CPUState *cpu, uint32_t target) {
    if (target == 0x8001A954u) site_note(&g_site_interp);
    /* Decline when the target page no longer matches the static game image.
     * Returning 0 lets the JAL/JALR handler fall through to local-flow interp
     * of the live RAM bytes instead of running stale compiled code. */
    if (!psx_game_text_native_ok(target)) return 0;
    if (psx_mixed_owner_enabled()
        && interp_host_stack_used() > psx_mixed_stack_watermark()) {
        cpu->pc = target;
        g_psx_call_bail = 1;
        return 1;
    }
    g_mixed_depth++;
    ls_func_enter(target, cpu);
    int prev_phase = g_exec_phase;
    g_exec_phase = 3;
    int r = psx_dispatch_game_compiled(cpu, target);
    g_exec_phase = prev_phase;
    ls_func_exit(target, cpu, r);
    g_mixed_depth--;
    return r;
}
#endif /* PSX_HAS_GAME_DISPATCH */

/* Execute ONE instruction at *pc on the given CPU state.  Returns:
 *   0 = continue (advance pc by 4)
 *   1 = control transferred OR unsupported opcode (caller checks
 *       g_unsupported_seen to distinguish).
 * Branches encode their delay slot themselves before returning 1. */
static int exec_one_fetched(CPUState *cpu, uint32_t pc, uint32_t insn,
                            uint32_t *next_pc_out);
static int exec_one(CPUState *cpu, uint32_t pc, uint32_t *next_pc_out) {
    return exec_one_fetched(cpu, pc, fetch_word(pc & 0x1FFFFFFFu), next_pc_out);
}

/* Forward: helper for delay-slot execution on jumps/branches. */
static void exec_delay_slot(CPUState *cpu, uint32_t pc) {
    /* Delay-slot instruction at pc must NOT be a control transfer.
     * Recursively interpret as a single non-branching instruction. */
    uint32_t ds_phys = pc & 0x1FFFFFFFu;
    uint32_t insn = fetch_word(ds_phys);
    uint32_t opc = op_field(insn);
    uint32_t fnt = funct_field(insn);
    /* Reject branches/jumps in delay slots — undefined on R3000A and our
     * static recompiler explicitly handles this case differently (the
     * fall-through fix from 2026-04-21).  In install stubs, delay slots
     * are always nop or simple arithmetic. */
    if (opc == 0x02 /*j*/ || opc == 0x03 /*jal*/ ||
        opc == 0x04 /*beq*/ || opc == 0x05 /*bne*/ ||
        opc == 0x06 /*blez*/ || opc == 0x07 /*bgtz*/ ||
        opc == 0x01 /*regimm*/ ||
        (opc == 0x00 && (fnt == 0x08 /*jr*/ || fnt == 0x09 /*jalr*/))) {
        (void)abort_unsupported(pc, insn, "control-transfer in delay slot");
        return;
    }
    uint32_t dummy_next = 0;
    (void)exec_one_fetched(cpu, pc, insn, &dummy_next);
    g_dirty_ram_insns_run++;
    /* CYCLE MODEL: the delay-slot instruction is a real retired R3000A instruction
     * and is charged its own per-instruction interlock INSIDE exec_one (top-of-fn
     * §1+deps+DO_LDS, or psx_cyc_load_* for a load delay slot) — so a branch+slot
     * pair costs both, matching hardware. No separate charge here. */
}

/* Load-delay shim around the decoder (see dirty_ram_ld_delay_flush above).
 * Wrapping here keeps every individual load case untouched: the inner decoder
 * still writes gpr[rt] eagerly, and we roll that write back by one instruction
 * so the delay-slot instruction observes the architecturally-correct value. */
static int exec_one_fetched_inner(CPUState *cpu, uint32_t pc, uint32_t insn,
                                  uint32_t *next_pc_out);

static int exec_one_fetched(CPUState *cpu, uint32_t pc, uint32_t insn,
                            uint32_t *next_pc_out) {
    /* A load's writeback becomes visible to the instruction AFTER its delay
     * slot: load at N, hidden from N+1, visible from N+2. s_ld_pend_age tracks
     * that: 0 = armed by the instruction just executed, 1 = the delay slot has
     * now run, so retire it before executing anything else.
     *
     * Doing the retire HERE (rather than after the slot returns) is what makes
     * branches correct: a branch's own delay slot is executed nested inside
     * exec_one_fetched_inner, and re-enters this wrapper — by then age is 1, so
     * the slot correctly observes the loaded value. */
    if (s_ld_pend_armed && s_ld_pend_age != 0u) {
        s_ld_pend_armed = 0;
        if (s_ld_pend_rt != 0u) cpu->gpr[s_ld_pend_rt] = s_ld_pend_val;
        cpu->gpr[0] = 0;
    } else if (s_ld_pend_armed) {
        /* LWL/LWR exemption. `lwl rt,x` immediately followed by `lwr rt,y` is
         * THE unaligned-load idiom every MIPS compiler emits, and MIPS-I
         * explicitly permits the pair back-to-back: the second half merges with
         * the first's result rather than the pre-load register. Hiding the
         * pending write here would make the merge read a stale rt and silently
         * corrupt every unaligned load (it wedged Tomba 2 at boot). Retire the
         * write early so the partner sees it — this is forwarding, not a
         * shortcut around the delay slot. */
        const uint32_t nx_op = op_field(insn);
        const int is_lwlr = (nx_op == 0x22u || nx_op == 0x26u);
        if (is_lwlr && rt_field(insn) == s_ld_pend_rt) {
            s_ld_pend_armed = 0;
            if (s_ld_pend_rt != 0u) cpu->gpr[s_ld_pend_rt] = s_ld_pend_val;
            cpu->gpr[0] = 0;
        } else {
            s_ld_pend_age = 1u; /* this instruction IS the delay slot: stay hidden */
        }
    }

    /* op 0x20..0x26 = LB/LH/LWL/LW/LBU/LHU/LWR. LWC2 (GTE, 0x32) targets a COP2
     * register, not a GPR, so it needs no deferral here. */
    const uint32_t ld_op = op_field(insn);
    const uint32_t ld_rt = rt_field(insn);
    const uint32_t pc_phys = pc & 0x1FFFFFFFu;
    /* OpenBIOS executes cardfasttrack both from ROM and from its low-RAM
     * kernel copy (for example 0x3554..0x36D4). Its hand-written dependent
     * load requires the real R3000A value delay. Game-owned dirty RAM crosses
     * mixed compiled/interpreted boundaries that do not carry this pending
     * writeback yet, so preserve the historical eager-value contract there. */
    const int      in_bios_rom =
        pc_phys >= 0x1FC00000u && pc_phys < 0x1FC80000u;
    const int      in_bios_kernel_ram = pc_phys < 0x00010000u;
    const int      is_ld = (in_bios_rom || in_bios_kernel_ram) &&
                           (ld_op >= 0x20u && ld_op <= 0x26u) &&
                           (ld_rt != 0u);
    const uint32_t ld_before = is_ld ? cpu->gpr[ld_rt] : 0u;

    const int rv = exec_one_fetched_inner(cpu, pc, insn, next_pc_out);

    if (is_ld) {
        const uint32_t loaded = cpu->gpr[ld_rt];
        if (loaded != ld_before) {
            /* Back-to-back loads: retire the older writeback before reusing the
             * slot, otherwise its register write would be dropped entirely. */
            if (s_ld_pend_armed && s_ld_pend_rt != ld_rt && s_ld_pend_rt != 0u)
                cpu->gpr[s_ld_pend_rt] = s_ld_pend_val;
            /* Undo the eager write; it becomes visible one instruction later.
             * On a same-register conflict this naturally makes the LOAD win,
             * matching what the compiled backend emits for a dependent pair. */
            cpu->gpr[ld_rt] = ld_before;
            s_ld_pend_rt    = ld_rt;
            s_ld_pend_val   = loaded;
            s_ld_pend_age   = 0u;
            s_ld_pend_armed = 1;
            cpu->gpr[0]     = 0;
        }
    }
    return rv;
}

static int exec_one_fetched_inner(CPUState *cpu, uint32_t pc, uint32_t insn,
                                  uint32_t *next_pc_out) {
    exec_pc_table_record(pc);
    uint32_t opc  = op_field(insn);
    uint32_t rs   = rs_field(insn);
    uint32_t rt   = rt_field(insn);
    uint32_t rd   = rd_field(insn);
    uint32_t sh   = shamt_field(insn);
    uint32_t fnt  = funct_field(insn);
    int32_t  simm = simm16_field(insn);
    uint32_t imm  = imm16_field(insn);

    *next_pc_out = pc + 4;

#ifdef PSX_ENABLE_BLOCK_CYCLES
    /* Instruction FETCH cost (I-cache) — charged FIRST, before the §1 base, exactly
     * like Beetle ReadInstruction precedes the per-instruction base (cpu.cpp). HIT=+0,
     * KSEG1=+4, cached miss=+3+refill; a miss also clears the load give-back. */
    psx_icache_fetch_interp(cpu, pc);

    /* Per-instruction R3000A load-delay interlock (single-source: psx_cyc.h, shared
     * with both static emitters). §1 base + GPR_DEPRES + DO_LDS run HERE, before the
     * instruction body, so §1 precedes any muldiv/GTE deadline stall the body applies
     * (Beetle order). CPU loads (op 0x20-0x26) are skipped here — psx_cyc_load_* runs
     * their full interlock inside the body (and arms LDWhich=rt). This replaces the
     * old flat per-instruction psx_advance_cycles(psx_instr_base_cycles). */
    if (!(opc >= 0x20u && opc <= 0x26u))
        interp_cyc_step(cpu, psx_cyc_dep_res_mask(insn));
#endif

    /* Widescreen far-backdrop column PRELOAD (auto_backdrop). At a detected
     * window START/END finalize, force the loop bound (START->0 / END->sentinel)
     * so the generator's own clamps preload the WHOLE finite tile row into the
     * revealed 16:9 margin. The site is a move/addiu that writes exactly one GPR
     * and has no other effect, so substituting the value and advancing pc+4 is
     * complete. Gated on the runtime predicate first => 4:3 pays nothing. Each
     * rewrite is recorded to the always-on backdrop ring with the live extent
     * (s7), camera-X (scratchpad 0x176) and DL count for `ws_backdrop_ring`. */
    /* Opcode pre-filter FIRST: only addu/or/addi/addiu can be a rewrite site
     * (the detector below rewrites exactly one GPR-writing move/addiu). The old
     * order ran the ±512-byte site scan for EVERY interpreted instruction; with
     * the small direct-mapped cache thrashing, that was the mechanism of the
     * native-wide 16:9 CPU collapse (Tomba2 attract 12fps, stack-sampled). */
    if (((opc == 0x00u && (fnt == 0x21u || fnt == 0x25u)) ||
         opc == 0x09u || opc == 0x08u) &&
        psx_ws_backdrop_preload()) {
        int wcols = 0;
        int bk = ws_backdrop_site_kind(pc, &wcols);
        if (bk != WS_BD_NONE) {
            uint32_t dest = (opc == 0x00u && (fnt == 0x21u || fnt == 0x25u)) ? rd
                          : (opc == 0x09u || opc == 0x08u)                   ? rt
                          : 0xFFFFFFFFu;
            if (dest != 0xFFFFFFFFu) {
                /* orig = the instruction's normal result, so the widen shifts the
                 * real camera-tracked bound (move: gpr[src]; addiu: gpr[rs]+simm). */
                uint32_t orig = (opc == 0x00u)
                    ? ((rt == 0u) ? cpu->gpr[rs] : cpu->gpr[rt])
                    : (cpu->gpr[rs] + (uint32_t)simm);
                /* Tell psx_ws_backdrop_value the interp will record the rich ring
                 * entry (it skips its own note when this flag is set). */
                g_ws_bd_from_interp = 1;
                uint32_t finalv = psx_ws_backdrop_value(orig, bk == WS_BD_END, wcols);
                cpu->gpr[dest] = finalv;
                cpu->gpr[0] = 0;
                /* Rich snapshot for the ring: s7 (gpr[23]) = byte tile-row extent;
                 * s0 (gpr[16]) = DL object, count byte at +3; camera-X = scratchpad
                 * halfword 0x176. */
                {
                    int      extent = (int)(int16_t)cpu->gpr[23];
                    int      camx   = (int)(int16_t)(cpu->read_word(0x1F800174u) >> 16);
                    uint32_t s0a    = cpu->gpr[16] + 3u;
                    int      count  = (int)((cpu->read_word(s0a & ~3u) >> ((s0a & 3u) * 8u)) & 0xFFu);
                    /* base = s4 (gpr[20]) = backdrop data ptr (extent@+0, table@+4);
                     * dl = s0 (gpr[16]) = the ordering-table object. */
                    uint32_t a1 = cpu->gpr[20];
                    psx_ws_backdrop_ring_note(pc, bk, wcols, orig, finalv, extent, camx,
                                              count, a1, cpu->gpr[16]);
                    /* Publish the backdrop structure's address range so the GL
                     * 2D-stretch gate can match tile prims by gp0_cmd_source_addr.
                     * Tiles live at a1 + table[col]; bound by the LAST table entry
                     * (table is ascending) + packet slack. */
                    if (extent > 0 && extent <= 256) {
                        extern uint32_t g_ws_backdrop_lo, g_ws_backdrop_hi;
                        uint32_t tbl_last = cpu->read_word(a1 + 4u + (uint32_t)(extent - 1) * 4u);
                        g_ws_backdrop_lo = a1;
                        g_ws_backdrop_hi = a1 + tbl_last + 0x400u;
                    }
                }
                return 0;
            }
        }
    }

    switch (opc) {
    case 0x00: /* SPECIAL */
        switch (fnt) {
        case 0x00: { /* SLL rd, rt, sh (also nop when all fields are 0) */
            uint32_t a = cpu->gpr[rt];
            cpu->gpr[rd] = a << sh;
            psx_pgxp_alu(cpu, insn, cpu->gpr[rd], a, sh);
            cpu->gpr[0] = 0;
            return 0;
        }
        case 0x02: { /* SRL */
            uint32_t a = cpu->gpr[rt];
            cpu->gpr[rd] = a >> sh;
            psx_pgxp_alu(cpu, insn, cpu->gpr[rd], a, sh);
            cpu->gpr[0] = 0;
            return 0;
        }
        case 0x03: { /* SRA */
            uint32_t a = cpu->gpr[rt];
            cpu->gpr[rd] = (uint32_t)((int32_t)a >> sh);
            psx_pgxp_alu(cpu, insn, cpu->gpr[rd], a, sh);
            cpu->gpr[0] = 0;
            return 0;
        }
        case 0x04: { /* SLLV */
            uint32_t a = cpu->gpr[rt], b = cpu->gpr[rs];
            cpu->gpr[rd] = a << (b & 31);
            psx_pgxp_alu(cpu, insn, cpu->gpr[rd], a, b);
            cpu->gpr[0] = 0;
            return 0;
        }
        case 0x06: { /* SRLV */
            uint32_t a = cpu->gpr[rt], b = cpu->gpr[rs];
            cpu->gpr[rd] = a >> (b & 31);
            psx_pgxp_alu(cpu, insn, cpu->gpr[rd], a, b);
            cpu->gpr[0] = 0;
            return 0;
        }
        case 0x07: { /* SRAV */
            uint32_t a = cpu->gpr[rt], b = cpu->gpr[rs];
            cpu->gpr[rd] = (uint32_t)((int32_t)a >> (b & 31));
            psx_pgxp_alu(cpu, insn, cpu->gpr[rd], a, b);
            cpu->gpr[0] = 0;
            return 0;
        }
        case 0x08: { /* JR rs */
            uint32_t target = cpu->gpr[rs];
            if (target & 3) return interp_exception(cpu, 4, target, pc);  /* LoadAddressError */
            exec_delay_slot(cpu, pc + 4);
            cosim_exec_one_transfer_hook(pc + 4);
            /* crossing (if target is compiled) is counted at the block-loop
             * tail-transfer site (interp_enter_compiled, §18) — not here, to
             * avoid double-counting J/JR-to-compiled. */
            cpu->pc = target;
            return 1;
        }
        case 0x09: { /* JALR rd, rs */
            uint32_t target = cpu->gpr[rs];
            if (target & 3) return interp_exception(cpu, 4, target, pc);  /* LoadAddressError */
            uint32_t return_pc = pc + 8;
            cpu->gpr[rd ? rd : 31] = return_pc;
            PGXP_GPR_WRITE(rd ? rd : 31);
            cpu->gpr[0] = 0;
            exec_delay_slot(cpu, pc + 4);
            cosim_exec_one_transfer_hook(pc + 4);
            uint32_t site_sp = cpu->gpr[29];  /* call contract: sp at the call */
#ifndef PSX_NO_DEBUG_TOOLS
            xprobe_event(pc, XOP_JALR, XSITE_INTERP, target,
                         fetch_word((pc + 4) & 0x1FFFFFFFu), site_sp, cpu->gpr[31], 1);
#endif
            uint32_t _cr = callret_begin(cpu, pc, target);   /* call-resolution ring */
#define CRET(code, rv) do { callret_end(_cr, cpu, (code)); return (rv); } while (0)
            if (g_precise_mode || g_ls_replay_active) { cpu->pc = target; CRET(CRES_PLAIN, 1); }  /* slice / lockstep-replay: plain transfer, never execute the callee */
#ifdef PSX_HAS_GAME_DISPATCH
            cpu->pc = 0;
            if (interp_enter_compiled(cpu, target)) {
                if (g_psx_call_bail) CRET(CRES_EC_BAIL, 1);  /* wild unwind: cpu->pc = true target */
                if (cpu->pc != 0) CRET(CRES_EC_PC, 1);
                if (rd == 0 || rd == 31) {
                    if (psx_call_contract(cpu, return_pc, site_sp)) CRET(CRES_EC_CONTRACT, 1);
                }
                { int _r = dirty_ram_finish_call_return(cpu, return_pc, next_pc_out);
                  CRET(CRES_EC_RET | (_r ? 0x100u : 0u), _r); }
            }
#endif
            /* Native overlay candidates get the SAME call contract as
             * statically-compiled callees: run as a unit, resume at
             * return_pc. A bare pc-chain here loses the return obligation
             * when the callee runs natively (C return, pc==0) — the dispatch
             * loop unwinds past the suspended caller, leaking its frame. */
            {
                extern int overlay_loader_call_native(CPUState *cpu, uint32_t addr);
                cpu->pc = 0;
                if (overlay_loader_call_native(cpu, target)) {
                    if (g_psx_call_bail) CRET(CRES_OV_BAIL, 1);
                    if (cpu->pc != 0) CRET(CRES_OV_PC, 1);
                    if (rd == 0 || rd == 31) {
                        if (psx_call_contract(cpu, return_pc, site_sp)) CRET(CRES_OV_CONTRACT, 1);
                    }
                    { int _r = dirty_ram_finish_call_return(cpu, return_pc, next_pc_out);
                      CRET(CRES_OV_RET | (_r ? 0x100u : 0u), _r); }
                }
            }
            if (!is_local_dirty_target(target)) {
                int _r = dispatch_nonlocal_call(cpu, target, return_pc, next_pc_out);
                CRET(g_nl_exit_code | ((_r && g_nl_exit_code == CRES_NL_RET) ? 0x100u : 0u), _r);
            }
            cpu->pc = target;
            CRET(CRES_PCCHAIN, 1);
#undef CRET
        }
        case 0x0C: /* SYSCALL */
            cpu->pc = pc;
            psx_syscall(cpu, (insn >> 6) & 0xFFFFFu);
            return (cpu->pc != 0);
        case 0x0D: /* BREAK */
            psx_break(cpu, (insn >> 6) & 0xFFFFFu, pc);
            return 1;
        case 0x0F: /* SYNC */
            return 0;
        case 0x10: /* MFHI */
#ifdef PSX_ENABLE_BLOCK_CYCLES
            psx_muldiv_stall(cpu);   /* stall to mult/div completion (faithful) */
#endif
            cpu->gpr[rd] = cpu->hi;
            psx_pgxp_alu(cpu, insn, cpu->gpr[rd], cpu->hi, 0);
            cpu->gpr[0] = 0;
            return 0;
        case 0x11: /* MTHI */
            cpu->hi = cpu->gpr[rs];
            psx_pgxp_alu(cpu, insn, cpu->hi, cpu->hi, 0);
            return 0;
        case 0x12: /* MFLO */
#ifdef PSX_ENABLE_BLOCK_CYCLES
            psx_muldiv_stall(cpu);   /* stall to mult/div completion (faithful) */
#endif
            cpu->gpr[rd] = cpu->lo;
            psx_pgxp_alu(cpu, insn, cpu->gpr[rd], cpu->lo, 0);
            cpu->gpr[0] = 0;
            return 0;
        case 0x13: /* MTLO */
            cpu->lo = cpu->gpr[rs];
            psx_pgxp_alu(cpu, insn, cpu->lo, cpu->lo, 0);
            return 0;
        case 0x18: { /* MULT */
            int64_t r = (int64_t)(int32_t)cpu->gpr[rs] * (int64_t)(int32_t)cpu->gpr[rt];
            cpu->lo = (uint32_t)r;
            cpu->hi = (uint32_t)((uint64_t)r >> 32);
            psx_pgxp_muldiv(cpu, insn, cpu->hi, cpu->lo, cpu->gpr[rs], cpu->gpr[rt]);
#ifdef PSX_ENABLE_BLOCK_CYCLES
            psx_muldiv_set(cpu, psx_mult_latency_s(cpu->gpr[rs]));  /* completion deadline */
#endif
            return 0;
        }
        case 0x19: { /* MULTU */
            uint64_t r = (uint64_t)cpu->gpr[rs] * (uint64_t)cpu->gpr[rt];
            cpu->lo = (uint32_t)r;
            cpu->hi = (uint32_t)(r >> 32);
            psx_pgxp_muldiv(cpu, insn, cpu->hi, cpu->lo, cpu->gpr[rs], cpu->gpr[rt]);
#ifdef PSX_ENABLE_BLOCK_CYCLES
            psx_muldiv_set(cpu, psx_mult_latency_u(cpu->gpr[rs]));  /* completion deadline */
#endif
            return 0;
        }
        case 0x1A: { /* DIV */
            int32_t a = (int32_t)cpu->gpr[rs];
            int32_t b = (int32_t)cpu->gpr[rt];
            if (b == 0) {
                cpu->lo = (a < 0) ? 1u : 0xFFFFFFFFu;
                cpu->hi = (uint32_t)a;
            } else if ((uint32_t)a == 0x80000000u && b == -1) {
                cpu->lo = 0x80000000u;
                cpu->hi = 0;
            } else {
                cpu->lo = (uint32_t)(a / b);
                cpu->hi = (uint32_t)(a % b);
            }
            psx_pgxp_muldiv(cpu, insn, cpu->hi, cpu->lo, cpu->gpr[rs], cpu->gpr[rt]);
#ifdef PSX_ENABLE_BLOCK_CYCLES
            psx_muldiv_set(cpu, 37u);   /* DIV completion deadline (fixed) */
#endif
            return 0;
        }
        case 0x1B: /* DIVU */
            if (cpu->gpr[rt] == 0) {
                cpu->lo = 0xFFFFFFFFu;
                cpu->hi = cpu->gpr[rs];
            } else {
                cpu->lo = cpu->gpr[rs] / cpu->gpr[rt];
                cpu->hi = cpu->gpr[rs] % cpu->gpr[rt];
            }
            psx_pgxp_muldiv(cpu, insn, cpu->hi, cpu->lo, cpu->gpr[rs], cpu->gpr[rt]);
#ifdef PSX_ENABLE_BLOCK_CYCLES
            psx_muldiv_set(cpu, 37u);   /* DIVU completion deadline (fixed) */
#endif
            return 0;
        case 0x20: /* ADD - overflow traps are delegated if they occur. */
        case 0x21: { /* ADDU rd, rs, rt */
            uint32_t a = cpu->gpr[rs], b = cpu->gpr[rt];
            cpu->gpr[rd] = a + b;
            psx_pgxp_alu(cpu, insn, cpu->gpr[rd], a, b);
            cpu->gpr[0] = 0;
            return 0;
        }
        case 0x22: /* SUB - overflow traps are delegated if they occur. */
        case 0x23: { /* SUBU */
            uint32_t a = cpu->gpr[rs], b = cpu->gpr[rt];
            if (rs == 0 && psx_ws_is_cull_negsub_site(pc))
                cpu->gpr[rd] = 0u - b - (uint32_t)psx_ws_x_margin();
            else
                cpu->gpr[rd] = a - b;
            psx_pgxp_alu(cpu, insn, cpu->gpr[rd], a, b);
            cpu->gpr[0] = 0;
            return 0;
        }
        case 0x24: /* AND */
            cpu->gpr[rd] = cpu->gpr[rs] & cpu->gpr[rt];
            PGXP_GPR_WRITE(rd);
            cpu->gpr[0] = 0;
            return 0;
        case 0x25: { /* OR */
            uint32_t a = cpu->gpr[rs], b = cpu->gpr[rt];
            cpu->gpr[rd] = a | b;
            psx_pgxp_alu(cpu, insn, cpu->gpr[rd], a, b);
            cpu->gpr[0] = 0;
            return 0;
        }
        case 0x26: /* XOR */
            cpu->gpr[rd] = cpu->gpr[rs] ^ cpu->gpr[rt];
            PGXP_GPR_WRITE(rd);
            cpu->gpr[0] = 0;
            return 0;
        case 0x27: /* NOR */
            cpu->gpr[rd] = ~(cpu->gpr[rs] | cpu->gpr[rt]);
            PGXP_GPR_WRITE(rd);
            cpu->gpr[0] = 0;
            return 0;
        case 0x2A: /* SLT */
        {
            uint32_t vanilla =
                ((int32_t)cpu->gpr[rs] < (int32_t)cpu->gpr[rt]) ? 1u : 0u;
            uint32_t kept = vanilla;
            /* A widen site moves the bound; a keep site pins the verdict. A
             * site is only ever one of the three, but check widen first so a
             * config migrated from keep to widen cannot be served the pinned
             * answer by a stale entry. */
            if (!psx_ws_aspect_cone_site(cpu, pc, insn, vanilla, &kept) &&
                !psx_ws_cull_widen_site(pc, insn, cpu->gpr[rs], cpu->gpr[rt],
                                        0u, &kept))
                (void)psx_ws_cull_keep_site(pc, insn, vanilla, &kept);
            cpu->gpr[rd] = kept;
            PGXP_GPR_WRITE(rd);
            cpu->gpr[0] = 0;
            return 0;
        }
        case 0x2B: /* SLTU */
        {
            uint32_t vanilla = (cpu->gpr[rs] < cpu->gpr[rt]) ? 1u : 0u;
            uint32_t kept = vanilla;
            (void)psx_ws_cull_keep_site(pc, insn, vanilla, &kept);
            cpu->gpr[rd] = kept;
            PGXP_GPR_WRITE(rd);
            cpu->gpr[0] = 0;
            return 0;
        }
        default:
            return abort_unsupported(pc, insn, "SPECIAL funct");
        }
        break;

    case 0x02: { /* J target */
        uint32_t target = ((pc + 4) & 0xF0000000u) | (target26(insn) << 2);
        exec_delay_slot(cpu, pc + 4);
        cosim_exec_one_transfer_hook(pc + 4);
        /* crossing counted at the block-loop tail-transfer site (§18). */
        cpu->pc = target;
        return 1;
    }
    case 0x03: { /* JAL target */
        uint32_t target = ((pc + 4) & 0xF0000000u) | (target26(insn) << 2);
        uint32_t return_pc = pc + 8;
        cpu->gpr[31] = return_pc;
        PGXP_GPR_WRITE(31);
        exec_delay_slot(cpu, pc + 4);
        cosim_exec_one_transfer_hook(pc + 4);
        uint32_t site_sp = cpu->gpr[29];  /* call contract: sp at the call */
#ifndef PSX_NO_DEBUG_TOOLS
        int xw = g_xprobe_watch(target);  /* record RESOLUTION path for watched targets */
        xprobe_event(pc, XOP_JAL, XSITE_INTERP, target,
                     fetch_word((pc + 4) & 0x1FFFFFFFu), site_sp, cpu->gpr[31], 1);
#define XRES(code) do { if (xw) xprobe_event(pc, XOP_RES, (uint8_t)(code), target, \
                                             cpu->gpr[2], cpu->gpr[29], cpu->gpr[31], 1); } while (0)
#else
#define XRES(code) do { (void)(code); } while (0)
#endif
        if (g_precise_mode || g_ls_replay_active) { cpu->pc = target; return 1; }  /* slice / lockstep-replay: plain transfer, never execute the callee */
#ifdef PSX_HAS_GAME_DISPATCH
        cpu->pc = 0;
        if (interp_enter_compiled(cpu, target)) {
            if (g_psx_call_bail) { XRES(XRES_EC_BAIL); return 1; }  /* wild unwind: cpu->pc = true target */
            if (cpu->pc != 0)    { XRES(XRES_EC_PC); return 1; }
            if (psx_call_contract(cpu, return_pc, site_sp)) { XRES(XRES_EC_CONTRACT); return 1; }
            XRES(XRES_EC_RET);
            return dirty_ram_finish_call_return(cpu, return_pc, next_pc_out);
        }
#endif
        /* Native overlay candidates get the SAME call contract as statically-
         * compiled callees: run as a unit, resume at return_pc. A bare
         * pc-chain here loses the return obligation when the callee runs
         * natively (C return, pc==0) — the dispatch loop unwinds past the
         * suspended caller, leaking its frame (dwarf->overworld root cause:
         * F=0x800338A8's epilogue skipped, sp leaked 0x18 per entity). */
        {
            extern int overlay_loader_call_native(CPUState *cpu, uint32_t addr);
            cpu->pc = 0;
            if (overlay_loader_call_native(cpu, target)) {
                if (g_psx_call_bail) { XRES(XRES_OV_BAIL); return 1; }
                if (cpu->pc != 0)    { XRES(XRES_OV_PC); return 1; }
                if (psx_call_contract(cpu, return_pc, site_sp)) { XRES(XRES_OV_CONTRACT); return 1; }
                XRES(XRES_OV_RET);
                return dirty_ram_finish_call_return(cpu, return_pc, next_pc_out);
            }
        }
        if (!is_local_dirty_target(target)) {
            XRES(XRES_NONLOCAL);
            return dispatch_nonlocal_call(cpu, target, return_pc, next_pc_out);
        }
        if (!dirty_ram_word_looks_decodable(fetch_word(target & 0x1FFFFFFFu))) {
            XRES(XRES_UNDECODABLE);
            return dispatch_nonlocal_call(cpu, target, return_pc, next_pc_out);
        }
        XRES(XRES_PCCHAIN);
        cpu->pc = target;
        return 1;
#undef XRES
    }
    case 0x04: { /* BEQ rs, rt, simm */
        int taken = (cpu->gpr[rs] == cpu->gpr[rt]);
        exec_delay_slot(cpu, pc + 4);
        cosim_exec_one_transfer_hook(pc + 4);
        cpu->pc = taken ? (pc + 4 + (simm << 2)) : (pc + 8);
        return 1;
    }
    case 0x05: { /* BNE */
        int taken = (cpu->gpr[rs] != cpu->gpr[rt]);
        exec_delay_slot(cpu, pc + 4);
        cosim_exec_one_transfer_hook(pc + 4);
        cpu->pc = taken ? (pc + 4 + (simm << 2)) : (pc + 8);
        return 1;
    }
    case 0x06: { /* BLEZ */
        int taken = ((int32_t)cpu->gpr[rs] <= 0);
        exec_delay_slot(cpu, pc + 4);
        cosim_exec_one_transfer_hook(pc + 4);
        cpu->pc = taken ? (pc + 4 + (simm << 2)) : (pc + 8);
        return 1;
    }
    case 0x07: { /* BGTZ */
        int taken = ((int32_t)cpu->gpr[rs] > 0);
        exec_delay_slot(cpu, pc + 4);
        cosim_exec_one_transfer_hook(pc + 4);
        cpu->pc = taken ? (pc + 4 + (simm << 2)) : (pc + 8);
        return 1;
    }
    case 0x01: { /* REGIMM: BLTZ/BGEZ/BLTZAL/BGEZAL by rt field */
        int taken;
        switch (rt) {
        case 0x00: /* BLTZ */
            /* Widescreen render-funnel LEFT-edge widen (auto_screen_x): a
             * classified funnel bltz rejects only past the revealed margin.
             * Identity at 4:3 (margin 0). Gated per-game, cheap cached scan. */
            if (psx_ws_auto_cull_on() && ws_cull_bltz_site(pc))
                taken = psx_ws_cull_bltz(cpu->gpr[rs]);
            else
                taken = ((int32_t)cpu->gpr[rs] <  0);
            break;
        case 0x01: /* BGEZ */    taken = ((int32_t)cpu->gpr[rs] >= 0); break;
        case 0x10: /* BLTZAL */  taken = ((int32_t)cpu->gpr[rs] <  0);
                                  cpu->gpr[31] = pc + 8;
                                  PGXP_GPR_WRITE(31); break;
        case 0x11: /* BGEZAL */  taken = ((int32_t)cpu->gpr[rs] >= 0);
                                  cpu->gpr[31] = pc + 8;
                                  PGXP_GPR_WRITE(31); break;
        default: return abort_unsupported(pc, insn, "REGIMM rt");
        }
        exec_delay_slot(cpu, pc + 4);
        cosim_exec_one_transfer_hook(pc + 4);
        cpu->pc = taken ? (pc + 4 + (simm << 2)) : (pc + 8);
        return 1;
    }
    case 0x08: /* ADDI rt, rs, simm — same as ADDIU, sans overflow trap (we don't model traps here) */
    {
        uint32_t widened = 0;
        uint32_t a = cpu->gpr[rs];
        if (psx_ws_angle_site(pc, insn, &widened))
            cpu->gpr[rt] = widened;
        else
            cpu->gpr[rt] = a + (uint32_t)simm
                         + (psx_ws_is_cull_bias_site(pc)
                                ? (uint32_t)psx_ws_activation_margin() : 0u);
        psx_pgxp_alu(cpu, insn, cpu->gpr[rt], a, (uint32_t)simm);
        cpu->gpr[0] = 0;
        return 0;
    }
    case 0x09: /* ADDIU rt, rs, simm */
    {
        uint32_t widened = 0;
        uint32_t a = cpu->gpr[rs];
        if (psx_ws_angle_site(pc, insn, &widened))
            cpu->gpr[rt] = widened;
        else
            cpu->gpr[rt] = a + (uint32_t)simm
                         + (psx_ws_is_cull_bias_site(pc)
                                ? (uint32_t)psx_ws_activation_margin() : 0u);
        psx_pgxp_alu(cpu, insn, cpu->gpr[rt], a, (uint32_t)simm);
        cpu->gpr[0] = 0;
        return 0;
    }
    case 0x0A: /* SLTI */
    {
        uint32_t vanilla = ((int32_t)cpu->gpr[rs] < simm) ? 1u : 0u;
        uint32_t kept = vanilla;
        uint32_t widened = 0;
        if (psx_ws_aspect_cone_site(cpu, pc, insn, vanilla, &kept))
            cpu->gpr[rt] = kept;
        else if (psx_ws_cull_keep_site(pc, insn, vanilla, &kept))
            cpu->gpr[rt] = kept;
        else if (psx_ws_is_cull_depth_site(pc))
            cpu->gpr[rt] = ((int32_t)cpu->gpr[rs] < psx_ws_depth_bound(simm)) ? 1u : 0u;
        /* Widescreen render-funnel RIGHT-edge widen (auto_screen_x) for the
         * signed min/max funnel idiom (`slti v, minSX, W`) — the paired left
         * edge is the bltz above. Identity at 4:3 (margin 0). */
        else if (psx_ws_cull_widen_site(pc, insn, cpu->gpr[rs], 0u, imm,
                                        &widened))
            cpu->gpr[rt] = widened;
        else if (psx_ws_is_cull_slti_lower_site(pc))
            cpu->gpr[rt] = (uint32_t)psx_ws_cull_slti_lower(
                cpu->gpr[rs], imm);
        else if (psx_ws_is_cull_slti_site(pc) ||
            (psx_ws_auto_cull_on() && psx_ws_is_cull_w_imm(imm) && ws_cull_site(pc)))
            cpu->gpr[rt] = (uint32_t)psx_ws_cull_slti(cpu->gpr[rs], imm);
        else
            cpu->gpr[rt] = ((int32_t)cpu->gpr[rs] < simm) ? 1u : 0u;
        PGXP_GPR_WRITE(rt);
        cpu->gpr[0] = 0;
        return 0;
    }
    case 0x0B: /* SLTIU */
    {
        uint32_t vanilla = (cpu->gpr[rs] < (uint32_t)simm) ? 1u : 0u;
        uint32_t kept = vanilla;
        /* Widescreen render-funnel cull widening (auto_screen_x): apply the
         * shared helper for a flagged render-cull site — it is byte-identical
         * to the vanilla compare at 4:3 (margin 0) and widens at 16:9, so the one
         * code path serves both aspects (no widescreen-specific caching). */
        if (psx_ws_cull_keep_site(pc, insn, vanilla, &kept))
            cpu->gpr[rt] = kept;
        else if (psx_ws_is_cull_depth_site(pc))
            cpu->gpr[rt] = (cpu->gpr[rs] <
                            (uint32_t)psx_ws_depth_bound(simm)) ? 1u : 0u;
        else if (psx_ws_is_cull_vxrange_site(pc))
            cpu->gpr[rt] = (uint32_t)psx_ws_cull_vxrange(cpu->gpr[rs], imm);
        else if (psx_ws_is_cull_range_site(pc)) {
            /* Explicit world-space classifier widen. The native emitter uses
             * the same bound transform for configured range_sites. */
            cpu->gpr[rt] = (cpu->gpr[rs] <
                            ((uint32_t)simm + 2u *
                             (uint32_t)psx_ws_activation_margin())) ? 1u : 0u;
        }
        else if (psx_ws_auto_cull_on() && psx_ws_is_cull_w_imm(imm) && ws_cull_site(pc))
            cpu->gpr[rt] = (uint32_t)psx_ws_cull_sltiu(cpu->gpr[rs], imm);
        else
            cpu->gpr[rt] = (cpu->gpr[rs] < (uint32_t)simm) ? 1u : 0u;
        PGXP_GPR_WRITE(rt);
        cpu->gpr[0] = 0;
        return 0;
    }
    case 0x0C: /* ANDI */
        cpu->gpr[rt] = cpu->gpr[rs] & imm;
        PGXP_GPR_WRITE(rt);
        cpu->gpr[0] = 0;
        return 0;
    case 0x0D: { /* ORI */
        uint32_t a = cpu->gpr[rs];
        cpu->gpr[rt] = a | imm;
        psx_pgxp_alu(cpu, insn, cpu->gpr[rt], a, imm);
        cpu->gpr[0] = 0;
        return 0;
    }
    case 0x0E: /* XORI */
        cpu->gpr[rt] = cpu->gpr[rs] ^ imm;
        PGXP_GPR_WRITE(rt);
        cpu->gpr[0] = 0;
        return 0;
    case 0x0F: /* LUI rt, imm */
        if (psx_ws_is_signed_x_bound_site(pc, insn))
            cpu->gpr[rt] = (uint32_t)psx_ws_player_x_bound((int32_t)(imm << 16));
        else
            cpu->gpr[rt] = imm << 16;
        psx_pgxp_alu(cpu, insn, cpu->gpr[rt], 0, 0);
        cpu->gpr[0] = 0;
        return 0;
    case 0x10: { /* COP0 */
        uint32_t cop_op = rs;
        if (cop_op == 0x00) { /* MFC0 — delayed load (Beetle: LDAbsorb=0, LDWhich=rt) */
#ifdef PSX_ENABLE_BLOCK_CYCLES
            cpu->ld_absorb = 0u;
            cpu->ld_which_t = (uint8_t)rt;
#endif
            cpu->gpr[rt] = cpu->cop0[rd];
            PGXP_GPR_WRITE(rt);
            cpu->gpr[0] = 0;
            return 0;
        }
        if (cop_op == 0x02) { /* CFC0 — identical to MFC0 on PSX */
#ifdef PSX_ENABLE_BLOCK_CYCLES
            cpu->ld_absorb = 0u;
            cpu->ld_which_t = (uint8_t)rt;
#endif
            cpu->gpr[rt] = cpu->cop0[rd];
            PGXP_GPR_WRITE(rt);
            cpu->gpr[0] = 0;
            return 0;
        }
        if (cop_op == 0x04) { /* MTC0 */
            uint32_t val = cpu->gpr[rt];
            if (rd == 13) {
                /* Cause register: only the software-interrupt pending bits
                 * [9:8] are writable; hardware IP [15:10], ExcCode [6:2] and
                 * BD [31] are read-only from software (psx-spx, matches
                 * PCSX-Redux/Beetle MTC0). */
                cpu->cop0[13] = (cpu->cop0[13] & ~0x0300u) | (val & 0x0300u);
            } else {
                cpu->cop0[rd] = val;
            }
            /* psxTestSWInts: after writing Status or Cause, check if a
             * software interrupt is now deliverable (Cause & Status & 0x0300
             * with Status.IEc set).  Matches PCSX-Redux's MTC0 path. */
            if ((rd == 12 /* Status */ || rd == 13 /* Cause */) &&
                (cpu->cop0[13] & cpu->cop0[12] & 0x0300u) &&
                (cpu->cop0[12] & 0x1u)) {
                g_dirty_safe_resume_pc = pc + 4;
                cpu->pc = pc + 4;
                psx_check_interrupts(cpu);
                g_dirty_safe_resume_pc = 0;
                return (cpu->pc != pc + 4);  /* transferred if exception taken */
            }
            return 0;
        }
        if (cop_op == 0x06) { /* CTC0 — identical to MTC0 on PSX */
            uint32_t val = cpu->gpr[rt];
            if (rd == 13) {
                cpu->cop0[13] = (cpu->cop0[13] & ~0x0300u) | (val & 0x0300u);
            } else {
                cpu->cop0[rd] = val;
            }
            if ((rd == 12 || rd == 13) &&
                (cpu->cop0[13] & cpu->cop0[12] & 0x0300u) &&
                (cpu->cop0[12] & 0x1u)) {
                g_dirty_safe_resume_pc = pc + 4;
                cpu->pc = pc + 4;
                psx_check_interrupts(cpu);
                g_dirty_safe_resume_pc = 0;
                return (cpu->pc != pc + 4);
            }
            return 0;
        }
        if (cop_op == 0x10 && fnt == 0x10) { /* RFE */
            uint32_t sr = cpu->cop0[12];
            cpu->cop0[12] = (sr & 0xFFFFFFF0u) | ((sr >> 2) & 0x0Fu);
            /* Backend contract parity (overlay_api.h v12; the emitter calls
             * this after every recompiled rfe): inside the synchronous
             * handler window this arms the host escape taken at the next
             * committed transfer. Without it an interpreted handler whose
             * post-RFE EPC lands in dirty RAM keeps interpreting flat inside
             * psx_check_interrupts' window — in_exception never clears, the
             * cycle-paced VBlank authority is vetoed forever, and the guest
             * livelocks in the kernel idle loop (dbg-build boot wedge). */
            psx_rfe_mark_escape();
            return 0;
        }
        return abort_unsupported(pc, insn, "COP0 op");
    }
    case 0x12: { /* COP2 / GTE */
        uint32_t cop_op = rs;
        /* Faithful GTE: any COP2 register access stalls to the pending command
         * completion deadline (gte_execute armed it via psx_gte_set). */
        if (cop_op == 0x00) { /* MFC2 — read: stall + give-back (Beetle) */
#ifdef PSX_ENABLE_BLOCK_CYCLES
            psx_gte_read(cpu, rt);
#endif
            cpu->gpr[rt] = gte_read_data(cpu, (uint8_t)rd);
            psx_pgxp_cop2(cpu, insn, cpu->gpr[rt], 0);
            cpu->gpr[0] = 0;
            return 0;
        }
        if (cop_op == 0x02) { /* CFC2 — read: stall + give-back (Beetle) */
#ifdef PSX_ENABLE_BLOCK_CYCLES
            psx_gte_read(cpu, rt);
#endif
            cpu->gpr[rt] = gte_read_ctrl(cpu, (uint8_t)rd);
            psx_pgxp_cop2(cpu, insn, cpu->gpr[rt], 0);
            cpu->gpr[0] = 0;
            return 0;
        }
        if (cop_op == 0x04) { /* MTC2 */
#ifdef PSX_ENABLE_BLOCK_CYCLES
            psx_gte_stall(cpu);
#endif
            gte_write_data(cpu, (uint8_t)rd, cpu->gpr[rt]);
            psx_pgxp_cop2(cpu, insn, cpu->gpr[rt], 0);
            return 0;
        }
        if (cop_op == 0x06) { /* CTC2 */
#ifdef PSX_ENABLE_BLOCK_CYCLES
            psx_gte_stall(cpu);
#endif
            gte_write_ctrl(cpu, (uint8_t)rd, cpu->gpr[rt]);
            return 0;
        }
        if (cop_op & 0x10) {
            gte_execute(cpu, insn & 0x1FFFFFFu);
            return 0;
        }
        return abort_unsupported(pc, insn, "COP2 op");
    }
    /* CPU loads: the full per-instruction R3000A interlock (§1 + GPR_DEPRES(rs) +
     * DO_LDS + ReadMemory timing + arm LDWhich=rt) lives in psx_cyc_load_*; exec_one's
     * top-of-function step is skipped for op 0x20-0x26 so it is not double-charged.
     * #ifndef PSX_ENABLE_BLOCK_CYCLES these still read the value via the uncharged
     * accessor (psx_cyc_load_* falls back to a plain read when cycles are off). */
    case 0x20: { /* LB rt, simm(rs) */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        cpu->gpr[rt] = (uint32_t)(int32_t)(int8_t)psx_cyc_load_byte(cpu, addr, rt, 1u << rs);
        psx_pgxp_load(cpu, insn, addr, cpu->gpr[rt]);
        cpu->gpr[0] = 0;
        return 0;
    }
    case 0x21: { /* LH */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        if (addr & 1) return interp_exception(cpu, 4, addr, pc);  /* LoadAddressError */
        cpu->gpr[rt] = (uint32_t)(int32_t)(int16_t)psx_cyc_load_half(cpu, addr, rt, 1u << rs);
        psx_pgxp_load(cpu, insn, addr, cpu->gpr[rt]);
        cpu->gpr[0] = 0;
        return 0;
    }
    case 0x22: { /* LWL */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        uint32_t word = psx_cyc_load_word(cpu, addr & ~3u, rt, 1u << rs);
        cpu->gpr[rt] = lwl_merge(addr, word, cpu->gpr[rt]);
        psx_pgxp_load(cpu, insn, addr, cpu->gpr[rt]);
        cpu->gpr[0] = 0;
        return 0;
    }
    case 0x23: { /* LW */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        if (addr & 3) return interp_exception(cpu, 4, addr, pc);  /* LoadAddressError */
        if (psx_ws_is_cull_plane_nx_site(pc))
            /* Side-plane normal-X: inverse-aspect scale while revealed. */
            cpu->gpr[rt] = (uint32_t)psx_ws_plane_nx(
                (int32_t)psx_cyc_load_word(cpu, addr, rt, 1u << rs));
        else if (psx_ws_is_cull_xclip_load_site(pc))
            /* Per-prim X-reject bound: INT32_MAX while revealed (gpu.c). */
            cpu->gpr[rt] = psx_ws_xclip_bound(psx_cyc_load_word(cpu, addr, rt, 1u << rs));
        else
            cpu->gpr[rt] = psx_cyc_load_word(cpu, addr, rt, 1u << rs);
        psx_pgxp_load(cpu, insn, addr, cpu->gpr[rt]);
        cpu->gpr[0] = 0;
        return 0;
    }
    case 0x24: { /* LBU */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        cpu->gpr[rt] = (uint32_t)psx_cyc_load_byte(cpu, addr, rt, 1u << rs);
        psx_pgxp_load(cpu, insn, addr, cpu->gpr[rt]);
        cpu->gpr[0] = 0;
        return 0;
    }
    case 0x25: { /* LHU */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        if (addr & 1) return interp_exception(cpu, 4, addr, pc);  /* LoadAddressError */
        cpu->gpr[rt] = (uint32_t)psx_cyc_load_half(cpu, addr, rt, 1u << rs);
        psx_pgxp_load(cpu, insn, addr, cpu->gpr[rt]);
        cpu->gpr[0] = 0;
        return 0;
    }
    case 0x26: { /* LWR */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        uint32_t word = psx_cyc_load_word(cpu, addr & ~3u, rt, 1u << rs);
        cpu->gpr[rt] = lwr_merge(addr, word, cpu->gpr[rt]);
        psx_pgxp_load(cpu, insn, addr, cpu->gpr[rt]);
        cpu->gpr[0] = 0;
        return 0;
    }
    case 0x28: { /* SB */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        g_debug_last_store_pc = pc;
        cpu->write_byte(addr, (uint8_t)cpu->gpr[rt]);
        psx_pgxp_store(cpu, insn, addr, cpu->gpr[rt] & 0xFFu);
        return 0;
    }
    case 0x29: { /* SH */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        if (addr & 1) return interp_exception(cpu, 5, addr, pc);  /* StoreAddressError */
        uint16_t val  = (uint16_t)cpu->gpr[rt];
        /* Widescreen backdrop screenX squash on the interpreter path: mirrors
         * the recompiler emit at [widescreen.backdrop] x_sites. Overlay code
         * (the parallax backdrop handlers) runs interpreted when no cache DLL
         * is loaded, so without this the squash never happens. Identity at 4:3
         * (psx_ws_backdrop_x gates on ws_active). */
        if (psx_ws_is_backdrop_site(pc))
            val = (uint16_t)psx_ws_backdrop_x((int16_t)val);
        g_debug_last_store_pc = pc;
        cpu->write_half(addr, val);
        psx_pgxp_store(cpu, insn, addr, val);
        return 0;
    }
    case 0x2A: { /* SWL */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        g_debug_last_store_pc = pc;
        interp_swl(cpu, addr, cpu->gpr[rt]);
        psx_pgxp_store(cpu, insn, addr, cpu->gpr[rt]);
        return 0;
    }
    case 0x2B: { /* SW */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        if (addr & 3) return interp_exception(cpu, 5, addr, pc);  /* StoreAddressError */
        g_debug_last_store_pc = pc;
        cpu->write_word(addr, cpu->gpr[rt]);
        psx_pgxp_store(cpu, insn, addr, cpu->gpr[rt]);
        return 0;
    }
    case 0x2E: { /* SWR */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        g_debug_last_store_pc = pc;
        interp_swr(cpu, addr, cpu->gpr[rt]);
        psx_pgxp_store(cpu, insn, addr, cpu->gpr[rt]);
        return 0;
    }
    case 0x32: { /* LWC2 — §1+DO_LDS charged by exec_one's top step (mask 0) */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
#ifdef PSX_ENABLE_BLOCK_CYCLES
        psx_gte_stall(cpu);   /* COP2 reg write stalls to GTE completion */
#endif
        {
            uint32_t lw2 = psx_cyc_lwc2_read(cpu, addr);
            gte_write_data(cpu, (uint8_t)rt, lw2);
            psx_pgxp_cop2(cpu, insn, lw2, addr);
        }
        return 0;
    }
    case 0x3A: { /* SWC2 */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
#ifdef PSX_ENABLE_BLOCK_CYCLES
        psx_gte_stall(cpu);   /* COP2 reg read stalls to GTE completion */
#endif
        g_debug_last_store_pc = pc;
        {
            uint32_t sw2 = gte_read_data(cpu, (uint8_t)rt);
            cpu->write_word(addr, sw2);
            gte_precision_store_word(addr, (uint8_t)rt);
            psx_pgxp_cop2(cpu, insn, sw2, addr);
        }
        return 0;
    }
    default:
        return abort_unsupported(pc, insn, "primary opcode");
    }
    return 0;
}

static int dirty_ram_dispatch_inner(CPUState* cpu, uint32_t addr, uint32_t stop_addr);

/* Public entry point.  Caller (psx_dispatch) has translated `addr` to a
 * KSEG-stripped form already in some cases, so accept any address and
 * mask. Returns 1 if interpretation handled the basic block; 0 if the
 * caller should fall back (e.g. unsupported opcode at the entry, page
 * not dirty).
 *
 * Thin wrapper that brackets the body with g_dirty_interp_active so the
 * event-timeline ring can tag events as INTERP vs STATIC. The EPC-sentinel
 * branch inside can longjmp out (not restoring here); psx_check_interrupts
 * clears the flag at its longjmp landing to cover that case. */
/* Always-on runaway-recursion guard. The host C stack mirrors the guest call
 * graph (each guest call = a nested psx_dispatch_call -> dirty_ram_dispatch C
 * frame), so an unbounded self-recursion — e.g. a bogus jump-table handler that
 * re-enters its own entity loop (issue #1 seesaw crash: v0=0x8001DFF8) — silently
 * overflows the host stack with no diagnostic (raw STATUS_STACK_OVERFLOW). Trip
 * well below overflow: dump full state (crash report carries dispatch_depth + the
 * dirty_block cycle that NAMES the recursing PC) and halt-and-serve, so the next
 * reproduction (whoever hits it) is captured cleanly + live-inspectable instead of
 * a bare SEH. Normal guest nesting is shallow (<~64); the default 256 sits far
 * above that and below overflow (~500+ frames on a 1 MB stack). Override via
 * PSX_RECURSION_LIMIT. One-shot. NOT a fix — the upstream bogus-pointer divergence
 * still needs the oracle; this converts a hard crash into a catchable, diagnosed
 * bail (Rule 15: make the failure observable). */
int dirty_ram_dispatch(CPUState* cpu, uint32_t addr, uint32_t stop_addr) {
    extern int g_psx_dispatch_depth;
    extern void psx_fatal_halt(const char *reason);
    /* High-mirror code PCs fold to the low 2 MiB only while still aliased.
     * Unique high pages (enhancement code) keep their real PC. */
    addr = psx_ram_canon_code_addr(addr);
    if (stop_addr != 0u)
        stop_addr = psx_ram_canon_code_addr(stop_addr);
#ifndef PSX_NO_DEBUG_TOOLS
    /* A0/B0/C0 kernel-vector stubs are runtime-written, so calls to them
     * land HERE, not in the static dispatcher — which meant the bioscall
     * ring (debug_server_trace_dispatch) never saw a single vector call on
     * a BIOS whose stubs stay dirty (OpenBIOS bring-up: ring total 0 while
     * the guest hammered B0). Feed the same observer from this path. */
    {
        uint32_t vphys = addr & 0x1FFFFFFFu;
        if (vphys == 0xA0u || vphys == 0xB0u || vphys == 0xC0u) {
            extern void debug_server_trace_dispatch(uint32_t func_addr);
            debug_server_trace_dispatch(vphys);
        }
    }
#endif
    static int  s_rec_guard = 0;
    static int  s_rec_limit = 0;
    if (s_rec_limit == 0) {
        const char *e = getenv("PSX_RECURSION_LIMIT");
        s_rec_limit = (e && *e) ? atoi(e) : 256;
        if (s_rec_limit < 32) s_rec_limit = 32;
    }
    if (!s_rec_guard && g_psx_dispatch_depth > s_rec_limit) {
        s_rec_guard = 1;   /* one-shot: first trip wins the report */
        psx_fatal_halt("dispatch recursion guard tripped — runaway self-call "
                       "(see dispatch_depth + dirty_block cycle for the recursing PC)");
    }
    /* Game-start detection for the dirty-RAM interpreter path.
     * When dispatch_count == 0 (all code runs interpreted), the native
     * dispatch path's fntrace_record() never fires, so widescreen/mouselook/
     * CD-speed-switch are never engaged.  This one-shot check closes the gap
     * with the SAME semantics as the native path: latch only on the exact
     * game entry PC.  A broader match (any phys >= 0x10000) fires during
     * BIOS boot — the shell/kernel run relocated RAM code above 0x10000 —
     * and the handoff's baseline/scratch clears then corrupt the boot
     * (observed: MoH SLUS-00974 garbage-jump/VBLANK-wedge, 2026-08-06). */
    fntrace_maybe_mark_game_started(cpu, addr);
    if (addr == 0x8001A954u)      site_note(&g_site_dd954);   /* loop head re-dispatch */
    else if (addr == 0x80046264u) site_note(&g_site_dd264);   /* loop tail re-dispatch */
    int prev = g_dirty_interp_active;
    g_dirty_interp_active = 1;
    int prev_phase = g_exec_phase;
    g_exec_phase = 1;
    int r = dirty_ram_dispatch_inner(cpu, addr, stop_addr);

    /* pc=0 producer tripwire: the dirty path reported "handled" yet published a
     * null PC — the abnormal-exit source we're hunting (MMX6 boot wedge). Latch
     * full context the first time; count every occurrence. */
    if (r == 1 && cpu->pc == 0u) {
        extern uint32_t g_async_rfe_resume_pc, g_dirty_safe_resume_pc;
        g_pczero_count++;
        /* Journal every occurrence (site 15): the dirty/overlay dispatch chain
         * reported handled yet published a null PC — at top level this IS the
         * abnormal exit. a = dispatched addr; d = occurrence count. */
        psx_pc0_journal_note(15u, cpu, addr,
                             (uint32_t)g_pczero_count | (g_dd_last_route << 28));
        if (!g_pczero_latched) {
            g_pczero_latched    = 1;
            g_pczero_addr       = addr;
            g_pczero_ra         = cpu->gpr[31];
            g_pczero_in_exc     = (uint32_t)psx_get_in_exception();
            g_pczero_async_rfe  = g_async_rfe_resume_pc;
            g_pczero_dirty_safe = g_dirty_safe_resume_pc;
        }
    }

    /* Dirty-RAM scheduling contract (Tomba 2 Whoopee-Camp softlock fix):
     * ANY path that advanced guest cycles must periodically expose the CPU to
     * the interrupt/event pump, regardless of RAM region. The local-dirty-flow
     * fast path (overlay region, phys >= OVERLAY_REGION_FLOOR) already pumps
     * psx_check_interrupts() every 4096 insns internally — but a LOW-RAM kernel
     * dirty loop (e.g. 0x2CA8 / 0xE10 / 0xB0 polling for VBlank) takes the
     * per-block-return path instead and re-dispatches through the CPS chain
     * WITHOUT ever surfacing to the top-level interrupt check. Result: VBlank is
     * never raised, i_stat stays 0, the wait-loop deadlocks (frame frozen,
     * total_checks frozen, dirty_ram_insns -> billions). OVERLAY_REGION_FLOOR may
     * decide HOW dirty code is dispatched; it must never decide whether time
     * exists. So pump here on a region-independent insn budget. This is a clean
     * poll boundary: the inner committed cpu->pc, the executed instruction and
     * its delay slot are fully retired, and psx_check_interrupts() clears
     * g_dirty_interp_active at its longjmp landing (same contract as the inner
     * local-flow pump), so a deliver-via-longjmp here is safe. */
    if (r == 1) {
        uint64_t now = g_dirty_ram_insns_run;
        uint64_t gap = now - s_last_dirty_irq_pump_insns;
        if (gap > g_dirty_pump_max_gap_insns) g_dirty_pump_max_gap_insns = gap;
        if (gap >= 4096u) {
            s_last_dirty_irq_pump_insns = now;
            g_dirty_pump_count++;
            uint32_t committed = cpu->pc;      /* block already retired; this is next PC */
            int prev_site = g_cosim_dirty_pump_site;
            g_cosim_dirty_pump_site = 6;
            g_dirty_safe_resume_pc = committed;
            psx_check_interrupts(cpu);   /* may exception-enter / longjmp */
            /* Frame-1997 fix: a game-driven ReturnFromException longjmp'd through the
             * handler and left cpu->pc=0. Here (outer pump, the block already returned)
             * that null PC would propagate up the trampoline to the OUTERMOST dispatch
             * and be read as a clean "execution completed" exit. Restore the committed
             * guest PC so the trampoline re-dispatches and the game resumes. (The
             * local-flow internal pump resumes via its own loop, so this only matters at
             * block-return pumps.) */
            if (cpu->pc == 0u && committed != 0u) {
                cpu->pc = committed;
                g_async_rfe_fire_count++;
            }
            g_dirty_safe_resume_pc = 0;
            g_cosim_dirty_pump_site = prev_site;
        }
    }

    g_dirty_interp_active = prev;
    g_exec_phase = prev_phase;
    return r;
}

/* ===== Cycle-budgeted precise event slicing (PRECISE_IRQ_SLICE.md) ===== */

/* Is a hardware interrupt deliverable to the guest at this exact instruction
 * boundary? Mirrors the gate in psx_check_interrupts: a pending+unmasked I_STAT
 * bit, COP0 IEc + IM2 set, and not already inside the exception handler. */
static int precise_irq_deliverable(CPUState *cpu) {
    extern uint32_t i_stat;
    uint32_t sr = cpu->cop0[12];
    /* COP0 software interrupts (CAUSE.IP0/IP1 & SR.IM0/IM1): guest-raised via
     * mtc0 to CAUSE, independent of the INTC line (see psx_check_interrupts). */
    uint32_t sw_pending = cpu->cop0[13] & sr & 0x0300u;
    if ((i_stat & i_mask) == 0 && sw_pending == 0) return 0;
    if (psx_get_in_exception()) return 0;
    if (!(sr & 0x1u))        return 0;   /* IEc: interrupts globally enabled */
    /* INTC needs IM2; a pending software interrupt is deliverable without it. */
    if (!(sr & (1u << 10)) && sw_pending == 0)  return 0;
    return 1;
}

/* Is `pc` a point the TOP-LEVEL dispatcher can re-enter? Clean (compiled) game
 * text is only re-enterable at a FUNCTION ENTRY: psx_dispatch_game_compiled is a
 * switch over entries and returns 0 for any mid-function PC; the dirty path then
 * sees a non-dirty page and also returns 0, which the top dispatch reads as PC=0
 * ("execution completed" — the deterministic precise-slice boot exit, root-caused
 * to a mid-function clean-text resume PC, PRECISE_IRQ_SLICE.md Task #4). Every
 * other PC class — BIOS ROM, dirty RAM (overlays / install-at-runtime stubs),
 * overlay candidates, the A0/B0/C0 kernel call vectors, scratchpad — is handled by
 * psx_dispatch / the dirty interp / the overlay loader, so it is dispatchable.
 * Therefore the precise slicer must never hand control back at a mid-function
 * clean-text PC: it keeps interpreting (exec_one handles arbitrary mid-function
 * flow) until cpu->pc satisfies this predicate. */
static int precise_pc_dispatchable(uint32_t pc) {
#ifdef PSX_HAS_GAME_DISPATCH
    uint32_t phys = pc & 0x1FFFFFFFu;
    if (psx_game_address_in_text(pc) && !dirty_ram_is_dirty(phys))
        return psx_game_is_function_entry(pc);
#endif
    (void)pc;
    return 1;
}

/* Run the guest per-instruction from cpu->pc through the interpreter, taking any
 * deliverable interrupt at the EXACT instruction boundary, until a safe resume
 * point. g_precise_mode makes exec_one step INTO jal/jalr callees per-instruction
 * (ignore native availability), so the slice is owned by the event deadline, not
 * the function boundary. ALL exits land on a control-transfer target — every
 * branch/jump target is dispatchable (compiled blocks end with cpu->pc=target;
 * return), whereas an arbitrary mid-block PC is not — so after taking a mid-block
 * IRQ we keep interpreting to the next transfer before handing back to compiled.
 * `bcyc` = originating block's cycle budget; `deadline_entry` = entered because an
 * event is due within bcyc (vs a side-effect-only block, which runs one block). */
static void psx_run_precise(CPUState *cpu, uint32_t bcyc, int deadline_entry) {
    int prev_precise = g_precise_mode;
    int prev_active  = g_dirty_interp_active;
    int prev_phase   = g_exec_phase;
    g_precise_mode = 1;
    g_dirty_interp_active = 1;
    g_exec_phase = 1;

    uint32_t pc = cpu->pc;
    g_slice_last_block    = pc;
    g_slice_last_first_pc = pc;
    g_slice_last_first_insn = fetch_word(pc & 0x1FFFFFFFu);
    g_slice_exit_pc = pc;
    g_slice_exit_reason = 0;
    g_slice_exit_iter = 0;
    g_slice_exit_dispatchable = 0;
    g_slice_exit_dirty = dirty_ram_is_dirty(pc & 0x1FFFFFFFu) ? 1u : 0u;
#ifdef PSX_HAS_GAME_DISPATCH
    g_slice_exit_in_text = psx_game_address_in_text(pc) ? 1u : 0u;
#else
    g_slice_exit_in_text = 0;
#endif
    g_slice_exit_want = 0;
    int irq_taken = 0;   /* one take per slice (avoid re-taking an unacked IRQ) */
    enum { MAX_PRECISE_INSNS = 200000 };
    for (int i = 0; i < MAX_PRECISE_INSNS; i++) {
        if (!irq_taken && precise_irq_deliverable(cpu)) {
            uint32_t committed = pc;
            extern uint32_t i_stat;
            g_slice_last_committed = committed;
            g_slice_last_istat = i_stat;
            g_slice_last_imask = i_mask;
            g_slice_last_sr    = cpu->cop0[12];
            cpu->pc = committed;
            int prev_site = g_cosim_dirty_pump_site;
            g_cosim_dirty_pump_site = 7;
            g_dirty_safe_resume_pc = committed;
            s_last_dirty_irq_pump_insns = g_dirty_ram_insns_run;
            psx_check_interrupts(cpu);
            if (cpu->pc == 0u && committed != 0u) {
                cpu->pc = committed;
                g_async_rfe_fire_count++;
            }
            g_dirty_safe_resume_pc = 0;
            g_cosim_dirty_pump_site = prev_site;
            g_slice_irq_taken++;
            irq_taken = 1;
            pc = cpu->pc ? cpu->pc : committed;
            cpu->pc = pc;
            if (precise_pc_dispatchable(cpu->pc)) {
                g_slice_exit_reason = 1;
                g_slice_exit_iter = (uint32_t)i;
                g_slice_exit_want = 1;
                break;
            }
            continue;
        }

        uint32_t next_pc = 0;
        g_unsupported_seen = 0;
        cosim_exec_one_begin();
        int transferred = exec_one(cpu, pc, &next_pc);  /* charges its own interlock */
        g_dirty_ram_insns_run++;
#ifdef PSX_COSIM
        if (!cosim_exec_one_did_hook()) { extern void cosim_instr(uint32_t); cosim_instr(pc); }
#endif
        if (g_unsupported_seen) {
            /* Valid compiled code always decodes; if not, hand the committed PC to
             * the dispatcher rather than abort the slice. */
            g_unsupported_seen = 0;
            pc = transferred ? cpu->pc : next_pc;
            g_slice_exit_reason = 2;
            g_slice_exit_iter = (uint32_t)i;
            g_slice_exit_want = 0;
            break;
        }
        uint32_t committed = transferred ? cpu->pc : next_pc;

        /* `want_exit`: the slice has done its job and would like to hand back. But
         * it may ONLY return control at a dispatchable PC (precise_pc_dispatchable)
         * — a mid-function clean-text resume PC is unhandleable by the top dispatch
         * (root cause of the deterministic boot PC=0). When we want to exit but the
         * committed PC is mid-function clean text, keep interpreting (exec_one runs
         * arbitrary mid-function flow) until control lands on a real entry / leaves
         * clean game text. */
        int want_exit = 0;

        /* Exact take-point: an interrupt deliverable on THIS instruction's cycle is
         * taken before the next instruction retires — once per slice. Re-checking
         * after a take re-fires an IRQ the handler has not acked yet (an
         * 8-takes-in-11-insns storm), so gate on !irq_taken. */
        if (!irq_taken && precise_irq_deliverable(cpu)) {
            extern uint32_t i_stat;
            g_slice_last_committed = committed;
            g_slice_last_istat = i_stat;
            g_slice_last_imask = i_mask;
            g_slice_last_sr    = cpu->cop0[12];
            cpu->pc = committed;
            int prev_site = g_cosim_dirty_pump_site;
            g_cosim_dirty_pump_site = 7;
            g_dirty_safe_resume_pc = committed;   /* real EPC for exception entry */
            s_last_dirty_irq_pump_insns = g_dirty_ram_insns_run;
            psx_check_interrupts(cpu);            /* takes it; runs handler; restores GPRs */
            g_dirty_safe_resume_pc = 0;
            g_cosim_dirty_pump_site = prev_site;
            g_slice_irq_taken++;
            irq_taken = 1;
            committed = cpu->pc ? cpu->pc : committed;
            want_exit = 1;
        }

        pc = committed;
        cpu->pc = committed;

        if (g_psx_call_bail) {
            g_slice_exit_reason = 3;
            g_slice_exit_iter = (uint32_t)i;
            g_slice_exit_want = want_exit ? 1u : 0u;
            break;   /* wild unwind: hand cpu->pc to the dispatcher */
        }

        if (irq_taken) {
            want_exit = 1;            /* after the take, leave as soon as it is safe */
        } else if (transferred) {
            want_exit = 1;
            /* else still imminent — keep slicing across this boundary */
        }

        /* Hand back ONLY at a dispatchable PC. Otherwise (mid-function clean text)
         * keep interpreting until one is reached. */
        if (want_exit && precise_pc_dispatchable(cpu->pc)) {
            g_slice_exit_reason = 1;
            g_slice_exit_iter = (uint32_t)i;
            g_slice_exit_want = 1;
            break;
        }
    }
    if (g_slice_exit_reason == 0) {
        g_slice_exit_reason = 4;
        g_slice_exit_iter = MAX_PRECISE_INSNS;
    }

    cpu->pc = pc;
    g_slice_exit_pc = cpu->pc;
    g_slice_exit_dispatchable = precise_pc_dispatchable(cpu->pc) ? 1u : 0u;
    g_slice_exit_dirty = dirty_ram_is_dirty(cpu->pc & 0x1FFFFFFFu) ? 1u : 0u;
#ifdef PSX_HAS_GAME_DISPATCH
    g_slice_exit_in_text = psx_game_address_in_text(cpu->pc) ? 1u : 0u;
#else
    g_slice_exit_in_text = 0;
#endif
    g_precise_mode = prev_precise;
    g_dirty_interp_active = prev_active;
    g_exec_phase = prev_phase;
}

/* Precise-slice gate (PARKED default OFF). Hot callers use the cpu_state.h
 * inline which returns 0 when this is 0 — no out-of-line call. Opt in with
 * PSX_PRECISE_SLICE=1 (same binary A/B). */
int g_psx_precise_slice = 0;

void psx_precise_slice_init_from_env(void) {
    const char *e = getenv("PSX_PRECISE_SLICE");
    g_psx_precise_slice = (e && e[0] == '1') ? 1 : 0;
}

/* Block-leader slice guard impl. Emitted code calls the inline wrapper
 * psx_slice_block(); overlay CPS callbacks point here directly.
 * Returns 1 if it sliced (ran the block — and possibly more — through the precise
 * interpreter and left cpu->pc at a dispatchable resume point; the caller MUST
 * `return` so its compiled body does not re-execute the same instructions).
 * Returns 0 if the whole block is provably safe to run as fast compiled C. */
int psx_slice_block_impl(CPUState *cpu, uint32_t block_addr, uint32_t bcyc, int side_effects) {
    /* PARKED (PRECISE_IRQ_SLICE.md): precise take-point slicing is a later
     * correctness upgrade, NOT the current FMV blocker (that is the -8 cycle
     * drift / faithful per-instruction cycle model — see CLAUDE.md Rule -1). */
    if (!g_psx_precise_slice) return 0;

    /* No nested slicing: a handler dispatched from inside precise-mode, and any
     * block executed while in_exception, run compiled (interrupts are gated during
     * exception handling anyway). Keeps re-entrancy structurally impossible. */
    if (g_precise_mode || psx_get_in_exception()) return 0;

    static int s_slice_always = -1;
    static int s_slice_margin = -1;
    if (s_slice_always < 0) {
        const char *e = getenv("PSX_PRECISE_ALWAYS");
        s_slice_always = (e && e[0] == '1') ? 1 : 0;
    }
    if (s_slice_margin < 0) {
        const char *e = getenv("PSX_PRECISE_MARGIN");
        s_slice_margin = (e && *e) ? atoi(e) : 0;
        if (s_slice_margin < 0) s_slice_margin = 0;
    }

    uint32_t deadline = cycles_to_next_event();
    uint32_t budget = bcyc + (uint32_t)s_slice_margin;
    if (budget < bcyc) budget = 0xFFFFFFFFu;
    int entry_deliverable = precise_irq_deliverable(cpu);
    int has_deadline = s_slice_always || entry_deliverable || (deadline <= budget);
    if (!has_deadline && !side_effects) return 0;   /* fast path: no event in this block */

    g_slice_entry_deliverable = (uint32_t)entry_deliverable;
    g_slice_fired++;
    cpu->pc = block_addr;
    psx_run_precise(cpu, bcyc, has_deadline);
    return 1;
}

static int dirty_ram_dispatch_inner(CPUState* cpu, uint32_t addr, uint32_t stop_addr) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    int clean_game_text_miss = 0;

    if (addr == 0x80000048u) {
        g_dd_last_route = 1;
        g_sentinel_reach_dirty++;
        if (!psx_get_in_exception()) g_sentinel_reach_traps++; /* reuse: in_exc==0 dirty reaches */
        g_sentinel_reach_async = g_async_rfe_resume_pc;
        psx_pc0_journal_note(PSX_PC0J_DIRTY_SENTINEL, cpu, addr,
                             (psx_get_in_exception() ? 1u : 0u) |
                             (g_async_rfe_resume_pc ? 2u : 0u));
        cpu->pc = 0;
        if (psx_get_in_exception()) {
            psx_exception_longjmp(); /* does not return */
        }
        /* Async ReturnFromException (Tomba 2 frame-1997 fix): game-installed handler
         * RFE'd outside our exception window. Resume at the latched real guest PC
         * instead of returning pc=0 (which the top dispatch reads as a clean exit). */
        if (g_async_rfe_resume_pc != 0u) {
            g_async_rfe_fire_count++;
            cpu->pc = g_async_rfe_resume_pc;
            return 1;
        }
        return 1;
    }

#ifdef PSX_HAS_GAME_DISPATCH
#ifndef PSX_NO_DEBUG_TOOLS
    xprobe_event(cpu->gpr[31], XOP_DD, XSITE_DD, addr, 0u, cpu->gpr[29], cpu->gpr[31], 0);
#endif
    /* Run the statically-compiled game function only while the target is still
     * native-safe. Dirty overlay pages and pages whose text bytes diverged from
     * the original EXE image fall through to interpret the live RAM bytes. */
    if (psx_game_text_native_ok(addr)) {
        g_mixed_depth++;
        {
            ls_func_enter(addr, cpu);
            int prev_phase = g_exec_phase;
            g_exec_phase = 3;
            g_dd_last_route = 2;
            int _gc = psx_dispatch_game_compiled(cpu, addr);
            g_exec_phase = prev_phase;
            ls_func_exit(addr, cpu, _gc);
            g_mixed_depth--;
            if (_gc) return 1;
        }
        clean_game_text_miss = psx_game_address_in_text(addr) ? 1 : 0;
    } else if (psx_game_address_in_text(addr)) {
        /* RAM at a game-text address diverged from the static EXE image
         * (runtime-relocated / overlaid / self-modified code the compiled
         * static function no longer reflects). The live RAM is the truth:
         * fall through to INTERPRET it here rather than bail (line below) to
         * the shell shadow (normalize() -> shell ROM). Crash Bash relocates a
         * code page onto 0x30000 (inside the BIOS shell window); without this
         * its 0x30FF4 call diverged (native_ok=0) yet was not dirty, so the
         * interpreter bailed and normalize() shadowed it to dead shell ROM ->
         * unknown-dispatch abort. Marking it a clean game-text miss lets the
         * dirty interpreter execute the real RAM bytes. */
        clean_game_text_miss = 1;
    }
#endif

    /* B-2: statically-compiled overlay functions (generated/overlays_static.c).
     * Inert unless a game provides an overlays_static.c at build time. */
#ifdef PSX_HAS_OVERLAY_DISPATCH
    {
        extern int psx_overlay_dispatch(CPUState *cpu, uint32_t addr);
        g_dd_last_route = 3;
        if (psx_overlay_dispatch(cpu, addr)) return 1;
    }
#endif

    /* A-1: dynamically-loaded overlay DLL functions, checked before the
     * interpreter.  No-op (returns 0) until overlay_loader_init() runs, which
     * only happens when the overlay cache is enabled in config. */
    /* §5-E native↔interp fingerprint: capture entry register state for a
     * candidate overlay function, so native and interpreted runs can be diffed
     * by sequence. Additive only — no control-flow change. */
    extern int      overlay_loader_is_candidate(uint32_t phys);
    extern int      overlay_fp_enabled(void);
    extern void     overlay_regs_snap(uint32_t out[34], const CPUState *cpu);
    extern void     overlay_fp_log(uint32_t addr, const uint32_t *in_regs,
                                   const CPUState *cpu, int native);
    int      _ovfp = overlay_fp_enabled() &&
                     overlay_cache_window_contains(phys) &&
                     overlay_loader_is_candidate(phys);
    uint32_t _in_regs[34];
    if (_ovfp) {
        overlay_regs_snap(_in_regs, cpu);
        /* One-shot insn-ring freeze BEFORE the Nth dispatch of the watched
         * candidate — the ring tail then holds the pre-divergence window. */
        if (g_insn_freeze_addr && phys == g_insn_freeze_addr &&
            ++g_insn_freeze_count == g_insn_freeze_nth)
            g_insn_log_frozen = 1;
    }

    {
        extern int overlay_loader_dispatch(CPUState *cpu, uint32_t addr);
        g_dd_last_route = 4;
        if (overlay_loader_dispatch(cpu, addr)) {
            if (_ovfp) overlay_fp_log(addr, _in_regs, cpu, 1);
            return 1;
        }
    }
/* Every exit retires any deferred load writeback: once we hand control back to
 * compiled code (or the dispatch loop) nothing downstream knows a register
 * write is still owed, and the pipeline would have drained by then anyway. */
#define OV_FPLOG_RET1() do { g_dd_last_route = 5; dirty_ram_ld_delay_flush(cpu); if (_ovfp) overlay_fp_log(addr, _in_regs, cpu, 0); return 1; } while (0)

    if (!dirty_ram_is_dirty(phys) && !clean_game_text_miss) {
        /* Bulk host transfers can populate post-EXE executable RAM without
         * passing through the write hooks that mark dirty pages. A real
         * control transfer to a decodable word OUTSIDE the configured boot-EXE
         * text image is enough evidence to admit that word to the interpreter.
         * "Outside" is both sides of the text: a boot EXE that loads high
         * streams its gameplay overlays into the RAM below itself (Klonoa's
         * text is 0x180000-0x18B000, its overlays run from 0x10000+), and
         * gating on the floor alone rejected every one of those targets —
         * a JALR into a CD-DMA'd overlay page then fell through to
         * psx_unknown_dispatch and fail-fast exit(1). Data and invalid targets
         * still fail closed via the decodability check. */
        if (phys < (8u * 1024u * 1024u) &&
            phys_is_overlay_region(phys) &&
            dirty_ram_word_looks_decodable(fetch_word(phys))) {
            dirty_ram_mark_executable_range(phys, 4u);
        } else {
            return 0;
        }
    }

    /* Interp-pressure signal for variant-capture automation (step 2.8):
     * counts dispatches the interpreter actually handles inside a capture
     * window. The autocapture tick reads-and-resets this to decide whether
     * an unseen region variant is being interp-executed right now. */
    if (overlay_cache_window_contains(phys)) g_dirty_window_dispatches++;

    /* Reset soft-fail state at block entry. */
    g_unsupported_seen = 0;
    /* Overlay flow above the kernel window — kernel window stays per-block (see
     * is_local_dirty_target / phys_is_overlay_flow_region). Includes boot-text
     * pages overwritten by a runtime overlay (Tomba 2), not just [FLOOR, RAM). */
    int allow_local_dirty_flow = phys_is_overlay_flow_region(phys);

    /* Backend-invariant mod_function_entry hooks: generated code fires
     * psx_mod_function_entry at listed function entries, but a mod-patched
     * (dirty) page runs HERE instead and would silently skip them. Fire the
     * same hook on interp dispatch so the contract does not depend on which
     * backend executes the page. The runtime filters by exact address, so
     * this is a short scan over the (tiny) registered-plugin list. */
    {
        extern void psx_mod_function_entry(CPUState* cpu, uint32_t address);
        psx_mod_function_entry(cpu, addr);
    }

    /* Per-PC entry counter (visible via dirty_ram_stats). */
    DirtyRamPcEntry *pc_entry = pc_table_get_or_insert(phys);
    if (pc_entry) pc_entry->hits++;
    {
        uint32_t word = phys >> 2;
        g_dirty_ram_dispatch_pc_bitmap[word >> 5] |= 1u << (word & 31u);
    }

    /* External-entry attribution: when the previous interp run exited by
     * handing a target back to the dispatch loop, the very next dirty
     * dispatch at that target is the SAME logical execution continuing —
     * not a new entry. Anything else arrived from native code (a call or
     * a fresh dispatch) and is real interior-entry evidence for alias
     * seeding. */
    if (pc_entry && addr != g_dirty_interp_chain_target) pc_entry->entry_hits++;
    g_dirty_interp_chain_target = 0;

    /* Block-entry ring buffer — answers "who tried to JALR into this RAM
     * stub" by capturing cpu->gpr[31] (the caller's RA) at dispatch time.
     * Always-on; eviction keeps memory bounded. Honors the capture freeze. */
#ifndef PSX_NO_DEBUG_TOOLS
    if (!g_insn_log_frozen) {
        uint64_t s = g_dirty_ram_block_log_seq++;
        DirtyRamBlockLogEntry *e =
            &g_dirty_ram_block_log[s & (DIRTY_RAM_BLOCK_LOG_CAP - 1u)];
        e->seq    = s;
        e->target = addr;
        e->ra     = cpu->gpr[31];
        e->a0     = cpu->gpr[4];
        e->a1     = cpu->gpr[5];
        e->a2     = cpu->gpr[6];
        e->a3     = cpu->gpr[7];
        e->t0     = cpu->gpr[8];
        e->t1     = cpu->gpr[9];
        e->t2     = cpu->gpr[10];
        e->sp     = cpu->gpr[29];
        e->frame  = (uint32_t)s_frame_count;
    }
#endif

#ifndef PSX_NO_DEBUG_TOOLS
    if (debug_server_dirty_break_maybe_pause(addr, cpu)) {
        debug_server_wait_if_paused();
    }
#endif

    /* Run dirty code locally until it returns to compiled/non-dirty code.
     * Runtime-loaded overlays are larger than BIOS install stubs, so stopping
     * at every local branch burns the dispatch loop. */
    enum { MAX_INSNS_PER_DISPATCH = 1000000 };
    uint32_t pc = addr;
    uint32_t current_page = phys >> 12;
    int current_page_dirty = dirty_ram_is_dirty(phys);
    int insns_executed = 0;
#ifndef PSX_NO_DEBUG_TOOLS
    extern void debug_server_cyc_observe(uint32_t block_leader_phys);
#endif
    /* Async-interrupt latency fix. A guest wait loop that lives ENTIRELY in
     * dirty RAM (e.g. libcd's CdSync spin, as in Kula World) re-enters this
     * interpreter once per short (~7-insn) block and exits before the
     * per-invocation `(insns_executed & 0xFFF)` gate below can fire — so a
     * pending, IEc+IM2-enabled interrupt is never taken and the loop spins for
     * seconds while the CD IRQ that would set its wait-flag is never serviced.
     * When an IRQ is already deliverable, poll EVERY entry (guest-deterministic;
     * MotK post-FMV overlay wait @0x80076880 with latched I_STAT.VBlank forked
     * Win↔Linux on a host-only %%64 stride). Otherwise throttle on the global
     * invocation counter. psx_check_interrupts runs the handler and returns with
     * registers restored, so continuing the loop afterward is safe. */
    {
        static int s_entry_poll_enabled = -1;   /* DIAGNOSTIC toggle (590c236 x kind-30 escape regression hunt) */
        if (s_entry_poll_enabled < 0) {
            const char* e = getenv("PSX_DIRTY_ENTRY_POLL");
            s_entry_poll_enabled = (e && e[0] == '0') ? 0 : 1;
        }
        if (s_entry_poll_enabled) {
            extern uint32_t i_stat;
            uint32_t sr = cpu->cop0[12];
            int deliverable =
                ((i_stat & i_mask) != 0u) &&
                !psx_get_in_exception() &&
                (sr & 0x1u) != 0u &&
                (sr & (1u << 10)) != 0u;
            if (deliverable || (++s_interp_entry_poll & 0x3Fu) == 0) {
                cpu->pc = pc;
                s_last_dirty_irq_pump_insns = g_dirty_ram_insns_run;
                psx_check_interrupts(cpu);
                if (cpu->pc != 0u && !dirty_ram_same_pc(cpu->pc, pc)) {
                    /* Handler resumed elsewhere — surface to dispatch. */
                    g_dirty_ram_blocks_run++;
                    if (pc_entry) pc_entry->insns += (uint64_t)insns_executed;
                    g_dirty_interp_chain_target = cpu->pc;
                    OV_FPLOG_RET1();
                }
                if (cpu->pc == 0u)
                    cpu->pc = pc;
            }
        }
    }
    for (int i = 0; i < MAX_INSNS_PER_DISPATCH; i++) {
        uint32_t next_pc = 0;
#ifndef PSX_NO_DEBUG_TOOLS
        /* Interp-path cycle ruler: make every interpreted PC anchorable by
         * cyc_watch (parity with the compiled emitter's block-leader observe).
         * Early-returns when cyc_watch is disarmed → ~free in normal runs. */
        g_ls_dirty_observe = 1;
        debug_server_cyc_observe(pc & 0x1FFFFFFFu);
        g_ls_dirty_observe = 0;
#endif
#ifdef PSX_COSIM
        { extern void cosim_block(uint32_t); cosim_block(pc); }
#endif
        uint32_t insn = fetch_word(pc & 0x1FFFFFFFu);
#ifndef PSX_NO_DEBUG_TOOLS
        uint32_t before_s0 = cpu->gpr[16];
        uint32_t before_ra = cpu->gpr[31];
        /* Callee-smear tripwire: latch the first instruction in the
         * watched pc window whose execution changes $s3 (gpr19) — for a jalr
         * this spans the ENTIRE nested native callee, naming the callee that
         * returned with a clobbered callee-saved register. Armed via the
         * s3_smear_watch TCP command; zero-cost when disarmed. */
        extern uint32_t g_s3_smear_lo, g_s3_smear_hi;
        extern uint32_t g_s3_smear_pc, g_s3_smear_insn, g_s3_smear_old,
                        g_s3_smear_new, g_s3_smear_tgt, g_s3_smear_frame;
        extern int g_s3_smear_valid;
        uint32_t before_s3 = cpu->gpr[19];
#endif
        cosim_exec_one_begin();
        int transferred = exec_one_fetched(cpu, pc, insn, &next_pc);
#ifndef PSX_NO_DEBUG_TOOLS
        if (g_s3_smear_lo && !g_s3_smear_valid &&
            pc >= g_s3_smear_lo && pc < g_s3_smear_hi &&
            cpu->gpr[19] != before_s3 &&
            (g_s3_smear_excl == 0u || insn != g_s3_smear_excl)) {
            g_s3_smear_valid = 1;
            g_s3_smear_pc    = pc;
            g_s3_smear_insn  = insn;
            g_s3_smear_old   = before_s3;
            g_s3_smear_new   = cpu->gpr[19];
            /* for jr/jalr the smearing callee = rs at the call site */
            g_s3_smear_tgt   = cpu->gpr[(insn >> 21) & 0x1Fu];
            g_s3_smear_frame = (uint32_t)s_frame_count;
            extern int g_insn_log_frozen;
            g_insn_log_frozen = 1;   /* freeze the insn ring at the smear */
        }
        /* $ra->1 corruption tripwire (confirm-first probe): did THIS overlay
         * instruction clobber $ra to 1? Latches once, cheap after. */
        if (cpu->gpr[31] == 1u && before_ra != 1u) {
            extern void psx_ra_tripwire(CPUState *, uint32_t, uint32_t, uint32_t);
            psx_ra_tripwire(cpu, before_ra, pc, 0u /*INTERP*/);
        }
        /* Tomba2 ra-corruption pin (Confirm-(b)): capture the EXACT instruction
         * that loads/sets $ra to the watched wild value (0x49422E54). For a
         * `lw ra, off(sp)` this records the loading pc/insn + sp + the source
         * stack address so we can see whether sp is wrong or the filename string
         * was written onto the saved-ra slot. Latches g_ra_load_snap_valid. */
        if (g_ra_load_watch != 0u && cpu->gpr[31] == g_ra_load_watch &&
            before_ra != g_ra_load_watch && !g_ra_load_snap_valid) {
            g_ra_load_snap_valid = 1;
            g_ra_load_snap_pc    = pc;
            g_ra_load_snap_insn  = insn;
            g_ra_load_snap_before_ra = before_ra;
            for (int r = 0; r < 32; r++) g_ra_load_snap_gpr[r] = cpu->gpr[r];
            /* If it's a load (lw/lbu/...), decode base+imm to record the source addr. */
            uint32_t op = insn >> 26;
            if (op >= 0x20u && op <= 0x25u) { /* lb/lh/lwl/lw/lbu/lhu */
                uint32_t base = cpu->gpr[(insn >> 21) & 0x1Fu];
                int16_t  imm  = (int16_t)(insn & 0xFFFFu);
                g_ra_load_snap_srcaddr = base + (int32_t)imm;
            } else {
                g_ra_load_snap_srcaddr = 0;
            }
        }
#endif
        /* Per-instruction cycle cost (R3000A load-delay interlock) is charged
         * INSIDE exec_one (top-of-fn §1+deps+DO_LDS, or psx_cyc_load_* for loads). */
#ifndef PSX_NO_DEBUG_TOOLS
        dirty_ram_log_instruction(cpu, pc, insn, before_s0, next_pc,
                                  transferred ? cpu->pc : next_pc,
                                  transferred);
#endif
#ifdef PSX_COSIM
        if (!cosim_exec_one_did_hook()) { extern void cosim_instr(uint32_t); cosim_instr(pc); }
#endif
        if (g_unsupported_seen) {
            if (insns_executed == 0) {
                /* Couldn't decode the first instruction.  Most likely
                 * dispatch landed in a dirty page that's not actually
                 * code (stale data, return-target into save area, etc.).
                 * Hand off to psx_unknown_dispatch which has its own
                 * pattern-matching trampoline resolver. */
                return 0;
            }
            /* Made some progress, then hit unknown.  Treat as a no-op
             * return like psx_unknown_dispatch does for unrecognized
             * targets — set cpu->pc=0 so the trampoline exits cleanly.
             * If this turns out to be load-bearing, measurement will
             * surface it as a card-protocol stall and we can add the
             * missing opcode.
             *
             * No fprintf — read the last-* globals via TCP if needed
             * (CLAUDE.md §3). Synchronous stderr at the rate this fires
             * starves the dispatch loop and the debug-server poll. */
            g_dirty_ram_unsupported_midblock++;
            g_dirty_ram_last_unsupported_entry   = addr;
            g_dirty_ram_last_unsupported_entry_ra = cpu->gpr[31];
            g_dirty_ram_last_unsupported_entry_sp = cpu->gpr[29];
            g_dirty_ram_last_unsupported_insns   = (uint32_t)insns_executed;
            g_dirty_ram_last_unsupported_pc     = g_unsupported_pc;
            g_dirty_ram_last_unsupported_insn   = g_unsupported_insn;
            g_dirty_ram_last_unsupported_reason = g_unsupported_reason;
            psx_pc0_journal_note(PSX_PC0J_DIRTY_UNSUPPORTED, cpu,
                                 g_unsupported_pc, g_unsupported_insn);
            cpu->pc = 0;
            OV_FPLOG_RET1();
        }
        g_dirty_ram_insns_run++;
        insns_executed++;
        /* COP0 software-interrupt latency: an MTC0 to CAUSE or SR that makes a
         * software interrupt deliverable (CAUSE.IP0/IP1 & SR.IM0/IM1 & IEc)
         * must be taken IMMEDIATELY — real hardware vectors within the next
         * instruction. Waiting for the next block boundary opens a window in
         * which another hardware IRQ can re-enter the guest's (single-slot,
         * legitimately non-reentrant) dispatcher and destroy its saved SR:
         * Jackie Chan Stuntmaster's stage-1 handler does exactly
         * `mtc0 CAUSE,0x100; mtc0 SR,0x101` and relies on the instant sw-int
         * to reach its stage-2 before anything else runs. Same nested-delivery
         * machinery as the entry poll above; EPC = the committed next pc. */
        if ((insn & 0xFFE00000u) == 0x40800000u) { /* MTC0 */
            uint32_t sw_rd = (insn >> 11) & 0x1Fu;
            if ((sw_rd == 12u || sw_rd == 13u) &&
                (cpu->cop0[13] & cpu->cop0[12] & 0x0300u) != 0u &&
                (cpu->cop0[12] & 0x1u) != 0u) {
                extern uint32_t g_dirty_safe_resume_pc;
                uint32_t saved_resume = g_dirty_safe_resume_pc;
                cpu->pc = next_pc ? next_pc : pc + 4u;
                g_dirty_safe_resume_pc = cpu->pc;
                /* Retire an owed load writeback before vectoring: the R3000A
                 * pipeline drains on exception entry, and the handler must not
                 * observe a stale destination register. */
                dirty_ram_ld_delay_flush(cpu);
                psx_check_interrupts(cpu);
                g_dirty_safe_resume_pc = saved_resume;
            }
        }
        if (transferred) {
            if (g_psx_call_bail) {
                /* A bail unwind began inside a surfaced call: stop the interp
                 * run and hand the wild target (cpu->pc) up to the dispatch
                 * loop's bail handling. */
                g_dirty_ram_blocks_run++;
                if (pc_entry) pc_entry->insns += (uint64_t)insns_executed;
                OV_FPLOG_RET1();
            }
            /* Trampoline contract parity (full_function_emitter emits this
             * check after every compiled function return): the transfer that
             * just committed may be the jr whose delay-slot RFE armed the
             * host escape — cpu->pc now holds the real EPC, so take the
             * escape BEFORE local dirty flow can continue interpreting the
             * resumed code inside the handler's host window. Same-fiber: this
             * longjmps to the psx_check_interrupts frame that dispatched the
             * handler (its epilogue restores in_exception and it returns with
             * cpu->pc = EPC through the existing pump-boundary redirect).
             * Foreign fiber: declines, and the normal surfacing below applies. */
            if (g_rfe_escape_pending)
                psx_rfe_escape_check(cpu);
            uint32_t target = cpu->pc;
            if (target != 0u && dirty_ram_pump_boundary(cpu, target, 1)) {
                g_dirty_ram_blocks_run++;
                if (pc_entry) pc_entry->insns += (uint64_t)insns_executed;
                g_dirty_interp_chain_target = cpu->pc;
                OV_FPLOG_RET1();
            }
            target = cpu->pc;
#ifdef PSX_HAS_GAME_DISPATCH
            if (target != 0) {
                /* §20 FIX — the long-run idle freeze. A guest TAIL-transfer (j/jr/
                 * branch) from an interpreted overlay into compiled code used to NEST
                 * a fresh psx_dispatch_game_compiled trampoline here. That nest never
                 * unwound for the per-frame render chain, leaking ~one host call chain
                 * (~1.17 KB) PER FRAME -> the 64 MB guest stack overflowed after ~40k
                 * idle frames -> the native-stack guard tripped (RECURSION_BUG.md
                 * §19/§20; confirmed by the ce_profile linear climb).
                 *
                 * A tail-transfer carries NO return obligation, so the correct action
                 * is to SURFACE the target (leave it in cpu->pc) and return; the OUTER
                 * psx_dispatch_impl trampoline re-dispatches cpu->pc FLAT
                 * (full_function_emitter.cpp:1230), keeping the host stack bounded
                 * across frames. Dirty-overlay targets still take the local-dirty-flow
                 * just below (kept flat in-interpreter); compiled / static / unknown
                 * targets fall through to the surface return (case 3). Real guest
                 * CALLS (jal/jalr) are unaffected — they nest inline in exec_one and
                 * return normally. (The §14 watermark surfaced via g_psx_call_bail,
                 * which is wild-return semantics and wedged; a plain tail surface does
                 * not touch the bail.) */
#ifndef PSX_NO_DEBUG_TOOLS
                xprobe_event(pc, XOP_BR, XSITE_INTERP, target, insn,
                             cpu->gpr[29], cpu->gpr[31], 1);
#endif
                cpu->pc = target;  /* surfaced; trampoline re-dispatches flat */
            }
#endif
            uint32_t target_phys = target & 0x1FFFFFFFu;
            if (allow_local_dirty_flow && target != 0 &&
                target != stop_addr &&
                phys_is_overlay_flow_region(target_phys) &&
                dirty_ram_is_dirty(target_phys)) {
                /* A runtime overlay may start executing while its final code
                 * bytes are still being installed. Entry-time native validation
                 * must reject that partial image, but local dirty flow used to
                 * remain here for up to one million instructions after the bytes
                 * became an exact cached match. Tomba 2's MDEC path turns that
                 * one early miss into an entire FMV interpreted at ~23 fps.
                 *
                 * Re-check exact cached entries at safe guest transfer
                 * boundaries. The loader re-hashes generation-changed code
                 * before executing it, so incomplete/self-modified bytes stay on
                 * this authoritative interpreter path. */
                if (overlay_loader_is_candidate(target_phys)) {
                    extern int overlay_loader_dispatch(CPUState *cpu, uint32_t addr);
                    if (overlay_loader_dispatch(cpu, target)) {
                        g_dirty_ram_native_handoffs++;
                        g_dirty_ram_blocks_run++;
                        if (pc_entry) pc_entry->insns += (uint64_t)insns_executed;
                        OV_FPLOG_RET1();
                    }
                }
#ifdef PSX_HAS_GAME_DISPATCH
                /* A patched prologue can force entry through the interpreter,
                 * while the remaining static ranges at a later continuation
                 * are still safe to run as compiled code. */
                if (clean_game_text_miss && interp_enter_compiled(cpu, target)) {
                    g_dirty_ram_native_handoffs++;
                    g_dirty_ram_blocks_run++;
                    if (pc_entry) pc_entry->insns += (uint64_t)insns_executed;
                    g_dirty_interp_chain_target = cpu->pc;
                    OV_FPLOG_RET1();
                }
#endif
                /* Capture freeze gates ONLY the ring write — never flow. */
#ifndef PSX_NO_DEBUG_TOOLS
                if (!g_insn_log_frozen) {
                    uint64_t s = g_dirty_ram_flow_log_seq++;
                    DirtyRamFlowLogEntry *e =
                        &g_dirty_ram_flow_log[s & (DIRTY_RAM_FLOW_LOG_CAP - 1u)];
                    e->seq = s;
                    e->pc = pc;
                    e->target = target;
                    e->ra = cpu->gpr[31];
                    e->a0 = cpu->gpr[4];
                    e->a1 = cpu->gpr[5];
                    e->a2 = cpu->gpr[6];
                    e->a3 = cpu->gpr[7];
                    e->sp = cpu->gpr[29];
                    e->frame = (uint32_t)s_frame_count;
                }
#endif
                pc = target;
                current_page = target_phys >> 12;
                current_page_dirty = 1; /* is_local_dirty_target proved it */
                if ((insns_executed & 0xFFF) == 0) {
                    debug_server_poll();
                    debug_server_wait_if_paused();
                }
                continue;
            }
            g_dirty_ram_blocks_run++;
            if (pc_entry) pc_entry->insns += (uint64_t)insns_executed;
            g_dirty_interp_chain_target = cpu->pc;
            OV_FPLOG_RET1();
        }
        pc = next_pc;
#ifdef PSX_HAS_GAME_DISPATCH
        /* Guest call returns advance without transferred set. Re-check the
         * resume PC so a patched entry can hand its unchanged tail back to
         * compiled code without adding probes to ordinary dirty overlay runs. */
        if (clean_game_text_miss && interp_enter_compiled(cpu, pc)) {
            g_dirty_ram_native_handoffs++;
            g_dirty_ram_blocks_run++;
            if (pc_entry) pc_entry->insns += (uint64_t)insns_executed;
            g_dirty_interp_chain_target = cpu->pc;
            OV_FPLOG_RET1();
        }
#endif
        /* Straight-line flow reaching the dispatch return contract — exit
         * so the loop returns into the suspended native caller (same
         * hazard as a transfer to stop_addr). */
        if (stop_addr != 0 && pc == stop_addr) {
            cpu->pc = pc;
            if (dirty_ram_pump_boundary(cpu, pc, 2)) {
                g_dirty_ram_blocks_run++;
                if (pc_entry) pc_entry->insns += (uint64_t)insns_executed;
                g_dirty_interp_chain_target = cpu->pc;
                OV_FPLOG_RET1();
            }
            g_dirty_ram_blocks_run++;
            if (pc_entry) pc_entry->insns += (uint64_t)insns_executed;
            g_dirty_interp_chain_target = pc;
            OV_FPLOG_RET1();
        }
        /* Straight-line code that left the dirty page — hand back to
         * static dispatch by setting cpu->pc and returning. */
        uint32_t next_phys = pc & 0x1FFFFFFFu;
        uint32_t next_page = next_phys >> 12;
        if ((!current_page_dirty || next_page != current_page) &&
            !dirty_ram_is_dirty(next_phys)) {
            cpu->pc = pc;
            if (dirty_ram_pump_boundary(cpu, pc, 3)) {
                g_dirty_ram_blocks_run++;
                if (pc_entry) pc_entry->insns += (uint64_t)insns_executed;
                g_dirty_interp_chain_target = cpu->pc;
                OV_FPLOG_RET1();
            }
            g_dirty_ram_blocks_run++;
            if (pc_entry) pc_entry->insns += (uint64_t)insns_executed;
            g_dirty_interp_chain_target = pc;
            OV_FPLOG_RET1();
        }
        if (next_page != current_page) current_page_dirty = 1;
        current_page = next_page;
    }
    g_dirty_ram_last_unsupported_pc = pc;
    g_dirty_ram_last_unsupported_insn = fetch_word(pc & 0x1FFFFFFFu);
    g_dirty_ram_last_unsupported_reason = "instruction guard";
    g_dirty_ram_guard_yields++;
    g_dirty_ram_blocks_run++;
    cpu->pc = pc;
    if (pc_entry) pc_entry->insns += (uint64_t)insns_executed;
    g_dirty_interp_chain_target = pc;
    {
        uint64_t gap = g_dirty_ram_insns_run - s_last_dirty_irq_pump_insns;
        if (gap >= 4096u) {
            uint32_t committed = cpu->pc;
            int prev_site = g_cosim_dirty_pump_site;
            g_cosim_dirty_pump_site = 4;
            g_dirty_safe_resume_pc = committed;
            s_last_dirty_irq_pump_insns = g_dirty_ram_insns_run;
            dirty_ram_ld_delay_flush(cpu);   /* pipeline drains on exception entry */
            psx_check_interrupts(cpu);
            /* Frame-1997 fix (see outer pump): restore the committed PC if a game RFE
             * longjmp left cpu->pc=0, so the trampoline re-dispatches instead of exiting. */
            if (cpu->pc == 0u && committed != 0u) {
                cpu->pc = committed;
                g_async_rfe_fire_count++;
            }
            g_dirty_safe_resume_pc = 0;
            g_cosim_dirty_pump_site = prev_site;
        }
    }
    OV_FPLOG_RET1();
}
#undef OV_FPLOG_RET1

/* ============================================================================
 * #2 — Lockstep "unit-test interp" comparator. See lockstep.h.
 * Compiled-first + read-trace replay, per basic block, window-gated.
 * ==========================================================================*/
#include "lockstep.h"

int g_ls_mode = 0;
int g_ls_replay_active = 0;
int g_ls_suppress_record = 0;

static uint32_t s_ls_frame_lo = 0, s_ls_frame_hi = 0;   /* hi==0 => disabled */

typedef struct { uint8_t is_write, size; uint32_t addr, val; } ls_op_t;
enum { LS_TRACE_CAP = 65536 };
static ls_op_t  s_ls_trace[LS_TRACE_CAP];
static int      s_ls_trace_n = 0, s_ls_trace_idx = 0;
static int      s_ls_overflow = 0, s_ls_mismatch = 0;
static int      s_ls_replay_done = 0;   /* trace exhausted: stop replay (benign count mismatch) */
static uint32_t s_ls_cur_pc = 0;          /* pc of the instruction being replayed */
static int      s_ls_shadow_owner = 0;     /* 0 none, 1 record, 2 replay */
static int      s_ls_shadow_saw_exception = 0;
static CPUState s_ls_R0;                   /* register snapshot at block entry     */
static uint32_t s_ls_block = 0;
static int      s_ls_prev_valid = 0;

/* mismatch detail captured by the hooks (read by ls_replay_and_compare) */
static int      s_ls_m_kind = 0;          /* 1 read-addr 2 write-addr 3 write-val 4 trace-exhausted */
static uint32_t s_ls_m_pc = 0, s_ls_m_addr = 0, s_ls_m_exp = 0, s_ls_m_act = 0;

static struct {
    int      found;
    int      kind;        /* see ls_kind below */
    uint32_t frame, block, pc, addr;
    uint32_t exp, act;    /* exp = interp(correct), act = compiled(actual)   */
    int      detail;      /* reg index for REG kind, else 0                  */
    uint64_t blocks_checked;
} s_ls_div = {0};
/* kinds: 1 REG 2 HI 3 LO 4 WRITE-VAL 5 READ-ADDR 6 WRITE-ADDR 7 TRACE-EXHAUSTED
 *        8 COMPILED-EXTRA-OPS 9 PATH-CAP */
/* Post-mortem: the recorded op-trace of the diverging block + the replay's
 * op index where it diverged, so the desync can be read off directly. */
static ls_op_t  s_ls_div_trace[48];
static int      s_ls_div_trace_n = 0, s_ls_div_idx = 0;

static int s_ls_record_only = 0;   /* diagnostic: record but skip inline replay (perturbation test) */

static uint32_t s_lsf_frame_lo = 0, s_lsf_frame_hi = 0; /* hi==0 => disabled */
static int      s_lsf_record_only = 0;
static int      s_lsf_active = 0;
static int      s_lsf_recording = 0;
static int      s_lsf_saw_irq = 0;
static CPUState s_lsf_R0;
static uint32_t s_lsf_entry = 0, s_lsf_dispatch_entry = 0, s_lsf_frame = 0;

static struct {
    int      found;
    int      kind;        /* see lsf_kind names in ls_get_func_json */
    uint32_t frame, entry, pc, addr;
    uint32_t exp, act;
    int      detail;
    uint64_t segments_checked, skipped_irq, skipped_overflow;
    uint64_t skipped_unhandled, skipped_conflict, skipped_disabled;
    uint32_t trace_ops, replay_ops, steps;
} s_lsf_div = {0};

static ls_op_t s_lsf_div_trace[48];
static int     s_lsf_div_trace_n = 0, s_lsf_div_idx = 0;

void ls_set_window(uint32_t lo, uint32_t hi) { s_ls_frame_lo = lo; s_ls_frame_hi = hi; }
void ls_set_record_only(int on) { s_ls_record_only = on; }
void ls_func_set_window(uint32_t lo, uint32_t hi) { s_lsf_frame_lo = lo; s_lsf_frame_hi = hi; }
void ls_func_set_record_only(int on) { s_lsf_record_only = on; }

void ls_note_exception_entry(void) {
    if (s_lsf_active) s_lsf_saw_irq = 1;
    if (s_ls_shadow_owner == 1) s_ls_shadow_saw_exception = 1;
}

int ls_shadow_record_begin(void) {
    if (s_ls_shadow_owner || g_ls_mode != 0 || g_ls_replay_active ||
        s_lsf_active || s_ls_prev_valid)
        return 0;
    s_ls_trace_n = 0;
    s_ls_trace_idx = 0;
    s_ls_overflow = 0;
    s_ls_mismatch = 0;
    s_ls_m_kind = 0;
    s_ls_replay_done = 0;
    s_ls_shadow_saw_exception = 0;
    s_ls_shadow_owner = 1;
    g_ls_mode = 1;
    return 1;
}

int ls_shadow_record_end(uint32_t *ops, int *saw_exception) {
    if (s_ls_shadow_owner != 1) return 0;
    g_ls_mode = 0;
    s_ls_shadow_owner = 0;
    if (ops) *ops = (uint32_t)s_ls_trace_n;
    if (saw_exception) *saw_exception = s_ls_shadow_saw_exception;
    return !s_ls_overflow;
}

int ls_shadow_replay_begin(void) {
    if (s_ls_shadow_owner || g_ls_mode != 0 || g_ls_replay_active ||
        s_ls_overflow)
        return 0;
    s_ls_trace_idx = 0;
    s_ls_mismatch = 0;
    s_ls_m_kind = 0;
    s_ls_replay_done = 0;
    s_ls_shadow_owner = 2;
    g_ls_replay_active = 1;
    g_ls_mode = 2;
    return 1;
}

int ls_shadow_replay_end(uint32_t *ops, int *mismatch_kind,
                         uint32_t *pc, uint32_t *addr,
                         uint32_t *expected, uint32_t *actual) {
    if (s_ls_shadow_owner != 2) return 0;
    int complete = !s_ls_mismatch && !s_ls_replay_done &&
                   s_ls_trace_idx == s_ls_trace_n;
    g_ls_mode = 0;
    g_ls_replay_active = 0;
    s_ls_shadow_owner = 0;
    if (ops) *ops = (uint32_t)s_ls_trace_idx;
    if (mismatch_kind) *mismatch_kind = s_ls_m_kind;
    if (pc) *pc = s_ls_m_pc;
    if (addr) *addr = s_ls_m_addr;
    if (expected) *expected = s_ls_m_exp;
    if (actual) *actual = s_ls_m_act;
    return complete;
}

void ls_shadow_abort(void) {
    g_ls_mode = 0;
    g_ls_replay_active = 0;
    s_ls_shadow_owner = 0;
    s_ls_shadow_saw_exception = 0;
}

void ls_suppress_begin(void) {
    g_ls_suppress_record++;
}

void ls_suppress_end(void) {
    if (g_ls_suppress_record > 0) g_ls_suppress_record--;
}

uint32_t ls_read_hook(uint32_t addr, int size, uint32_t real_val) {
    if (g_ls_mode == 2) {                 /* replay: serve from trace */
        if (s_ls_trace_idx >= s_ls_trace_n) { s_ls_replay_done = 1; return 0; }  /* count mismatch: benign block-boundary artifact */
        ls_op_t *o = &s_ls_trace[s_ls_trace_idx];
        if (o->is_write || o->size != (uint8_t)size || o->addr != addr) {
            if (!s_ls_mismatch) { s_ls_mismatch = 1; s_ls_m_kind = 1; s_ls_m_pc = s_ls_cur_pc;
                                  s_ls_m_addr = addr; s_ls_m_exp = addr; s_ls_m_act = o->addr; }
            return 0;
        }
        s_ls_trace_idx++;
        return o->val;
    }
    if (g_ls_mode == 1) {                  /* record */
        if (s_ls_trace_n < LS_TRACE_CAP) {
            ls_op_t *o = &s_ls_trace[s_ls_trace_n++];
            o->is_write = 0; o->size = (uint8_t)size; o->addr = addr; o->val = real_val;
        } else s_ls_overflow = 1;
    }
    return real_val;
}

void ls_write_hook(uint32_t addr, int size, uint32_t val) {
    if (g_ls_mode == 2) {                 /* replay: verify against trace */
        if (s_ls_trace_idx >= s_ls_trace_n) { s_ls_replay_done = 1; return; }  /* count mismatch: benign block-boundary artifact */
        ls_op_t *o = &s_ls_trace[s_ls_trace_idx];
        if (!o->is_write || o->size != (uint8_t)size || o->addr != addr) {
            if (!s_ls_mismatch) { s_ls_mismatch = 1; s_ls_m_kind = 2; s_ls_m_pc = s_ls_cur_pc;
                                  s_ls_m_addr = addr; s_ls_m_exp = addr; s_ls_m_act = o->addr; }
            return;
        }
        if (o->val != val) {              /* compiled wrote a different value than interp */
            if (!s_ls_mismatch) { s_ls_mismatch = 1; s_ls_m_kind = 3; s_ls_m_pc = s_ls_cur_pc;
                                  s_ls_m_addr = addr; s_ls_m_exp = val; s_ls_m_act = o->val; }
        }
        s_ls_trace_idx++;
        return;
    }
    if (g_ls_mode == 1) {                  /* record */
        if (s_ls_trace_n < LS_TRACE_CAP) {
            ls_op_t *o = &s_ls_trace[s_ls_trace_n++];
            o->is_write = 1; o->size = (uint8_t)size; o->addr = addr; o->val = val;
        } else s_ls_overflow = 1;
    }
}

static void ls_latch(int kind, uint32_t frame, uint32_t pc, uint32_t addr,
                     uint32_t exp, uint32_t act, int detail) {
    if (s_ls_div.found) return;
    s_ls_div.found = 1; s_ls_div.kind = kind; s_ls_div.frame = frame;
    s_ls_div.block = s_ls_block; s_ls_div.pc = pc; s_ls_div.addr = addr;
    s_ls_div.exp = exp; s_ls_div.act = act; s_ls_div.detail = detail;
}

/* Re-interpret the just-recorded block from the entry snapshot, comparing the
 * interp result to native (cpu == post-block native state). */
static void ls_replay_and_compare(CPUState *cpu, uint32_t frame, uint32_t target_phys) {
    CPUState rep = s_ls_R0;                 /* registers from block entry (pc is stale in compiled) */
    rep.pc = s_ls_block | 0x80000000u;      /* start at the recorded block's leader (KSEG0) */
    s_ls_trace_idx = 0; s_ls_mismatch = 0; s_ls_m_kind = 0; s_ls_replay_done = 0;
    /* The replay's exec_one writes process-global decode state on any
     * instruction it can't handle; snapshot+restore so the replay can never
     * divert the REAL dispatch (which reacts to g_unsupported_seen). */
    int         sav_unsup_seen   = g_unsupported_seen;
    uint32_t    sav_unsup_pc     = g_unsupported_pc;
    uint32_t    sav_unsup_insn   = g_unsupported_insn;
    const char *sav_unsup_reason = g_unsupported_reason;
    g_ls_replay_active = 1;
    g_ls_mode = 2;
    uint32_t pc = rep.pc;
    int cap = 512, steps = 0;
    for (;;) {
        /* Stop at the next block leader (fall-through block, or a branch's
         * target). steps>0 so a self-looping block runs one iteration. */
        if (steps > 0 && (pc & 0x1FFFFFFFu) == target_phys) break;
        if (cap-- <= 0 || s_ls_mismatch || s_ls_replay_done) break;
        uint32_t next_pc = 0;
        s_ls_cur_pc = pc;
        int transferred = exec_one(&rep, pc, &next_pc);
        pc = transferred ? rep.pc : next_pc;
        steps++;
        /* Also stop at the block TERMINATOR (branch/jump/call) — do NOT follow
         * it. A call would run the callee (kernel/syscall/another fn) which the
         * recording dispatched separately or whose ops trail in the trace;
         * following it desyncs. Compare only the block's own in-line memory ops,
         * checked op-by-op against the trace as we go. */
        if (transferred) break;
    }
    g_ls_mode = 0;
    g_ls_replay_active = 0;
    g_unsupported_seen   = sav_unsup_seen;     /* undo any replay-side decode-state leak */
    g_unsupported_pc     = sav_unsup_pc;
    g_unsupported_insn   = sav_unsup_insn;
    g_unsupported_reason = sav_unsup_reason;
    s_ls_div.blocks_checked++;

    /* Divergence = an in-block memory-op mismatch (the interp replay read a
     * different address, or the compiled block wrote a different value, at the
     * same op index given identical entry state). Leftover trace ops past the
     * terminator are the callee's (recording spanned a call) and are ignored;
     * registers are not compared (boundary-sensitive across calls). A wrong
     * register value still surfaces here later, as a wrong store value / load
     * address, when the bad value is used. */
    if (s_ls_mismatch) {
        int n = s_ls_trace_n; if (n > 48) n = 48;
        for (int i = 0; i < n; i++) s_ls_div_trace[i] = s_ls_trace[i];
        s_ls_div_trace_n = n;
        s_ls_div_idx = s_ls_trace_idx;
        int k = (s_ls_m_kind == 1) ? 5 : (s_ls_m_kind == 2) ? 6 : (s_ls_m_kind == 3) ? 4 : 7;
        ls_latch(k, frame, s_ls_m_pc, s_ls_m_addr, s_ls_m_exp, s_ls_m_act, 0);
        return;
    }
}

static int ls_same_phys(uint32_t a, uint32_t b) {
    return (((a ^ b) & 0x1FFFFFFFu) == 0);
}

static void lsf_copy_div_trace(void) {
    int n = s_ls_trace_n;
    if (n > 48) n = 48;
    for (int i = 0; i < n; i++) s_lsf_div_trace[i] = s_ls_trace[i];
    s_lsf_div_trace_n = n;
    s_lsf_div_idx = s_ls_trace_idx;
}

static void lsf_latch(int kind, uint32_t frame, uint32_t entry, uint32_t pc,
                      uint32_t addr, uint32_t exp, uint32_t act, int detail,
                      uint32_t steps) {
    if (s_lsf_div.found) return;
    s_lsf_div.found = 1;
    s_lsf_div.kind = kind;
    s_lsf_div.frame = frame;
    s_lsf_div.entry = entry;
    s_lsf_div.pc = pc;
    s_lsf_div.addr = addr;
    s_lsf_div.exp = exp;
    s_lsf_div.act = act;
    s_lsf_div.detail = detail;
    s_lsf_div.trace_ops = (uint32_t)s_ls_trace_n;
    s_lsf_div.replay_ops = (uint32_t)s_ls_trace_idx;
    s_lsf_div.steps = steps;
    lsf_copy_div_trace();
}

static void lsf_compare_regs(CPUState *rep, CPUState *nat, uint32_t frame,
                             uint32_t entry, uint32_t steps) {
    if (!ls_same_phys(rep->pc, nat->pc)) {
        lsf_latch(4, frame, entry, rep->pc, 0, rep->pc, nat->pc, 0, steps);
        return;
    }
    for (int i = 0; i < 32; i++) {
        if (rep->gpr[i] != nat->gpr[i]) {
            lsf_latch(1, frame, entry, rep->pc, 0, rep->gpr[i], nat->gpr[i], i, steps);
            return;
        }
    }
    if (rep->hi != nat->hi) {
        lsf_latch(2, frame, entry, rep->pc, 0, rep->hi, nat->hi, 0, steps);
        return;
    }
    if (rep->lo != nat->lo) {
        lsf_latch(3, frame, entry, rep->pc, 0, rep->lo, nat->lo, 0, steps);
        return;
    }
}

static void lsf_replay_and_compare(CPUState *native_post, uint32_t frame,
                                   uint32_t entry_pc) {
    CPUState rep = s_lsf_R0;
    uint32_t target = native_post->pc;
    rep.pc = entry_pc;

    s_ls_trace_idx = 0;
    s_ls_mismatch = 0;
    s_ls_m_kind = 0;
    s_ls_replay_done = 0;

    int         sav_unsup_seen   = g_unsupported_seen;
    uint32_t    sav_unsup_pc     = g_unsupported_pc;
    uint32_t    sav_unsup_insn   = g_unsupported_insn;
    const char *sav_unsup_reason = g_unsupported_reason;
    uint32_t    sav_store_pc     = g_debug_last_store_pc;
    uint32_t    sav_func_addr    = g_debug_current_func_addr;

    g_ls_replay_active = 1;
    g_ls_mode = 2;

    uint32_t pc = entry_pc;
    uint32_t steps = 0;
    int done = 0;
    enum { MAX_FUNC_REPLAY_INSNS = 250000 };
    for (;;) {
        if (steps > 0 && target != 0 && ls_same_phys(pc, target) &&
            s_ls_trace_idx == s_ls_trace_n) {
            done = 1;
            break;
        }
        if (steps >= MAX_FUNC_REPLAY_INSNS) {
            lsf_latch(10, frame, entry_pc, pc, 0, target, pc, 0, steps);
            break;
        }
        if (s_ls_mismatch || s_ls_replay_done) break;

        uint32_t next_pc = 0;
        s_ls_cur_pc = pc;
        g_unsupported_seen = 0;
        int transferred = exec_one(&rep, pc, &next_pc);
        if (g_unsupported_seen) {
            lsf_latch(11, frame, entry_pc, pc, 0, 0, g_unsupported_insn, 0, steps);
            break;
        }
        pc = transferred ? rep.pc : next_pc;
        rep.pc = pc;
        steps++;
    }

    g_ls_mode = 0;
    g_ls_replay_active = 0;
    g_unsupported_seen   = sav_unsup_seen;
    g_unsupported_pc     = sav_unsup_pc;
    g_unsupported_insn   = sav_unsup_insn;
    g_unsupported_reason = sav_unsup_reason;
    g_debug_last_store_pc = sav_store_pc;
    g_debug_current_func_addr = sav_func_addr;

    s_lsf_div.segments_checked++;

    if (s_ls_mismatch) {
        int k = (s_ls_m_kind == 1) ? 6 : (s_ls_m_kind == 2) ? 7 :
                (s_ls_m_kind == 3) ? 5 : 8;
        lsf_latch(k, frame, entry_pc, s_ls_m_pc, s_ls_m_addr,
                  s_ls_m_exp, s_ls_m_act, 0, steps);
        return;
    }
    if (s_ls_replay_done) {
        lsf_latch(8, frame, entry_pc, s_ls_cur_pc, 0, (uint32_t)s_ls_trace_n,
                  (uint32_t)s_ls_trace_idx, 0, steps);
        return;
    }
    if (!done) {
        lsf_latch(10, frame, entry_pc, pc, 0, target, pc, 0, steps);
        return;
    }
    if (s_ls_trace_idx != s_ls_trace_n) {
        lsf_latch(9, frame, entry_pc, pc, 0, (uint32_t)s_ls_trace_n,
                  (uint32_t)s_ls_trace_idx, 0, steps);
        return;
    }

    lsf_compare_regs(&rep, native_post, frame, entry_pc, steps);
}

void ls_func_enter(uint32_t entry_pc, CPUState *cpu) {
    if (s_lsf_frame_hi == 0 || s_lsf_div.found || !cpu) return;
    if (g_ls_replay_active || g_ls_mode != 0 || s_lsf_active) {
        s_lsf_div.skipped_conflict++;
        return;
    }
    if (psx_get_in_exception()) {
        s_lsf_div.skipped_irq++;
        return;
    }
    uint32_t frame = (uint32_t)s_frame_count;
    if (frame < s_lsf_frame_lo || frame > s_lsf_frame_hi) {
        s_lsf_div.skipped_disabled++;
        return;
    }
    uint32_t phys = entry_pc & 0x1FFFFFFFu;
    if (phys < 0x00010000u || phys >= 0x00200000u) return;

    s_lsf_dispatch_entry = entry_pc;
    s_lsf_entry = entry_pc;
    s_lsf_frame = frame;
    s_lsf_saw_irq = 0;
    s_lsf_active = 1;
    s_lsf_recording = 0;
}

void ls_func_exit(uint32_t entry_pc, CPUState *cpu, int handled) {
    if (!s_lsf_active) return;
    if (s_lsf_dispatch_entry != entry_pc) {
        g_ls_mode = 0;
        s_lsf_active = 0;
        s_lsf_recording = 0;
        s_lsf_div.skipped_conflict++;
        return;
    }
    g_ls_mode = 0;
    s_lsf_active = 0;
    if (!s_lsf_recording) {
        s_lsf_div.skipped_unhandled++;
        return;
    }
    s_lsf_recording = 0;

    if (!handled || !cpu) {
        s_lsf_div.skipped_unhandled++;
        return;
    }
    if (s_lsf_saw_irq) {
        s_lsf_div.skipped_irq++;
        return;
    }
    if (s_ls_overflow) {
        s_lsf_div.skipped_overflow++;
        return;
    }
    if (s_lsf_record_only) {
        s_lsf_div.segments_checked++;
        return;
    }
    lsf_replay_and_compare(cpu, s_lsf_frame, s_lsf_entry);
}

void ls_at_leader(uint32_t leader_phys, CPUState *cpu) {
    if (s_lsf_active) {
        if (!s_lsf_recording && cpu && !s_lsf_div.found && g_ls_mode == 0) {
            s_lsf_R0 = *cpu;
            s_lsf_entry = leader_phys | 0x80000000u;
            s_ls_trace_n = 0;
            s_ls_trace_idx = 0;
            s_ls_overflow = 0;
            s_ls_mismatch = 0;
            s_ls_m_kind = 0;
            s_lsf_recording = 1;
            g_ls_mode = 1;
        }
        return;                           /* function-scope recording owns g_ls_mode */
    }
    if (s_ls_frame_hi == 0 || s_ls_div.found || !cpu) return;
    if (g_ls_mode == 2) return;            /* re-entrancy guard (shouldn't happen) */
    uint32_t frame = (uint32_t)s_frame_count;
    int in_win = (frame >= s_ls_frame_lo && frame <= s_ls_frame_hi);
    if (!in_win) { if (g_ls_mode == 1) { g_ls_mode = 0; s_ls_prev_valid = 0; } return; }

    if (s_ls_prev_valid && g_ls_mode == 1) {   /* finalize previous game block */
        g_ls_mode = 0;
        if (!s_ls_record_only) {                /* record_only: skip inline replay (perturbation test) */
            ls_replay_and_compare(cpu, frame, leader_phys);  /* this leader = where prev block went */
            if (s_ls_div.found) { s_ls_prev_valid = 0; return; }
        } else {
            s_ls_div.blocks_checked++;          /* count recorded blocks */
        }
        s_ls_prev_valid = 0;
    }
    /* Only START recording for a genuinely COMPILED game-text block:
     *  - !g_ls_dirty_observe: the dirty-RAM interp loop also calls cyc_observe
     *    (per-instruction, for the cycle ruler); skip those — they're already
     *    interpreted (clean game text dispatched FROM the dirty path still runs
     *    compiled, so we can't use g_dirty_interp_active here).
     *  - [0x10000, 0x200000): game EXE text in main RAM (above the low-RAM
     *    kernel/relocated-BIOS area, which isn't the regression locus). */
    if (!g_ls_dirty_observe && leader_phys >= 0x00010000u && leader_phys < 0x00200000u) {
        s_ls_R0 = *cpu;
        s_ls_block = leader_phys;
        s_ls_trace_n = 0; s_ls_trace_idx = 0; s_ls_overflow = 0; s_ls_mismatch = 0;
        s_ls_prev_valid = 1;
        g_ls_mode = 1;
    }
}

int ls_get_diverge_json(char *buf, int buflen) {
    static const char *kn[] = {"none","reg","hi","lo","write-val","read-addr",
                               "write-addr","trace-exhausted","compiled-extra-ops","path-cap"};
    int k = (s_ls_div.kind >= 0 && s_ls_div.kind <= 9) ? s_ls_div.kind : 0;
    int p = snprintf(buf, buflen,
        "{\"found\":%d,\"kind\":\"%s\",\"frame\":%u,\"block\":\"0x%08X\",\"pc\":\"0x%08X\","
        "\"addr\":\"0x%08X\",\"interp_expected\":\"0x%08X\",\"compiled_actual\":\"0x%08X\","
        "\"reg\":%d,\"blocks_checked\":%llu,\"window\":[%u,%u],\"div_idx\":%d,\"trace\":[",
        s_ls_div.found, kn[k], s_ls_div.frame, s_ls_div.block, s_ls_div.pc,
        s_ls_div.addr, s_ls_div.exp, s_ls_div.act, s_ls_div.detail,
        (unsigned long long)s_ls_div.blocks_checked, s_ls_frame_lo, s_ls_frame_hi,
        s_ls_div_idx);
    for (int i = 0; i < s_ls_div_trace_n && p < buflen - 48; i++) {
        p += snprintf(buf + p, buflen - p, "%s\"%c%d:%08X=%08X\"",
                      i ? "," : "", s_ls_div_trace[i].is_write ? 'W' : 'R',
                      s_ls_div_trace[i].size, s_ls_div_trace[i].addr, s_ls_div_trace[i].val);
    }
    p += snprintf(buf + p, buflen - p, "]}");
    return p;
}

int ls_get_func_json(char *buf, int buflen) {
    static const char *kn[] = {
        "none","gpr","hi","lo","pc","write-val","read-addr","write-addr",
        "trace-exhausted","trace-leftover","path-cap","unsupported"
    };
    int k = (s_lsf_div.kind >= 0 && s_lsf_div.kind <= 11) ? s_lsf_div.kind : 0;
    int p = snprintf(buf, buflen,
        "{\"found\":%d,\"kind\":\"%s\",\"frame\":%u,\"entry\":\"0x%08X\","
        "\"pc\":\"0x%08X\",\"addr\":\"0x%08X\","
        "\"interp_expected\":\"0x%08X\",\"compiled_actual\":\"0x%08X\","
        "\"reg\":%d,\"segments_checked\":%llu,\"skipped_irq\":%llu,"
        "\"skipped_overflow\":%llu,\"skipped_unhandled\":%llu,"
        "\"skipped_conflict\":%llu,\"skipped_disabled\":%llu,"
        "\"window\":[%u,%u],\"trace_ops\":%u,\"replay_ops\":%u,"
        "\"steps\":%u,\"div_idx\":%d,\"trace\":[",
        s_lsf_div.found, kn[k], s_lsf_div.frame, s_lsf_div.entry,
        s_lsf_div.pc, s_lsf_div.addr, s_lsf_div.exp, s_lsf_div.act,
        s_lsf_div.detail, (unsigned long long)s_lsf_div.segments_checked,
        (unsigned long long)s_lsf_div.skipped_irq,
        (unsigned long long)s_lsf_div.skipped_overflow,
        (unsigned long long)s_lsf_div.skipped_unhandled,
        (unsigned long long)s_lsf_div.skipped_conflict,
        (unsigned long long)s_lsf_div.skipped_disabled,
        s_lsf_frame_lo, s_lsf_frame_hi, s_lsf_div.trace_ops,
        s_lsf_div.replay_ops, s_lsf_div.steps, s_lsf_div_idx);
    for (int i = 0; i < s_lsf_div_trace_n && p < buflen - 48; i++) {
        p += snprintf(buf + p, buflen - p, "%s\"%c%d:%08X=%08X\"",
                      i ? "," : "", s_lsf_div_trace[i].is_write ? 'W' : 'R',
                      s_lsf_div_trace[i].size, s_lsf_div_trace[i].addr,
                      s_lsf_div_trace[i].val);
    }
    p += snprintf(buf + p, buflen - p, "]}");
    return p;
}
