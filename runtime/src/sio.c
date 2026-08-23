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
#include "psx_bss.h"
#include "memcard.h"
#include "debug_server.h"
#include "event_ring.h"
#include "gpu.h"
#include <stdlib.h>
#include <string.h>

/* I_STAT is owned by memory.c */
extern uint32_t i_stat;
/* Central IRQ-raise choke point (interrupts.c) — also records the device ring. */
extern void psx_irq_raise(uint32_t bit, uint32_t detail);

/* IRQ bit for SIO0 */
#define IRQ_SIO0 7

/* SIO registers */
static uint8_t  sio_tx_data;
static uint8_t  sio_rx_data;
static uint16_t sio_stat;
static uint16_t sio_mode;
static uint16_t sio_ctrl;
static uint16_t sio_baud;
static uint32_t sio_debug_poll_counter;

static void sio_debug_poll_maybe(void) {
    if ((++sio_debug_poll_counter & 0x3FFu) == 0) {
        debug_server_poll();
    }
}

/* Pad state: 0=pressed, 1=released (PS1 convention). Indexed by LOGICAL pad
 * 0 .. PSX_MAX_PLAYERS-1 (not physical SIO slot). */
static uint16_t pad_buttons[PSX_MAX_PLAYERS] = { [0 ... PSX_MAX_PLAYERS - 1] = 0xFFFF };

/* Per-logical-pad type + analog stick state. analog: 0=digital pad (poll id
 * 0x41), 1=DualShock/analog (poll id 0x73). Sticks are 0..255, 0x80 centred. */
static PSX_BSS uint8_t pad_analog[PSX_MAX_PLAYERS];
static uint8_t pad_stick[PSX_MAX_PLAYERS][4] = {
    [0 ... PSX_MAX_PLAYERS - 1] = { 0x80, 0x80, 0x80, 0x80 }
}; /* lx,ly,rx,ry */

/* DualShock command 0x4D maps the six writable bytes in a 0x42 poll onto the
 * two motors: 0x00 = small/high-frequency, 0x01 = large/low-frequency,
 * 0xFF = unused. The map powers up unassigned and is echoed back while a new
 * map is latched, matching the physical pad/Mednafen protocol. */
static uint8_t pad_rumble_map[PSX_MAX_PLAYERS][6] = {
    [0 ... PSX_MAX_PLAYERS - 1] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
};
static PSX_BSS uint8_t pad_rumble_small[PSX_MAX_PLAYERS];
static PSX_BSS uint8_t pad_rumble_large[PSX_MAX_PLAYERS];

/* Analog-mode lock, per logical pad. A real DualShock's config command 0x44
 * 0x..02/0x03 locks/unlocks the mode (dualshock.cpp:714-725); a locked pad
 * ignores the physical analog button (dualshock.cpp:203). We emulate the
 * analog button via the host hybrid heuristic (pad_type_req), so when a game
 * LOCKS the mode the hybrid auto-flip must not override it — else the type
 * flips underneath a game that pinned DualShock, the exact desync the
 * deferred-request machinery cannot otherwise prevent. */
static PSX_BSS uint8_t analog_mode_locked[PSX_MAX_PLAYERS];

/* Which logical pads have devices connected (bit i = pad i). Fits 5 pads. */
static uint8_t pad_connected = 0;

/* Host-side SCPH-1070 enable. Only meaningful when PSX_MAX_PLAYERS >= 5. */
static int sio_multitap_enabled = 0;
/* Physical SIO port hosting the tap: 0 = console Port 1, 1 = Port 2. */
static int sio_multitap_port = 0;
/* Opt-in DualShock on tap seats (default off — faithful digital taps). */
static int sio_multitap_analog_hack = 0;

/* Pad communication state machine */
typedef enum {
    PAD_IDLE,
    PAD_WAIT_ACCESS,    /* received 0x01 (pad access) */
    PAD_SEND_RESPONSE,  /* sending command response bytes */
} PadState;

/* Multitap 0x42 bulk: ID(0x80)+0x5A + 4×8 pad status bytes. */
#define PAD_RESPONSE_MAX 34

/* What the next 0x42 on a multitap port returns (psx-spx TAP/REQ latch).
 * Indexed by physical SIO port so dual multitap keeps independent FSMs. */
typedef enum {
    MTAP_NEXT_SLOT_A = 0,
    MTAP_NEXT_BULK,
    MTAP_NEXT_GARBAGE,
} MtapNextMode;

static PadState pad_state = PAD_IDLE;
static int selected_slot = 0;          /* physical SIO slot (CTRL bit13): 0 or 1 */
static int pad_active_logical = 0;     /* logical pad for single-pad / config cmds */
static PSX_BSS uint8_t pad_response[PAD_RESPONSE_MAX];
static uint8_t pad_response_len = 0;
static uint8_t pad_response_idx = 0;
static uint8_t pad_current_cmd = 0;
/* Address byte that opened this pad txn (01h=Slot A / bulk, 02h..04h=B..D). */
static uint8_t pad_mtap_addr = 0x01;
static int mtap_next_mode[2] = { MTAP_NEXT_SLOT_A, MTAP_NEXT_SLOT_A };
static int mtap_req_this[2];          /* TAP==1 seen on current 0x42 txn */
static int mtap_returned[2] = { MTAP_NEXT_SLOT_A, MTAP_NEXT_SLOT_A };
/* DualShock config-mode latch, per logical pad. A real controller only answers
 * the config commands (0x44/0x45/0x46/0x47/0x4C/0x4D/0x4F) and reports the
 * config ID 0xF3 while it is IN config mode; outside config it reports its
 * normal ID (0x41 digital / 0x73 analog) and ignores config commands. Config is
 * entered/exited by command 0x43 with the data byte 0x01(enter)/0x00(exit).
 * Faking "always in config" (constant 0xF3) wedges games that probe the pad
 * type via 0x43 before polling — e.g. Mega Man X6 loops 01 43 00 00 forever
 * and never reaches 0x42. (MMX6 ISSUES.md #2.) */
static PSX_BSS uint8_t pad_in_config[PSX_MAX_PLAYERS];

/* Whether the pad on a logical slot is a config-capable DualShock (1) or a
 * plain digital controller (0). A real SCPH-1080 digital pad (poll id 0x41)
 * does NOT answer the config-mode commands (0x43/0x44/.../0x4F): it returns
 * hi-z and the transaction ends. A game's pad driver that probes with 0x43 to
 * detect a DualShock therefore classifies a digital pad as digital-only and
 * just polls it with 0x42. Tomba 2's driver probes this way every frame; when
 * the SM (wrongly) answered 0x43 for its digital pad it went down the
 * DualShock config path and read the 0x00 config-response bytes as buttons ->
 * phantom "all pressed" input. Default 1 keeps analog/hybrid pads unchanged;
 * main.cpp sets 0 for PAD_MODE_DIGITAL. */
static uint8_t pad_supports_config[PSX_MAX_PLAYERS] = {
    [0 ... PSX_MAX_PLAYERS - 1] = 1
};

/* Coherent-DualShock model (Tomba phantom-input fix). A real controller never
 * changes its reported type (0x41 digital <-> 0x73 analog) in the middle of a
 * transaction, nor while the host is mid config-handshake: the type only flips
 * at a clean boundary, driven by the physical ANALOG button or a game 0x44
 * set-mode command. The launcher "hybrid" mode emulates that analog button, so
 * it must NOT slam pad_analog[] every frame (which could flip the reported type
 * underneath an in-flight poll or a multi-transaction config handshake and
 * desync the game's pad driver -> phantom/garbage button reads). Instead the
 * host REQUESTS a type via pad_type_req[] and the change is applied atomically
 * only when the bus is idle (PAD_IDLE) and the pad is NOT in config mode. A
 * request raised during config is held until config exits. -1 = no request. */
static int8_t pad_type_req[PSX_MAX_PLAYERS] = {
    [0 ... PSX_MAX_PLAYERS - 1] = -1
};

/* ---- Logical pad ↔ physical SIO port mapping ----
 *
 * Multitap off (default / PSX_MAX_PLAYERS==2):
 *   physical 0 → logical 0, physical 1 → logical 1
 * Single multitap (PSX_MAX_PLAYERS 5..7) on Port 1 (sio_multitap_port==0):
 *   physical 0 → multitap (pads A–D = logical 0–3; Slot A path = 0)
 *   physical 1 → logical 4
 * Single multitap on Port 2 (sio_multitap_port==1):
 *   physical 0 → logical 0
 *   physical 1 → multitap (pads A–D = logical 1–4; Slot A path = 1)
 * Dual multitap (PSX_MAX_PLAYERS >= 8):
 *   physical 0 → multitap pads A–D = logical 0–3
 *   physical 1 → multitap pads A–D = logical 4–7
 */
static int sio_multitap_active(void) {
#if PSX_MAX_PLAYERS >= 5
    return sio_multitap_enabled;
#else
    return 0;
#endif
}

/* Dual SCPH-1070 when the build can host 8 pads. */
static int sio_dual_multitap(void) {
#if PSX_MAX_PLAYERS >= 8
    return sio_multitap_enabled;
#else
    return 0;
#endif
}

static int mtap_slot_a_logical(void) {
    if (sio_dual_multitap())
        return (selected_slot == 0) ? 0 : 4;
    return (sio_multitap_port == 0) ? 0 : 1;
}

static int mtap_standalone_logical(void) {
    if (sio_dual_multitap())
        return -1; /* no lone opposite-port pad */
    return (sio_multitap_port == 0) ? 4 : 0;
}

static int pad_logical_for_port(int phys_port) {
    if (phys_port < 0 || phys_port > 1) return -1;
    if (sio_multitap_active()) {
        if (sio_dual_multitap())
            return (phys_port == 0) ? 0 : 4;
        if (phys_port == sio_multitap_port)
            return (sio_multitap_port == 0) ? 0 : 1;
        return mtap_standalone_logical();
    }
    return phys_port;
}

/* Physical port answers when a device is present. Multitap itself is present
 * whenever enabled (individual tap slots may still be empty). */
static int pad_port_has_device(int phys_port) {
    if (phys_port < 0 || phys_port > 1) return 0;
    if (sio_multitap_active()) {
        if (sio_dual_multitap()) return 1;
        if (phys_port == sio_multitap_port) return 1;
        return (pad_connected & (1u << mtap_standalone_logical())) ? 1 : 0;
    }
    return (pad_connected & (1u << phys_port)) ? 1 : 0;
}

static int selected_is_mtap_port(void) {
    if (!sio_multitap_active()) return 0;
    if (sio_dual_multitap()) return (selected_slot == 0 || selected_slot == 1);
    return selected_slot == sio_multitap_port;
}

/* After a completed 0x42 on the multitap port, arm the next response mode
 * from the REQ bit seen this transfer and what we just returned (psx-spx). */
static void mtap_finish_42(void) {
    int p;
    if (!selected_is_mtap_port() || pad_current_cmd != 0x42 || pad_mtap_addr != 0x01)
        return;
    p = selected_slot;
    if (p < 0 || p > 1) return;
    if (!mtap_req_this[p]) {
        mtap_next_mode[p] = MTAP_NEXT_SLOT_A;
    } else if (mtap_returned[p] == MTAP_NEXT_SLOT_A) {
        mtap_next_mode[p] = MTAP_NEXT_BULK;
    } else if (mtap_returned[p] == MTAP_NEXT_BULK) {
        mtap_next_mode[p] = MTAP_NEXT_GARBAGE;
    } else {
        mtap_next_mode[p] = MTAP_NEXT_BULK;
    }
}

/* Fill 8-byte per-pad status block used in multitap bulk 0x42 responses.
 * Disconnected → all 0xFF. Digital → 0x41 0x5A btnL btnH + 0xFF pad.
 * Analog/config → 0x73/0xF3 0x5A btn + stick bytes. */
static void pad_fill_status8(int logical, uint8_t out[8]) {
    if (logical < 0 || logical >= PSX_MAX_PLAYERS ||
        !(pad_connected & (1u << logical))) {
        memset(out, 0xFF, 8);
        return;
    }
    const uint8_t id = pad_in_config[logical] ? 0xF3
                       : (pad_analog[logical] ? 0x73 : 0x41);
    const uint16_t btn = pad_buttons[logical];
    out[0] = id;
    out[1] = 0x5A;
    out[2] = (uint8_t)(btn & 0xFF);
    out[3] = (uint8_t)(btn >> 8);
    if (pad_analog[logical] || pad_in_config[logical]) {
        out[4] = pad_stick[logical][2]; /* right X */
        out[5] = pad_stick[logical][3]; /* right Y */
        out[6] = pad_stick[logical][0]; /* left X */
        out[7] = pad_stick[logical][1]; /* left Y */
    } else {
        out[4] = out[5] = out[6] = out[7] = 0xFF;
    }
}

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
    MC_WRITE_LSB_ECHO,   /* rx = sector_lsb while accepting first data byte */
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
static PSX_BSS SioTraceEntry sio_trace_buf[SIO_TRACE_CAP];
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

/* Hold ChangeThread-defer across the card ACK → guest IntRP epilogue window.
 * SELECT deassert clears mc_state before DeliverEvent / nested pops finish, so
 * protocol_active alone is too narrow. ~2 VBlank periods of tail cover the
 * libcard A6C10/B4E38 handshake without pinning defer forever on a wedge. */
static uint64_t s_card_ct_defer_until_cyc = 0;
/* Declared early: sio_should_defer_thread_switch() needs it before the txn ring. */
static int sio_txn_open = 0;

static void sio_arm_card_ct_defer_guard(void) {
    extern uint64_t psx_get_cycle_count(void);
    uint64_t now = psx_get_cycle_count();
    uint64_t until = now + (uint64_t)gpu_vblank_period_cycles() * 2ull;
    if (until > s_card_ct_defer_until_cyc)
        s_card_ct_defer_until_cyc = until;
}

