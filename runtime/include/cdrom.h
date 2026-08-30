#ifndef PSXRECOMP_CDROM_H
#define PSXRECOMP_CDROM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void cdrom_init(const char* cue_path);

/* Did cdrom_init() actually mount an image? 0 = the drive is empty.
 *
 * cdrom_init() is deliberately non-fatal when iso_open() fails (a BIOS-only
 * target legitimately runs with no disc), so a game that was GIVEN a disc path
 * whose image could not be mounted would otherwise boot straight into an empty
 * drive and render nothing -- a black screen with no explanation. Callers that
 * passed a real path must check this and report. */
int cdrom_has_disc(void);
/* Disc license region for GetID ("SCEE"/"SCEA"/"SCEI"); derived from the
 * mounted disc's boot serial at launch. Defaults to "SCEA" (SCPH1001). */
void cdrom_set_disc_scex(const char scex[4]);

/* Set disc speed multiplier immediately. divisor=0 → instant, 1 → 1x, 2 → 2x. */
void cdrom_set_speed(int divisor);
/* Store configured game speed (applied post-BIOS via cdrom_notify_game_started). */
void cdrom_set_game_speed(int divisor);
/* Called by fntrace on first game-range dispatch; switches to game speed. */
void cdrom_notify_game_started(void);
/* LBA of the most recent SetLoc command (-1 if none yet). Used by the DMA
 * ring buffer to correlate each sector transfer with its disc position. */
int  cdrom_get_setloc_lba(void);

/* 'instant' per-frame sector-IRQ budget (step 3). Clamped to [1, 4096];
 * the per-sector period additionally floors at CDROM_MIN_DELAY. Writers:
 * game.toml [runtime] instant_max_per_frame, the cdrom_instant_rate TCP
 * command, and the turbo-through-loads predicate (step 4). */
void cdrom_set_instant_rate(int per_frame);
int  cdrom_get_instant_rate(void);

/* Strict, config-driven warm-load route accelerator. Up to 16 routes may be
 * registered after cdrom_init. A route arms only after arm_lba and activates
 * only if the following SetLoc values match lbas in order. Any mismatch
 * restores normal disc timing. Only non-XA read cadence is accelerated. */
void cdrom_register_warm_route(int arm_lba, const int* lbas, int count,
                               int instant_max_per_frame);
void cdrom_warm_route_set_enabled(int enabled);
void cdrom_warm_route_stats_json(char* out, int cap);

/* Passive physical-deadline -> buffer -> INT1 exposure telemetry (L1.5).
 * Diagnostics only: recording never changes CD scheduling or delivery. */
void cdrom_timing_reset(void);
void cdrom_timing_stats_json(char* out, int cap);
void debug_force_cd_reinsert(void);
/* FMV auto-skip detection: cdrom_xa_stream_active() lets the frontend detect
 * that streaming XA (FMV/CDDA) is in progress. The skip itself is done by the
 * frontend via uncapped pacing (it does NOT alter CD timing — flooding XA
 * sectors desyncs and hangs the player). */
int  cdrom_xa_stream_active(void);
/* True while a CD read is armed with XA/FMV mode bits (or XA already
 * streaming). Netplay uses this to arm no-invent / refuse tip episodes
 * before the first MDEC colour decode — MotK intro invent≠Start at FMV
 * entry opened a tip episode into a matched black wait. */
int  cdrom_fmv_stream_pending(void);
/* Re-arm host absolute CD deadlines from restored relative delays after
 * boot_state / RB snap load (read stream + pending/present dues from snap).
 * Does NOT clamp delays (unlike accelerate). */
void cdrom_resync_deadlines_after_restore(void);

/* CD load-burst ring (always-on). One record per gap-separated run of
 * delivered data sectors. `out` receives up to `max` records, newest first
 * (layout matches cdrom.c's CdBurst — consumed by debug_server.c only). */
typedef struct CdBurstRecord {
    uint32_t start_frame, end_frame;
    uint64_t start_ms, end_ms;
    uint32_t sectors;
    uint32_t rate;
    uint32_t divisor;
} CdBurstRecord;
int      cdrom_get_bursts(void *out, int max);
uint32_t cdrom_get_burst_total(void);

