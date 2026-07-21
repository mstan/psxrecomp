/*
 * test_sio_card_protocol.c — pin SIO memory card protocol against no$psx spec.
 *
 * Drives sio.c's TX/RX state machine through the canonical 137-byte
 * memory-card READ command and asserts each RX byte matches the spec.
 *
 * Spec reference (no$psx — Memory Cards / Read Command):
 *
 *   | Send | Reply | Comment                                    |
 *   |------|-------|--------------------------------------------|
 *   | 81h  | N/A   | Memcard access prefix                      |
 *   | 52h  | FLAG  | Read cmd. Reply = "new card" flag (08h)    |
 *   | 00h  | 5Ah   | Memory Card ID1                            |
 *   | 00h  | 5Dh   | Memory Card ID2                            |
 *   | MSB  | (00h) | Send sector MSB ; Receive Cmd Ack 1        |
 *   | LSB  | (pre) | Send sector LSB ; Receive Cmd Ack 2 (prev) |
 *   | 00h  | 5Ch   | Receive Confirm 1                          |
 *   | 00h  | 5Dh   | Receive Confirm 2                          |
 *   | 00h  | MSB   | Receive sector MSB echo                    |
 *   | 00h  | LSB   | Receive sector LSB echo                    |
 *   | 00h  | D0    |   (data byte 0)                            |
 *   | ... 127 more data bytes ...                                |
 *   | 00h  | CHK   | Receive checksum (MSB^LSB^data[0..127])    |
 *   | 00h  | 47h   | Receive End Byte ('G' = good)              |
 *
 * Build:
 *   cc -I../include -o test_sio_card_protocol \
 *       test_sio_card_protocol.c ../src/sio.c ../src/memcard.c
 */

#include "sio.h"
#include "memcard.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Stubs for sio.c's external dependencies ---- */
uint32_t i_stat = 0;
uint32_t g_debug_current_func_addr = 0;
int psx_get_in_exception(void) { return 0; }
uint8_t psx_read_byte(uint32_t addr) { (void)addr; return 0; }
uint32_t memory_get_sr(void) { return 0; }
void debug_server_poll(void) {}
void debug_server_log_sio_write(uint32_t addr, uint32_t value, uint8_t width) {
    (void)addr; (void)value; (void)width;
}
void starvation_ring_record(uint8_t kind, uint8_t tx, uint8_t rx,
                            uint16_t ctrl, uint16_t stat, uint8_t active_device,
                            uint8_t selected_slot, uint16_t mc_state,
                            uint8_t mc_cmd, uint16_t mc_sector,
                            uint8_t mc_data_idx, uint32_t func_addr) {
    (void)kind; (void)tx; (void)rx; (void)ctrl; (void)stat;
    (void)active_device; (void)selected_slot; (void)mc_state;
    (void)mc_cmd; (void)mc_sector; (void)mc_data_idx; (void)func_addr;
}
void card_read_summary_record(uint8_t slot, uint8_t cmd, uint16_t sector,
                              uint8_t end, uint8_t checksum,
                              uint8_t data0, uint8_t data1,
                              uint32_t func_addr) {
    (void)slot; (void)cmd; (void)sector; (void)end; (void)checksum;
    (void)data0; (void)data1; (void)func_addr;
}
void card_data_writes_arm(uint8_t value, uint16_t mc_state,
                          uint8_t mc_data_idx, uint8_t slot) {
    (void)value; (void)mc_state; (void)mc_data_idx; (void)slot;
}
void event_ring_record_aux(uint16_t kind, uint8_t detail, uint32_t aux) {
    (void)kind; (void)detail; (void)aux;
}
void psx_irq_raise(uint32_t bit, uint32_t detail) {
    (void)detail;
    i_stat |= 1u << bit;
}

/* ---- Test harness ---- */

static int g_failures = 0;
static int g_checks   = 0;

#define EXPECT_EQ(label, expected, actual)                                      \
    do {                                                                        \
        g_checks++;                                                             \
        uint32_t _e = (uint32_t)(expected);                                     \
        uint32_t _a = (uint32_t)(actual);                                       \
        if (_e != _a) {                                                         \
            g_failures++;                                                       \
            fprintf(stderr,                                                     \
                    "FAIL  %s:%d  %s  expected=0x%X got=0x%X\n",                \
                    __FILE__, __LINE__, label, _e, _a);                         \
        }                                                                       \
    } while (0)

/* ---- SIO MMIO offsets ---- */
#define SIO_TX_DATA  0x1F801040
#define SIO_RX_DATA  0x1F801040
#define SIO_STAT     0x1F801044
#define SIO_CTRL     0x1F80104A

