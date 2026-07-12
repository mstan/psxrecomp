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
#if defined(_MSC_VER)
#include <intrin.h>       /* MSVC intrinsics: _BitScanForward (no __builtin_ctz) */
#endif
#include "cpu_state.h"   /* CPUState (guard-safe: cpu_state.h includes us last) */

#ifdef __cplusplus
extern "C" {
#endif

extern void psx_advance_cycles(uint32_t cycles);

/* §1 base (Beetle cpu.cpp:795-798). */
static inline void psx_cyc_base(CPUState* cpu) {
    uint8_t w = cpu->read_absorb_which;
    if (cpu->read_absorb[w]) cpu->read_absorb[w]--;
    else                     psx_advance_cycles(1u);
}

/* GPR_DEPRES (Beetle cpu.cpp:702-705): zero ReadAbsorb[n] for every source/dest
 * GPR of this instruction, preserving ReadAbsorb[0] (skipping bit 0 == Beetle's
 * save/restore of ReadAbsorb[0]). */
static inline void psx_cyc_deps(CPUState* cpu, uint32_t reg_mask) {
    reg_mask &= 0xFFFFFFFEu;   /* never touch ReadAbsorb[0] */
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

/* CPU data load value+timing (memory.c). Does the full Beetle per-instruction
 * sequence: §1 base + GPR_DEPRES(reg_mask) + (LDWhich==rt cancel) + DO_LDS +
 * ReadMemory timing (clear give-back, fudge, region+completion, arm LDAbsorb +
 * LDWhich=rt), and returns the raw value (caller applies the same width/sign as the
 * prior cpu->read_* call). For LWL/LWR pass the WORD-ALIGNED address. */
extern uint32_t psx_cyc_load_word(CPUState* cpu, uint32_t addr, uint32_t rt, uint32_t reg_mask);
extern uint16_t psx_cyc_load_half(CPUState* cpu, uint32_t addr, uint32_t rt, uint32_t reg_mask);
extern uint8_t  psx_cyc_load_byte(CPUState* cpu, uint32_t addr, uint32_t rt, uint32_t reg_mask);

/* LWC2 (GTE load) ReadMemory timing only (completion +1, NO LDWhich arm — the dest
 * is a GTE register). Call AFTER psx_cyc_step(cpu,0) (§1+DO_LDS) and psx_gte_stall.
 * Returns the raw 32-bit value. */
extern uint32_t psx_cyc_lwc2_read(CPUState* cpu, uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif /* PSX_CYC_H */
