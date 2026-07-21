/*
 * cdrom.c — PS1 CD-ROM controller (adapted from psxrecomp-v3)
 *
 * Registers at 0x1F801800-0x1F801803 with index-based register banking.
 * Reference: nocash PSX specs — CDROM Controller section
 *
 * v4 adaptation: removed event_deliver (HLE), removed CPUState dependency,
 * removed hard-coded RAM writes, removed fprintf. IRQ delivery is via
 * i_stat bit 2 (IRQ_CDROM); the recompiled BIOS exception handler
 * processes events natively.
 */

#include "cdrom.h"
#include "cdrom_irq.h"
#include "dma.h"
#include "spu.h"
#include "event_ring.h"
#include "audio_trace.h"
#include "psx_cycles.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifndef _WIN32
#  include <signal.h>
#endif
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <time.h>
#endif

/* C wrappers for the C++ ISOReader (defined in iso_reader_c.cpp) */
extern void* iso_open(const char* path);
extern int iso_read_sector(void* handle, uint32_t lba, uint8_t* buffer, int size);
extern int iso_read_raw_sector(void* handle, uint32_t lba, uint8_t* buffer, int size);
extern uint32_t iso_sector_count(void* handle);
extern void iso_close(void* handle);
/* Multi-track TOC accessors (CD-DA / multi-track discs). track is 1-based. */
extern int iso_track_count(void* handle);
extern uint32_t iso_track_start_lba(void* handle, int track);
extern uint32_t iso_track_pregap_lba(void* handle, int track);
extern int iso_track_is_audio(void* handle, int track);

/* I_STAT owned by memory.c — set bit 2 for CDROM IRQ */
extern uint32_t i_stat;
/* Central IRQ-raise choke point (interrupts.c) — also records the device ring. */
extern void psx_irq_raise(uint32_t bit, uint32_t detail);
extern uint32_t g_debug_current_func_addr;
extern uint32_t g_debug_last_store_pc;
extern uint64_t s_frame_count;

/* CD-ROM state */
static uint8_t index_reg;
static uint8_t stat_reg;
static uint8_t request_reg;
static uint8_t irq_enable;
static uint8_t irq_flag;

/* Disc license region string returned in GetID's last four response bytes
 * ("SCEE" PAL / "SCEA" NTSC-U / "SCEI" NTSC-J). Real hardware reports the
 * region of the INSERTED DISC (mechacon reads it from the license area);
 * the BIOS CD driver revalidates it when the kernel CD subsystem
 * reinitializes mid-game, and a mismatch throws it into an endless
 * GetStat/Init retry loop (Kula World wedged at its first level load this
 * way). Set from the mounted disc's SYSTEM.CNF serial at launch
 * (main.cpp); the default matches the console region of the one supported
 * BIOS (SCPH1001, NTSC-U). */
static uint8_t disc_scex[4] = { 'S', 'C', 'E', 'A' };

void cdrom_set_disc_scex(const char scex[4]) {
    memcpy(disc_scex, scex, 4);
}

/* CPS-native CD interrupt single-outstanding latch.
 *
 * The CD controller serializes responses: it presents one INT to the CPU
 * interrupt controller per visible IRQ generation, and the next is presented
 * only after the guest acknowledges the controller (write to 0x1F801803.1).
 *
 * Under the recompiled CPS interrupt model, psx_check_interrupts re-checks
 * (i_stat & i_mask) at every dispatch boundary. Without this latch,
 * refresh_cdrom_irq_line() re-asserts i_stat bit 2 from an already-set
 * irq_flag on every cdrom_advance, so an unacked CD INT becomes an unbounded
 * exception re-entry storm. The latch presents each generation to INTC exactly
 * once; the next generation can raise i_stat only after a controller ack.
 *
 * cdrom_intc_request_latched == 1  -> the current CD INT has been presented.
 * cdrom_irq_generation             -> increments per visible INT (set_irq); debug.
 * cdrom_intc_latched_generation    -> the generation last presented to INTC; debug.
 */
static int cdrom_intc_request_latched;
static uint32_t cdrom_irq_generation;
static uint32_t cdrom_intc_latched_generation;

/* CD response presentation latency.
 *
 * Real CD hardware does not produce a command's first response instantly: the
 * controller takes thousands of cycles to process the command before raising
 * the response IRQ. The previous model presented the first response (and the
 * delayed second response) SYNCHRONOUSLY inside the guest's command/ack MMIO
 * write. That re-creates a lost-interrupt hazard: when the BIOS issues the next
 * CD command from *inside* its CD interrupt handler (e.g. SeekL, issued right
 * before the handler's trailing I_STAT ack), the synchronous response raises
 * i_stat bit2, the handler's own INTC ack then clears it while irq_flag is
 * still set, and the single-outstanding latch suppresses re-presentation
 * forever -> the command never completes (boot-EXE-load SeekL wedge).
 *
 * Fix: arm a presentation delay on every new response (set_irq) and gate
 * present_cdrom_irq() on it. The present then lands at a later cdrom_advance,
 * at a clean instruction boundary after the issuing ISR has returned and
 * re-enabled interrupts — exactly as on real hardware. The latch is unchanged
 * (it still models edge-latched delivery: one present per generation).
 *
 * Sized to comfortably outlast an ISR teardown (tens-to-hundreds of cycles)
 * while staying well under the second-response delays (10k-30k), so command
 * throughput is unaffected. */
#define CDROM_IRQ_PRESENT_DELAY 5000
static int cdrom_irq_present_delay;

/* Parameter FIFO */
#define PARAM_FIFO_SIZE 16
static uint8_t param_fifo[PARAM_FIFO_SIZE];
static int param_count;

/* Response FIFO */
#define RESPONSE_FIFO_SIZE 16
static uint8_t response_fifo[RESPONSE_FIFO_SIZE];
static int response_read;
static int response_count;

/* Data buffer. Whole-sector mode transfers 0x924 bytes starting after
 * the 12 sync bytes: header, subheader, and sector payload. */
#define SECTOR_SIZE 2048
#define RAW_SECTOR_SIZE 2352
#define RAW_USER_DATA_OFFSET 24
#define WHOLE_SECTOR_OFFSET 12
#define WHOLE_SECTOR_SIZE (RAW_SECTOR_SIZE - WHOLE_SECTOR_OFFSET)
#define FALLBACK_SECTOR_HEADER_SIZE 12
#define FALLBACK_WHOLE_SECTOR_SIZE (FALLBACK_SECTOR_HEADER_SIZE + SECTOR_SIZE)
#define SECTOR_BUFFER_SIZE WHOLE_SECTOR_SIZE
static uint8_t sector_buffer[SECTOR_BUFFER_SIZE];
static int sector_read_pos;
static int sector_available;
static int sector_size;
static uint8_t last_sector_buffer[SECTOR_BUFFER_SIZE];
static int last_sector_lba;
static int last_sector_size;
static uint32_t last_sector_frame;
static uint8_t last_sector_mode;
static uint8_t last_sector_have_raw;
static uint8_t last_sector_raw_mode;
static uint8_t last_sector_xa_file;
static uint8_t last_sector_xa_channel;
static uint8_t last_sector_xa_submode;
static uint8_t last_sector_xa_coding;
static CDROMSectorHistoryEntry sector_history[CDROM_SECTOR_HISTORY_CAP];
static uint64_t sector_history_seq;
static CDROMCommandHistoryEntry command_history[CDROM_COMMAND_HISTORY_CAP];
static uint64_t command_history_seq;

/* Seek target */
static uint8_t seek_min, seek_sec, seek_sect;
static int     s_setloc_lba = -1;  /* LBA captured at SetLoc time */
static int     setloc_seek_far;

/* Read state */
static int reading;
static int read_min, read_sec, read_sect;
static uint8_t mode_reg;
static uint8_t read_cmd;
static int read_delay;

/* ---- Sector deadline/exposure telemetry (L1.5, passive) -----------------
 * One record per physical sector deadline. It separates drive scheduling
 * lateness (buffer_cycle - due_cycle) from guest/controller backpressure
 * (IRQ arm/presentation after the buffer already exists). */
#define CD_TIMING_CAP 4096u
#define CDT_DATA      0x01u
#define CDT_DMA       0x02u
#define CDT_PENDED    0x04u
#define CDT_LOST      0x08u
#define CDT_IRQ_ARMED 0x10u
#define CDT_INTC      0x20u
typedef struct {
    uint64_t seq;
    uint64_t due_cycle;
    uint64_t buffer_cycle;
    uint64_t irq_arm_cycle;
    uint64_t intc_cycle;
    uint32_t frame;
    int32_t  lba;
    uint8_t  flags;
} CdTimingRecord;
static CdTimingRecord s_cd_timing[CD_TIMING_CAP];
static uint64_t s_cd_timing_total;
static uint64_t s_cd_timing_reset_seq;
static uint64_t s_cd_timing_next_due;
static uint64_t s_cd_timing_irq_seq = UINT64_MAX;
static uint64_t s_cd_timing_pending_seq = UINT64_MAX;
static uint64_t s_cd_timing_stream_starts;
static uint64_t s_cd_timing_reset_cycle;
static uint64_t s_cd_probe_read_start_count, s_cd_probe_read_start_cycles;
static uint64_t s_cd_probe_pause_count, s_cd_probe_pause_cycles;
static uint64_t s_cd_probe_seek_count, s_cd_probe_seek_cycles;
static uint64_t s_cd_probe_motor_count, s_cd_probe_motor_cycles;
static uint64_t s_cd_probe_stop_count, s_cd_probe_stop_cycles;

static void cd_timing_note_intc(void);
static uint8_t filter_file;
static uint8_t filter_channel;
static uint8_t cd_muted;

#define XA_SUBHEADER_OFFSET 16
#define XA_DATA_OFFSET      24
#define XA_SOUND_GROUPS     18
#define XA_NATIVE_FRAMES    (XA_SOUND_GROUPS * 8 * 28)
#define XA_MAX_44100_FRAMES 9408

#define XA_SUBMODE_AUDIO 0x04
#define XA_SUBMODE_REALTIME 0x40
#define CDROM_SECTOR_MODE2 0x02

#define CDROM_SKIP_NONE 0
#define CDROM_SKIP_XA_AUDIO_REALTIME 1
#define CDROM_REQUEST_BFRD 0x80u

typedef struct CDROMSectorDelivery {
    uint8_t raw_mode;
    uint8_t xa_file;
    uint8_t xa_channel;
    uint8_t xa_submode;
    uint8_t xa_coding;
    uint8_t data_delivered;
    uint8_t xa_audio_delivered;
    uint8_t skip_reason;
} CDROMSectorDelivery;

static int32_t xa_hist_l[2];
static int32_t xa_hist_r[2];
static uint8_t xa_stream_file;
static uint8_t xa_stream_channel;
static uint8_t xa_stream_coding;
static int xa_stream_active;

/* Red Book CD-DA playback state. One raw audio sector contains exactly 588
 * stereo frames; at 75 sectors/second this is the SPU's native 44.1 kHz. */
#define CDDA_SECTOR_FRAMES 588
static int cdda_playing;
static int cdda_track;
static uint32_t cdda_lba;
static int cdda_delay;
/* Natural track-end INT4 can occur while a previous CD response is still
 * awaiting acknowledgement. Keep it pending instead of dropping the only
 * notification the game uses to restart looping level music. */
static int cdda_data_end_pending;
static uint64_t cdda_sectors_played;

/* Operating divisor: 1x during BIOS boot, switches to g_game_divisor
 * when the game's entry point first fires (via cdrom_notify_game_started). */
static int g_disc_speed_divisor = 1;
/* Configured target speed — applied post-BIOS. */
static int g_game_divisor = 1;

void cdrom_set_speed(int divisor) {
    g_disc_speed_divisor = divisor;
}

/* Store the configured speed for post-BIOS application. Boot stays at 1x. */
void cdrom_set_game_speed(int divisor) {
    g_game_divisor = divisor;
}

/* Called by fntrace_record on first game-range dispatch. */
void cdrom_notify_game_started(void) {
    g_disc_speed_divisor = g_game_divisor;
}

int cdrom_get_setloc_lba(void) { return s_setloc_lba; }

/* Frontend XA-stream probe (FMV auto-skip / turbo-load gating in main.cpp). */
int cdrom_xa_stream_active(void) { return xa_stream_active; }

/* Response-overwrite diagnostics consumed by debug_server.c. master's CD
 * response arbiter tracks how often an undelivered response was overwritten;
 * this CD model (forwarded from the Ape bring-up branch) delivers responses
 * directly and does not overwrite, so these stay zero. Defined here so the
 * debug command links against either CD model. */
uint32_t g_cd_overwrite_count = 0;
uint8_t  g_cd_overwrite_first_prev = 0;
uint8_t  g_cd_overwrite_first_new  = 0;
uint32_t g_cd_overwrite_first_frame = 0;
uint8_t  g_cd_overwrite_last_prev = 0;
uint8_t  g_cd_overwrite_last_new  = 0;
uint32_t g_cd_overwrite_last_frame = 0;

/* Minimum cycles between CD-ROM IRQs in fast modes. Must be enough for the
 * interrupt handler to save state, check the IRQ flag, process data, and
 * return. Too low → interrupt fires before the previous one is processed →
 * game hangs. 500 cycles ≈ 15µs at 33MHz, still ~900x faster than authentic. */
#define CDROM_MIN_DELAY 500

/* 'instant' disc speed (divisor 0): as fast as the engine can absorb WITHOUT
 * starving the 60Hz VBLANK or the main loop. A flat CDROM_MIN_DELAY collapses
 * every sector read to 500cy, so a multi-sector load fires ~1100 DMA-completion
 * IRQs per VBLANK frame (564480/500). That buries VBLANK and the main loop
 * under a DMA-IRQ storm — the field loop is pinned and the watchdog reports a
 * freeze. Floor the per-response period so at most ~CDROM_INSTANT_MAX_PER_FRAME
 * sector IRQs land per frame. At 32/frame this is still ~3x faster than 4x
 * (sector ≈ 17640cy vs 56448cy) so loads stay near-instant in wall-clock, but
 * frames always advance. Paired with the per-source exception-progress
 * guarantee in interrupts.c (the real anti-starvation fix); this floor keeps
 * the exception VOLUME sane so the framerate doesn't collapse. Tunable via the
 * MAX_PER_FRAME knob. */
