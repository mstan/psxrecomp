/*
 * test_spu_fidelity.c — clean-room unit tests for the SPU DSP fidelity
 * features (GitHub issue #103): SPU IRQ (I_STAT bit 9), reverb, the noise
 * generator, volume sweeps, the capture buffers, and savestate coverage of
 * all the new state.
 *
 * Harness pattern: includes spu.c directly (white-box — the tests assert on
 * internal engine state like rev_cur and the sweep envelopes as well as on
 * guest-visible register reads and rendered PCM) and stubs the few externs
 * spu.c pulls in. Written from scratch against the psx-spx hardware
 * documentation; no emulator source consulted.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "../src/spu.c"

/* ---- stubs for spu.c externs -------------------------------------------- */
uint64_t s_frame_count = 0;

static uint32_t g_irq_raises = 0;
static uint32_t g_irq_last_bit = 0;
static uint32_t g_irq_last_detail = 0;
void psx_irq_raise(uint32_t bit, uint32_t detail) {
    g_irq_raises++;
    g_irq_last_bit = bit;
    g_irq_last_detail = detail;
}

uint64_t psx_get_cycle_count(void) { return 0; }
void audio_trace_pcm(int tap, const int16_t *stereo, int frames) {
    (void)tap; (void)stereo; (void)frames;
}
void audio_trace_event(uint16_t kind, uint32_t a, uint32_t b) {
    (void)kind; (void)a; (void)b;
}
bool spu_shadow_enabled(void) { return false; }
void spu_shadow_reset(void) {}
void spu_shadow_process(int16_t *canon, int frames) { (void)canon; (void)frames; }

/* ---- tiny check harness -------------------------------------------------- */
static int g_checks = 0;
static int g_fails = 0;
#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_fails++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* ---- helpers -------------------------------------------------------------- */
#define R_SPUCNT   0x1F801DAAu
#define R_SPUSTAT  0x1F801DAEu
#define R_IRQADDR  0x1F801DA4u
#define R_XFERADDR 0x1F801DA6u
#define R_FIFO     0x1F801DA8u
#define R_KON_LO   0x1F801D88u
#define R_NON_LO   0x1F801D94u
#define R_EON_LO   0x1F801D98u
#define R_MAINVOLL 0x1F801D80u
#define R_MAINVOLR 0x1F801D82u
#define R_CDVOLL   0x1F801DB0u
#define R_CDVOLR   0x1F801DB2u

static void wr(uint32_t addr, uint16_t v) { spu_write(addr, v); }
static uint16_t rd(uint32_t addr) { return (uint16_t)spu_read(addr); }

static int16_t g_render_buf[4096 * 2];
static void render_n(int frames) {
    while (frames > 0) {
        int n = frames > 4096 ? 4096 : frames;
        spu_render(g_render_buf, n);
        frames -= n;
    }
}

static void push_cd_const(int frames, int16_t l, int16_t r) {
    static int16_t buf[1024 * 2];
    while (frames > 0) {
        int n = frames > 1024 ? 1024 : frames;
        for (int i = 0; i < n; i++) { buf[i * 2] = l; buf[i * 2 + 1] = r; }
        spu_cd_audio_push(buf, n);
        frames -= n;
    }
}

/* voice register byte addresses: base + 2*reg (reg: 0=VOLL 1=VOLR 2=PITCH
 * 3=START 4=ADSRLO 5=ADSRHI 6=CURVOL 7=LOOP) */
static uint32_t vreg(int v, int reg) {
    return 0x1F801C00u + (uint32_t)(v * 8 + reg) * 2u;
}

static void setup_voice(int v, uint16_t start_unit, uint16_t voll, uint16_t volr) {
    wr(vreg(v, 0), voll);
    wr(vreg(v, 1), volr);
    wr(vreg(v, 2), 0x1000);      /* pitch 1.0 */
    wr(vreg(v, 3), start_unit);
    wr(vreg(v, 4), 0x0000);      /* Ar=0 (fastest attack) */
    wr(vreg(v, 5), 0x0000);
}

