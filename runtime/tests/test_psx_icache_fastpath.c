#include "cpu_state.h"
#include "psx_icache.h"

#include <stdio.h>
#include <string.h>

int g_ls_replay_active = 0;
static uint64_t test_cycles = 0;

void psx_advance_cycles(uint32_t cycles) { test_cycles += cycles; }

static int expect(int condition, const char *message) {
    if (condition) return 1;
    fprintf(stderr, "FAIL: %s\n", message);
    return 0;
}

int main(void) {
    CPUState cpu;
    memset(&cpu, 0, sizeof(cpu));

    psx_icache_reset();
    g_psx_icache_active = 1;
    test_cycles = 0;
    cpu.read_absorb_which = 1;
    cpu.read_absorb[0] = 77u;
    cpu.read_absorb[1] = 99u;
    psx_icache_fetch_interp(&cpu, 0x80010000u);
    if (!expect(test_cycles == 7u, "word-0 cold refill cost")) return 1;
    if (!expect(cpu.read_absorb_which == 0u && cpu.read_absorb[1] == 0u &&
                cpu.read_absorb[0] == 77u,
                "true miss clears only the selected load give-back")) return 1;
    cpu.read_absorb_which = 1;
    cpu.read_absorb[1] = 55u;
    psx_icache_fetch_interp(&cpu, 0x80010000u);
    psx_icache_fetch_interp(&cpu, 0x80010004u);
    if (!expect(test_cycles == 7u, "steady tag hits are cycle-free")) return 1;
    if (!expect(cpu.read_absorb_which == 1u && cpu.read_absorb[1] == 55u,
                "tag hits preserve load give-back")) return 1;

    psx_icache_reset();
    g_psx_icache_active = 1;
    test_cycles = 0;
    psx_icache_fetch_interp(&cpu, 0x80010008u);
    if (!expect(test_cycles == 5u, "word-2 partial refill cost")) return 1;
    psx_icache_fetch_interp(&cpu, 0x80010000u);
    if (!expect(test_cycles == 12u, "earlier word remains invalid after partial refill")) return 1;

    psx_icache_reset();
    g_psx_icache_active = 1;
    test_cycles = 0;
    psx_icache_fetch_interp(&cpu, 0xA0010000u);
    psx_icache_fetch_interp(&cpu, 0xA0010000u);
    if (!expect(test_cycles == 8u, "KSEG1 remains uncached")) return 1;

    psx_icache_reset();
    g_psx_icache_active = 1;
    test_cycles = 0;
    psx_icache_fetch_interp(&cpu, 0x80010000u);
    psx_icache_fetch_interp(&cpu, 0x00010000u);
    if (!expect(test_cycles == 14u,
                "KUSEG/KSEG0 aliases replace full virtual tags")) return 1;
    psx_icache_reset();
    if (!expect(g_psx_icache_tv[0] == 1u,
                "reset makes a warmed line cold")) return 1;

    g_psx_icache_active = 1;
    test_cycles = 0;
    psx_icache_fetch(&cpu, 0x80010000u);
    if (!expect(test_cycles == 7u,
                "compiled-code compatibility wrapper preserves refill")) return 1;

    psx_icache_reset();
    g_psx_icache_active = 1;
    test_cycles = 0;
    g_ls_replay_active = 1;
    cpu.read_absorb_which = 1;
    cpu.read_absorb[1] = 44u;
    psx_icache_fetch_interp(&cpu, 0x80010000u);
    g_ls_replay_active = 0;
    if (!expect(test_cycles == 0u, "lockstep replay does not mutate cache")) return 1;
    if (!expect(g_psx_icache_tv[0] == 1u, "replay preserved cold tag")) return 1;
    if (!expect(cpu.read_absorb_which == 1u && cpu.read_absorb[1] == 44u,
                "replay preserves load give-back")) return 1;

    g_psx_icache_active = 0;
    psx_icache_fetch_interp(&cpu, 0x80010000u);
    if (!expect(test_cycles == 0u, "disabled cache model is free")) return 1;
    if (!expect(cpu.read_absorb_which == 1u && cpu.read_absorb[1] == 44u,
                "disabled cache preserves load give-back")) return 1;

    puts("PASS: interpreter I-cache hit path preserves miss/refill semantics");
    return 0;
}