#define VBLANK_CYCLES_NTSC          564480   /* 33.8688 MHz / 60 Hz; matches interrupts.c */
#define CDROM_INSTANT_MAX_PER_FRAME_DEFAULT 32
#define CDROM_SINGLE_SPEED_SECTOR_CYCLES 451584

/* Runtime-tunable 'instant' budget (step 3). One knob drives three writers:
 * game.toml [runtime] instant_max_per_frame, the cdrom_instant_rate TCP
 * command (in-session A/B without rebuilds), and — step 4 — the
 * turbo-through-loads predicate. Period floors at CDROM_MIN_DELAY, so the
 * effective ceiling is VBLANK_CYCLES_NTSC/CDROM_MIN_DELAY ≈ 1128/frame. */
static int g_instant_max_per_frame = CDROM_INSTANT_MAX_PER_FRAME_DEFAULT;

/* Config-driven, game-specific warm-load routes. These deliberately change
 * only non-XA read cadence: the normal command, seek/motor timing, IRQ,
 * callback, DMA, and guest decompression paths remain authoritative. */
#define CDROM_WARM_ROUTE_MAX 64
#define CDROM_WARM_ROUTES_MAX 16
typedef struct CDROMWarmRoute {
    int arm_lba;
    int lbas[CDROM_WARM_ROUTE_MAX];
    int count;
    int rate;
} CDROMWarmRoute;
static CDROMWarmRoute s_warm_routes[CDROM_WARM_ROUTES_MAX];
static int      s_warm_routes_count;
static int      s_warm_route_configured;
static int      s_warm_route_enabled;
static int      s_warm_route_armed;
static int      s_warm_route_armed_lba = -1;
static int      s_warm_route_active;
static int      s_warm_route_active_index = -1;
static int      s_warm_route_next;
static int      s_warm_route_last_lba = -1;
static uint64_t s_warm_route_matches;
static uint64_t s_warm_route_mismatches;
static uint64_t s_warm_route_sectors;
static uint64_t s_warm_route_consumer_waits;
static uint64_t s_warm_route_consumer_wait_cycles;

void cdrom_register_warm_route(int arm_lba, const int* lbas, int count,
                               int instant_max_per_frame) {
    if (!lbas || count < 1 || count > CDROM_WARM_ROUTE_MAX || arm_lba < 0 ||
        s_warm_routes_count >= CDROM_WARM_ROUTES_MAX) return;
    if (instant_max_per_frame < 1) instant_max_per_frame = 1;
    if (instant_max_per_frame > 4096) instant_max_per_frame = 4096;
    CDROMWarmRoute *route = &s_warm_routes[s_warm_routes_count++];
    memcpy(route->lbas, lbas, (size_t)count * sizeof(lbas[0]));
    route->arm_lba = arm_lba;
    route->count = count;
    route->rate = instant_max_per_frame;
    s_warm_route_configured = 1;
    s_warm_route_enabled = 1;
    s_warm_route_armed = 0;
    s_warm_route_armed_lba = -1;
    s_warm_route_active = 0;
    s_warm_route_active_index = -1;
    s_warm_route_next = 0;
    s_warm_route_last_lba = -1;
}

void cdrom_warm_route_set_enabled(int enabled) {
    s_warm_route_enabled = enabled ? 1 : 0;
    if (!s_warm_route_enabled) {
        s_warm_route_armed = 0;
        s_warm_route_armed_lba = -1;
        s_warm_route_active = 0;
        s_warm_route_active_index = -1;
        s_warm_route_next = 0;
        s_warm_route_last_lba = -1;
    }
}

static const CDROMWarmRoute* warm_route_current(void) {
    if (!s_warm_route_active || s_warm_route_active_index < 0 ||
        s_warm_route_active_index >= s_warm_routes_count) return NULL;
    return &s_warm_routes[s_warm_route_active_index];
}

void cdrom_warm_route_stats_json(char* out, int cap) {
    if (!out || cap <= 0) return;
    int state_index = s_warm_route_active ? s_warm_route_active_index : -1;
    const CDROMWarmRoute *route = state_index >= 0
        ? &s_warm_routes[state_index]
        : (s_warm_routes_count > 0 ? &s_warm_routes[0] : NULL);
    snprintf(out, (size_t)cap,
             "\"configured\":%d,\"enabled\":%d,\"armed\":%d,"
             "\"active\":%d,\"routes\":%d,\"active_route\":%d,"
             "\"arm_lba\":%d,\"route_entries\":%d,"
             "\"next_entry\":%d,\"rate\":%d,\"matches\":%llu,"
             "\"mismatches\":%llu,\"sectors\":%llu,"
             "\"consumer_waits\":%llu,\"consumer_wait_cycles\":%llu",
             s_warm_route_configured, s_warm_route_enabled,
             s_warm_route_armed, s_warm_route_active, s_warm_routes_count,
             state_index, route ? route->arm_lba : -1,
             route ? route->count : 0, s_warm_route_next,
             route ? route->rate : 0,
             (unsigned long long)s_warm_route_matches,
             (unsigned long long)s_warm_route_mismatches,
             (unsigned long long)s_warm_route_sectors,
             (unsigned long long)s_warm_route_consumer_waits,
             (unsigned long long)s_warm_route_consumer_wait_cycles);
}

static void warm_route_on_setloc(int lba) {
    if (!s_warm_route_configured || !s_warm_route_enabled) return;

    for (int i = 0; i < s_warm_routes_count; i++) {
        if (lba == s_warm_routes[i].arm_lba) {
            s_warm_route_armed = 1;
            s_warm_route_armed_lba = lba;
            s_warm_route_active = 0;
            s_warm_route_active_index = -1;
            s_warm_route_next = 0;
            s_warm_route_last_lba = lba;
            return;
        }
    }

    if (!s_warm_route_active) {
        if (s_warm_route_armed) {
            int match = -1;
            for (int i = 0; i < s_warm_routes_count; i++) {
                if (s_warm_routes[i].arm_lba == s_warm_route_armed_lba &&
                    lba == s_warm_routes[i].lbas[0]) {
                    match = i;
                    break;
                }
            }
            s_warm_route_armed = 0;
            s_warm_route_armed_lba = -1;
            if (match >= 0) {
                s_warm_route_active = 1;
                s_warm_route_active_index = match;
                s_warm_route_next = 1;
                s_warm_route_last_lba = lba;
                s_warm_route_matches++;
            } else {
                s_warm_route_mismatches++;
            }
        }
        return;
    }

    const CDROMWarmRoute *route = warm_route_current();
    /* Repeated SetLoc for the current file is harmless. Every transition to
     * a new file must match the captured order exactly. */
    if (lba == s_warm_route_last_lba) return;
    if (route && s_warm_route_next < route->count &&
        lba == route->lbas[s_warm_route_next]) {
        s_warm_route_last_lba = lba;
        s_warm_route_next++;
        return;
    }

    s_warm_route_active = 0;
    s_warm_route_active_index = -1;
    s_warm_route_armed = 0;
    s_warm_route_armed_lba = -1;
    s_warm_route_next = 0;
    s_warm_route_last_lba = -1;
    s_warm_route_mismatches++;
}

void cdrom_set_instant_rate(int per_frame) {
    if (per_frame < 1)    per_frame = 1;
    if (per_frame > 4096) per_frame = 4096;
    g_instant_max_per_frame = per_frame;
}
int cdrom_get_instant_rate(void) { return g_instant_max_per_frame; }

static int instant_period(void) {
    int p = VBLANK_CYCLES_NTSC / g_instant_max_per_frame;
    return p < CDROM_MIN_DELAY ? CDROM_MIN_DELAY : p;
}

static int warm_route_period(void) {
    const CDROMWarmRoute *route = warm_route_current();
    int rate = route ? route->rate : CDROM_INSTANT_MAX_PER_FRAME_DEFAULT;
    int p = VBLANK_CYCLES_NTSC / rate;
    return p < CDROM_MIN_DELAY ? CDROM_MIN_DELAY : p;
}

static int apply_speed(int delay) {
    /* XA streaming (FMV / CDDA background music): preserve authentic timing.
     * FMVs interleave XA audio + MDEC video — speeding up sector delivery
     * would cause both to play faster than the display refresh rate. */
    if (xa_stream_active) return delay;
    if (g_disc_speed_divisor == 0) return instant_period(); /* bounded 'instant' */
    int d = delay / g_disc_speed_divisor;
    return d < CDROM_MIN_DELAY ? CDROM_MIN_DELAY : d;
}

static int apply_read_speed(int delay) {
    /* A route is an explicit DATA-read allowlist, never a blanket drive-speed
     * change. XA filter/ADPCM modes are rejected before the first streaming
     * sector can set xa_stream_active; established XA remains authentic too. */
    if (xa_stream_active || (mode_reg & 0x48u)) return delay;
    if (s_warm_route_active) return warm_route_period();
    return apply_speed(delay);
}

/* ---- CD load-burst ring (always-on; CLAUDE.md ring-buffer rule) ----------
 * A "burst" is a run of delivered sectors with no gap longer than
 * CD_BURST_GAP_FRAMES — i.e. one load. Records make "load duration" a
 * measured quantity (frames + host wall ms + sectors + the instant rate in
 * effect), queried via the cdrom_bursts TCP command. */
#define CD_BURST_CAP        128
#define CD_BURST_GAP_FRAMES 30
typedef CdBurstRecord CdBurst;       /* layout shared with cdrom.h consumers */
static CdBurst  s_bursts[CD_BURST_CAP];
static uint32_t s_burst_count = 0;   /* monotonic; slot = (count-1) % CAP */

static uint64_t host_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

static void burst_note_sector(void) {
    uint32_t f  = (uint32_t)s_frame_count;
    uint64_t ms = host_ms();
    CdBurst *b = (s_burst_count > 0)
               ? &s_bursts[(s_burst_count - 1u) % CD_BURST_CAP] : NULL;
    if (!b || f > b->end_frame + CD_BURST_GAP_FRAMES) {
        b = &s_bursts[s_burst_count++ % CD_BURST_CAP];
        b->start_frame = f;
        b->start_ms    = ms;
        b->sectors     = 0;
    }
    b->end_frame = f;
    b->end_ms    = ms;
    b->sectors++;
    const CDROMWarmRoute *route = warm_route_current();
    b->rate    = (uint32_t)(route ? route->rate : g_instant_max_per_frame);
    b->divisor = (uint32_t)(s_warm_route_active ? 0
                                                : g_disc_speed_divisor);
}

/* Copy out the most recent `max` bursts, newest first. Returns count. */
int cdrom_get_bursts(void *out, int max) {
    CdBurst *dst = (CdBurst *)out;
    int n = 0;
    for (uint32_t k = 0; k < CD_BURST_CAP && n < max; k++) {
        if (k >= s_burst_count) break;
        dst[n++] = s_bursts[(s_burst_count - 1u - k) % CD_BURST_CAP];
    }
    return n;
}
uint32_t cdrom_get_burst_total(void) { return s_burst_count; }

static int sector_delay_cycles(void) {
    /* PS1 CPU is 33.8688 MHz. CD-ROM sectors arrive at 75 Hz in 1x
     * mode, or twice that rate when SetMode bit 7 enables double speed. */
    int base = (mode_reg & 0x80)
        ? (CDROM_SINGLE_SPEED_SECTOR_CYCLES / 2)
        : CDROM_SINGLE_SPEED_SECTOR_CYCLES;
    return apply_read_speed(base);
}

static int initial_read_delay_cycles(void) {
    /* Beetle/PCSX model an additional read-start latency after ReadN/ReadS.
     * In double-speed mode the first sector still waits one 1x sector period;
     * subsequent sectors use the steady-state 2x cadence above. */
    int base = (mode_reg & 0x80)
        ? CDROM_SINGLE_SPEED_SECTOR_CYCLES
        : (CDROM_SINGLE_SPEED_SECTOR_CYCLES * 2);
    return apply_read_speed(base);
}

static int seek_complete_delay_cycles(void) {
    /* PCSX carries a far-SetLoc seek state and explicitly calls out Rockman X5:
     * far SeekL/SeekP completes after roughly four 1x sector periods, while a
     * near/already-settled seek returns quickly. */
    int base = setloc_seek_far ? (CDROM_SINGLE_SPEED_SECTOR_CYCLES * 4) : 0x800;
    return apply_speed(base);
}

/* Pending command */
typedef struct {
    uint8_t cmd;
    int pending;
    int delay;
    int phase;
} PendingCmd;
static PendingCmd pending;

typedef struct {
    uint8_t cmd;
    uint8_t params[PARAM_FIFO_SIZE];
    int param_count;
    int pending;
} QueuedCmd;
static QueuedCmd queued_cmd;

static void exec_command(uint8_t cmd);

/* ISO reader */
static void* iso_handle = NULL;

static CDROMTraceEntry cdrom_trace[CDROM_TRACE_CAP];
static uint64_t cdrom_trace_seq;

static void trace_cdrom(uint8_t kind, uint32_t addr, uint32_t val, uint8_t width) {
    CDROMTraceEntry *e = &cdrom_trace[cdrom_trace_seq % CDROM_TRACE_CAP];
    e->seq = cdrom_trace_seq++;
    e->kind = kind;
    e->addr = addr;
    e->val = val;
    e->width = width;
    e->func = g_debug_current_func_addr;
    e->pc = g_debug_last_store_pc;
    e->frame = (uint32_t)s_frame_count;
    e->i_stat = i_stat;
    e->index_reg = index_reg;
    e->stat_reg = stat_reg;
    e->request_reg = request_reg;
    e->irq_enable = irq_enable;
    e->irq_flag = irq_flag;
    e->mode_reg = mode_reg;
    e->param_count = (uint8_t)param_count;
    e->response_read = (uint8_t)response_read;
    e->response_count = (uint8_t)response_count;
    e->sector_available = (uint8_t)sector_available;
    e->sector_read_pos = sector_read_pos;
    e->sector_size = sector_size;
    e->pending_cmd = pending.cmd;
    e->pending_pending = (uint8_t)pending.pending;
    e->pending_delay = pending.delay;
    e->reading = (uint8_t)reading;
    e->read_cmd = read_cmd;
    e->read_delay = read_delay;
}