static int ram_region_zero(uint32_t start, uint32_t end) {
    for (uint32_t a = start; a < end; a++)
        if (spu_ram[a]) return 0;
    return 1;
}

static int buf_has_nonzero(const int16_t *buf, int samples) {
    for (int i = 0; i < samples; i++)
        if (buf[i]) return 1;
    return 0;
}

/* ==== 1. SPU IRQ ============================================================ */

/* FIFO, DMA-write, DMA-read, transfer-address-write and SPUCNT-write access
 * sites; latch/ack/re-arm behaviour; SPUSTAT bit 6 mirror. */
static void test_irq_transfer_paths(void) {
    spu_init();
    g_irq_raises = 0;

    /* Program the IRQ address and transfer address BEFORE enabling the IRQ:
     * enabling with both still at their reset value of 0 is itself a
     * (correct) unit-0 match under the enable-time re-check. */
    wr(R_SPUCNT, 0x8000);        /* SPU enable, IRQ still off */
    wr(R_IRQADDR, 0x0600);       /* IRQ at byte 0x3000 */
    wr(R_XFERADDR, 0x05FF);      /* transfer addr byte 0x2FF8: no match */
    wr(R_SPUCNT, 0x8040);        /* IRQ enable; resting unit 0x5FF: no fire */
    CHECK(g_irq_raises == 0);

    /* DMA writes advance 0x2FF8 -> 0x3000; the third word touches 0x3000. */
    spu_dma_write(0xAABBCCDDu);
    spu_dma_write(0x11223344u);
    CHECK(g_irq_raises == 0);
    spu_dma_write(0x55667788u);
    CHECK(g_irq_raises == 1);
    CHECK(g_irq_last_bit == 9);
    CHECK(g_irq_last_detail == 0x3000u);
    CHECK((rd(R_SPUSTAT) & 0x40u) != 0);

    /* Latched: another hit does not re-raise until acknowledged. */
    spu_dma_write(0x99999999u);  /* 0x3004..0x3007, still unit 0x600 */
    CHECK(g_irq_raises == 1);

    /* Acknowledge: SPUCNT write with bit 6 clear drops SPUSTAT bit 6. */
    wr(R_SPUCNT, 0x8000);
    CHECK((rd(R_SPUSTAT) & 0x40u) == 0);

    /* Re-arm with transfer address resting at a NON-matching unit. */
    wr(R_SPUCNT, 0x8040);        /* transfer now 0x3008 (unit 0x601): no fire */
    CHECK(g_irq_raises == 1);

    /* Transfer-address write pointing AT the IRQ address is a compare site. */
    wr(R_XFERADDR, 0x0600);
    CHECK(g_irq_raises == 2);
    CHECK((rd(R_SPUSTAT) & 0x40u) != 0);

    /* SPUCNT write with bit 6 set is a compare site too (transfer address
     * still resting on the IRQ unit). */
    wr(R_SPUCNT, 0x8000);        /* ack */
    wr(R_SPUCNT, 0x8040);        /* re-enable -> immediate re-hit */
    CHECK(g_irq_raises == 3);

    /* Manual FIFO writes: park below the IRQ unit while IRQ is acked, walk
     * up to it, and the store that touches byte 0x3000 fires. */
    wr(R_SPUCNT, 0x8000);        /* ack + disable */
    wr(R_XFERADDR, 0x05FF);      /* byte 0x2FF8 */
    wr(R_SPUCNT, 0x8040);        /* unit 0x5FF: no immediate fire */
    CHECK(g_irq_raises == 3);
    wr(R_FIFO, 0x1111);          /* 0x2FF8 */
    wr(R_FIFO, 0x2222);          /* 0x2FFA */
    wr(R_FIFO, 0x3333);          /* 0x2FFC */
    wr(R_FIFO, 0x4444);          /* 0x2FFE */
    CHECK(g_irq_raises == 3);
    wr(R_FIFO, 0x5555);          /* 0x3000 -> fire */
    CHECK(g_irq_raises == 4);

    /* DMA read direction: data comes back LE and the access fires the IRQ. */
    wr(R_SPUCNT, 0x8000);        /* ack + IRQ off (no checks while moving) */
    wr(R_XFERADDR, 0x05FF);      /* byte 0x2FF8 */
    CHECK(spu_dma_read() == 0x22221111u);   /* FIFO halfwords 1111,2222 */
    CHECK(spu_dma_read() == 0x44443333u);   /* FIFO halfwords 3333,4444 */
    wr(R_IRQADDR, 0x0601);       /* byte 0x3008 */
    wr(R_SPUCNT, 0x8040);        /* transfer at 0x3000 (unit 0x600): no fire */
    CHECK(g_irq_raises == 4);
    /* 0x3000: FIFO halfword 5555 over the DMA word 0x55667788's top half. */
    CHECK(spu_dma_read() == 0x55665555u);
    CHECK(g_irq_raises == 4);
    (void)spu_dma_read();                    /* 0x3004 */
    CHECK(g_irq_raises == 4);
    (void)spu_dma_read();                    /* 0x3008 -> fire */
    CHECK(g_irq_raises == 5);
    CHECK(g_irq_last_detail == 0x3008u);
}

