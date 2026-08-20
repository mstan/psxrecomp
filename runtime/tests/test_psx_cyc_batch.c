#include "psx_cyc.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

uint64_t psx_cycle_count = 0;
uint64_t psx_next_service_cycle = 0;
uint32_t g_psx_cyc_batch = 0;
uint32_t g_psx_cyc_batch_limit = 0;
uint64_t g_psx_cycle_fast_limit = 0;
int g_psx_cyc_bb_defer = 0;
uint32_t *g_psx_cyc_local_acc = 0;
uint32_t g_psx_oc_scale_q16 = 65536u;
uint32_t g_psx_oc_accum = 0;
int psx_in_device_service = 0;
int g_event_step_conservative = 0;
int g_ls_replay_active = 0;
int g_ls_mode = 0;
volatile int g_ds_recording = 0;
static uint8_t test_ram[0x200000];
uint8_t *g_psx_ram = test_ram;
int g_psx_load_delay = 1;
uint32_t g_psx_ram_size = 0x00200000u;
uint32_t g_psx_ram_mask = 0x001FFFFFu;

int psx_load_delay_enabled(void) { return g_psx_load_delay; }
uint32_t psx_cyc_load_word_slow(CPUState *cpu, uint32_t addr,
                                uint32_t rt, uint32_t mask) {
    (void)cpu; (void)addr; (void)rt; (void)mask;
    assert(!"main-RAM load unexpectedly took the slow path");
    return 0u;
}
uint16_t psx_cyc_load_half_slow(CPUState *cpu, uint32_t addr,
                                uint32_t rt, uint32_t mask) {
    (void)cpu; (void)addr; (void)rt; (void)mask;
    assert(!"main-RAM load unexpectedly took the slow path");
    return 0u;
}

static int service_count;

void psx_devices_service_to_now(void) {
    service_count++;
    psx_next_service_cycle = psx_cycle_count + 1000u;
}

void psx_advance_cycles_slow(uint32_t cycles) {
    psx_cycle_count += cycles;
}

static void reset_clock(uint64_t deadline) {
    psx_cycle_count = 0;
    psx_next_service_cycle = deadline;
    g_psx_cyc_batch = 0;
    g_psx_cyc_batch_limit = 0;
    g_psx_cycle_fast_limit = 0;
    g_psx_cyc_bb_defer = 0;
    g_psx_cyc_local_acc = 0;
    g_psx_oc_scale_q16 = 65536u;
    g_psx_oc_accum = 0u;
    psx_in_device_service = 0;
    g_event_step_conservative = 0;
    g_ls_replay_active = 0;
    service_count = 0;
}

static void reset_pipe(CPUState *cpu, uint32_t seed) {
    memset(cpu, 0, sizeof(*cpu));
    for (uint32_t i = 0; i < 33u; i++)
        cpu->read_absorb[i] = (uint8_t)((seed + i * 7u) % 9u);
    cpu->read_absorb_which = (uint8_t)(seed & 31u);
    cpu->read_fudge = (uint8_t)((seed >> 5) & 31u);
    cpu->ld_which_t = (uint8_t)((seed >> 10) % 33u);
    cpu->ld_absorb = (seed >> 15) & 7u;
}

static uint32_t reference_load_word(CPUState *cpu, uint32_t addr,
                                    uint32_t rt, uint32_t reg_mask) {
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
    uint32_t value;
    memcpy(&value, g_psx_ram + psx_ram_map_read(addr), sizeof(value));
    return value;
}

typedef struct StepResult {
    CPUState cpu;
    uint64_t cycle_count;
    uint32_t batch;
    uint32_t batch_limit;
    uint32_t oc_accum;
    uint32_t local_acc;
    int service_count;
} StepResult;

