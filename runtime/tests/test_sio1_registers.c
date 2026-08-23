/*
 * test_sio1_registers.c -- SIO1 register FSM, no link peer, no runtime.
 *
 * Links only src/sio1.c + src/psx_link.c: the device is an instance with an
 * injected clock and IRQ callback, so no runtime stubs are needed (contrast
 * test_sio_card_protocol.c's stub wall).
 */
#include <stdio.h>
#include <stdlib.h>

#include "sio1.h"

static int g_fail;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail = 1; \
    } \
} while (0)

static uint32_t g_irq_count;
static uint32_t g_irq_last_detail;
static void irq_cb(void *user, uint32_t detail) {
    (void)user;
    g_irq_count++;
    g_irq_last_detail = detail;
}

/* MMIO shorthand against a fixed test clock. */
static uint64_t now;
static Sio1Device *d;
static uint32_t rd(uint32_t addr, uint32_t w) {
    return sio1_device_read(d, addr, w, now);
}
static void wr(uint32_t addr, uint32_t w, uint32_t v) {
    sio1_device_write(d, addr, w, v, now);
}

static void test_power_on(void) {
    CHECK((rd(0x1F801054, 4) & 0x7FFu) == 0x0005u);
    CHECK(rd(0x1F801058, 2) == 0);          /* MODE */
    CHECK(rd(0x1F80105A, 2) == 0);          /* CTRL */
    CHECK(rd(0x1F80105E, 2) == 0);          /* BAUD */
}

/* Replay the exact WipEout 3 SE libcomb init (func_801621D8). */
static void test_wipeout_init_sequence(void) {
    uint16_t ctrl_rb;
    wr(0x1F80105A, 2, 0x0050);              /* RESET|ACK strobe, first */
    CHECK(rd(0x1F80105A, 2) == 0);          /* strobes read back 0 */
    CHECK(rd(0x1F801058, 2) == 0);
    CHECK(rd(0x1F80105E, 2) == 0);
    CHECK((rd(0x1F801054, 4) & 0x7FFu) == 0x0005u);
    wr(0x1F801058, 2, 0x00CE);              /* MODE: MUL16, 8-N-2 */
    wr(0x1F80105E, 2, 0x00D8);              /* BAUD: 216 */
    ctrl_rb = (uint16_t)rd(0x1F80105A, 2);  /* driver read-back: must be 0 */
    CHECK(ctrl_rb == 0);
    wr(0x1F80105A, 2, ctrl_rb | 0x0010u);   /* ACK strobe */
    wr(0x1F80105A, 2, 0x0005);              /* TXEN|RXEN steady state */
    CHECK(rd(0x1F80105A, 2) == 0x0005u);
    CHECK(sio1_device_bit_cycles(d) == 3456u);
    CHECK(sio1_device_char_cycles(d) == 38016u);
}

static void test_write_masks(void) {
    wr(0x1F80105A, 2, 0xFFFF & ~0x0050u);   /* everything but the strobes */
    CHECK(rd(0x1F80105A, 2) == (0xFFFFu & SIO1_CTRL_STORED_MASK & ~0x0050u));
    CHECK((rd(0x1F80105A, 2) & 0xE000u) == 0);   /* bits 13-15 always 0 */
    wr(0x1F801058, 2, 0xFFFF);
    CHECK(rd(0x1F801058, 2) == 0x00FFu);    /* MODE high byte reads 0 */
    wr(0x1F80105A, 2, 0x0040);              /* RESET */
}

static void test_sticky_and_ack(void) {
    /* Force an overrun via loopback: 9 chars TX'd, none read. */
    PsxLinkEndpoint *lb = psx_link_loopback_create();
    sio1_device_attach(d, lb);
    wr(0x1F80105A, 2, 0x0040);
    wr(0x1F801058, 2, 0x00CE);
    wr(0x1F80105E, 2, 0x00D8);
    wr(0x1F80105A, 2, 0x0005);
    for (int i = 0; i < 9; i++) {
        wr(0x1F801050, 1, 0x40 + (uint32_t)i);
        now += sio1_device_char_cycles(d);
        sio1_device_advance(d, sio1_device_char_cycles(d), now);
    }
    CHECK((rd(0x1F801054, 4) & SIO1_STAT_OVERRUN) != 0);
    /* ACK clears sticky bits... */
    wr(0x1F80105A, 2, 0x0005 | 0x0010);
    CHECK((rd(0x1F801054, 4) & SIO1_STAT_OVERRUN) == 0);
    /* ...and FIFO yields the oldest 8 in order. */
    for (int i = 0; i < 8; i++)
        CHECK(rd(0x1F801050, 1) == (uint32_t)(0x40 + i));
    sio1_device_attach(d, psx_link_null());
    psx_link_loopback_destroy(lb);
    wr(0x1F80105A, 2, 0x0040);
}