#if SIO_MODEL_CYCLE_PACED
static void sio_fire_ack_irq(void);
static void sio_handle_shift_complete(void);
#endif

int sio_should_defer_thread_switch(void) {
    /* Diagnostic helper for card_handoff / future gates. Not wired into
     * can_defer — forcing defer while A6C10 is nested prevented recovery. */
    if (sio_txn_open) return 1;
    if (sio_card_protocol_active()) return 1;
    extern uint64_t psx_get_cycle_count(void);
    return psx_get_cycle_count() < s_card_ct_defer_until_cyc;
}

/* ---- bit7 → TX 0x57 handoff ring (Ape LOAD diagnosis) ---- */
#define CARD_HANDOFF_CAP 256
static SioCardHandoffEntry s_card_handoff[CARD_HANDOFF_CAP];
static int s_card_handoff_idx = 0;
static int s_card_handoff_count = 0;
static int s_card_handoff_armed = 0; /* set after LOAD-style probe abort */

static void card_handoff_push(uint8_t kind, uint8_t byte) {
    extern uint32_t g_debug_current_func_addr;
    extern uint32_t g_debug_last_store_pc;
    extern uint32_t i_mask;
    extern uint64_t psx_get_cycle_count(void);
    extern uint32_t psx_read_word(uint32_t addr);
    SioCardHandoffEntry *e = &s_card_handoff[s_card_handoff_idx];
    e->kind  = kind;
    e->byte  = byte;
    e->imask = (uint16_t)(i_mask & 0x7FFu);
    e->pc    = g_debug_last_store_pc;
    e->func  = g_debug_current_func_addr;
    e->a6c10 = psx_read_word(0x800A6C10u);
    e->b4e30 = psx_read_word(0x800B4E30u);
    e->b4e38 = psx_read_word(0x800B4E38u);
    e->cyc   = psx_get_cycle_count();
    s_card_handoff_idx = (s_card_handoff_idx + 1) % CARD_HANDOFF_CAP;
    s_card_handoff_count++;
}

/* =========================================================================
 * Ape Escape LOAD GAME — IMPORTANT memcard nest repair
 *
 * Without this block, LOAD wedges on the empty Checking starfield after
 * 81 52 00 (A6C10 stuck nested, B4E38 never latches). Do not remove,
 * collapse A6C10 to 0, or host-synth B4E20/B4E30/B4E38 without re-running
 * ApeEscapeRecomp/tools/ape_memcard_loadtest.py.
 * Doc: ApeEscapeRecomp/docs/APE_MEMCARD_LOAD.md
 *
 * EXPERIMENT (TwistedMetal4Recomp): formerly netplay-gated OFF so MotK
 * leftover-time SIO walks stayed uncapped. Ungated here for local netplay
 * desync testing — PSX_APE_CARD_UNSTICK=0 still disables.
 * =========================================================================
 *
 * LibCardIntRP (0x800226C8) only sets B4E38 when A6C10 becomes idle
 * (bit31 / 0xFFFFFFFF) after pop. Merged IRQ7 edges leave depth≥1 so one
 * pop lands on 0 and skips B4E38 — guest never re-arms bit7 / directory.
 * Repair = re-edge IRQ7 only (no host B4E* / A6C10 stores).
 * Default ON; PSX_APE_CARD_UNSTICK=0 disables. */
static int s_ape_unstick_env = -1;
static int s_ape_unstick_pending = 0;
static uint64_t s_ape_unstick_cool_cyc = 0;
static int s_ape_torn_pulses = 0;

static int ape_unstick_enabled(void) {
    if (s_ape_unstick_env < 0) {
        const char *e = getenv("PSX_APE_CARD_UNSTICK");
        if (e && e[0] == '0')
            s_ape_unstick_env = 0;
        else
            s_ape_unstick_env = 1;
    }
    return s_ape_unstick_env;
}

/* Defined later; sio_tick calls the pump. */
void sio_ape_card_unstick_pump(void);

/* Nest repair for Ape Escape LOAD (LibCardIntRP).
 *
 * Idle test is A6C10 bit31; publish runs only when a successful pop leaves
 * bit31 set. Tip post-probe hang without collapse: a6=1, busy=2, B4E38=0
 * (one IntRP short). Re-edge IRQ7 until idle/publish — never poke A6C10
 * or invent B4E20/B4E30/B4E38. */
static void ape_card_unstick_maybe(int allow_b4e38_synth) {
    if (!ape_unstick_enabled()) return;
    if (!s_ape_unstick_pending) return;
    if (sio_txn_open) return;
    extern uint64_t psx_get_cycle_count(void);
    extern uint32_t psx_read_word(uint32_t addr);
    extern void psx_write_word(uint32_t addr, uint32_t val);
    uint64_t now = psx_get_cycle_count();
    if (now < s_ape_unstick_cool_cyc) return;
    uint32_t a6 = psx_read_word(0x800A6C10u);
    uint32_t b30 = psx_read_word(0x800B4E30u);
    uint32_t b38 = psx_read_word(0x800B4E38u);
    (void)allow_b4e38_synth;
    if (b38 != 0u || (a6 & 0x80000000u) != 0u) {
        s_ape_unstick_pending = 0;
        s_ape_torn_pulses = 0;
        return;
    }
    if (b30 == 0u) {
        s_ape_unstick_pending = 0;
        return;
    }

    /* Nested post-probe / mid-scan: keep re-edging IRQ7 so IntRP can pop.
     * Depth 1 needs two successful pops (1→0→FFFFFFFF) before B4E38 latches;
     * a single pulse after SELECT is often eaten. No A6C10/B4E* host stores. */
    extern uint32_t i_mask;
    if (!(i_mask & 0x80u))
        i_mask |= 0x80u;
    i_stat &= ~0x80u;
    psx_irq_raise(IRQ_SIO0, 0);
    /* ~2ms between pulses — faster than one VB/8 so two pops can land
     * before BIOS clears I_MASK.7 for good. */
    s_ape_unstick_cool_cyc = now + (uint64_t)gpu_vblank_period_cycles() / 32ull;
    if (s_card_handoff_armed)
        card_handoff_push(6, (uint8_t)(a6 & 0xffu));
    if (++s_ape_torn_pulses >= 128)
        s_ape_unstick_pending = 0;
}

void sio_card_handoff_on_imask(uint32_t old_mask, uint32_t new_mask) {
    if (!s_card_handoff_armed) return;
    if (!(old_mask & 0x80u) && (new_mask & 0x80u))
        card_handoff_push(2, 0);
    else if ((old_mask & 0x80u) && !(new_mask & 0x80u)) {
        card_handoff_push(3, 0);
        ape_card_unstick_maybe(1);
    }
}

int sio_card_should_hold_imask_bit7(void) {
    static int s_hold_left = -1; /* -1 = idle; 0 = exhausted; >0 remaining */
    if (!ape_unstick_enabled()) { s_hold_left = -1; return 0; }
    if (!s_card_handoff_armed && !s_ape_unstick_pending) {
        s_hold_left = -1;
        return 0;
    }
    extern uint32_t psx_read_word(uint32_t addr);
    uint32_t a6 = psx_read_word(0x800A6C10u);
    uint32_t b38 = psx_read_word(0x800B4E38u);
    if ((a6 & 0x80000000u) != 0u || b38 != 0u) {
        s_hold_left = -1;
        return 0;
    }
    if (s_hold_left < 0)
        s_hold_left = 32; /* fresh nest episode — need room for 1→0→idle */
    if (s_hold_left == 0)
        return 0; /* exhausted this episode */
    s_hold_left--;
    if (s_card_handoff_armed)
        card_handoff_push(10, (uint8_t)(a6 & 0xffu)); /* b7_hold */
    return 1;
}

const SioCardHandoffEntry *sio_get_card_handoff(int *idx_out, int *count_out) {
    if (idx_out) *idx_out = s_card_handoff_idx;
    if (count_out) *count_out = s_card_handoff_count;
    return s_card_handoff;
}
int sio_card_handoff_cap(void) { return CARD_HANDOFF_CAP; }
int sio_card_handoff_armed(void) { return s_card_handoff_armed; }

int sio_hold_present_for_card(void) {
    /* Current CRTC VBlank period — matches interrupts.c. */
    enum { SIO_PRESENT_HOLD_STALE_VB = 10 };
    static uint32_t s_hold_seq;
    static uint64_t s_hold_progress_cyc;
    static int s_hold_armed;
    uint32_t seq;
    uint64_t now;
    const uint64_t stale_cycles =
        (uint64_t)gpu_vblank_period_cycles() * (uint64_t)SIO_PRESENT_HOLD_STALE_VB;

    if (!sio_card_protocol_active()) {
        s_hold_armed = 0;
        return 0;
    }
    extern uint64_t psx_get_cycle_count(void);
    seq = sio_get_seq();
    now = psx_get_cycle_count();
    if (!s_hold_armed || seq != s_hold_seq) {
        s_hold_seq = seq;
        s_hold_progress_cyc = now;
        s_hold_armed = 1;
    }
    if (now - s_hold_progress_cyc >= stale_cycles)
        return 0; /* stale: allow present drain */
    return 1;
}

/* Forward decl: defined below sio_get_freeze_diag. */
static int sio_card_burst_drain(int max_iters);
extern int psx_get_in_exception(void);
extern uint8_t psx_read_byte(uint32_t addr);

/* ---- Phase 1.0e-e2 cycle-paced SIO state (lifted above sio_write) ----
 *
 * Pad and card share a single shifter / one-byte buffer / ACK pipeline.
 * No device-specific paths. Macro is in sio.h; defaults to 1. */
volatile int g_sio_timing_active = 0;
#if SIO_MODEL_CYCLE_PACED
#define SIO_BAUD_CYCLES_DEFAULT 1088
#define SIO_ACK_CYCLES_DEFAULT  170
static int sio_tick_quantum_cycles = 64;
static int     sio_shift_active     = 0;
static uint8_t sio_shift_byte       = 0;
static int     sio_shift_remaining  = 0;
static int     sio_tx_buffered      = 0;
static uint8_t sio_tx_buffer        = 0;
static int     sio_shift_ack_irq_en = 0;
static int     sio_tx_buffer_ack_irq_en = 0;
static int     sio_pending_ack      = 0;
static int     sio_ack_remaining    = 0;
static int     sio_pending_ack_irq_en = 0;
typedef enum {
    SIO_OWNER_NONE = 0, SIO_OWNER_CARD = 1, SIO_OWNER_PAD = 2, SIO_OWNER_UNKNOWN = 3
} SioBusOwner;
static SioBusOwner sio_bus_owner = SIO_OWNER_NONE;
static uint32_t sio_bus_byte_index = 0;
static uint64_t s_pace_tx_writes_buffered;
static uint64_t s_pace_tx_writes_dropped_busy;
static uint64_t s_pace_tx_writes_dropped_cross_device;
static uint64_t s_pace_cross_device_pad_during_card;
static uint64_t s_pace_tx_buffer_promoted;
static uint64_t s_pace_tx_buffer_promoted_during_card;
static uint64_t s_pace_pad_byte_processed_in_card_data;
static uint64_t s_pace_shift_completes;
static uint64_t s_pace_ack_fires;
#endif

/* ---- Starvation-ring helper -------------------------------------------
 * Fill in current SIO state at every recorded event. */
#include "starvation_ring.h"
static void sr_record(uint8_t kind, uint8_t tx, uint8_t rx) {
#if SIO_MODEL_CYCLE_PACED
    starvation_ring_record(kind, tx, rx,
                           sio_ctrl, sio_stat,
                           sio_shift_active, sio_shift_remaining,
                           sio_tx_buffered, sio_pending_ack,
                           sio_ack_remaining,
                           (uint8_t)sio_bus_owner, sio_bus_byte_index,
                           (uint8_t)active_device, (uint8_t)mc_state,
                           (uint8_t)pad_state, (uint8_t)selected_slot,
                           g_sio_timing_active);
#else
    starvation_ring_record(kind, tx, rx, sio_ctrl, sio_stat,
                           0, 0, 0, 0, 0, 0, 0,
                           (uint8_t)active_device, (uint8_t)mc_state,
                           (uint8_t)pad_state, (uint8_t)selected_slot, 0);
#endif
}


/* ---- Card transaction ring buffer ---- */
static PSX_BSS SioTxnEntry sio_txn_buf[SIO_TXN_CAP];
static int       sio_txn_idx = 0;        /* next-write slot */
static uint32_t  sio_txn_seq = 0;        /* monotonic id of next-to-close */
/* sio_txn_open declared above (defer guard). */
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
    /* Ape Escape LOAD presence probe: 81 52 00 + SELECT abort. Arm handoff
     * watch for the follow-up bit7 → directory / file-list path. */
    if (end_reason == SIO_TXN_END_ABORT_OTHER &&
        sio_txn_cur.byte_count == 3 &&
        sio_txn_cur.tx[0] == 0x81 && sio_txn_cur.tx[1] == 0x52) {
        s_card_handoff_armed = 1;
        s_ape_torn_pulses = 0;
        card_handoff_push(1, 0x52);
        sio_arm_card_ct_defer_guard();
        /* IMPORTANT (Ape Escape): tip often ends this probe one IntRP short
         * (A6C10=1, Ready=0). Do NOT poke A6C10 — writing 0 skips a nest
         * level and leaves torn idle (a6=0, no B4E38). With MT=1 + ACK
         * defer + I_MASK.7 hold, re-edge IRQ7 and let LibCardIntRP pop
         * naturally (1→0→FFFFFFFF + publish). */
        if (ape_unstick_enabled()) {
            extern uint32_t psx_read_word(uint32_t addr);
            extern uint32_t i_mask;
            uint32_t a6 = psx_read_word(0x800A6C10u);
            uint32_t b38 = psx_read_word(0x800B4E38u);
            if ((a6 & 0x80000000u) == 0u && b38 == 0u) {
                if (!(i_mask & 0x80u))
                    i_mask |= 0x80u;
                i_stat &= ~0x80u;
                card_handoff_push(9, (uint8_t)(a6 & 0xffu)); /* nest_irq_pulse */
                psx_irq_raise(IRQ_SIO0, 0);
            }
        }
    }
    /* Any finished card txn can leave A6C10 nested; arm unstick check. */
    if (ape_unstick_enabled())
        s_ape_unstick_pending = 1;
    sio_txn_buf[sio_txn_idx]   = sio_txn_cur;
    sio_txn_idx = (sio_txn_idx + 1) % SIO_TXN_CAP;
    sio_txn_seq++;
    sio_txn_open = 0;
    ape_card_unstick_maybe(0);
}