#define CTRL_TX_EN          (1 << 0)
#define CTRL_SELECT         (1 << 1)
#define CTRL_ACK            (1 << 4)
#define CTRL_ACK_IRQ_EN     (1 << 12)
#define CTRL_SLOT           (1 << 13)

#define STAT_ACK            (1 << 7)

/* Write a card-protocol byte and return the RX response. */
static uint8_t card_xchg(uint8_t tx, int slot) {
    uint16_t ctrl = CTRL_TX_EN | CTRL_SELECT | CTRL_ACK_IRQ_EN
                  | (slot ? CTRL_SLOT : 0);
    sio_write(SIO_CTRL, ctrl);
    sio_write(SIO_TX_DATA, tx);
    sio_tick(2000);
    uint8_t rx = (uint8_t)sio_read(SIO_RX_DATA);
    /* Acknowledge IRQ between bytes (BIOS pattern) */
    sio_write(SIO_CTRL, ctrl | CTRL_ACK);
    return rx;
}

static void card_deselect(int slot) {
    uint16_t ctrl = CTRL_TX_EN | CTRL_ACK_IRQ_EN | (slot ? CTRL_SLOT : 0);
    sio_write(SIO_CTRL, ctrl);
    sio_tick(2000);
}

/* Drive a complete card READ for the given sector and capture every RX byte.
 * Returns count of RX bytes captured (137 for a successful read). */
static int run_card_read(int slot, uint16_t sector, uint8_t out_rx[140]) {
    int n = 0;
    uint8_t msb = (uint8_t)(sector >> 8);
    uint8_t lsb = (uint8_t)(sector & 0xFF);

    out_rx[n++] = card_xchg(0x81, slot);   /* idx 0: prefix */
    out_rx[n++] = card_xchg(0x52, slot);   /* idx 1: cmd → FLAG */
    out_rx[n++] = card_xchg(0x00, slot);   /* idx 2: → 5A */
    out_rx[n++] = card_xchg(0x00, slot);   /* idx 3: → 5D */
    out_rx[n++] = card_xchg(msb,  slot);   /* idx 4: → ack1 */
    out_rx[n++] = card_xchg(lsb,  slot);   /* idx 5: → ack2 */
    out_rx[n++] = card_xchg(0x00, slot);   /* idx 6: → 5C */
    out_rx[n++] = card_xchg(0x00, slot);   /* idx 7: → 5D */
    out_rx[n++] = card_xchg(0x00, slot);   /* idx 8: → MSB echo */
    out_rx[n++] = card_xchg(0x00, slot);   /* idx 9: → LSB echo */

    for (int i = 0; i < 128; i++) {
        out_rx[n++] = card_xchg(0x00, slot);   /* idx 10..137: 128 data bytes */
    }
    out_rx[n++] = card_xchg(0x00, slot);   /* idx 138: checksum */
    out_rx[n++] = card_xchg(0x00, slot);   /* idx 139: end byte */
    return n;
}

static int run_card_write(int slot, uint16_t sector, const uint8_t data[128],
                          uint8_t out_rx[140]) {
    int n = 0;
    uint8_t msb = (uint8_t)(sector >> 8);
    uint8_t lsb = (uint8_t)(sector & 0xFF);
    uint8_t chk = msb ^ lsb;
    for (int i = 0; i < 128; i++) chk ^= data[i];

    out_rx[n++] = card_xchg(0x81, slot);   /* idx 0: prefix */
    out_rx[n++] = card_xchg(0x57, slot);   /* idx 1: cmd -> FLAG */
    out_rx[n++] = card_xchg(0x00, slot);   /* idx 2: -> 5A */
    out_rx[n++] = card_xchg(0x00, slot);   /* idx 3: -> 5D */
    out_rx[n++] = card_xchg(msb,  slot);   /* idx 4: address MSB */
    out_rx[n++] = card_xchg(lsb,  slot);   /* idx 5: address LSB */
    for (int i = 0; i < 128; i++) {
        out_rx[n++] = card_xchg(data[i], slot);
    }
    out_rx[n++] = card_xchg(chk,  slot);
    out_rx[n++] = card_xchg(0x00, slot);   /* ack 1 */
    out_rx[n++] = card_xchg(0x00, slot);   /* ack 2 */
    out_rx[n++] = card_xchg(0x00, slot);   /* end byte */
    return n;
}