static StepResult run_step(uint32_t seed, uint32_t mask,
                           const uint32_t regs[3], uint32_t count,
                           uint32_t mode, int specialized) {
    StepResult out;
    uint32_t local_acc = 0u;

    reset_clock(1000000u);
    if (mode == 0u) {
        g_psx_cyc_bb_defer = 1;
    } else if (mode == 1u) {
        g_psx_cyc_bb_defer = 1;
        g_psx_cyc_local_acc = &local_acc;
    } else if (mode == 2u) {
        /* Ordinary deadline-bounded batch path. */
    } else if (mode == 3u) {
        g_event_step_conservative = 1;
    } else if (mode == 4u) {
        psx_in_device_service = 1;
    } else {
        /* The production 900% clock scale: compare carried fractional state at
         * the publication barrier, not just the raw deferred total. */
        g_psx_cyc_bb_defer = 1;
        g_psx_oc_scale_q16 = (uint32_t)((65536ull * 100ull) / 900ull);
        g_psx_oc_accum = seed & 0xffffu;
    }

    reset_pipe(&out.cpu, seed);
    if (!specialized) psx_cyc_step(&out.cpu, mask);
    else if (count == 0u) psx_cyc_step_0(&out.cpu);
    else if (count == 1u) psx_cyc_step_1(&out.cpu, regs[0]);
    else if (count == 2u) psx_cyc_step_2(&out.cpu, regs[0], regs[1]);
    else psx_cyc_step_3(&out.cpu, regs[0], regs[1], regs[2]);
    psx_cyc_batch_flush();

    out.cycle_count = psx_cycle_count;
    out.batch = g_psx_cyc_batch;
    out.batch_limit = g_psx_cyc_batch_limit;
    out.oc_accum = g_psx_oc_accum;
    out.local_acc = local_acc;
    out.service_count = service_count;
    g_psx_cyc_local_acc = 0;
    return out;
}

static void compare_specialized(uint32_t seed, uint32_t mask,
                                const uint32_t regs[3], uint32_t count,
                                uint32_t mode) {
    CPUState generic, specialized;
    StepResult generic_result = run_step(seed, mask, regs, count, mode, 0);
    StepResult specialized_result = run_step(seed, mask, regs, count, mode, 1);
    generic = generic_result.cpu;
    specialized = specialized_result.cpu;
    assert(memcmp(&generic, &specialized, sizeof(generic)) == 0);
    assert(generic_result.cycle_count == specialized_result.cycle_count);
    assert(generic_result.batch == specialized_result.batch);
    assert(generic_result.batch_limit == specialized_result.batch_limit);
    assert(generic_result.oc_accum == specialized_result.oc_accum);
    assert(generic_result.local_acc == specialized_result.local_acc);
    assert(generic_result.service_count == specialized_result.service_count);
}

typedef struct ClockResult {
    uint64_t cycle_count;
    uint64_t next_service_cycle;
    uint32_t oc_accum;
    int service_count;
} ClockResult;

static ClockResult run_fast_charge(uint64_t start, uint64_t limit,
                                   uint32_t scale, uint32_t accum,
                                   uint32_t cycles, int use_fast) {
    ClockResult out;
    reset_clock(limit ? limit + 1u : start + 1u);
    psx_cycle_count = start;
    g_psx_cycle_fast_limit = limit;
    g_psx_oc_scale_q16 = scale;
    g_psx_oc_accum = accum;
    if (!use_fast || !psx_cycles_try_fast_charge(cycles))
        psx_advance_cycles(cycles);
    out.cycle_count = psx_cycle_count;
    out.next_service_cycle = psx_next_service_cycle;
    out.oc_accum = g_psx_oc_accum;
    out.service_count = service_count;
    return out;
}

