#ifndef PSXRECOMP_V4_PSX_RUNTIME_H
#define PSXRECOMP_V4_PSX_RUNTIME_H

#include "cpu_state.h"
#include "interrupts.h"
#include "psx_cycles.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline void call_by_address(CPUState* cpu, uint32_t addr) {
    psx_dispatch_call(cpu, addr, cpu->gpr[31]);
}

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_V4_PSX_RUNTIME_H */
