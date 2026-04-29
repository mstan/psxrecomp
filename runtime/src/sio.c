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
#include "debug_server.h"
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
    MC_READ_MSB_ECHO,    /* rx = sector_msb (cmd ack 1 echo per no$psx) */
    MC_READ_LSB_ECHO,    /* rx = sector_lsb (cmd ack 2 echo per no$psx) */
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

/* Per-slot card state.  On real hardware each card controller is a
 * separate physical device that retains its protocol state independently.
 * When the BIOS alternates between slot 0 and slot 1 (by changing the
 * SLOT bit in SIO_CTRL), the in-flight card on one slot must not lose
 * its state because the other slot is being probed. */
typedef struct {
    McState  state;
    uint8_t  cmd;
    uint16_t sector;
    uint8_t  sector_msb;
    uint8_t  sector_lsb;
    uint8_t  data[128];
    int      data_idx;
    uint8_t  checksum;
    uint8_t  flag;  /* 0x08=new data, 0x00=normal */
} McSlotState;

static McSlotState mc_slots[2] = {
    { MC_IDLE, 0, 0, 0, 0, {0}, 0, 0, 0x08 },
    { MC_IDLE, 0, 0, 0, 0, {0}, 0, 0, 0x08 },
};

/* Active slot's state — copied from mc_slots[selected_slot] before each
 * byte exchange and copied back after.  The mc_process_byte function
 * operates on these "working" variables for simplicity. */
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

/* Save working mc_ vars back to the slot state for the given slot. */
static void mc_save_slot(int slot) {
    if (slot < 0 || slot > 1) return;
    McSlotState *s = &mc_slots[slot];
    s->state      = mc_state;
    s->cmd        = mc_cmd;
    s->sector     = mc_sector;
    s->sector_msb = mc_sector_msb;
    s->sector_lsb = mc_sector_lsb;
    memcpy(s->data, mc_data, sizeof(mc_data));
    s->data_idx   = mc_data_idx;
    s->checksum   = mc_checksum;
    s->flag       = mc_flag;
}

/* Load slot state into working mc_ vars. */
static void mc_load_slot(int slot) {
    if (slot < 0 || slot > 1) return;
    const McSlotState *s = &mc_slots[slot];
    mc_state      = s->state;
    mc_cmd        = s->cmd;
    mc_sector     = s->sector;
    mc_sector_msb = s->sector_msb;
    mc_sector_lsb = s->sector_lsb;
    memcpy(mc_data, s->data, sizeof(mc_data));
    mc_data_idx   = s->data_idx;
    mc_checksum   = s->checksum;
    mc_flag       = s->flag;
}

/* Card probe diagnostic counters */
static int sio_mc_probe_count = 0;  /* times 0x81 written to TX */
static int sio_mc_ack_count = 0;    /* times card sent ACK */
static int sio_mc_cmd_count = 0;    /* times card reached CMD state */
static int sio_mc_read_count = 0;   /* times 0x52 (read) cmd received */
static int sio_mc_read_done = 0;    /* times read protocol completed */
static uint32_t sio_mc_last_caller = 0; /* func that last sent 0x81 */
static int sio_mc_abort_count = 0;  /* times mc_state reset from non-IDLE */
static int sio_mc_abort_state = 0;  /* mc_state at last abort */
static uint16_t sio_mc_abort_ctrl = 0; /* CTRL value that caused last abort */
static int sio_mc_max_state = 0;    /* highest mc_state reached */
static int sio_tx_writes = 0;       /* ANY write to SIO_TX_DATA */
static int sio_tx_gated = 0;        /* writes gated by missing TX_EN */
static uint16_t sio_last_ctrl_on_tx = 0; /* CTRL at last TX write */

/* ---- SIO byte-level trace ring buffer ---- */
static SioTraceEntry sio_trace_buf[SIO_TRACE_CAP];
static int sio_trace_idx = 0;       /* next write position */
static uint32_t sio_trace_seq = 0;  /* monotonic sequence number */

uint32_t sio_get_trace(const SioTraceEntry **buf_out, int *write_idx_out) {
    if (buf_out) *buf_out = sio_trace_buf;
    if (write_idx_out) *write_idx_out = sio_trace_idx;
    return sio_trace_seq;
}

uint32_t sio_get_seq(void) {
    return sio_trace_seq;
}

int sio_card_protocol_active(void) {
    /* Active if either slot has an in-flight protocol, or the working
     * mc_state is non-idle. */
    if (mc_state != MC_IDLE) return 1;
    if (mc_slots[0].state != MC_IDLE) return 1;
    if (mc_slots[1].state != MC_IDLE) return 1;
    return 0;
}

/* Forward decl: defined below sio_get_freeze_diag. */
static int sio_card_burst_drain(int max_iters);
extern int psx_get_in_exception(void);
extern uint8_t psx_read_byte(uint32_t addr);


