/*
 * sio.c -- PS1 Serial I/O (SIO0) controller
 *
 * Handles controller (pad) and memory card communication.
 * SIO0 registers at 0x1F801040-0x1F80104E.
 *
 * Pure hardware simulation. No BIOS state, no HLE, no stubs.
 *
 * Ported from v3 with audit:
 *   - Removed psx_runtime.h dependency (unused)
 *   - Removed all fprintf (CLAUDE.md rule #3)
 *   - IRQ delivery via i_stat bit-set (same as timers/dma)
 */

#include "sio.h"
#include "memcard.h"
#include <string.h>

/* I_STAT is owned by memory.c */
extern uint32_t i_stat;

/* IRQ bit for SIO0 */
#define IRQ_SIO0 7

/* SIO registers */
static uint8_t  sio_tx_data;
static uint8_t  sio_rx_data;
static uint16_t sio_stat;
static uint16_t sio_mode;
static uint16_t sio_ctrl;
static uint16_t sio_baud;

/* Pad state: 0=pressed, 1=released (PS1 convention) */
static uint16_t pad_buttons = 0xFFFF; /* all released */

/* Which slots have devices connected */
static uint8_t pad_connected = 0;

/* Pad communication state machine */
typedef enum {
    PAD_IDLE,
    PAD_WAIT_ACCESS,    /* received 0x01 (pad access) */
    PAD_SEND_ID_LO,    /* sending pad ID low byte (0x41 = digital) */
    PAD_SEND_ID_HI,    /* sending pad ID high byte (0x5A) */
    PAD_SEND_BTN_LO,   /* sending button low byte */
    PAD_SEND_BTN_HI,   /* sending button high byte */
} PadState;

static PadState pad_state = PAD_IDLE;
static int selected_slot = 0;

/* Memory card SIO state machine */
typedef enum {
    MC_IDLE,
    MC_CMD,
    MC_ID1,
    MC_ID2,
    MC_ADDR_MSB,
    MC_ADDR_LSB,
    /* Read states */
    MC_READ_ACK1,
    MC_READ_ACK2,
    MC_READ_DATA,
    MC_READ_CHK,
    MC_READ_END,
    /* Write states */
    MC_WRITE_DATA,
    MC_WRITE_CHK,
    MC_WRITE_ACK1,
    MC_WRITE_ACK2,
    MC_WRITE_END,
    /* Get ID states */
    MC_GETID_1,
    MC_GETID_2,
    MC_GETID_3,
    MC_GETID_4,
} McState;

static McState mc_state = MC_IDLE;
static int mc_slot = 0;
static uint8_t mc_cmd = 0;
static uint16_t mc_sector = 0;
static uint8_t mc_sector_msb = 0;
static uint8_t mc_sector_lsb = 0;
static uint8_t mc_data[128];
static int mc_data_idx = 0;
static uint8_t mc_checksum = 0;
static uint8_t mc_flag = 0x08; /* 0x08=new data, 0x00=normal */

typedef enum {
    DEV_NONE,
    DEV_PAD,
    DEV_MEMCARD,
} ActiveDevice;

static ActiveDevice active_device = DEV_NONE;

/* Card probe diagnostic counters */
static int sio_mc_probe_count = 0;  /* times 0x81 written to TX */
static int sio_mc_ack_count = 0;    /* times card sent ACK */
static int sio_mc_cmd_count = 0;    /* times card reached CMD state */
static int sio_tx_writes = 0;       /* ANY write to SIO_TX_DATA */
static int sio_tx_gated = 0;        /* writes gated by missing TX_EN */
static uint16_t sio_last_ctrl_on_tx = 0; /* CTRL at last TX write */