/* True while a data-sector load is in progress (read stream active or a
 * data sector delivered within the burst-gap window). XA streaming is never
 * a load. Drives turbo-through-loads (step 4). */
int cdrom_load_in_progress(void);
/* Physical non-XA data-read command state, without the logical load gap
 * bridge used by cdrom_load_in_progress(). Diagnostics only. */
int cdrom_data_read_active(void);

/* boot_state / netplay digest — full controller FSM (sector FIFOs included). */
uint32_t cdrom_snapshot_bytes(void);
void     cdrom_snapshot_write(uint8_t *p);
int      cdrom_snapshot_read(const uint8_t *p, uint32_t len);

/* After savestate restore: clamp long CD second-response / read-start delays
 * and arm a short boost window so post-load ReadTOC/seek/Init waits do not
 * freeze the picture for ~1s+. Completions still fire (IRQs preserved). */
void cdrom_accelerate_after_savestate(void);
/* Call once per host vblank while the boost window is armed. */
void cdrom_savestate_boost_vblank(void);
/* Non-zero while boost is armed AND a CD wait is outstanding (pending
 * second response or non-XA read). Lets turbo_loads unpace those waits. */
int  cdrom_savestate_cd_wait_active(void);
/* Remaining boost vblanks (0 when idle). Diagnostics / post-load probe. */
int  cdrom_savestate_boost_vblanks_remaining(void);

/* MMIO read/write (0x1F801800-0x1F801803) */
uint32_t cdrom_read(uint32_t addr);
void cdrom_write(uint32_t addr, uint32_t value);

/* Advance CD-ROM state machine by guest CPU cycles. Sets i_stat IRQ_CDROM
 * when a command completes or data is ready. */
void cdrom_advance(uint32_t cycles);

/* Cycle-budgeted precise event slicing: guest CPU cycles until a DELIVERABLE
 * CD-ROM IRQ (bit2 unmasked in i_mask). UINT32_MAX if none. */
uint32_t cdrom_cycles_to_irq(uint32_t i_mask);

/* Legacy frame-paced entry point for interpreter/oracle builds. */
void cdrom_tick(void);

/* DMA channel 3 interface */
uint32_t cdrom_dma_read(void);
int cdrom_dma_ready(void);
uint32_t cdrom_dma_sector_word_count(void);

typedef struct CDROMDebugState {
    uint64_t seq;
    uint8_t index_reg;
    uint8_t stat_reg;
    uint8_t request_reg;
    uint8_t irq_enable;
    uint8_t irq_flag;
    uint8_t mode_reg;
    uint8_t seek_min;
    uint8_t seek_sec;
    uint8_t seek_sect;
    uint8_t pending_cmd;
    uint8_t queued_cmd;
    uint8_t queued_pending;
    uint8_t queued_param_count;
    int has_disc;
    int param_count;
    int response_read;
    int response_count;
    int sector_read_pos;
    int sector_available;
    int sector_size;
    int reading;
    int read_min;
    int read_sec;
    int read_sect;
    uint8_t read_cmd;
    uint8_t filter_file;
    uint8_t filter_channel;
    uint8_t muted;
    int read_delay;
    /* Cycles until a held CD response is presented to INTC (0 = ready). */
    int irq_present_delay;
    int pending_pending;
    int pending_delay;
    int pending_phase;
    /* Operating disc-speed divisor (1 during BIOS; game.toml post-entry). */
    int speed_divisor;
    uint32_t i_stat;
    int last_sector_lba;
    int last_sector_size;
    uint32_t last_sector_frame;
    uint8_t last_sector_mode;
    uint8_t last_sector_have_raw;
    /* Read-stream hold accounting: guest cycles the disc clock was frozen
     * waiting on guest INT-ack / buffer drain (unfaithful pause; real discs
     * never stop). Must stay 0 since the 2026-07-10 continuous-disc fix;
     * nonzero = regression. */
    uint64_t read_hold_cycles;
    uint64_t read_hold_events;
    /* One-deep pended data-ready INT1 accounting (Beetle SetAIP analog). */
    uint64_t int1_pended;
    uint64_t int1_lost;
    uint8_t  int1_pending_now;
    /* Native media-clock observability. Diagnostic snapshots only; these
     * fields are not part of the controller savestate wire format. */
    uint8_t  xa_stream_active;
    uint8_t  cdda_playing;
    uint32_t cdda_lba;
    int      cdda_delay;
    uint64_t xa_audio_sectors;
    uint64_t xa_audio_frames;
    uint64_t cdda_sectors_played;
} CDROMDebugState;

