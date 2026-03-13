#ifndef PSX_RUNTIME_H
#define PSX_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MIPS R3000A register file + GTE coprocessor registers + memory callbacks.
 * This struct is the only coupling between generated code and the runtime. */
typedef struct CPUState {
    uint32_t zero, at, v0, v1, a0, a1, a2, a3;
    uint32_t t0, t1, t2, t3, t4, t5, t6, t7;
    uint32_t s0, s1, s2, s3, s4, s5, s6, s7;
    uint32_t t8, t9, k0, k1, gp, sp, fp, ra;
    uint32_t pc, hi, lo;
    uint32_t cop0[32];
    uint32_t gte_data[32];
    uint32_t gte_ctrl[32];

    uint32_t (*read_word)(uint32_t addr);
    void     (*write_word)(uint32_t addr, uint32_t value);
    uint16_t (*read_half)(uint32_t addr);
    void     (*write_half)(uint32_t addr, uint16_t value);
    uint8_t  (*read_byte)(uint32_t addr);
    void     (*write_byte)(uint32_t addr, uint8_t value);
    uint32_t (*lwl)(uint32_t addr, uint32_t rt_value);
    uint32_t (*lwr)(uint32_t addr, uint32_t rt_value);
    void     (*swl)(uint32_t addr, uint32_t rt_value);
    void     (*swr)(uint32_t addr, uint32_t rt_value);
} CPUState;

/* Initialize a CPUState and install memory callbacks. Call before running any
 * recompiled function. */
void psx_runtime_init(CPUState* cpu);

/* Load bytes into PS1 RAM at the given virtual address. */
void psx_runtime_load(uint32_t addr, const uint8_t* data, uint32_t size);

/* MMIO tracing — logs all hardware register accesses to mmio_trace.log.
 * Call with enable=1 after psx_runtime_init(). */
void psx_mmio_trace_enable(int enable);

/* Install signal handlers for SIGSEGV/SIGABRT to dump CPU state on crash. */
void psx_install_crash_handler(void);

/* Dynamic call dispatch — handles BIOS calls and jump-register targets.
 * Logs any address not yet implemented. Never crashes on unknown calls. */
void call_by_address(CPUState* cpu, uint32_t addr);

/* Override gate — emitted by code_generator at the top of every function.
 * Returns 1 if intercepted (body skipped), 0 if normal execution continues.
 * Default: always returns 0. Add real implementations in runtime.c when needed. */
int psx_override_dispatch(CPUState* cpu, uint32_t addr);

/* SYSCALL handler — emitted for SYSCALL instructions. */
void psx_syscall(CPUState* cpu, uint32_t code);

/* GTE coprocessor — implemented in gte.cpp. */
void gte_execute(CPUState* cpu, uint32_t cmd);

/* GTE register transfer helpers (for overlay interpreter) */
uint32_t gte_read_data(CPUState* cpu, uint8_t reg);
uint32_t gte_read_ctrl(CPUState* cpu, uint8_t reg);
void     gte_write_data(CPUState* cpu, uint8_t reg, uint32_t val);
void     gte_write_ctrl(CPUState* cpu, uint8_t reg, uint32_t val);

/* Direct access to PS1 RAM (2MB) and scratchpad (1KB). Used by automation scripts. */
uint8_t* psx_get_ram(void);
uint8_t* psx_get_scratch(void);

/* Frame-gated GP1 display command tracker (called from gpu_write_gp1). */
void diag_track_gp1(uint32_t cmd);

#ifdef __cplusplus
}
#endif

#endif /* PSX_RUNTIME_H */