/* SPUCNT bit 6 clear => no IRQ, ever, from any access. */
static void test_irq_disabled_never_fires(void) {
    spu_init();
    g_irq_raises = 0;

    wr(R_SPUCNT, 0x8000);        /* SPU enable, IRQ DISABLED */
    wr(R_IRQADDR, 0x0600);
    wr(R_XFERADDR, 0x0600);
    spu_dma_write(0x12345678u);
    wr(R_XFERADDR, 0x0600);
    wr(R_FIFO, 0xBEEF);
    render_n(16);                /* capture + reverb accesses also silent */
    CHECK(g_irq_raises == 0);
    CHECK((rd(R_SPUSTAT) & 0x40u) == 0);
}

/* Voice ADPCM block fetch is a compare site. */
static void test_irq_on_block_fetch(void) {
    spu_init();
    g_irq_raises = 0;

    wr(R_SPUCNT, 0x8000);
    wr(R_IRQADDR, 0x0600);       /* byte 0x3000 */
    wr(R_SPUCNT, 0x8040);        /* enable IRQ after aiming it */
    CHECK(g_irq_raises == 0);
    setup_voice(0, 0x0600, 0x3FFF, 0x3FFF);
    wr(R_KON_LO, 0x0001);
    render_n(2);                 /* first sample fetches the block at 0x3000 */
    CHECK(g_irq_raises == 1);
    CHECK(g_irq_last_detail == 0x3000u);
}

/* Capture-buffer writes are compare sites (IRQ parked in the CD-R ring). */
static void test_irq_on_capture_write(void) {
    spu_init();
    g_irq_raises = 0;

    wr(R_SPUCNT, 0x8000);
    wr(R_IRQADDR, 0x0080);       /* byte 0x400: CD-R capture ring, offset 0 */
    wr(R_SPUCNT, 0x8040);
    CHECK(g_irq_raises == 0);
    render_n(1);
    CHECK(g_irq_raises == 1);
    CHECK(g_irq_last_detail == 0x400u);
}

/* Reverb work-area accesses are compare sites (reads happen even with the
 * reverb master write-enable OFF). */
static void test_irq_on_reverb_access(void) {
    spu_init();
    g_irq_raises = 0;

    wr(R_SPUCNT, 0x8000);        /* enable; SPUCNT.7 (reverb) stays OFF */
    wr(R_IRQADDR, 0xFFF0);       /* byte 0x7FF80 */
    wr(0x1F801DA2u, 0xFFF0);     /* mBASE -> work area [0x7FF80, 0x80000) */
    wr(R_SPUCNT, 0x8040);        /* IRQ on; resting transfer unit 0: no fire */
    CHECK(g_irq_raises == 0);
    render_n(2);                 /* one 22050 Hz step -> reads at the base */
    CHECK(g_irq_raises == 1);
}

/* ==== 2. Reverb ============================================================= */