typedef struct CDROMSectorDebugState {
    int current_available;
    int current_read_pos;
    int current_size;
    int last_lba;
    int last_size;
    uint32_t last_frame;
    uint8_t last_mode;
    uint8_t last_have_raw;
} CDROMSectorDebugState;

typedef struct CDROMTraceEntry {
    uint64_t seq;
    uint32_t addr;
    uint32_t val;
    uint32_t func;
    uint32_t pc;
    uint32_t frame;
    uint32_t i_stat;
    uint8_t kind;
    uint8_t width;
    uint8_t index_reg;
    uint8_t stat_reg;
    uint8_t irq_enable;
    uint8_t irq_flag;
    uint8_t param_count;
    uint8_t response_read;
    uint8_t response_count;
    uint8_t sector_available;
    uint8_t request_reg;
    uint8_t mode_reg;
    uint8_t pending_cmd;
    uint8_t pending_pending;
    uint8_t reading;
    uint8_t read_cmd;
    int sector_read_pos;
    int sector_size;
    int pending_delay;
    int read_delay;
} CDROMTraceEntry;

#define CDROM_TRACE_CAP (1 << 16)
#define CDROM_COMMAND_HISTORY_CAP (1 << 13)
#define CDROM_SECTOR_HISTORY_CAP (1 << 13)
#define CDROM_SECTOR_HISTORY_BYTES 128

typedef struct CDROMCommandHistoryEntry {
    uint64_t seq;
    uint32_t frame;
    uint32_t func;
    uint32_t pc;
    uint32_t i_stat;
    uint8_t kind;
    uint8_t cmd;
    uint8_t param_count;
    uint8_t params[16];
    uint8_t stat;
    uint8_t request;
    uint8_t irq_enable;
    uint8_t irq_flag;
    uint8_t mode;
    uint8_t seek_min;
    uint8_t seek_sec;
    uint8_t seek_sect;
    uint8_t read_min;
    uint8_t read_sec;
    uint8_t read_sect;
    uint8_t read_cmd;
    uint8_t reading;
    uint8_t pending_cmd;
    uint8_t pending_pending;
    uint8_t queued_cmd;
    uint8_t queued_pending;
} CDROMCommandHistoryEntry;

typedef struct CDROMSectorHistoryEntry {
    uint64_t seq;
    int lba;
    int size;
    uint32_t frame;
    uint8_t mode;
    uint8_t have_raw;
    uint8_t raw_mode;
    uint8_t xa_file;
    uint8_t xa_channel;
    uint8_t xa_submode;
    uint8_t xa_coding;
    uint8_t data_delivered;
    uint8_t xa_audio_delivered;
    uint8_t skip_reason;
    uint16_t bytes_len;
    uint8_t bytes[CDROM_SECTOR_HISTORY_BYTES];
} CDROMSectorHistoryEntry;

void cdrom_debug_snapshot(CDROMDebugState* out);
uint64_t cdrom_debug_get_trace(const CDROMTraceEntry** out_entries);
void cdrom_debug_clear_trace(void);
uint64_t cdrom_debug_get_command_history(const CDROMCommandHistoryEntry** out_entries);
void cdrom_debug_clear_command_history(void);
uint32_t cdrom_debug_copy_last_sector(uint32_t offset, uint32_t len,
                                      uint8_t* out,
                                      CDROMSectorDebugState* state);
uint64_t cdrom_debug_get_sector_history(const CDROMSectorHistoryEntry** out_entries);
void cdrom_debug_clear_sector_history(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_CDROM_H */
