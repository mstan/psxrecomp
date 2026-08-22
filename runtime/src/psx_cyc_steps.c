/* Shared generated-code specializations for R3000A load-delay timing. */

#include "psx_cyc.h"

/* Generated functions have psx_cyc_bb_defer active until every IRQ/MMIO
 * observation barrier.  In that exact context the generic psx_cyc_charge path
 * necessarily ends by adding to g_psx_cyc_batch, so do that directly.  Every
 * exceptional mode takes the unchanged generic path. */
#if !defined(PSX_ENABLE_BLOCK_CYCLES)
static inline void psx_cyc_generated_base(CPUState *cpu)
{
    uint8_t w = cpu->read_absorb_which;
    if (cpu->read_absorb[w]) {
        cpu->read_absorb[w]--;
        return;
    }

#if !defined(PSX_COSIM)
    if (g_psx_cyc_bb_defer > 0 && g_psx_cyc_local_acc == NULL &&
        !g_ls_replay_active && !g_event_step_conservative &&
        !psx_in_device_service && g_psx_cyc_batch != UINT32_MAX) {
        g_psx_cyc_batch++;
        return;
    }
#endif
    psx_cyc_charge(1u);
}

/* These four wrappers are shared deliberately.  LTO sees constant register
 * arguments at generated call sites and otherwise clones the interlock body
 * throughout generated code, growing the hot loop and regressing I-cache
 * throughput.  Keep psx_cyc_generated_base inline inside one shared wrapper,
 * but keep the wrappers themselves as call boundaries. */
#if defined(_MSC_VER)
#define PSX_CYC_STEP_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define PSX_CYC_STEP_NOINLINE __attribute__((noinline))
#else
#define PSX_CYC_STEP_NOINLINE
#endif

PSX_CYC_STEP_NOINLINE void psx_cyc_step_0(CPUState* cpu)
{
    psx_cyc_generated_base(cpu);
    psx_cyc_lds(cpu);
}

PSX_CYC_STEP_NOINLINE void psx_cyc_step_1(CPUState* cpu, uint32_t r0)
{
    psx_cyc_generated_base(cpu);
    cpu->read_absorb[r0] = 0u;
    psx_cyc_lds(cpu);
}

PSX_CYC_STEP_NOINLINE void psx_cyc_step_2(CPUState* cpu, uint32_t r0, uint32_t r1)
{
    psx_cyc_generated_base(cpu);
    cpu->read_absorb[r0] = 0u;
    cpu->read_absorb[r1] = 0u;
    psx_cyc_lds(cpu);
}

PSX_CYC_STEP_NOINLINE void psx_cyc_step_3(CPUState* cpu, uint32_t r0, uint32_t r1, uint32_t r2)
{
    psx_cyc_generated_base(cpu);
    cpu->read_absorb[r0] = 0u;
    cpu->read_absorb[r1] = 0u;
    cpu->read_absorb[r2] = 0u;
    psx_cyc_lds(cpu);
}
#endif

#undef PSX_CYC_STEP_NOINLINE