/* ---- Card transaction ring buffer ---- */
static SioTxnEntry sio_txn_buf[SIO_TXN_CAP];
static int       sio_txn_idx = 0;        /* next-write slot */
static uint32_t  sio_txn_seq = 0;        /* monotonic id of next-to-close */
static int       sio_txn_open = 0;       /* 1 when a txn is in progress */
static SioTxnEntry sio_txn_cur;          /* in-progress txn, flushed on close */

uint32_t sio_get_card_txns(const SioTxnEntry **buf_out, int *write_idx_out, int *open_out) {
    if (buf_out) *buf_out = sio_txn_buf;
    if (write_idx_out) *write_idx_out = sio_txn_idx;
    if (open_out) *open_out = sio_txn_open;
    return sio_txn_seq;
}

const SioTxnEntry *sio_get_card_txn_live(void) {
    return sio_txn_open ? &sio_txn_cur : NULL;
}

/* Open a new txn. Caller must have ensured none is currently open. */
static void txn_open(uint8_t slot, uint32_t start_byte_seq, uint32_t func) {
    memset(&sio_txn_cur, 0, sizeof(sio_txn_cur));
    sio_txn_cur.txn_seq         = sio_txn_seq;
    sio_txn_cur.start_byte_seq  = start_byte_seq;
    sio_txn_cur.start_func      = func;
    sio_txn_cur.slot            = slot;
    sio_txn_cur.sector          = 0xFFFF;
    sio_txn_cur.terminal_state  = MC_IDLE;
    sio_txn_cur.end_reason      = SIO_TXN_END_OPEN;
    sio_txn_open = 1;
}

/* Append the just-processed byte to the current txn. */
static void txn_record_byte(uint8_t tx, uint8_t rx,
                            uint8_t cmd_after, uint16_t sector_after,
                            int got_ack, uint32_t byte_seq) {
    if (!sio_txn_open) return;
    if (sio_txn_cur.byte_count < SIO_TXN_MAX_BYTES) {
        sio_txn_cur.tx[sio_txn_cur.byte_count] = tx;
        sio_txn_cur.rx[sio_txn_cur.byte_count] = rx;
    }
    sio_txn_cur.byte_count++;
    sio_txn_cur.end_byte_seq = byte_seq;
    if (cmd_after && !sio_txn_cur.cmd) sio_txn_cur.cmd = cmd_after;
    if (sector_after != 0xFFFF) sio_txn_cur.sector = sector_after;
    if (got_ack) sio_txn_cur.ack_count++;
}

/* Close the current txn into the ring. Safe to call when no txn open. */
static void txn_close(uint8_t end_reason, uint8_t terminal_state, uint32_t func) {
    if (!sio_txn_open) return;
    sio_txn_cur.end_reason     = end_reason;
    sio_txn_cur.terminal_state = terminal_state;
    sio_txn_cur.end_func       = func;
    sio_txn_buf[sio_txn_idx]   = sio_txn_cur;
    sio_txn_idx = (sio_txn_idx + 1) % SIO_TXN_CAP;
    sio_txn_seq++;
    sio_txn_open = 0;
}

/* ---- SIO IRQ #7 delivery ring ---- */
static SioIrqEntry sio_irq_buf[SIO_IRQ_RING_CAP];
static int       sio_irq_idx = 0;
static uint32_t  sio_irq_seq = 0;

/* Pending-IRQ context — captured when the countdown is armed, used when it fires. */
static uint8_t   sio_irq_pending_source     = SIO_IRQ_SRC_UNKNOWN;
static uint8_t   sio_irq_pending_slot       = 0;
static uint8_t   sio_irq_pending_delay      = 0;
static uint8_t   sio_irq_pending_mc_state   = 0;
static uint32_t  sio_irq_pending_byte_seq   = 0;

uint32_t sio_get_irq_ring(const SioIrqEntry **buf_out, int *write_idx_out) {
    if (buf_out) *buf_out = sio_irq_buf;
    if (write_idx_out) *write_idx_out = sio_irq_idx;
    return sio_irq_seq;
}

/* ---- Card IRQ-arm audit -----------------------------------------------
 * Per-call counters that record what happened at the IRQ-arm decision
 * point in sio_write SIO_TX_DATA, partitioned by active_device==DEV_MEMCARD
 * (card path) vs not (pad/none). For each card-path call we further
 * record:
 *   - "tx_card": total card TX writes that reached this point
 *   - "armed_card": cases where (ACK && ACK_IRQ_EN) — countdown was set
 *   - "no_ack": cases where SIO_STAT_ACK was clear (state machine didn't ACK)
 *   - "no_ackirqen": cases where ACK was set but ACK_IRQ_EN bit was clear
 *   - "ctrl_last": last ctrl seen at decision time (for sanity)
 *   - "stat_pre_last", "stat_post_last": last seen sio_stat
 *   - "countdown_after_last": value of sio_irq_countdown after the if-block
 * Same fields tracked for pad path so we can compare. Always-on; cheap. */
