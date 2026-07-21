/* ============================================================================
 * cpu_state.h  —  Phase 2 runtime version
 * ----------------------------------------------------------------------------
 * This is the RUNTIME version of the CPUState header. It has the same struct
 * layout as generated/cpu_state.h (Phase 1a compile-only artifact) but is
 * intended to be linked into a runnable binary.
 *
 * The generated C files (SCPH1001_full.c, SCPH1001_dispatch.c) include
 * "cpu_state.h" and the build uses -I runtime/include so they find this file.
 * ========================================================================== */

#ifndef PSXRECOMP_CPU_STATE_H
#define PSXRECOMP_CPU_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* (sljit shard host-helper table removed 2026-07-15 with the sljit tier.) */

typedef struct CPUState {
    uint32_t gpr[32];   /* $0..$31; gpr[0] is hardwired zero, never written */
    uint32_t pc;        /* program counter */
    uint32_t hi, lo;    /* mult/div result registers */
    uint32_t cop0[32];  /* COP0 system control registers (SR, Cause, EPC, ...) */
    uint32_t gte_data[32]; /* COP2 (GTE) data registers */
    uint32_t gte_ctrl[32]; /* COP2 (GTE) control registers */

    /* Memory access function pointers -- wired at init to psx_read/psx_write. */
    uint32_t (*read_word)(uint32_t addr);
    void     (*write_word)(uint32_t addr, uint32_t value);
    uint16_t (*read_half)(uint32_t addr);
    void     (*write_half)(uint32_t addr, uint16_t value);
    uint8_t  (*read_byte)(uint32_t addr);
    void     (*write_byte)(uint32_t addr, uint8_t value);

    /* Mult/div completion deadline (absolute guest cycle). Set by MULT/MULTU/
     * DIV/DIVU to psx_cycle_count()+latency; a later MFLO/MFHI stalls (advances
     * cycles) until this deadline — faithful R3000A behavior (Beetle
     * muldiv_ts_done). Appended at END so prior field offsets are unchanged. */
    uint64_t muldiv_ts_done;

    /* GTE (COP2) command completion deadline (absolute guest cycle). A GTE
     * command sets this to psx_cycle_count()+(cost-1) (cost from gte.cpp per-op
     * returns; -1 because the COP2 instruction's own +1 base is charged
     * separately). Back-to-back commands serialize (set stalls to the prior
     * deadline first); any COP2 register access (MFC2/CFC2/MTC2/CTC2/LWC2/SWC2)
     * stalls until it — faithful R3000A behavior (Beetle gte_ts_done).
     * Appended at END so prior field offsets are unchanged. */
    uint64_t gte_ts_done;

    /* ---- R3000A load-delay pipeline interlock (Beetle ReadAbsorb/ReadFudge/
     * LDAbsorb/LDWhich) — TIMING ONLY; the load VALUE delay is handled by the
     * existing load-delay correctness path. See psx_cyc.h + accuracy/
     * load_readfudge_ldabsorb.md. Models: a load's data-access cost becomes a
     * per-register "give-back" (read_absorb) that following instructions consume
     * instead of charging their own +1 base; plus the +2 "fudge" charged on a
     * load whose predecessor committed no load. Appended at END so prior field
     * offsets are unchanged (precompiled overlay DLLs bake offsets). */
    uint8_t  read_absorb[33];   /* ReadAbsorb[0..31] + [32]=0x20 DO_LDS dummy slot */
    uint8_t  read_absorb_which; /* ReadAbsorbWhich: GPR the last committed load wrote */
    uint8_t  read_fudge;        /* ReadFudge: last committed load's dest reg, or 0x20 = none */
    uint8_t  ld_which_t;        /* LDWhich (timing): pending load dest reg, 0x20 = none */
    uint32_t ld_absorb;         /* LDAbsorb: pending load's give-back (region+completion) */
} CPUState;

/* Overlay DLLs batch per-instruction cycle charges in a DLL-local accumulator.
 * A guest store is an observation boundary: MMIO handlers must see the cycle
 * count through the store instruction, and RAM/self-modification/capture hooks
 * must run after those cycles have been published to the host.  Generated code
 * calls this immediately before every store (and before SWL/SWR's raw read).
 * Static recompiled code and the interpreter share host cycle state directly,
 * so their barrier compiles to nothing. */
#ifdef PSX_OVERLAY_DLL_BUILD
void overlay_flush_cycles(void);
static inline void psx_store_cycle_barrier(void) { overlay_flush_cycles(); }
#else
static inline void psx_store_cycle_barrier(void) { }
#endif