static void record_command_history(uint8_t kind, uint8_t cmd,
                                   const uint8_t* params, int count) {
    CDROMCommandHistoryEntry *e =
        &command_history[command_history_seq % CDROM_COMMAND_HISTORY_CAP];
    memset(e, 0, sizeof(*e));
    e->seq = command_history_seq++;
    e->frame = (uint32_t)s_frame_count;
    e->func = g_debug_current_func_addr;
    e->pc = g_debug_last_store_pc;
    e->i_stat = i_stat;
    e->kind = kind;
    e->cmd = cmd;
    if (count < 0) count = 0;
    if (count > PARAM_FIFO_SIZE) count = PARAM_FIFO_SIZE;
    e->param_count = (uint8_t)count;
    if (params && count > 0) {
        memcpy(e->params, params, (size_t)count);
    }
    e->stat = stat_reg;
    e->request = request_reg;
    e->irq_enable = irq_enable;
    e->irq_flag = irq_flag;
    e->mode = mode_reg;
    e->seek_min = seek_min;
    e->seek_sec = seek_sec;
    e->seek_sect = seek_sect;
    e->read_min = (uint8_t)read_min;
    e->read_sec = (uint8_t)read_sec;
    e->read_sect = (uint8_t)read_sect;
    e->read_cmd = read_cmd;
    e->reading = (uint8_t)(reading ? 1 : 0);
    e->pending_cmd = pending.cmd;
    e->pending_pending = (uint8_t)(pending.pending ? 1 : 0);
    e->queued_cmd = queued_cmd.cmd;
    e->queued_pending = (uint8_t)(queued_cmd.pending ? 1 : 0);
}

static int xa_is_audio_realtime(const CDROMSectorDelivery *d) {
    return d &&
           d->raw_mode == CDROM_SECTOR_MODE2 &&
           ((d->xa_submode & (XA_SUBMODE_AUDIO | XA_SUBMODE_REALTIME)) ==
            (XA_SUBMODE_AUDIO | XA_SUBMODE_REALTIME));
}

static CDROMSectorDelivery classify_raw_sector(const uint8_t *raw_data, int have_raw) {
    CDROMSectorDelivery d;
    memset(&d, 0, sizeof(d));
    if (!have_raw || !raw_data) return d;

    d.raw_mode = raw_data[15];
    if (d.raw_mode == CDROM_SECTOR_MODE2) {
        d.xa_file = raw_data[XA_SUBHEADER_OFFSET + 0];
        d.xa_channel = raw_data[XA_SUBHEADER_OFFSET + 1];
        d.xa_submode = raw_data[XA_SUBHEADER_OFFSET + 2];
        d.xa_coding = raw_data[XA_SUBHEADER_OFFSET + 3];
    }
    return d;
}

static void record_sector_history(int lba, int size, uint8_t mode, int have_raw,
                                  const uint8_t *bytes,
                                  const CDROMSectorDelivery *delivery) {
    CDROMSectorHistoryEntry *e =
        &sector_history[sector_history_seq % CDROM_SECTOR_HISTORY_CAP];
    e->seq = sector_history_seq++;
    e->lba = lba;
    e->size = size;
    e->frame = (uint32_t)s_frame_count;
    e->mode = mode;
    e->have_raw = (uint8_t)(have_raw ? 1 : 0);
    e->raw_mode = delivery ? delivery->raw_mode : 0;
    e->xa_file = delivery ? delivery->xa_file : 0;
    e->xa_channel = delivery ? delivery->xa_channel : 0;
    e->xa_submode = delivery ? delivery->xa_submode : 0;
    e->xa_coding = delivery ? delivery->xa_coding : 0;
    e->data_delivered = delivery ? delivery->data_delivered : 0;
    e->xa_audio_delivered = delivery ? delivery->xa_audio_delivered : 0;
    e->skip_reason = delivery ? delivery->skip_reason : CDROM_SKIP_NONE;
    e->bytes_len = (uint16_t)((size < CDROM_SECTOR_HISTORY_BYTES)
        ? size : CDROM_SECTOR_HISTORY_BYTES);
    if (e->bytes_len && bytes) {
        memcpy(e->bytes, bytes, e->bytes_len);
    }
    if (e->bytes_len < CDROM_SECTOR_HISTORY_BYTES) {
        memset(e->bytes + e->bytes_len, 0,
               CDROM_SECTOR_HISTORY_BYTES - e->bytes_len);
    }
}

static int has_disc(void) {
    return iso_handle != NULL;
}

/* CD status bits */
#define CDSTAT_ERROR    0x01
#define CDSTAT_MOTOR    0x02
#define CDSTAT_SEEKERR  0x04
#define CDSTAT_IDERROR  0x08
#define CDSTAT_SHELL    0x10
#define CDSTAT_READ     0x20
#define CDSTAT_SEEK     0x40
#define CDSTAT_PLAY     0x80

/* IRQ types */
#define CDIRQ_DATA_READY  1
#define CDIRQ_COMPLETE    2
#define CDIRQ_ACK         3
#define CDIRQ_DATA_END    4
#define CDIRQ_ERROR       5

static void response_clear(void) {
    response_read = 0;
    response_count = 0;
}

static void response_push(uint8_t val) {
    if (response_count < RESPONSE_FIFO_SIZE) {
        response_fifo[response_count++] = val;
    }
}

static void set_irq(int type) {
    irq_flag = (uint8_t)type;
    /* New visible CD INT generation: it has not yet been presented to INTC,
     * so re-arm the latch (the delayed present below raises it once). */
    cdrom_irq_generation++;
    cdrom_intc_request_latched = 0;
    /* Arm the presentation latency: this response must NOT be presented to INTC
     * synchronously inside the guest store that triggered it (see the
     * CDROM_IRQ_PRESENT_DELAY note). It is presented at a later cdrom_advance. */
    cdrom_irq_present_delay = CDROM_IRQ_PRESENT_DELAY;
    trace_cdrom('I', 0, (uint32_t)type, 0);
    /* DEQUEUE: CD response/data event fired (aux = CD irq type). */
    event_ring_record_aux(EV_DEQ, (uint8_t)SRC_CD_IRQ, (uint32_t)type);
}

/* Present the current CD INT to the CPU interrupt controller exactly once per
 * generation. Re-presentation of the same unacked INT is suppressed by the
 * latch (this is what stops the CPS exception re-entry storm). The next
 * generation re-arms via set_irq; a controller ack re-arms via the ack path. */
/* Present the current CD INT to INTC at most once per generation. Silent
 * except when it actually raises i_stat (trace 'F', low-frequency: once per
 * generation). Suppression of a duplicate presentation is intentionally NOT
 * traced — refresh runs every cdrom_advance, so tracing it would flood the CD
 * trace ring and evict useful history. Suppression is implicit (no 'F' fires
 * between an ack and the next set_irq). */
static void present_cdrom_irq(void) {
    /* Presentation latency: hold off raising i_stat until the response's
     * processing delay has elapsed (decremented in cdrom_advance). This is what
     * keeps a freshly-issued response from being presented synchronously inside
     * the guest's command/ack write — and thus from being lost to that ISR's
     * own trailing INTC ack. */
    if (cdrom_irq_present_delay > 0) return;
    if (cdrom_irq_mask_matches_reason(irq_enable, irq_flag) &&
        !cdrom_intc_request_latched) {
        psx_irq_raise(2, irq_flag); /* IRQ_CDROM; detail = CD response/IRQ type */
        cdrom_intc_request_latched = 1;
        cdrom_intc_latched_generation = cdrom_irq_generation;
        event_ring_record(EV_ISTAT_RAISE, 2 /* IRQ_CDROM bit */);
        trace_cdrom('F', 0, irq_flag, 0);
        if (irq_flag == CDIRQ_DATA_READY) cd_timing_note_intc();
    }
}

/* Fire CDROM IRQ into the interrupt controller (explicit, per command/response).
 * Trace the masked case here only (low-frequency); refresh stays silent. */
static void fire_cdrom_irq(void) {
    if (irq_flag && !cdrom_irq_mask_matches_reason(irq_enable, irq_flag)) {
        trace_cdrom('f', 0, irq_flag, 0);
    }
    present_cdrom_irq();
}

static void refresh_cdrom_irq_line(void) {
    present_cdrom_irq();
}

static int bcd_to_bin(uint8_t bcd) {
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}

static int msf_to_lba(int m, int s, int f) {
    return (m * 60 + s) * 75 + f - 150;
}

static CdTimingRecord *cd_timing_lookup(uint64_t seq) {
    if (seq == UINT64_MAX || seq >= s_cd_timing_total ||
        s_cd_timing_total - seq > CD_TIMING_CAP) return NULL;
    CdTimingRecord *r = &s_cd_timing[seq & (CD_TIMING_CAP - 1u)];
    return r->seq == seq ? r : NULL;
}

static uint64_t cd_timing_begin_sector(int lba) {
    uint64_t seq = s_cd_timing_total++;
    CdTimingRecord *r = &s_cd_timing[seq & (CD_TIMING_CAP - 1u)];
    memset(r, 0, sizeof(*r));
    r->seq = seq;
    r->due_cycle = s_cd_timing_next_due ? s_cd_timing_next_due : psx_cycle_count;
    r->buffer_cycle = psx_cycle_count;
    r->frame = (uint32_t)s_frame_count;
    r->lba = lba;
    return seq;
}

static void cd_timing_flag(uint64_t seq, uint8_t flags) {
    CdTimingRecord *r = cd_timing_lookup(seq);
    if (r) r->flags |= flags;
}

static void cd_timing_arm_irq(uint64_t seq) {
    CdTimingRecord *r = cd_timing_lookup(seq);
    if (!r) return;
    r->flags |= CDT_IRQ_ARMED;
    r->irq_arm_cycle = psx_cycle_count;
    s_cd_timing_irq_seq = seq;
}

static void cd_timing_note_intc(void) {
    CdTimingRecord *r = cd_timing_lookup(s_cd_timing_irq_seq);
    if (!r || (r->flags & CDT_INTC)) return;
    r->flags |= CDT_INTC;
    r->intc_cycle = psx_cycle_count;
}

void cdrom_timing_reset(void) {
    s_cd_timing_reset_seq = s_cd_timing_total;
    s_cd_timing_stream_starts = 0;
    s_cd_timing_reset_cycle = psx_cycle_count;
    s_cd_probe_read_start_count = s_cd_probe_read_start_cycles = 0;
    s_cd_probe_pause_count = s_cd_probe_pause_cycles = 0;
    s_cd_probe_seek_count = s_cd_probe_seek_cycles = 0;
    s_cd_probe_motor_count = s_cd_probe_motor_cycles = 0;
    s_cd_probe_stop_count = s_cd_probe_stop_cycles = 0;
}

void cdrom_timing_stats_json(char *out, int cap) {
    if (!out || cap <= 0) return;
    uint64_t start = s_cd_timing_reset_seq;
    uint64_t dropped = 0;
    if (s_cd_timing_total - start > CD_TIMING_CAP) {
        dropped = s_cd_timing_total - start - CD_TIMING_CAP;
        start = s_cd_timing_total - CD_TIMING_CAP;
    }
    uint64_t data = 0, exact = 0, early = 0, late = 0;
    uint64_t late_sum = 0, late_max = 0;
    uint64_t armed = 0, arm_sum = 0, arm_max = 0;
    uint64_t exposed = 0, exposure_sum = 0, exposure_max = 0;
    uint64_t pended = 0, lost = 0, dma = 0, over_sector = 0;
    uint64_t first_due = UINT64_MAX, last_buffer = 0;
    uint64_t bins[5] = {0, 0, 0, 0, 0};
    for (uint64_t seq = start; seq < s_cd_timing_total; seq++) {
        CdTimingRecord *r = cd_timing_lookup(seq);
        if (!r || !(r->flags & CDT_DATA)) continue;
        data++;
        if (r->due_cycle < first_due) first_due = r->due_cycle;
        if (r->buffer_cycle > last_buffer) last_buffer = r->buffer_cycle;
        if (r->buffer_cycle < r->due_cycle) {
            early++;
        } else {
            uint64_t d = r->buffer_cycle - r->due_cycle;
            if (d == 0) { exact++; bins[0]++; }
            else {
                late++; late_sum += d; if (d > late_max) late_max = d;
                if (d <= 64u) bins[1]++;
                else if (d <= 1024u) bins[2]++;
                else if (d <= 5000u) bins[3]++;
                else bins[4]++;
            }
        }
        if (r->flags & CDT_IRQ_ARMED) {
            uint64_t d = r->irq_arm_cycle - r->buffer_cycle;
            armed++; arm_sum += d; if (d > arm_max) arm_max = d;
        }
        if (r->flags & CDT_INTC) {
            uint64_t d = r->intc_cycle - r->buffer_cycle;
            exposed++; exposure_sum += d;
            if (d > exposure_max) exposure_max = d;
            if (d > CDROM_SINGLE_SPEED_SECTOR_CYCLES) over_sector++;
        }
        if (r->flags & CDT_PENDED) pended++;
        if (r->flags & CDT_LOST) lost++;
        if (r->flags & CDT_DMA) dma++;
    }
    snprintf(out, (size_t)cap,
             "\"stream_starts\":%llu,\"records\":%llu,\"dropped\":%llu,"
             "\"data_sectors\":%llu,\"schedule_exact\":%llu,"
             "\"schedule_early\":%llu,\"schedule_late\":%llu,"
             "\"late_cycles_avg\":%llu,\"late_cycles_max\":%llu,"
             "\"late_bins\":[%llu,%llu,%llu,%llu,%llu],"
             "\"irq_armed\":%llu,\"arm_delay_avg\":%llu,"
             "\"arm_delay_max\":%llu,\"intc_exposed\":%llu,"
             "\"exposure_delay_avg\":%llu,\"exposure_delay_max\":%llu,"
             "\"exposure_over_1x_sector\":%llu,\"pended\":%llu,"
             "\"lost\":%llu,\"dma_refills\":%llu,"
             "\"data_span_cycles\":%llu,\"window_cycles\":%llu,"
             "\"read_start_count\":%llu,\"read_start_cycles\":%llu,"
             "\"pause_count\":%llu,\"pause_cycles\":%llu,"
             "\"seek_count\":%llu,\"seek_cycles\":%llu,"
             "\"motor_count\":%llu,\"motor_cycles\":%llu,"
             "\"stop_count\":%llu,\"stop_cycles\":%llu,"
             "\"latency_upper_cycles\":%llu",
             (unsigned long long)s_cd_timing_stream_starts,
             (unsigned long long)(s_cd_timing_total - s_cd_timing_reset_seq),
             (unsigned long long)dropped, (unsigned long long)data,
             (unsigned long long)exact, (unsigned long long)early,
             (unsigned long long)late,
             (unsigned long long)(late ? late_sum / late : 0),
             (unsigned long long)late_max,
             (unsigned long long)bins[0], (unsigned long long)bins[1],
             (unsigned long long)bins[2], (unsigned long long)bins[3],
             (unsigned long long)bins[4], (unsigned long long)armed,
             (unsigned long long)(armed ? arm_sum / armed : 0),
             (unsigned long long)arm_max, (unsigned long long)exposed,
             (unsigned long long)(exposed ? exposure_sum / exposed : 0),
             (unsigned long long)exposure_max,
             (unsigned long long)over_sector, (unsigned long long)pended,
             (unsigned long long)lost, (unsigned long long)dma,
             (unsigned long long)(first_due != UINT64_MAX && last_buffer >= first_due
                                  ? last_buffer - first_due : 0),
             (unsigned long long)(psx_cycle_count - s_cd_timing_reset_cycle),
             (unsigned long long)s_cd_probe_read_start_count,
             (unsigned long long)s_cd_probe_read_start_cycles,
             (unsigned long long)s_cd_probe_pause_count,
             (unsigned long long)s_cd_probe_pause_cycles,
             (unsigned long long)s_cd_probe_seek_count,
             (unsigned long long)s_cd_probe_seek_cycles,
             (unsigned long long)s_cd_probe_motor_count,
             (unsigned long long)s_cd_probe_motor_cycles,
             (unsigned long long)s_cd_probe_stop_count,
             (unsigned long long)s_cd_probe_stop_cycles,
             (unsigned long long)(s_cd_probe_read_start_cycles +
                                  s_cd_probe_pause_cycles +
                                  s_cd_probe_seek_cycles +
                                  s_cd_probe_motor_cycles +
                                  s_cd_probe_stop_cycles));
}