typedef struct {
    uint32_t tx_total;
    uint32_t armed;
    uint32_t no_ack;
    uint32_t no_ackirqen;
    uint16_t ctrl_last;
    uint16_t stat_pre_last;
    uint16_t stat_post_last;
    int32_t  countdown_after_last;
} CardArmAudit;

static CardArmAudit s_card_arm_audit_card;
static CardArmAudit s_card_arm_audit_pad;
static CardArmAudit s_card_arm_audit_none;

void sio_card_arm_audit_record(int dev, uint16_t ctrl_pre,
                               uint16_t stat_pre, uint16_t stat_post,
                               int armed, int countdown_after) {
    CardArmAudit *a = (dev == DEV_MEMCARD) ? &s_card_arm_audit_card
                    : (dev == DEV_PAD)     ? &s_card_arm_audit_pad
                    :                        &s_card_arm_audit_none;
    a->tx_total++;
    if (armed) {
        a->armed++;
    } else {
        /* SIO_STAT_ACK = (1<<7) = 0x0080;
         * SIO_CTRL_ACK_IRQ_EN = (1<<12) = 0x1000.
         * Constants are #define'd later in this file; re-state them here
         * to avoid forward-decl ordering issues. */
        if (!(stat_post & 0x0080u))      a->no_ack++;
        else if (!(ctrl_pre & 0x1000u))  a->no_ackirqen++;
    }
    a->ctrl_last            = ctrl_pre;
    a->stat_pre_last        = stat_pre;
    a->stat_post_last       = stat_post;
    a->countdown_after_last = countdown_after;
}

void sio_get_card_arm_audit(uint32_t out[3][7]) {
    /* row 0 = card, row 1 = pad, row 2 = none.
     * cols: tx_total, armed, no_ack, no_ackirqen, ctrl_last, stat_pre, stat_post */
    const CardArmAudit *src[3] = {
        &s_card_arm_audit_card, &s_card_arm_audit_pad, &s_card_arm_audit_none };
    for (int i = 0; i < 3; i++) {
        out[i][0] = src[i]->tx_total;
        out[i][1] = src[i]->armed;
        out[i][2] = src[i]->no_ack;
        out[i][3] = src[i]->no_ackirqen;
        out[i][4] = (uint32_t)src[i]->ctrl_last;
        out[i][5] = (uint32_t)src[i]->stat_pre_last;
        out[i][6] = (uint32_t)src[i]->stat_post_last;
    }
}

int sio_get_card_arm_countdown_after(void) {
    return s_card_arm_audit_card.countdown_after_last;
}

int sio_get_mc_probe_count(void) { return sio_mc_probe_count; }
int sio_get_mc_ack_count(void) { return sio_mc_ack_count; }
int sio_get_mc_cmd_count(void) { return sio_mc_cmd_count; }
int sio_get_mc_read_count(void) { return sio_mc_read_count; }
int sio_get_mc_read_done(void) { return sio_mc_read_done; }
uint32_t sio_get_mc_last_caller(void) { return sio_mc_last_caller; }
int sio_get_mc_abort_count(void) { return sio_mc_abort_count; }
int sio_get_mc_abort_state(void) { return sio_mc_abort_state; }
uint16_t sio_get_mc_abort_ctrl(void) { return sio_mc_abort_ctrl; }
int sio_get_mc_max_state(void) { return sio_mc_max_state; }
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
/* IRQ delay for pad detection — short, just enough for the BIOS
 * "write-clear-check" card detection sequence. */
#define SIO_IRQ_DELAY_PAD 4

/* IRQ delay for active card transfers — moderate value to keep TX_RDY
 * cleared briefly during a byte transfer.  Per-slot state already
 * prevents pad bytes from corrupting card state, so we don't need a
 * long delay — we just need the IRQ to fire SOON so the SIO chain
 * walker can dispatch the next card byte before the next timer tick
 * clears the protocol counter. */
