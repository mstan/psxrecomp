#include "cpu_state.h"
#include "gte.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

using PSXRecomp::GTE::GTEState;
using PSXRecomp::GTE::gte_cfc2;
using PSXRecomp::GTE::gte_ctc2;
using PSXRecomp::GTE::gte_mfc2;
using PSXRecomp::GTE::gte_mtc2;

extern "C" void gte_canonicalize_cpu_state(CPUState *cpu);
extern "C" void gte_test_set_precise_valid_mask(uint32_t mask);
extern "C" uint32_t gte_test_get_precise_valid_mask(void);
extern "C" void gte_test_set_timeline_generations(uint32_t precision,
                                                   uint32_t geometry);
extern "C" uint32_t gte_test_get_precision_generation(void);
extern "C" uint32_t gte_test_get_geometry_generation(void);
extern "C" void gte_precision_tracking_set(int enabled);
extern "C" void gte_precision_invalidate_word(uint32_t addr);
extern "C" int gte_precision_load_word(uint32_t addr, uint32_t packed,
                                        int32_t *x16, int32_t *y16,
                                        uint16_t *z);
extern "C" void gte_geometry_correction_set(int enabled);
extern "C" int gte_geometry_correction_lookup(uint32_t packed,
                                                int32_t *x16, int32_t *y16);
extern "C" void gte_test_seed_precise_projection(uint32_t index,
                                                   uint32_t packed,
                                                   int32_t x16,
                                                   int32_t y16,
                                                   uint16_t z);
extern "C" void gte_test_get_precise_projection(uint32_t index,
                                                  uint32_t *packed,
                                                  int32_t *x16,
                                                  int32_t *y16,
                                                  uint16_t *z,
                                                  uint8_t *valid);
extern "C" void gte_test_seed_geometry(uint32_t packed, int32_t x16,
                                        int32_t y16);
extern "C" void gte_test_execute_reference(CPUState *cpu, uint32_t cmd);

/* gte.cpp runtime dependencies that are irrelevant to register-transfer tests. */
extern "C" int gpu_ws_present_native_43(void) { return 0; }
extern "C" void psx_ws_note_gte_project(int) {}
extern "C" {
uint64_t s_frame_count = 0;
}

uint64_t g_test_cycle = 0;
uint32_t g_test_gte_set_calls = 0;
uint32_t g_test_gte_last_latency = 0;

extern "C" uint32_t psx_gte_cmd_latency(uint32_t cmd) {
    return 7u + (cmd & 0x3Fu);
}

extern "C" void psx_gte_set(CPUState *cpu, uint32_t latency) {
    ++g_test_gte_set_calls;
    g_test_gte_last_latency = latency;
    if (cpu->gte_ts_done > g_test_cycle) g_test_cycle = cpu->gte_ts_done;
    cpu->gte_ts_done = g_test_cycle + latency;
}

namespace {

struct PreciseState {
    uint32_t packed;
    int32_t x16;
    int32_t y16;
    uint16_t z;
    uint8_t valid;
};

std::array<PreciseState, 4> precise_snapshot() {
    std::array<PreciseState, 4> result{};
    for (uint32_t i = 0; i < result.size(); ++i) {
        auto &p = result[i];
        gte_test_get_precise_projection(i, &p.packed, &p.x16, &p.y16,
                                        &p.z, &p.valid);
    }
    return result;
}

void seed_precise_snapshot() {
    for (uint32_t i = 0; i < 4; ++i)
        gte_test_seed_precise_projection(i, 0x01010001u * (i + 1u),
                                         0x10000 + static_cast<int32_t>(i * 31),
                                         -0x20000 - static_cast<int32_t>(i * 47),
                                         static_cast<uint16_t>(0x100u + i));
}

bool same_precise(const std::array<PreciseState, 4> &lhs,
                  const std::array<PreciseState, 4> &rhs) {
    for (uint32_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].packed != rhs[i].packed || lhs[i].x16 != rhs[i].x16 ||
            lhs[i].y16 != rhs[i].y16 || lhs[i].z != rhs[i].z ||
            lhs[i].valid != rhs[i].valid)
            return false;
    }
    return true;
}