int sio_get_mc_probe_count(void) { return sio_mc_probe_count; }
int sio_get_mc_ack_count(void) { return sio_mc_ack_count; }
int sio_get_mc_cmd_count(void) { return sio_mc_cmd_count; }
int sio_get_tx_writes(void) { return sio_tx_writes; }
int sio_get_tx_gated(void) { return sio_tx_gated; }
uint16_t sio_get_last_ctrl_on_tx(void) { return sio_last_ctrl_on_tx; }

/* Delayed IRQ mechanism.
 *
 * On real PS1, each SIO byte transfer takes BAUD*8 cycles (~1088 for memcard)
 * and the ACK fires ~170 cycles later. The BIOS card detection sequence
 * depends on this timing:
 *   1. Write TX byte
 *   2. Delay loop (~50-100 instructions)
 *   3. Clear JOY_STAT.INTR and I_STAT.IRQ7
 *   4. Check if IRQ7 was re-set -> if yes, device present
 *
 * If we fire IRQ7 instantly on step 1, step 3 clears it, and step 4 sees
 * nothing -> BIOS thinks no device is connected.
 *
 * Fix: process the byte immediately (RX data available) but delay the ACK
 * and IRQ7 by SIO_IRQ_DELAY ticks. */
/* In v4, this counts SIO register accesses (not interpreter steps).
 * The BIOS card detection does: TX write, STAT read, RX read, CTRL
 * write (clear IRQ), then checks I_STAT. We need the IRQ to fire
 * AFTER the CTRL clear. 4 accesses covers the typical sequence. */
#define SIO_IRQ_DELAY 4
static int sio_irq_pending = 0;
static int sio_irq_countdown = 0;

/* SIO status register bits */
#define SIO_STAT_TX_RDY      (1 << 0)
#define SIO_STAT_RX_RDY      (1 << 1)
#define SIO_STAT_TX_EMPTY    (1 << 2)
#define SIO_STAT_ACK         (1 << 7)
#define SIO_STAT_IRQ         (1 << 9)

/* SIO control register bits */
#define SIO_CTRL_TX_EN       (1 << 0)
#define SIO_CTRL_SELECT      (1 << 1)
#define SIO_CTRL_RX_EN       (1 << 2)
#define SIO_CTRL_ACK         (1 << 4)
#define SIO_CTRL_RESET       (1 << 6)
#define SIO_CTRL_RX_IRQ_EN  (1 << 11)
#define SIO_CTRL_ACK_IRQ_EN (1 << 12)
#define SIO_CTRL_SLOT        (1 << 13)

void sio_init(void) {
    sio_tx_data = 0;
    sio_rx_data = 0xFF;
    sio_stat = SIO_STAT_TX_RDY | SIO_STAT_TX_EMPTY;
    sio_mode = 0;
    sio_ctrl = 0;
    sio_baud = 0;
    pad_state = PAD_IDLE;
    pad_buttons = 0xFFFF;
    pad_connected = 0;
    mc_state = MC_IDLE;
    active_device = DEV_NONE;
    sio_irq_pending = 0;
    sio_irq_countdown = 0;
}

void sio_connect_pad(int slot) {
    if (slot >= 0 && slot <= 1)
        pad_connected |= (1 << slot);
}

void sio_set_pad_state(uint16_t buttons) {
    pad_buttons = buttons;
}

uint16_t sio_get_pad_buttons(void) {
    return pad_buttons;
}