#define SIO_IRQ_DELAY_CARD 8

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
    for (int i = 0; i < 2; i++) {
        mc_slots[i].state = MC_IDLE;
        mc_slots[i].cmd = 0;
        mc_slots[i].sector = 0;
        mc_slots[i].sector_msb = 0;
        mc_slots[i].sector_lsb = 0;
        memset(mc_slots[i].data, 0, sizeof(mc_slots[i].data));
        mc_slots[i].data_idx = 0;
        mc_slots[i].checksum = 0;
        mc_slots[i].flag = 0x08;
    }
    active_device = DEV_NONE;
    sio_irq_pending = 0;
    sio_irq_countdown = 0;
    sio_txn_open = 0;
    /* Note: sio_txn_buf, sio_txn_idx, sio_txn_seq deliberately persist
     * across sio_init so post-reset diagnostics can still inspect prior
     * transactions. Boot path zero-inits them via BSS. */
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
    if ((int)mc_state > sio_mc_max_state) sio_mc_max_state = (int)mc_state;
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
            if (tx_byte == 0x52) sio_mc_read_count++;
            mc_state = MC_ID1;
            sio_rx_data = mc_flag;
            sio_stat |= SIO_STAT_ACK;
            /* no$psx: FLAG byte is 0x08 only after newly-inserted/changed-battery
             * card; cleared on first read or write. Without this clear, the BIOS
             * sees 0x08 forever, treats every read as a fresh-card probe, and
             * resets the chain counter (v0=-1 + 0x7520=1 path in BFC152E0).
             * Beetle's card sim returns 0x00 in steady-state — match that. */
            mc_flag = 0x00;
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
        mc_state = MC_READ_MSB_ECHO;
        sio_rx_data = 0x5D;
        sio_stat |= SIO_STAT_ACK;
        break;

    case MC_READ_MSB_ECHO:
        /* Per no$psx: card echoes the requested sector address back to host
         * as a confirm-the-address handshake, BEFORE sending the data bytes. */
        mc_state = MC_READ_LSB_ECHO;
        sio_rx_data = mc_sector_msb;
        sio_stat |= SIO_STAT_ACK;
        break;

    case MC_READ_LSB_ECHO:
        mc_state = MC_READ_DATA;
        mc_data_idx = 0;
        sio_rx_data = mc_sector_lsb;
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
        sio_mc_read_done++;
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
    /* ---- Trace: capture pre-state ---- */
    extern uint32_t g_debug_current_func_addr;
    uint8_t trace_mc_pre = (uint8_t)mc_state;
    uint8_t trace_dev_pre = (uint8_t)active_device;
    uint8_t trace_irq_cd = (uint8_t)(sio_irq_countdown > 255 ? 255 : sio_irq_countdown);
    int trace_abort_before = sio_mc_abort_count;
    int trace_ack_before   = sio_mc_ack_count;

    /* ---- Card transaction tracking ----
     * Decide whether this byte (a) closes a prior txn before processing,
     * (b) opens a new txn before processing, (c) belongs to an existing
     * txn, or (d) is unrelated to the card protocol. The actual record
     * happens AFTER mc_process_byte runs so post-state is accurate. */
    int txn_was_card_byte = 0;
    uint8_t txn_pre_state  = (uint8_t)mc_state;

    if (active_device == DEV_NONE) {
        selected_slot = (sio_ctrl & SIO_CTRL_SLOT) ? 1 : 0;

        if (tx_byte == 0x01) {
            /* Pad select.  Save any in-flight card state back to its slot
             * so it survives pad polling.  Don't touch mc_state — we need
             * it per-slot, and mc_load_slot will restore it when the card
             * slot is selected again. */
            if (mc_state != MC_IDLE) {
                mc_save_slot(mc_slot);
                mc_state = MC_IDLE; /* working vars idle while pad talks */
            }
            active_device = DEV_PAD;
            pad_process_byte(tx_byte);
        } else if (tx_byte == 0x81) {
            /* Card select.  Load this slot's state.  If the slot already
             * has an in-flight protocol, the 0x81 restarts it (abort). */
            mc_load_slot(selected_slot);
            if (mc_state != MC_IDLE) {
                sio_mc_abort_count++;
                sio_mc_abort_state = (int)mc_state;
                sio_mc_abort_ctrl = sio_ctrl;
                /* Old txn (still open) gets force-closed before the new
                 * 0x81 starts its own. */
                txn_close(SIO_TXN_END_ABORT_RESELECT, mc_state, g_debug_current_func_addr);
                mc_state = MC_IDLE;
            }
            active_device = DEV_MEMCARD;
            mc_slot = selected_slot;
            sio_mc_probe_count++;
            sio_mc_last_caller = g_debug_current_func_addr;
            if (!sio_txn_open) {
                txn_open((uint8_t)selected_slot, sio_trace_seq, g_debug_current_func_addr);
            }
            txn_pre_state = (uint8_t)mc_state;
            txn_was_card_byte = 1;
            mc_process_byte(tx_byte);
        } else {
            /* Non-select byte with DEV_NONE — only resume a card protocol
             * if the CTRL slot matches the slot that started the card
             * transaction.  Otherwise this is stray data (e.g. pad bytes
             * that leaked through). */
            mc_load_slot(selected_slot);
            if (mc_state != MC_IDLE && selected_slot == mc_slot) {
                active_device = DEV_MEMCARD;
                /* Continuation of an in-flight card txn across deselect/
                 * reselect. Re-open ring entry if we don't already have
                 * one (rare: pad polling closed it via mc_save_slot). */
                if (!sio_txn_open) {
                    txn_open((uint8_t)mc_slot, sio_trace_seq, g_debug_current_func_addr);
                }
                txn_pre_state = (uint8_t)mc_state;
                txn_was_card_byte = 1;
                mc_process_byte(tx_byte);
            } else {
                if (mc_state != MC_IDLE) {
                    /* Slot-mismatch reset: the slot's saved card state is
                     * abandoned because the BIOS aimed at a different slot
                     * with a non-select byte. Close the txn as ABORT_SLOT. */
                    txn_close(SIO_TXN_END_ABORT_SLOT, mc_state, g_debug_current_func_addr);
                }
                mc_state = MC_IDLE;
                sio_rx_data = 0xFF;
                sio_stat |= SIO_STAT_TX_RDY | SIO_STAT_TX_EMPTY;
            }
        }
    } else if (active_device == DEV_PAD) {
        pad_process_byte(tx_byte);
    } else if (active_device == DEV_MEMCARD) {
        if (mc_state == MC_IDLE) {
            /* Card protocol finished or was aborted.  Only start a new
             * card transaction if this is actually 0x81 (card select).
             * Any other byte means the BIOS moved on to a different
             * device (pad polling sends 0x01 here). */
            if (tx_byte == 0x81) {
                /* New txn after a previous one closed naturally. */
                if (!sio_txn_open) {
                    txn_open((uint8_t)mc_slot, sio_trace_seq, g_debug_current_func_addr);
                }
                txn_pre_state = (uint8_t)mc_state;
                txn_was_card_byte = 1;
                mc_process_byte(tx_byte);
            } else {
                active_device = DEV_NONE;
                selected_slot = (sio_ctrl & SIO_CTRL_SLOT) ? 1 : 0;
                if (tx_byte == 0x01) {
                    active_device = DEV_PAD;
                    pad_process_byte(tx_byte);
                } else {
                    sio_rx_data = 0xFF;
                    sio_stat |= SIO_STAT_TX_RDY | SIO_STAT_TX_EMPTY;
                }
            }
        } else {
            txn_pre_state = (uint8_t)mc_state;
            txn_was_card_byte = 1;
            mc_process_byte(tx_byte);
        }
    }

    /* ---- Card transaction tracking: record + maybe-close ---- */
    if (txn_was_card_byte && sio_txn_open) {
        int got_ack = (sio_mc_ack_count > trace_ack_before) ? 1 : 0;
        uint16_t sector_now = (mc_state >= MC_READ_ACK1 || mc_state == MC_WRITE_DATA
                               || mc_state >= MC_WRITE_ACK1)
                              ? mc_sector : 0xFFFF;
        txn_record_byte(tx_byte, sio_rx_data, mc_cmd, sector_now,
                        got_ack, sio_trace_seq);

        /* Natural close: mc_state went IDLE this byte from non-IDLE. */
        if (mc_state == MC_IDLE && txn_pre_state != MC_IDLE) {
            uint8_t reason;
            switch (txn_pre_state) {
            case MC_READ_END:
            case MC_WRITE_END:
            case MC_GETID_4:
                reason = SIO_TXN_END_SUCCESS;
                break;
            case MC_CMD:
                /* Bad cmd byte: mc_process_byte resets to IDLE. */
                reason = SIO_TXN_END_ABORT_BAD_CMD;
                break;
            default:
                reason = SIO_TXN_END_ABORT_OTHER;
                break;
            }
            txn_close(reason, txn_pre_state, g_debug_current_func_addr);
        }
    }

    /* ---- Trace: capture post-state and record entry ---- */
    {
        SioTraceEntry *e = &sio_trace_buf[sio_trace_idx];
        e->seq           = sio_trace_seq;
        e->tx            = tx_byte;
        e->rx            = sio_rx_data;
        e->mc_state_pre  = trace_mc_pre;
        e->mc_state_post = (uint8_t)mc_state;
        e->dev_pre       = trace_dev_pre;
        e->dev_post      = (uint8_t)active_device;
        e->ctrl          = sio_ctrl;
        e->func_addr     = g_debug_current_func_addr;
        { extern uint32_t memory_get_sr(void);
          e->cop0_sr      = memory_get_sr(); }
        e->was_abort     = (sio_mc_abort_count > trace_abort_before) ? 1 : 0;
        e->irq_countdown = trace_irq_cd;
        { extern int psx_get_in_exception(void);
          e->in_exception = (uint8_t)psx_get_in_exception(); }
        { extern uint8_t psx_read_byte(uint32_t addr);
          /* Read card counter 0x7514 — low byte only for trace */
          e->counter_7514 = psx_read_byte(0x7514); }
        e->slot0_state = (uint8_t)mc_slots[0].state;
        e->slot1_state = (uint8_t)mc_slots[1].state;
        sio_trace_idx = (sio_trace_idx + 1) % SIO_TRACE_CAP;
        sio_trace_seq++;
    }
}

