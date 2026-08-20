/* Deterministic generated-code/runtime boundary microbenchmark.
 *
 * This is not a game benchmark. It executes a fixed, generated-style mix of
 * 32 guest instructions per block through the production cycle-step, timed
 * main-RAM load, instruction-cache, and 900% overclock paths. The final guest
 * cycle count and state digest make optimizer-induced semantic drift visible.
 */

#include "psx_cyc.h"
#include "psx_icache.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

enum {
    BENCH_BLOCK_GUEST_INSNS = 32,
    BENCH_DEFAULT_BLOCKS = 4000000,
    BENCH_SAMPLES = 7,
    BENCH_RAM_SIZE = 0x00200000
};

uint64_t psx_cycle_count = 0;
uint64_t psx_next_service_cycle = 0;
uint32_t g_psx_cyc_batch = 0;
uint32_t g_psx_cyc_batch_limit = 0;
uint64_t g_psx_cycle_fast_limit = 0;
int g_psx_cyc_bb_defer = 0;
uint32_t *g_psx_cyc_local_acc = NULL;
uint32_t g_psx_oc_scale_q16 = 65536u;
uint32_t g_psx_oc_accum = 0;
int psx_in_device_service = 0;
int g_event_step_conservative = 0;
int g_ls_replay_active = 0;
int g_ls_mode = 0;
volatile int g_ds_recording = 0;
int g_psx_load_delay = 1;
uint32_t g_psx_ram_size = BENCH_RAM_SIZE;
uint32_t g_psx_ram_mask = BENCH_RAM_SIZE - 1u;

static uint8_t bench_ram[BENCH_RAM_SIZE];
uint8_t *g_psx_ram = bench_ram;

static uint64_t service_count = 0;

int psx_load_delay_enabled(void) {
    return g_psx_load_delay;
}

uint32_t psx_cyc_load_word_slow(CPUState *cpu, uint32_t addr,
                                uint32_t rt, uint32_t reg_mask) {
    (void)cpu;
    (void)addr;
    (void)rt;
    (void)reg_mask;
    fputs("unexpected slow word load\n", stderr);
    abort();
}

uint16_t psx_cyc_load_half_slow(CPUState *cpu, uint32_t addr,
                                uint32_t rt, uint32_t reg_mask) {
    (void)cpu;
    (void)addr;
    (void)rt;
    (void)reg_mask;
    fputs("unexpected slow half load\n", stderr);
    abort();
}

void psx_devices_service_to_now(void) {
    service_count++;
    psx_next_service_cycle = psx_cycle_count + UINT64_C(0x100000000);
}

void psx_advance_cycles_slow(uint32_t cycles) {
    psx_cycle_count += cycles;
}

#if defined(_MSC_VER)
#define BENCH_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define BENCH_NOINLINE __attribute__((noinline, aligned(64)))
#else
#define BENCH_NOINLINE
#endif

static double monotonic_seconds(void) {
#if defined(_WIN32)
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1.0e-9;
#endif
}

static void reset_workload(CPUState *cpu) {
    memset(cpu, 0, sizeof(*cpu));
    for (uint32_t i = 0; i < 32u; i++)
        cpu->gpr[i] = UINT32_C(0x9E3779B9) * (i + 1u);
    cpu->read_fudge = 0x20u;
    cpu->ld_which_t = 0x20u;

    psx_cycle_count = 0;
    psx_next_service_cycle = UINT64_C(0x100000000);
    g_psx_cyc_batch = 0;
    g_psx_cyc_batch_limit = 0;
    g_psx_cycle_fast_limit = psx_next_service_cycle - 1u;
    g_psx_cyc_bb_defer = 1;
    g_psx_cyc_local_acc = NULL;
    g_psx_oc_scale_q16 = (uint32_t)((UINT64_C(65536) * 100u) / 900u);
    g_psx_oc_accum = 0;
    psx_in_device_service = 0;
    g_event_step_conservative = 0;
    g_ls_replay_active = 0;
    g_ls_mode = 0;
    g_ds_recording = 0;
    service_count = 0;

    psx_icache_reset();
    g_psx_icache_active = 1;
    for (uint32_t i = 0; i < 8u; i++) {
        uint32_t addr = UINT32_C(0x80010000) + i * 16u;
        uint32_t idx = (addr & 0xFFCu) >> 2;
        g_psx_icache_tv[idx] = addr;
    }
}

/* Kept out of the timing driver's TU-level control flow, like a generated
 * guest function reached through dispatch. ThinLTO may import the small
 * runtime helpers into this function, which is the boundary being measured. */
