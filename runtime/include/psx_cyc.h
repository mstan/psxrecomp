/* psx_cyc.h — R3000A load-delay pipeline interlock (ReadAbsorb / ReadFudge /
 * LDAbsorb / LDWhich).  TIMING ONLY — the load VALUE delay is handled by the
 * existing load-delay correctness path; this models only the CPU cycle cost.
 *
 * FAITHFUL_TIMING_PLAN.md / accuracy/load_readfudge_ldabsorb.md. Transcribed
 * (facts, not code) from the in-tree Beetle oracle
 * psxrecomp/beetle-psx/mednafen/psx/cpu.cpp (RunReal §1 base 795-798, GPR_DEPRES
 * 702-705, DO_LDS 800, ReadMemory 364-451) and libretro.cpp (main-RAM read wait
 * = +3, libretro.cpp:884). Every backend — the dirty-RAM interpreter and BOTH
 * static emitters (code_generator.cpp, full_function_emitter.cpp/strict_translator.cpp)
 * — accounts guest CPU cycles THROUGH THESE HELPERS so they can never disagree.
 *
 * Model (per retired instruction, in program order):
 *   §1   base : if(ReadAbsorb[ReadAbsorbWhich]) ReadAbsorb[ReadAbsorbWhich]--;
 *               else timestamp++;            -- a pending load's give-back is
 *               consumed INSTEAD of the +1 base (pipeline write-back overlap).
 *   deps      : for each GPR this opcode reads/writes, ReadAbsorb[reg]=0 (ends a
 *               give-back when the loaded value is used). ReadAbsorb[0] preserved.
 *   DO_LDS    : commit the PREVIOUS instruction's pending load TIMING:
 *               ReadAbsorb[LDWhich]=LDAbsorb; ReadFudge=LDWhich; ...; LDWhich=0x20.
 *   load only : ReadMemory — clear the current give-back slot, charge the +2 fudge
 *               iff the predecessor committed no load (ReadFudge==0x20), charge the
 *               region wait (main RAM +3) + completion (+2 CPU / +1 LWC2); the
 *               (region+completion) becomes this load's LDAbsorb give-back, armed on
 *               LDWhich=rt for the NEXT instruction's DO_LDS to commit.
 */
#ifndef PSX_CYC_H
#define PSX_CYC_H

#include <stdint.h>
#include <string.h>
#if defined(_MSC_VER)
#include <intrin.h>       /* MSVC intrinsics: _BitScanForward (no __builtin_ctz) */
#endif
#include "cpu_state.h"   /* CPUState (guard-safe: cpu_state.h includes us last) */
#include "psx_cycles.h"  /* inline psx_advance_cycles */

