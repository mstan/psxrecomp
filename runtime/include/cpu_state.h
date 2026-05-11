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

#ifndef PSXRECOMP_V4_CPU_STATE_H
#define PSXRECOMP_V4_CPU_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
} CPUState;

/* Trap trampolines — defined in runtime/src/traps.c */
extern void psx_syscall(CPUState* cpu, uint32_t code);
extern void psx_arith_overflow(CPUState* cpu);
extern void psx_unaligned_access(CPUState* cpu, uint32_t addr, uint32_t pc);
extern void psx_break(CPUState* cpu, uint32_t code, uint32_t pc);

/* Dispatch — defined in generated/SCPH1001_dispatch.c */
extern void psx_dispatch(CPUState* cpu, uint32_t target_addr);
extern void psx_dispatch_call(CPUState* cpu, uint32_t target_addr, uint32_t return_addr);

/* Unknown dispatch — defined in runtime/src/traps.c */
extern void psx_unknown_dispatch(CPUState* cpu, uint32_t addr, uint32_t phys);

/* GTE (COP2) — defined in runtime/src/gte.cpp */
extern void     gte_execute(CPUState* cpu, uint32_t cmd);
extern uint32_t gte_read_data(CPUState* cpu, uint8_t reg);
extern uint32_t gte_read_ctrl(CPUState* cpu, uint8_t reg);
extern void     gte_write_data(CPUState* cpu, uint8_t reg, uint32_t val);
extern void     gte_write_ctrl(CPUState* cpu, uint8_t reg, uint32_t val);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_V4_CPU_STATE_H */