constexpr std::array<uint32_t, 10> kEdgeValues = {
    0x00000000u, 0x00000001u, 0x00007FFFu, 0x00008000u, 0x0000FFFFu,
    0x00010000u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFEu, 0xFFFFFFFFu,
};

constexpr std::array<uint8_t, 6> kInvalidRegs = {32u, 33u, 63u, 64u, 127u, 255u};

uint64_t g_rng = 0xD1B54A32D192ED03ull;

uint32_t random_u32() {
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return static_cast<uint32_t>((g_rng * 0x2545F4914F6CDD1Dull) >> 16);
}

void randomize_gte(CPUState &cpu) {
    std::memset(&cpu, 0, sizeof(cpu));
    for (uint32_t &value : cpu.gte_data) value = random_u32();
    for (uint32_t &value : cpu.gte_ctrl) value = random_u32();
}

void oracle_load(GTEState &gte, const CPUState &cpu) {
    for (uint8_t reg = 0; reg < 32; ++reg) {
        if (reg == 15 || reg == 28) continue;
        gte_mtc2(&gte, reg, cpu.gte_data[reg]);
    }
    for (uint8_t reg = 0; reg < 32; ++reg)
        gte_ctc2(&gte, reg, cpu.gte_ctrl[reg]);
    /* Importing emulator state is not a guest CTC2 write. Preserve a computed
     * FLAG error bit and any existing backing bits exactly like gte_load_data. */
    gte.FLAG = cpu.gte_ctrl[31];
}

void oracle_export(CPUState &cpu, GTEState &gte) {
    for (uint8_t reg = 0; reg < 32; ++reg)
        cpu.gte_data[reg] = gte_mfc2(&gte, reg);
    for (uint8_t reg = 0; reg < 32; ++reg)
        cpu.gte_ctrl[reg] = gte_cfc2(&gte, reg);
}

void oracle_canonicalize(CPUState &cpu) {
    GTEState gte;
    oracle_load(gte, cpu);
    oracle_export(cpu, gte);
}

uint32_t oracle_read_data(const CPUState &cpu, uint8_t reg) {
    GTEState gte;
    oracle_load(gte, cpu);
    return gte_mfc2(&gte, reg);
}

uint32_t oracle_read_ctrl(const CPUState &cpu, uint8_t reg) {
    GTEState gte;
    oracle_load(gte, cpu);
    return gte_cfc2(&gte, reg);
}

void oracle_write_data(CPUState &cpu, uint8_t reg, uint32_t value) {
    GTEState gte;
    oracle_load(gte, cpu);
    gte_mtc2(&gte, reg, value);
    oracle_export(cpu, gte);
}

void oracle_write_ctrl(CPUState &cpu, uint8_t reg, uint32_t value) {
    GTEState gte;
    oracle_load(gte, cpu);
    gte_ctc2(&gte, reg, value);
    oracle_export(cpu, gte);
}

bool same_gte(const CPUState &lhs, const CPUState &rhs) {
    return std::memcmp(lhs.gte_data, rhs.gte_data, sizeof(lhs.gte_data)) == 0 &&
           std::memcmp(lhs.gte_ctrl, rhs.gte_ctrl, sizeof(lhs.gte_ctrl)) == 0;
}

int fail_value(const char *phase, unsigned iteration, unsigned reg,
               uint32_t value, uint32_t expected, uint32_t actual) {
    std::fprintf(stderr,
                 "FAIL: %s iter=%u reg=%u value=%08X expected=%08X actual=%08X\n",
                 phase, iteration, reg, value, expected, actual);
    return 1;
}