static void pad_process_byte(uint8_t tx_byte) {
    switch (pad_state) {
    case PAD_IDLE:
        if (pad_connected & (1 << selected_slot)) {
            pad_state = PAD_WAIT_ACCESS;
            sio_rx_data = 0xFF;
            sio_stat |= SIO_STAT_ACK;
        } else {
            sio_rx_data = 0xFF;
        }
        break;

    case PAD_WAIT_ACCESS:
        if (tx_byte == 0x42) {
            pad_state = PAD_SEND_ID_LO;
            sio_rx_data = 0x41; /* Digital pad type */
            sio_stat |= SIO_STAT_ACK;
        } else {
            pad_state = PAD_IDLE;
            sio_rx_data = 0xFF;
        }
        break;

    case PAD_SEND_ID_LO:
        pad_state = PAD_SEND_ID_HI;
        sio_rx_data = 0x5A;
        sio_stat |= SIO_STAT_ACK;
        break;

    case PAD_SEND_ID_HI:
        pad_state = PAD_SEND_BTN_LO;
        sio_rx_data = (uint8_t)(pad_buttons & 0xFF);
        sio_stat |= SIO_STAT_ACK;
        break;

    case PAD_SEND_BTN_LO:
        pad_state = PAD_IDLE;
        sio_rx_data = (uint8_t)(pad_buttons >> 8);
        /* No ACK after last byte */
        break;

    default:
        pad_state = PAD_IDLE;
        sio_rx_data = 0xFF;
        break;
    }

    sio_stat |= SIO_STAT_RX_RDY;
    sio_stat |= SIO_STAT_TX_RDY | SIO_STAT_TX_EMPTY;
}

