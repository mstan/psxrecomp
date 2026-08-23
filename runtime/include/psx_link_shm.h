/*
 * psx_link_shm.h -- shared-memory serial link between TWO PROCESSES.
 *
 * The performance route to full-speed link play: each console is a complete
 * single-console runtime in its own process (own core, own window, own pad),
 * and the SIO1 "cable" is a pair of SPSC rings in a shared-memory segment,
 * paced by the same bounded-skew barrier the in-process dual mode validated
 * (psx_dual_barrier_reached). In-process dual halves the frame rate by
 * construction -- two guests on one core -- and the alternative (threads in
 * one process) requires making ~590 device-module statics and 66 config
 * setters per-machine; two processes get each console a core for free.
 *
 * TIMELINE. The two processes boot at different wall times, so their
 * psx_cycle_count values are unrelated. The link runs on LINK-TIME: local
 * cycles minus an epoch. tx stamps due = sender_link_now + latency; rx
 * releases when receiver_link_now >= due.
 *
 * TWO PACING MODES share the segment layout:
 *
 *  - WALL mode (local 2P couch link): epoch latched when the pair forms;
 *    the cycle barrier in psx_link_shm_poll keeps |A - B| <= lookahead and
 *    latency >= nothing in particular -- bytes may arrive LATE on the
 *    receiver's clock, which the libcomb traffic tolerates. Non-deterministic
 *    across runs (wall-clock pairing), fine for couch play.
 *
 *  - PAIR mode (netplay link lobbies): the segment couples a netplay CLIENT
 *    (session member, visible) with a local FOLLOWER (headless co-simulator
 *    of the other console). Epochs are anchored at each side's netplay tick 0
 *    (psx_link_shm_anchor_epoch), making every due stamp a pure function of
 *    the deterministic guest timeline -- identical on every machine in the
 *    session. The cable rings become TRUNCATABLE LOGS (monotonic u64 cursors;
 *    consumption moves a read cursor only) so pair-atomic rollback can rewind
 *    reads and truncate speculative writes; the endpoint snapshot carries the
 *    cursors (psx_link_shm_snap_cursors). Pacing is the driver->follower
 *    COMMAND CHANNEL (SAVE/TICK/LOAD/START), not the cycle barrier: with
 *    cable latency >= one vblank tick and the driver admitting tick T only
 *    after the follower acknowledged T-1, neither side can need a byte the
 *    other has not written yet -- delivery is deterministic by construction.
 *
 * LIVENESS. Ops stay pure (determinism contract in psx_link.h); the wall
 * clock is used ONLY by the barrier poll / command waits for peer-death
 * detection. A dead peer (stale heartbeat) reads as an unplugged cable:
 * connected()=0, DSR/CTS low, barrier disengaged.
 */
#ifndef PSX_LINK_SHM_H
#define PSX_LINK_SHM_H

#include <stdint.h>
#include "psx_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create-or-attach the segment and return this process's endpoint, or NULL.
 * Role: 'a' creates/claims side A, 'b' side B, 0 = first free side. */
PsxLinkEndpoint *psx_link_shm_open(const char *name, char role);
void             psx_link_shm_close(PsxLinkEndpoint *ep);
int              psx_link_shm_is(const PsxLinkEndpoint *ep);

/* Bounded-skew barrier + pairing + heartbeat. Called from the emu thread's
 * interrupt-poll site; blocks (short sleeps) while this side is more than
 * `lookahead` link-cycles ahead of the peer. Cheap no-op when inactive, and
 * heartbeat-only in PAIR mode (the command channel paces instead). */
void psx_link_shm_poll(uint64_t cycle_now);
int  psx_link_shm_active(void);

/* Diagnostics for the [DUAL]-style status line. */
void psx_link_shm_stats(uint64_t *my_link_now, uint64_t *peer_link_now,
                        uint32_t *paired, uint64_t *barrier_waits_ms);

/* ===== PAIR mode (netplay link lobbies) ================================= */

/* Determinism-relevant launch config. The DRIVER (netplay client) writes it
 * into the segment at open; the FOLLOWER verifies every field against its own
 * environment and refuses to pair on mismatch -- a config fork here would
 * surface as an unexplainable A-group desync minutes later. */
