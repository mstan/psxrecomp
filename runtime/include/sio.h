#ifndef PSXRECOMP_SIO_H
#define PSXRECOMP_SIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Logical pad count. Default 2 (port1=pad0, port2=pad1).
 * PSX_MAX_PLAYERS 5..7: one SCPH-1070 (sio_set_multitap_port) + opposite pad.
 * PSX_MAX_PLAYERS 8: dual multitap (port1 pads 0–3, port2 pads 4–7). */
#ifndef PSX_MAX_PLAYERS
#define PSX_MAX_PLAYERS 2
#endif
#if PSX_MAX_PLAYERS < 1 || PSX_MAX_PLAYERS > 8
#error "PSX_MAX_PLAYERS must be in 1..8"
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

/* SCPH-1070 multitap. Off by default. When enabled (PSX_MAX_PLAYERS>=5):
 *   multitap_port==0 (console Port 1): tap on phys0 → logical pads 0–3,
 *     phys1 → logical 4.
 *   multitap_port==1 (console Port 2): phys0 → logical 0, tap on phys1 →
 *     logical pads 1–4 (Bomberman Party Edition and a few others).
 * Bulk 0x80 responses follow the real TAP/REQ latch (psx-spx): REQ=1 in the
 * third command byte arms the *next* transfer; empty tap slots are fine. */
void sio_set_multitap(int enabled);
/* [runtime] ape_card_unstick config default (env PSX_APE_CARD_UNSTICK overrides). */
void sio_set_ape_card_unstick(int on);
int  sio_get_multitap(void);
/* phys_port: 0 = console Port 1, 1 = console Port 2. Default 0. */
void sio_set_multitap_port(int phys_port);
int  sio_get_multitap_port(void);
/* Tomba-only legacy pad config path ([controller] legacy_pad_config). */
void sio_set_legacy_cfg(int enabled);
int  sio_get_legacy_cfg(void);
/* 1 when multitap is armed and `logical_slot` is a tap pad (not the lone
 * pad on the opposite console port). SCPH-1070 taps are treated as plain
 * digital controllers (0x41) by default — DualShock/analog on a tap is not
 * reliable across titles. Opt in with sio_set_multitap_analog(1) (hack). */
int  sio_pad_on_multitap(int logical_slot);
/* Opt-in DualShock-on-tap hack. Off by default. When on, tap seats may
 * report 0x73 + stick bytes in multitap bulk status (and host/netplay
 * sampling stops forcing digital). game.toml [controller] multitap_analog /
 * settings.toml / lobby match_caps may enable this. */
void sio_set_multitap_analog(int enabled);
int  sio_get_multitap_analog(void);

/* Update pad button state. Buttons use PS1 convention: 0=pressed, 1=released.
   Bit layout: SELECT, L3, R3, START, UP, RIGHT, DOWN, LEFT,
               L2, R2, L1, R1, TRIANGLE, CIRCLE, CROSS, SQUARE
   sio_set_pad_state targets logical pad 0; the _slot form targets
   logical pad 0 .. PSX_MAX_PLAYERS-1. */
void sio_set_pad_state(uint16_t buttons);
void sio_set_pad_state_slot(int slot, uint16_t buttons);

/* Set the analog stick state + pad type for a logical pad. enabled selects the
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

/* Connect / disconnect a logical pad (0 .. PSX_MAX_PLAYERS-1). By default no
 * pads are connected during initial BIOS boot. */
void sio_connect_pad(int slot);
void sio_set_pad_connected(int slot, int connected);

/* Netplay / rematch: force a peer-identical pad topology + idle bus.
 * Immediate digital (not deferred type_req), connected seats 0..slot_count-1,
 * pad FSM IDLE, cleared response buffer. Call at session start and before
 * dig0 snap so baseline_ext/sio pads cannot fork on local DualShock vs
 * keyboard seeding. */
void sio_netplay_canonicalize_session_pads(int slot_count);

/* Declare whether the pad on a logical slot is a config-capable DualShock (1)
 * or a plain digital controller (0). A real digital controller (SCPH-1080,
 * poll id 0x41) does NOT answer the config-mode commands (0x43/0x44/0x45/
 * 0x46/0x47/0x4C/0x4D/0x4F) — it returns hi-z / no ACK, so a game's pad
 * driver classifies it as digital-only and just polls with 0x42. A DualShock
 * answers them. Set from the per-player pad mode (DIGITAL => 0, ANALOG/HYBRID
 * => 1) at boot/hotplug. Default is 1 (config-capable) so existing
 * analog/hybrid behaviour is unchanged. */