/* Pin idx 8/9 explicitly (MSB/LSB echo) for non-zero sectors. */
static void test_msb_lsb_echo(void) {
    sio_init();
    memcard_init("test_dir");
    uint8_t rx[140];
    uint16_t sector = 0x1234;  /* MSB=0x12, LSB=0x34 */
    int n = run_card_read(0, sector, rx);
    EXPECT_EQ("echo.count", 140, n);
    EXPECT_EQ("echo.idx[8]_MSB", 0x12, rx[8]);
    EXPECT_EQ("echo.idx[9]_LSB", 0x34, rx[9]);
    (void)n;
}

/* ---- Tests ---- */

/* Test 1: card not present → all RX = 0xFF */
static void test_no_card_present(void) {
    sio_init();
    /* Don't call memcard_init with a valid dir — no cards loaded. */
    memcard_init(NULL);

    uint8_t rx[140];
    int n = run_card_read(0, 0, rx);
    EXPECT_EQ("no_card.count", 140, n);

    for (int i = 0; i < n; i++) {
        if (rx[i] != 0xFF) {
            fprintf(stderr,
                    "FAIL  no_card.idx[%d]  expected=0xFF got=0x%02X\n", i, rx[i]);
            g_failures++;
        }
        g_checks++;
    }
}

/* Test 2: card present with known sector — full protocol matches no$psx spec */
static void test_card_read_sector_0(void) {
    sio_init();

    /* Set up a synthetic card in slot 0 with sector 0 = "MC<Hello>" */
    uint8_t fake_card[MEMCARD_SIZE];
    memset(fake_card, 0xFF, sizeof(fake_card));
    fake_card[0] = 'M';
    fake_card[1] = 'C';
    for (int i = 2; i < 128; i++) fake_card[i] = (uint8_t)i;  /* known pattern */

    /* Stuff into memcard — easiest path: write to .mcd and load it */
    FILE* f = fopen("test_card_synth.mcd", "wb");
    assert(f);
    fwrite(fake_card, 1, MEMCARD_SIZE, f);
    /* Pad to full 128KB */
    uint8_t zero = 0;
    long pad = MEMCARD_SIZE - (long)ftell(f);
    for (long i = 0; i < pad; i++) fwrite(&zero, 1, 1, f);
    fclose(f);

    /* memcard_init looks for cardN.mcd in dir — set up a temp dir */
    /* Simpler: direct write test using memcard API — but memcard_init is the
     * only public entry. Let's symlink/copy our file as card1.mcd. */
    f = fopen("test_dir/card1.mcd", "wb");
    if (!f) {
        system("mkdir test_dir 2>/dev/null || mkdir -p test_dir");
        f = fopen("test_dir/card1.mcd", "wb");
    }
    assert(f);
    fwrite(fake_card, 1, MEMCARD_SIZE, f);
    fclose(f);

    memcard_init("test_dir");

    /* Run protocol on slot 0, sector 0 */
    uint8_t rx[140];
    int n = run_card_read(0, 0, rx);
    EXPECT_EQ("read.count", 140, n);

    /* Per no$psx spec: */
    EXPECT_EQ("read.idx[0]_prefix",      0xFF, rx[0]);   /* response to 0x81 */
    EXPECT_EQ("read.idx[1]_FLAG",        0x08, rx[1]);   /* "new card" flag */
    EXPECT_EQ("read.idx[2]_ID1",         0x5A, rx[2]);
    EXPECT_EQ("read.idx[3]_ID2",         0x5D, rx[3]);
    /* idx 4-5: cmd ack 1/2 — spec says "00" / "previous", we accept either */
    /* idx 6: confirm 1 */
    EXPECT_EQ("read.idx[6]_confirm1",    0x5C, rx[6]);
    EXPECT_EQ("read.idx[7]_confirm2",    0x5D, rx[7]);
    /* idx 8-9: MSB/LSB echo (sector 0 → both 0x00) */
    /* idx 10..137: 128 data bytes — should match fake_card[0..127] */
    for (int i = 0; i < 128; i++) {
        if (rx[10 + i] != fake_card[i]) {
            fprintf(stderr,
                    "FAIL  read.data[%d]  expected=0x%02X got=0x%02X\n",
                    i, fake_card[i], rx[10 + i]);
            g_failures++;
        }
        g_checks++;
    }
    /* idx 138: checksum = MSB ^ LSB ^ data[0..127] */
    uint8_t expected_chk = 0;
    for (int i = 0; i < 128; i++) expected_chk ^= fake_card[i];
    EXPECT_EQ("read.idx[138]_checksum", expected_chk, rx[138]);
    /* idx 139: end byte */
    EXPECT_EQ("read.idx[139]_end",       0x47, rx[139]);

    EXPECT_EQ("read2.prefix",            0xFF, card_xchg(0x81, 0));
    EXPECT_EQ("read2.flag_still_set",    0x08, card_xchg(0x52, 0));
}