uint32_t sio_read(uint32_t addr) {
    /* Advance delayed IRQ on every SIO register access.
     * In v4, recompiled functions run as native C without per-instruction
     * stepping. sio_tick() from the dispatch loop won't advance during
     * a tight BIOS polling loop within a single function. Advancing on
     * register access ensures the IRQ fires in time for the BIOS's
     * "clear, delay, check" card detection sequence. */
    sio_tick(0);

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

    /* Capture writing PC (set by recompiler before every store) into the
     * SIO PC tracer ring before the write actually takes effect. */
    debug_server_log_sio_write(addr, value, 4);

    switch (addr) {
    case 0x1F801040: /* SIO_TX_DATA */
        sio_tx_data = (uint8_t)value;
        sio_tx_writes++;
        sio_last_ctrl_on_tx = sio_ctrl;
        if (!(sio_ctrl & SIO_CTRL_TX_EN)) sio_tx_gated++;
        if (sio_ctrl & SIO_CTRL_TX_EN) {
            /* IRQ-arm chain audit: capture pre-state for diagnostics.
             * NOTE: bucket by active_device AT-DECISION-TIME (post-process),
             * not pre-process — sio_process_byte (re)assigns active_device
             * on its first byte / on deselect-continuation. */
            uint16_t arm_dbg_ctrl_pre  = sio_ctrl;
            uint16_t arm_dbg_stat_pre  = sio_stat;

            sio_process_byte(sio_tx_data);
            /* Capture the device the if-block will read. */
            uint8_t  arm_dbg_dev_at_decision = (uint8_t)active_device;
            /* Model SIO busy: on real hardware TX_RDY clears during the
             * byte transfer (~BAUD*8 cycles) and re-sets when complete.
             * The BIOS pad polling tight-loops on TX_RDY before sending
             * each byte.  Without this, pad polling proceeds instantly
             * over a card byte that's still "in flight", interleaving
             * pad and card protocols within the same interrupt. */
            sio_stat &= ~(SIO_STAT_TX_RDY | SIO_STAT_TX_EMPTY);

            /* IRQ-arm chain audit: count attempts and failure reasons.
             * Counted only for the card path (active_device==DEV_MEMCARD).
             * Exposed via sio_get_card_arm_audit() / debug_server. */
            extern void sio_card_arm_audit_record(int dev, uint16_t ctrl_pre,
                                                  uint16_t stat_pre, uint16_t stat_post,
                                                  int armed, int countdown_after);

            int armed_now = 0;
            if ((sio_stat & SIO_STAT_ACK) && (sio_ctrl & SIO_CTRL_ACK_IRQ_EN)) {
                sio_stat &= ~SIO_STAT_ACK;
                sio_irq_pending = 1;
                /* Use a longer delay for card transfers to keep TX_RDY
                 * cleared through pad polling within the same ISR. */
                sio_irq_countdown = (active_device == DEV_MEMCARD)
                    ? SIO_IRQ_DELAY_CARD : SIO_IRQ_DELAY_PAD;
                /* Arm IRQ ring context for capture at fire time. */
                sio_irq_pending_source   = (active_device == DEV_MEMCARD)
                                           ? SIO_IRQ_SRC_CARD_ACK : SIO_IRQ_SRC_PAD_ACK;
                sio_irq_pending_slot     = (uint8_t)selected_slot;
                sio_irq_pending_delay    = (uint8_t)sio_irq_countdown;
                sio_irq_pending_mc_state = (uint8_t)mc_state;
                sio_irq_pending_byte_seq = sio_trace_seq;
                armed_now = 1;
            }

            sio_card_arm_audit_record(arm_dbg_dev_at_decision, arm_dbg_ctrl_pre,
                                      arm_dbg_stat_pre, sio_stat,
                                      armed_now, sio_irq_countdown);
        }
        break;

    case 0x1F801048: /* SIO_MODE */
        sio_mode = (uint16_t)value;
        break;

    case 0x1F80104A: /* SIO_CTRL */ {
        uint16_t old_ctrl = sio_ctrl;
        sio_ctrl = (uint16_t)value;
        if (value & SIO_CTRL_ACK) {
            sio_stat &= ~SIO_STAT_IRQ;
            sio_stat &= ~SIO_STAT_ACK;
        }
        if (value & SIO_CTRL_RESET) {
            if (mc_state != MC_IDLE) {
                sio_mc_abort_count++;
                sio_mc_abort_state = (int)mc_state;
                sio_mc_abort_ctrl = (uint16_t)value;
            }
            if (sio_txn_open) {
                extern uint32_t g_debug_current_func_addr;
                txn_close(SIO_TXN_END_ABORT_RESET, mc_state, g_debug_current_func_addr);
            }
            sio_stat = SIO_STAT_TX_RDY | SIO_STAT_TX_EMPTY;
            sio_mode = 0;
            sio_ctrl = 0;
            sio_baud = 0;
            pad_state = PAD_IDLE;
            mc_state = MC_IDLE;
            mc_slots[0].state = MC_IDLE;
            mc_slots[1].state = MC_IDLE;
            active_device = DEV_NONE;
            sio_irq_pending = 0;
            sio_irq_countdown = 0;
        }
        /* On SELECT deassert: reset all volatile protocol state.
         *
         * Earlier model preserved mc_state into mc_slots[] so a card
         * protocol could resume across brief SELECT drops (e.g. CTRL
         * ACK writes during pad polling). Audit confirmed this caused
         * EVERY new 0x81 after a SELECT-deassert to land on a saved
         * non-IDLE state, triggering abort_reselect bookkeeping. The
         * abort path itself resets mc_state, so the BIOS-visible RX is
         * still correct — but the simulated card retaining sector/cmd/
         * counter context across what BIOS treats as independent
         * transactions is wrong per the no$psx model: real cards
         * restart on every fresh 0x81. SELECT is a bus-wide signal;
         * both physical cards see the deassert and reset their state
         * machines simultaneously.
         *
         * Cleared: state, cmd, sector, sector_msb, sector_lsb,
         *          data_idx, checksum (volatile protocol state).
         * Preserved: flag (persistent "new card" metadata, only cleared
         *            after first read/write completes; carries across
         *            SELECT drops on real hardware) and data (sector
         *            buffer; harmless to retain since the next read
         *            overwrites it). */
        if ((old_ctrl & SIO_CTRL_SELECT) && !(value & SIO_CTRL_SELECT)) {
            if (active_device == DEV_MEMCARD && mc_state != MC_IDLE && sio_txn_open) {
                extern uint32_t g_debug_current_func_addr;
                txn_close(SIO_TXN_END_ABORT_OTHER, mc_state, g_debug_current_func_addr);
            }
            mc_state = MC_IDLE;
            for (int i = 0; i < 2; i++) {
                mc_slots[i].state      = MC_IDLE;
                mc_slots[i].cmd        = 0;
                mc_slots[i].sector     = 0;
                mc_slots[i].sector_msb = 0;
                mc_slots[i].sector_lsb = 0;
                mc_slots[i].data_idx   = 0;
                mc_slots[i].checksum   = 0;
                /* keep mc_slots[i].flag, mc_slots[i].data */
            }
            pad_state = PAD_IDLE;
            active_device = DEV_NONE;
        }
        break;
    }

    case 0x1F80104E: /* SIO_BAUD */
        sio_baud = (uint16_t)value;
        break;

    default:
        break;
    }

    /* Advance delayed IRQ after write processing (not before).
     * This ensures CTRL ACK writes clear the old IRQ before the
     * pending one fires. */
    sio_tick(0);
}