#ifdef __cplusplus
extern "C" {
#endif

/* Load-charge batching (MotK VLC): cache the nearer of the next device
 * deadline or a 64-cycle soft publication limit once per batch. Generated
 * GCC/Clang blocks may defer to their IRQ edge; MMIO also flushes. Pipeline
 * state still updates per instruction, and guest totals at barriers match. */
enum { PSX_CYC_BATCH_SOFT = 64u };

#if defined(PSX_OVERLAY_DLL_BUILD)
/* The overlay shim already batches cycle charges until a host callback edge. */
static inline void psx_cyc_bb_defer_begin(void) { }
static inline void psx_cyc_bb_defer_end(void) { }
static inline void psx_cyc_bb_defer_flush(void) { overlay_flush_cycles(); }
static inline void psx_cyc_local_begin(uint32_t *acc) { (void)acc; }
static inline void psx_cyc_local_end(void) { }
#else
static inline void psx_cyc_bb_defer_begin(void) { g_psx_cyc_bb_defer++; }
static inline void psx_cyc_bb_defer_end(void) {
    if (g_psx_cyc_bb_defer > 0) g_psx_cyc_bb_defer--;
    if (g_psx_cyc_bb_defer == 0) psx_cyc_batch_flush();
}
static inline void psx_cyc_bb_defer_flush(void) { psx_cyc_batch_flush(); }

/* Install a function-local charge sink (MotK VLC leaves). Soft-cap publish
 * at 1<<28 so uint32 cannot wrap; IRQ/MMIO still go through batch_flush. */
static inline void psx_cyc_local_begin(uint32_t *acc) {
#if !defined(PSX_COSIM)
    if (acc) *acc = 0;
    g_psx_cyc_local_acc = acc;
#else
    (void)acc;
#endif
}
static inline void psx_cyc_local_end(void) {
#if !defined(PSX_COSIM)
    psx_cyc_local_publish();
    g_psx_cyc_local_acc = NULL;
#endif
}
#endif

#if defined(__GNUC__) || defined(__clang__)
static inline void psx_cyc_bb_defer_cleanup(int *guard) {
    (void)guard;
    psx_cyc_bb_defer_end();
}
static inline void psx_cyc_local_cleanup(uint32_t **guard) {
    (void)guard;
    psx_cyc_local_end();
}
#endif

static inline void psx_cyc_charge(uint32_t cycles) {
    if (cycles == 0u) return;
#if defined(PSX_OVERLAY_DLL_BUILD)
    psx_advance_cycles(cycles);
#else
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_expect(g_ls_replay_active | g_event_step_conservative, 0)) {
#else
    if (g_ls_replay_active || g_event_step_conservative) {
#endif
        psx_advance_cycles(cycles);
        return;
    }
    if (psx_in_device_service) {
        psx_cycle_count += (uint64_t)cycles;
        return;
    }
#if !defined(PSX_COSIM)
    /* Emitter-gated VLC path: stack local, no deadline probe. */
    if (g_psx_cyc_local_acc) {
        uint32_t prior = *g_psx_cyc_local_acc;
        if (cycles <= UINT32_MAX - prior && (prior + cycles) < (1u << 28)) {
            *g_psx_cyc_local_acc = prior + cycles;
            return;
        }
        *g_psx_cyc_local_acc = 0;
        {
            uint32_t *saved = g_psx_cyc_local_acc;
            g_psx_cyc_local_acc = NULL;
            if (prior) psx_advance_cycles(prior);
            g_psx_cyc_local_acc = saved;
        }
        *g_psx_cyc_local_acc = cycles;
        return;
    }
    {
        uint32_t prior = g_psx_cyc_batch;
        uint32_t sum = prior + cycles;
        if (sum >= prior) { /* no uint32 wrap */
            if (g_psx_cyc_bb_defer > 0) {
                g_psx_cyc_batch = sum;
                return;
            }
            if (prior == 0u) {
                uint64_t room = 0u;
                if (psx_next_service_cycle > psx_cycle_count)
                    room = psx_next_service_cycle - psx_cycle_count;
                g_psx_cyc_batch_limit = room == 0u ? 1u :
                    (room < (uint64_t)PSX_CYC_BATCH_SOFT
                        ? (uint32_t)room : (uint32_t)PSX_CYC_BATCH_SOFT);
            }
            if (sum < g_psx_cyc_batch_limit) {
                g_psx_cyc_batch = sum;
                return;
            }
        }
    }
#endif
    psx_advance_cycles(cycles); /* publishes any pending batch first */
#endif
}

/* §1 base (Beetle cpu.cpp:795-798). */
static inline void psx_cyc_base(CPUState* cpu) {
    uint8_t w = cpu->read_absorb_which;
    if (cpu->read_absorb[w]) cpu->read_absorb[w]--;
    else                     psx_cyc_charge(1u);
}

/* GPR_DEPRES (Beetle cpu.cpp:702-705): zero ReadAbsorb[n] for every source/dest
 * GPR of this instruction, preserving ReadAbsorb[0] (skipping bit 0 == Beetle's
 * save/restore of ReadAbsorb[0]). */
static inline void psx_cyc_deps(CPUState* cpu, uint32_t reg_mask) {
    reg_mask &= 0xFFFFFFFEu;   /* never touch ReadAbsorb[0] */
    if (reg_mask && (reg_mask & (reg_mask - 1u)) == 0u) {
#if defined(_MSC_VER)
        unsigned long n;
        _BitScanForward(&n, reg_mask);
        cpu->read_absorb[(unsigned)n] = 0u;
#else
        cpu->read_absorb[(unsigned)__builtin_ctz(reg_mask)] = 0u;
#endif
        return;
    }
    while (reg_mask) {
#if defined(_MSC_VER)
        unsigned long _psx_ctz_idx;
        _BitScanForward(&_psx_ctz_idx, reg_mask);
        unsigned n = (unsigned)_psx_ctz_idx;
#else
        unsigned n = (unsigned)__builtin_ctz(reg_mask);
#endif
        cpu->read_absorb[n] = 0u;
        reg_mask &= reg_mask - 1u;
    }
}

/* DO_LDS timing-commit (Beetle cpu.cpp:800). LDWhich==0x20 (no pending) writes the
 * dummy slot read_absorb[32] and sets read_fudge=0x20 (=> next load gets +2 fudge). */
static inline void psx_cyc_lds(CPUState* cpu) {
    uint8_t lw = cpu->ld_which_t;
    cpu->read_absorb[lw]    = (uint8_t)cpu->ld_absorb;
    cpu->read_fudge         = lw;
    cpu->read_absorb_which  = (uint8_t)(cpu->read_absorb_which | (lw & 0x1Fu));
    cpu->ld_which_t         = 0x20u;
}

/* Full per-instruction interlock for a NON-CPU-load instruction (ALU, shift,
 * branch, jump, store, COP control, LWC2/SWC2 pre-step, mult/div, mfhi/mflo, ...).
 * reg_mask from psx_cyc_dep_res_mask(). MUST be emitted BEFORE the instruction
 * body so §1 precedes any muldiv/GTE deadline stall in the body (Beetle order). */
static inline void psx_cyc_step(CPUState* cpu, uint32_t reg_mask) {
    psx_cyc_base(cpu);
    psx_cyc_deps(cpu, reg_mask);
    psx_cyc_lds(cpu);
}

/* The GPR dep+res bitmask used by psx_cyc_step lives in psx_instr_cost.h
 * (psx_cyc_dep_res_mask) — a standalone pure function shared by the emitters
 * (gen-time literal) and the interpreter (runtime), with no CPUState dependency. */

/* Main-RAM base + load-delay gate (memory.c). Inlined load helpers use these
 * so MotK VLC / decode hot paths avoid an out-of-line call per LW/LH. */
extern uint8_t *g_psx_ram;
extern int      g_psx_load_delay;
extern int      g_ls_mode;
extern volatile int g_ds_recording;
int psx_load_delay_enabled(void);
uint32_t psx_cyc_load_word_slow(CPUState* cpu, uint32_t addr, uint32_t rt, uint32_t reg_mask);
uint16_t psx_cyc_load_half_slow(CPUState* cpu, uint32_t addr, uint32_t rt, uint32_t reg_mask);

/* Overlay DLLs route timed loads through the host callback shim so they share
 * runtime timing and memory state. Normal generated code keeps the main-RAM
 * fast path inline. */
#if defined(PSX_OVERLAY_DLL_BUILD)
uint32_t psx_cyc_load_word(CPUState* cpu, uint32_t addr,
                           uint32_t rt, uint32_t reg_mask);
uint16_t psx_cyc_load_half(CPUState* cpu, uint32_t addr,
                           uint32_t rt, uint32_t reg_mask);
#else
/* CPU data load value+timing. Full Beetle sequence for main RAM is inlined;
 * MMIO / lockstep / data-shard fall through to *_slow in memory.c. */
static inline uint32_t psx_cyc_load_word(CPUState* cpu, uint32_t addr,
                                          uint32_t rt, uint32_t reg_mask) {
#ifdef PSX_ENABLE_BLOCK_CYCLES
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (g_ls_mode == 0 && !g_ds_recording && phys < 0x00800000u) {
        if (g_psx_load_delay < 0) (void)psx_load_delay_enabled();
        if (g_psx_load_delay) {
            psx_cyc_base(cpu);
            psx_cyc_deps(cpu, reg_mask);
            if (cpu->ld_which_t == rt) cpu->ld_which_t = 0u;
            psx_cyc_lds(cpu);
            cpu->read_absorb[cpu->read_absorb_which] = 0u;
            cpu->read_absorb_which = 0u;
            uint32_t fudge = (uint32_t)((cpu->read_fudge >> 4) & 2u);
            cpu->ld_absorb = 5u; /* main-RAM wait 3 + completion 2 */
            psx_cyc_charge(fudge + 5u);
            cpu->ld_which_t = (uint8_t)rt;
        }
        uint32_t value;
        memcpy(&value, g_psx_ram + (phys & 0x1FFFFFu), sizeof(value));
        return value;
    }
    return psx_cyc_load_word_slow(cpu, addr, rt, reg_mask);
#else
    (void)cpu; (void)rt; (void)reg_mask;
    extern uint32_t psx_read_word(uint32_t a);
    return psx_read_word(addr);
#endif
}

static inline uint16_t psx_cyc_load_half(CPUState* cpu, uint32_t addr,
                                          uint32_t rt, uint32_t reg_mask) {
#ifdef PSX_ENABLE_BLOCK_CYCLES
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (g_ls_mode == 0 && !g_ds_recording && phys < 0x00800000u) {
        if (g_psx_load_delay < 0) (void)psx_load_delay_enabled();
        if (g_psx_load_delay) {
            psx_cyc_base(cpu);
            psx_cyc_deps(cpu, reg_mask);
            if (cpu->ld_which_t == rt) cpu->ld_which_t = 0u;
            psx_cyc_lds(cpu);
            cpu->read_absorb[cpu->read_absorb_which] = 0u;
            cpu->read_absorb_which = 0u;
            uint32_t fudge = (uint32_t)((cpu->read_fudge >> 4) & 2u);
            cpu->ld_absorb = 5u;
            psx_cyc_charge(fudge + 5u);
            cpu->ld_which_t = (uint8_t)rt;
        }
        uint16_t value;
        memcpy(&value, g_psx_ram + (phys & 0x1FFFFFu), sizeof(value));
        return value;
    }
    return psx_cyc_load_half_slow(cpu, addr, rt, reg_mask);
#else
    (void)cpu; (void)rt; (void)reg_mask;
    extern uint16_t psx_read_half(uint32_t a);
    return psx_read_half(addr);
#endif
}
#endif

extern uint8_t psx_cyc_load_byte(CPUState* cpu, uint32_t addr, uint32_t rt, uint32_t reg_mask);

/* Charge the exact timing/pipeline effects of a 32-bit CPU load without
 * invoking the memory handler.  Enhancement HLE may use this only when static
 * analysis proves the loaded value and read side effects are unobservable. */
extern void psx_cyc_load_word_timing_only(CPUState* cpu, uint32_t addr,
                                           uint32_t rt, uint32_t reg_mask);

/* LWC2 (GTE load) ReadMemory timing only (completion +1, NO LDWhich arm — the dest
 * is a GTE register). Call AFTER psx_cyc_step(cpu,0) (§1+DO_LDS) and psx_gte_stall.
 * Returns the raw 32-bit value. */
extern uint32_t psx_cyc_lwc2_read(CPUState* cpu, uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif /* PSX_CYC_H */
