#ifndef PSXRECOMP_V4_SIO_H
#define PSXRECOMP_V4_SIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SIO0 register base: 0x1F801040 */
#define SIO_BASE 0x1F801040

void sio_init(void);

/* MMIO read/write (0x1F801040-0x1F80104E) */
uint32_t sio_read(uint32_t addr);
void sio_write(uint32_t addr, uint32_t value);

/* Advance SIO timing by one step. Called from psx_check_interrupts().
 * Fires pending IRQ7 after the transfer delay expires. */
void sio_tick(void);

/* Update pad button state. Buttons use PS1 convention: 0=pressed, 1=released.
   Bit layout: SELECT, L3, R3, START, UP, RIGHT, DOWN, LEFT,
               L2, R2, L1, R1, TRIANGLE, CIRCLE, CROSS, SQUARE */
void sio_set_pad_state(uint16_t buttons);

/* Connect a pad to a slot (0=port1, 1=port2). By default no pads are
 * connected during initial BIOS boot. */
void sio_connect_pad(int slot);

/* Return current pad button state (for debug server). */
uint16_t sio_get_pad_buttons(void);

/* ---- SIO byte-level trace ring buffer ---- */
#define SIO_TRACE_CAP 65536

typedef struct {
    uint32_t seq;           /* monotonic sequence number */
    uint8_t  tx;            /* byte written to TX */
    uint8_t  rx;            /* byte produced in RX */
    uint8_t  mc_state_pre;  /* mc_state BEFORE processing */
    uint8_t  mc_state_post; /* mc_state AFTER processing */
    uint8_t  dev_pre;       /* active_device BEFORE (0=NONE,1=PAD,2=MC) */
    uint8_t  dev_post;      /* active_device AFTER */
    uint16_t ctrl;          /* CTRL register value */
    uint32_t func_addr;     /* g_debug_current_func_addr */
    uint8_t  was_abort;     /* 1 if this byte caused mc_state abort */
    uint8_t  irq_countdown; /* sio_irq_countdown at entry */
    uint8_t  in_exception;  /* psx_get_in_exception() at time of byte */
    uint8_t  counter_7514; /* RAM[0x7514] at time of byte */
    uint32_t cop0_sr;       /* COP0 SR at time of byte (IEc=bit0, IM2=bit10) */
} SioTraceEntry;

/* Get pointer to ring buffer and current write index.
 * Returns number of entries ever written (seq of next write). */
uint32_t sio_get_trace(const SioTraceEntry **buf_out, int *write_idx_out);

/* ---- Card transaction ring buffer ----
 *
 * One entry per card protocol transaction (0x81 → terminal state / abort).
 * Always-on. Sized for ~4096 transactions; each holds the full TX/RX byte
 * stream of the transaction up to SIO_TXN_MAX_BYTES. */
#define SIO_TXN_CAP        4096
#define SIO_TXN_MAX_BYTES  256

typedef enum {
    SIO_TXN_END_OPEN          = 0, /* still in progress (only on the live txn) */
    SIO_TXN_END_SUCCESS       = 1, /* mc_state reached IDLE via natural progression */
    SIO_TXN_END_ABORT_RESELECT= 2, /* 0x81 received while txn already non-IDLE */
    SIO_TXN_END_ABORT_RESET   = 3, /* CTRL.RESET written */
    SIO_TXN_END_ABORT_SLOT    = 4, /* slot mismatch reset (DEV_NONE non-0x81 path) */
    SIO_TXN_END_ABORT_BAD_CMD = 5, /* CMD byte not 0x52/0x57/0x53 */
    SIO_TXN_END_ABORT_OTHER   = 6, /* state machine fell through */
} SioTxnEndReason;

typedef struct {
    uint32_t txn_seq;        /* monotonic transaction id */
    uint32_t start_byte_seq; /* SIO byte ring seq at first byte of txn */
    uint32_t end_byte_seq;   /* SIO byte ring seq at last byte (inclusive) */
    uint32_t start_func;     /* g_debug_current_func_addr at first byte */
    uint32_t end_func;       /* g_debug_current_func_addr at last byte */
    uint8_t  slot;           /* mc_slot when txn opened */
    uint8_t  cmd;            /* 0x52/0x57/0x53 once observed; 0 before */
    uint16_t sector;         /* 0xFFFF if address phase not reached */
    uint16_t ack_count;      /* SIO ACKs returned during txn */
    uint8_t  terminal_state; /* mc_state at termination */
    uint8_t  end_reason;     /* SioTxnEndReason */
    uint16_t byte_count;     /* total bytes processed (may exceed MAX_BYTES) */
    uint8_t  tx[SIO_TXN_MAX_BYTES];
    uint8_t  rx[SIO_TXN_MAX_BYTES];
} SioTxnEntry;

/* Returns ring buffer + next-write index + whether a live txn is currently
 * open. The live txn (when open_out=1) is NOT yet in the ring; query
 * sio_get_card_txn_live() if you want it too. Total seq = number of CLOSED
 * transactions ever recorded. */
uint32_t sio_get_card_txns(const SioTxnEntry **buf_out, int *write_idx_out, int *open_out);

/* Returns pointer to live (open) txn, or NULL if none. Lifetime: pointer
 * is valid until the next sio_process_byte call. */
const SioTxnEntry *sio_get_card_txn_live(void);

/* ---- SIO IRQ #7 delivery ring ----
 *
 * Captures every time IRQ #7 (SIO0) is raised into I_STAT. Always-on,
 * 4096 entries (~64 KB). The handoff hypothesis "SIO IRQ pacing too slow"
 * needs this ring to be falsifiable. */
#define SIO_IRQ_RING_CAP 4096

typedef enum {
    SIO_IRQ_SRC_UNKNOWN  = 0,
    SIO_IRQ_SRC_CARD_ACK = 1, /* IRQ from card-side ACK after card byte */
    SIO_IRQ_SRC_PAD_ACK  = 2, /* IRQ from pad-side ACK after pad byte */
} SioIrqSource;

typedef struct {
    uint32_t seq;
    uint32_t byte_seq;       /* corresponding sio_trace_seq when this IRQ was scheduled */
    uint32_t i_stat_before;  /* I_STAT just before raising bit 7 */
    uint32_t i_stat_after;   /* I_STAT immediately after */
    uint32_t mc_state;       /* mc_state at time of fire */
    uint32_t active_device;  /* DEV_NONE/PAD/MEMCARD at time of fire */
    uint32_t ctrl;           /* sio_ctrl at fire */
    uint32_t func_addr;      /* g_debug_current_func_addr at fire */
    uint32_t counter_7514;   /* card chain counter byte at fire */
    uint8_t  source;         /* SioIrqSource */
    uint8_t  slot;           /* selected_slot at fire */
    uint8_t  delay_applied;  /* the SIO_IRQ_DELAY_* value used */
    uint8_t  pad;
} SioIrqEntry;

uint32_t sio_get_irq_ring(const SioIrqEntry **buf_out, int *write_idx_out);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_V4_SIO_H */