int fail_state(const char *phase, unsigned iteration, unsigned reg, uint32_t value,
               const CPUState &expected, const CPUState &actual) {
    for (unsigned i = 0; i < 32; ++i) {
        if (expected.gte_data[i] != actual.gte_data[i]) {
            std::fprintf(stderr,
                         "FAIL: %s iter=%u source_reg=%u value=%08X data=%u "
                         "expected=%08X actual=%08X\n",
                         phase, iteration, reg, value, i, expected.gte_data[i],
                         actual.gte_data[i]);
            return 1;
        }
    }
    for (unsigned i = 0; i < 32; ++i) {
        if (expected.gte_ctrl[i] != actual.gte_ctrl[i]) {
            std::fprintf(stderr,
                         "FAIL: %s iter=%u source_reg=%u value=%08X ctrl=%u "
                         "expected=%08X actual=%08X\n",
                         phase, iteration, reg, value, i, expected.gte_ctrl[i],
                         actual.gte_ctrl[i]);
            return 1;
        }
    }
    std::fprintf(stderr, "FAIL: %s reported unequal states without a differing word\n",
                 phase);
    return 1;
}

int check_all_reads(const CPUState &expected, CPUState &actual,
                    const char *phase, unsigned iteration) {
    for (uint8_t reg = 0; reg < 32; ++reg) {
        const uint32_t expected_data = oracle_read_data(expected, reg);
        const uint32_t actual_data = gte_read_data(&actual, reg);
        if (actual_data != expected_data)
            return fail_value(phase, iteration, reg, 0, expected_data, actual_data);

        const uint32_t expected_ctrl = oracle_read_ctrl(expected, reg);
        const uint32_t actual_ctrl = gte_read_ctrl(&actual, reg);
        if (actual_ctrl != expected_ctrl)
            return fail_value(phase, iteration, reg, 1, expected_ctrl, actual_ctrl);
    }
    return 0;
}

int test_canonicalizer() {
    for (unsigned iteration = 0; iteration < 512; ++iteration) {
        CPUState expected;
        randomize_gte(expected);
        CPUState actual = expected;
        oracle_canonicalize(expected);
        gte_canonicalize_cpu_state(&actual);
        if (!same_gte(expected, actual))
            return fail_state("canonicalize", iteration, 0, 0, expected, actual);
    }
    return 0;
}

int test_reads() {
    for (unsigned iteration = 0; iteration < 256; ++iteration) {
        CPUState state;
        randomize_gte(state);
        /* Deliberately use raw randomized backing here. Read helpers must match
         * the architectural value reconstructed by the old bridge even before
         * an explicit canonicalization boundary. */
        CPUState before = state;
        if (int rc = check_all_reads(state, state, "raw read", iteration)) return rc;
        if (!same_gte(before, state))
            return fail_state("read mutated state", iteration, 0, 0, before, state);

        for (uint8_t reg : kInvalidRegs) {
            const uint32_t actual_data = gte_read_data(&state, reg);
            const uint32_t actual_ctrl = gte_read_ctrl(&state, reg);
            if (actual_data != 0u)
                return fail_value("invalid data read", iteration, reg, 0, 0, actual_data);
            if (actual_ctrl != 0u)
                return fail_value("invalid ctrl read", iteration, reg, 0, 0, actual_ctrl);
        }
    }
    return 0;
}

int test_writes() {
    for (unsigned iteration = 0; iteration < 64; ++iteration) {
        CPUState seed;
        randomize_gte(seed);

        /* Valid helper writes must also normalize unrelated backing words.
         * This preserves the old bridge's behavior when falling back from
         * committed AOT C that predates helper routing for masked registers. */
        for (uint8_t reg = 0; reg < 32; ++reg) {
            for (uint32_t edge : kEdgeValues) {
                const uint32_t value = edge ^ (iteration ? random_u32() : 0u);

                CPUState expected = seed;
                CPUState actual = seed;
                oracle_write_data(expected, reg, value);
                gte_write_data(&actual, reg, value);
                if (!same_gte(expected, actual))
                    return fail_state("data write", iteration, reg, value,
                                      expected, actual);

                expected = seed;
                actual = seed;
                oracle_write_ctrl(expected, reg, value);
                gte_write_ctrl(&actual, reg, value);
                if (!same_gte(expected, actual))
                    return fail_state("ctrl write", iteration, reg, value,
                                      expected, actual);
            }
        }

        CPUState canonical_seed = seed;
        oracle_canonicalize(canonical_seed);
        for (uint8_t reg : kInvalidRegs) {
            CPUState expected = canonical_seed;
            CPUState actual = canonical_seed;
            const uint32_t value = random_u32();
            oracle_write_data(expected, reg, value);
            gte_write_data(&actual, reg, value);
            if (!same_gte(expected, actual))
                return fail_state("invalid data write", iteration, reg, value,
                                  expected, actual);

            expected = canonical_seed;
            actual = canonical_seed;
            oracle_write_ctrl(expected, reg, value);
            gte_write_ctrl(&actual, reg, value);
            if (!same_gte(expected, actual))
                return fail_state("invalid ctrl write", iteration, reg, value,
                                  expected, actual);
        }
    }

    return 0;
}