static uint8_t bin_to_bcd(int val) {
    return (uint8_t)(((val / 10) << 4) | (val % 10));
}

static void lba_to_msf(int lba, int pregap, int* m, int* s, int* f) {
    int frames = lba + pregap;
    if (frames < 0) frames = 0;
    *m = frames / (60 * 75);
    frames %= 60 * 75;
    *s = frames / 75;
    *f = frames % 75;
}

static int16_t clamp16_cd(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static void xa_reset_decode(void) {
    xa_hist_l[0] = xa_hist_l[1] = 0;
    xa_hist_r[0] = xa_hist_r[1] = 0;
    xa_stream_file = 0xFF;
    xa_stream_channel = 0xFF;
    xa_stream_coding = 0xFF;
    xa_stream_active = 0;
}

/* CD-audio volume matrix ([data_source][output_port], 0x80 = unity) applied
 * to every decoded XA/CD-DA sample, exactly as the CD controller does on
 * real hardware (Beetle PS_CDC::ApplyVolume, cdc.cpp:524). Written via the
 * index-2/3 register banks and latched by the "apply changes" bit; games
 * drive it CONSTANTLY — X5 fades music between scenes with it (measured
 * 0x7E steady, ramping to 0x00 at transitions on the Beetle oracle). Not
 * modeling it left fades missing and steady levels ~0.6 dB hot. */
static uint8_t cd_pending_vol[2][2] = { { 0x80, 0x00 }, { 0x00, 0x80 } };
static uint8_t cd_decode_vol[2][2]  = { { 0x80, 0x00 }, { 0x00, 0x80 } };

static void cd_apply_decode_volume(int16_t *stereo, int frames) {
    /* Fast path: identity matrix (the reset state). */
    if (cd_decode_vol[0][0] == 0x80 && cd_decode_vol[1][1] == 0x80 &&
        cd_decode_vol[0][1] == 0x00 && cd_decode_vol[1][0] == 0x00)
        return;
    for (int i = 0; i < frames; i++) {
        int32_t l = stereo[i * 2 + 0];
        int32_t r = stereo[i * 2 + 1];
        int32_t lo = ((l * cd_decode_vol[0][0]) >> 7) + ((r * cd_decode_vol[1][0]) >> 7);
        int32_t ro = ((l * cd_decode_vol[0][1]) >> 7) + ((r * cd_decode_vol[1][1]) >> 7);
        stereo[i * 2 + 0] = clamp16_cd(lo);
        stereo[i * 2 + 1] = clamp16_cd(ro);
    }
}

static int xa_decode_sector_4bit_stereo(const uint8_t* data, int16_t* out) {
    static const int k0[5] = { 0, 60, 115, 98, 122 };
    static const int k1[5] = { 0, 0, -52, -55, -60 };
    int pair = 0;

    for (int g = 0; g < XA_SOUND_GROUPS; g++) {
        const uint8_t* grp = data + g * 128;
        for (int blk = 0; blk < 4; blk++) {
            uint8_t hdr_l = grp[4 + blk * 2];
            uint8_t hdr_r = grp[4 + blk * 2 + 1];
            int shift_l = 12 - (int)(hdr_l & 0x0F);
            int shift_r = 12 - (int)(hdr_r & 0x0F);
            int filter_l = (hdr_l >> 4) & 0x03;
            int filter_r = (hdr_r >> 4) & 0x03;
            if (shift_l < 0) shift_l = 0;
            if (shift_r < 0) shift_r = 0;

            for (int i = 0; i < 28; i++) {
                uint8_t b = grp[16 + blk + i * 4];
                int32_t nib_l = b & 0x0F;
                int32_t nib_r = (b >> 4) & 0x0F;
                if (nib_l >= 8) nib_l -= 16;
                if (nib_r >= 8) nib_r -= 16;

                int32_t sample_l = (nib_l << shift_l)
                    + ((k0[filter_l] * xa_hist_l[0] + k1[filter_l] * xa_hist_l[1] + 32) >> 6);
                int32_t sample_r = (nib_r << shift_r)
                    + ((k0[filter_r] * xa_hist_r[0] + k1[filter_r] * xa_hist_r[1] + 32) >> 6);
                sample_l = clamp16_cd(sample_l);
                sample_r = clamp16_cd(sample_r);
                xa_hist_l[1] = xa_hist_l[0];
                xa_hist_l[0] = sample_l;
                xa_hist_r[1] = xa_hist_r[0];
                xa_hist_r[0] = sample_r;

                out[pair * 2 + 0] = (int16_t)sample_l;
                out[pair * 2 + 1] = (int16_t)sample_r;
                pair++;
            }
        }
    }

    return pair;
}

static int xa_decode_sector_4bit_mono(const uint8_t* data, int16_t* out) {
    static const int k0[5] = { 0, 60, 115, 98, 122 };
    static const int k1[5] = { 0, 0, -52, -55, -60 };
    int pair = 0;

    for (int g = 0; g < XA_SOUND_GROUPS; g++) {
        const uint8_t* grp = data + g * 128;
        for (int blk = 0; blk < 4; blk++) {
            for (int nibble = 0; nibble < 2; nibble++) {
                uint8_t hdr = grp[4 + blk * 2 + nibble];
                int shift = 12 - (int)(hdr & 0x0F);
                int filter = (hdr >> 4) & 0x03;
                if (shift < 0) shift = 0;

                for (int i = 0; i < 28; i++) {
                    uint8_t b = grp[16 + blk + i * 4];
                    int32_t sample_nibble = nibble ? ((b >> 4) & 0x0F) : (b & 0x0F);
                    if (sample_nibble >= 8) sample_nibble -= 16;

                    int32_t sample = (sample_nibble << shift)
                        + ((k0[filter] * xa_hist_l[0] + k1[filter] * xa_hist_l[1] + 32) >> 6);
                    sample = clamp16_cd(sample);
                    xa_hist_l[1] = xa_hist_l[0];
                    xa_hist_l[0] = sample;

                    out[pair * 2 + 0] = (int16_t)sample;
                    out[pair * 2 + 1] = (int16_t)sample;
                    pair++;
                }
            }
        }
    }

    xa_hist_r[0] = xa_hist_l[0];
    xa_hist_r[1] = xa_hist_l[1];
    return pair;
}

static int xa_resample_to_44100(const int16_t* in, int in_frames,
                                int sample_rate, int16_t* out, int max_frames) {
    if (!in || !out || in_frames <= 0 || sample_rate <= 0 || max_frames <= 0) return 0;
    int out_frames = 0;
    int in_pos = 0;
    int phase = 0;

    while (in_pos < in_frames && out_frames < max_frames) {
        int next_pos = (in_pos + 1 < in_frames) ? in_pos + 1 : in_pos;
        int32_t cur_l = in[in_pos * 2 + 0];
        int32_t cur_r = in[in_pos * 2 + 1];
        int32_t next_l = in[next_pos * 2 + 0];
        int32_t next_r = in[next_pos * 2 + 1];
        out[out_frames * 2 + 0] = (int16_t)(cur_l + ((next_l - cur_l) * phase) / 44100);
        out[out_frames * 2 + 1] = (int16_t)(cur_r + ((next_r - cur_r) * phase) / 44100);
        out_frames++;

        phase += sample_rate;
        while (phase >= 44100) {
            phase -= 44100;
            in_pos++;
        }
    }

    return out_frames;
}

/* Always-on XA zero-run scanner (audio_trace event ring). Decoded XA music
 * must not contain long exact-zero spans when the source sectors are dense;
 * a run here localizes corruption to a pipeline stage (stage 0 = straight
 * out of the ADPCM decoder at native rate, stage 1 = after resample +
 * decode-volume, i.e. exactly what spu_cd_audio_push receives). */
static void xa_zero_scan(const int16_t *stereo, int frames, int lba,
                         int stage) {
    int run = 0, start = 0;
    for (int i = 0; i <= frames; i++) {
        int z = (i < frames) && stereo[i * 2 + 0] == 0 && stereo[i * 2 + 1] == 0;
        if (z) {
            if (!run) start = i;
            run++;
        } else if (run) {
            if (run >= 64)
                audio_trace_event(AUDIO_EV_XA_ZERO, (uint32_t)lba,
                                  ((uint32_t)stage << 28) |
                                  ((uint32_t)(start & 0x3FFF) << 14) |
                                  (uint32_t)(run > 0x3FFF ? 0x3FFF : run));
            run = 0;
        }
    }
}

static int maybe_deliver_xa_audio(const uint8_t* raw_data, int lba,
                                  const CDROMSectorDelivery *delivery) {
    if (!(mode_reg & 0x40u) || !raw_data || !delivery || cd_muted) return 0;
    if (!xa_is_audio_realtime(delivery)) return 0;

    uint8_t file = delivery->xa_file;
    uint8_t channel = delivery->xa_channel;
    uint8_t coding = delivery->xa_coding;

    if ((mode_reg & 0x08u) &&
        (file != filter_file || channel != filter_channel)) {
        trace_cdrom('a', 0, ((uint32_t)file << 16) | ((uint32_t)channel << 8) | coding, 0);
        return 0;
    }

    int stereo = (coding & 0x01u) != 0;
    int rate_code = (coding >> 2) & 0x03;
    int depth_code = (coding >> 4) & 0x03;
    int sample_rate = (rate_code == 0) ? 37800 : ((rate_code == 1) ? 18900 : 0);
    if (depth_code != 0 || sample_rate == 0) {
        trace_cdrom('X', 0, ((uint32_t)file << 16) | ((uint32_t)channel << 8) | coding, 0);
        return 0;
    }

    if (!xa_stream_active ||
        xa_stream_file != file ||
        xa_stream_channel != channel ||
        xa_stream_coding != coding) {
        xa_reset_decode();
        xa_stream_file = file;
        xa_stream_channel = channel;
        xa_stream_coding = coding;
        xa_stream_active = 1;
    }

    int16_t native[XA_NATIVE_FRAMES * 2];
    int16_t pcm_44100[XA_MAX_44100_FRAMES * 2];
    int native_frames = stereo
        ? xa_decode_sector_4bit_stereo(raw_data + XA_DATA_OFFSET, native)
        : xa_decode_sector_4bit_mono(raw_data + XA_DATA_OFFSET, native);
    xa_zero_scan(native, native_frames, lba, 0);
    int out_frames = xa_resample_to_44100(native, native_frames, sample_rate,
                                          pcm_44100, XA_MAX_44100_FRAMES);
    /* Volume is applied after resampling, per PS1 hardware tests (Beetle
     * cdc.cpp GetCDAudio comment). */
    cd_apply_decode_volume(pcm_44100, out_frames);
    xa_zero_scan(pcm_44100, out_frames, lba, 1);
    spu_cd_audio_push(pcm_44100, out_frames);
    trace_cdrom('A', 0,
                ((uint32_t)file << 24) | ((uint32_t)channel << 16) |
                ((uint32_t)coding << 8) | ((uint32_t)(out_frames / 32) & 0xFFu),
                0);
    return 1;
}

static int read_sector_at(int min, int sec, int sect) {
    int lba = msf_to_lba(min, sec, sect);
    uint8_t user_data[SECTOR_SIZE];
    uint8_t raw_data[RAW_SECTOR_SIZE];
    int have_raw = 0;
    CDROMSectorDelivery delivery;
    const uint8_t *history_bytes = user_data;
    int history_size = SECTOR_SIZE;

    if (iso_handle) {
        have_raw = iso_read_raw_sector(iso_handle, lba, raw_data, RAW_SECTOR_SIZE);
        if (have_raw) {
            memcpy(user_data, raw_data + RAW_USER_DATA_OFFSET, SECTOR_SIZE);
        } else if (!iso_read_sector(iso_handle, lba, user_data, SECTOR_SIZE)) {
            memset(user_data, 0, sizeof(user_data));
        }
    } else {
        memset(user_data, 0, sizeof(user_data));
    }

    delivery = classify_raw_sector(raw_data, have_raw);
    delivery.xa_audio_delivered =
        (uint8_t)maybe_deliver_xa_audio(raw_data, lba, &delivery);
    delivery.data_delivered = 1;
    if (delivery.xa_audio_delivered ||
        ((mode_reg & 0x08u) && xa_is_audio_realtime(&delivery))) {
        delivery.data_delivered = 0;
        delivery.skip_reason = CDROM_SKIP_XA_AUDIO_REALTIME;
    }

    memset(sector_buffer, 0, sizeof(sector_buffer));
    if (delivery.data_delivered && (mode_reg & 0x20)) {
        if (have_raw) {
            memcpy(sector_buffer, raw_data + WHOLE_SECTOR_OFFSET, WHOLE_SECTOR_SIZE);
            sector_size = WHOLE_SECTOR_SIZE;
            history_bytes = sector_buffer;
            history_size = sector_size;
        } else {
            sector_buffer[0] = bin_to_bcd(min);
            sector_buffer[1] = bin_to_bcd(sec);
            sector_buffer[2] = bin_to_bcd(sect);
            sector_buffer[3] = 0x02; /* Mode 2 sector. */
            memcpy(sector_buffer + FALLBACK_SECTOR_HEADER_SIZE, user_data, SECTOR_SIZE);
            sector_size = FALLBACK_WHOLE_SECTOR_SIZE;
            history_bytes = sector_buffer;
            history_size = sector_size;
        }
    } else if (delivery.data_delivered) {
        memcpy(sector_buffer, user_data, SECTOR_SIZE);
        sector_size = SECTOR_SIZE;
        history_bytes = sector_buffer;
        history_size = sector_size;
    } else {
        sector_size = 0;
    }

    sector_read_pos = 0;
    sector_available = delivery.data_delivered ? 1 : 0;
    if (delivery.data_delivered) {
        memcpy(last_sector_buffer, sector_buffer, (size_t)sector_size);
        burst_note_sector();
        if (s_warm_route_active) s_warm_route_sectors++;
    } else {
        uint32_t copy_size = history_size < SECTOR_BUFFER_SIZE ? (uint32_t)history_size
                                                               : SECTOR_BUFFER_SIZE;
        memcpy(last_sector_buffer, history_bytes, copy_size);
        if (copy_size < SECTOR_BUFFER_SIZE) {
            memset(last_sector_buffer + copy_size, 0, SECTOR_BUFFER_SIZE - copy_size);
        }
    }
    last_sector_lba = lba;
    last_sector_size = delivery.data_delivered ? sector_size : history_size;
    last_sector_frame = (uint32_t)s_frame_count;
    last_sector_mode = mode_reg;
    last_sector_have_raw = (uint8_t)(have_raw ? 1 : 0);
    last_sector_raw_mode = delivery.raw_mode;
    last_sector_xa_file = delivery.xa_file;
    last_sector_xa_channel = delivery.xa_channel;
    last_sector_xa_submode = delivery.xa_submode;
    last_sector_xa_coding = delivery.xa_coding;
    record_sector_history(lba, history_size, mode_reg, have_raw,
                          history_bytes, &delivery);
    trace_cdrom('S', 0, (uint32_t)lba, 0);
    if (!delivery.data_delivered) {
        trace_cdrom('s', 0,
                    ((uint32_t)delivery.xa_file << 24) |
                    ((uint32_t)delivery.xa_channel << 16) |
                    ((uint32_t)delivery.xa_submode << 8) |
                    delivery.xa_coding,
                    0);
    }
    return delivery.data_delivered ? 1 : 0;
}

static void advance_msf(int* m, int* s, int* f) {
    (*f)++;
    if (*f >= 75) { *f = 0; (*s)++; }
    if (*s >= 60) { *s = 0; (*m)++; }
}

static void clear_sector_buffer(void) {
    sector_read_pos = 0;
    sector_size = 0;
    sector_available = 0;
    request_reg &= (uint8_t)~CDROM_REQUEST_BFRD;
}

/* One-deep asynchronous data-ready notification, mirroring Beetle
 * PS_CDC::SetAIP/CheckAIP (cdc.cpp:829,816): a data sector that comes due
 * while the guest still has an unacked controller INT does NOT stop disc
 * time — the sector buffer is overwritten on schedule (hardware clobbers
 * the FIFO the same way) and its INT1 pends here until the ack clears
 * irq_flag. If ANOTHER data sector lands while one is still pending, the
 * old notification is lost exactly like Beetle's "Previous notification
 * skipped" warning (counted, traced 'P'). */
static uint8_t  pending_dataready;        /* 0/1: INT1 awaiting presentation */
static uint8_t  pending_dataready_stat;   /* stat_reg snapshot at pend time */
static uint64_t s_int1_pended;            /* INT1s that had to wait for ack */
static uint64_t s_int1_lost;              /* pended INT1s replaced unseen */

/* Drive-state changes (Read/Play/Pause/Stop/Seek) cancel a pended
 * notification, matching Beetle's ClearAIP in every such command. */
static void cdrom_clear_pending_dataready(void) {
    pending_dataready = 0;
    pending_dataready_stat = 0;
    s_cd_timing_pending_seq = UINT64_MAX;
}

static void start_read_stream(uint8_t cmd) {
    cdda_playing = 0;
    cdda_track = 0;
    cdda_delay = 0;
    cdda_data_end_pending = 0;
    stat_reg &= (uint8_t)~CDSTAT_PLAY;
    clear_sector_buffer();
    /* Drive-state change cancels any pended notification (Beetle clears
     * AIP on Play/Read/Pause/Stop/Seek alike). */
    cdrom_clear_pending_dataready();
    if (mode_reg & 0x40u) {
        xa_reset_decode();
        spu_cd_audio_reset();
    }
    read_min = seek_min;
    read_sec = seek_sec;
    read_sect = seek_sect;
    read_cmd = cmd;
    read_delay = initial_read_delay_cycles();
    s_cd_probe_read_start_count++;
    s_cd_probe_read_start_cycles += (uint64_t)read_delay;
    s_cd_timing_next_due = psx_cycle_count + (uint64_t)read_delay;
    s_cd_timing_stream_starts++;
    reading = 1;
    stat_reg |= CDSTAT_READ;
    /* ENQUEUE: sector-read stream scheduled (due in read_delay cycles). A
     * content load that happens in OFF but not ON shows up as a missing
     * SRC_CD_READ enqueue here. */
    event_ring_record_aux(EV_ENQ, (uint8_t)SRC_CD_READ, (uint32_t)read_delay);
}

static void stop_read_stream(void) {
    reading = 0;
    read_cmd = 0;
    read_delay = 0;
    s_cd_timing_next_due = 0;
    cdrom_clear_pending_dataready();
}

static void stop_cdda_playback(void) {
    cdda_playing = 0;
    cdda_track = 0;
    cdda_delay = 0;
    cdda_data_end_pending = 0;
    stat_reg &= (uint8_t)~CDSTAT_PLAY;
}

static void deliver_cdda_data_end(void) {
    if (!cdda_data_end_pending || irq_flag != 0) return;
    cdda_data_end_pending = 0;
    response_clear();
    response_push(stat_reg);
    set_irq(CDIRQ_DATA_END);
    fire_cdrom_irq();
}

static int cdda_track_for_lba(uint32_t lba) {
    int count = iso_handle ? iso_track_count(iso_handle) : 0;
    int found = 0;
    for (int track = 1; track <= count; ++track) {
        if (iso_track_pregap_lba(iso_handle, track) > lba) break;
        found = track;
    }
    return found;
}

static uint32_t cdda_track_end_lba(int track) {
    int count = iso_handle ? iso_track_count(iso_handle) : 0;
    if (track > 0 && track < count)
        return iso_track_pregap_lba(iso_handle, track + 1);
    return iso_handle ? iso_sector_count(iso_handle) : 0;
}

static int start_cdda_playback(int requested_track) {
    uint32_t lba;
    int track;
    if (requested_track > 0) {
        track = requested_track;
        lba = iso_track_start_lba(iso_handle, track);
    } else {
        int pos = s_setloc_lba >= 0
            ? s_setloc_lba
            : msf_to_lba(seek_min, seek_sec, seek_sect);
        if (pos < 0) pos = 0;
        lba = (uint32_t)pos;
        track = cdda_track_for_lba(lba);
    }

    if (track <= 0 || track > iso_track_count(iso_handle) ||
        !iso_track_is_audio(iso_handle, track))
        return 0;

    stop_read_stream();
    spu_cd_audio_reset();
    cdda_data_end_pending = 0;
    cdda_playing = 1;
    cdda_track = track;
    cdda_lba = lba;
    cdda_delay = CDROM_SINGLE_SPEED_SECTOR_CYCLES;
    stat_reg = (stat_reg & ~(CDSTAT_SEEK | CDSTAT_READ)) |
               CDSTAT_MOTOR | CDSTAT_PLAY;
    return 1;
}

static void process_cdda_stream(uint32_t cycles) {
    if (!cdda_playing) {
        deliver_cdda_data_end();
        return;
    }
    cdda_delay -= (int)cycles;

    int delivered = 0;
    while (cdda_playing && cdda_delay <= 0 && delivered < 16) {
        uint8_t raw[RAW_SECTOR_SIZE];
        int16_t pcm[CDDA_SECTOR_FRAMES * 2];
        if (!iso_read_raw_sector(iso_handle, cdda_lba, raw, sizeof(raw))) {
            memset(pcm, 0, sizeof(pcm));
        } else {
            /* BIN/CUE CD-DA sectors store signed 16-bit interleaved stereo in
             * little-endian byte order. */
            for (int i = 0; i < CDDA_SECTOR_FRAMES * 2; ++i) {
                uint16_t u = (uint16_t)raw[i * 2 + 0] |
                             ((uint16_t)raw[i * 2 + 1] << 8);
                pcm[i] = (int16_t)u;
            }
        }

        if (cd_muted) memset(pcm, 0, sizeof(pcm));
        cd_apply_decode_volume(pcm, CDDA_SECTOR_FRAMES);
        spu_cd_audio_push(pcm, CDDA_SECTOR_FRAMES);
        cdda_sectors_played++;
        trace_cdrom('a', 0, cdda_lba, (uint32_t)cdda_track);

        cdda_lba++;
        delivered++;
        cdda_delay += CDROM_SINGLE_SPEED_SECTOR_CYCLES;

        uint32_t track_end = cdda_track_end_lba(cdda_track);
        if (track_end == 0 || cdda_lba >= track_end) {
            int next = cdda_track + 1;
            int count = iso_track_count(iso_handle);
            if ((mode_reg & 0x02u) || next > count ||
                !iso_track_is_audio(iso_handle, next)) {
                stop_cdda_playback();
                cdda_data_end_pending = 1;
                deliver_cdda_data_end();
            } else {
                cdda_track = next;
            }
        }
    }

    /* A very large host-side cycle jump must not create an unbounded catch-up
     * burst. Resume at the authentic next-sector cadence. */
    if (cdda_playing && cdda_delay <= 0)
        cdda_delay = CDROM_SINGLE_SPEED_SECTOR_CYCLES;

    deliver_cdda_data_end();
}

static int data_fifo_ready(void) {
    return (request_reg & CDROM_REQUEST_BFRD) &&
           sector_available &&
           sector_read_pos < sector_size;
}

static uint64_t s_dataready_fires;  /* INT1 (data-ready) raised per streamed sector — FMV dispatch probe */
uint64_t cdrom_get_dataready_fires(void) { return s_dataready_fires; }

static int deliver_read_sector(void) {
    int delivered = read_sector_at(read_min, read_sec, read_sect);
    advance_msf(&read_min, &read_sec, &read_sect);
    if (!delivered) return 0;
    response_clear();
    response_push(stat_reg);
    set_irq(CDIRQ_DATA_READY);
    fire_cdrom_irq();
    s_dataready_fires++;
    return 1;
}

static int deliver_read_sector_without_irq(void) {
    int delivered = read_sector_at(read_min, read_sec, read_sect);
    advance_msf(&read_min, &read_sec, &read_sect);
    return delivered;
}

static void try_execute_queued_command(void) {
    if (!queued_cmd.pending || irq_flag != 0) return;

    uint8_t cmd = queued_cmd.cmd;
    int count = queued_cmd.param_count;
    if (count < 0) count = 0;
    if (count > PARAM_FIFO_SIZE) count = PARAM_FIFO_SIZE;

    memcpy(param_fifo, queued_cmd.params, (size_t)count);
    param_count = count;

    queued_cmd.pending = 0;
    queued_cmd.cmd = 0;
    queued_cmd.param_count = 0;
    memset(queued_cmd.params, 0, sizeof(queued_cmd.params));

    exec_command(cmd);
}

static void queue_or_exec_command(uint8_t cmd) {
    if (irq_flag == 0) {
        exec_command(cmd);
        return;
    }

    queued_cmd.cmd = cmd;
    queued_cmd.param_count = param_count;
    if (queued_cmd.param_count < 0) queued_cmd.param_count = 0;
    if (queued_cmd.param_count > PARAM_FIFO_SIZE) queued_cmd.param_count = PARAM_FIFO_SIZE;
    memcpy(queued_cmd.params, param_fifo, (size_t)queued_cmd.param_count);
    queued_cmd.pending = 1;
    param_count = 0;
    trace_cdrom('Q', 0, cmd, 0);
    record_command_history('Q', cmd, queued_cmd.params, queued_cmd.param_count);
}

/* Pause (0x09) completion latency — Beetle PS_CDC::Command_Pause.
 *
 * A Pause issued while the drive is reading/playing completes only after the
 * head settles: (1124584 + lba*42596/4500) CPU cycles in double-speed mode,
 * doubled at single speed — roughly 1.1M+ cycles (~2 video frames). A Pause
 * issued while already paused/stopped acks its second response quickly (5000).
 *
 * This is a CPU-visible ordering contract, NOT sector cadence: games issue
 * Pause from mainline code at every streamed-file boundary and finish their
 * driver bookkeeping in the frames before the completion INT2 arrives. The
 * previous flat apply_speed(10000) delivered INT2 ~100x early — inside the
 * issuing frame — and the game's driver intermittently lost the completion
 * (Ape Escape's memcard scene loader wedged at a random file boundary, its
 * async CD queue never pumping the next file). Hence also NO apply_speed():
 * disc-speed divisors / 'instant' mode must never compress this latency back
 * into the race window. Call BEFORE stop_read_stream()/CDSTAT_READ clear. */
static int pause_complete_delay_cycles(void) {
    if (!reading && !(stat_reg & (CDSTAT_READ | CDSTAT_PLAY)))
        return 5000;
    int lba = reading ? msf_to_lba(read_min, read_sec, read_sect)
                      : last_sector_lba;
    if (lba < 0) lba = 0;
    int64_t cycles = 1124584 + (int64_t)lba * 42596 / (75 * 60);
    if (!(mode_reg & 0x80))
        cycles *= 2;
    return (int)cycles;
}

static void exec_command(uint8_t cmd) {
#if !defined(PSX_NO_DEBUG_TOOLS) && !defined(_WIN32)
    /* Self-stop trap: PSX_CD_TRAP_CMD=<byte> makes the process SIGSTOP
     * itself the moment that CD command dispatches, so a debugger can
     * attach and read the full native+guest call chain at the exact
     * instant with ZERO run-speed cost. (A gdb conditional breakpoint on
     * this function slows the emu two orders of magnitude via ptrace —
     * unusable for wedges that need minutes of healthy boot first.) */
    {
        static long trap_cmd = -2, trap_nth = 1, trap_hits = 0;
        if (trap_cmd == -2) {
            const char *e = getenv("PSX_CD_TRAP_CMD");
            trap_cmd = (e && *e) ? strtol(e, NULL, 0) : -1;
            e = getenv("PSX_CD_TRAP_NTH");   /* stop on the Nth match (default 1st) */
            if (e && *e) trap_nth = strtol(e, NULL, 0);
        }
        if ((long)cmd == trap_cmd && ++trap_hits == trap_nth) raise(SIGSTOP);
    }
#endif
    trace_cdrom('C', 0, cmd, 0);
    /* ENQUEUE: a CD command was issued (aux = command byte). */
    event_ring_record_aux(EV_ENQ, (uint8_t)SRC_CD_CMD, (uint32_t)cmd);
    /* Snapshot the param FIFO before handlers consume it, so the
     * command-history record at the end sees the original params. */
    uint8_t cmd_params[PARAM_FIFO_SIZE];
    int cmd_param_count = param_count;
    if (cmd_param_count < 0) cmd_param_count = 0;
    if (cmd_param_count > PARAM_FIFO_SIZE) cmd_param_count = PARAM_FIFO_SIZE;
    memcpy(cmd_params, param_fifo, (size_t)cmd_param_count);
    response_clear();

    switch (cmd) {
    case 0x01: /* GetStat */
        if (has_disc()) {
            stat_reg |= CDSTAT_MOTOR;
        } else {
            stat_reg |= CDSTAT_SHELL;
        }
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        break;

    case 0x02: /* SetLoc */
        if (param_count >= 3) {
            int current_lba = (last_sector_lba >= 0)
                ? last_sector_lba
                : msf_to_lba(seek_min, seek_sec, seek_sect);
            seek_min = bcd_to_bin(param_fifo[0]);
            seek_sec = bcd_to_bin(param_fifo[1]);
            seek_sect = bcd_to_bin(param_fifo[2]);
            s_setloc_lba = msf_to_lba(seek_min, seek_sec, seek_sect);
            setloc_seek_far = (abs(current_lba - s_setloc_lba) > 16) ? 1 : 0;
            warm_route_on_setloc(s_setloc_lba);
        }
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        break;

    case 0x06: /* ReadN */
        if (!has_disc()) {
            response_push(stat_reg | CDSTAT_ERROR);
            set_irq(CDIRQ_ERROR);
            break;
        }
        start_read_stream(cmd);
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        break;

    case 0x07: /* MotorOn — spin the drive motor up. Two-phase like Stop:
                * INT3 (ACK) now with the current status, then a pending INT2
                * (COMPLETE) after spin-up reporting status with the motor bit
                * SET (psx-spx "07h MotorOn"). If the motor is ALREADY spinning
                * the hardware rejects the command with INT5 (ERROR) and error
                * code 0x20 ("wrong condition") — replicate that.
                *
                * Previously 0x07 had no case and fell through to default ->
                * CDIRQ_ERROR (INT5) with no error code, so a game that spins the
                * motor up (e.g. after a Stop) and waits for the MotorOn
                * completion IRQ would never see it. */
        if (stat_reg & CDSTAT_MOTOR) {
            response_push(stat_reg | CDSTAT_ERROR);
            response_push(0x20); /* error code: motor already on */
            set_irq(CDIRQ_ERROR);
            break;
        }
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        pending.cmd = 0x07;
        pending.pending = 1;
        pending.delay = apply_speed(30000); /* motor spin-up */
        s_cd_probe_motor_count++;
        s_cd_probe_motor_cycles += (uint64_t)pending.delay;
        pending.phase = 1;
        break;

    case 0x08: /* Stop — stop the motor. Two-phase like Pause: INT3 (ACK) now
                * with the pre-stop status (motor still spinning), then a pending
                * INT2 (COMPLETE) after the motor spins down, reporting the new
                * status with the motor bit cleared (psx-spx "08h Stop").
                *
                * Previously 0x08 had no case and fell through to default ->
                * CDIRQ_ERROR (INT5). A game that stops the drive on a scene
                * change then waits for the Stop completion IRQ never sees it and
                * hangs: Tsumu Light's CD library retries Stop forever (~90-frame
                * timeout) and never advances past its first content load. */
        stop_read_stream();
        stop_cdda_playback();
        xa_reset_decode();
        spu_cd_audio_reset();
        stat_reg &= ~(CDSTAT_READ | CDSTAT_PLAY | CDSTAT_SEEK);
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        pending.cmd = 0x08;
        pending.pending = 1;
        pending.delay = apply_speed(30000); /* motor spin-down */
        s_cd_probe_stop_count++;
        s_cd_probe_stop_cycles += (uint64_t)pending.delay;
        pending.phase = 1;
        break;

    case 0x09: { /* Pause */
        int complete_delay = pause_complete_delay_cycles();
        stop_read_stream();
        stop_cdda_playback();
        xa_reset_decode();
        spu_cd_audio_reset();
        stat_reg &= ~(CDSTAT_READ | CDSTAT_PLAY);
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        pending.cmd = 0x09;
        pending.pending = 1;
        pending.delay = complete_delay;
        s_cd_probe_pause_count++;
        s_cd_probe_pause_cycles += (uint64_t)complete_delay;
        pending.phase = 1;
        break;
    }

    case 0x0A: /* Init */
        stop_read_stream();
        stop_cdda_playback();
        spu_cd_audio_reset();
        xa_reset_decode();
        stat_reg = has_disc() ? CDSTAT_MOTOR : CDSTAT_SHELL;
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        pending.cmd = 0x0A;
        pending.pending = 1;
        /* Beetle models the drive-reset busy period as 1136000 cycles
         * (PS_CDC::Command_Reset, PSRCounter). Same second-response class as
         * Pause: an authentic multi-frame latency games' drivers rely on —
         * never scaled by the disc-speed divisor. */
        pending.delay = 1136000;
        pending.phase = 1;
        break;

    case 0x0B: /* Mute */
        cd_muted = 1;
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        break;

    case 0x0C: /* Demute */
        cd_muted = 0;
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        break;

    case 0x0D: /* SetFilter */
        if (param_count >= 2) {
            filter_file = param_fifo[0];
            filter_channel = param_fifo[1];
            xa_reset_decode();
        }
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        break;

    case 0x0E: /* SetMode */
        if (param_count >= 1) {
            mode_reg = param_fifo[0];
            if (!(mode_reg & 0x40u)) {
                xa_reset_decode();
                spu_cd_audio_reset();
            }
        }
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        break;

    case 0x0F: /* Getparam */
        response_push(stat_reg);
        response_push(mode_reg);
        response_push(0x00);
        response_push(filter_file);
        response_push(filter_channel);
        set_irq(CDIRQ_ACK);
        break;

    case 0x10: { /* GetlocL */
        int lba = (last_sector_lba >= 0)
            ? last_sector_lba
            : msf_to_lba(read_min, read_sec, read_sect);
        int m, s, f;
        lba_to_msf(lba, 150, &m, &s, &f);
        response_push(bin_to_bcd(m));
        response_push(bin_to_bcd(s));
        response_push(bin_to_bcd(f));
        response_push(last_sector_raw_mode ? last_sector_raw_mode : CDROM_SECTOR_MODE2);
        response_push(last_sector_xa_file);
        response_push(last_sector_xa_channel);
        response_push(last_sector_xa_submode);
        response_push(last_sector_xa_coding);
        set_irq(CDIRQ_ACK);
        break;
    }

    case 0x11: { /* GetlocP */
        int lba;
        int track = 1;
        int track_lba = 0;
        if (cdda_playing) {
            lba = (int)cdda_lba;
            track = cdda_track;
            track_lba = (int)iso_track_start_lba(iso_handle, track);
        } else if (reading) {
            /* GetlocP reports the drive/sub-Q position. During a read the
             * sector stream has already advanced past the data-ready sector. */
            lba = msf_to_lba(read_min, read_sec, read_sect);
        } else if (last_sector_lba >= 0) {
            lba = last_sector_lba;
        } else {
            lba = msf_to_lba(seek_min, seek_sec, seek_sect);
        }
        int rm, rs, rf;
        int am, as, af;
        lba_to_msf(lba - track_lba, 0, &rm, &rs, &rf);
        lba_to_msf(lba, 150, &am, &as, &af);
        response_push(bin_to_bcd(track));
        response_push(0x01);
        response_push(bin_to_bcd(rm));
        response_push(bin_to_bcd(rs));
        response_push(bin_to_bcd(rf));
        response_push(bin_to_bcd(am));
        response_push(bin_to_bcd(as));
        response_push(bin_to_bcd(af));
        set_irq(CDIRQ_ACK);
        break;
    }

    case 0x13: /* GetTN — first + last track numbers (BCD) */
        response_push(stat_reg);
        response_push(0x01); /* first track is always 1 */
        {
            int last = iso_handle ? iso_track_count(iso_handle) : 1;
            if (last < 1) last = 1;
            response_push(bin_to_bcd(last));
        }
        set_irq(CDIRQ_ACK);
        break;

    case 0x14: { /* GetTD — start MSF (BCD) of a track; track 0/0xAA = lead-out */
        if (!has_disc()) {
            response_push(stat_reg | CDSTAT_ERROR);
            response_push(0x80);
            set_irq(CDIRQ_ERROR);
            break;
        }

        int raw = (param_count >= 1) ? param_fifo[0] : 0;
        int track = bcd_to_bin(raw);
        int lba;
        if (raw == 0xAA || track == 0) {
            /* Lead-out: end of the whole image. */
            uint32_t sectors = iso_sector_count(iso_handle);
            lba = sectors ? (int)sectors : 0;
        } else {
            /* .bin-relative start LBA of the requested track (0 for track 1
             * data; the CD-DA audio track's real start for multi-track discs). */
            lba = iso_handle ? (int)iso_track_start_lba(iso_handle, track) : 0;
        }

        int m, s, f;
        (void)f;
        lba_to_msf(lba, 150, &m, &s, &f);
        response_push(stat_reg);
        response_push(bin_to_bcd(m));
        response_push(bin_to_bcd(s));
        set_irq(CDIRQ_ACK);
        break;
    }

    case 0x03: /* Play — start CD-DA audio playback from SetLoc, or from the
                * optional BCD track number in param[0]. */
        if (!has_disc()) {
            response_push(stat_reg | CDSTAT_ERROR);
            set_irq(CDIRQ_ERROR);
            break;
        }
        {
            int requested_track = 0;
            if (param_count >= 1 && param_fifo[0] != 0)
                requested_track = bcd_to_bin(param_fifo[0]);
            if (!start_cdda_playback(requested_track)) {
                if (requested_track == 0) {
                    /* Some games probe Play from a data-track SetLoc before
                     * selecting their real audio track. The previous CD model
                     * acknowledged that command and entered a silent PLAY
                     * state; rejecting it makes Tomba 2 reset and retry its CD
                     * initialization forever. Preserve that compatibility
                     * behavior without feeding data sectors to the SPU. */
                    stop_read_stream();
                    cdda_data_end_pending = 0;
                    stat_reg = (stat_reg & ~(CDSTAT_SEEK | CDSTAT_READ)) |
                               CDSTAT_MOTOR | CDSTAT_PLAY;
                } else {
                    response_push(stat_reg | CDSTAT_ERROR);
                    response_push(0x10); /* invalid explicit track */
                    set_irq(CDIRQ_ERROR);
                    break;
                }
            }
        }
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        break;

    case 0x15: /* SeekL (data-mode seek, uses sector headers) */
    case 0x16: /* SeekP (audio-mode seek, uses subchannel Q) — this model seeks
                * to the SetLoc position the same way for both commands. */
        if (!has_disc()) {
            response_push(stat_reg | CDSTAT_ERROR | CDSTAT_SEEKERR);
            set_irq(CDIRQ_ERROR);
            break;
        }
        xa_reset_decode();
        spu_cd_audio_reset();
        stop_cdda_playback();
        stat_reg |= CDSTAT_SEEK;
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        pending.cmd = cmd;   /* 0x15 or 0x16 — completed in process_pending */
        pending.pending = 1;
        pending.delay = seek_complete_delay_cycles();
        s_cd_probe_seek_count++;
        s_cd_probe_seek_cycles += (uint64_t)pending.delay;
        pending.phase = 1;
        break;

    case 0x1A: /* GetID */
        /* GetID always sends INT3 (ACK) first, then a pending
         * second response: INT2 (COMPLETE) with disc ID if present,
         * or INT5 (ERROR) if no disc / lid open. */
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        pending.cmd = 0x1A;
        pending.pending = 1;
        /* Beetle PS_CDC::Command_ID: second response after 33868 cycles.
         * Unscaled — same authentic-latency class as Pause/Init/ReadTOC. */
        pending.delay = 33868;
        pending.phase = 1;
        break;

    case 0x1B: /* ReadS */
        if (!has_disc()) {
            response_push(stat_reg | CDSTAT_ERROR);
            set_irq(CDIRQ_ERROR);
            break;
        }
        start_read_stream(cmd);
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        break;

    case 0x1E: /* ReadTOC */
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        pending.cmd = 0x1E;
        pending.pending = 1;
        /* Beetle PS_CDC::Command_ReadTOC: ~30M cycles (a near-second TOC
         * re-scan; Beetle adds a seek term on top — we keep the dominant
         * constant). Unscaled — authentic-latency class. */
        pending.delay = 30000000;
        pending.phase = 1;
        break;

    case 0x19: /* Test */
        if (param_count >= 1 && param_fifo[0] == 0x20) {
            /* CD controller firmware version (BCD date + region). This BIOS is
             * SCPH-1001, whose sub-CPU reports the 1994 controller: 94/09/19 C0.
             * The value must be < 0x95 in the high byte — the shell's CD-init
             * (func at ROM 0x1DF50) sets kernel flag [0xA000DFFC]=1 when the
             * version byte >= 0x95, which later makes the boot CD-open
             * (0xBFC0D570) issue a spurious ReadTOC that wedges the game's
             * streaming reads. Beetle hardcodes the PSone-era 0x97 regardless of
             * BIOS, which is wrong for SCPH-1001; matching the real 1994
             * controller keeps the flag clear (Kula World demo-load wedge). */
            response_push(0x94);
            response_push(0x09);
            response_push(0x19);
            response_push(0xC0);
            set_irq(CDIRQ_ACK);
        } else {
            response_push(stat_reg);
            set_irq(CDIRQ_ACK);
        }
        break;

    default:
        response_push(stat_reg | CDSTAT_ERROR);
        set_irq(CDIRQ_ERROR);
        break;
    }

    param_count = 0;

    /* Fire the CDROM IRQ for the immediate response.
     * Delayed responses (pending) fire from process_pending(). */
    fire_cdrom_irq();
    record_command_history('C', cmd, cmd_params, cmd_param_count);
}

static void process_pending(uint32_t cycles) {
    if (!pending.pending) return;

    if (cycles == 0) return;
    /* Serialized responses: a delayed completion (e.g. SeekL / Init COMPLETE)
     * cannot be presented while a prior response IRQ is still unacked. Freeze
     * pending.delay while held so it does not accrue fake time debt and
     * underflow once the guest finally acks (mirrors process_read_stream). */
    if (irq_flag != 0) return;
    pending.delay -= (int)cycles;
    if (pending.delay > 0) return;

    pending.pending = 0;
    response_clear();

    switch (pending.cmd) {
    case 0x06: /* ReadN data ready */
    case 0x1B: /* ReadS data ready */
        if (reading) {
            int delivered = read_sector_at(read_min, read_sec, read_sect);
            advance_msf(&read_min, &read_sec, &read_sect);
            if (delivered) {
                response_push(stat_reg);
                set_irq(CDIRQ_DATA_READY);
                fire_cdrom_irq();
            }
        }
        break;

    case 0x07: /* MotorOn complete — motor now spinning */
        stat_reg |= CDSTAT_MOTOR;
        response_push(stat_reg);
        set_irq(CDIRQ_COMPLETE);
        fire_cdrom_irq();
        break;

    case 0x08: /* Stop complete — motor has spun down */
        stat_reg &= ~(CDSTAT_MOTOR | CDSTAT_READ | CDSTAT_PLAY | CDSTAT_SEEK);
        response_push(stat_reg);
        set_irq(CDIRQ_COMPLETE);
        fire_cdrom_irq();
        break;

    case 0x09: /* Pause complete */
        stat_reg &= ~CDSTAT_READ;
        response_push(stat_reg);
        set_irq(CDIRQ_COMPLETE);
        fire_cdrom_irq();
        break;

    case 0x0A: /* Init complete */
        response_push(stat_reg);
        set_irq(CDIRQ_COMPLETE);
        fire_cdrom_irq();
        break;

    case 0x15: /* SeekL complete */
    case 0x16: /* SeekP complete */
        stat_reg &= ~CDSTAT_SEEK;
        setloc_seek_far = 0;
        response_push(stat_reg);
        set_irq(CDIRQ_COMPLETE);
        fire_cdrom_irq();
        break;

    case 0x1A: { /* GetID result */
        if (!has_disc()) {
            response_push(stat_reg | CDSTAT_IDERROR);
            response_push(0x80);
            set_irq(CDIRQ_ERROR);
        } else {
            response_push(stat_reg);
            response_push(0x00);
            response_push(0x20);
            response_push(0x00);
            response_push(disc_scex[0]);
            response_push(disc_scex[1]);
            response_push(disc_scex[2]);
            response_push(disc_scex[3]);
            set_irq(CDIRQ_COMPLETE);
        }
        fire_cdrom_irq();
        break;
    }

    case 0x1E: /* ReadTOC complete */
        response_push(stat_reg);
        set_irq(CDIRQ_COMPLETE);
        fire_cdrom_irq();
        break;

    default:
        break;
    }
}

/* Read-stream hold accounting (always-on). Before 2026-07-10 the stream
 * FROZE read_delay whenever the guest had not yet acked the previous INT.
 * On real hardware the disc never pauses; the accumulated freeze made XA
 * sectors arrive ~1.5-3% late, starving the XA->SPU ring ~5-6x/second for
 * ~147 samples each = the audible 6 Hz music crackle (measured on MMX4's
 * attract/title XA streams). The freeze is gone; these counters must now
 * stay at zero and exist as a regression tripwire (cdrom_state JSON). */
static uint64_t s_read_hold_cycles;
static uint64_t s_read_hold_events;

/* Route-only HLE producer/consumer handshake. Do not overwrite a cached
 * sector or data-ready callback: make the next sector eligible only after the
 * guest consumes the FIFO and acknowledges the previous notification.
 * Multi-sector DMA may refill while its original data-ready IRQ is active. */
static int warm_route_consumer_blocked(void) {
    if (!s_warm_route_active) return 0;
    /* Tomba's raw-sector path intentionally consumes 12 header + 2048 data
     * bytes from a 2340-byte FIFO and leaves the final 280 bytes unread. The
     * IRQ ack (plus an inactive DMA channel), not sector_available, is the
     * authoritative consumer-complete handshake. */
    if (pending_dataready) return 1;
    if (irq_flag != 0 && !dma_cdrom_transfer_active()) return 1;
    return 0;
}

static void process_read_stream(uint32_t cycles) {
    if (!reading) return;

    if (warm_route_consumer_blocked()) {
        s_warm_route_consumer_waits++;
        s_warm_route_consumer_wait_cycles += cycles;
        return;
    }

    /* Disc time NEVER pauses while the drive reads (Beetle cdc.cpp
     * HandleSectorRead: the sector pipeline advances on cycle deadlines
     * regardless of INT ack state). XA-ADPCM realtime audio flows to the
     * SPU decoder unconditionally inside read_sector_at — gating it on the
     * guest's INT-ack latency is what caused the 6 Hz XA dropouts. */
    if (cycles > 0) {
        read_delay -= (int)cycles;
    }

    if (read_delay <= 0) {
        uint64_t timing_seq = cd_timing_begin_sector(
            msf_to_lba(read_min, read_sec, read_sect));
        if (irq_flag == 0) {
            if (sector_available) {
                trace_cdrom('O', 0, (uint32_t)sector_read_pos, 0);
            }
            if (deliver_read_sector()) {
                cd_timing_flag(timing_seq, CDT_DATA);
                cd_timing_arm_irq(timing_seq);
            }
        } else if (dma_cdrom_transfer_active()) {
            /* Active multi-sector CD DMA with the data-ready INT still
             * asserted: refill the buffer so the DMA keeps draining, no new
             * INT (the historical shape; games start the DMA inside the
             * data-ready callback before acking). */
            if (deliver_read_sector_without_irq())
                cd_timing_flag(timing_seq, CDT_DATA | CDT_DMA);
        } else {
            /* Guest hasn't acked the previous INT yet. Read the sector on
             * schedule (XA audio + buffer overwrite happen inside), and
             * pend its data-ready INT1 one deep. */
            int delivered = deliver_read_sector_without_irq();
            if (delivered) {
                cd_timing_flag(timing_seq, CDT_DATA | CDT_PENDED);
                if (pending_dataready) {
                    s_int1_lost++;
                    trace_cdrom('P', 0, (uint32_t)last_sector_lba, 0);
                    cd_timing_flag(s_cd_timing_pending_seq, CDT_LOST);
                }
                pending_dataready = 1;
                pending_dataready_stat = stat_reg;
                s_cd_timing_pending_seq = timing_seq;
                s_int1_pended++;
            }
        }
        read_delay += sector_delay_cycles();
        /* Clamp pathological underflow to one sector period: never replay a
         * catch-up burst of missed sectors, never leave a huge negative debt. */
        if (read_delay <= 0) {
            read_delay = sector_delay_cycles();
        }
        s_cd_timing_next_due = psx_cycle_count + (uint64_t)read_delay;
    }
}

/* Present a pended data-ready INT1 the moment the guest fully acks the
 * previous INT (Beetle CheckAIP: async results present as soon as the IRQ
 * register clears). Called from the irq_flag ack write. */
static void present_pending_dataready(void) {
    if (!pending_dataready || irq_flag != 0) return;
    uint64_t timing_seq = s_cd_timing_pending_seq;
    pending_dataready = 0;
    s_cd_timing_pending_seq = UINT64_MAX;
    response_clear();
    response_push(pending_dataready_stat);
    set_irq(CDIRQ_DATA_READY);
    fire_cdrom_irq();
    cd_timing_arm_irq(timing_seq);
    s_dataready_fires++;
}

void cdrom_init(const char* cue_path) {
    memset(param_fifo, 0, sizeof(param_fifo));
    memset(response_fifo, 0, sizeof(response_fifo));
    memset(sector_buffer, 0, sizeof(sector_buffer));
    memset(last_sector_buffer, 0, sizeof(last_sector_buffer));

    /* Rematch re-calls cdrom_init; boot must see 1x until game entry again. */
    g_disc_speed_divisor = 1;

    index_reg = 0;
    request_reg = 0;
    irq_enable = 0x1F;
    irq_flag = 0;
    cdrom_intc_request_latched = 0;
    cdrom_irq_generation = 0;
    cdrom_intc_latched_generation = 0;
    cdrom_irq_present_delay = 0;
    param_count = 0;
    response_read = 0;
    response_count = 0;
    sector_read_pos = 0;
    sector_size = 0;
    sector_available = 0;
    last_sector_lba = -1;
    last_sector_size = 0;
    last_sector_frame = 0;
    last_sector_mode = 0;
    last_sector_have_raw = 0;
    last_sector_raw_mode = 0;
    last_sector_xa_file = 0;
    last_sector_xa_channel = 0;
    last_sector_xa_submode = 0;
    last_sector_xa_coding = 0;
    stop_read_stream();
    stop_cdda_playback();
    cdda_lba = 0;
    cdda_sectors_played = 0;
    mode_reg = 0;
    filter_file = 0;
    filter_channel = 0;
    cd_muted = 0;
    xa_reset_decode();
    spu_cd_audio_reset();
    pending.pending = 0;
    memset(&queued_cmd, 0, sizeof(queued_cmd));
    seek_min = seek_sec = seek_sect = 0;
    setloc_seek_far = 0;
    s_warm_routes_count = 0;
    s_warm_route_configured = 0;
    s_warm_route_enabled = 0;
    s_warm_route_armed = 0;
    s_warm_route_armed_lba = -1;
    s_warm_route_active = 0;
    s_warm_route_active_index = -1;
    s_warm_route_next = 0;
    s_warm_route_last_lba = -1;
    s_warm_route_matches = 0;
    s_warm_route_mismatches = 0;
    s_warm_route_sectors = 0;
    s_warm_route_consumer_waits = 0;
    s_warm_route_consumer_wait_cycles = 0;
    cdrom_debug_clear_sector_history();

    if (cue_path) {
        /* Rematch re-opens the same disc; close the prior handle first. */
        if (iso_handle) {
            iso_close(iso_handle);
            iso_handle = NULL;
        }
        iso_handle = iso_open(cue_path);
    }

    stat_reg = has_disc() ? CDSTAT_MOTOR : CDSTAT_SHELL;
    cdrom_debug_clear_trace();
    cdrom_debug_clear_command_history();
    trace_cdrom('N', 0, has_disc() ? 1u : 0u, 0);
}

uint32_t cdrom_read(uint32_t addr) {
    uint32_t ret = 0;
    switch (addr) {
    case 0x1F801800: {
        uint8_t s = index_reg & 0x03;
        /* Bit 2 is ADPBUSY (XA-ADPCM playback in progress), NOT "ADPCM
         * empty" — it must idle at 0. The oracle (beetle cdc.cpp, Read
         * A==0) never raises it. Raising it permanently made every BIOS
         * status poll see "XA busy" and steered the kernel CD driver
         * init down a different branch from real hardware. */
        if (param_count == 0) s |= (1 << 3);
        if (param_count < PARAM_FIFO_SIZE) s |= (1 << 4);
        if (response_read < response_count) s |= (1 << 5);
        if (data_fifo_ready()) s |= (1 << 6);
        /* Bit 7 BUSYSTS: command written but not yet executed (our queued
         * path; the synchronous path leaves no guest-observable window).
         * Mirrors beetle's PendingCommandCounter/phase<=1 busy window. */
        if (queued_cmd.pending) s |= (1 << 7);
        ret = s;
        break;
    }

    case 0x1F801801:
        if (response_read < response_count) {
            ret = response_fifo[response_read++];
        }
        break;

    case 0x1F801802:
        if (data_fifo_ready()) {
            ret = sector_buffer[sector_read_pos++];
            if (sector_read_pos >= sector_size) {
                sector_available = 0;
            }
        }
        break;

    case 0x1F801803:
        if (index_reg == 0 || index_reg == 2) {
            ret = irq_enable;
        } else {
            ret = irq_flag | 0xE0;
        }
        break;

    default:
        ret = 0;
        break;
    }
    trace_cdrom('R', addr, ret, 1);
    return ret;
}

void cdrom_write(uint32_t addr, uint32_t value) {
    uint8_t val = (uint8_t)value;
    trace_cdrom('W', addr, val, 1);

    switch (addr) {
    case 0x1F801800:
        index_reg = val & 0x03;
        break;

    case 0x1F801801:
        if (index_reg == 0) {
            queue_or_exec_command(val);
        } else if (index_reg == 3) {
            cd_pending_vol[1][1] = val;   /* Right-CD -> Right-SPU */
        }
        break;

    case 0x1F801802:
        if (index_reg == 0) {
            if (param_count < PARAM_FIFO_SIZE) {
                param_fifo[param_count++] = val;
            }
        } else if (index_reg == 1) {
            irq_enable = val & 0x1F;
        } else if (index_reg == 2) {
            cd_pending_vol[0][0] = val;   /* Left-CD -> Left-SPU */
        } else if (index_reg == 3) {
            cd_pending_vol[1][0] = val;   /* Right-CD -> Left-SPU */
        }
        break;

    case 0x1F801803:
        if (index_reg == 2) {
            cd_pending_vol[0][1] = val;   /* Left-CD -> Right-SPU */
        } else if (index_reg == 3) {
            /* Apply-changes latch (bit 5) copies pending -> live, exactly
             * like Beetle cdc.cpp case 0x0B. (ADPMute bit 0 is not modeled
             * by the Beetle oracle either.) */
            if (val & 0x20) {
                memcpy(cd_decode_vol, cd_pending_vol, sizeof(cd_decode_vol));
            }
        } else if (index_reg == 0) {
            request_reg = val;
            if (!(request_reg & CDROM_REQUEST_BFRD)) {
                sector_read_pos = 0;
            }
        } else if (index_reg == 1) {
            /* Controller IRQ acknowledge. irq_flag is a single numeric response
             * code (1=INT1 data, 2=INT2, 3=INT3 ack, ...); a full ack clears it
             * to 0. When the current visible INT is fully acked, re-arm the
             * latch so the NEXT generation can be presented to INTC. Use the
             * active->inactive edge rather than a bare !=0->0 so partial-clear
             * writes don't mis-rearm. */
            int had_active_irq = (irq_flag & 0x1F) != 0;
            irq_flag &= ~(val & 0x1F);
            if (had_active_irq && (irq_flag & 0x1F) == 0) {
                cdrom_intc_request_latched = 0;
            }
            if (val & 0x40) {
                param_count = 0;
            }
            /* A fully-acked INT presents any pended data-ready first
             * (Beetle CheckAIP on IRQ-register clear); a queued command
             * then waits behind that INT1's own ack cycle. */
            present_pending_dataready();
            try_execute_queued_command();
        }
        break;

    default:
        break;
    }
}

/* Cycle-budgeted precise event slicing: guest CPU cycles until the CD-ROM
 * raises a DELIVERABLE IRQ (bit2 unmasked in i_mask). UINT32_MAX if none.
 * Conservative under-estimate (smaller => slice more => safe): returns the
 * nearest in-flight countdown that leads to an i_stat bit2 raise — the
 * presentation delay of an already-armed response, a pending second response,
 * or the next sector data-ready. See PRECISE_IRQ_SLICE.md. */
uint32_t cdrom_cycles_to_irq(uint32_t i_mask) {
    if (!(i_mask & (1u << 2))) return 0xFFFFFFFFu;   /* IRQ_CDROM masked */
    uint32_t best = 0xFFFFFFFFu;
    /* Armed response awaiting presentation (will raise bit2 when delay hits 0). */
    if (cdrom_irq_mask_matches_reason(irq_enable, irq_flag)) {
        uint32_t d = cdrom_irq_present_delay > 0 ? (uint32_t)cdrom_irq_present_delay : 0u;
        if (d < best) best = d;
    }
    /* Pending second response: fires set_irq in pending.delay cycles. */
    if (pending.pending && pending.delay > 0 && (uint32_t)pending.delay < best)
        best = (uint32_t)pending.delay;
    /* Active sector read: next data-ready in read_delay cycles. */
    if (reading && !warm_route_consumer_blocked() &&
        read_delay > 0 && (uint32_t)read_delay < best)
        best = (uint32_t)read_delay;
    return best;
}

void cdrom_advance(uint32_t cycles) {
    /* Age the response presentation latency. Once it elapses, the following
     * refresh_cdrom_irq_line() presents the held response to INTC — at this
     * cdrom_advance boundary, not synchronously inside the guest store that
     * armed it. */
    if (cdrom_irq_present_delay > 0) {
        cdrom_irq_present_delay -= (int)cycles;
        if (cdrom_irq_present_delay < 0) cdrom_irq_present_delay = 0;
    }
    refresh_cdrom_irq_line();
    try_execute_queued_command();
    process_pending(cycles);
    process_read_stream(cycles);
    process_cdda_stream(cycles);
    refresh_cdrom_irq_line();
}

void cdrom_tick(void) {
    cdrom_advance(33868u);
}

uint32_t cdrom_dma_read(void) {
    uint32_t val = 0;
    int got = 0;
    if ((request_reg & CDROM_REQUEST_BFRD) && sector_available &&
        sector_read_pos + 4 <= sector_size) {
        memcpy(&val, sector_buffer + sector_read_pos, 4);
        sector_read_pos += 4;
        if (sector_read_pos >= sector_size) {
            sector_available = 0;
        }
        got = 1;
    }
    /* Per-word DMA data reads flood the CD trace ring (hundreds per sector) and
     * evict the command/IRQ/seek/play history we actually need to read the
     * streaming/audio flow (Rule 15). Gate them OFF by default; opt in with
     * PSX_CD_DMA_TRACE=1 when specifically inspecting the data path. */
    if (got) {
        static int s_dma_trace = -1;
        if (s_dma_trace < 0) {
            const char *e = getenv("PSX_CD_DMA_TRACE");
            s_dma_trace = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        if (s_dma_trace) trace_cdrom('D', 0, val, 4);
    }
    return val;
}

int cdrom_dma_ready(void) {
    return (request_reg & CDROM_REQUEST_BFRD) &&
           sector_available &&
           (sector_read_pos + 4 <= sector_size);
}

uint32_t cdrom_dma_sector_word_count(void) {
    int size = sector_size;

    /* CD DMA BCR low half zero means transfer one sector-sized payload.
     * If DMA is armed just before the next sector becomes available, fall
     * back to the active mode's payload size. */
    if (size <= 0) {
        size = (mode_reg & 0x20u) ? WHOLE_SECTOR_SIZE : SECTOR_SIZE;
    }

    return (uint32_t)((size + 3) / 4);
}

void cdrom_debug_snapshot(CDROMDebugState* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->seq = cdrom_trace_seq;
    out->index_reg = index_reg;
    out->stat_reg = stat_reg;
    out->request_reg = request_reg;
    out->irq_enable = irq_enable;
    out->irq_flag = irq_flag;
    out->mode_reg = mode_reg;
    out->seek_min = seek_min;
    out->seek_sec = seek_sec;
    out->seek_sect = seek_sect;
    out->pending_cmd = pending.cmd;
    out->queued_cmd = queued_cmd.cmd;
    out->queued_pending = (uint8_t)queued_cmd.pending;
    out->queued_param_count = (uint8_t)queued_cmd.param_count;
    out->has_disc = has_disc();
    out->param_count = param_count;
    out->response_read = response_read;
    out->response_count = response_count;
    out->sector_read_pos = sector_read_pos;
    out->sector_available = sector_available;
    out->sector_size = sector_size;
    out->reading = reading;
    out->read_min = read_min;
    out->read_sec = read_sec;
    out->read_sect = read_sect;
    out->read_cmd = read_cmd;
    out->filter_file = filter_file;
    out->filter_channel = filter_channel;
    out->muted = cd_muted;
    out->read_delay = read_delay;
    out->read_hold_cycles = s_read_hold_cycles;
    out->read_hold_events = s_read_hold_events;
    out->int1_pended = s_int1_pended;
    out->int1_lost = s_int1_lost;
    out->int1_pending_now = pending_dataready;
    out->pending_pending = pending.pending;
    out->pending_delay = pending.delay;
    out->pending_phase = pending.phase;
    out->i_stat = i_stat;
    out->last_sector_lba = last_sector_lba;
    out->last_sector_size = last_sector_size;
    out->last_sector_frame = last_sector_frame;
    out->last_sector_mode = last_sector_mode;
    out->last_sector_have_raw = last_sector_have_raw;
}

uint64_t cdrom_debug_get_trace(const CDROMTraceEntry** out_entries) {
    if (out_entries) *out_entries = cdrom_trace;
    return cdrom_trace_seq;
}

void cdrom_debug_clear_trace(void) {
    memset(cdrom_trace, 0, sizeof(cdrom_trace));
    cdrom_trace_seq = 0;
}

uint64_t cdrom_debug_get_command_history(const CDROMCommandHistoryEntry** out_entries) {
    if (out_entries) *out_entries = command_history;
    return command_history_seq;
}

void cdrom_debug_clear_command_history(void) {
    memset(command_history, 0, sizeof(command_history));
    command_history_seq = 0;
}

uint64_t cdrom_debug_get_sector_history(const CDROMSectorHistoryEntry** out_entries) {
    if (out_entries) *out_entries = sector_history;
    return sector_history_seq;
}

void cdrom_debug_clear_sector_history(void) {
    memset(sector_history, 0, sizeof(sector_history));
    sector_history_seq = 0;
}

uint32_t cdrom_debug_copy_last_sector(uint32_t offset, uint32_t len,
                                      uint8_t* out,
                                      CDROMSectorDebugState* state) {
    if (state) {
        memset(state, 0, sizeof(*state));
        state->current_available = sector_available;
        state->current_read_pos = sector_read_pos;
        state->current_size = sector_size;
        state->last_lba = last_sector_lba;
        state->last_size = last_sector_size;
        state->last_frame = last_sector_frame;
        state->last_mode = last_sector_mode;
        state->last_have_raw = last_sector_have_raw;
    }

    if (!out || last_sector_size <= 0 || offset >= (uint32_t)last_sector_size) {
        return 0;
    }
    uint32_t avail = (uint32_t)last_sector_size - offset;
    if (len > avail) len = avail;
    memcpy(out, last_sector_buffer + offset, len);
    return len;
}

/* Load-in-progress predicate for turbo-through-loads (step 4). True while a
 * data-sector read stream is active or a data sector was delivered within
 * the burst-gap window (bridges the per-file Setloc/seek gaps inside one
 * logical load). XA streaming (FMV / CD audio) is NEVER a load — its pacing
 * must stay authentic. */
int cdrom_load_in_progress(void) {
    if (xa_stream_active) return 0;
    if (reading) return 1;
    if (s_burst_count > 0) {
        const CdBurst *b = &s_bursts[(s_burst_count - 1u) % CD_BURST_CAP];
        if ((uint32_t)s_frame_count <= b->end_frame + CD_BURST_GAP_FRAMES)
            return 1;
    }
    return 0;
}

int cdrom_data_read_active(void) {
    return reading && !xa_stream_active;
}

/* ---- boot snapshot: complete CD-ROM controller FSM (see boot_state.h) ---- */
/* Every functional controller/drive/FIFO/IRQ-latch/XA-decode/timing global is
 * listed here exactly once. The X-macro guarantees bytes()/write()/read() can
 * never drift: they all expand the same field list. Pure diagnostics (trace
 * ring, command/sector history rings, the load-burst ring, response-overwrite
 * counters) and the host-only iso_handle pointer are intentionally excluded.
 * PendingCmd / QueuedCmd are plain pointer-free structs, so raw copy is sound. */
#define CDROM_SNAP_FIELDS(X) \
    /* index-banked controller registers + IRQ latch */ \
    X(index_reg) X(stat_reg) X(request_reg) X(irq_enable) X(irq_flag) \
    /* CPS single-outstanding INTC latch + presentation delay */ \
    X(cdrom_intc_request_latched) X(cdrom_irq_generation) \
    X(cdrom_intc_latched_generation) X(cdrom_irq_present_delay) \
    /* parameter FIFO */ \
    X(param_fifo) X(param_count) \
    /* response FIFO */ \
    X(response_fifo) X(response_read) X(response_count) \
    /* current sector buffer */ \
    X(sector_buffer) X(sector_read_pos) X(sector_available) X(sector_size) \
    /* last-delivered sector (GetlocL/GetlocP + in-flight re-delivery) */ \
    X(last_sector_buffer) X(last_sector_lba) X(last_sector_size) \
    X(last_sector_frame) X(last_sector_mode) X(last_sector_have_raw) \
    X(last_sector_raw_mode) X(last_sector_xa_file) X(last_sector_xa_channel) \
    X(last_sector_xa_submode) X(last_sector_xa_coding) \
    /* seek target */ \
    X(seek_min) X(seek_sec) X(seek_sect) X(s_setloc_lba) X(setloc_seek_far) \
    /* read stream state */ \
    X(reading) X(read_min) X(read_sec) X(read_sect) X(mode_reg) \
    X(read_cmd) X(read_delay) X(filter_file) X(filter_channel) X(cd_muted) \
    /* Red Book CD-DA stream */ \
    X(cdda_playing) X(cdda_track) X(cdda_lba) X(cdda_delay) \
    X(cdda_data_end_pending) X(cdda_sectors_played) \
    /* XA ADPCM decode + active-stream identity */ \
    X(xa_hist_l) X(xa_hist_r) X(xa_stream_file) X(xa_stream_channel) \
    X(xa_stream_coding) X(xa_stream_active) \
    /* disc-speed timing model */ \
    X(g_disc_speed_divisor) X(g_game_divisor) X(g_instant_max_per_frame) \
    /* pending (delayed second response) + queued command */ \
    X(pending) X(queued_cmd) \
    /* one-deep pended data-ready INT1 (Beetle SetAIP analog) */ \
    X(pending_dataready) X(pending_dataready_stat)

uint32_t cdrom_snapshot_bytes(void){ uint32_t n=0;
#define X(f) n += (uint32_t)sizeof(f);
    CDROM_SNAP_FIELDS(X)
#undef X
    return n; }
void cdrom_snapshot_write(uint8_t *p){
#define X(f) memcpy(p,&(f),sizeof(f)); p+=sizeof(f);
    CDROM_SNAP_FIELDS(X)
#undef X
}
int cdrom_snapshot_read(const uint8_t *p, uint32_t len){ if(len!=cdrom_snapshot_bytes()) return 0;
#define X(f) memcpy(&(f),p,sizeof(f)); p+=sizeof(f);
    CDROM_SNAP_FIELDS(X)
#undef X
    return 1; }

void debug_force_cd_reinsert(void) {
    // Simulamos la apertura de la bandeja borrando los flujos actuales
    stop_read_stream();
    stop_cdda_playback();
    xa_reset_decode();
    spu_cd_audio_reset();

    // Forzamos el estado de la lectora a "Bandeja Abierta" temporalmente
    stat_reg = CDSTAT_SHELL;
    cdrom_clear_pending_dataready();
    response_clear();

    // Forzamos al emulador a reinicializar el lector con el archivo de disco actual
    if (iso_handle) {
        stat_reg = CDSTAT_MOTOR; // Volvemos a encender el motor virtual
    }

    // Emitimos una interrupción de ACK para despertar al kernel del juego
    set_irq(CDIRQ_ACK);
    fire_cdrom_irq();

    // Forzamos la ejecución de cualquier comando atascado en cola
    try_execute_queued_command();
}
