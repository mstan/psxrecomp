/*
 * test_sio_spu_istat_ownership.c — cross-device I_STAT ownership.
 *
 * A memory-card ACK IRQ is an SIO0 event. Delivering it must set I_STAT
 * bit 7 and must not touch any other device's pending source. The pinned
 * regression: SPU (I_STAT bit 9) is pending and unacknowledged while the
 * guest runs a card transaction; every card ACK must leave bit 9 set until
 * the guest performs the SPU/INTC acknowledgement itself.
 *
 * Run: ctest -R sio_spu_istat_ownership_test
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
uint32_t i_mask = 0;
uint32_t g_debug_current_func_addr = 0;
uint32_t g_debug_last_store_pc = 0;
int psx_get_in_exception(void) { return 0; }
uint8_t psx_read_byte(uint32_t addr) { (void)addr; return 0; }
uint32_t psx_read_word(uint32_t addr) { (void)addr; return 0; }
/* sio.c gates card-transfer deferral on `psx_get_cycle_count() < deadline`
 * (s_card_ct_defer_until_cyc). A constant clock would defer forever and stall
 * the state machine, so the stub advances monotonically. */
uint64_t psx_get_cycle_count(void) {
    static uint64_t t;
    return t += 64;
}
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

#define EXPECT_TRUE(label, cond)                                                \
    do {                                                                        \
        g_checks++;                                                             \
        if (!(cond)) {                                                          \
            g_failures++;                                                       \
            fprintf(stderr, "FAIL  %s:%d  %s\n", __FILE__, __LINE__, label);    \
        }                                                                       \
    } while (0)

/* ---- SIO MMIO offsets ---- */
#define SIO_TX_DATA  0x1F801040
#define SIO_RX_DATA  0x1F801040
#define SIO_CTRL     0x1F80104A

#define CTRL_TX_EN          (1 << 0)
#define CTRL_SELECT         (1 << 1)
#define CTRL_ACK            (1 << 4)
#define CTRL_ACK_IRQ_EN     (1 << 12)

#define ISTAT_SIO0          (1u << 7)
#define ISTAT_SPU           (1u << 9)

/* Exchange one card-protocol byte: TX, walk the shifter until the ACK IRQ
 * lands, read RX, acknowledge the SIO side only. */
static uint8_t card_xchg(uint8_t tx) {
    uint16_t ctrl = CTRL_TX_EN | CTRL_SELECT | CTRL_ACK_IRQ_EN;
    sio_write(SIO_CTRL, ctrl);
    sio_write(SIO_TX_DATA, tx);
    sio_tick(2000);
    uint8_t rx = (uint8_t)sio_read(SIO_RX_DATA);
    sio_write(SIO_CTRL, ctrl | CTRL_ACK);
    return rx;
}

int main(void) {
    /* Synthetic formatted card in slot 0 so the card path answers with real
     * ACKs rather than the no-card fallthrough. */
    uint8_t fake_card[MEMCARD_SIZE];
    memset(fake_card, 0xFF, sizeof(fake_card));
    fake_card[0] = 'M';
    fake_card[1] = 'C';

    FILE* f = fopen("test_dir/card1.mcd", "wb");
    if (!f) {
        system("mkdir test_dir 2>/dev/null || mkdir -p test_dir");
        f = fopen("test_dir/card1.mcd", "wb");
    }
    assert(f);
    fwrite(fake_card, 1, MEMCARD_SIZE, f);
    fclose(f);

    sio_init();
    memcard_init("test_dir");

    /* SPU interrupt pending and unacknowledged for the whole transaction. */
    i_stat = ISTAT_SPU;

    /* Card READ preamble — each byte delivers a card ACK IRQ. */
    static const uint8_t preamble[] = { 0x81, 0x52, 0x00, 0x00 };
    int sio_irqs_seen = 0;
    for (size_t i = 0; i < sizeof(preamble); i++) {
        card_xchg(preamble[i]);
        if (i_stat & ISTAT_SIO0) {
            sio_irqs_seen++;
            /* Guest INTC acknowledgement of the SIO source ONLY. */
            i_stat &= ~ISTAT_SIO0;
        }
        EXPECT_TRUE("card ACK preserves pending SPU (I_STAT.9)",
                    (i_stat & ISTAT_SPU) != 0);
    }

    /* The ACK IRQs themselves must still be delivered. */
    EXPECT_TRUE("card ACK raises I_STAT.7 at least once", sio_irqs_seen > 0);

    /* Guest acknowledges SPU last — nothing may have consumed it earlier. */
    EXPECT_TRUE("SPU still pending at guest acknowledgement time",
                (i_stat & ISTAT_SPU) != 0);
    i_stat &= ~ISTAT_SPU;
    EXPECT_TRUE("SPU acknowledgement sticks", (i_stat & ISTAT_SPU) == 0);

    if (g_failures) {
        fprintf(stderr, "FAILED (%d of %d checks)\n", g_failures, g_checks);
        return 1;
    }
    printf("ALL PASS (%d checks)\n", g_checks);
    return 0;
}