static void setup_basic_reverb(void) {
    wr(0x1F801DA2u, 0x2000);         /* mBASE: byte 0x10000 */
    wr(SPU_R_VLIN,  0x7FFF);
    wr(SPU_R_VRIN,  0x7FFF);
    wr(SPU_R_VIIR,  0x4000);
    wr(SPU_R_VWALL, 0x4000);
    wr(SPU_R_MLSAME, 0x0004);        /* write tap: +32 bytes */
    wr(SPU_R_MRSAME, 0x0104);
    wr(SPU_R_DLSAME, 0x0006);
    wr(SPU_R_DRSAME, 0x0106);
    wr(SPU_R_MLCOMB1, 0x0002);       /* read tap: +16 bytes (8 steps behind) */
    wr(SPU_R_MRCOMB1, 0x0102);
    wr(SPU_R_VCOMB1, 0x4000);
    wr(SPU_R_VLOUT, 0x7FFF);
    wr(SPU_R_VROUT, 0x7FFF);
    wr(R_CDVOLL, 0x7FFF);
    wr(R_CDVOLR, 0x7FFF);
    wr(R_MAINVOLL, 0x3FFF);
    wr(R_MAINVOLR, 0x3FFF);
}

/* Work-area WRITES are suppressed while SPUCNT.7 is clear and happen when it
 * is set — driven from the CD-audio-only path (zero active voices), which is
 * exactly the FMV case the old fast path silently skipped. */
static void test_reverb_write_gating_and_cd_path(void) {
    spu_init();
    g_irq_raises = 0;

    /* SPU enable + CD enable + CD reverb send; reverb write-enable OFF. */
    wr(R_SPUCNT, 0x8005);
    setup_basic_reverb();

    for (int v = 0; v < 24; v++) CHECK(!voices[v].active);

    push_cd_const(64, 0x2000, 0x2000);
    render_n(64);
    /* Reads happened, but nothing may have been written to the work area. */
    CHECK(ram_region_zero(0x10000u, 0x10800u));

    /* Now enable reverb writes (SPUCNT bit 7). */
    wr(R_SPUCNT, 0x8085);
    push_cd_const(64, 0x2000, 0x2000);
    render_n(64);
    CHECK(!ram_region_zero(0x10000u, 0x10800u));

    /* Reverb tail: CD input stops; the wet path must keep the output alive
     * (this is the reverb-on-CD-only / FMV behaviour in one assert). */
    memset(g_render_buf, 0, sizeof(g_render_buf));
    spu_render(g_render_buf, 64);
    CHECK(buf_has_nonzero(g_render_buf, 64 * 2));
}

/* Reverb addressing wraps inside [mBASE, 0x80000) and never escapes it,
 * whatever the offset registers hold. */