void sio_get_freeze_diag(int *out_irq_pending, int *out_irq_countdown,
                         uint16_t *out_sio_stat, uint16_t *out_sio_ctrl,
                         int *out_card_active) {
    if (out_irq_pending)   *out_irq_pending   = sio_irq_pending;
    if (out_irq_countdown) *out_irq_countdown = sio_irq_countdown;
    if (out_sio_stat)      *out_sio_stat      = sio_stat;
    if (out_sio_ctrl)      *out_sio_ctrl      = sio_ctrl;
    if (out_card_active)   *out_card_active   = sio_card_protocol_active();
}

/* ---- Bounded SIO IRQ burst drain (active card data phase only) ---------
 * Approved as the FIRST step on the path away from "1 byte/VBlank". This
 * is NOT the cycle-paced rewrite. It is a transient measure that, while
 * a card transfer is in-flight, drains the access-paced countdown so the
 * IRQ that would have stalled until the next pad poll instead fires now.
 *
 * Invariants:
 *   - Only invoked from sio_write SIO_TX_DATA, after the IRQ-arm decision.
 *   - Caller guarantees active_device==DEV_MEMCARD and !in_exception.
 *   - Hard cap on iterations (default 128).
 *   - Break early if (a) no IRQ pending and countdown idle, OR
 *                    (b) mc_state == MC_IDLE (transaction finished/aborted).
 *   - Each iteration calls sio_tick() ONCE. sio_tick already handles "fire
 *     when countdown==0" and updates i_stat / sio_stat / IRQ ring atomically.
 *   - sio_tick does not enter the BIOS exception. The BIOS exception fires
 *     later in the dispatch loop when (i_stat & i_mask & !in_exception).
 *
 * Stats (always-on, exposed via sio_burst_stats TCP cmd) so we can see if
 * the bound is being hit, what the typical iter count is, and which break
 * reason dominates. */
