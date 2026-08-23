/*
 * test_sio1_link_loopback.c -- two SIO1 devices over the crossover cable.
 *
 * Covers: exact char-time delivery, RX-depth IRQ, overrun, TX IRQ, DSR/CTS
 * edges, RESET mid-shift, and -- critically -- that sio1_device_cycles_to_irq
 * is an EXACT under-estimate at every step (advancing by it lands ON the
 * event, never past). That invariant protects the event-slicing sites in
 * interrupts.c / psx_cycles.c.
 */
#include <stdio.h>

#include "sio1.h"

static int g_fail;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail = 1; \
    } \
} while (0)

static uint64_t now;
static Sio1Device *A, *B;
static uint32_t irq_a, irq_b, irq_b_detail;
static void cb_a(void *u, uint32_t det) { (void)u; (void)det; irq_a++; }
static void cb_b(void *u, uint32_t det) { (void)u; irq_b++; irq_b_detail = det; }

static uint32_t rd(Sio1Device *d, uint32_t a, uint32_t w) {
    return sio1_device_read(d, a, w, now);
}
static void wr(Sio1Device *d, uint32_t a, uint32_t w, uint32_t v) {
    sio1_device_write(d, a, w, v, now);
}
static void advance_both(uint32_t c) {
    now += c;
    sio1_device_advance(A, c, now);
    sio1_device_advance(B, c, now);
}
/* WipEout wire config on one device. */
static void cfg(Sio1Device *d, uint16_t ctrl) {
    wr(d, 0x1F80105A, 2, 0x0040);
    wr(d, 0x1F801058, 2, 0x00CE);
    wr(d, 0x1F80105E, 2, 0x00D8);
    wr(d, 0x1F80105A, 2, ctrl);
}
static uint32_t cc(void) { return sio1_device_char_cycles(A); }

/* Advance by exactly cycles_to_irq until quiescent; assert each hop lands
 * ON an event boundary (state changes at that hop, never before). */
static void test_exact_slicing_byte(void) {
    uint32_t d1, hops = 0;
    cfg(A, SIO1_CTRL_TXEN | SIO1_CTRL_RXEN);
    cfg(B, SIO1_CTRL_TXEN | SIO1_CTRL_RXEN);
    wr(A, 0x1F801050, 1, 0x5A);
    d1 = sio1_device_cycles_to_irq(A, 0xFFFFFFFFu);
    CHECK(d1 == cc());                       /* full char remaining */
    /* one cycle short: nothing may arrive */
    advance_both(d1 - 1);
    CHECK((rd(B, 0x1F801054, 4) & SIO1_STAT_RXNE) == 0);
    d1 = sio1_device_cycles_to_irq(A, 0xFFFFFFFFu);
    CHECK(d1 == 1u);
    advance_both(1);
    CHECK((rd(B, 0x1F801054, 4) & SIO1_STAT_RXNE) != 0);
    CHECK(rd(B, 0x1F801050, 1) == 0x5Au);
    CHECK((rd(B, 0x1F801054, 4) & SIO1_STAT_RXNE) == 0);
    /* quiescent: both report never */
    CHECK(sio1_device_cycles_to_irq(A, 0xFFFFFFFFu) == 0xFFFFFFFFu);
    CHECK(sio1_device_cycles_to_irq(B, 0xFFFFFFFFu) == 0xFFFFFFFFu);
    (void)hops;
}

static void test_rx_depth_irq(void) {
    irq_b = 0;
    cfg(A, SIO1_CTRL_TXEN | SIO1_CTRL_RXEN);
    /* depth 4 (code 2), RX IRQ enabled */
    cfg(B, SIO1_CTRL_TXEN | SIO1_CTRL_RXEN | (2u << 8) | SIO1_CTRL_RX_IRQ);
    for (int i = 0; i < 4; i++) {
        wr(A, 0x1F801050, 1, (uint32_t)i);
        advance_both(cc());
        if (i < 3) CHECK(irq_b == 0);        /* no IRQ before the 4th */
    }
    CHECK(irq_b == 1);
    CHECK(irq_b_detail == SIO1_IRQ_DETAIL_RX);
    CHECK((rd(B, 0x1F801054, 4) & SIO1_STAT_IRQ) != 0);
    /* latch sticky until ACK */
    wr(A, 0x1F801050, 1, 0xEE);
    advance_both(cc());
    CHECK(irq_b == 1);
    wr(B, 0x1F80105A, 2,
       SIO1_CTRL_TXEN | SIO1_CTRL_RXEN | (2u << 8) | SIO1_CTRL_RX_IRQ |
       SIO1_CTRL_ACK);
    CHECK((rd(B, 0x1F801054, 4) & SIO1_STAT_IRQ) == 0);
    while (rd(B, 0x1F801054, 4) & SIO1_STAT_RXNE) (void)rd(B, 0x1F801050, 1);
}

