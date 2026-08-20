#ifndef PGXP_HOOKS_H
#define PGXP_HOOKS_H

/* PGXP dataflow-shadowing hook surface (ENHANCEMENTS.md G1.2/G1.3).
 *
 * The runtime keeps a host-only "shadow" of sub-pixel GTE projection results
 * and follows them BY PROVENANCE as the game moves them through registers and
 * RAM into a GP0 packet (pgxp.cpp). Generated code participates by calling
 * these hooks after the guest operation completes.
 *
 * Gating model (the G1.3 decision): the emitter writes PGXP_*() macro
 * invocations unconditionally; this header expands them to real calls only
 * when the translation unit is compiled with -DPSX_PGXP=1 (the pgxp build
 * variant). In the base variant the macros expand to ((void)0), so base
 * objects compile to exactly the pre-feature code — the feature costs nothing
 * unless a title links the pgxp target.
 *
 * RULES for emit sites:
 *   - Macro arguments MUST be side-effect free (they vanish in base builds).
 *   - Hooks run AFTER the guest architectural effect (value = what actually
 *     loaded/stored/resulted), so the shadow can validate against reality.
 *   - `cpu` must be in scope at every emit site (it always is in generated C).
 *
 * The interpreters (dirty_ram_interp.c, psx_interpreter.c) call the
 * psx_pgxp_* functions directly — they are runtime code compiled once, and
 * every hook early-outs on a single global when the feature is off.
 *
 * Overlay DLLs cannot link runtime symbols directly; they forward through the
 * PGXPHooks table below (OverlayCallbacks.pgxp, overlay_api.h). A NULL table
 * no-ops every hook — precision shadowing is a visual enhancement, never
 * load-bearing.
 *
 * LICENSING: this is a clean-room implementation of the publicly documented
 * PGXP technique (psx-spx, public design write-ups). Do NOT port code from
 * the vendored duckstation/ (CC BY-NC-ND) or beetle-psx/ (GPL) trees; they
 * are black-box behavioral oracles only. See ENHANCEMENTS.md G1.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/* Funnel entry points (implemented in runtime/src/pgxp.cpp). The raw
 * instruction word carries the op/rs/rt/rd/imm decode so the emitted call is
 * uniform per class; `value` is the architectural result of the operation.
 *
 *   load:   rt <- [addr]; value = loaded word/half/byte (post-extension)
 *   store:  [addr] <- rt; value = the stored word/half/byte
 *   alu:    rd/rt <- op(s1, s2); result = the written value (also LUI, HILO
 *           moves and MOVE idioms — the body decodes the class from instr)
 *   muldiv: HI/LO <- op(s1, s2)
 *   cop2:   MFC2/CFC2/MTC2/CTC2 (addr = 0) and LWC2/SWC2 (addr = guest addr);
 *           value = the transferred word
 */
void psx_pgxp_load  (struct CPUState *cpu, uint32_t instr, uint32_t addr, uint32_t value);
void psx_pgxp_store (struct CPUState *cpu, uint32_t instr, uint32_t addr, uint32_t value);
void psx_pgxp_alu   (struct CPUState *cpu, uint32_t instr, uint32_t result, uint32_t s1, uint32_t s2);
void psx_pgxp_muldiv(struct CPUState *cpu, uint32_t instr, uint32_t hi, uint32_t lo, uint32_t s1, uint32_t s2);
void psx_pgxp_cop2  (struct CPUState *cpu, uint32_t instr, uint32_t value, uint32_t addr);

/* Forwarder table for overlay DLLs (OverlayCallbacks.pgxp). Appended-last
 * member semantics apply: a NULL pointer (older host) means "no shadowing". */
typedef struct PGXPHooks {
    void (*load)  (struct CPUState *cpu, uint32_t instr, uint32_t addr, uint32_t value);
    void (*store) (struct CPUState *cpu, uint32_t instr, uint32_t addr, uint32_t value);
    void (*alu)   (struct CPUState *cpu, uint32_t instr, uint32_t result, uint32_t s1, uint32_t s2);
    void (*muldiv)(struct CPUState *cpu, uint32_t instr, uint32_t hi, uint32_t lo, uint32_t s1, uint32_t s2);
    void (*cop2)  (struct CPUState *cpu, uint32_t instr, uint32_t value, uint32_t addr);
} PGXPHooks;

#if defined(PSX_PGXP) && PSX_PGXP
/* ALU/MULDIV hooks matter only under tier-2 cpu_mode or the correction
 * consumers' dataflow chains; an NCLIP-only arm leaves them idle. Their hook
 * bodies already early-out, but at ~100M ALU/s the CALL ITSELF was 3.4% of
 * the emu thread (WipEout 3 120 Hz profile) — gate inline on a flag pgxp.cpp
 * maintains (g_pgxp_alu_armed = active && (cpu_mode || full_hooks)). LOAD/
 * STORE/COP2 stay direct: they carry the SXY provenance chain. */
extern int g_pgxp_alu_armed;
#define PGXP_LOAD(instr, addr, val)              psx_pgxp_load(cpu, (instr), (addr), (val))
#define PGXP_STORE(instr, addr, val)             psx_pgxp_store(cpu, (instr), (addr), (val))
#define PGXP_ALU(instr, res, s1, s2)             do { if (g_pgxp_alu_armed) psx_pgxp_alu(cpu, (instr), (res), (s1), (s2)); } while (0)
#define PGXP_MULDIV(instr, hi, lo, s1, s2)       do { if (g_pgxp_alu_armed) psx_pgxp_muldiv(cpu, (instr), (hi), (lo), (s1), (s2)); } while (0)
#define PGXP_COP2(instr, val, addr)              psx_pgxp_cop2(cpu, (instr), (val), (addr))
#else
#define PGXP_LOAD(instr, addr, val)              ((void)0)
#define PGXP_STORE(instr, addr, val)             ((void)0)
#define PGXP_ALU(instr, res, s1, s2)             ((void)0)
#define PGXP_MULDIV(instr, hi, lo, s1, s2)       ((void)0)
#define PGXP_COP2(instr, val, addr)              ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* PGXP_HOOKS_H */