int test_sequence_fuzz() {
    for (unsigned iteration = 0; iteration < 128; ++iteration) {
        CPUState expected;
        randomize_gte(expected);
        oracle_canonicalize(expected);
        CPUState actual = expected;

        for (unsigned step = 0; step < 512; ++step) {
            const uint32_t selector = random_u32();
            const uint8_t reg = (selector & 7u) == 0u
                                    ? kInvalidRegs[selector % kInvalidRegs.size()]
                                    : static_cast<uint8_t>((selector >> 8) & 31u);
            const uint32_t value = random_u32();
            switch ((selector >> 16) & 3u) {
            case 0: {
                const uint32_t want = oracle_read_data(expected, reg);
                const uint32_t got = gte_read_data(&actual, reg);
                if (want != got)
                    return fail_value("sequence data read", iteration, reg, value,
                                      want, got);
                break;
            }
            case 1: {
                const uint32_t want = oracle_read_ctrl(expected, reg);
                const uint32_t got = gte_read_ctrl(&actual, reg);
                if (want != got)
                    return fail_value("sequence ctrl read", iteration, reg, value,
                                      want, got);
                break;
            }
            case 2:
                oracle_write_data(expected, reg, value);
                gte_write_data(&actual, reg, value);
                break;
            case 3:
                oracle_write_ctrl(expected, reg, value);
                gte_write_ctrl(&actual, reg, value);
                break;
            }
            if (!same_gte(expected, actual))
                return fail_state("sequence state", iteration, reg, value,
                                  expected, actual);
        }
    }
    return 0;
}