static void test_reverb_addressing_wrap(void) {
    spu_init();

    wr(R_SPUCNT, 0x8085);            /* enable + CD + reverb writes (no IRQ) */
    wr(0x1F801DA2u, 0xFFF0);         /* mBASE byte 0x7FF80: 128-byte area */
    wr(SPU_R_VLIN,  0x7FFF);
    wr(SPU_R_VRIN,  0x7FFF);
    wr(SPU_R_VIIR,  0x4000);
    wr(SPU_R_VWALL, 0x4000);
    wr(SPU_R_MLSAME, 0x0020);        /* +0x100 bytes: exceeds the area, must wrap */
    wr(SPU_R_MRSAME, 0x0030);
    wr(SPU_R_DLSAME, 0x0040);
    wr(SPU_R_DRSAME, 0x0050);
    wr(SPU_R_MLAPF1, 0x1234);        /* arbitrary large offsets */
    wr(SPU_R_MRAPF1, 0x4321);
    wr(SPU_R_MLAPF2, 0x7FFF);
    wr(SPU_R_MRAPF2, 0x0FFF);
    wr(SPU_R_DAPF1, 0x3000);
    wr(SPU_R_DAPF2, 0x0800);
    wr(SPU_R_VAPF1, 0x3000);
    wr(SPU_R_VAPF2, 0x3000);
    wr(SPU_R_VLOUT, 0x7FFF);
    wr(SPU_R_VROUT, 0x7FFF);
    wr(R_CDVOLL, 0x7FFF);
    wr(R_CDVOLR, 0x7FFF);

    /* Every effective address the engine can form stays inside the area. */
    static const uint32_t addr_regs[] = {
        SPU_R_MLSAME, SPU_R_MRSAME, SPU_R_DLSAME, SPU_R_DRSAME,
        SPU_R_MLAPF1, SPU_R_MRAPF1, SPU_R_MLAPF2, SPU_R_MRAPF2,
    };
    for (unsigned i = 0; i < sizeof(addr_regs) / sizeof(addr_regs[0]); i++) {
        uint32_t ea0 = rev_ea(addr_regs[i], 0);
        uint32_t eam = rev_ea(addr_regs[i], -2);
        uint32_t eap = rev_ea(addr_regs[i], -((int32_t)RREG(SPU_R_DAPF1) << 3));
        CHECK(ea0 >= 0x7FF80u && ea0 < 0x80000u);
        CHECK(eam >= 0x7FF80u && eam < 0x80000u);
        CHECK(eap >= 0x7FF80u && eap < 0x80000u);
    }

    /* Run long enough to lap the 128-byte area several times. */
    push_cd_const(256, 0x3000, -0x3000);
    render_n(256);
    CHECK(rev_cur >= 0x7FF80u && rev_cur < 0x80000u);
    /* Nothing below the work area was touched (capture rings stay <0x1000). */
    CHECK(ram_region_zero(0x7F000u, 0x7FF80u));
    /* The area itself was written. */
    CHECK(!ram_region_zero(0x7FF80u, 0x80000u));
}

/* ==== 3. Noise generator ==================================================== */

static void test_noise_generator(void) {
    /* Control: same voice in ADPCM mode over all-zero SPU RAM is silent. */
    spu_init();
    wr(R_SPUCNT, 0xB400);            /* enable; noise shift=13, step=4 */
    wr(R_MAINVOLL, 0x3FFF);
    wr(R_MAINVOLR, 0x3FFF);
    setup_voice(0, 0x0600, 0x3FFF, 0x3FFF);
    wr(R_KON_LO, 0x0001);
    memset(g_render_buf, 0, sizeof(g_render_buf));
    spu_render(g_render_buf, 512);
    CHECK(!buf_has_nonzero(g_render_buf, 512 * 2));

    /* Noise mode: same setup + NON bit -> non-silent, non-constant output. */
    spu_init();
    wr(R_SPUCNT, 0xB400);
    wr(R_MAINVOLL, 0x3FFF);
    wr(R_MAINVOLR, 0x3FFF);
    setup_voice(0, 0x0600, 0x3FFF, 0x3FFF);
    wr(R_NON_LO, 0x0001);
    wr(R_KON_LO, 0x0001);
    memset(g_render_buf, 0, sizeof(g_render_buf));
    spu_render(g_render_buf, 512);
    CHECK(buf_has_nonzero(g_render_buf, 512 * 2));

    /* Count distinct left-channel values: the LFSR must keep moving. */
    int distinct = 0;
    for (int i = 0; i < 512; i++) {
        int seen = 0;
        for (int j = 0; j < i; j++)
            if (g_render_buf[j * 2] == g_render_buf[i * 2]) { seen = 1; break; }
        if (!seen) distinct++;
    }
    CHECK(distinct >= 8);
    CHECK(noise_lfsr != 0);          /* self-seeded from 0 via the xor-1 term */
}

/* ==== 4. Volume sweeps + direct decode ===================================== */