/* Faithful exception-return (fix B) — defined in runtime/src/interrupts.c. The
 * interrupted-thread resume PC is the REAL guest PC, stored in COP0.EPC and (by the
 * BIOS handler) in the thread's TCB EPC slot — never a sentinel, never a single
 * global — so a thread suspended mid-exception (ChangeThread) resumes at its OWN PC.
 * The host longjmp escape out of the NESTED synchronous handler is keyed on "RFE
 * while in_exception" (psx_rfe_mark_escape, called by the recompiled `rfe` op;
 * psx_rfe_escape_check, called in the dispatch trampoline), not on pc==sentinel.
 * Declared here (not psx_runtime.h) because the generated BIOS includes only this. */
typedef enum {
    PSX_EXC_ESCAPE_NONE = 0,
    PSX_EXC_ESCAPE_RFE_RETURN,
    PSX_EXC_ESCAPE_SYSCALL_RETURN,
    PSX_EXC_ESCAPE_LEGACY_SENTINEL,
} psx_exc_escape_reason_t;

/* The host escape token for the SYNCHRONOUS (nested) exception handler. It is a
 * pure HOST control-flow marker — it must NEVER be persisted into a guest TCB as a
 * thread's resume PC. If it does, that thread will resume by resolving the sentinel
 * through host escape state (the multi-thread bug fix B repaired), not at its own
 * code. The save/restore/fiber-entry guards in traps.c are fail-closed on this. */
#define PSX_EXC_SENTINEL_PC 0x80000048u
extern int      g_rfe_escape_pending;
extern int      g_exc_escape_reason;     /* psx_exc_escape_reason_t */
extern uint32_t g_exception_real_epc;
void psx_rfe_mark_escape(void);
void psx_rfe_escape_check(CPUState* cpu);

/* Trap trampolines — defined in runtime/src/traps.c */
/* psx_syscall returns 1 if control transfers (cpu->pc set to a dispatch target;
 * the caller must return to the trampoline), 0 for a directly-handled "void"
 * syscall (Enter/ExitCriticalSection). Under CPS the generated code emits
 * `if (psx_syscall(...)) return;` so a void syscall falls through to the inline
 * post-syscall code (the guest's own jr $ra); legacy callers ignore the return
 * value and rely on cpu->pc (0 = handled, resume at caller). */
extern int psx_syscall(CPUState* cpu, uint32_t code);
extern void psx_arith_overflow(CPUState* cpu);
/* Mult/div completion-stall timing (psx_cycles.c). MULT/MULTU/DIV/DIVU call
 * psx_muldiv_set with their latency (DIV/DIVU = 37; MULT/MULTU via the latency
 * helpers, indexed on the first operand magnitude). MFLO/MFHI call
 * psx_muldiv_stall, which advances guest cycles to the deadline if not yet
 * reached. Faithful only with per-instruction cycle charging. */
extern void     psx_muldiv_set(CPUState* cpu, uint32_t latency);
extern void     psx_muldiv_stall(CPUState* cpu);
extern uint32_t psx_mult_latency_s(uint32_t rs);   /* MULT  (signed)   */
extern uint32_t psx_mult_latency_u(uint32_t rs);   /* MULTU (unsigned) */

/* GTE (COP2) per-command completion-stall timing (psx_cycles.c). gte_execute()
 * calls psx_gte_set with the command's added latency (cost-1, from
 * psx_gte_cmd_latency); it serializes back-to-back ops by stalling to the prior
 * deadline first. Every COP2 register access calls psx_gte_stall, which advances
 * guest cycles to the deadline if the op is still in flight. Faithful only with
 * per-instruction cycle charging. */
/* R3000A I-cache instruction-FETCH cost (psx_icache.h). HIT is inlined;
 * MISS lives in psx_icache.c. Shared by interp + compiled emitters. */
#include "psx_icache.h"

extern void     psx_gte_set(CPUState* cpu, uint32_t latency);
extern void     psx_gte_stall(CPUState* cpu);
/* MFC2/CFC2 GTE register read: stall to the deadline AND arm the load-delay
 * give-back (ld_absorb=gte_ts_done-now, ld_which_t=rt) — Beetle MFC2/CFC2. */
extern void     psx_gte_read(CPUState* cpu, uint32_t rt);
extern uint32_t psx_gte_cmd_latency(uint32_t cmd);  /* cost-1 for the 6-bit op  */