static void mc_process_byte(uint8_t tx_byte) {
    switch (mc_state) {
    case MC_IDLE:
        mc_slot = selected_slot;
        if (memcard_is_present(mc_slot)) {
            mc_state = MC_CMD;
            sio_rx_data = 0xFF;
            sio_stat |= SIO_STAT_ACK;
            sio_mc_ack_count++;
        } else {
            sio_rx_data = 0xFF;
        }
        break;

    case MC_CMD:
        mc_cmd = tx_byte;
        sio_mc_cmd_count++;
        if (tx_byte == 0x52 || tx_byte == 0x57 || tx_byte == 0x53) {
            mc_state = MC_ID1;
            sio_rx_data = mc_flag;
            sio_stat |= SIO_STAT_ACK;
        } else {
            mc_state = MC_IDLE;
            sio_rx_data = 0xFF;
        }
        break;

    case MC_ID1:
        mc_state = MC_ID2;
        sio_rx_data = 0x5A;
        sio_stat |= SIO_STAT_ACK;
        break;

    case MC_ID2:
        if (mc_cmd == 0x53) {
            mc_state = MC_GETID_1;
            sio_rx_data = 0x5D;
            sio_stat |= SIO_STAT_ACK;
        } else {
            mc_state = MC_ADDR_MSB;
            sio_rx_data = 0x5D;
            sio_stat |= SIO_STAT_ACK;
        }
        break;

    case MC_GETID_1:
        mc_state = MC_GETID_2;
        sio_rx_data = 0x04;
        sio_stat |= SIO_STAT_ACK;
        break;

    case MC_GETID_2:
        mc_state = MC_GETID_3;
        sio_rx_data = 0x00;
        sio_stat |= SIO_STAT_ACK;
        break;

    case MC_GETID_3:
        mc_state = MC_GETID_4;
        sio_rx_data = 0x00;
        sio_stat |= SIO_STAT_ACK;
        break;

    case MC_GETID_4:
        mc_state = MC_IDLE;
        sio_rx_data = 0x80;
        break;

    case MC_ADDR_MSB:
        mc_sector_msb = tx_byte;
        mc_state = MC_ADDR_LSB;
        sio_rx_data = 0x00;
        sio_stat |= SIO_STAT_ACK;
        break;

    case MC_ADDR_LSB:
        mc_sector_lsb = tx_byte;
        mc_sector = ((uint16_t)mc_sector_msb << 8) | mc_sector_lsb;

        if (mc_cmd == 0x52) {
            if (mc_sector < MEMCARD_SECTORS) {
                memcard_read_sector(mc_slot, mc_sector, mc_data);
            } else {
                memset(mc_data, 0xFF, 128);
            }
            mc_data_idx = 0;
            mc_checksum = mc_sector_msb ^ mc_sector_lsb;
            for (int i = 0; i < 128; i++)
                mc_checksum ^= mc_data[i];
            mc_state = MC_READ_ACK1;
        } else {
            mc_data_idx = 0;
            mc_state = MC_WRITE_DATA;
        }
        sio_rx_data = 0x00;
        sio_stat |= SIO_STAT_ACK;
        break;

    /* ---- READ states ---- */
    case MC_READ_ACK1:
        mc_state = MC_READ_ACK2;
        sio_rx_data = 0x5C;
        sio_stat |= SIO_STAT_ACK;
        break;

    case MC_READ_ACK2:
        mc_state = MC_READ_DATA;
        mc_data_idx = 0;
        sio_rx_data = 0x5D;
        sio_stat |= SIO_STAT_ACK;
        break;

    case MC_READ_DATA:
        sio_rx_data = mc_data[mc_data_idx++];
        if (mc_data_idx >= 128) {
            mc_state = MC_READ_CHK;
        }
        sio_stat |= SIO_STAT_ACK;
        break;

    case MC_READ_CHK:
        mc_state = MC_READ_END;
        sio_rx_data = mc_checksum;
        sio_stat |= SIO_STAT_ACK;
        break;

    case MC_READ_END:
        mc_state = MC_IDLE;
        if (mc_sector < MEMCARD_SECTORS) {
            sio_rx_data = 0x47; /* 'G' = Good */
        } else {
            sio_rx_data = 0xFF;
        }
        break;

    /* ---- WRITE states ---- */
    case MC_WRITE_DATA:
        mc_data[mc_data_idx++] = tx_byte;
        sio_rx_data = 0x00;
        if (mc_data_idx >= 128) {
            mc_state = MC_WRITE_CHK;
        }
        sio_stat |= SIO_STAT_ACK;
        break;

    case MC_WRITE_CHK: {
        uint8_t expected = mc_sector_msb ^ mc_sector_lsb;
        for (int i = 0; i < 128; i++)
            expected ^= mc_data[i];

        mc_state = MC_WRITE_ACK1;
        sio_rx_data = 0x00;
        sio_stat |= SIO_STAT_ACK;

        if (tx_byte == expected && mc_sector < MEMCARD_SECTORS) {
            memcard_write_sector(mc_slot, mc_sector, mc_data);
            memcard_flush(mc_slot);
            mc_checksum = 0x47; /* Good */
        } else if (mc_sector >= MEMCARD_SECTORS) {
            mc_checksum = 0xFF; /* Bad sector */
        } else {
            mc_checksum = 0x4E; /* 'N' = bad checksum */
        }
        break;
    }

    case MC_WRITE_ACK1:
        mc_state = MC_WRITE_ACK2;
        sio_rx_data = 0x5C;
        sio_stat |= SIO_STAT_ACK;
        break;

    case MC_WRITE_ACK2:
        mc_state = MC_WRITE_END;
        sio_rx_data = 0x5D;
        sio_stat |= SIO_STAT_ACK;
        break;

    case MC_WRITE_END:
        mc_state = MC_IDLE;
        sio_rx_data = mc_checksum;
        mc_flag = 0x00; /* Clear "new data" flag after first write */
        break;

    default:
        mc_state = MC_IDLE;
        sio_rx_data = 0xFF;
        break;
    }

    sio_stat |= SIO_STAT_RX_RDY;
    sio_stat |= SIO_STAT_TX_RDY | SIO_STAT_TX_EMPTY;
}

static void sio_process_byte(uint8_t tx_byte) {
    if (active_device == DEV_NONE) {
        selected_slot = (sio_ctrl & SIO_CTRL_SLOT) ? 1 : 0;
        if (tx_byte == 0x01) {
            active_device = DEV_PAD;
            pad_process_byte(tx_byte);
        } else if (tx_byte == 0x81) {
            active_device = DEV_MEMCARD;
            sio_mc_probe_count++;
            mc_process_byte(tx_byte);
        } else {
            sio_rx_data = 0xFF;
            sio_stat |= SIO_STAT_TX_RDY | SIO_STAT_TX_EMPTY;
        }
    } else if (active_device == DEV_PAD) {
        pad_process_byte(tx_byte);
    } else if (active_device == DEV_MEMCARD) {
        mc_process_byte(tx_byte);
    }
}