int test_command_marshaling() {
    constexpr std::array<uint8_t, 22> kFunctions = {
        0x01, 0x06, 0x0C, 0x10, 0x11, 0x12, 0x13, 0x14,
        0x16, 0x1B, 0x1C, 0x1E, 0x20, 0x28, 0x29, 0x2A,
        0x2D, 0x2E, 0x30, 0x3D, 0x3E, 0x3F,
    };

    for (uint8_t function : kFunctions) {
        for (unsigned iteration = 0; iteration < 96; ++iteration) {
            CPUState seed;
            randomize_gte(seed);  // deliberately raw/noncanonical legacy state
            CPUState expected = seed;
            CPUState actual = seed;
            const uint32_t cmd = (random_u32() & ~0x3Fu) | function;
            gte_test_execute_reference(&expected, cmd);
            gte_execute(&actual, cmd);
            if (!same_gte(expected, actual))
                return fail_state("command marshal", iteration, function, cmd,
                                  expected, actual);
            if (actual.gte_data[15] != actual.gte_data[14] ||
                actual.gte_data[23] != 0u ||
                actual.gte_data[28] != actual.gte_data[29] ||
                actual.gte_data[31] != gte_read_data(&actual, 31))
                return fail_value("command canonical aliases", iteration,
                                  function, cmd, actual.gte_data[14],
                                  actual.gte_data[15]);
        }
    }

    /* MVMVA's matrix/vector/translation dependencies are all selected by the
     * command word. Exhaust every selector plus sf/lm combination explicitly. */
    for (uint32_t mx = 0; mx < 4; ++mx) {
        for (uint32_t vv = 0; vv < 4; ++vv) {
            for (uint32_t tv = 0; tv < 4; ++tv) {
                for (uint32_t sf = 0; sf < 2; ++sf) {
                    for (uint32_t lm = 0; lm < 2; ++lm) {
                        CPUState seed;
                        randomize_gte(seed);
                        CPUState expected = seed;
                        CPUState actual = seed;
                        const uint32_t cmd = 0x12u | (mx << 17) | (vv << 15) |
                                             (tv << 13) | (sf << 19) | (lm << 10);
                        gte_test_execute_reference(&expected, cmd);
                        gte_execute(&actual, cmd);
                        if (!same_gte(expected, actual))
                            return fail_state("MVMVA selectors", mx * 16u + vv * 4u + tv,
                                              static_cast<unsigned>(sf * 2u + lm),
                                              cmd, expected, actual);
                    }
                }
            }
        }
    }

    /* Stateful command streams catch FIFO/alias state that single-command
     * comparisons can accidentally reinitialize away. */
    for (unsigned iteration = 0; iteration < 64; ++iteration) {
        CPUState expected;
        randomize_gte(expected);
        CPUState actual = expected;
        for (unsigned step = 0; step < 128; ++step) {
            const uint8_t function = kFunctions[random_u32() % kFunctions.size()];
            const uint32_t cmd = (random_u32() & ~0x3Fu) | function;
            gte_test_execute_reference(&expected, cmd);
            gte_execute(&actual, cmd);
            if (!same_gte(expected, actual))
                return fail_state("command stream", iteration, function, cmd,
                                  expected, actual);
        }
    }

    /* Projection commands update a host-only precise-SXY FIFO. Reset that
     * global state around each path so the old and direct marshalers can be
     * compared independently instead of executing twice on one timeline. */
    for (uint8_t function : kFunctions) {
        CPUState seed;
        randomize_gte(seed);
        CPUState expected = seed;
        CPUState actual = seed;
        const uint32_t cmd = (random_u32() & ~0x3Fu) | function;
        seed_precise_snapshot();
        gte_test_execute_reference(&expected, cmd);
        const auto expected_precise = precise_snapshot();
        seed_precise_snapshot();
        gte_execute(&actual, cmd);
        const auto actual_precise = precise_snapshot();
        if (!same_precise(expected_precise, actual_precise))
            return fail_value("command precise provenance", 0, function,
                              cmd, 1u, 0u);
    }
    return 0;
}

int test_command_timing_hook() {
    CPUState cpu{};
    g_test_cycle = 100u;
    cpu.gte_ts_done = 175u;
    g_test_gte_set_calls = 0;
    g_test_gte_last_latency = 0;
    gte_execute(&cpu, 0x06u);
    if (g_test_gte_set_calls != 1u || g_test_gte_last_latency != 13u ||
        cpu.gte_ts_done != 188u)
        return fail_value("command timing serialization", 0, 0x06u, 0,
                          188u, static_cast<uint32_t>(cpu.gte_ts_done));
    gte_execute(&cpu, 0x30u);
    if (g_test_gte_set_calls != 2u || g_test_gte_last_latency != 55u ||
        cpu.gte_ts_done != 243u)
        return fail_value("back-to-back command timing", 0, 0x30u, 0,
                          243u, static_cast<uint32_t>(cpu.gte_ts_done));
    return 0;
}