typedef struct PsxLinkPairCfg {
    uint32_t session_id;      /* netplay session (segment identity check)   */
    uint32_t driver_side;     /* 0 = driver owns console A, 1 = console B   */
    uint32_t tick_len_cycles; /* vblank period at anchor time               */
    uint32_t latency_cycles;  /* cable char latency; must be >= tick length */
    uint32_t flags;           /* PSX_LINK_PAIR_F_*                          */
    uint32_t bios_id;         /* settled BIOS identity                      */
    uint32_t codegen_hash;    /* AOT codegen hash (same binary contract)    */
    uint32_t mod_plan_hash;   /* digest of the applied mod plan (0 = vanilla)*/
} PsxLinkPairCfg;

#define PSX_LINK_PAIR_F_SW_RASTER   (1u << 0)
#define PSX_LINK_PAIR_F_NO_IDLE     (1u << 1)
#define PSX_LINK_PAIR_F_NO_AUTOFMV  (1u << 2)
#define PSX_LINK_PAIR_F_NO_MODS     (1u << 3)
#define PSX_LINK_PAIR_F_BLANK_CARDS (1u << 4)

/* Driver->follower command stream. TICK carries the pad rows the driver's
 * netplay engine applied for the FOLLOWER's console this tick (predicted or
 * sealed -- the follower has zero prediction logic of its own; it replays
 * whatever the driver's rollback engine decided, including resims). */
enum {
    PSX_LINK_CMD_START = 1,   /* tick-0 anchor: follower arms epoch, runs   */
    PSX_LINK_CMD_TICK  = 2,   /* run one tick with these two pad rows       */
    PSX_LINK_CMD_SAVE  = 3,   /* snapshot at this tick boundary             */
    PSX_LINK_CMD_LOAD  = 4,   /* restore snapshot; ack when done            */
    PSX_LINK_CMD_STOP  = 5,   /* session over: follower exits cleanly       */
};

#define PSX_LINK_PAIR_ROW_BYTES 8u   /* one netplay pad blob per seat */

typedef struct PsxLinkPairCmd {
    uint8_t  kind;
    uint8_t  flags;
    uint16_t _pad;
    uint32_t tick;
    uint8_t  rows[2][PSX_LINK_PAIR_ROW_BYTES];
} PsxLinkPairCmd;

/* Driver: switch an open segment into PAIR mode and publish cfg. Call once,
 * before the follower attaches (i.e. before spawning it). */
int  psx_link_shm_pair_init(const PsxLinkPairCfg *cfg);
/* Follower: read the driver-published cfg (returns 0 until it is present). */
int  psx_link_shm_pair_cfg(PsxLinkPairCfg *out);
int  psx_link_shm_pair_mode(void);

/* Both sides: anchor link-time epoch at the deterministic tick-0 boundary
 * (netplay park release / first START command). Until armed, tx/rx are dead
 * (no cable), which is correct: no guest link traffic exists before tick 0. */
void psx_link_shm_anchor_epoch(uint64_t cycle_now);

/* Driver side. push returns 0 only if the follower died (ring full = short
 * block; the follower drains fast). wait_tick blocks until the follower has
 * acknowledged executing tick >= tick (the pipeline barrier); wait_cmds
 * blocks until every pushed command was executed (the LOAD fence). Both
 * return 0 on peer death. */
int  psx_link_shm_cmd_push(const PsxLinkPairCmd *cmd);
int  psx_link_shm_wait_follower_tick(uint32_t tick);
int  psx_link_shm_wait_cmds_drained(void);

/* Follower side: block until the next command is available (returns 0 on
 * driver death), then pop it. done_tick publishes execution progress. */
int  psx_link_shm_cmd_wait_pop(PsxLinkPairCmd *out);
void psx_link_shm_set_done_tick(uint32_t tick);

/* ===== PSX_LINK_PERF=1 instrumentation ================================== */

enum { PSX_LINK_WAIT_ADMIT = 0, PSX_LINK_WAIT_FOLD = 1 };