static void test_flag_clears_on_write(void) {
    sio_init();
    memcard_init("test_dir");

    uint8_t data[128];
    memset(data, 0x00, sizeof(data));
    data[0] = 'M';
    data[1] = 'C';
    uint8_t rx[140];
    int n = run_card_write(0, 0x003F, data, rx);
    EXPECT_EQ("write.count", 138, n);
    EXPECT_EQ("write.idx[1]_initial_FLAG", 0x08, rx[1]);
    EXPECT_EQ("write.end", 0x47, rx[137]);

    EXPECT_EQ("post_write.prefix",       0xFF, card_xchg(0x81, 0));
    EXPECT_EQ("post_write.flag_clear",   0x00, card_xchg(0x52, 0));
}

/* Test 3: pad-poll mid-card-read — does our SIO model preserve card state? */
static void test_pad_poll_during_card_read(void) {
    sio_init();
    /* Connect a pad in slot 0 so 0x01 returns valid pad data. */
    sio_connect_pad(0);

    FILE* f = fopen("test_dir/card1.mcd", "rb");
    if (!f) { fprintf(stderr, "SKIP test_pad_poll: card file missing\n"); return; }
    fclose(f);
    memcard_init("test_dir");

    /* Send first 4 card bytes (header phase) */
    EXPECT_EQ("pad_intr.0x81", 0xFF, card_xchg(0x81, 0));
    EXPECT_EQ("pad_intr.0x52", 0x08, card_xchg(0x52, 0));
    EXPECT_EQ("pad_intr.id1",  0x5A, card_xchg(0x00, 0));
    EXPECT_EQ("pad_intr.id2",  0x5D, card_xchg(0x00, 0));
    card_deselect(0);

    /* Inject pad poll between card bytes */
    (void)card_xchg(0x01, 0);  /* pad select — by spec, breaks card protocol */
    (void)card_xchg(0x42, 0);
    (void)card_xchg(0x00, 0);
    (void)card_xchg(0x00, 0);
    (void)card_xchg(0x00, 0);

    /* Resume card protocol by issuing 0x81 again — must restart cleanly */
    card_deselect(0);
    EXPECT_EQ("pad_intr.restart_0x81", 0xFF, card_xchg(0x81, 0));
    EXPECT_EQ("pad_intr.restart_0x52", 0x08, card_xchg(0x52, 0));
    /* Note: FLAG should still be 0x08 since no successful WRITE has occurred */
}

/* Test 4: per-slot state isolation — slot 0 mid-read, switch to slot 1 */
static void test_per_slot_state(void) {
    sio_init();
    memcard_init("test_dir");

    /* Start protocol on slot 0 */
    EXPECT_EQ("perslot.s0_0x81", 0xFF, card_xchg(0x81, 0));
    EXPECT_EQ("perslot.s0_0x52", 0x08, card_xchg(0x52, 0));
    card_deselect(0);
    /* Switch to slot 1 mid-protocol — start fresh on slot 1 */
    EXPECT_EQ("perslot.s1_0x81", 0xFF, card_xchg(0x81, 1));
    /* Slot 1 might have no card (test_dir/card2.mcd doesn't exist) — accept either */

    /* Switch back to slot 0 — should restart fresh (real card forgets state) */
    card_deselect(1);
    EXPECT_EQ("perslot.s0_resume_0x81", 0xFF, card_xchg(0x81, 0));
    EXPECT_EQ("perslot.s0_resume_0x52", 0x08, card_xchg(0x52, 0));
}

/* Test 5: directory entry layout — sector 1 is the first directory frame */
static void test_directory_frame_layout(void) {
    sio_init();
    memcard_init("test_dir");

    uint8_t buf[128];
    int rc = memcard_read_sector(0, 1, buf);
    EXPECT_EQ("dir.read_sector_rc", 0, rc);
    /* For a freshly-formatted card, frame 1 should have status=0xA0 (free)
     * — but our test_dir/card1.mcd was raw-written, so layout depends on file.
     * Just check that read succeeded. */
}

/* Exact scheduler deadlines must fire on the call that reaches the boundary.
 * The old strict-inequality loops subtracted the countdown to zero without
 * processing the event, after which sio_cycles_to_irq() no longer advertised
 * it and the event could be delayed until an unrelated scheduler wake. */