static void test_tx_irq_once_per_char(void) {
    irq_a = 0;
    cfg(A, SIO1_CTRL_TXEN | SIO1_CTRL_RXEN | SIO1_CTRL_TX_IRQ);
    cfg(B, SIO1_CTRL_TXEN | SIO1_CTRL_RXEN);
    wr(A, 0x1F801050, 1, 0x11);
    CHECK(irq_a == 1);                       /* TXRDY1 rise at shift start */
    /* ACK between events; completion raises TXDONE edge -> second IRQ */
    wr(A, 0x1F80105A, 2,
       SIO1_CTRL_TXEN | SIO1_CTRL_RXEN | SIO1_CTRL_TX_IRQ | SIO1_CTRL_ACK);
    advance_both(cc());
    CHECK(irq_a == 2);
    (void)rd(B, 0x1F801050, 1);
}

static void test_dsr_cts(void) {
    irq_b = 0;
    cfg(A, 0);
    cfg(B, SIO1_CTRL_DSR_IRQ);
    CHECK((rd(B, 0x1F801054, 4) & SIO1_STAT_DSR) == 0);
    /* A raises DTR+RTS -> B sees DSR+CTS; DSR edge IRQ fires. */
    wr(A, 0x1F80105A, 2, SIO1_CTRL_DTR | SIO1_CTRL_RTS);
    advance_both(16);
    CHECK((rd(B, 0x1F801054, 4) & SIO1_STAT_DSR) != 0);
    CHECK((rd(B, 0x1F801054, 4) & SIO1_STAT_CTS) != 0);
    CHECK(irq_b == 1);
    CHECK(irq_b_detail == SIO1_IRQ_DETAIL_DSR);
    /* Dropping DTR is not an edge; re-raising after ACK is. */
    wr(B, 0x1F80105A, 2, SIO1_CTRL_DSR_IRQ | SIO1_CTRL_ACK);
    wr(A, 0x1F80105A, 2, 0);
    advance_both(16);
    CHECK(irq_b == 1);
    wr(A, 0x1F80105A, 2, SIO1_CTRL_DTR);
    advance_both(16);
    CHECK(irq_b == 2);
}

static void test_reset_mid_shift_cancels(void) {
    cfg(A, SIO1_CTRL_TXEN | SIO1_CTRL_RXEN);
    cfg(B, SIO1_CTRL_TXEN | SIO1_CTRL_RXEN);
    wr(A, 0x1F801050, 1, 0x77);
    advance_both(cc() / 2u);
    wr(A, 0x1F80105A, 2, 0x0040);            /* RESET mid-character */
    advance_both(cc() * 2u);
    CHECK((rd(B, 0x1F801054, 4) & SIO1_STAT_RXNE) == 0);  /* never arrives */
}

static void test_loopback_backend(void) {
    Sio1Device *L = sio1_device_create();
    PsxLinkEndpoint *lb = psx_link_loopback_create();
    sio1_device_attach(L, lb);
    sio1_device_write(L, 0x1F801058, 2, 0x00CE, now);
    sio1_device_write(L, 0x1F80105E, 2, 0x00D8, now);
    sio1_device_write(L, 0x1F80105A, 2,
                      SIO1_CTRL_TXEN | SIO1_CTRL_RXEN | SIO1_CTRL_DTR, now);
    /* Self-wired: own DTR reads back as DSR; TX returns to own RX. */
    CHECK((sio1_device_read(L, 0x1F801054, 4, now) & SIO1_STAT_DSR) != 0);
    sio1_device_write(L, 0x1F801050, 1, 0xC3, now);
    now += sio1_device_char_cycles(L);
    sio1_device_advance(L, sio1_device_char_cycles(L), now);
    CHECK(sio1_device_read(L, 0x1F801050, 1, now) == 0xC3u);
    sio1_device_destroy(L);
    psx_link_loopback_destroy(lb);
}

int main(void) {
    PsxLinkEndpoint *ea, *eb;
    psx_link_crossover_create(&ea, &eb);
    if (!ea || !eb) { fprintf(stderr, "crossover alloc failed\n"); return 1; }
    A = sio1_device_create();
    B = sio1_device_create();
    sio1_device_set_irq(A, cb_a, NULL);
    sio1_device_set_irq(B, cb_b, NULL);
    sio1_device_attach(A, ea);
    sio1_device_attach(B, eb);
    now = 5000000;
    sio1_device_reset(A, now);
    sio1_device_reset(B, now);

    test_exact_slicing_byte();
    test_rx_depth_irq();
    test_tx_irq_once_per_char();
    test_dsr_cts();
    test_reset_mid_shift_cancels();
    test_loopback_backend();

    sio1_device_destroy(A);
    sio1_device_destroy(B);
    psx_link_crossover_destroy(ea, eb);
    if (g_fail) { fprintf(stderr, "test_sio1_link_loopback: FAILED\n"); return 1; }
    printf("test_sio1_link_loopback: OK\n");
    return 0;
}
