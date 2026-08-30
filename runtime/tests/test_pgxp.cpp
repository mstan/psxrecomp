/* test_pgxp.cpp — PGXP value-propagation engine unit tests (ENHANCEMENTS.md
 * G1.2/G1.3). White-box over runtime/src/pgxp.cpp with the gte.cpp fallback
 * cache stubbed, exercising exactly the properties the engine's safety rests
 * on: provenance roundtrips, validate-on-read, half-word semantics, the
 * repack arithmetic, the suppression bracket, and the GPU-side safeguards. */

#include "pgxp.h"
#include "pgxp_hooks.h"

#include <cstdio>
#include <cstring>

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #cond);                                             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

/* ---- gte.cpp fallback-cache stub ----------------------------------------- */

static uint32_t g_fb_packed = 0;
static int32_t  g_fb_x16 = 0, g_fb_y16 = 0;
static int      g_fb_valid = 0;

extern "C" int gte_geometry_correction_lookup(uint32_t packed,
                                              int32_t *x16, int32_t *y16) {
    if (!g_fb_valid || packed != g_fb_packed) return 0;
    if (x16) *x16 = g_fb_x16;
    if (y16) *y16 = g_fb_y16;
    return 1;
}

extern "C" uint32_t memory_get_ram_bytes(void) {
    return 2u * 1024u * 1024u;
}

/* ---- MIPS encodings ------------------------------------------------------ */

static uint32_t enc_i(uint32_t op, uint32_t rs, uint32_t rt, uint16_t imm) {
    return (op << 26) | (rs << 21) | (rt << 16) | imm;
}
static uint32_t enc_r(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sh,
                      uint32_t funct) {
    return (rs << 21) | (rt << 16) | (rd << 11) | (sh << 6) | funct;
}
static uint32_t enc_cop2(uint32_t sub, uint32_t rt, uint32_t rd) {
    return (0x12u << 26) | (sub << 21) | (rt << 16) | (rd << 11);
}

#define LW(rs, rt)   enc_i(0x23, rs, rt, 0)
#define SW(rs, rt)   enc_i(0x2B, rs, rt, 0)
#define LH(rs, rt)   enc_i(0x21, rs, rt, 0)
#define LHU(rs, rt)  enc_i(0x25, rs, rt, 0)
#define SH(rs, rt)   enc_i(0x29, rs, rt, 0)
#define SB(rs, rt)   enc_i(0x28, rs, rt, 0)
#define LWC2(rt)     enc_i(0x32, 1, rt, 0)
#define SWC2(rt)     enc_i(0x3A, 1, rt, 0)
#define MFC2(rt, rd) enc_cop2(0x00, rt, rd)
#define MTC2(rt, rd) enc_cop2(0x04, rt, rd)
#define ADDIU(rs, rt, imm) enc_i(0x09, rs, rt, (uint16_t)(imm))
#define LUI(rt, imm) enc_i(0x0F, 0, rt, (uint16_t)(imm))
#define SLL(rt, rd, sh) enc_r(0, rt, rd, sh, 0x00)
#define SRA(rt, rd, sh) enc_r(0, rt, rd, sh, 0x03)
#define OR(rs, rt, rd)  enc_r(rs, rt, rd, 0, 0x25)
#define ADDU(rs, rt, rd) enc_r(rs, rt, rd, 0, 0x21)

/* One projected vertex: x = 160.5, y = 80.25 -> packed integer word. */
static const uint32_t PACKED  = (80u << 16) | 160u;
static const int32_t  X16     = (160 << 16) | 0x8000;   /* 160.5  */
static const int32_t  Y16     = (80 << 16)  | 0x4000;   /* 80.25  */
static const uint16_t SZ3     = 100;

static const uint32_t ADDR_A  = 0x80100000u;   /* packet slot A (KSEG0)  */
static const uint32_t ADDR_B  = 0x00100040u;   /* packet slot B (KUSEG)  */
static const uint32_t ADDR_C  = 0x00100080u;   /* packet slot C (KUSEG)  */

static void produce_at(uint32_t addr) {
    pgxp_gte_push_sxy(X16, Y16, SZ3, PACKED);
    psx_pgxp_cop2(nullptr, SWC2(14), PACKED, addr);
}

static int lookup(uint32_t addr, uint32_t word, int32_t ix, int32_t iy,
                  int32_t *x, int32_t *y, uint16_t *z) {
    int32_t lx, ly; uint16_t lz;
    int r = pgxp_get_precise_vertex(addr, word, ix, iy, &lx, &ly, &lz);
    if (x) *x = lx;
    if (y) *y = ly;
    if (z) *z = lz;
    return r;
}