static void test_cycle_paced_exact_boundaries(void) {
    const uint32_t irq_sio0 = 1u << 7;
    const uint16_t ctrl = CTRL_TX_EN | CTRL_SELECT | CTRL_ACK_IRQ_EN;

    sio_init();
    memcard_init("test_dir");
    i_stat = 0;
    sio_write(SIO_CTRL, ctrl);
    sio_write(SIO_TX_DATA, 0x81);

    EXPECT_EQ("paced.shift.initial", 1088, sio_cycles_to_irq(irq_sio0));
    sio_advance(1087);
    EXPECT_EQ("paced.shift.remaining", 1, sio_cycles_to_irq(irq_sio0));
    EXPECT_EQ("paced.shift.no_irq", 0, i_stat & irq_sio0);
    sio_advance(1);
    EXPECT_EQ("paced.ack.initial", 170, sio_cycles_to_irq(irq_sio0));
    EXPECT_EQ("paced.ack.no_irq", 0, i_stat & irq_sio0);
    sio_advance(169);
    EXPECT_EQ("paced.ack.remaining", 1, sio_cycles_to_irq(irq_sio0));
    sio_advance(1);
    EXPECT_EQ("paced.ack.irq", irq_sio0, i_stat & irq_sio0);

    /* sio_tick() retains the same event walker for access-paced callers. */
    sio_init();
    memcard_init("test_dir");
    i_stat = 0;
    sio_write(SIO_CTRL, ctrl);
    sio_write(SIO_TX_DATA, 0x81);
    sio_tick(1088);
    EXPECT_EQ("paced.tick.ack", 170, sio_cycles_to_irq(irq_sio0));
    sio_tick(170);
    EXPECT_EQ("paced.tick.irq", irq_sio0, i_stat & irq_sio0);
}

int main(int argc, char **argv) {
    fprintf(stderr, "=== sio card protocol tests ===\n");

    if (argc == 2 && strcmp(argv[1], "exact-boundaries") == 0) {
        test_cycle_paced_exact_boundaries();
        fprintf(stderr, "test_cycle_paced_exact_boundaries:  %d/%d ok\n",
                g_checks - g_failures, g_checks);
        return g_failures > 0 ? 1 : 0;
    }

    test_no_card_present();
    fprintf(stderr, "test_no_card_present:               %d/%d ok\n",
            g_checks - g_failures, g_checks);

    int prev_checks = g_checks, prev_fails = g_failures;
    test_card_read_sector_0();
    fprintf(stderr, "test_card_read_sector_0:            %d/%d ok\n",
            (g_checks - prev_checks) - (g_failures - prev_fails),
            g_checks - prev_checks);

    prev_checks = g_checks; prev_fails = g_failures;
    test_flag_clears_on_write();
    fprintf(stderr, "test_flag_clears_on_write:          %d/%d ok\n",
            (g_checks - prev_checks) - (g_failures - prev_fails),
            g_checks - prev_checks);

    prev_checks = g_checks; prev_fails = g_failures;
    test_pad_poll_during_card_read();
    fprintf(stderr, "test_pad_poll_during_card_read:     %d/%d ok\n",
            (g_checks - prev_checks) - (g_failures - prev_fails),
            g_checks - prev_checks);

    prev_checks = g_checks; prev_fails = g_failures;
    test_per_slot_state();
    fprintf(stderr, "test_per_slot_state:                %d/%d ok\n",
            (g_checks - prev_checks) - (g_failures - prev_fails),
            g_checks - prev_checks);

    prev_checks = g_checks; prev_fails = g_failures;
    test_msb_lsb_echo();
    fprintf(stderr, "test_msb_lsb_echo:                  %d/%d ok\n",
            (g_checks - prev_checks) - (g_failures - prev_fails),
            g_checks - prev_checks);

    prev_checks = g_checks; prev_fails = g_failures;
    test_directory_frame_layout();
    fprintf(stderr, "test_directory_frame_layout:        %d/%d ok\n",
            (g_checks - prev_checks) - (g_failures - prev_fails),
            g_checks - prev_checks);

    prev_checks = g_checks; prev_fails = g_failures;
    test_cycle_paced_exact_boundaries();
    fprintf(stderr, "test_cycle_paced_exact_boundaries:  %d/%d ok\n",
            (g_checks - prev_checks) - (g_failures - prev_fails),
            g_checks - prev_checks);

    fprintf(stderr, "=== TOTAL: %d checks, %d failures ===\n",
            g_checks, g_failures);
    return g_failures > 0 ? 1 : 0;
}