/* ---- SIO IRQ #7 delivery ring ---- */
static PSX_BSS SioIrqEntry sio_irq_buf[SIO_IRQ_RING_CAP];
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
static int sio_ack_visible_reads = 0;

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
    pad_response_len = 0;
    pad_response_idx = 0;
    pad_current_cmd = 0;
    pad_active_logical = 0;
    memset(pad_rumble_map, 0xFF, sizeof(pad_rumble_map));
    memset(pad_rumble_small, 0, sizeof(pad_rumble_small));
    memset(pad_rumble_large, 0, sizeof(pad_rumble_large));
    for (int i = 0; i < PSX_MAX_PLAYERS; i++) {
        pad_buttons[i] = 0xFFFF;
        pad_analog[i] = 0;
        pad_stick[i][0] = pad_stick[i][1] = pad_stick[i][2] = pad_stick[i][3] = 0x80;
        pad_in_config[i] = 0;
        pad_type_req[i] = -1;
        analog_mode_locked[i] = 0;
        pad_supports_config[i] = 1;
    }
    pad_connected = 0;
    /* Multitap enable/port are host preferences — leave them alone across
     * sio_init so a soft reset does not drop the tap configuration. */
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
    sio_ack_visible_reads = 0;
#if SIO_MODEL_CYCLE_PACED
    sio_shift_active = 0;
    sio_shift_remaining = 0;
    sio_tx_buffered = 0;
    sio_shift_ack_irq_en = 0;
    sio_tx_buffer_ack_irq_en = 0;
    sio_pending_ack = 0;
    sio_ack_remaining = 0;
    sio_pending_ack_irq_en = 0;
    sio_bus_owner = SIO_OWNER_NONE;
    sio_bus_byte_index = 0;
    g_sio_timing_active = 0;
#endif
    sio_txn_open = 0;
    /* Note: sio_txn_buf, sio_txn_idx, sio_txn_seq deliberately persist
     * across sio_init so post-reset diagnostics can still inspect prior
     * transactions. Boot path zero-inits them via BSS. */
}

/* Cycle-budgeted precise event slicing: guest CPU cycles until SIO raises a
 * DELIVERABLE IRQ (bit7 unmasked in i_mask). UINT32_MAX if none. Returns the
 * nearest armed countdown: shift-complete, pending ack, or the pad/card IRQ
 * delivery countdown. See PRECISE_IRQ_SLICE.md. */
uint32_t sio_cycles_to_irq(uint32_t i_mask) {
    if (!(i_mask & (1u << 7))) return 0xFFFFFFFFu;   /* IRQ_SIO0 masked */
    uint32_t best = 0xFFFFFFFFu;
    if (sio_irq_pending && sio_irq_countdown > 0 && (uint32_t)sio_irq_countdown < best)
        best = (uint32_t)sio_irq_countdown;
#if SIO_MODEL_CYCLE_PACED
    if (g_sio_timing_active) {
        if (sio_pending_ack && sio_ack_remaining > 0 && (uint32_t)sio_ack_remaining < best)
            best = (uint32_t)sio_ack_remaining;
        if (sio_shift_active && sio_shift_remaining > 0 && (uint32_t)sio_shift_remaining < best)
            best = (uint32_t)sio_shift_remaining;
    }
#endif
    return best;
}

void sio_set_multitap(int enabled) {
#if PSX_MAX_PLAYERS >= 5
    sio_multitap_enabled = enabled ? 1 : 0;
    if (!enabled) {
        mtap_next_mode[0] = mtap_next_mode[1] = MTAP_NEXT_SLOT_A;
        mtap_req_this[0] = mtap_req_this[1] = 0;
        mtap_returned[0] = mtap_returned[1] = MTAP_NEXT_SLOT_A;
    }
#else
    (void)enabled;
    sio_multitap_enabled = 0;
#endif
}

int sio_get_multitap(void) {
    return sio_multitap_active();
}

void sio_set_multitap_port(int phys_port) {
#if PSX_MAX_PLAYERS >= 5
    sio_multitap_port = (phys_port == 1) ? 1 : 0;
    mtap_next_mode[0] = mtap_next_mode[1] = MTAP_NEXT_SLOT_A;
    mtap_req_this[0] = mtap_req_this[1] = 0;
    mtap_returned[0] = mtap_returned[1] = MTAP_NEXT_SLOT_A;
#else
    (void)phys_port;
    sio_multitap_port = 0;
#endif
}

int sio_get_multitap_port(void) {
#if PSX_MAX_PLAYERS >= 5
    return sio_multitap_port;
#else
    return 0;
#endif
}

void sio_set_multitap_analog(int enabled) {
    sio_multitap_analog_hack = enabled ? 1 : 0;
}

int sio_get_multitap_analog(void) {
    return sio_multitap_analog_hack;
}

int sio_pad_on_multitap(int logical_slot) {
#if PSX_MAX_PLAYERS >= 5
    if (!sio_multitap_active()) return 0;
    if (logical_slot < 0 || logical_slot >= PSX_MAX_PLAYERS) return 0;
    /* Dual tap: every seat is a tap pad. Single tap: opposite-port lone pad
     * may stay DualShock; every other slot is on the tap. */
    if (sio_dual_multitap()) return 1;
    return (logical_slot == mtap_standalone_logical()) ? 0 : 1;
#else
    (void)logical_slot;
    return 0;
#endif
}

/* Tap seat that must stay digital unless the analog hack is armed. */
static int sio_tap_force_digital(int logical_slot) {
    return sio_pad_on_multitap(logical_slot) && !sio_multitap_analog_hack;
}

void sio_connect_pad(int slot) {
    if (slot >= 0 && slot < PSX_MAX_PLAYERS)
        pad_connected |= (uint8_t)(1u << slot);
}

void sio_netplay_canonicalize_session_pads(int slot_count)
{
    int i;
    if (slot_count < 2)
        slot_count = 2;
    if (slot_count > PSX_MAX_PLAYERS)
        slot_count = PSX_MAX_PLAYERS;

    if (slot_count >= 3)
        sio_set_multitap(1);
    else
        sio_set_multitap(0);

    /* Idle the pad bus — dig0 snaps otherwise capture mid-poll FSM / response
     * bytes that differ when peers finish present dig at different host times. */
    pad_state = PAD_IDLE;
    selected_slot = 0;
    pad_active_logical = 0;
    pad_response_len = 0;
    pad_response_idx = 0;
    pad_current_cmd = 0;
    pad_mtap_addr = 0x01;
    memset(pad_response, 0, sizeof(pad_response));
    mtap_next_mode[0] = mtap_next_mode[1] = MTAP_NEXT_SLOT_A;
    mtap_returned[0] = mtap_returned[1] = MTAP_NEXT_SLOT_A;
    mtap_req_this[0] = mtap_req_this[1] = 0;
    if (active_device == DEV_PAD)
        active_device = DEV_NONE;

    for (i = 0; i < PSX_MAX_PLAYERS; i++) {
        if (i < slot_count) {
            sio_connect_pad(i);
            /* Immediate digital — sio_request_pad_type is deferred and left
             * rematch hosts (local DualShock seed) analog=1 through dig0. */
            sio_set_pad_analog(i, 0, 0x80, 0x80, 0x80, 0x80);
            sio_set_pad_state_slot(i, 0xFFFFu);
            {
                const int force_dig =
                    sio_pad_on_multitap(i) && !sio_get_multitap_analog();
                sio_set_pad_config_capable(i, force_dig ? 0 : 1);
            }
            pad_in_config[i] = 0;
            analog_mode_locked[i] = 0;
            pad_type_req[i] = -1;
            memset(pad_rumble_map[i], 0xFF, sizeof(pad_rumble_map[i]));
            pad_rumble_small[i] = 0;
            pad_rumble_large[i] = 0;
        } else {
            sio_set_pad_connected(i, 0);
            sio_set_pad_analog(i, 0, 0x80, 0x80, 0x80, 0x80);
            sio_set_pad_state_slot(i, 0xFFFFu);
            pad_in_config[i] = 0;
            pad_type_req[i] = -1;
        }
    }
}

void sio_set_pad_connected(int slot, int connected) {
    if (slot < 0 || slot >= PSX_MAX_PLAYERS) return;
    if (connected) {
        pad_connected |= (uint8_t)(1u << slot);
    } else {
        pad_connected &= (uint8_t)~(1u << slot);
        pad_rumble_small[slot] = 0;
        pad_rumble_large[slot] = 0;
    }
}

void sio_set_pad_config_capable(int slot, int capable) {
    if (slot < 0 || slot >= PSX_MAX_PLAYERS) return;
    if (sio_tap_force_digital(slot)) capable = 0;
    pad_supports_config[slot] = capable ? 1 : 0;
    /* A plain digital pad can never be in config mode; clear any stale latch so
     * the next poll reports the digital id (0x41), not the config id (0xF3). */
    if (!capable) {
        pad_in_config[slot] = 0;
        pad_rumble_small[slot] = 0;
        pad_rumble_large[slot] = 0;
    }
}

void sio_get_pad_rumble(int slot, uint8_t *small, uint8_t *large) {
    uint8_t s = 0, l = 0;
    if (slot >= 0 && slot < PSX_MAX_PLAYERS && (pad_connected & (1u << slot))) {
        s = pad_rumble_small[slot];
        l = pad_rumble_large[slot];
    }
    if (small) *small = s;
    if (large) *large = l;
}

void sio_set_pad_state(uint16_t buttons) {
    pad_buttons[0] = buttons;
}

void sio_set_pad_state_slot(int slot, uint16_t buttons) {
    if (slot >= 0 && slot < PSX_MAX_PLAYERS) pad_buttons[slot] = buttons;
}

/* Direct set of pad type + sticks. Used at boot/hotplug (refresh_player_devices)
 * to establish the initial pinned type; safe there because the bus is idle and
 * no handshake is in flight. Per-frame input must NOT use this for the type —
 * use sio_set_pad_sticks + sio_request_pad_type so the type change is applied
 * coherently (see pad_type_req[] above). */
void sio_set_pad_analog(int slot, int enabled,
                        uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry) {
    if (slot < 0 || slot >= PSX_MAX_PLAYERS) return;
    if (sio_tap_force_digital(slot)) {
        enabled = 0;
        lx = ly = rx = ry = 0x80;
    }
    pad_analog[slot]   = enabled ? 1 : 0;
    pad_type_req[slot] = -1;   /* explicit set supersedes any pending request */
    pad_stick[slot][0] = lx; pad_stick[slot][1] = ly;
    pad_stick[slot][2] = rx; pad_stick[slot][3] = ry;
}

/* Per-frame stick update (does not touch the reported pad type). */
void sio_set_pad_sticks(int slot, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry) {
    if (slot < 0 || slot >= PSX_MAX_PLAYERS) return;
    pad_stick[slot][0] = lx; pad_stick[slot][1] = ly;
    pad_stick[slot][2] = rx; pad_stick[slot][3] = ry;
}

/* Per-frame type request (the emulated DualShock "analog button"). The flip is
 * deferred and applied atomically at the next idle, non-config boundary, so it
 * can never split a poll or a config handshake. A no-op if already that type. */
void sio_request_pad_type(int slot, int analog) {
    if (slot < 0 || slot >= PSX_MAX_PLAYERS) return;
    if (sio_tap_force_digital(slot)) analog = 0;
    int want = analog ? 1 : 0;
    pad_type_req[slot] = (pad_analog[slot] == want) ? -1 : (int8_t)want;
}

uint16_t sio_get_pad_buttons(void) {
    return pad_buttons[0];
}

uint16_t sio_get_pad_buttons_slot(int slot) {
    return (slot >= 0 && slot < PSX_MAX_PLAYERS) ? pad_buttons[slot] : 0xFFFF;
}

int sio_get_pad_connected(int slot) {
    if (slot < 0 || slot >= PSX_MAX_PLAYERS) return 0;
    return (pad_connected & (1u << slot)) ? 1 : 0;
}

int sio_get_pad_analog(int slot) {
    return (slot >= 0 && slot < PSX_MAX_PLAYERS) ? pad_analog[slot] : 0;
}

void sio_get_pad_sticks(int slot, uint8_t out[4]) {
    if (!out) return;
    if (slot < 0 || slot >= PSX_MAX_PLAYERS) {
        out[0] = out[1] = out[2] = out[3] = 0x80;
        return;
    }
    out[0] = pad_stick[slot][0]; out[1] = pad_stick[slot][1];
    out[2] = pad_stick[slot][2]; out[3] = pad_stick[slot][3];
}