int test_precise_sxy_invalidation() {
    CPUState cpu{};
    for (uint8_t reg = 0; reg < 32; ++reg) {
        gte_test_set_precise_valid_mask(0xFu);
        gte_write_data(&cpu, reg, 0x12345678u);
        const uint32_t expected = (reg >= 12 && reg <= 15) ? 0u : 0xFu;
        const uint32_t actual = gte_test_get_precise_valid_mask();
        if (actual != expected)
            return fail_value("precise SXY invalidation", 0, reg,
                              0x12345678u, expected, actual);
    }

    for (uint16_t wide_reg = 32; wide_reg <= 255; ++wide_reg) {
        const uint8_t reg = static_cast<uint8_t>(wide_reg);
        gte_test_set_precise_valid_mask(0xFu);
        gte_write_data(&cpu, reg, 0x87654321u);
        if (gte_test_get_precise_valid_mask() != 0xFu)
            return fail_value("invalid data preserves precise SXY", 0, reg,
                              0x87654321u, 0xFu,
                              gte_test_get_precise_valid_mask());
    }

    for (uint16_t wide_reg = 0; wide_reg <= 255; ++wide_reg) {
        const uint8_t reg = static_cast<uint8_t>(wide_reg);
        gte_test_set_precise_valid_mask(0xFu);
        gte_write_ctrl(&cpu, reg, 0xCAFEBABEu);
        if (gte_test_get_precise_valid_mask() != 0xFu)
            return fail_value("ctrl preserves precise SXY", 0, reg,
                              0xCAFEBABEu, 0xFu,
                              gte_test_get_precise_valid_mask());
    }

    gte_test_set_precise_valid_mask(0xFu);
    gte_test_set_timeline_generations(41u, 73u);
    gte_canonicalize_cpu_state(&cpu);
    if (gte_test_get_precise_valid_mask() != 0u)
        return fail_value("canonicalize invalidates precise SXY", 0, 0,
                          0, 0, gte_test_get_precise_valid_mask());
    if (gte_test_get_precision_generation() != 42u)
        return fail_value("canonicalize advances precision generation", 0, 0,
                          0, 42u, gte_test_get_precision_generation());
    if (gte_test_get_geometry_generation() != 74u)
        return fail_value("canonicalize advances geometry generation", 0, 0,
                          0, 74u, gte_test_get_geometry_generation());

    /* Generation zero is reserved. Wrapping must clear stale generations and
     * restart at one rather than resurrecting ancient generation-one entries. */
    gte_test_set_precise_valid_mask(0xFu);
    gte_test_set_timeline_generations(0xFFFFFFFFu, 0xFFFFFFFFu);
    gte_precision_timeline_invalidate();
    if (gte_test_get_precise_valid_mask() != 0u ||
        gte_test_get_precision_generation() != 1u ||
        gte_test_get_geometry_generation() != 1u)
        return fail_value("timeline generation wrap", 0, 0, 0, 1u,
                          gte_test_get_precision_generation());
    return 0;
}