int main(void) {
    reset_clock(5u);
    for (int i = 0; i < 4; i++) psx_cyc_charge(1u);
    assert(psx_cycle_count == 0u);
    assert(g_psx_cyc_batch == 4u);
    assert(service_count == 0);

    psx_cyc_charge(1u);
    assert(psx_cycle_count == 5u);
    assert(g_psx_cyc_batch == 0u);
    assert(service_count == 1);

    reset_clock(1000u);
    for (int i = 0; i < 63; i++) psx_cyc_charge(1u);
    assert(psx_cycle_count == 0u);
    assert(g_psx_cyc_batch == 63u);
    psx_cyc_charge(1u);
    assert(psx_cycle_count == 64u);
    assert(g_psx_cyc_batch == 0u);
    assert(service_count == 0);

    reset_clock(5u);
    psx_cyc_bb_defer_begin();
    for (int i = 0; i < 10; i++) psx_cyc_charge(1u);
    assert(psx_cycle_count == 0u);
    assert(g_psx_cyc_batch == 10u);
    psx_cyc_bb_defer_flush();
    assert(psx_cycle_count == 10u);
    assert(service_count == 1);
    assert(g_psx_cyc_bb_defer == 1);
    psx_cyc_bb_defer_end();
    assert(g_psx_cyc_bb_defer == 0);

    reset_clock(0u);
    psx_cyc_charge(1u);
    assert(psx_cycle_count == 1u);
    assert(service_count == 1);

    /* Emitter-level local accumulator: charges stay off the published clock
     * until local_publish / batch_flush; guest total at the barrier matches. */
    reset_clock(1000u);
    {
        uint32_t acc = 0;
        psx_cyc_local_begin(&acc);
        for (int i = 0; i < 100; i++) psx_cyc_charge(5u);
        assert(psx_cycle_count == 0u);
        assert(g_psx_cyc_batch == 0u);
        assert(acc == 500u);
        psx_cyc_bb_defer_flush();
        assert(psx_cycle_count == 500u);
        assert(acc == 0u);
        psx_cyc_local_end();
        assert(g_psx_cyc_local_acc == 0);
    }

    /* Exhaustive emitted-mask arities over varied pipeline states.  These
     * compare every CPUState byte plus the deferred cycle total against the
     * authoritative generic helper. */
    {
        const uint32_t r0[3] = {0, 0, 0};
        const uint32_t r1[3] = {3, 0, 0};
        const uint32_t r2[3] = {2, 29, 0};
        const uint32_t r3[3] = {1, 17, 31};
        for (uint32_t seed = 0; seed < 4096u; seed++) {
            for (uint32_t mode = 0; mode < 6u; mode++) {
                compare_specialized(seed, 0u, r0, 0u, mode);
                compare_specialized(seed, 1u << r1[0], r1, 1u, mode);
                compare_specialized(seed, (1u << r2[0]) | (1u << r2[1]), r2, 2u, mode);
                compare_specialized(seed,
                    (1u << r3[0]) | (1u << r3[1]) | (1u << r3[2]), r3, 3u, mode);
            }
        }
    }

    /* Combining base+wait+completion into one publication at the load value
     * boundary must preserve the generic pipeline oracle byte-for-byte. */
    {
        const uint32_t addr = 0x100u;
        const uint32_t mask = (1u << 4) | (1u << 7);
        memcpy(test_ram + addr, "R3OK", 4u);
        for (uint32_t seed = 0; seed < 2048u; seed++) {
            for (uint32_t overclock = 0; overclock < 2u; overclock++) {
                CPUState reference, combined;
                uint64_t ref_cycle;
                uint32_t ref_accum, ref_batch, ref_value;

                reset_clock(1000000u);
                g_psx_cyc_bb_defer = 1;
                g_psx_cyc_batch = seed % 7u;
                g_psx_oc_scale_q16 = overclock
                    ? (uint32_t)((65536ull * 100ull) / 900ull) : 65536u;
                g_psx_oc_accum = seed & 0xffffu;
                reset_pipe(&reference, seed);
                combined = reference;
                ref_value = reference_load_word(&reference, addr, 7u, mask);
                psx_cyc_batch_flush();
                ref_cycle = psx_cycle_count;
                ref_accum = g_psx_oc_accum;
                ref_batch = g_psx_cyc_batch;

                reset_clock(1000000u);
                g_psx_cyc_bb_defer = 1;
                g_psx_cyc_batch = seed % 7u;
                g_psx_oc_scale_q16 = overclock
                    ? (uint32_t)((65536ull * 100ull) / 900ull) : 65536u;
                g_psx_oc_accum = seed & 0xffffu;
                uint32_t combined_value = psx_cyc_load_word(
                    &combined, addr, 7u, mask);
                psx_cyc_batch_flush();
                assert(combined_value == ref_value);
                assert(memcmp(&combined, &reference, sizeof(combined)) == 0);
                assert(psx_cycle_count == ref_cycle);
                assert(g_psx_oc_accum == ref_accum);
                assert(g_psx_cyc_batch == ref_batch);
            }
        }
    }

    /* The pre-deadline shortcut is a strict oracle match, including the Q16
     * carried remainder required by the production 900% overclock. Boundary
     * crossings must miss and run the canonical service path exactly once. */
    {
        const uint32_t scales[] = {65536u, (uint32_t)((65536ull * 100ull) / 900ull)};
        const uint32_t charges[] = {1u, 3u, 20u};
        for (uint32_t si = 0; si < 2u; si++) {
            for (uint32_t ai = 0; ai < 257u; ai += 17u) {
                for (uint32_t ci = 0; ci < 3u; ci++) {
                    for (uint64_t room = 0; room < 12u; room++) {
                        uint64_t start = 1000u;
                        uint64_t limit = room ? start + room - 1u : 0u;
                        ClockResult canonical = run_fast_charge(
                            start, limit, scales[si], ai * 251u,
                            charges[ci], 0);
                        ClockResult fast = run_fast_charge(
                            start, limit, scales[si], ai * 251u,
                            charges[ci], 1);
                        assert(memcmp(&canonical, &fast, sizeof(canonical)) == 0);
                    }
                }
            }
        }
    }
    return 0;
}