static BENCH_NOINLINE uint64_t run_guest_blocks(CPUState *cpu,
                                                uint32_t block_count) {
    uint32_t ram_cursor = 0x1000u;
    for (uint32_t i = 0; i < block_count; i++) {
        psx_icache_fetch_interp(cpu, 0x80010000u);
        psx_cyc_step_0(cpu);
        cpu->gpr[2] += cpu->gpr[3];
        psx_cyc_step_2(cpu, 2u, 3u);
        cpu->gpr[4] ^= cpu->gpr[2];
        psx_cyc_step_1(cpu, 4u);
        psx_cyc_step_0(cpu);

        psx_icache_fetch_interp(cpu, 0x80010010u);
        cpu->gpr[5] = psx_cyc_load_word(cpu, ram_cursor, 5u, 1u << 6);
        psx_cyc_step_2(cpu, 5u, 7u);
        cpu->gpr[7] += cpu->gpr[5];
        psx_cyc_step_1(cpu, 7u);
        psx_cyc_step_0(cpu);

        psx_icache_fetch_interp(cpu, 0x80010020u);
        cpu->gpr[8] += cpu->gpr[9];
        psx_cyc_step_2(cpu, 8u, 9u);
        cpu->gpr[10] ^= cpu->gpr[8];
        psx_cyc_step_1(cpu, 10u);
        psx_cyc_step_0(cpu);
        psx_cyc_step_1(cpu, 11u);

        psx_icache_fetch_interp(cpu, 0x80010030u);
        cpu->gpr[12] = psx_cyc_load_word(cpu, ram_cursor + 4u, 12u,
                                         (1u << 11) | (1u << 13));
        psx_cyc_step_2(cpu, 12u, 14u);
        cpu->gpr[14] += cpu->gpr[12];
        psx_cyc_step_1(cpu, 14u);
        psx_cyc_step_0(cpu);

        psx_icache_fetch_interp(cpu, 0x80010040u);
        cpu->gpr[15] ^= cpu->gpr[16];
        psx_cyc_step_2(cpu, 15u, 16u);
        psx_cyc_step_0(cpu);
        psx_cyc_step_1(cpu, 17u);
        cpu->gpr[18] += cpu->gpr[17];

        psx_icache_fetch_interp(cpu, 0x80010050u);
        psx_cyc_step_2(cpu, 18u, 19u);
        cpu->gpr[20] = psx_cyc_load_word(cpu, ram_cursor + 8u, 20u,
                                         1u << 21);
        psx_cyc_step_1(cpu, 20u);
        psx_cyc_step_0(cpu);

        psx_icache_fetch_interp(cpu, 0x80010060u);
        psx_cyc_step_2(cpu, 22u, 23u);
        cpu->gpr[22] += cpu->gpr[23];
        psx_cyc_step_1(cpu, 22u);
        psx_cyc_step_0(cpu);
        psx_cyc_step_3(cpu, 24u, 25u, 26u);

        psx_icache_fetch_interp(cpu, 0x80010070u);
        cpu->gpr[24] ^= cpu->gpr[25] + cpu->gpr[26];
        psx_cyc_step_2(cpu, 27u, 28u);
        psx_cyc_step_1(cpu, 29u);
        psx_cyc_step_0(cpu);
        psx_cyc_step_2(cpu, 30u, 31u);
        psx_cyc_step_0(cpu);

        ram_cursor = (ram_cursor + 12u) & (BENCH_RAM_SIZE - 16u);
    }
    psx_cyc_batch_flush();

    uint64_t digest = psx_cycle_count ^ ((uint64_t)g_psx_oc_accum << 32);
    for (uint32_t i = 1; i < 32u; i++)
        digest = (digest * UINT64_C(0x100000001B3)) ^ cpu->gpr[i];
    digest ^= (uint64_t)cpu->read_absorb_which << 56;
    digest ^= (uint64_t)cpu->ld_which_t << 48;
    return digest;
}

static int compare_double(const void *a, const void *b) {
    const double da = *(const double *)a;
    const double db = *(const double *)b;
    return (da > db) - (da < db);
}

int main(int argc, char **argv) {
    uint32_t block_count = BENCH_DEFAULT_BLOCKS;
    if (argc == 2) {
        unsigned long parsed = strtoul(argv[1], NULL, 10);
        if (parsed == 0 || parsed > UINT32_MAX) {
            fputs("block count must be in [1, UINT32_MAX]\n", stderr);
            return 2;
        }
        block_count = (uint32_t)parsed;
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [block-count]\n", argv[0]);
        return 2;
    }

#if defined(_WIN32)
    (void)SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1u << 14);
#endif

    for (uint32_t i = 0; i < BENCH_RAM_SIZE; i++)
        bench_ram[i] = (uint8_t)((i * 37u + 11u) & 0xFFu);

    double rates[BENCH_SAMPLES];
    uint64_t reference_digest = 0;
    uint64_t reference_cycles = 0;
    uint32_t reference_accum = 0;
    uint64_t reference_services = 0;
    CPUState cpu;

    for (uint32_t sample = 0; sample < BENCH_SAMPLES; sample++) {
        reset_workload(&cpu);
        double start = monotonic_seconds();
        uint64_t digest = run_guest_blocks(&cpu, block_count);
        double elapsed = monotonic_seconds() - start;
        rates[sample] = ((double)block_count * BENCH_BLOCK_GUEST_INSNS) / elapsed;

        if (sample == 0) {
            reference_digest = digest;
            reference_cycles = psx_cycle_count;
            reference_accum = g_psx_oc_accum;
            reference_services = service_count;
        } else if (digest != reference_digest ||
                   psx_cycle_count != reference_cycles ||
                   g_psx_oc_accum != reference_accum ||
                   service_count != reference_services) {
            fputs("semantic checksum changed between identical samples\n", stderr);
            return 1;
        }
    }

    qsort(rates, BENCH_SAMPLES, sizeof(rates[0]), compare_double);
    printf("blocks=%" PRIu32 " guest_insns=%" PRIu64
           " samples=%u median_guest_insns_per_sec=%.0f"
           " min=%.0f max=%.0f guest_cycles=%" PRIu64
           " oc_accum=%" PRIu32 " services=%" PRIu64
           " digest=%016" PRIx64 "\n",
           block_count,
           (uint64_t)block_count * BENCH_BLOCK_GUEST_INSNS,
           BENCH_SAMPLES,
           rates[BENCH_SAMPLES / 2], rates[0], rates[BENCH_SAMPLES - 1],
           reference_cycles, reference_accum, reference_services,
           reference_digest);
    return 0;
}