typedef struct {
    uint64_t calls;            /* total burst invocations */
    uint64_t iters_total;      /* total sio_tick iterations across all calls */
    uint32_t iter_max_seen;    /* max iters in a single call */
    uint64_t break_idle;       /* broke because pending=0 && countdown=0 */
    uint64_t break_mode_clear; /* broke because mc_state==MC_IDLE */
    uint64_t break_capped;     /* broke at max_iters cap */
    uint32_t fires_in_burst;   /* IRQ fires that occurred during a burst */
    uint32_t last_iters;       /* iterations on most recent call */
    uint8_t  last_break_reason;/* 1=idle, 2=mode_clear, 3=capped, 0=skip */
} SioBurstStats;

static SioBurstStats s_burst_stats;
static uint32_t s_irq_seq_at_burst_start = 0;

extern uint8_t psx_read_byte(uint32_t addr);
extern int     psx_get_in_exception(void);

static int sio_card_burst_drain(int max_iters) {
    if (max_iters <= 0) max_iters = 128;
    if (max_iters > 1024) max_iters = 1024;

    s_burst_stats.calls++;
    uint32_t fires_before = sio_irq_seq;
    int iters = 0;
    uint8_t reason = 1; /* default: idle break */

    for (; iters < max_iters; iters++) {
        /* Idle check (do BEFORE the tick — if there's nothing pending and
         * countdown is 0, ticking is a no-op anyway). */
        if (!sio_irq_pending && sio_irq_countdown == 0) {
            reason = 1;
            break;
        }
        /* Card protocol returned to IDLE — transaction finished or aborted.
         * mc_state is the authoritative "transfer in flight" signal; the
         * BIOS-RAM word [0x75C0] is only set during the data-byte phase
         * (after the address handshake), so checking it here would have
         * blocked bursting through the SELECT/CMD/ADDR setup bytes that
         * precede the data phase. */
        if (mc_state == MC_IDLE) {
            reason = 2;
            break;
        }
        sio_tick(0);
    }

    if (iters == max_iters) reason = 3;

    s_burst_stats.iters_total += (uint64_t)iters;
    if ((uint32_t)iters > s_burst_stats.iter_max_seen) s_burst_stats.iter_max_seen = (uint32_t)iters;
    if (reason == 1) s_burst_stats.break_idle++;
    else if (reason == 2) s_burst_stats.break_mode_clear++;
    else if (reason == 3) s_burst_stats.break_capped++;
    s_burst_stats.last_iters = (uint32_t)iters;
    s_burst_stats.last_break_reason = reason;
    s_burst_stats.fires_in_burst += (uint32_t)(sio_irq_seq - fires_before);
    return iters;
}

