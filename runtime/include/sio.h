#ifndef PSXRECOMP_SIO_H
#define PSXRECOMP_SIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SIO0 register base: 0x1F801040 */
#define SIO_BASE 0x1F801040

/* Phase 1.0c-v2: cycle-paced SIO model. Default 1 enables the dispatch-
 * loop quantum tick (gated by g_sio_timing_active). Set to 0 to revert
 * to legacy access-paced behavior across all callers. */
#ifndef SIO_MODEL_CYCLE_PACED
#define SIO_MODEL_CYCLE_PACED 1
#endif

void sio_init(void);

/* MMIO read/write (0x1F801040-0x1F80104E) */
uint32_t sio_read(uint32_t addr);
void sio_write(uint32_t addr, uint32_t value);

/* Advance SIO timing by `cycles` PSX cycles.
 *
 * In cycle-paced mode (SIO_MODEL_CYCLE_PACED=1, default in 1.0c+): walks
 * shift-complete and ACK-fire events scheduled in cycles ahead. In 1.0c
 * the shifter is never armed (TX path still synchronous), so the body
 * is effectively inert. Phase 1.0d will route TX through the shifter.
 *
 * The legacy access-paced IRQ countdown body always runs and is the
 * actual IRQ delivery mechanism in 1.0c. */
void sio_tick(int cycles);

/* Convenience: advance SIO by the dispatch-loop quantum. Called from
 * psx_check_interrupts inside !in_exception, gated by g_sio_timing_active
 * (no-op when no SIO cycle-paced work is pending). No-op under macro=0. */
void sio_tick_quantum(void);

/* Hot-path active guard. Set to 1 when sio_shift / sio_pending_ack /
 * buffer arms; cleared when all three empty. */
extern volatile int g_sio_timing_active;

/* Phase 1.0e-e1: peripheral-only cycle advance. Drives shifter+ack
 * scheduler. Does NOT touch the legacy sio_irq_pending/countdown. Safe
 * to call from psx_advance_cycles per emitted block. No-op when
 * g_sio_timing_active is 0. */
void sio_advance(uint32_t cycles);

/* Cycle-budgeted precise event slicing: guest CPU cycles until a DELIVERABLE
 * SIO IRQ (bit7 unmasked in i_mask). UINT32_MAX if none. */
uint32_t sio_cycles_to_irq(uint32_t i_mask);

/* Telemetry counters for sio_advance. */
uint64_t sio_get_advance_called(void);
uint64_t sio_get_advance_with_work(void);

/* Update pad button state. Buttons use PS1 convention: 0=pressed, 1=released.
   Bit layout: SELECT, L3, R3, START, UP, RIGHT, DOWN, LEFT,
               L2, R2, L1, R1, TRIANGLE, CIRCLE, CROSS, SQUARE
   sio_set_pad_state targets port 1 (slot 0); the _slot form targets either. */
void sio_set_pad_state(uint16_t buttons);
void sio_set_pad_state_slot(int slot, uint16_t buttons);

/* Set the analog stick state + pad type for a slot. enabled selects the
 * emulated pad: 0 = digital (poll id 0x41), 1 = DualShock/analog (poll id
 * 0x73, with the four 0..255 stick axes appended; 0x80 = centred). */
void sio_set_pad_analog(int slot, int enabled,
                        uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);

/* Per-frame input plumbing for the coherent-DualShock model. Update the sticks
 * every frame with sio_set_pad_sticks; change the reported pad type with
 * sio_request_pad_type (the emulated "analog button"), which is applied
 * atomically at the next idle, non-config bus boundary so a hybrid stick/d-pad
 * flip can never split a poll or a config handshake. Do NOT call
 * sio_set_pad_analog every frame for the type — that is for boot/hotplug only. */
void sio_set_pad_sticks(int slot, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);
void sio_request_pad_type(int slot, int analog);

/* Connect / disconnect a pad on a slot (0=port1, 1=port2). By default no pads
 * are connected during initial BIOS boot. */
void sio_connect_pad(int slot);
void sio_set_pad_connected(int slot, int connected);