typedef struct PsxLinkShmPerf {
    uint64_t wait_admit_us;   /* driver blocked on the tick barrier         */
    uint32_t wait_admit_n;
    uint64_t wait_fold_us;    /* driver blocked for the follower's digest   */
    uint32_t wait_fold_n;
    uint64_t wait_ack_us;     /* driver blocked on the LOAD fence           */
    uint64_t push_block_us;   /* command ring full                          */
    uint64_t pop_wait_us;     /* follower idle waiting for a command        */
    uint32_t pop_wait_n;
    uint64_t naps;            /* sleep calls across every wait              */
    uint64_t tx_bytes, rx_bytes;
    uint64_t rx_not_due;      /* rx refused: due cycle not reached yet      */
    uint64_t tx_block_us;     /* wire full                                  */
    uint32_t ring_max;        /* deepest inbound occupancy                  */
    /* Cable read barrier: guest needed a byte the LOCAL peer process had
     * not published yet — host-time blocking INSIDE the guest window (the
     * pair-cadence serialization cost, previously invisible inside emu). */
    uint64_t rdbar_us;
    uint32_t rdbar_n;
    /* Lookahead barrier (psx_link_shm_poll): this side ran more than
     * `lookahead` guest cycles ahead of the local peer and blocked. */
    uint64_t ahead_us;
    uint32_t ahead_n;
} PsxLinkShmPerf;

/* Snapshot AND reset the counters (call once per report interval). */
void psx_link_shm_perf_take(PsxLinkShmPerf *out);
int  psx_link_shm_wait_follower_tick_r(uint32_t tick, int reason);

/* Follower post-run core digest per tick: published before done_tick, read
 * by the driver after wait_follower_tick(tick) — the machine's FRAME_COMMIT
 * folds it with the client's digest so BOTH consoles on every machine are
 * covered by the session's hash-confirm ladder. */
void psx_link_shm_publish_digest(uint32_t tick, uint32_t core);
int  psx_link_shm_read_digest(uint32_t tick, uint32_t *out);

/* Full per-partition digest set for one console tick — fold-mismatch
 * forensics. The follower publishes its console's partitions alongside the
 * folded core; the driver rings them up with its own so a FIRST MISMATCH can
 * name the diverging console AND partition by diffing the two machines'
 * logs. sio1/spad exist because the observed race-start mismatch coincides
 * with the first serial exchange. Same release/acquire contract as fol_dig
 * (written before done_tick). */
typedef struct PsxLinkFolParts {
    uint32_t core;
    uint32_t cpu;
    uint32_t clk;
    uint32_t tim;
    uint32_t ram;
    uint32_t dirty;
    uint32_t sio1;
    uint32_t spad;
    /* Raw boundary state at digest time (not CRCs): a digest-boundary phase
     * slip between machines reads directly as a cycle delta in the dump. */
    uint64_t cyc;             /* psx_cycle_count at digest */
    uint32_t csv;             /* cycles since vblank at digest */
    uint32_t istat;           /* I_STAT at digest */
} PsxLinkFolParts;

void psx_link_shm_publish_parts(uint32_t tick, const PsxLinkFolParts *p);
int  psx_link_shm_read_parts(uint32_t tick, PsxLinkFolParts *out);

/* LOAD fence + health. load_ack counts completed LOAD commands (published
 * by the follower AFTER its restore, so the driver's wait covers the whole
 * truncation); rewind_done_count resets the executed-count after a restore.
 * follower_err is a one-way fatal latch: any wait on the driver side returns
 * 0 once it is set. */
void     psx_link_shm_load_ack_publish(void);
int      psx_link_shm_wait_load_ack(uint32_t count);
void     psx_link_shm_rewind_done_count(uint32_t executed_count);
void     psx_link_shm_set_follower_err(uint32_t code);
uint32_t psx_link_shm_follower_err(void);

/* Snapshot integration (BS_SEC_SIO1 via the endpoint snap ops): in PAIR mode
 * the endpoint serializes {in_read, out_write} log cursors and restore
 * rewinds/truncates them. Exposed for diagnostics. */
void psx_link_shm_log_cursors(uint64_t *in_read, uint64_t *in_write,
                              uint64_t *out_read, uint64_t *out_write);

#ifdef __cplusplus
}
#endif
#endif /* PSX_LINK_SHM_H */