void sio_get_burst_stats(uint64_t out[10]) {
    out[0] = s_burst_stats.calls;
    out[1] = s_burst_stats.iters_total;
    out[2] = (uint64_t)s_burst_stats.iter_max_seen;
    out[3] = s_burst_stats.break_idle;
    out[4] = s_burst_stats.break_mode_clear;
    out[5] = s_burst_stats.break_capped;
    out[6] = (uint64_t)s_burst_stats.fires_in_burst;
    out[7] = (uint64_t)s_burst_stats.last_iters;
    out[8] = (uint64_t)s_burst_stats.last_break_reason;
    out[9] = 0;
}

/* Phase 1.0a: SIO_MODEL_CYCLE_PACED defaulting to 0 keeps the legacy
 * access-paced behavior unchanged. The cycles arg on sio_tick is
 * reserved for a future cycle-paced model and is ignored under
 * macro=0. */
#ifndef SIO_MODEL_CYCLE_PACED
#define SIO_MODEL_CYCLE_PACED 0
#endif

void sio_tick(int cycles) {
    (void)cycles;  /* reserved for cycle-paced model (Phase 1.0c+) */
    if (sio_irq_pending && sio_irq_countdown > 0) {
        sio_irq_countdown--;
        if (sio_irq_countdown == 0) {
            sio_irq_pending = 0;
            sio_stat |= SIO_STAT_ACK;
            sio_stat |= SIO_STAT_IRQ;
            /* Restore TX_RDY — the byte transfer is complete.
             * This unblocks the BIOS pad polling loop that spins
             * on TX_RDY before writing the next byte. */
            sio_stat |= SIO_STAT_TX_RDY | SIO_STAT_TX_EMPTY;
            uint32_t i_stat_before = i_stat;
            i_stat |= (1 << IRQ_SIO0);

            /* SIO IRQ ring capture. */
            extern uint32_t g_debug_current_func_addr;
            extern uint8_t psx_read_byte(uint32_t addr);
            SioIrqEntry *e = &sio_irq_buf[sio_irq_idx];
            e->seq            = sio_irq_seq;
            e->byte_seq       = sio_irq_pending_byte_seq;
            e->i_stat_before  = i_stat_before;
            e->i_stat_after   = i_stat;
            e->mc_state       = (uint32_t)sio_irq_pending_mc_state;
            e->active_device  = (uint32_t)active_device;
            e->ctrl           = (uint32_t)sio_ctrl;
            e->func_addr      = g_debug_current_func_addr;
            e->counter_7514   = (uint32_t)psx_read_byte(0x7514);
            e->source         = sio_irq_pending_source;
            e->slot           = sio_irq_pending_slot;
            e->delay_applied  = sio_irq_pending_delay;
            sio_irq_idx = (sio_irq_idx + 1) % SIO_IRQ_RING_CAP;
            sio_irq_seq++;
        }
    }
}