/* ── LEGACY pad-config compatibility (Tomba "Hybrid" controller) ─────────────
 *
 * Why this exists, and why it is explicitly LEGACY:
 *
 *   This is the descendant of our FIRST controller implementation. It was built
 *   for Tomba, to reproduce the seamless analog/digital feel of Tomba: Special
 *   Edition — the launcher "Hybrid" mode flips the emulated pad's reported TYPE
 *   between digital (poll id 0x41) and DualShock/analog (poll id 0x73) as the
 *   player moves between the d-pad and the stick. In that first implementation
 *   the SIO pad answered the DualShock config-mode commands trivially (it always
 *   reported the config id 0xF3), and Tomba's Hybrid flip worked.
 *
 *   We then matured the pad against Mega Man X6. MMX6's libpad probes the pad
 *   type via config mode (01 43 00 00 ...) BEFORE it ever polls, and the trivial
 *   "always 0xF3" answer WEDGED it: it looped the probe forever, never reached
 *   the 0x42 poll, and had no input. The fix (commit 98aa688) was a REAL
 *   DualShock config-mode state machine — report 0xF3 only while actually in
 *   config, track 0x43 enter/exit, answer the capability queries (0x45/0x46/
 *   0x47/0x4C) like the real pad. That "modern" SM is the correct behaviour, is
 *   what MMX6 and every other title needs, and is the default.
 *
 *   But post-MMX6, under the modern SM, Tomba's Hybrid flip regressed. Any type
 *   change makes libpad re-run findpad / re-detect the pad; under the modern SM
 *   that re-detect manufactures a one-frame "pad unplugged" (buf[0] = 0xFF).
 *   Tomba reads it as a controller disconnect and unpauses the menu / drops
 *   input; MMX6 reads the held direction as released-then-re-pressed and fires a
 *   phantom dash. We could NOT, with the modern SM, keep the flip benign. Rather
 *   than block Tomba's Hybrid feature, we kept the original behaviour available
 *   as a per-game opt-in — this flag.
 *
 * What the flag does:
 *   g_pad_legacy_cfg == 0 (default)  -> modern DualShock config state machine.
 *                                       Required by MMX6; correct for every title.
 *   g_pad_legacy_cfg != 0            -> the pre-98aa688 behaviour: config commands
 *                                       always answer the config id 0xF3, with no
 *                                       enter/exit tracking. Tomba's libpad was
 *                                       written against exactly this, so its Hybrid
 *                                       flip re-detect is benign.
 *
 *   Driven per-game by [controller] legacy_pad_config in game.toml (applied via
 *   sio_set_legacy_cfg() at boot). ONLY Tomba opts in. Because the default is 0,
 *   the modern path in pad_process_byte() below is byte-for-byte unchanged when
 *   the flag is off — no other title is affected. The `pad_cfg` debug command can
 *   also flip it live for A/B testing.
 *
 * THIS IS LEGACY — remove it once the behavioural mechanism is right. A real
 * DualShock tolerates unlimited analog-button presses (type changes) with no
 * disconnect, which proves the Hybrid flip CAN be benign under a correct state
 * machine for every game, with no per-game compatibility branch. When that
 * findpad/re-detect refactor lands, DELETE this whole legacy feature set — this
 * flag, the g_pad_legacy_cfg-gated branches in pad_process_byte(), the
 * legacy_pad_config config field, and the per-game game.toml opt-in — and let
 * Tomba ride the modern SM like everything else. */
volatile int g_pad_legacy_cfg = 0;
int sio_get_legacy_cfg(void) { return g_pad_legacy_cfg; }
void sio_set_legacy_cfg(int v) {
    g_pad_legacy_cfg = v ? 1 : 0;
    /* Clear any in-flight config latch so a mid-session toggle can't carry a
     * stale 0xF3/8-byte poll into the other mode's dispatch. */
    for (int s = 0; s < PSX_MAX_PLAYERS; s++)
        pad_in_config[s] = 0;
}