/* Declare whether the pad on a slot is a config-capable DualShock (1) or a
 * plain digital controller (0). A real digital controller (SCPH-1080, poll id
 * 0x41) does NOT answer the config-mode commands (0x43/0x44/0x45/0x46/0x47/
 * 0x4C/0x4D/0x4F) — it returns hi-z / no ACK, so a game's pad driver classifies
 * it as digital-only and just polls with 0x42. A DualShock answers them. Set
 * from the per-player pad mode (DIGITAL => 0, ANALOG/HYBRID => 1) at boot/
 * hotplug. Default is 1 (config-capable) so existing analog/hybrid behaviour is
 * unchanged. */
void sio_set_pad_config_capable(int slot, int capable);

/* Return current pad button state (for debug server). _slot targets either. */
uint16_t sio_get_pad_buttons(void);
uint16_t sio_get_pad_buttons_slot(int slot);

/* Side-effect-FREE SIO register peeks for the debug/observability path ONLY.
 * NEVER use these for the guest bus — the guest must call sio_read() (which keeps
 * its hardware side effects: sio_tick, ACK-clear, RX-FIFO-pop). These let the
 * debug server sample SIO without perturbing the pad/memcard handshake. */
uint16_t sio_peek_stat(void);
uint16_t sio_peek_ctrl(void);
uint8_t  sio_peek_rx_data(void);

/* Debug accessors: is a pad connected on the slot, and is it in analog mode. */
int sio_get_pad_connected(int slot);
int sio_get_pad_analog(int slot);
void sio_get_pad_sticks(int slot, uint8_t out[4]);

/* ---- SIO byte-level trace ring buffer ----
 * 1M entries × ~28 B ≈ 32 MB.  At ~600 byte/sec that's ~30 min of history. */
#define SIO_TRACE_CAP (1 << 20)

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
    uint8_t  counter_7514;  /* RAM[0x7514] at time of byte */
    uint32_t cop0_sr;       /* COP0 SR at time of byte (IEc=bit0, IM2=bit10) */
    /* Saved slot states captured AT TRACE TIME (after the byte was processed
     * and any mc_save/load_slot has run). Lets the audit show whether a
     * SELECT-deassert preserved state into the next 0x81. */
    uint8_t  slot0_state;
    uint8_t  slot1_state;
} SioTraceEntry;

/* Get pointer to ring buffer and current write index.
 * Returns number of entries ever written (seq of next write). */
uint32_t sio_get_trace(const SioTraceEntry **buf_out, int *write_idx_out);

/* Current SIO byte sequence — used by SIO PC tracer for cross-referencing. */
uint32_t sio_get_seq(void);

/* Returns 1 if a memcard protocol exchange is in progress on either
 * slot. Callers (notably the VBlank scheduler) use this to defer
 * interrupt delivery so the BIOS's pad polling routine — which fires
 * from the VBlank handler — doesn't preempt an in-flight card
 * transaction by issuing 0x01 on the SIO bus mid-read. */
int sio_card_protocol_active(void);

/* Snapshot SIO IRQ-pacing internals for the freeze_check diagnostic. */
void sio_get_freeze_diag(int *out_irq_pending, int *out_irq_countdown,
                         uint16_t *out_sio_stat, uint16_t *out_sio_ctrl,
                         int *out_card_active);

/* ---- Card transaction ring buffer ----
 *
 * One entry per card protocol transaction (0x81 → terminal state / abort).
 * Always-on.  64K entries × ~544 B ≈ 35 MB — holds tens of minutes of card
 * activity at typical detection-cycle rates. */
#define SIO_TXN_CAP        (1 << 16)
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
 * Captures every time IRQ #7 (SIO0) is raised into I_STAT.  Always-on.
 * 1M entries × ~64 B ≈ 64 MB — holds tens of minutes of IRQ history. */
#define SIO_IRQ_RING_CAP (1 << 20)

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

/* Phantom-input regression A/B instrument: select pre/post-98aa688 pad config
 * behavior at runtime (0 = new state machine, 1 = legacy always-0xF3). */
int  sio_get_legacy_cfg(void);
void sio_set_legacy_cfg(int v);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_SIO_H */