int main(void) {
    pgxp_set_enabled(1);
    pgxp_set_tolerance(-1.0f);
    pgxp_set_cpu_mode(0);

    /* --- SWC2 produce -> GPU consume (the perspective-texturing spine) --- */
    produce_at(ADDR_A);
    CHECK(pgxp_memory_page_armed(ADDR_A & 0x1FFFFFFFu));
    {
        int32_t x, y; uint16_t z;
        CHECK(lookup(ADDR_A, PACKED, 160, 80, &x, &y, &z) == PGXP_SRC_DATAFLOW);
        CHECK(x == X16 && y == Y16 && z == SZ3);
        /* mirrors resolve to the same shadow word */
        CHECK(lookup(0xA0100000u, PACKED, 160, 80, &x, &y, &z) ==
              PGXP_SRC_DATAFLOW);
    }

    /* --- DMA/untracked overwrite: value validation rejects the shadow --- */
    {
        int32_t x, y; uint16_t z;
        uint32_t other = (81u << 16) | 161u;
        CHECK(lookup(ADDR_A, other, 161, 81, &x, &y, &z) == PGXP_SRC_NATIVE);
        CHECK(x == (161 << 16) && y == (81 << 16) && z == 0);
    }

    /* --- LW/SW roundtrip: packet copied by the CPU keeps provenance --- */
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_store(nullptr, SW(1, 8), ADDR_B, PACKED);
    {
        int32_t x, y; uint16_t z;
        CHECK(lookup(ADDR_B, PACKED, 160, 80, &x, &y, &z) == PGXP_SRC_DATAFLOW);
        CHECK(x == X16 && y == Y16 && z == SZ3);
    }

    /* --- stale GPR: register changed between load and store --- */
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_store(nullptr, SW(1, 8), ADDR_B, 0xDEADBEEFu);   /* r8 mutated */
    CHECK(lookup(ADDR_B, 0xDEADBEEFu, 0, 0, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);

    /* A byte-identical destructive ALU result is still a new writer. The
     * live-mask gate makes the invalidation call conditional, but equality
     * must not let the old projection reach the following SW. */
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    CHECK((g_pgxp_gpr_live_mask & (1u << 8)) != 0);
    psx_pgxp_gpr_write(nullptr, 8);
    CHECK((g_pgxp_gpr_live_mask & (1u << 8)) == 0);
    psx_pgxp_store(nullptr, SW(1, 8), ADDR_B, PACKED);
    CHECK(lookup(ADDR_B, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);

    /* An unhooked/raw byte-identical RAM write also kills provenance. */
    produce_at(ADDR_A);
    CHECK(pgxp_memory_page_armed(ADDR_A & 0x1FFFFFFFu));
    CHECK(!pgxp_memory_page_armed((ADDR_A & 0x1FFFFFFFu) + 0x1000u));
    PGXPStats raw_before, raw_after;
    pgxp_get_stats(&raw_before);
    pgxp_memory_write(ADDR_A, 4u);
    pgxp_get_stats(&raw_after);
    CHECK(raw_after.memory_invalidations == raw_before.memory_invalidations + 1);
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);

    /* --- MOVE idiom (memory mode, no cpu_mode needed) --- */
    pgxp_set_full_hooks(1);
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_alu(nullptr, ADDU(8, 0, 10), PACKED, PACKED, 0);
    psx_pgxp_store(nullptr, SW(1, 10), ADDR_B, PACKED);
    CHECK(lookup(ADDR_B, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_DATAFLOW);
    pgxp_set_full_hooks(0);

    /* --- MFC2 -> SW (register transfer path) --- */
    pgxp_gte_push_sxy(X16, Y16, SZ3, PACKED);
    psx_pgxp_cop2(nullptr, MFC2(9, 14), PACKED, 0);
    psx_pgxp_store(nullptr, SW(1, 9), ADDR_B, PACKED);
    CHECK(lookup(ADDR_B, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_DATAFLOW);

    /* --- LH/SH: halves from one projection rebuild coherent XY + depth --- */
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LHU(1, 8), ADDR_A + 2u, PACKED >> 16);     /* Y   */
    pgxp_memory_write(ADDR_B + 2u, 2u);
    psx_pgxp_store(nullptr, SH(1, 8), ADDR_B + 2u, PACKED >> 16);
    psx_pgxp_load(nullptr, LHU(1, 8), ADDR_A, PACKED & 0xFFFFu);     /* X   */
    pgxp_memory_write(ADDR_B, 2u);
    psx_pgxp_store(nullptr, SH(1, 8), ADDR_B, PACKED & 0xFFFFu);
    {
        int32_t x, y; uint16_t z;
        CHECK(lookup(ADDR_B, PACKED, 160, 80, &x, &y, &z) == PGXP_SRC_DATAFLOW);
        CHECK(x == X16 && y == Y16);
        CHECK(z == SZ3);                     /* paired SH rebuild carries Z */
    }

    /* X and Y from different projections may have identical packed integer
     * halves and even the same Z. Their component IDs must still reject both
     * geometry and perspective correction. */
    pgxp_gte_push_sxy(X16 + 0x1000, Y16 + 0x2000, SZ3, PACKED);
    psx_pgxp_cop2(nullptr, SWC2(14), PACKED, ADDR_C);
    pgxp_memory_write(ADDR_B, 4u);
    psx_pgxp_load(nullptr, LHU(1, 8), ADDR_A, PACKED & 0xFFFFu);
    pgxp_memory_write(ADDR_B, 2u);
    psx_pgxp_store(nullptr, SH(1, 8), ADDR_B, PACKED & 0xFFFFu);
    psx_pgxp_load(nullptr, LHU(1, 9), ADDR_C + 2u, PACKED >> 16);
    pgxp_memory_write(ADDR_B + 2u, 2u);
    psx_pgxp_store(nullptr, SH(1, 9), ADDR_B + 2u, PACKED >> 16);
    PGXPStats mixed_before, mixed_after;
    pgxp_get_stats(&mixed_before);
    /* Even an addressless geometry-cache hit must not launder a known mixed
     * dataflow value back into a correctable vertex. */
    g_fb_valid = 1;
    g_fb_packed = PACKED;
    g_fb_x16 = X16;
    g_fb_y16 = Y16;
    CHECK(pgxp_debug_shadow_class(ADDR_B, PACKED) ==
          PGXP_SHADOW_MIXED_PROJECTION);
    CHECK(pgxp_load_precise_word(ADDR_B, PACKED, nullptr, nullptr, nullptr) == 0);
    CHECK(lookup(ADDR_B, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);
    g_fb_valid = 0;
    pgxp_get_stats(&mixed_after);
    CHECK(mixed_after.mixed_depth_reject > mixed_before.mixed_depth_reject);

    /* --- SB destroys the touched half only --- */
    produce_at(ADDR_B);
    psx_pgxp_store(nullptr, SB(1, 8), ADDR_B, PACKED & 0xFFu);  /* same byte */
    {
        int32_t x, y; uint16_t z;
        /* low half invalidated -> not a full XY hit anymore */
        CHECK(lookup(ADDR_B, PACKED, 160, 80, &x, &y, &z) != PGXP_SRC_DATAFLOW);
    }

    /* --- cpu-mode repack: lhu / sll 16 / or (the classic vertex build) --- */
    pgxp_set_cpu_mode(1);
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LHU(1, 8), ADDR_A + 2u, PACKED >> 16);     /* Y   */
    psx_pgxp_alu(nullptr, SLL(8, 9, 16), (PACKED >> 16) << 16,
                 PACKED >> 16, 16);
    psx_pgxp_load(nullptr, LHU(1, 10), ADDR_A, PACKED & 0xFFFFu);    /* X   */
    psx_pgxp_alu(nullptr, OR(9, 10, 11), PACKED,
                 (PACKED >> 16) << 16, PACKED & 0xFFFFu);
    psx_pgxp_store(nullptr, SW(1, 11), ADDR_B, PACKED);
    {
        int32_t x, y;
        CHECK(lookup(ADDR_B, PACKED, 160, 80, &x, &y, nullptr) ==
              PGXP_SRC_DATAFLOW);
        CHECK(x == X16 && y == Y16);
    }

    /* --- cpu-mode addiu: fraction rides an integer offset (incl. -N) --- */
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_alu(nullptr, ADDIU(8, 12, 4), PACKED + 4u, PACKED, 4u);
    psx_pgxp_store(nullptr, SW(1, 12), ADDR_B, PACKED + 4u);
    {
        int32_t x, y;
        CHECK(lookup(ADDR_B, PACKED + 4u, 164, 80, &x, &y, nullptr) ==
              PGXP_SRC_DATAFLOW);
        CHECK(x == X16 + (4 << 16) && y == Y16);
    }
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_alu(nullptr, ADDIU(8, 12, (uint16_t)-4), PACKED - 4u, PACKED,
                 (uint32_t)(int32_t)-4);
    psx_pgxp_store(nullptr, SW(1, 12), ADDR_B, PACKED - 4u);
    {
        int32_t x;
        CHECK(lookup(ADDR_B, PACKED - 4u, 156, 80, &x, nullptr, nullptr) ==
              PGXP_SRC_DATAFLOW);
        CHECK(x == X16 - (4 << 16));
    }
    pgxp_set_cpu_mode(0);

    /* --- cpu-mode OFF: the same repack must degrade to native, cleanly --- */
    pgxp_invalidate_all();                    /* discard prior GPR shadows   */
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LHU(1, 8), ADDR_A + 2u, PACKED >> 16);
    psx_pgxp_alu(nullptr, SLL(8, 9, 16), (PACKED >> 16) << 16,
                 PACKED >> 16, 16);
    psx_pgxp_store(nullptr, SW(1, 9), ADDR_B, (PACKED >> 16) << 16);
    CHECK(lookup(ADDR_B, (PACKED >> 16) << 16, 0, 80, nullptr, nullptr,
                 nullptr) == PGXP_SRC_NATIVE);

    /* --- truncation agreement: integer part must match the native parse --- */
    produce_at(ADDR_A);
    CHECK(lookup(ADDR_A, PACKED, 161, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);

    /* --- tolerance clamp --- */
    produce_at(ADDR_A);
    pgxp_set_tolerance(0.25f);                 /* fraction is 0.5 -> reject  */
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);
    pgxp_set_tolerance(0.75f);                 /* 0.5 <= 0.75 -> accept      */
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_DATAFLOW);
    pgxp_set_tolerance(-1.0f);

    /* --- fallback tier: no address -> position cache, never a depth --- */
    g_fb_valid = 1; g_fb_packed = PACKED; g_fb_x16 = X16; g_fb_y16 = Y16;
    {
        int32_t x, y; uint16_t z;
        CHECK(lookup(0xFFFFFFFFu, PACKED, 160, 80, &x, &y, &z) ==
              PGXP_SRC_FALLBACK);
        CHECK(x == X16 && y == Y16 && z == 0);
    }
    g_fb_valid = 0;

    /* --- suppression bracket: nothing records inside it --- */
    pgxp_invalidate_all();
    pgxp_suppress_begin();
    produce_at(ADDR_A);
    pgxp_suppress_end();
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);

    /* --- deferred invalidate inside the bracket --- */
    produce_at(ADDR_A);
    pgxp_suppress_begin();
    pgxp_invalidate_all();                     /* deferred                   */
    pgxp_suppress_end();                       /* applies here               */
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);

    /* --- invalidate-all + generation wrap --- */
    produce_at(ADDR_A);
    pgxp_invalidate_all();
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);
    pgxp_test_set_generation(0xFFFFFFFFu);
    pgxp_invalidate_all();
    CHECK(pgxp_test_generation() == 1u);

    /* Disabled invalidation is a true no-op. Re-enable still advances the
     * generation before any hook can observe an old shadow. */
    pgxp_set_enabled(0);
    const uint32_t disabled_gen = pgxp_test_generation();
    PGXPStats disabled_before, disabled_after;
    pgxp_get_stats(&disabled_before);
    pgxp_invalidate_all();
    pgxp_get_stats(&disabled_after);
    CHECK(pgxp_test_generation() == disabled_gen);
    CHECK(disabled_after.invalidations == disabled_before.invalidations);
    pgxp_set_enabled(1);
    CHECK(pgxp_test_generation() == disabled_gen + 1u);

    /* --- test accessors mirror the SXY FIFO shadows --- */
    pgxp_test_seed_gte_sxy(2, PACKED, X16, Y16, SZ3, 1);
    {
        uint32_t packed; int32_t x, y; uint16_t z; uint8_t valid;
        pgxp_test_get_gte_sxy(2, &packed, &x, &y, &z, &valid);
        CHECK(valid && packed == PACKED && x == X16 && y == Y16 && z == SZ3);
        pgxp_test_seed_gte_sxy(2, 0, 0, 0, 0, 0);
        pgxp_test_get_gte_sxy(2, &packed, &x, &y, &z, &valid);
        CHECK(!valid);
    }

    /* --- stats sanity: dataflow hits were counted --- */
    {
        PGXPStats st;
        pgxp_get_stats(&st);
        CHECK(st.lookups > 0);
        CHECK(st.dataflow_hit > 0);
        CHECK(st.native > 0);
        CHECK(st.fallback_hit > 0);
        CHECK(st.value_mismatch > 0);
    }

    if (g_failures) {
        std::fprintf(stderr, "test_pgxp: %d FAILURES\n", g_failures);
        return 1;
    }
    std::printf("test_pgxp: all checks passed\n");
    return 0;
}