extern void psx_unaligned_access(CPUState* cpu, uint32_t addr, uint32_t pc);
extern void psx_break(CPUState* cpu, uint32_t code, uint32_t pc);
/* Fail-closed native entry guard: a function's CPS entry-switch calls this when
 * dispatched at a PC that is not one of its legal entries (foreign interior PC
 * from a range-ownership mismatch). The function returns without executing; the
 * overlay dispatch then routes the PC to the sanctioned dirty-RAM interpreter. */
extern void psx_native_bad_entry(CPUState* cpu, uint32_t owner, uint32_t pc);

/* Dispatch — defined in generated/SCPH1001_dispatch.c */
extern void psx_dispatch(CPUState* cpu, uint32_t target_addr);
extern void psx_dispatch_call(CPUState* cpu, uint32_t target_addr, uint32_t return_addr);

/* Cycle-budgeted precise event slicing — defined in runtime/src/dirty_ram_interp.c.
 * Emitted at each compiled block leader: if it returns nonzero the block ran
 * through the per-instruction interpreter (interrupt taken at the exact
 * architectural instruction) and cpu->pc holds a dispatchable resume point, so
 * the caller must `return` without executing its compiled body. See
 * PRECISE_IRQ_SLICE.md.
 *
 * PARKED default OFF (g_psx_precise_slice=0). The hot inline returns 0 with no
 * call; PSX_PRECISE_SLICE=1 enables the impl for A/B. Every compiled block
 * leader hits this — MotK VLC alone issues millions of calls/s. */
extern int g_psx_precise_slice;
extern int psx_slice_block_impl(CPUState* cpu, uint32_t block_addr, uint32_t bcyc, int side_effects);
void psx_precise_slice_init_from_env(void);
static inline int psx_slice_block(CPUState* cpu, uint32_t block_addr, uint32_t bcyc, int side_effects) {
    if (!g_psx_precise_slice) return 0;
    return psx_slice_block_impl(cpu, block_addr, bcyc, side_effects);
}

/* Unknown dispatch — defined in runtime/src/traps.c */
extern void psx_unknown_dispatch(CPUState* cpu, uint32_t addr, uint32_t phys);

/* GTE (COP2) — defined in runtime/src/gte.cpp */
extern void     gte_execute(CPUState* cpu, uint32_t cmd);
/* Widescreen X-squash: display aspect num:den (4,3 = identity/off). Scales
 * RTPS/RTPT screen-X around the game's OFX so a 4:3 frame stretched to the
 * wide aspect shows a wider field of view. */
extern void     gte_set_display_aspect(int num, int den);
extern void     gte_ws_set_suppress(int on);  /* 8C: un-squash far backdrop draws */
extern void     gte_ws_configure_dome_sites(const uint32_t* sites, int count);
extern uint32_t gte_read_data(CPUState* cpu, uint8_t reg);
extern uint32_t gte_read_ctrl(CPUState* cpu, uint8_t reg);
extern void     gte_write_data(CPUState* cpu, uint8_t reg, uint32_t val);
extern void     gte_write_ctrl(CPUState* cpu, uint8_t reg, uint32_t val);
/* Normalize raw save-state/imported backing arrays to the architectural form
 * expected by generated direct-register fast paths.  This is not a guest
 * register write: in particular, it preserves a computed FLAG bit 31. */
extern void     gte_canonicalize_cpu_state(CPUState* cpu);
/* Drop host-only projection/geometry provenance after a raw machine-state
 * rewind or discarded speculative pass. Guest-visible GTE registers are not
 * changed. */
extern void     gte_precision_timeline_invalidate(void);
/* Isolate a speculative native validation pass from host-only projection
 * caches while preserving the preceding authoritative interpreter result. */
extern void     gte_precision_speculative_begin(void);
extern void     gte_precision_speculative_end(void);
extern void     gte_precision_store_word(uint32_t addr, uint8_t reg);