int test_precision_speculative_transaction() {
    constexpr uint32_t address = 0x00123450u;
    constexpr uint32_t packed = 0x00420021u;
    constexpr int32_t x16 = 0x00123456;
    constexpr int32_t y16 = -0x00034567;
    constexpr uint16_t z = 0x4567u;

    gte_precision_tracking_set(1);
    gte_geometry_correction_set(1);
    gte_test_set_timeline_generations(41u, 73u);
    gte_test_seed_precise_projection(2, packed, x16, y16, z);
    gte_precision_store_word(address, 14);
    gte_test_seed_geometry(packed, x16, y16);

    int32_t got_x = 0, got_y = 0;
    uint16_t got_z = 0;
    if (!gte_precision_load_word(address, packed, &got_x, &got_y, &got_z) ||
        got_x != x16 || got_y != y16 || got_z != z)
        return fail_value("precision seed lookup", 0, 14, packed, z, got_z);
    if (!gte_geometry_correction_lookup(packed, &got_x, &got_y) ||
        got_x != x16 || got_y != y16)
        return fail_value("geometry seed lookup", 0, 0, packed,
                          static_cast<uint32_t>(x16), static_cast<uint32_t>(got_x));

    gte_precision_speculative_begin();
    gte_precision_speculative_begin();
    if (gte_precision_load_word(address, packed, &got_x, &got_y, &got_z) != 0)
        return fail_value("speculative precision read", 0, 0, packed, 0, 1);
    if (gte_geometry_correction_lookup(packed, &got_x, &got_y) != 0)
        return fail_value("speculative geometry read", 0, 0, packed, 0, 1);
    gte_precision_invalidate_word(address);  // suppressed: must not age live entry
    CPUState cpu{};
    gte_write_data(&cpu, 14, 0xDEADBEEFu);  // mutates speculative SXY only
    gte_precision_speculative_end();
    if (gte_precision_load_word(address, packed, &got_x, &got_y, &got_z) != 0)
        return fail_value("nested speculation remains isolated", 0, 0,
                          packed, 0, 1);
    gte_precision_speculative_end();

    if (gte_test_get_precise_valid_mask() != 0x4u)
        return fail_value("speculative SXY restore", 0, 0, 0, 0x4u,
                          gte_test_get_precise_valid_mask());
    if (!gte_precision_load_word(address, packed, &got_x, &got_y, &got_z) ||
        got_x != x16 || got_y != y16 || got_z != z)
        return fail_value("authoritative precision survives", 0, 0, packed,
                          z, got_z);
    if (!gte_geometry_correction_lookup(packed, &got_x, &got_y) ||
        got_x != x16 || got_y != y16)
        return fail_value("authoritative geometry survives", 0, 0, packed,
                          static_cast<uint32_t>(x16), static_cast<uint32_t>(got_x));

    /* A raw savestate/import restore is authoritative even when host polling
     * reaches it during speculation. Defer its timeline invalidation until the
     * outer transaction closes, but never restore pre-load provenance. */
    gte_precision_speculative_begin();
    gte_precision_speculative_begin();
    gte_precision_timeline_invalidate();
    if (gte_test_get_precision_generation() != 41u ||
        gte_test_get_geometry_generation() != 73u)
        return fail_value("deferred timeline invalidation", 0, 0, 0, 41u,
                          gte_test_get_precision_generation());
    gte_precision_speculative_end();
    if (gte_test_get_precision_generation() != 41u)
        return fail_value("nested timeline remains deferred", 0, 0, 0, 41u,
                          gte_test_get_precision_generation());
    gte_precision_speculative_end();
    if (gte_test_get_precise_valid_mask() != 0u ||
        gte_test_get_precision_generation() != 42u ||
        gte_test_get_geometry_generation() != 74u ||
        gte_precision_load_word(address, packed, nullptr, nullptr, nullptr) != 0 ||
        gte_geometry_correction_lookup(packed, nullptr, nullptr) != 0)
        return fail_value("authoritative restore invalidation", 0, 0, 0, 42u,
                          gte_test_get_precision_generation());

    /* Exercise each cache's wrap independently and prove stale entries miss,
     * rather than checking generation counters alone. */
    gte_test_set_timeline_generations(0xFFFFFFFFu, 90u);
    gte_test_seed_precise_projection(2, packed, x16, y16, z);
    gte_precision_store_word(address, 14);
    gte_test_seed_geometry(packed, x16, y16);
    gte_precision_timeline_invalidate();
    if (gte_test_get_precision_generation() != 1u ||
        gte_test_get_geometry_generation() != 91u ||
        gte_precision_load_word(address, packed, nullptr, nullptr, nullptr) != 0 ||
        gte_geometry_correction_lookup(packed, nullptr, nullptr) != 0)
        return fail_value("independent precision wrap", 0, 0, 0, 1u,
                          gte_test_get_precision_generation());

    gte_test_set_timeline_generations(120u, 0xFFFFFFFFu);
    gte_test_seed_precise_projection(2, packed, x16, y16, z);
    gte_precision_store_word(address, 14);
    gte_test_seed_geometry(packed, x16, y16);
    gte_precision_timeline_invalidate();
    if (gte_test_get_precision_generation() != 121u ||
        gte_test_get_geometry_generation() != 1u ||
        gte_precision_load_word(address, packed, nullptr, nullptr, nullptr) != 0 ||
        gte_geometry_correction_lookup(packed, nullptr, nullptr) != 0)
        return fail_value("independent geometry wrap", 0, 0, 0, 1u,
                          gte_test_get_geometry_generation());
    return 0;
}

} // namespace

int main() {
    if (int rc = test_canonicalizer()) return rc;
    if (int rc = test_reads()) return rc;
    if (int rc = test_writes()) return rc;
    if (int rc = test_sequence_fuzz()) return rc;
    if (int rc = test_command_marshaling()) return rc;
    if (int rc = test_command_timing_hook()) return rc;
    if (int rc = test_precise_sxy_invalidation()) return rc;
    if (int rc = test_precision_speculative_transaction()) return rc;
    std::puts("PASS: canonical GTE register helpers match GTEState transfer oracle");
    return 0;
}
