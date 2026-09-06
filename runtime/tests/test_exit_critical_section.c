/* Exercise the real psx_syscall case, with no BIOS or retail fixture.
 * LTO removes the unused scheduler/exception cases so this focused executable
 * does not need to stub them. The syscall selector remains a constant 2. */
#include "cpu_state.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int psx_syscall(CPUState *cpu, uint32_t code);

int main(void) {
    const uint32_t results[] = {0u, 1u, 0x80010000u, UINT32_MAX};
    const uint32_t statuses[] = {0u, 1u, 0x40000400u, 0x40000401u};
    for (unsigned r = 0; r < sizeof(results) / sizeof(results[0]); ++r) {
        for (unsigned s = 0; s < sizeof(statuses) / sizeof(statuses[0]); ++s) {
            CPUState cpu = {0};
            for (unsigned i = 1; i < 32; ++i)
                cpu.gpr[i] = 0x98760000u + i;
            cpu.gpr[2] = results[r];
            cpu.gpr[4] = 2u;
            cpu.pc = 0x80012340u;
            cpu.hi = 0x12345678u;
            cpu.lo = 0xFEDCBA98u;
            cpu.cop0[12] = statuses[s];
            CPUState expected = cpu;
            expected.cop0[12] |= 0x401u;
            expected.pc = 0u; /* host continuation: resume after SYSCALL */
            if (psx_syscall(&cpu, 0u) != 0 ||
                memcmp(&cpu, &expected, sizeof(cpu)) != 0) {
                fprintf(stderr, "SYS(02h) changed preserved state: v0=%08X SR=%08X\n",
                        results[r], statuses[s]);
                return 1;
            }
        }
    }
    puts("exit_critical_section_test: PASS (16 state combinations)");
    return 0;
}