/* ============================================================================
 * Dispatch call contract (Bug D / wild-return family fix)
 * ----------------------------------------------------------------------------
 * Generated C continuations mirror the guest call graph: after `jal F`, the C
 * code calls F's C function and then falls into the continuation block.  Real
 * hardware has only one stack; if F's flow goes wild (returns to an address
 * other than the call site's $ra, or with a shifted $sp — e.g. a mid-function
 * entry running an epilogue for a frame it never pushed), the hardware simply
 * keeps executing wherever the guest jumped, and the interrupted caller's
 * code never resumes.  Our C model, by contrast, resumes the suspended C
 * continuation whenever the C call returns — re-executing tails on a moved
 * guest stack (the Bug A/C/D zombie family).
 *
 * The contract: a C continuation may run ONLY if the guest actually arrived
 * at it — i.e. the callee came back with $ra == the call site's return
 * address and $sp == the caller's stack pointer at the call.  When the
 * contract is violated, we begin a "bail" unwind: cpu->pc is set to the
 * guest's true target (the wild jr's destination), g_psx_call_bail is set,
 * and every generated frame returns immediately without running its
 * continuation.  The unwind resolves at the first enclosing call site (or
 * dispatch loop) whose (return address, sp) contract matches the guest's
 * arrival state; if none matches, the outermost dispatch loop clears the
 * flag and tail-dispatches the wild target with a clean host stack.
 *
 * g_psx_call_bail is defined in runtime/src/traps.c.  Overlay DLLs share
 * the runtime's state through pointers wired by overlay_init (the
 * PSX_OVERLAY_DLL_BUILD branch; see overlay_api.h / compile_overlays.py).
 * ========================================================================== */
#ifdef PSX_OVERLAY_DLL_BUILD
extern int      *g_psx_call_bail_p;
extern uint64_t *g_psx_bail_first_p;
extern uint64_t *g_psx_bail_resolved_p;
#define g_psx_call_bail     (*g_psx_call_bail_p)
#define g_psx_bail_first    (*g_psx_bail_first_p)
#define g_psx_bail_resolved (*g_psx_bail_resolved_p)
#else
extern int      g_psx_call_bail;
extern uint64_t g_psx_bail_first;      /* contract violations detected      */
extern uint64_t g_psx_bail_resolved;   /* unwinds resolved at a call site   */
extern uint64_t g_psx_bail_flattened;  /* unwinds flattened at outermost    */
extern uint64_t g_psx_bail_anomaly;    /* bail flag seen where impossible   */
#endif

/* Deduped wild-return source ledger (traps.c). Not present in overlay DLLs
 * (they share runtime bail state via pointers, not this recorder). */
#ifndef PSX_OVERLAY_DLL_BUILD
extern void psx_bail_record(uint32_t site_ra, uint32_t site_sp,
                            uint32_t wild_pc, uint32_t guest_sp);
#endif

/* Validate a direct call site after the callee's C return.
 * Returns 1 if the caller must `return;` immediately (bail in progress),
 * 0 if the continuation is valid.  site_ra = the call's return address,
 * site_sp = guest $sp recorded immediately before the call. */
static inline int psx_call_contract(CPUState* cpu, uint32_t site_ra,
                                    uint32_t site_sp) {
    if (g_psx_call_bail) {
        /* An inner frame began a bail unwind.  Resolve here iff the guest's
         * arrival state matches this site's contract. */
        if (((cpu->pc ^ site_ra) & 0x1FFFFFFFu) == 0 &&
            cpu->gpr[29] == site_sp) {
            g_psx_call_bail = 0;
            g_psx_bail_resolved++;
            cpu->pc = 0;
            return 0;
        }
        return 1;
    }
    if (cpu->gpr[29] != site_sp ||
        ((cpu->gpr[31] ^ site_ra) & 0x1FFFFFFFu) != 0) {
        /* First detection: the callee C-returned but the guest did not
         * return here.  $ra holds the wild jr's true destination (the
         * longjmp-return emission sets cpu->pc = $ra before returning,
         * which is the same value). */
        g_psx_call_bail = 1;
        g_psx_bail_first++;
#ifndef PSX_OVERLAY_DLL_BUILD
        psx_bail_record(site_ra, site_sp, cpu->gpr[31], cpu->gpr[29]);
#endif
        cpu->pc = cpu->gpr[31];
        return 1;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

/* R3000A load-delay interlock helpers (psx_cyc.h). Included LAST so CPUState is
 * fully defined first; psx_cyc.h's own #include "cpu_state.h" is a guarded no-op.
 * This makes the per-instruction timing API visible to EVERY generated file and
 * runtime TU that includes cpu_state.h. */
#include "psx_cyc.h"

/* Release call-entry stamp — inlined into every generated BIOS/game TU that
 * includes this header (MotK VLC entry tax). Debug builds use the out-of-line
 * ring path in debug_server.c. */
#ifdef PSX_NO_DEBUG_TOOLS
#ifdef __cplusplus
extern "C" {
#endif
extern volatile uint32_t g_psx_last_fn_entry;
static inline void debug_server_log_call_entry(uint32_t func_addr) {
    g_psx_last_fn_entry = func_addr;
}
#ifdef __cplusplus
}
#endif
#endif

#endif /* PSXRECOMP_CPU_STATE_H */