uint32_t sio_read(uint32_t addr) {
    /* Advance delayed IRQ on every SIO register access.
     * In v4, recompiled functions run as native C without per-instruction
     * stepping. sio_tick() from the dispatch loop won't advance during
     * a tight BIOS polling loop within a single function. Advancing on
     * register access ensures the IRQ fires in time for the BIOS's
     * "clear, delay, check" card detection sequence. */
    sio_tick();

    switch (addr) {
    case 0x1F801040: /* SIO_RX_DATA */
        sio_stat &= ~SIO_STAT_RX_RDY;
        return sio_rx_data;

    case 0x1F801044: /* SIO_STAT */
        return sio_stat;

    case 0x1F801048: /* SIO_MODE */
        return sio_mode;

    case 0x1F80104A: /* SIO_CTRL */
        return sio_ctrl;

    case 0x1F80104E: /* SIO_BAUD */
        return sio_baud;

    default:
        return 0;
    }
}

void sio_write(uint32_t addr, uint32_t value) {
    /* Advance delayed IRQ AFTER write processing (not before).
     * Ticking before a CTRL ACK write would fire the pending IRQ
     * right before the CTRL clears it — wrong order. */

    switch (addr) {
    case 0x1F801040: /* SIO_TX_DATA */
        sio_tx_data = (uint8_t)value;
        sio_tx_writes++;
        sio_last_ctrl_on_tx = sio_ctrl;
        if (!(sio_ctrl & SIO_CTRL_TX_EN)) sio_tx_gated++;
        if (sio_ctrl & SIO_CTRL_TX_EN) {
            sio_process_byte(sio_tx_data);
            if ((sio_stat & SIO_STAT_ACK) && (sio_ctrl & SIO_CTRL_ACK_IRQ_EN)) {
                sio_stat &= ~SIO_STAT_ACK;
                sio_irq_pending = 1;
                sio_irq_countdown = SIO_IRQ_DELAY;
            }
        }
        break;

    case 0x1F801048: /* SIO_MODE */
        sio_mode = (uint16_t)value;
        break;

    case 0x1F80104A: /* SIO_CTRL */
        sio_ctrl = (uint16_t)value;
        if (value & SIO_CTRL_ACK) {
            sio_stat &= ~SIO_STAT_IRQ;
            sio_stat &= ~SIO_STAT_ACK;
        }
        if (value & SIO_CTRL_RESET) {
            sio_stat = SIO_STAT_TX_RDY | SIO_STAT_TX_EMPTY;
            sio_mode = 0;
            sio_ctrl = 0;
            sio_baud = 0;
            pad_state = PAD_IDLE;
            mc_state = MC_IDLE;
            active_device = DEV_NONE;
            sio_irq_pending = 0;
            sio_irq_countdown = 0;
        }
        if (!(value & SIO_CTRL_SELECT)) {
            pad_state = PAD_IDLE;
            mc_state = MC_IDLE;
            active_device = DEV_NONE;
        }
        break;

    case 0x1F80104E: /* SIO_BAUD */
        sio_baud = (uint16_t)value;
        break;

    default:
        break;
    }

    /* Advance delayed IRQ after write processing (not before).
     * This ensures CTRL ACK writes clear the old IRQ before the
     * pending one fires. */
    sio_tick();
}

void sio_tick(void) {
    if (sio_irq_pending && sio_irq_countdown > 0) {
        sio_irq_countdown--;
        if (sio_irq_countdown == 0) {
            sio_irq_pending = 0;
            sio_stat |= SIO_STAT_ACK;
            sio_stat |= SIO_STAT_IRQ;
            i_stat |= (1 << IRQ_SIO0);
        }
    }
}