static void test_direct_volume_decode(void) {
    spu_init();
    /* Documented decode: effective volume = signed bits14-0 << 1. */
    wr(vreg(0, 0), 0x2000);
    CHECK(sweep_voice_env[0][0].level == 0x4000);
    wr(vreg(0, 0), 0x3FFF);
    CHECK(sweep_voice_env[0][0].level == 0x7FFE);
    wr(vreg(0, 0), 0x4001);          /* negative: (int16)(0x4001<<1) = -32766 */
    CHECK(sweep_voice_env[0][0].level == -32766);
    wr(vreg(0, 0), 0x0000);
    CHECK(sweep_voice_env[0][0].level == 0);

    /* End-to-end gain pipeline: CD sample 0x1000 x CD vol 0x7FFF x main
     * 0x3FFF(direct => 0x7FFE) = 4094. Locks the >>15 application shape. */
    spu_init();
    wr(R_SPUCNT, 0x8001);            /* enable + CD audio */
    wr(R_CDVOLL, 0x7FFF);
    wr(R_CDVOLR, 0x7FFF);
    wr(R_MAINVOLL, 0x3FFF);
    wr(R_MAINVOLR, 0x3FFF);
    push_cd_const(1, 0x1000, 0x1000);
    spu_render(g_render_buf, 1);
    CHECK(g_render_buf[0] == 4094);
    CHECK(g_render_buf[1] == 4094);
}

static void test_sweep_rise_and_fall(void) {
    spu_init();
    wr(R_SPUCNT, 0x8000);

    /* Linear increase sweep, rate 0x20, from level 0. */
    wr(vreg(0, 0), 0x8000 | 0x0020);
    render_n(64);
    int16_t mid = sweep_voice_env[0][0].level;
    CHECK(mid > 0);
    CHECK((int16_t)rd(vreg(0, 0)) == mid);   /* guest read sees the live level */
    render_n(64);
    int16_t late = sweep_voice_env[0][0].level;
    CHECK(late > mid);

    /* Decrease sweep starts from the current (direct-set) level and falls. */
    wr(vreg(0, 1), 0x3FFF);                  /* direct: level 0x7FFE */
    CHECK(sweep_voice_env[0][1].level == 0x7FFE);
    wr(vreg(0, 1), 0x8000 | 0x2000 | 0x0020); /* sweep, decrease, rate 0x20 */
    render_n(64);
    int16_t fall_mid = sweep_voice_env[0][1].level;
    CHECK(fall_mid < 0x7FFE);
    CHECK(fall_mid >= 0);
    render_n(64);
    int16_t fall_late = sweep_voice_env[0][1].level;
    CHECK(fall_late < fall_mid);
    CHECK(fall_late >= 0);

    /* Main-volume sweeps use the same machinery. */
    wr(R_MAINVOLL, 0x8000 | 0x0020);
    render_n(64);
    CHECK(sweep_main_env[0].level > 0);
    /* Current-main-volume mirror register reports the live level. */
    CHECK((int16_t)rd(0x1F801DB8u) == sweep_main_env[0].level);
}

/* Capture rings advance one halfword per output sample, wrap at 0x400, and
 * record the raw CD input bus. */
static void test_capture_buffer_contents(void) {
    spu_init();
    wr(R_SPUCNT, 0x8000);            /* SPU enable; CD MIX disabled — the
                                        capture rings record the bus anyway */
    push_cd_const(4, 0x1234, 0x2345);
    render_n(4);
    for (int i = 0; i < 4; i++) {
        uint16_t l = (uint16_t)(spu_ram[i * 2] | (spu_ram[i * 2 + 1] << 8));
        uint16_t r = (uint16_t)(spu_ram[0x400 + i * 2] | (spu_ram[0x400 + i * 2 + 1] << 8));
        CHECK(l == 0x1234);
        CHECK(r == 0x2345);
    }
    CHECK(capture_pos == 8);

    render_n(508);                   /* 512 total -> exactly one wrap */
    CHECK(capture_pos == 0);
    CHECK((rd(R_SPUSTAT) & 0x800u) == 0);   /* first half again */
    render_n(256);
    CHECK(capture_pos == 0x200);
    CHECK((rd(R_SPUSTAT) & 0x800u) != 0);   /* second half */
}

/* ==== 5. Savestate round-trip =============================================== */