void sio_set_pad_config_capable(int slot, int capable);

/* Read the raw DualShock motor state selected by the guest's 0x4D rumble map
 * and most recent 0x42 poll. `small` is the fixed-strength high-frequency
 * motor byte; `large` is the variable-strength low-frequency motor byte.
 * The frontend translates these values to its host controller API. */
void sio_get_pad_rumble(int slot, uint8_t *small, uint8_t *large);

/* Return current pad button state (for debug server). _slot targets a logical
 * pad 0 .. PSX_MAX_PLAYERS-1. */
uint16_t sio_get_pad_buttons(void);
uint16_t sio_get_pad_buttons_slot(int slot);

/* Side-effect-FREE SIO register peeks for the debug/observability path ONLY.
 * NEVER use these for the guest bus — the guest must call sio_read() (which keeps
 * its hardware side effects: sio_tick, ACK-clear, RX-FIFO-pop). These let the
 * debug server sample SIO without perturbing the pad/memcard handshake. */
uint16_t sio_peek_stat(void);
uint16_t sio_peek_ctrl(void);
uint8_t  sio_peek_rx_data(void);

/* Debug accessors: is a logical pad connected, and is it in analog mode. */
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

/* 1 while a card txn/protocol is live or a card ACK just fired. interrupts.c
 * uses this to defer in-exception ChangeThread so the game's SIO IntRP can
 * finish its A6C10/B4E38 handshake (Ape Escape LOAD). */
int sio_should_defer_thread_switch(void);

/* -------------------------------------------------------------------------
 * Ape Escape LOAD GAME — IMPORTANT memcard path
 *
 * LibCardIntRP needs one distinct IRQ7 edge per nest pop. SIO pacing (MT=1),
 * ACK defer, I_MASK.7 hold, and nest IRQ pulses below exist so presence-probe
 * → directory → file-list can complete.
 * EXPERIMENT (TwistedMetal4Recomp): formerly netplay-inert / MotK-uncapped;
 * ungated for local netplay desync testing. PSX_APE_CARD_UNSTICK=0 disables
 * nest repair + bit7 hold. See ApeEscapeRecomp/docs/APE_MEMCARD_LOAD.md.
 * ------------------------------------------------------------------------- */

/* Post-probe handoff ring: bit7 enable → TX 0x57 (debug_server card_handoff). */
typedef struct {
    uint8_t  kind;   /* 1=probe_abort 2=b7_set 3=b7_clear 4=tx 5=card_ack
                      * 6=unstick 7=select_flush_ack 8=ack_deferred_istat7
                      * 9=nest_irq_pulse 10=b7_hold */
    uint8_t  byte;
    uint16_t imask;
    uint32_t pc;
    uint32_t func;
    uint32_t a6c10;
    uint32_t b4e30;
    uint32_t b4e38;
    uint64_t cyc;
} SioCardHandoffEntry;
const SioCardHandoffEntry *sio_get_card_handoff(int *idx_out, int *count_out);
int sio_card_handoff_cap(void);
int sio_card_handoff_armed(void);
void sio_card_handoff_on_imask(uint32_t old_mask, uint32_t new_mask);
/* Ape Escape: 1 while libcard nest is stuck post-probe — keep I_MASK.7 so
 * IntRP can finish (BIOS otherwise clears bit7 ~tens of cycles after SELECT
 * abort, before the pending SIO edge is taken). */
int sio_card_should_hold_imask_bit7(void);
/* Ape Escape: nest repair pump when the guest waiter polls RAM only
 * (no SIO MMIO → sio_tick never runs). IRQ re-edge only — never poke A6C10 /
 * invent B4E38. */
void sio_ape_card_unstick_pump(void);

/* Netplay deferred-present coexistence: 1 while native memcard SIO is
 * busy and still making progress. Callers keep s_present_pending and
 * retry later so BB-edge finish_frame does not run mid card busy-wait
 * (Ape Escape empty-starfield wedge; same class of risk on other titles).
 * Returns 0 after ~10 VBlanks with no SIO seq progress so a wedged card
 * FSM cannot stall commit forever (mirrors VBlank-for-SIO stale escape). */
int sio_hold_present_for_card(void);

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

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_SIO_H */