static void test_lane_decode(void) {
    /* Loopback with DTR high: DSR (STAT.7) must be visible in a byte read
     * of 0x1F801054, and CTS/IRQ region in a byte read of 0x1F801055. */
    PsxLinkEndpoint *lb = psx_link_loopback_create();
    sio1_device_attach(d, lb);
    wr(0x1F80105A, 2, 0x0040);
    wr(0x1F80105A, 2, SIO1_CTRL_DTR | SIO1_CTRL_RTS);
    CHECK((rd(0x1F801054, 1) & 0x80u) != 0);        /* DSR in byte 0 bit 7 */
    CHECK((rd(0x1F801055, 1) & 0x01u) != 0);        /* CTS = STAT bit 8 */
    /* Byte write to CTRL (0x105A) must NOT touch MODE (0x1058). */
    wr(0x1F801058, 2, 0x00CE);
    wr(0x1F80105A, 1, 0x0005);
    CHECK(rd(0x1F801058, 2) == 0x00CEu);
    CHECK(rd(0x1F80105A, 2) == 0x0005u);
    /* Width-4 write to 0x1058 applies MODE then CTRL. */
    wr(0x1F801058, 4, ((uint32_t)0x0005 << 16) | 0x00CE);
    CHECK(rd(0x1F801058, 2) == 0x00CEu);
    CHECK(rd(0x1F80105A, 2) == 0x0005u);
    /* Width-4 write to 0x105C carries BAUD in the high half. */
    wr(0x1F80105C, 4, (uint32_t)0x00D8 << 16);
    CHECK(rd(0x1F80105E, 2) == 0x00D8u);
    sio1_device_attach(d, psx_link_null());
    psx_link_loopback_destroy(lb);
    wr(0x1F80105A, 2, 0x0040);
}

static void test_baud_timer(void) {
    uint32_t t0, t1, reload;
    wr(0x1F80105A, 2, 0x0040);
    wr(0x1F801058, 2, 0x00CE);
    wr(0x1F80105E, 2, 0x00D8);
    reload = sio1_device_bit_cycles(d) / 2u;
    t0 = rd(0x1F801054, 4) >> 11;
    now += 100;
    t1 = rd(0x1F801054, 4) >> 11;
    CHECK(((t0 + reload - t1) % reload) == 100u % reload);
}

static void test_irq_latch_only_clears_via_ack(void) {
    g_irq_count = 0;
    wr(0x1F80105A, 2, 0x0040);
    wr(0x1F801058, 2, 0x00CE);
    wr(0x1F80105E, 2, 0x00D8);
    /* TX IRQ enabled: starting a shift raises TXRDY1 edge. */
    wr(0x1F80105A, 2, SIO1_CTRL_TXEN | SIO1_CTRL_TX_IRQ);
    wr(0x1F801050, 1, 0xAA);
    CHECK(g_irq_count == 1);
    CHECK(g_irq_last_detail == SIO1_IRQ_DETAIL_TX);
    CHECK((rd(0x1F801054, 4) & SIO1_STAT_IRQ) != 0);
    /* Latch holds: further TX events do not re-fire. */
    now += sio1_device_char_cycles(d);
    sio1_device_advance(d, sio1_device_char_cycles(d), now);
    CHECK(g_irq_count == 1);
    /* Writing CTRL without ACK does not clear the latch. */
    wr(0x1F80105A, 2, SIO1_CTRL_TXEN | SIO1_CTRL_TX_IRQ);
    CHECK((rd(0x1F801054, 4) & SIO1_STAT_IRQ) != 0);
    wr(0x1F80105A, 2, SIO1_CTRL_TXEN | SIO1_CTRL_TX_IRQ | SIO1_CTRL_ACK);
    CHECK((rd(0x1F801054, 4) & SIO1_STAT_IRQ) == 0);
}

int main(void) {
    d = sio1_device_create();
    if (!d) { fprintf(stderr, "alloc failed\n"); return 1; }
    sio1_device_set_irq(d, irq_cb, NULL);
    sio1_device_attach(d, psx_link_null());
    now = 1000000;
    sio1_device_reset(d, now);

    test_power_on();
    test_wipeout_init_sequence();
    test_write_masks();
    test_sticky_and_ack();
    test_lane_decode();
    test_baud_timer();
    test_irq_latch_only_clears_via_ack();

    sio1_device_destroy(d);
    if (g_fail) { fprintf(stderr, "test_sio1_registers: FAILED\n"); return 1; }
    printf("test_sio1_registers: OK\n");
    return 0;
}