static void test_savestate_roundtrip(void) {
    spu_init();
    g_irq_raises = 0;

    /* Busy state: reverb ringing, noise voice keyed, sweeps mid-glide,
     * capture advanced, IRQ latched. */
    wr(R_SPUCNT, 0xB485);    /* enable + noise(13,4) + reverb + CD + CD-rev */
    wr(R_IRQADDR, 0x0080);   /* park the IRQ in the CD-R capture ring */
    wr(R_SPUCNT, 0xB4C5);    /* + IRQ enable -> first render latches it */
    setup_basic_reverb();
    setup_voice(5, 0x0600, 0x3FFF, 0x3FFF);
    wr(R_NON_LO, 0x0020);           /* voice 5 is a noise voice */
    wr(R_EON_LO, 0x0020);           /* voice 5 also feeds reverb */
    wr(vreg(6, 0), 0x8000 | 0x0020); /* an idle voice's sweep still glides */
    wr(R_KON_LO, 0x0020);
    push_cd_const(128, 0x1800, -0x1800);
    render_n(128);                   /* ring drained exactly; state is rich */
    CHECK(irq_flag == 1);

    uint32_t n = spu_snapshot_bytes();
    uint8_t *snap = (uint8_t *)malloc(n);
    CHECK(snap != NULL);
    spu_snapshot_write(snap);
    /* The runtime savestate persists the 512KB SPU RAM separately (via
     * spu_get_ram_ptr); mirror that here — the reverb work area and capture
     * rings live in RAM and timeline B must replay over identical bytes. */
    uint8_t *ram_snap = (uint8_t *)malloc(SPU_RAM_SIZE);
    CHECK(ram_snap != NULL);
    memcpy(ram_snap, spu_ram, SPU_RAM_SIZE);

    uint32_t rev_cur_0     = rev_cur;
    uint8_t  rev_phase_0   = rev_phase;
    uint16_t noise_lfsr_0  = noise_lfsr;
    int32_t  noise_timer_0 = noise_timer;
    uint32_t capture_pos_0 = capture_pos;
    uint8_t  irq_flag_0    = irq_flag;
    int16_t  sweep_lvl_0   = sweep_voice_env[6][0].level;
    uint32_t sweep_div_0   = sweep_voice_env[6][0].divider;
    int32_t  rev_out_l_0   = rev_out_l;

    /* Timeline A. */
    static int16_t out_a[256 * 2], out_b[256 * 2];
    spu_render(out_a, 256);
    CHECK(buf_has_nonzero(out_a, 256 * 2));   /* noise + reverb tail audible */

    /* State moved on... */
    CHECK(rev_cur != rev_cur_0 || noise_lfsr != noise_lfsr_0
          || capture_pos != capture_pos_0);

    /* ...restore and replay: timeline B must be byte-identical. */
    CHECK(spu_snapshot_read(snap, n) == 1);
    memcpy(spu_ram, ram_snap, SPU_RAM_SIZE);
    CHECK(rev_cur == rev_cur_0);
    CHECK(rev_phase == rev_phase_0);
    CHECK(noise_lfsr == noise_lfsr_0);
    CHECK(noise_timer == noise_timer_0);
    CHECK(capture_pos == capture_pos_0);
    CHECK(irq_flag == irq_flag_0);
    CHECK(sweep_voice_env[6][0].level == sweep_lvl_0);
    CHECK(sweep_voice_env[6][0].divider == sweep_div_0);
    CHECK(rev_out_l == rev_out_l_0);

    spu_render(out_b, 256);
    CHECK(memcmp(out_a, out_b, sizeof(out_a)) == 0);

    /* A truncated blob is rejected. */
    CHECK(spu_snapshot_read(snap, n - 1) == 0);

    free(ram_snap);
    free(snap);
}

/* ==== main ================================================================== */

int main(void) {
    test_irq_transfer_paths();
    test_irq_disabled_never_fires();
    test_irq_on_block_fetch();
    test_irq_on_capture_write();
    test_irq_on_reverb_access();
    test_reverb_write_gating_and_cd_path();
    test_reverb_addressing_wrap();
    test_noise_generator();
    test_direct_volume_decode();
    test_sweep_rise_and_fall();
    test_capture_buffer_contents();
    test_savestate_roundtrip();

    printf("test_spu_fidelity: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
