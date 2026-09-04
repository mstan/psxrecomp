/* psx_interpreter.h — R3000A interpreter for oracle builds.
 *
 * Oracle-only: provides a cycle-accurate reference implementation
 * of the PSX CPU. Shares the same CPUState and memory bus as the
 * recompiled native build. The native build links stub_interpreter.c
 * instead (all functions are no-ops).
 *
 * Follows the Genesis/clown68000 pattern: two CMake targets from one
 * source tree, gated by PSX_ORACLE_BUILD.
 */

#ifndef PSXRECOMP_PSX_INTERPRETER_H
#define PSXRECOMP_PSX_INTERPRETER_H

#include "cpu_state.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize interpreter state. Call once after CPUState is allocated. */
void interp_init(CPUState* cpu);

/* Drop host-static pending load-delay (not in boot_state). See
 * dirty_ram_ld_delay_discard — same restore contract for the debug interp. */
void interp_ld_delay_discard(void);

/* Execute exactly `count` instructions. Returns number actually executed
 * (may be less if a breakpoint fires). */
uint32_t interp_step(CPUState* cpu, uint32_t count);

/* Run until breakpoint, halt, or max_instructions reached. */
uint32_t interp_run(CPUState* cpu, uint32_t max_instructions);

/* Check if interpreter hit a breakpoint on the last run. */
int interp_hit_breakpoint(void);

/* --- PC breakpoints --- */
int  interp_break_add(uint32_t pc);       /* returns slot, or -1 */
int  interp_break_remove(uint32_t pc);    /* returns 1 if removed */
void interp_break_clear_all(void);

/* --- Per-instruction trace ring --- */
typedef struct {
    uint32_t pc;
    uint32_t gpr[32];
    uint32_t hi, lo;
    uint32_t cop0_sr, cop0_cause, cop0_epc;
    uint32_t insn;        /* raw instruction word */
    uint64_t seq;         /* monotonic sequence number */
} InterpTraceEntry;

void     interp_trace_enable(int on);
uint64_t interp_trace_count(void);
/* Get entry by index into ring (0 = oldest available). Returns NULL if OOB. */
const InterpTraceEntry* interp_trace_get(uint32_t ring_idx);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_PSX_INTERPRETER_H */