static void pad_process_byte(uint8_t tx_byte) {
    /* Apply any pending host type change (the emulated analog button) ONLY while
     * the bus is idle and the pad is not in config mode. This guarantees the
     * reported type (0x41/0x73) is stable for the whole of any poll or config
     * handshake — a hybrid stick/d-pad flip can never desync the game's driver
     * mid-transaction. A request raised during config stays pending until exit. */
    if (pad_state == PAD_IDLE) {
        for (int s = 0; s < PSX_MAX_PLAYERS; s++) {
            /* A game-LOCKED analog mode (0x44 ..03) ignores the physical analog
             * button — and our hybrid auto-flip IS that button — so a locked slot
             * drops the pending host request instead of applying it. */
            if (pad_type_req[s] >= 0 && !pad_in_config[s] && !analog_mode_locked[s]) {
                pad_analog[s] = (uint8_t)pad_type_req[s];
                pad_type_req[s] = -1;
            }
        }
    }
    switch (pad_state) {
    case PAD_IDLE:
        /* Standard address 01h selects Slot A (or the standalone pad). With a
         * multitap, 02h..04h select pads B–D on that port (psx-spx method 2). */
        if (tx_byte == 0x01 && pad_port_has_device(selected_slot)) {
            pad_active_logical = pad_logical_for_port(selected_slot);
            pad_mtap_addr = 0x01;
            pad_state = PAD_WAIT_ACCESS;
            sio_rx_data = 0xFF;
            sio_stat |= SIO_STAT_ACK;
        } else if (selected_is_mtap_port() && tx_byte >= 0x02 && tx_byte <= 0x04) {
            pad_active_logical = mtap_slot_a_logical() + (int)(tx_byte - 1);
            pad_mtap_addr = tx_byte;
            pad_state = PAD_WAIT_ACCESS;
            sio_rx_data = 0xFF;
            sio_stat |= SIO_STAT_ACK;
        } else {
            sio_rx_data = 0xFF;
        }
        break;

    case PAD_WAIT_ACCESS:
        pad_current_cmd = tx_byte;
        pad_response_idx = 1;
        if (selected_slot >= 0 && selected_slot <= 1)
            mtap_req_this[selected_slot] = 0;
        /* SCPH-1070 method 1: only when address was 01h AND a prior transfer
         * latched REQ=1. Otherwise Slot A (or garbage) — never force bulk on
         * every 0x42 (that breaks P1 when the tap is present with one pad). */
        if (selected_is_mtap_port() && pad_mtap_addr == 0x01 && tx_byte == 0x42 &&
            mtap_next_mode[selected_slot] == MTAP_NEXT_BULK) {
            const int base = mtap_slot_a_logical();
            pad_response[0] = 0x80;
            pad_response[1] = 0x5A;
            for (int i = 0; i < 4; i++)
                pad_fill_status8(base + i, &pad_response[2 + i * 8]);
            pad_response_len = PAD_RESPONSE_MAX;
            mtap_returned[selected_slot] = MTAP_NEXT_BULK;
            pad_state = PAD_SEND_RESPONSE;
            sio_rx_data = pad_response[0];
            sio_stat |= SIO_STAT_ACK;
            break;
        }
        if (selected_is_mtap_port() && pad_mtap_addr == 0x01 && tx_byte == 0x42 &&
            mtap_next_mode[selected_slot] == MTAP_NEXT_GARBAGE) {
            /* HiZ,80h,5Ah,LSB(Slot A id) then abort (psx-spx). */
            const int a = mtap_slot_a_logical();
            const uint8_t id = (!(pad_connected & (1u << a))) ? 0xFFu
                               : (pad_in_config[a] ? 0xF3u
                                  : (pad_analog[a] ? 0x73u : 0x41u));
            pad_response[0] = 0x80;
            pad_response[1] = 0x5A;
            pad_response[2] = id;
            pad_response_len = 3;
            mtap_returned[selected_slot] = MTAP_NEXT_GARBAGE;
            pad_state = PAD_SEND_RESPONSE;
            sio_rx_data = pad_response[0];
            sio_stat |= SIO_STAT_ACK;
            break;
        }
        if (selected_is_mtap_port() && pad_mtap_addr == 0x01 && tx_byte == 0x42)
            mtap_returned[selected_slot] = MTAP_NEXT_SLOT_A;
        /* Single-pad path (standalone port / Slot A / method-2 pad / non-0x42). */
        {
        const int lp = pad_active_logical;
        if (lp < 0 || lp >= PSX_MAX_PLAYERS || !(pad_connected & (1u << lp))) {
            /* No pad on this logical slot (e.g. empty multitap A during a
             * non-bulk command): hi-z, end transaction. */
            pad_state = PAD_IDLE;
            pad_response_len = 0;
            pad_response_idx = 0;
            pad_current_cmd = 0;
            sio_rx_data = 0xFF;
            break;
        }
        /* Controller ID reported as the first response byte. Real hardware
         * reports the config ID (0xF3) ONLY while in config mode; otherwise the
         * normal mode ID (0x41 digital / 0x73 analog). */
        const uint8_t cur_id = pad_in_config[lp] ? 0xF3
                               : (pad_analog[lp] ? 0x73 : 0x41);
        /* A plain digital controller (SCPH-1080) answers ONLY the 0x42 poll; it
         * ignores every config-mode command (returns hi-z, no ACK). A driver
         * that probes with 0x43 to detect a DualShock then classifies it as
         * digital-only and just polls. Gate all config branches on this so a
         * digital-mode pad behaves like real hardware (see pad_supports_config). */
        const int ds = pad_supports_config[lp];
        if (tx_byte == 0x42) {
            /* Read poll. Analog (or in-config) uses the 8-byte format with the
             * four stick axes; a plain digital pad uses the 4-byte format. */
            const uint16_t btn = pad_buttons[lp];
            pad_response[0] = cur_id;
            pad_response[1] = 0x5A;
            pad_response[2] = (uint8_t)(btn & 0xFF);
            pad_response[3] = (uint8_t)(btn >> 8);
            if (pad_analog[lp] || pad_in_config[lp]) {
                pad_response[4] = pad_stick[lp][2]; /* right X */
                pad_response[5] = pad_stick[lp][3]; /* right Y */
                pad_response[6] = pad_stick[lp][0]; /* left X */
                pad_response[7] = pad_stick[lp][1]; /* left Y */
                pad_response_len = 8;
            } else {
                pad_response_len = 4;
            }
            pad_state = PAD_SEND_RESPONSE;
            sio_rx_data = pad_response[0];
            sio_stat |= SIO_STAT_ACK;
        } else if (ds && tx_byte == 0x43) {
            /* Enter/exit config mode. The ID byte reflects the CURRENT mode; the
             * enter(0x01)/exit(0x00) flag is the second data byte, latched in
             * PAD_SEND_RESPONSE so it takes effect after this transaction. */
            const uint16_t btn = pad_buttons[lp];
            pad_response[1] = 0x5A;
            if (g_pad_legacy_cfg) {
                /* LEGACY (pre-98aa688): always config ID 0xF3, zero frame, no
                 * enter/exit tracking. */
                pad_response[0] = 0xF3;
                pad_response[2] = 0x00; pad_response[3] = 0x00;
                pad_response[4] = 0x00; pad_response[5] = 0x00;
                pad_response[6] = 0x00; pad_response[7] = 0x00;
                pad_response_len = 8;
            } else if (!pad_in_config[lp]) {
                /* ENTER attempt (normal mode): a real DualShock transmits the LIVE
                 * poll frame here — identical framing to 0x42 (dualshock.cpp:471-490)
                 * — and only latches config entry from the 0x01 data byte AFTERWARD.
                 * The old all-zero frame fed any driver that reads the 0x43 frame as
                 * input (most do; 0x43-with-data is a poll on the wire) a phantom
                 * "all pressed"/centered-stick garbage — the hybrid phantom-input
                 * mechanism (axis5_sio_controller.md D4). */
                pad_response[0] = cur_id;
                pad_response[2] = (uint8_t)(btn & 0xFF);
                pad_response[3] = (uint8_t)(btn >> 8);
                if (pad_analog[lp]) {
                    pad_response[4] = pad_stick[lp][2]; /* right X */
                    pad_response[5] = pad_stick[lp][3]; /* right Y */
                    pad_response[6] = pad_stick[lp][0]; /* left X */
                    pad_response[7] = pad_stick[lp][1]; /* left Y */
                    pad_response_len = 8;
                } else {
                    pad_response_len = 4;
                }
            } else {
                /* EXIT attempt (in config): config-mode 0x43 returns the zero frame
                 * (dualshock.cpp:660-674); cur_id is 0xF3 while in config. */
                pad_response[0] = cur_id;
                pad_response[2] = 0x00; pad_response[3] = 0x00;
                pad_response[4] = 0x00; pad_response[5] = 0x00;
                pad_response[6] = 0x00; pad_response[7] = 0x00;
                pad_response_len = 8;
            }
            pad_state = PAD_SEND_RESPONSE;
            sio_rx_data = pad_response[0];
            sio_stat |= SIO_STAT_ACK;
        } else if (ds && g_pad_legacy_cfg &&
                   (tx_byte == 0x45 || tx_byte == 0x46 || tx_byte == 0x47 ||
                    tx_byte == 0x4C || tx_byte == 0x4D)) {
            /* LEGACY config answers (pre-98aa688): canned 0xF3 responses given
             * UNCONDITIONALLY (no config-mode gating). 0x44/0x4F had no handler
             * then, so they fall through to the hi-z "no response" else below. */
            static const uint8_t r_45[8] = { 0xF3,0x5A,0x03,0x02,0x01,0x02,0x01,0x00 };
            static const uint8_t r_46[8] = { 0xF3,0x5A,0x00,0x00,0x01,0x02,0x00,0x0A };
            static const uint8_t r_47[8] = { 0xF3,0x5A,0x00,0x00,0x02,0x00,0x01,0x00 };
            static const uint8_t r_4c[8] = { 0xF3,0x5A,0x00,0x00,0x00,0x04,0x00,0x00 };
            static const uint8_t r_4d[8] = { 0xF3,0x5A,0x00,0x00,0x00,0x00,0x00,0x00 };
            const uint8_t *r = r_4d;
            if      (tx_byte == 0x45) r = r_45;
            else if (tx_byte == 0x46) r = r_46;
            else if (tx_byte == 0x47) r = r_47;
            else if (tx_byte == 0x4C) r = r_4c;
            memcpy(pad_response, r, 8);
            /* 0x45 status byte must report the LIVE analog mode, not a fixed
             * analog-on (dualshock.cpp:743) — see fix below for the modern path. */
            if (tx_byte == 0x45)
                pad_response[3] = pad_analog[lp] ? 0x01 : 0x00;
            pad_response_len = 8;
            pad_state = PAD_SEND_RESPONSE;
            sio_rx_data = pad_response[0];
            sio_stat |= SIO_STAT_ACK;
        } else if (ds && !g_pad_legacy_cfg && pad_in_config[lp] &&
                   (tx_byte == 0x44 || tx_byte == 0x45 || tx_byte == 0x46 ||
                    tx_byte == 0x47 || tx_byte == 0x4C || tx_byte == 0x4D ||
                    tx_byte == 0x4F)) {
            /* Config-only commands (analog/rumble init handshake). Canned
             * responses; only valid while in config mode (a digital pad ignores
             * them, handled by the else branch below). */
            static const uint8_t r_def[8] = { 0xF3,0x5A,0x00,0x00,0x00,0x00,0x00,0x00 };
            static const uint8_t r_45[8]  = { 0xF3,0x5A,0x03,0x02,0x01,0x02,0x01,0x00 };
            static const uint8_t r_46[8]  = { 0xF3,0x5A,0x00,0x00,0x01,0x02,0x00,0x0A };
            static const uint8_t r_47[8]  = { 0xF3,0x5A,0x00,0x00,0x02,0x00,0x01,0x00 };
            static const uint8_t r_4c[8]  = { 0xF3,0x5A,0x00,0x00,0x00,0x04,0x00,0x00 };
            const uint8_t *r = r_def;
            if      (tx_byte == 0x45) r = r_45;
            else if (tx_byte == 0x46) r = r_46;
            else if (tx_byte == 0x47) r = r_47;
            else if (tx_byte == 0x4C) r = r_4c;
            memcpy(pad_response, r, 8);
            /* 0x45 reports the analog-status byte: a driver polling 0x45 to learn
             * the live mode must read the CURRENT analog state, not a hard-coded
             * analog-on (dualshock.cpp:743 transmit_buffer[1]=analog_mode?1:0).
             * Reporting "analog" while we present digital (or vice-versa) makes the
             * driver mis-parse the poll frame length → off-by-frame garbage buttons
             * (axis5_sio_controller.md D8). */
            if (tx_byte == 0x45)
                pad_response[3] = pad_analog[lp] ? 0x01 : 0x00;
            /* 0x4D returns the previous six-byte motor map while latching the
             * replacement bytes later in this same transaction. */
            if (tx_byte == 0x4D)
                memcpy(&pad_response[2], pad_rumble_map[lp], 6);
            pad_response_len = 8;
            pad_state = PAD_SEND_RESPONSE;
            sio_rx_data = pad_response[0];
            sio_stat |= SIO_STAT_ACK;
        } else {
            /* Unknown command, or a config command issued OUTSIDE config mode:
             * a real pad does not respond — return hi-z and end the transaction. */
            pad_state = PAD_IDLE;
            pad_response_len = 0;
            pad_response_idx = 0;
            pad_current_cmd = 0;
            sio_rx_data = 0xFF;
        }
        }
        break;

    case PAD_SEND_RESPONSE:
        /* TAP/REQ (third command byte, paired with idhi/5Ah at idx==1): does not
         * change *this* response; it arms the next 0x42 on the multitap port. */
        if (selected_is_mtap_port() && pad_current_cmd == 0x42 &&
            pad_mtap_addr == 0x01 && pad_response_idx == 1 &&
            selected_slot >= 0 && selected_slot <= 1)
            mtap_req_this[selected_slot] = (tx_byte == 0x01) ? 1 : 0;
        /* DualShock rumble: index by the active logical pad (multitap-aware). */
        {
            const int rs = (pad_active_logical >= 0 && pad_active_logical < PSX_MAX_PLAYERS)
                               ? pad_active_logical
                               : selected_slot;
            if (rs >= 0 && rs < PSX_MAX_PLAYERS) {
                /* The six data bytes after 0x42's leading 0x00 occupy response
                 * indexes 2..7. Route each through the map negotiated by 0x4D. */
                if (pad_current_cmd == 0x42 &&
                    pad_response_idx >= 2 && pad_response_idx < 8) {
                    const unsigned map_index = (unsigned)pad_response_idx - 2u;
                    const uint8_t motor = pad_rumble_map[rs][map_index];
                    if (motor == 0x00)
                        pad_rumble_small[rs] = tx_byte;
                    else if (motor == 0x01)
                        pad_rumble_large[rs] = tx_byte;
                }
                /* 0x4D uses the same six wire positions. pad_response[] was
                 * populated with the old values before the transaction, so
                 * updating the live map here preserves echo-before-write. */
                if (pad_current_cmd == 0x4D &&
                    pad_response_idx >= 2 && pad_response_idx < 8) {
                    const unsigned map_index = (unsigned)pad_response_idx - 2u;
                    pad_rumble_map[rs][map_index] = tx_byte;
                }
            }
        }
        /* For 0x43 (enter/exit config), the data byte selecting enter(0x01)/
         * exit(0x00) arrives paired with response index 2. Latch the new config
         * state; it takes effect from the next transaction (the ID byte already
         * reported the mode that was current at the start of this one). */
        if (!g_pad_legacy_cfg && pad_current_cmd == 0x43 && pad_response_idx == 2 &&
            pad_active_logical >= 0 && pad_active_logical < PSX_MAX_PLAYERS)
            pad_in_config[pad_active_logical] = (tx_byte == 0x01) ? 1 : 0;
        /* 0x44 set-mode (game owns the analog/digital mode): the mode byte rides
         * in the same slot as 0x43's enter/exit flag (data position 3). 0x01 =>
         * analog (0x73), 0x00 => digital (0x41). Honouring it makes the pad
         * coherent — the type the game just selected is the type it then polls,
         * instead of the host hybrid silently winning. Drop any stale host
         * request so it can't immediately undo the game's choice. */
        if (!g_pad_legacy_cfg && pad_current_cmd == 0x44 && pad_response_idx == 2 &&
            pad_active_logical >= 0 && pad_active_logical < PSX_MAX_PLAYERS) {
            pad_analog[pad_active_logical] = (tx_byte == 0x01) ? 1 : 0;
            pad_type_req[pad_active_logical] = -1;
        }
        /* 0x44 lock byte (data position 4, the byte after the mode byte): 0x03 =>
         * lock analog mode, 0x02 => unlock (dualshock.cpp:714-725). A locked slot
         * ignores the host hybrid auto-flip (see analog_mode_locked). */
        if (!g_pad_legacy_cfg && pad_current_cmd == 0x44 && pad_response_idx == 3 &&
            pad_active_logical >= 0 && pad_active_logical < PSX_MAX_PLAYERS) {
            if      (tx_byte == 0x03) analog_mode_locked[pad_active_logical] = 1;
            else if (tx_byte == 0x02) analog_mode_locked[pad_active_logical] = 0;
        }
        if (pad_response_idx < pad_response_len) {
            sio_rx_data = pad_response[pad_response_idx++];
            if (pad_response_idx < pad_response_len) {
                sio_stat |= SIO_STAT_ACK;
            } else {
                mtap_finish_42();
                pad_state = PAD_IDLE;
                pad_response_len = 0;
                pad_response_idx = 0;
                pad_current_cmd = 0;
            }
        } else {
            mtap_finish_42();
            pad_state = PAD_IDLE;
            pad_response_len = 0;
            pad_response_idx = 0;
            pad_current_cmd = 0;
            sio_rx_data = 0xFF;
        }
        break;

    default:
        pad_state = PAD_IDLE;
        pad_response_len = 0;
        pad_response_idx = 0;
        pad_current_cmd = 0;
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
            sio_rx_data = 0x00;
        } else {
            mc_data_idx = 0;
            mc_state = MC_WRITE_LSB_ECHO;
            /* Hardware/Beetle echo the high address byte on the address-LSB
             * transfer (zero for all 1Mbit card sectors), then echo the low
             * address byte while receiving the first payload byte.  Some BIOS
             * card-write paths validate this handshake before they schedule the
             * follow-up directory reads. */
            sio_rx_data = mc_sector_msb;
        }
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

    case MC_READ_END: {
        mc_state = MC_IDLE;
        sio_mc_read_done++;
        extern void card_read_summary_record(uint8_t slot, uint8_t cmd,
                                             uint16_t sector,
                                             uint8_t checksum,
                                             uint8_t data_idx,
                                             const uint8_t *data128);
        card_read_summary_record(mc_slot, mc_cmd, mc_sector,
                                 mc_checksum, mc_data_idx, mc_data);
        if (mc_sector < MEMCARD_SECTORS) {
            sio_rx_data = 0x47; /* 'G' = Good */
        } else {
            sio_rx_data = 0xFF;
        }
        break;
    }

    /* ---- WRITE states ---- */
    case MC_WRITE_LSB_ECHO:
        mc_data[mc_data_idx++] = tx_byte;
        mc_state = MC_WRITE_DATA;
        sio_rx_data = mc_sector_lsb;
        sio_stat |= SIO_STAT_ACK;
        break;

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
        uint16_t sector_now = (mc_state >= MC_READ_ACK1 || mc_state == MC_WRITE_LSB_ECHO
                               || mc_state == MC_WRITE_DATA
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
    sio_debug_poll_maybe();

    /* Advance delayed IRQ on every SIO register access.
     * In v4, recompiled functions run as native C without per-instruction
     * stepping. sio_tick() from the dispatch loop won't advance during
     * a tight BIOS polling loop within a single function. Advancing on
     * register access ensures the IRQ fires in time for the BIOS's
     * "clear, delay, check" card detection sequence. */
    sio_tick(0);

    switch (addr) {
    case 0x1F801040: /* SIO_RX_DATA */ {
        uint8_t b = sio_rx_data;
        sio_stat &= ~SIO_STAT_RX_RDY;
        sr_record(SR_EVT_RX_DATA_READ, 0, b);
        if (active_device == DEV_MEMCARD &&
            mc_state >= MC_READ_DATA && mc_state <= MC_READ_END) {
            extern void card_data_writes_arm(uint8_t value,
                                             uint16_t mc_state,
                                             uint8_t mc_data_idx,
                                             uint8_t slot);
            card_data_writes_arm(b, (uint16_t)mc_state,
                                 mc_data_idx, (uint8_t)mc_slot);
        }
        return b;
    }

    case 0x1F801044: { /* SIO_STAT — record only on value transitions */
        static uint16_t s_last_stat_observed = 0xFFFF;
        uint16_t observed = sio_stat;
        if (sio_stat != s_last_stat_observed) {
            s_last_stat_observed = sio_stat;
            sr_record(SR_EVT_STAT_READ, 0, 0);
        }
        if (sio_ack_visible_reads > 0 && --sio_ack_visible_reads == 0) {
            sio_stat &= ~SIO_STAT_ACK;
        }
        return observed;
    }

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

/* Side-effect-FREE SIO register peeks for the debug/observability path ONLY.
 * sio_read() deliberately has guest-visible side effects (sio_tick advances the
 * delayed-IRQ state machine; reading SIO_STAT decrements sio_ack_visible_reads
 * and clears the ACK bit; reading SIO_RX_DATA pops the RX FIFO / clears RX_RDY).
 * Those are correct for the GUEST bus, but the TCP debug server reading SIO every
 * vblank (record_frame) or per query (sio_state) MUST NOT perturb the SIO/pad
 * handshake — that is the observer corrupting the observed, and it desynced the
 * DualShock handshake the Mega Man X engine depends on (dev builds skipped the
 * boot FMV; release, which never records frames, played it). Read raw state. */
uint16_t sio_peek_stat(void) { return sio_stat; }
uint16_t sio_peek_ctrl(void) { return sio_ctrl; }
uint8_t  sio_peek_rx_data(void) { return sio_rx_data; }

void sio_write(uint32_t addr, uint32_t value) {
    sio_debug_poll_maybe();

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
        {
            uint8_t hb = (uint8_t)value;
            /* Arm handoff on any card select so probe ACKs are visible too. */
            if (hb == 0x81 && (sio_ctrl & SIO_CTRL_SELECT))
                s_card_handoff_armed = 1;
            if (s_card_handoff_armed &&
                (hb == 0x81 || hb == 0x52 || hb == 0x57 || hb == 0x53))
                card_handoff_push(4, hb);
        }
        if (!(sio_ctrl & SIO_CTRL_TX_EN)) {
            sio_tx_gated++;
            sr_record(SR_EVT_TX_DATA_WRITE, (uint8_t)value, 0);
            break;
        }
#if SIO_MODEL_CYCLE_PACED
        /* Cycle-paced TX. Pad and card share single bus. */
        {
            uint8_t b = (uint8_t)value;
            /* FAITHFUL pad RX pacing (fixes dead pads under OpenBIOS). The pad
             * "fast path" below processes the byte SYNCHRONOUSLY at the TX
             * write: RX_RDY asserts instantly, while the ACK IRQ is scheduled
             * BAUD+ACK (1088+170) cycles out. Real hardware asserts RX_RDY only
             * when the byte has SHIFTED (~1088 cycles); the /ACK pulse lands
             * ~170 cycles after that. A driver that waits on RXRDY (absorbing
             * the shift time) and then spins a SHORT bounded loop on I_STAT.7 —
             * OpenBIOS readPad's 0x50-iteration wait — starts that spin ~1000
             * cycles early against the synchronous model and TIMES OUT: every
             * poll aborts with padBuffer[0]=0xFF and the game sees a dead pad
             * (byte 1 survived only by its extra busyloops; byte 2 aborted —
             * sio_trace showed exactly 0x01,0x42 then abandon, one ack/poll).
             * Default: route pads through the same cycle-paced shifter the
             * memcard uses (RX_RDY at shift-complete, ack +170 after). Total
             * ack-IRQ latency is UNCHANGED (1088+170), so the SCPH1001
             * pad-detect phase the constants were tuned for is preserved.
             * A/B: PSX_SIO_PAD_SYNC_RX=1 restores the legacy synchronous path. */
            static int s_pad_sync_rx = -1;
            if (s_pad_sync_rx < 0) {
                const char *e = getenv("PSX_SIO_PAD_SYNC_RX");
                s_pad_sync_rx = (e && e[0] == '1') ? 1 : 0;
            }
            int pad_fast_path = s_pad_sync_rx &&
                active_device != DEV_MEMCARD &&
                (active_device == DEV_PAD ||
                 sio_bus_owner == SIO_OWNER_PAD ||
                 b == 0x01);

            if (pad_fast_path) {
                uint16_t arm_dbg_ctrl_pre = sio_ctrl;
                uint16_t arm_dbg_stat_pre = sio_stat;
                sio_shift_active = 0;
                sio_shift_remaining = 0;
                sio_tx_buffered = 0;
                sio_shift_ack_irq_en = 0;
                sio_tx_buffer_ack_irq_en = 0;
                sio_pending_ack = 0;
                sio_ack_remaining = 0;
                sio_pending_ack_irq_en = 0;
                sio_bus_owner = SIO_OWNER_PAD;
                sio_bus_byte_index++;
                g_sio_timing_active = 0;

                sr_record(SR_EVT_TX_DATA_WRITE, b, 0);
                sio_process_byte(b);
                uint8_t arm_dbg_dev_at_decision = (uint8_t)active_device;
                int armed_now = 0;
                if ((sio_stat & SIO_STAT_ACK) && (sio_ctrl & SIO_CTRL_ACK_IRQ_EN)) {
                    sio_stat &= ~(SIO_STAT_ACK | SIO_STAT_TX_RDY | SIO_STAT_TX_EMPTY);
                    /* FAITHFUL pad ACK timing (faithful-core, replaces the
                     * access-paced sio_irq_countdown=SIO_IRQ_DELAY_PAD hack).
                     * On real hardware the controller's DSR/ACK pulse arrives
                     * ~one byte-shift (baud) + ack-pulse after the TX byte —
                     * a GUEST-CYCLE delay, independent of how many SIO register
                     * reads the CPU manages to issue while it waits. The old
                     * countdown decremented once per SIO register access
                     * (sio_tick(0)), so a faithful/faster CPU (load=4) fired the
                     * pad IRQ at the wrong guest-cycle phase relative to the
                     * cycle-paced timers/VBLANK, diverging the BIOS pad-detect
                     * state machine (load=4 Tomba2 boot wedge). Route through the
                     * same cycle-paced ack scheduler the card path uses, driven
                     * by sio_advance() from psx_advance_cycles(). */
                    sio_pending_ack        = 1;
                    sio_ack_remaining      = SIO_BAUD_CYCLES_DEFAULT + SIO_ACK_CYCLES_DEFAULT;
                    sio_pending_ack_irq_en = 1;
                    g_sio_timing_active    = 1;
                    event_ring_record_aux(EV_ENQ, (uint8_t)SRC_SIO, (uint32_t)sio_ack_remaining);
                    sio_irq_pending_source = SIO_IRQ_SRC_PAD_ACK;
                    sio_irq_pending_slot = (uint8_t)selected_slot;
                    sio_irq_pending_delay = (uint8_t)SIO_ACK_CYCLES_DEFAULT;
                    sio_irq_pending_mc_state = (uint8_t)mc_state;
                    sio_irq_pending_byte_seq = sio_trace_seq;
                    armed_now = 1;
                } else {
                    sio_stat |= SIO_STAT_TX_RDY | SIO_STAT_TX_EMPTY;
                }
                sio_card_arm_audit_record(arm_dbg_dev_at_decision, arm_dbg_ctrl_pre,
                                          arm_dbg_stat_pre, sio_stat,
                                          armed_now, sio_irq_countdown);
                break;
            }

            sr_record(SR_EVT_TX_DATA_WRITE, b, 0);
            if (sio_bus_byte_index == 0) {
                sio_bus_owner = (b == 0x81) ? SIO_OWNER_CARD
                              : (b == 0x01) ? SIO_OWNER_PAD
                              :               SIO_OWNER_UNKNOWN;
            }
            sio_bus_byte_index++;
            if (!sio_shift_active) {
                sio_shift_byte      = b;
                sio_shift_active    = 1;
                sio_shift_remaining = SIO_BAUD_CYCLES_DEFAULT;
                sio_shift_ack_irq_en = (sio_ctrl & SIO_CTRL_ACK_IRQ_EN) ? 1 : 0;
                sio_stat &= ~(SIO_STAT_TX_RDY | SIO_STAT_TX_EMPTY);
                g_sio_timing_active = 1;
                sr_record(SR_EVT_SHIFT_START, b, 0);
            } else if (!sio_tx_buffered) {
                sio_tx_buffer  = b;
                sio_tx_buffered = 1;
                sio_tx_buffer_ack_irq_en = (sio_ctrl & SIO_CTRL_ACK_IRQ_EN) ? 1 : 0;
                sio_stat &= ~SIO_STAT_TX_EMPTY;
                s_pace_tx_writes_buffered++;
                sr_record(SR_EVT_BUFFER_LOAD, b, 0);
            } else {
                s_pace_tx_writes_dropped_busy++;
                sr_record(SR_EVT_TX_DROPPED, b, 0);
            }
        }
#else
        {
            uint16_t arm_dbg_ctrl_pre  = sio_ctrl;
            uint16_t arm_dbg_stat_pre  = sio_stat;
            sio_process_byte(sio_tx_data);
            uint8_t  arm_dbg_dev_at_decision = (uint8_t)active_device;
            sio_stat &= ~(SIO_STAT_TX_RDY | SIO_STAT_TX_EMPTY);
            extern void sio_card_arm_audit_record(int dev, uint16_t ctrl_pre,
                                                  uint16_t stat_pre, uint16_t stat_post,
                                                  int armed, int countdown_after);
            int armed_now = 0;
            if ((sio_stat & SIO_STAT_ACK) && (sio_ctrl & SIO_CTRL_ACK_IRQ_EN)) {
                sio_stat &= ~SIO_STAT_ACK;
                sio_irq_pending = 1;
                sio_irq_countdown = (active_device == DEV_MEMCARD)
                    ? SIO_IRQ_DELAY_CARD : SIO_IRQ_DELAY_PAD;
                event_ring_record_aux(EV_ENQ, (uint8_t)SRC_SIO, (uint32_t)sio_irq_countdown);
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
#endif
        break;

    case 0x1F801048: /* SIO_MODE */
        sio_mode = (uint16_t)value;
        sr_record(SR_EVT_MODE_WRITE, (uint8_t)(value & 0xFF), (uint8_t)((value >> 8) & 0xFF));
        break;

    case 0x1F80104A: /* SIO_CTRL */ {
        uint16_t old_ctrl = sio_ctrl;
        sio_ctrl = (uint16_t)value;
        sr_record(SR_EVT_CTRL_WRITE, (uint8_t)(value & 0xFF), (uint8_t)((value >> 8) & 0xFF));
        if (value & SIO_CTRL_ACK) {
            sio_stat &= ~SIO_STAT_IRQ;
            sio_stat &= ~SIO_STAT_ACK;
            sio_ack_visible_reads = 0;
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
            pad_response_len = 0;
            pad_response_idx = 0;
            pad_current_cmd = 0;
            mc_state = MC_IDLE;
            mc_slots[0].state = MC_IDLE;
            mc_slots[1].state = MC_IDLE;
            active_device = DEV_NONE;
            sio_irq_pending = 0;
            sio_irq_countdown = 0;
            sio_ack_visible_reads = 0;
#if SIO_MODEL_CYCLE_PACED
            sio_shift_active = 0; sio_shift_remaining = 0;
            sio_tx_buffered = 0;
            sio_shift_ack_irq_en = 0; sio_tx_buffer_ack_irq_en = 0;
            sio_pending_ack = 0; sio_ack_remaining = 0;
            sio_pending_ack_irq_en = 0;
            sio_bus_owner = SIO_OWNER_NONE; sio_bus_byte_index = 0;
            g_sio_timing_active = 0;
#endif
            sr_record(SR_EVT_RESET, 0, 0);
        }
#if SIO_MODEL_CYCLE_PACED
        if (!(old_ctrl & SIO_CTRL_SELECT) && (value & SIO_CTRL_SELECT)) {
            sio_bus_owner = SIO_OWNER_NONE;
            sio_bus_byte_index = 0;
            sr_record(SR_EVT_SELECT_ASSERT, 0, 0);
        }
        /* TX_EN 1→0 transition kills any in-flight shifter the same
         * way SELECT-deassert does. Restore TX_RDY/TX_EMPTY so the
         * SIO returns to an idle status word, matching the recovery
         * the SELECT-deassert path now performs. */
        if ((old_ctrl & SIO_CTRL_TX_EN) && !(value & SIO_CTRL_TX_EN)) {
            if (sio_shift_active || sio_tx_buffered || sio_pending_ack) {
                sio_shift_active = 0; sio_shift_remaining = 0;
                sio_tx_buffered = 0;
                sio_shift_ack_irq_en = 0; sio_tx_buffer_ack_irq_en = 0;
                sio_pending_ack = 0; sio_ack_remaining = 0;
                sio_pending_ack_irq_en = 0;
                if (!(value & SIO_CTRL_SELECT)) {
                    sio_bus_owner = SIO_OWNER_NONE;
                    sio_bus_byte_index = 0;
                }
                g_sio_timing_active = 0;
                sio_stat |= SIO_STAT_TX_RDY | SIO_STAT_TX_EMPTY;
            }
        }
#endif
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
#if SIO_MODEL_CYCLE_PACED
            /* Finish a card byte that already shifted before killing the bus.
             * Cancelling a pending ACK here drops an IntRP pop and leaves
             * libcard A6C10 nested so B4E38 never arms the next transfer. */
            int preserve_card_ack = 0;
            int preserve_ack_rem = 0;
            if (active_device == DEV_MEMCARD) {
                if (sio_shift_active && sio_shift_remaining <= 0)
                    sio_handle_shift_complete();
                if (sio_pending_ack) {
                    sio_pending_ack = 0;
                    sio_ack_remaining = 0;
                    if (s_card_handoff_armed)
                        card_handoff_push(7, 0); /* select_flush_ack */
                    sio_fire_ack_irq();
                    /* fire may re-queue while I_STAT.7 is still set — keep it
                     * across the protocol reset below. */
                    if (sio_pending_ack) {
                        preserve_card_ack = 1;
                        preserve_ack_rem = sio_ack_remaining;
                    }
                }
            }
#endif
            if (active_device == DEV_MEMCARD && mc_state != MC_IDLE && sio_txn_open) {
                extern uint32_t g_debug_current_func_addr;
                txn_close(SIO_TXN_END_ABORT_OTHER, mc_state, g_debug_current_func_addr);
            }
            /* Persist mc_flag back to the active slot before resetting. mc_flag
             * was loaded from mc_slots[mc_slot].flag on the 0x81 select and
             * cleared to 0x00 in MC_CMD on the first 0x52/0x57/0x53. Without
             * writing it back here, every reselect re-loads the stale 0x08
             * "new card" value, and the BIOS aborts every multi-sector read
             * after the first one (sees FLAG=0x08 → "card was just inserted,
             * re-init" → 2-byte 0x81-0x52 abort). */
            if (active_device == DEV_MEMCARD && mc_slot >= 0 && mc_slot <= 1) {
                mc_slots[mc_slot].flag = mc_flag;
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
            pad_response_len = 0;
            pad_response_idx = 0;
            pad_current_cmd = 0;
            active_device = DEV_NONE;
#if SIO_MODEL_CYCLE_PACED
            sio_shift_active = 0; sio_shift_remaining = 0;
            sio_tx_buffered = 0;
            sio_shift_ack_irq_en = 0; sio_tx_buffer_ack_irq_en = 0;
            sio_pending_ack = 0; sio_ack_remaining = 0;
            sio_pending_ack_irq_en = 0;
            sio_bus_owner = SIO_OWNER_NONE; sio_bus_byte_index = 0;
            g_sio_timing_active = 0;
            if (preserve_card_ack) {
                sio_pending_ack = 1;
                sio_ack_remaining = preserve_ack_rem > 0 ? preserve_ack_rem : 16;
                sio_pending_ack_irq_en = 1;
                sio_irq_pending_source = SIO_IRQ_SRC_CARD_ACK;
                g_sio_timing_active = 1;
            }
            /* Killing an in-flight shifter mid-cycle leaves sio_stat
             * with TX_RDY/TX_EMPTY clear (they were masked by
             * SHIFT_START and only sio_handle_shift_complete restores
             * them). Restore here so the next handler sees an idle
             * SIO instead of busy-waiting forever on TX_RDY=0. */
            sio_stat |= SIO_STAT_TX_RDY | SIO_STAT_TX_EMPTY;
#endif
            /* Protocol is idle now — finish libcard B4E38 if still nested. */
            ape_card_unstick_maybe(1);
            sr_record(SR_EVT_SELECT_DEASS, 0, 0);
        }
        break;
    }

    case 0x1F80104E: /* SIO_BAUD */
        sio_baud = (uint16_t)value;
        sr_record(SR_EVT_BAUD_WRITE, (uint8_t)(value & 0xFF), (uint8_t)((value >> 8) & 0xFF));
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

/* Phase 1.0c-v2: flip default to 1, but gate the dispatch-loop quantum
 * tick by g_sio_timing_active so the per-call cost on the
 * psx_check_interrupts hot path is one load + one branch when no SIO
 * cycle-paced work is pending. TX path remains synchronous in 1.0c-v2
 * — nothing sets g_sio_timing_active, so the gate stays 0 and
 * sio_tick_quantum is never invoked. Phase 1.0d will reroute TX into
 * the shifter and arm the guard.
 *
 * Macro=0 still available to revert to legacy entirely. */
#ifndef SIO_MODEL_CYCLE_PACED
#define SIO_MODEL_CYCLE_PACED 1
#endif

/* Cycle-paced SIO state moved above sio_write (after forward decls). */

#if SIO_MODEL_CYCLE_PACED
/* ---- Phase 1.0c-v2: cycle-paced sio_tick helpers (inert in 1.0c-v2) ---
 *
 * In 1.0c-v2, the TX path is still synchronous and nothing arms
 * sio_shift_active or sio_pending_ack. Therefore these helpers exist
 * but are never reached at runtime: sio_tick_quantum is gated by
 * g_sio_timing_active (which stays 0), and even if called the cycle-
 * paced loop in sio_tick would find no events to fire. */
static int sio_consume_ack_event(void) {
    int acked = (sio_stat & SIO_STAT_ACK) ? 1 : 0;
    sio_stat &= ~SIO_STAT_ACK;
    return acked;
}

static void sio_fire_ack_irq(void) {
    /* Card: drop sticky unmasked SPU (I_STAT bit 9) before raising SIO.
     * Tip's LOAD probe ACKs otherwise land on i_stat_before 0x200 while
     * master sees 0x000 — guest-visible divergence.
     * EXPERIMENT: was offline-only; ungated for TM4 netplay test. */
    int card_ack = (sio_irq_pending_source == SIO_IRQ_SRC_CARD_ACK ||
                    active_device == DEV_MEMCARD);
    if (card_ack)
        i_stat &= ~(1u << 9);

    sio_stat |= SIO_STAT_ACK;
    sio_ack_visible_reads = 2;
    sr_record(SR_EVT_ACK_FIRE, 0, 0);
    int irq_enabled = sio_pending_ack_irq_en
                   || ((sio_ctrl & SIO_CTRL_ACK_IRQ_EN) ? 1 : 0);
    if (!irq_enabled) {
        sio_pending_ack_irq_en = 0;
        return;
    }

    /* IMPORTANT (Ape Escape): serialize card ACK IRQs. If I_STAT.7 is still
     * set, raising again merges into the same bit — LibCardIntRP pops once
     * → A6C10 stays nested → B4E38 never set → LOAD wedges after the
     * presence probe. Re-queue until the guest clears bit7.
     * EXPERIMENT: was offline-only; ungated for TM4 netplay test. */
    if (card_ack && (i_stat & 0x80u)) {
        sio_pending_ack = 1;
        sio_ack_remaining = 16;
        sio_pending_ack_irq_en = 1;
        g_sio_timing_active = 1;
        if (s_card_handoff_armed)
            card_handoff_push(8, 0); /* ack_deferred_istat7 */
        return;
    }
    sio_pending_ack_irq_en = 0;
    sio_stat |= SIO_STAT_IRQ;

    uint32_t i_stat_before = i_stat;
    psx_irq_raise(IRQ_SIO0, 0); /* SIO ACK IRQ */
    event_ring_record_aux(EV_DEQ, (uint8_t)SRC_SIO, 0u); /* SIO ACK IRQ fired */
    if (card_ack) {
        sio_arm_card_ct_defer_guard();
        if (s_card_handoff_armed)
            card_handoff_push(5, 0);
    }

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
    s_pace_ack_fires++;
    if (!sio_shift_active && !sio_tx_buffered && !sio_pending_ack) {
        g_sio_timing_active = 0;
    }
}

static void sio_handle_shift_complete(void) {
    uint8_t b = sio_shift_byte;
    sr_record(SR_EVT_SHIFT_DONE, b, 0);
    if (b == 0x01 && mc_state >= MC_READ_DATA && mc_state <= MC_READ_END) {
        s_pace_pad_byte_processed_in_card_data++;
    }
    sio_process_byte(b);
    int acked = sio_consume_ack_event();
    sio_shift_active = 0;
    sio_stat |= SIO_STAT_RX_RDY;
    s_pace_shift_completes++;

    sio_irq_pending_source   = (active_device == DEV_MEMCARD)
                               ? SIO_IRQ_SRC_CARD_ACK : SIO_IRQ_SRC_PAD_ACK;
    sio_irq_pending_slot     = (uint8_t)selected_slot;
    sio_irq_pending_delay    = (uint8_t)SIO_ACK_CYCLES_DEFAULT;
    sio_irq_pending_mc_state = (uint8_t)mc_state;
    sio_irq_pending_byte_seq = sio_trace_seq;

    if (acked) {
        sio_pending_ack   = 1;
        sio_ack_remaining = SIO_ACK_CYCLES_DEFAULT;
        sio_pending_ack_irq_en = sio_shift_ack_irq_en;
    }

    if (sio_tx_buffered) {
        sio_shift_byte      = sio_tx_buffer;
        sio_shift_active    = 1;
        sio_shift_remaining = SIO_BAUD_CYCLES_DEFAULT;
        sio_shift_ack_irq_en = sio_tx_buffer_ack_irq_en;
        sio_tx_buffered     = 0;
        sio_tx_buffer_ack_irq_en = 0;
        sio_stat |= SIO_STAT_TX_EMPTY;
        sio_bus_byte_index++;
        s_pace_tx_buffer_promoted++;
        if (sio_bus_owner == SIO_OWNER_CARD)
            s_pace_tx_buffer_promoted_during_card++;
    } else {
        sio_stat |= SIO_STAT_TX_RDY | SIO_STAT_TX_EMPTY;
    }
    if (!sio_shift_active && !sio_tx_buffered && !sio_pending_ack) {
        g_sio_timing_active = 0;
    }
}

/* Phase 1.0e-e1: peripheral-only advance.
 *
 * Drives the cycle-paced shifter+ack scheduler ONLY. Does NOT touch the
 * legacy sio_irq_pending/sio_irq_countdown — that's the unconditional-
 * decrement that broke pad timing when called from psx_advance_cycles
 * in earlier slices. Until the TX path is rerouted (1.0e-e2), nothing
 * arms the shifter, so this body returns immediately at the
 * g_sio_timing_active==0 gate without doing any work. */
static uint64_t s_sio_advance_called    = 0;
static uint64_t s_sio_advance_with_work = 0;

uint64_t sio_get_advance_called(void)    { return s_sio_advance_called; }
uint64_t sio_get_advance_with_work(void) { return s_sio_advance_with_work; }

/* Walk shift/ack deadlines for `cycles`.
 *
 * IMPORTANT (Ape Escape LOAD): MAX_TRANSITIONS=1 so each walk delivers at
 * most one shift/ack edge. Uncapped walks can fire shift-complete + ACK
 * pairs back-to-back under the card ISR; the next ACK merges into the same
 * I_STAT.7 edge and LibCardIntRP pops once for two bytes → nest stuck after
 * 81 52 00. Do not fake-collapse A6C10 to compensate.
 * EXPERIMENT: was uncapped under netplay/selfcheck (MotK leftover-time);
 * always MT=1 here for TM4 netplay desync testing. */
static void sio_pace_walk(int cycles) {
    int remaining = cycles;
    int transitions = 0;
    const int MAX_TRANSITIONS = 1;
    while (transitions < MAX_TRANSITIONS &&
           (remaining > 0 ||
            (sio_shift_active && sio_shift_remaining <= 0) ||
            (sio_pending_ack && sio_ack_remaining <= 0))) {
        int dt = remaining;
        int next_event = -1;
        if (sio_shift_active && sio_shift_remaining > 0
            && sio_shift_remaining <= dt) {
            dt = sio_shift_remaining;
            next_event = 0;
        }
        if (sio_pending_ack && sio_ack_remaining > 0
            && (sio_ack_remaining < dt ||
                (sio_ack_remaining == dt && next_event < 0))) {
            dt = sio_ack_remaining;
            next_event = 1;
        }
        if (sio_shift_active && sio_shift_remaining <= 0) {
            dt = 0; next_event = 0;
        } else if (sio_pending_ack && sio_ack_remaining <= 0) {
            dt = 0; next_event = 1;
        }
        if (sio_shift_active) sio_shift_remaining -= dt;
        if (sio_pending_ack)  sio_ack_remaining   -= dt;
        remaining -= dt;
        if (next_event == 0) {
            sio_handle_shift_complete();
            transitions++;
        } else if (next_event == 1) {
            sio_pending_ack = 0;
            sio_fire_ack_irq();
            transitions++;
        } else {
            break;
        }
    }
}

void sio_advance(uint32_t cycles) {
    if (cycles == 0) return;
    s_sio_advance_called++;
    if (!g_sio_timing_active) return;
    s_sio_advance_with_work++;
    sio_pace_walk((int)cycles);
}
#else
/* Macro=0: stubs for ABI symmetry. */
void sio_advance(uint32_t cycles) { (void)cycles; }
uint64_t sio_get_advance_called(void)    { return 0; }
uint64_t sio_get_advance_with_work(void) { return 0; }
#endif /* SIO_MODEL_CYCLE_PACED */

void sio_tick(int cycles) {
#if SIO_MODEL_CYCLE_PACED
    /* Access-paced callers (I_STAT / SIO MMIO) pass 0. Master never walked
     * the cycle-paced shifter on those edges. Flushing overdue shift/ack
     * from sio_tick(0) can raise the next card ACK inside the ISR that is
     * still clearing the previous one.
     * EXPERIMENT: was netplay-only MMIO flush (MotK peer parity); always
     * skip tick(0) flush here for TM4 netplay desync testing. */
    if (cycles > 0)
        sio_pace_walk(cycles);
#else
    (void)cycles;
#endif

    /* Legacy access-paced IRQ countdown. Always live. In 1.0c-v2 this
     * remains the actual IRQ delivery path because the TX path arms it.
     * Phase 1.0d will stop arming this and rely on the cycle-paced
     * shifter+ack path instead. */
    if (sio_irq_pending && sio_irq_countdown > 0) {
        sio_irq_countdown--;
        if (sio_irq_countdown == 0) {
            sio_irq_pending = 0;
            sio_stat |= SIO_STAT_ACK;
            sio_ack_visible_reads = 2;
            sio_stat |= SIO_STAT_IRQ;
            /* Restore TX_RDY — the byte transfer is complete.
             * This unblocks the BIOS pad polling loop that spins
             * on TX_RDY before writing the next byte. */
            sio_stat |= SIO_STAT_TX_RDY | SIO_STAT_TX_EMPTY;
            uint32_t i_stat_before = i_stat;
            psx_irq_raise(IRQ_SIO0, 1); /* SIO shift IRQ */
            event_ring_record_aux(EV_DEQ, (uint8_t)SRC_SIO, 1u); /* SIO shift IRQ fired */

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

    /* Nested A6C10 after card traffic: also pumped from interrupt checks. */
    sio_ape_card_unstick_pump();
}

void sio_ape_card_unstick_pump(void) {
    if (s_ape_unstick_pending)
        ape_card_unstick_maybe(1);
}

void sio_tick_quantum(void) {
#if SIO_MODEL_CYCLE_PACED
    sio_tick(sio_tick_quantum_cycles);
#endif
}

/* Telemetry accessor for pace_state TCP probe. Read-only. */
void sio_get_pace_state(uint64_t out[16]) {
#if SIO_MODEL_CYCLE_PACED
    out[ 0] = 1;  /* sio_model = cycle_paced */
    out[ 1] = (uint64_t)sio_tick_quantum_cycles;
    out[ 2] = (uint64_t)sio_shift_active;
    out[ 3] = (uint64_t)sio_shift_remaining;
    out[ 4] = (uint64_t)sio_tx_buffered;
    out[ 5] = (uint64_t)sio_pending_ack;
    out[ 6] = (uint64_t)sio_ack_remaining;
    out[ 7] = (uint64_t)sio_bus_owner;
    out[ 8] = (uint64_t)sio_bus_byte_index;
    out[ 9] = s_pace_tx_writes_buffered;
    out[10] = s_pace_tx_writes_dropped_busy;
    out[11] = s_pace_tx_writes_dropped_cross_device;
    out[12] = s_pace_tx_buffer_promoted;
    out[13] = s_pace_tx_buffer_promoted_during_card;
    out[14] = s_pace_pad_byte_processed_in_card_data;
    out[15] = s_pace_cross_device_pad_during_card;
#else
    for (int i = 0; i < 16; i++) out[i] = 0;
#endif
}

/* ---- boot snapshot (LE field wire; McSlotState has host padding) ---- */
#include "pst_wire.h"

static int sio_w_mcslot(PstW *w, const McSlotState *s) {
    return pst_w_u32(w, (uint32_t)s->state) && pst_w_u8(w, s->cmd) &&
           pst_w_u16(w, s->sector) && pst_w_u8(w, s->sector_msb) &&
           pst_w_u8(w, s->sector_lsb) && pst_w_bytes(w, s->data, 128) &&
           pst_w_i32(w, (int32_t)s->data_idx) && pst_w_u8(w, s->checksum) &&
           pst_w_u8(w, s->flag);
}
static int sio_r_mcslot(PstR *r, McSlotState *s) {
    uint32_t st = 0;
    int32_t di = 0;
    if (!pst_r_u32(r, &st) || !pst_r_u8(r, &s->cmd) || !pst_r_u16(r, &s->sector) ||
        !pst_r_u8(r, &s->sector_msb) || !pst_r_u8(r, &s->sector_lsb) ||
        !pst_r_bytes(r, s->data, 128) || !pst_r_i32(r, &di) ||
        !pst_r_u8(r, &s->checksum) || !pst_r_u8(r, &s->flag))
        return 0;
    s->state = (McState)st;
    s->data_idx = (int)di;
    return 1;
}

/* Snapshot wire is emitted in four sections so netplay diags can CRC each
 * independently (regs / pads / memcard / shift+irq FSM) — a single SIO digest
 * could not say WHICH subsystem forked a resim. Order and bytes unchanged. */
static int sio_snap_emit_regs(PstW *w) {
    return pst_w_u8(w, sio_tx_data) && pst_w_u8(w, sio_rx_data) &&
           pst_w_u16(w, sio_stat) && pst_w_u16(w, sio_mode) &&
           pst_w_u16(w, sio_ctrl) && pst_w_u16(w, sio_baud);
}

static int sio_snap_emit_pads(PstW *w) {
    /* Pad arrays sized by PSX_MAX_PLAYERS. Default MAX=2 keeps the historical
     * 2-pad snap layout byte-identical. pad_response runtime buffer is larger
     * for multitap bulk (34); snap still stores the first 8 bytes. */
    if (!pst_w_bytes(w, pad_analog, PSX_MAX_PLAYERS) || !pst_w_u8(w, pad_connected) ||
        !pst_w_u32(w, (uint32_t)pad_state) || !pst_w_i32(w, (int32_t)selected_slot) ||
        !pst_w_bytes(w, pad_response, 8) || !pst_w_u8(w, pad_response_len) ||
        !pst_w_u8(w, pad_response_idx) || !pst_w_u8(w, pad_current_cmd) ||
        !pst_w_bytes(w, pad_in_config, PSX_MAX_PLAYERS))
        return 0;
    for (int s = 0; s < PSX_MAX_PLAYERS; s++) {
        if (!pst_w_i16(w, (int16_t)pad_type_req[s]))
            return 0;
    }
    return 1;
}

static int sio_snap_emit_mc(PstW *w) {
    if (!pst_w_u32(w, (uint32_t)mc_state) || !pst_w_i32(w, (int32_t)mc_slot) ||
        !pst_w_u8(w, mc_cmd) || !pst_w_u16(w, mc_sector) ||
        !pst_w_u8(w, mc_sector_msb) || !pst_w_u8(w, mc_sector_lsb) ||
        !pst_w_bytes(w, mc_data, 128) || !pst_w_i32(w, (int32_t)mc_data_idx) ||
        !pst_w_u8(w, mc_checksum) || !pst_w_u8(w, mc_flag))
        return 0;
    if (!sio_w_mcslot(w, &mc_slots[0]) || !sio_w_mcslot(w, &mc_slots[1]))
        return 0;
    return pst_w_u32(w, (uint32_t)active_device);
}

/* Load-bearing pacing state (guest-visible IRQ timing). */
static int sio_snap_emit_fsm_pace(PstW *w) {
#if SIO_MODEL_CYCLE_PACED
    if (!pst_w_i32(w, (int32_t)g_sio_timing_active) ||
        !pst_w_i32(w, (int32_t)sio_shift_active) || !pst_w_u8(w, sio_shift_byte) ||
        !pst_w_i32(w, (int32_t)sio_shift_remaining) ||
        !pst_w_i32(w, (int32_t)sio_tx_buffered) || !pst_w_u8(w, sio_tx_buffer) ||
        !pst_w_i32(w, (int32_t)sio_shift_ack_irq_en) ||
        !pst_w_i32(w, (int32_t)sio_tx_buffer_ack_irq_en) ||
        !pst_w_i32(w, (int32_t)sio_pending_ack) ||
        !pst_w_i32(w, (int32_t)sio_ack_remaining) ||
        !pst_w_i32(w, (int32_t)sio_pending_ack_irq_en) ||
        !pst_w_u32(w, (uint32_t)sio_bus_owner) || !pst_w_u32(w, sio_bus_byte_index))
        return 0;
#endif
    return pst_w_i32(w, (int32_t)sio_irq_pending) &&
           pst_w_i32(w, (int32_t)sio_irq_countdown) &&
           pst_w_i32(w, (int32_t)sio_ack_visible_reads);
}

/* Audit/diag metadata — must not drive netplay digests (byte_seq tracks the
 * host-local sio_trace_seq counter, which is not itself a guest register). */
static int sio_snap_emit_fsm_meta(PstW *w) {
    return pst_w_u8(w, sio_irq_pending_source) && pst_w_u8(w, sio_irq_pending_slot) &&
           pst_w_u8(w, sio_irq_pending_delay) && pst_w_u8(w, sio_irq_pending_mc_state) &&
           pst_w_u32(w, sio_irq_pending_byte_seq);
}

/* DualShock rumble map/motors — appended after meta so pre-rumble snapshots
 * remain loadable and netplay digests that stop at fsm_pace stay stable. */
static int sio_snap_emit_rumble(PstW *w) {
    return pst_w_bytes(w, pad_rumble_map, sizeof(pad_rumble_map)) &&
           pst_w_bytes(w, pad_rumble_small, sizeof(pad_rumble_small)) &&
           pst_w_bytes(w, pad_rumble_large, sizeof(pad_rumble_large));
}

static int sio_snap_emit_fsm(PstW *w) {
    return sio_snap_emit_fsm_pace(w) && sio_snap_emit_fsm_meta(w);
}

static int sio_snap_emit(PstW *w) {
    return sio_snap_emit_regs(w) && sio_snap_emit_pads(w) &&
           sio_snap_emit_mc(w) && sio_snap_emit_fsm(w) && sio_snap_emit_rumble(w);
}

/* Cumulative section end offsets in the snapshot wire:
 * out[0]=regs, out[1]=pads, out[2]=memcard, out[3]=fsm_pace (netplay),
 * out[4]=full including fsm_meta + rumble (== sio_snapshot_bytes()). */
void sio_snapshot_section_ends(uint32_t out[5]) {
    PstW w;
    pst_w_init(&w, NULL, 0);
    (void)sio_snap_emit_regs(&w);
    out[0] = (uint32_t)w.written;
    (void)sio_snap_emit_pads(&w);
    out[1] = (uint32_t)w.written;
    (void)sio_snap_emit_mc(&w);
    out[2] = (uint32_t)w.written;
    (void)sio_snap_emit_fsm_pace(&w);
    out[3] = (uint32_t)w.written;
    (void)sio_snap_emit_fsm_meta(&w);
    (void)sio_snap_emit_rumble(&w);
    out[4] = (uint32_t)w.written;
}

static int sio_snap_parse(PstR *r) {
    uint32_t u;
    int32_t i;
    int16_t tr;
    if (!pst_r_u8(r, &sio_tx_data) || !pst_r_u8(r, &sio_rx_data) ||
        !pst_r_u16(r, &sio_stat) || !pst_r_u16(r, &sio_mode) ||
        !pst_r_u16(r, &sio_ctrl) || !pst_r_u16(r, &sio_baud))
        return 0;
    if (!pst_r_bytes(r, pad_analog, PSX_MAX_PLAYERS) || !pst_r_u8(r, &pad_connected) ||
        !pst_r_u32(r, &u) || !pst_r_i32(r, &i) ||
        !pst_r_bytes(r, pad_response, 8) || !pst_r_u8(r, &pad_response_len) ||
        !pst_r_u8(r, &pad_response_idx) || !pst_r_u8(r, &pad_current_cmd) ||
        !pst_r_bytes(r, pad_in_config, PSX_MAX_PLAYERS))
        return 0;
    pad_state = (PadState)u;
    selected_slot = (int)i;
    pad_active_logical = pad_logical_for_port(selected_slot);
    /* Multitap bulk responses are 34 bytes; snap only stores 8. Abort an
     * in-flight bulk restore rather than feed a truncated frame. */
    if (pad_response_len > 8) {
        pad_state = PAD_IDLE;
        pad_response_len = 0;
        pad_response_idx = 0;
        pad_current_cmd = 0;
    }
    for (int s = 0; s < PSX_MAX_PLAYERS; s++) {
        if (!pst_r_i16(r, &tr))
            return 0;
        pad_type_req[s] = (int8_t)tr;
    }
    if (!pst_r_u32(r, &u) || !pst_r_i32(r, &i) || !pst_r_u8(r, &mc_cmd) ||
        !pst_r_u16(r, &mc_sector) || !pst_r_u8(r, &mc_sector_msb) ||
        !pst_r_u8(r, &mc_sector_lsb) || !pst_r_bytes(r, mc_data, 128))
        return 0;
    mc_state = (McState)u;
    mc_slot = (int)i;
    if (!pst_r_i32(r, &i) || !pst_r_u8(r, &mc_checksum) || !pst_r_u8(r, &mc_flag))
        return 0;
    mc_data_idx = (int)i;
    if (!sio_r_mcslot(r, &mc_slots[0]) || !sio_r_mcslot(r, &mc_slots[1]))
        return 0;
    if (!pst_r_u32(r, &u)) return 0;
    active_device = (ActiveDevice)u;
#if SIO_MODEL_CYCLE_PACED
    if (!pst_r_i32(r, &i)) return 0;
    g_sio_timing_active = (int)i;
    if (!pst_r_i32(r, &i) || !pst_r_u8(r, &sio_shift_byte)) return 0;
    sio_shift_active = (int)i;
    if (!pst_r_i32(r, &i)) return 0;
    sio_shift_remaining = (int)i;
    if (!pst_r_i32(r, &i) || !pst_r_u8(r, &sio_tx_buffer)) return 0;
    sio_tx_buffered = (int)i;
    if (!pst_r_i32(r, &i)) return 0;
    sio_shift_ack_irq_en = (int)i;
    if (!pst_r_i32(r, &i)) return 0;
    sio_tx_buffer_ack_irq_en = (int)i;
    if (!pst_r_i32(r, &i)) return 0;
    sio_pending_ack = (int)i;
    if (!pst_r_i32(r, &i)) return 0;
    sio_ack_remaining = (int)i;
    if (!pst_r_i32(r, &i)) return 0;
    sio_pending_ack_irq_en = (int)i;
    if (!pst_r_u32(r, &u) || !pst_r_u32(r, &sio_bus_byte_index)) return 0;
    sio_bus_owner = (SioBusOwner)u;
#endif
    if (!pst_r_i32(r, &i)) return 0;
    sio_irq_pending = (int)i;
    if (!pst_r_i32(r, &i)) return 0;
    sio_irq_countdown = (int)i;
    if (!pst_r_i32(r, &i)) return 0;
    sio_ack_visible_reads = (int)i;
    if (!pst_r_u8(r, &sio_irq_pending_source) || !pst_r_u8(r, &sio_irq_pending_slot) ||
        !pst_r_u8(r, &sio_irq_pending_delay) || !pst_r_u8(r, &sio_irq_pending_mc_state) ||
        !pst_r_u32(r, &sio_irq_pending_byte_seq))
        return 0;
    /* sio_trace_seq is host-local and not on the wire; reseat it from the
     * restored byte_seq so the next TX does not stamp a peer-divergent
     * seq into sio_irq_pending_byte_seq (was forking fsm digests alone). */
    sio_trace_seq = sio_irq_pending_byte_seq;
    /* Pre-rumble snapshots end here. A mid-game legacy state cannot tell us
     * the negotiated map, so assume the standard 0x00/0x01 layout used by
     * commercial DualShock games while leaving both motors stopped. */
    if (r->p == r->end) {
        memset(pad_rumble_map, 0xFF, sizeof(pad_rumble_map));
        memset(pad_rumble_small, 0, sizeof(pad_rumble_small));
        memset(pad_rumble_large, 0, sizeof(pad_rumble_large));
        for (int s = 0; s < PSX_MAX_PLAYERS; s++) {
            pad_rumble_map[s][0] = 0x00;
            pad_rumble_map[s][1] = 0x01;
        }
        return 1;
    }
    if (!pst_r_bytes(r, pad_rumble_map, sizeof(pad_rumble_map)) ||
        !pst_r_bytes(r, pad_rumble_small, sizeof(pad_rumble_small)) ||
        !pst_r_bytes(r, pad_rumble_large, sizeof(pad_rumble_large)))
        return 0;
    if (r->p != r->end) return 0;
    return 1;
}

uint32_t sio_snapshot_bytes(void) {
    PstW w;
    pst_w_init(&w, NULL, 0);
    (void)sio_snap_emit(&w);
    return (uint32_t)w.written;
}

void sio_snapshot_write(uint8_t *p) {
    PstW w;
    uint32_t n = sio_snapshot_bytes();
    pst_w_init(&w, p, n);
    (void)sio_snap_emit(&w);
}

int sio_snapshot_read(const uint8_t *p, uint32_t len) {
    PstR r;
    const uint32_t current = sio_snapshot_bytes();
    const uint32_t rumble_bytes = (uint32_t)(sizeof(pad_rumble_map) +
                                  sizeof(pad_rumble_small) +
                                  sizeof(pad_rumble_large));
    if (len != current && (len > current || len + rumble_bytes != current))
        return 0;
    pst_r_init(&r, p, len);
    return sio_snap_parse(&r);
}
