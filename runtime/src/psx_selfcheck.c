/*
 * Solo rollback resim self-check — see psx_selfcheck.h.
 *
 * Window life cycle (boundary = one offline vblank finish_frame):
 *
 *   IDLE ── window due ──▶ SAVE_WAIT   (poll saves raw snap at next BB edge)
 *   SAVE_WAIT ─ saved ───▶ RECORD      (per-boundary rows + digests, span N)
 *   RECORD ── span done ─▶ LOAD_WAIT   (poll loads snap, longjmp resume)
 *   LOAD_WAIT ─ loaded ──▶ PRIME       (optional throwaway span; primes ambient)
 *   PRIME ── span done ──▶ LOAD_WAIT   (rewind again for counted resims)
 *   LOAD_WAIT ─ loaded ──▶ REPLAY #1   (republish rows, digest per boundary)
 *   REPLAY#1 ─ span done ▶ LOAD_WAIT   (rewind to the SAME snap again)
 *   … REPLAY #2, #3 … ──▶ IDLE        (verdict = #2 vs #3; cold dig = #1 vs #2)
 *
 * The snapshot is taken a few instructions into the tick after the boundary
 * that armed it (first BB-edge poll), so comparisons start at the first
 * boundary after the save completed — record and replay both cover exactly
 * the guest work from the snap point to each boundary.
 *
 * Verdict = warm #2 vs #3 (both peers in an episode are post-rewind resims).
 * Cold #1 vs #2 digs live-RECORD host ambient; ambient-prime (default on)
 * collapses most of that. Live-vs-resim is reported as restore-drift.
 */

#include "psx_selfcheck.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot_state.h"
#include "cpu_state.h"
#include "dirty_ram_interp.h"
#include "gpu.h"
#include "interrupts.h"
#include "netplay_state_digest.h"
#include "overlay_loader.h"
#include "psx_cycles.h"
#include "psx_netplay.h"
#include "psx_scheduler.h"
#include "savestate.h"
#include "sio.h"
#include "spu.h"

extern int fntrace_is_game_started(void);
extern uint64_t psx_cycle_count;
extern uint64_t s_frame_count;

#define SC_MAX_SLOTS 8
#define SC_SPAN_MAX  120
#define SC_SPAN_DEF  32u
#define SC_INTERVAL_DEF 600u
#define SC_WARMUP_BOUNDARIES 900u /* ~15s after enable before first window */
#define SC_ARM_TIMEOUT 10u        /* boundaries to find a BB-edge save/load */

enum {
    SC_OFF = 0,
    SC_IDLE,
    SC_SAVE_WAIT,
    SC_RECORD,
    SC_LOAD_WAIT,
    SC_REPLAY,
};

typedef struct {
    uint16_t buttons;
    uint8_t  lx, ly, rx, ry;
    uint8_t  analog;
    uint8_t  present;
} ScRow;

typedef struct {
    ScRow            rows[SC_MAX_SLOTS];
    NetplayCoreParts core;
    uint32_t         av, aux, cd, spad, dma;
    NetplaySioParts  sio;
} ScBoundary;

static int       s_phase = SC_OFF;
static CPUState *s_cpu;
static uint32_t  s_bios_checksum;
static uint32_t  s_entry_pc;

static uint32_t s_interval = SC_INTERVAL_DEF;
static uint32_t s_span = SC_SPAN_DEF;
static int      s_fault;
static int      s_mash;
static uint32_t s_mash_rate;       /* 0..100 */
static uint32_t s_mash_rng;
static uint16_t s_mash_word = 0xFFFFu;
static uint32_t s_mash_hold_left;
static uint32_t s_mash_latched_boundary = 0xffffffffu;
static uint32_t s_mash_edges;      /* diagnostic: press/release transitions */

static uint32_t s_boundary;        /* offline vblank boundaries seen */
static uint32_t s_next_window;
static uint32_t s_arm_deadline;
static uint32_t s_snap_boundary;   /* boundary counter when snap completed */

static uint8_t *s_blob;
static size_t   s_blob_len;
static uint32_t s_snap_csv; /* cycles_since_vblank at snap — restore for resim */
static uint64_t s_snap_host_frame; /* s_frame_count at snap — pin for resim */

static ScBoundary s_rec[SC_SPAN_MAX]; /* live pass rows + digests */
static ScBoundary s_r1[SC_SPAN_MAX];  /* replay #1 digests (rows unused) */
static ScRow      s_staged[SC_MAX_SLOTS]; /* sampler notes for this boundary */
static uint32_t   s_r1_gpr[SC_SPAN_MAX][32]; /* replay#1 GPRs for diverge dump */
static uint64_t   s_r1_cyc[SC_SPAN_MAX];
static uint32_t   s_r1_csv[SC_SPAN_MAX];

static int      s_pass;           /* 1..3 = replay #N (verdict = #2 vs #3) */
static uint32_t s_prime;          /* throwaway spans before counted resims */
static uint32_t s_prime_left;     /* remaining throwaways for this window */
static int      s_warming;        /* 1 = throwaway span to prime host ambient */
static uint32_t s_replay_idx;
static uint32_t s_mismatches;     /* warm resim-vs-resim (#2 vs #3) */
static uint32_t s_first_bad_idx;
static uint32_t s_cold_mismatches; /* #1 vs #2 — host-ambient warm-up dig */
static uint32_t s_cold_first_idx;
static uint32_t s_drift;          /* live-vs-resim forks (diagnostic) */
static uint32_t s_drift_first_idx;
static char     s_drift_parts[128];
static uint32_t s_window_no;
static uint32_t s_windows_pass;
static uint32_t s_windows_fail;
/* Span just ended in finish_frame — load from flush_load after present RAII
 * (not from a later fast-poll BB: #2 vs #3 were loading at different PCs). */
static int      s_load_after_present;
/* Raw guest PC at each boundary (digests clear PC). */
static uint32_t s_rec_pc[SC_SPAN_MAX];
static uint32_t s_r1_pc[SC_SPAN_MAX];

/* Post-load identity (after restore recipe, before longjmp). */
static ScBoundary s_post_load_r1;
static int        s_post_load_r1_valid;
static uint32_t   s_post_load_r1_gpr[32];
static uint32_t   s_post_load_r1_pc;
static uint64_t   s_post_load_r1_cyc;
static uint64_t   s_post_load_r1_host_frame;

static void sc_free_blob(void)
{
    free(s_blob);
    s_blob = NULL;
    s_blob_len = 0;
}

static void sc_schedule_next(void)
{
    s_next_window = s_boundary + s_interval;
}

static void sc_freeze_overlays(int on)
{
    overlay_loader_set_load_freeze(on);
}

static void sc_cancel(const char *why)
{
    fprintf(stderr, "psxrecomp: selfcheck win#%u aborted (%s)\n",
            (unsigned)s_window_no, why);
    fflush(stderr);
    sc_free_blob();
    sc_freeze_overlays(0);
    s_load_after_present = 0;
    s_warming = 0;
    s_phase = SC_IDLE;
    sc_schedule_next();
}

static void sc_capture_digests(ScBoundary *b)
{
    /* Present-edge digest: clear PC (host parks cpu->pc=0 at present while a
     * resim boundary may hold a live BB PC — the RB lesson). */
    CPUState dig_cpu = *s_cpu;
    dig_cpu.pc = 0;
    netplay_core_digest_parts(&dig_cpu, &b->core);
    b->av = netplay_av_digest();
    b->aux = netplay_aux_digest();
    b->cd = netplay_cdrom_digest();
    b->spad = netplay_spad_digest();
    b->dma = netplay_dma_digest();
    netplay_sio_digest_parts(&b->sio);
}

static uint32_t sc_raw_pc(void)
{
    return s_cpu ? s_cpu->pc : 0u;
}

/* Arm span-end load: prefer flush_load after present-body RAII so every
 * rewind happens at the same VBlank boundary (no post-span BB walk). */
static void sc_arm_span_end_load(void)
{
    s_phase = SC_LOAD_WAIT;
    s_load_after_present = 1;
    s_arm_deadline = s_boundary + SC_ARM_TIMEOUT;
}

static void sc_apply_rows(const ScBoundary *b)
{
    int i;
    for (i = 0; i < SC_MAX_SLOTS; ++i) {
        const ScRow *r = &b->rows[i];
        if (!r->present)
            continue;
        if (sio_pad_on_multitap(i))
            sio_set_pad_config_capable(i, 0);
        sio_set_pad_state_slot(i, r->buttons);
        sio_set_pad_sticks(i, r->lx, r->ly, r->rx, r->ry);
        sio_request_pad_type(i, r->analog ? 1 : 0);
    }
}

/* Append the name of each differing partition to buf. Returns count. */
static int sc_diff_parts(const ScBoundary *a, const ScBoundary *b,
                         char *buf, size_t cap)
{
    int n = 0;
    size_t off = 0;
#define SC_PART(name, expr_a, expr_b)                                   \
    do {                                                                \
        if ((expr_a) != (expr_b)) {                                     \
            int w = snprintf(buf + off, cap > off ? cap - off : 0,      \
                             "%s%s", n ? "," : "", name);               \
            if (w > 0) off += (size_t)w;                                \
            n++;                                                        \
        }                                                               \
    } while (0)
    SC_PART("cpu",  a->core.cpu,       b->core.cpu);
    SC_PART("clk",  a->core.clock_irq, b->core.clock_irq);
    SC_PART("tim",  a->core.timers,    b->core.timers);
    SC_PART("ram",  a->core.ram,       b->core.ram);
    SC_PART("dirty",a->core.dirty,     b->core.dirty);
    SC_PART("av",   a->av,             b->av);
    SC_PART("aux",  a->aux,            b->aux);
    SC_PART("cd",   a->cd,             b->cd);
    SC_PART("spad", a->spad,           b->spad);
    SC_PART("dma",  a->dma,            b->dma);
    SC_PART("sioR", a->sio.regs,       b->sio.regs);
    SC_PART("sioP", a->sio.pads,       b->sio.pads);
    SC_PART("sioM", a->sio.mc,         b->sio.mc);
    SC_PART("sioT", a->sio.pace,       b->sio.pace);
#undef SC_PART
    return n;
}

static uint32_t sc_env_u32(const char *name, uint32_t def, uint32_t lo,
                           uint32_t hi)
{
    const char *e = getenv(name);
    if (!e || !*e)
        return def;
    {
        unsigned long v = strtoul(e, NULL, 10);
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        return (uint32_t)v;
    }
}

/* PSX digital pad bits (active-low). Face + D-pad + shoulders — fighter mash
 * set. START is rare (menu advance); SELECT/L3/R3 stay released. */
#define SC_PAD_START    (1u << 3)
#define SC_PAD_UP       (1u << 4)
#define SC_PAD_RIGHT    (1u << 5)
#define SC_PAD_DOWN     (1u << 6)
#define SC_PAD_LEFT     (1u << 7)
#define SC_PAD_L2       (1u << 8)
#define SC_PAD_R2       (1u << 9)
#define SC_PAD_L1       (1u << 10)
#define SC_PAD_R1       (1u << 11)
#define SC_PAD_TRIANGLE (1u << 12)
#define SC_PAD_CIRCLE   (1u << 13)
#define SC_PAD_CROSS    (1u << 14)
#define SC_PAD_SQUARE   (1u << 15)

static const uint16_t sc_mash_bits[] = {
    SC_PAD_UP, SC_PAD_DOWN, SC_PAD_LEFT, SC_PAD_RIGHT,
    SC_PAD_TRIANGLE, SC_PAD_CIRCLE, SC_PAD_CROSS, SC_PAD_SQUARE,
    SC_PAD_L1, SC_PAD_R1, SC_PAD_L2, SC_PAD_R2,
};
#define SC_MASH_NBITS ((int)(sizeof(sc_mash_bits) / sizeof(sc_mash_bits[0])))

static uint32_t sc_mash_rng_next(void)
{
    /* xorshift32 */
    uint32_t x = s_mash_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_mash_rng = x ? x : 0xA5A5A5A5u;
    return s_mash_rng;
}

static uint16_t sc_mash_advance(void)
{
    uint16_t prev = s_mash_word;
    if (s_mash_hold_left > 0u) {
        s_mash_hold_left--;
        return s_mash_word;
    }
    /* Decide whether to open a new chord or sit idle. */
    if ((sc_mash_rng_next() % 100u) < s_mash_rate) {
        uint16_t pressed = 0u;
        int n = 1 + (int)(sc_mash_rng_next() % 3u); /* 1..3 simultaneous */
        int i;
        for (i = 0; i < n; ++i)
            pressed |= sc_mash_bits[sc_mash_rng_next() % (uint32_t)SC_MASH_NBITS];
        /* ~8% include START (menu / confirm spam). */
        if ((sc_mash_rng_next() % 100u) < 8u)
            pressed |= SC_PAD_START;
        s_mash_word = (uint16_t)(0xFFFFu & ~pressed);
        s_mash_hold_left = 1u + (sc_mash_rng_next() % 3u); /* hold 1..3 */
    } else {
        s_mash_word = 0xFFFFu; /* full release */
        s_mash_hold_left = sc_mash_rng_next() % 3u; /* idle 0..2 */
    }
    if (s_mash_word != prev)
        s_mash_edges++;
    return s_mash_word;
}

int psx_selfcheck_mash_override(uint16_t *out_buttons)
{
    if (!s_mash || s_phase == SC_OFF || s_phase == SC_REPLAY ||
        s_phase == SC_LOAD_WAIT)
        return 0;
    if (!out_buttons)
        return 0;
    /* One stable word per offline boundary (finish_frame bumps s_boundary
     * after sampling). Mid-frame low-latency resample must not see a new
     * RNG roll within the same tick. */
    if (s_mash_latched_boundary != s_boundary) {
        s_mash_latched_boundary = s_boundary;
        (void)sc_mash_advance();
    }
    *out_buttons = s_mash_word;
    return 1;
}

void psx_selfcheck_init(struct CPUState *cpu, uint32_t bios_checksum,
                        uint32_t entry_pc)
{
    const char *e = getenv("PSX_RB_SELFCHECK");
    if (!e || !*e || *e == '0')
        return;
    if (!psx_hle_scheduler_enabled()) {
        fprintf(stderr, "psxrecomp: selfcheck disabled — requires the HLE "
                        "scheduler (rewind longjmp)\n");
        return;
    }
    s_cpu = cpu;
    s_bios_checksum = bios_checksum;
    s_entry_pc = entry_pc;
    s_interval = sc_env_u32("PSX_RB_SELFCHECK_INTERVAL", SC_INTERVAL_DEF,
                            120u, 1000000u);
    s_span = sc_env_u32("PSX_RB_SELFCHECK_SPAN", SC_SPAN_DEF, 4u, SC_SPAN_MAX);
    s_fault = sc_env_u32("PSX_RB_SELFCHECK_FAULT", 0u, 0u, 1u) != 0u;
    /* Default 1: throwaway resim(s) of the snap clear live-RECORD host
     * ambient so counted #1 matches #2/#3. 0 disables; 2+ for stubborn windows. */
    s_prime = sc_env_u32("PSX_RB_SELFCHECK_PRIME", 1u, 0u, 4u);
    s_mash = sc_env_u32("PSX_RB_SELFCHECK_MASH", 0u, 0u, 1u) != 0u;
    s_mash_rate = sc_env_u32("PSX_RB_SELFCHECK_MASH_RATE", 75u, 1u, 100u);
    s_mash_rng = sc_env_u32("PSX_RB_SELFCHECK_MASH_SEED", 0xC0FFEEu, 1u,
                            0xffffffffu);
    s_mash_word = 0xFFFFu;
    s_mash_hold_left = 0u;
    s_mash_latched_boundary = 0xffffffffu;
    s_mash_edges = 0u;
    s_phase = SC_IDLE;
    s_next_window = SC_WARMUP_BOUNDARIES;
    /* Match netplay: idle-skip advances guest cycles without lockstep device
     * aging — host-trained streaks fork cold resim#1 from warm #2/#3. */
    {
        extern int g_idle_skip_enabled;
        if (g_idle_skip_enabled != 0) {
            g_idle_skip_enabled = 0;
            fprintf(stderr, "psxrecomp: selfcheck — idle_skip forced off\n");
        }
    }
    fprintf(stderr,
            "psxrecomp: selfcheck enabled interval=%u span=%u fault=%d "
            "prime=%u mash=%d rate=%u seed=%u\n",
            (unsigned)s_interval, (unsigned)s_span, s_fault,
            (unsigned)s_prime, s_mash, (unsigned)s_mash_rate,
            (unsigned)s_mash_rng);
    fflush(stderr);
}

int psx_selfcheck_enabled(void)      { return s_phase != SC_OFF; }
int psx_selfcheck_defer_present(void)
{
    /* API retained for netplay-parity probes; offline GPU present stays
     * immediate (BB-edge defer skews clk/tim between warm peers). */
    return s_phase == SC_SAVE_WAIT || s_phase == SC_RECORD ||
           s_phase == SC_LOAD_WAIT || s_phase == SC_REPLAY;
}
int psx_selfcheck_resim_active(void)
{
    /* RECORD must share the resim host-skip profile (no CD boost / audio pump /
     * wall pacer). Otherwise the ambient left by the live record pass forks
     * resim#1 from resim#2 while #2≡#3 (warm) stays bit-identical. */
    return s_phase == SC_RECORD || s_phase == SC_REPLAY ||
           s_phase == SC_LOAD_WAIT;
}
int psx_selfcheck_replay_input(void) { return s_phase == SC_REPLAY ||
                                              s_phase == SC_LOAD_WAIT; }
int psx_selfcheck_input_locked(void)
{
    return s_phase == SC_RECORD || s_phase == SC_LOAD_WAIT ||
           s_phase == SC_REPLAY;
}

static void sc_dump_gpr_diff(const uint32_t *a, const uint32_t *b,
                             const char *tag)
{
    int i;
    int n = 0;
    char buf[512];
    size_t off = 0;
    buf[0] = '\0';
    for (i = 0; i < 32; ++i) {
        if (a[i] == b[i])
            continue;
        int w = snprintf(buf + off, sizeof(buf) > off ? sizeof(buf) - off : 0,
                         "%sr%d=%08x/%08x", n ? " " : "", i,
                         (unsigned)a[i], (unsigned)b[i]);
        if (w > 0) off += (size_t)w;
        n++;
        if (n >= 12) break; /* keep the line readable */
    }
    fprintf(stderr, "psxrecomp: selfcheck %s gpr_diff n=%d %s%s\n",
            tag, n, n ? buf : "(none)", (n >= 12) ? " ..." : "");
    fflush(stderr);
}

void psx_selfcheck_note_pad(int slot, uint16_t buttons, uint8_t lx, uint8_t ly,
                            uint8_t rx, uint8_t ry, uint8_t analog)
{
    ScRow *r;
    if (s_phase != SC_RECORD && s_phase != SC_SAVE_WAIT)
        return;
    if (slot < 0 || slot >= SC_MAX_SLOTS)
        return;
    r = &s_staged[slot];
    r->buttons = buttons;
    r->lx = lx; r->ly = ly; r->rx = rx; r->ry = ry;
    r->analog = analog;
    r->present = 1;
}

static void sc_report(void)
{
    char drift[160];
    if (s_drift)
        snprintf(drift, sizeof(drift),
                 "restore-drift %u/%u (first idx=%u parts=%s)",
                 (unsigned)s_drift, (unsigned)s_span,
                 (unsigned)s_drift_first_idx, s_drift_parts);
    else
        snprintf(drift, sizeof(drift), "restore-drift 0/%u",
                 (unsigned)s_span);
    if (s_mismatches == 0) {
        s_windows_pass++;
        fprintf(stderr,
                "psxrecomp: selfcheck win#%u PASS warm-resim(#2vs#3) %u/%u; "
                "cold(#1vs#2) mismatched=%u/%u; %s (pass=%u fail=%u)",
                (unsigned)s_window_no, (unsigned)s_span, (unsigned)s_span,
                (unsigned)s_cold_mismatches, (unsigned)s_span, drift,
                (unsigned)s_windows_pass, (unsigned)s_windows_fail);
    } else {
        s_windows_fail++;
        fprintf(stderr,
                "psxrecomp: selfcheck win#%u FAIL warm-resim(#2vs#3) "
                "mismatched=%u/%u first_idx=%u; cold(#1vs#2) "
                "mismatched=%u/%u first_idx=%u; %s (pass=%u fail=%u)",
                (unsigned)s_window_no, (unsigned)s_mismatches,
                (unsigned)s_span, (unsigned)s_first_bad_idx,
                (unsigned)s_cold_mismatches, (unsigned)s_span,
                (unsigned)s_cold_first_idx, drift,
                (unsigned)s_windows_pass, (unsigned)s_windows_fail);
    }
    if (s_mash)
        fprintf(stderr, " mash_edges=%u", (unsigned)s_mash_edges);
    fprintf(stderr, "\n");
    fflush(stderr);
    sc_freeze_overlays(0);
}

void psx_selfcheck_finish_frame(int defer_window)
{
    if (s_phase == SC_OFF)
        return;
    if (psx_netplay_active()) {
        /* Netplay owns the resim machinery from here on. */
        fprintf(stderr, "psxrecomp: selfcheck off (netplay active)\n");
        fflush(stderr);
        sc_free_blob();
        sc_freeze_overlays(0);
        s_phase = SC_OFF;
        return;
    }
    /* First boundary after resume_at — native chain under the snap PC is live
     * again for subsequent ticks (same point netplay clears). */
    if (s_phase == SC_REPLAY)
        psx_scheduler_top_level_resume_clear();

    s_boundary++;

    switch (s_phase) {
    case SC_IDLE:
        if (s_boundary < s_next_window)
            return;
        if (defer_window || !fntrace_is_game_started() ||
            gpu_display_is_depth24() || savestate_pending())
            return; /* retry next boundary */
        s_window_no++;
        memset(s_staged, 0, sizeof(s_staged));
        /* Freeze overlay discovery for the whole window (record+both
         * resims) so host-only DLL registration cannot fork BB edges. */
        sc_freeze_overlays(1);
        s_phase = SC_SAVE_WAIT;
        s_arm_deadline = s_boundary + SC_ARM_TIMEOUT;
        return;

    case SC_SAVE_WAIT:
        memset(s_staged, 0, sizeof(s_staged)); /* rows latch per boundary */
        if (s_boundary > s_arm_deadline)
            sc_cancel("no BB-edge save opportunity");
        return;

    case SC_RECORD: {
        uint32_t idx = s_boundary - s_snap_boundary - 1u;
        if (savestate_pending()) {
            sc_cancel("user savestate during record");
            return;
        }
        if (idx < s_span) {
            ScBoundary *b = &s_rec[idx];
            memcpy(b->rows, s_staged, sizeof(b->rows));
            sc_capture_digests(b);
            s_rec_pc[idx] = sc_raw_pc();
        }
        memset(s_staged, 0, sizeof(s_staged));
        if (idx + 1u >= s_span) {
            /* Optional throwaway span(s): prime host ambient left by live
             * RECORD so counted resim#1 matches #2/#3. */
            s_pass = 0;
            s_prime_left = s_prime;
            s_warming = s_prime_left > 0u ? 1 : 0;
            sc_arm_span_end_load();
        }
        return;
    }

    case SC_LOAD_WAIT:
        /* Live boundaries past the window while waiting for a BB edge —
         * discarded by the restore. */
        if (s_boundary > s_arm_deadline)
            sc_cancel("no BB-edge load opportunity");
        return;

    case SC_REPLAY: {
        ScBoundary cur;
        if (savestate_pending()) {
            sc_cancel("user savestate during replay");
            return;
        }
        if (s_replay_idx >= s_span) { /* defensive */
            s_phase = SC_IDLE;
            sc_schedule_next();
            return;
        }
        /* Order matches record: rows applied (sampler there, us here), then
         * digest at the same boundary point. */
        sc_apply_rows(&s_rec[s_replay_idx]);
        memset(&cur, 0, sizeof(cur));
        sc_capture_digests(&cur);
        if (s_warming) {
            /* Throwaway — no compare. Advance; more primes or counted #1. */
            s_replay_idx++;
            if (s_replay_idx >= s_span) {
                if (s_prime_left > 0u)
                    s_prime_left--;
                if (s_prime_left > 0u) {
                    fprintf(stderr,
                            "psxrecomp: selfcheck win#%u ambient-prime "
                            "pass done (%u left)\n",
                            (unsigned)s_window_no, (unsigned)s_prime_left);
                    fflush(stderr);
                    s_warming = 1;
                } else {
                    fprintf(stderr,
                            "psxrecomp: selfcheck win#%u ambient-prime done "
                            "— starting counted resims\n",
                            (unsigned)s_window_no);
                    fflush(stderr);
                    s_warming = 0;
                    s_pass = 0;
                }
                sc_arm_span_end_load();
            }
            return;
        }
        if (s_pass == 1) {
            /* Replay #1: seed cold reference; live-vs-resim = drift dig. */
            char parts[128];
            memcpy(cur.rows, s_rec[s_replay_idx].rows, sizeof(cur.rows));
            s_r1[s_replay_idx] = cur;
            memcpy(s_r1_gpr[s_replay_idx], s_cpu->gpr, sizeof(s_r1_gpr[0]));
            s_r1_cyc[s_replay_idx] = psx_cycle_count;
            s_r1_csv[s_replay_idx] = interrupts_get_cycles_since_vblank();
            s_r1_pc[s_replay_idx] = sc_raw_pc();
            if (sc_diff_parts(&s_rec[s_replay_idx], &cur, parts,
                              sizeof(parts))) {
                if (s_drift == 0) {
                    s_drift_first_idx = s_replay_idx;
                    snprintf(s_drift_parts, sizeof(s_drift_parts), "%s",
                             parts);
                }
                s_drift++;
            }
        } else if (s_pass == 2) {
            /* Replay #2: cold dig (#1 vs #2), then become the warm reference. */
            const ScBoundary *ref = &s_r1[s_replay_idx];
            char parts[128];
            int n = sc_diff_parts(ref, &cur, parts, sizeof(parts));
            if (n) {
                if (s_cold_mismatches == 0)
                    s_cold_first_idx = s_replay_idx;
                s_cold_mismatches++;
                if (s_cold_mismatches == 1) {
                    char tag[48];
                    fprintf(stderr,
                            "psxrecomp: selfcheck win#%u COLD idx=%u "
                            "parts=%s raw_pc=%08x/%08x\n",
                            (unsigned)s_window_no, (unsigned)s_replay_idx,
                            parts, (unsigned)s_r1_pc[s_replay_idx],
                            (unsigned)sc_raw_pc());
                    snprintf(tag, sizeof(tag), "win#%u cold idx=%u",
                             (unsigned)s_window_no, (unsigned)s_replay_idx);
                    sc_dump_gpr_diff(s_r1_gpr[s_replay_idx], s_cpu->gpr, tag);
                    interrupts_log_last_vblank_irqctx(tag);
                    fflush(stderr);
                }
            }
            s_r1[s_replay_idx] = cur;
            memcpy(s_r1_gpr[s_replay_idx], s_cpu->gpr, sizeof(s_r1_gpr[0]));
            s_r1_cyc[s_replay_idx] = psx_cycle_count;
            s_r1_csv[s_replay_idx] = interrupts_get_cycles_since_vblank();
            s_r1_pc[s_replay_idx] = sc_raw_pc();
        } else {
            /* Replay #3: warm resim-vs-resim — the rollback invariant after
             * host ambient has seen one resim of this snap. */
            const ScBoundary *ref = &s_r1[s_replay_idx];
            char parts[128];
            int n = sc_diff_parts(ref, &cur, parts, sizeof(parts));
            if (n) {
                int first = (s_mismatches == 0);
                if (first)
                    s_first_bad_idx = s_replay_idx;
                s_mismatches++;
                fprintf(stderr,
                        "psxrecomp: selfcheck win#%u DIVERGE idx=%u parts=%s "
                        "core=%08x/%08x ram=%08x/%08x cpu=%08x/%08x "
                        "clk=%08x/%08x av=%08x/%08x cd=%08x/%08x "
                        "sioT=%08x/%08x cyc=%llu/%llu d_cyc=%lld "
                        "csv=%u/%u host_f=%llu raw_pc=%08x/%08x\n",
                        (unsigned)s_window_no, (unsigned)s_replay_idx, parts,
                        (unsigned)ref->core.core, (unsigned)cur.core.core,
                        (unsigned)ref->core.ram, (unsigned)cur.core.ram,
                        (unsigned)ref->core.cpu, (unsigned)cur.core.cpu,
                        (unsigned)ref->core.clock_irq,
                        (unsigned)cur.core.clock_irq,
                        (unsigned)ref->av, (unsigned)cur.av,
                        (unsigned)ref->cd, (unsigned)cur.cd,
                        (unsigned)ref->sio.pace, (unsigned)cur.sio.pace,
                        (unsigned long long)s_r1_cyc[s_replay_idx],
                        (unsigned long long)psx_cycle_count,
                        (long long)psx_cycle_count -
                            (long long)s_r1_cyc[s_replay_idx],
                        (unsigned)s_r1_csv[s_replay_idx],
                        (unsigned)interrupts_get_cycles_since_vblank(),
                        (unsigned long long)s_frame_count,
                        (unsigned)s_r1_pc[s_replay_idx],
                        (unsigned)sc_raw_pc());
                fflush(stderr);
                if (first) {
                    char tag[48];
                    snprintf(tag, sizeof(tag), "win#%u idx=%u",
                             (unsigned)s_window_no,
                             (unsigned)s_replay_idx);
                    sc_dump_gpr_diff(s_r1_gpr[s_replay_idx], s_cpu->gpr, tag);
                    interrupts_log_last_vblank_irqctx(tag);
                }
            }
        }
        s_replay_idx++;
        if (s_replay_idx >= s_span) {
            if (s_pass == 1) {
                if (s_fault) {
                    uint32_t fi = s_span / 2u;
                    s_r1[fi].core.ram ^= 1u;
                    fprintf(stderr,
                            "psxrecomp: selfcheck win#%u fault injected "
                            "idx=%u\n",
                            (unsigned)s_window_no, (unsigned)fi);
                    fflush(stderr);
                }
                sc_arm_span_end_load();
            } else if (s_pass == 2) {
                /* Warm reference captured — one more rewind for #3. */
                sc_arm_span_end_load();
            } else {
                sc_report();
                sc_free_blob();
                s_load_after_present = 0;
                s_phase = SC_IDLE;
                sc_schedule_next();
            }
        }
        return;
    }

    default:
        return;
    }
}

/* SC_LOAD_WAIT rewind. Mirrors the RB restore recipe — cycle + IRQ resync,
 * SPU CD FIFO normalize, NO cdrom_accelerate, light frontend hook.
 * Longjmps via psx_scheduler_resume_at — never returns on success. */
static void sc_do_load(struct CPUState *cpu, const char *via)
{
    if (!cpu) {
        sc_cancel("load with null cpu");
        return;
    }
    if (!s_blob || !s_blob_len) {
        sc_cancel("no snap blob");
        return;
    }
    s_load_after_present = 0;
    /* Host-only ambient from the PREVIOUS pass — sample before load. */
    {
        extern uint32_t g_dirty_safe_resume_pc;
        extern uint32_t g_dirty_ram_code_gen;
        extern int g_idle_skip_enabled;
        extern int g_ls_suppress_record;
        extern int g_ls_mode;
        extern uint64_t g_guest_store_count;
        extern uint64_t g_irqctx_seq;
        extern uint32_t dirty_ram_text_diverged_pages(void);
        uint64_t kb[6];
        const int next_pass = s_warming ? 0 : (s_pass == 0 ? 1 : s_pass + 1);
        psx_kernel_bless_stats(kb);
        fprintf(stderr,
                "psxrecomp: selfcheck win#%u load ambient pre "
                "pass=%d via=%s dirty_resume=%08x irq_resume=%08x "
                "irq_chk_pc=%08x irq_chk_cyc=%llu tot_chk=%llu "
                "fast_m=%u code_gen=%u stores=%llu irqctx=%llu "
                "ls_sup=%d ls_mode=%d idle_skip=%d ovl=%d "
                "txt_div=%u kb_clean=%llu kb_mism=%llu\n",
                (unsigned)s_window_no, next_pass, via ? via : "?",
                (unsigned)g_dirty_safe_resume_pc,
                (unsigned)psx_compiled_irq_resume_pc(),
                (unsigned)psx_last_irq_check_pc(),
                (unsigned long long)psx_last_irq_check_cycle(),
                (unsigned long long)psx_interrupt_total_checks(),
                (unsigned)psx_interrupt_fast_maintenance(),
                (unsigned)g_dirty_ram_code_gen,
                (unsigned long long)g_guest_store_count,
                (unsigned long long)g_irqctx_seq,
                g_ls_suppress_record, g_ls_mode, g_idle_skip_enabled,
                overlay_loader_registered_count(),
                (unsigned)dirty_ram_text_diverged_pages(),
                (unsigned long long)kb[1], (unsigned long long)kb[2]);
        fflush(stderr);
    }
    if (!boot_state_load_buffer(s_blob, s_blob_len, s_bios_checksum,
                                s_entry_pc, cpu)) {
        sc_cancel("snap load failed");
        return;
    }
    psx_cycles_resync_after_restore(cpu);
    interrupts_resync_after_restore();
    /* Keep the snap's VBlank phase (boot_state also restores csv from IRQ
     * section; re-apply the latched value so ambient-prime / legacy 8B snaps
     * stay bit-identical across replay#1/#2). */
    interrupts_set_cycles_since_vblank(s_snap_csv);
    /* Pin host frame counter so present/GPU hysteresis that keys on
     * s_frame_count cannot fork resim#1 vs resim#2 (+span otherwise). */
    s_frame_count = s_snap_host_frame;
    /* Host-only overlay negative-lookup memo — not in the snap; a fill during
     * resim#1 made #2≡#3 while #1 forked (cold ambient). */
    overlay_loader_clear_lazy_miss();
    spu_cd_audio_reset();
    psx_frontend_on_rb_snap_loaded();
    /* Mirror RB flush_resume: longjmp abandons any flush_present frame. */
    gpu_vblank_release_present_flush_guard();
    gpu_vblank_clear_deferred_present();
    s_replay_idx = 0;
    s_cpu = cpu;
    if (s_warming) {
        /* Throwaway prime span — do not advance s_pass / post_load refs. */
        s_phase = SC_REPLAY;
        fprintf(stderr,
                "psxrecomp: selfcheck win#%u ambient-prime begin pc=0x%08x "
                "span=%u via=%s\n",
                (unsigned)s_window_no, (unsigned)cpu->pc, (unsigned)s_span,
                via ? via : "?");
        fflush(stderr);
        psx_scheduler_resume_at(cpu->pc);
        return; /* unreachable */
    }
    if (s_pass == 0) { /* entering counted replay #1 */
        s_mismatches = 0;
        s_first_bad_idx = 0;
        s_cold_mismatches = 0;
        s_cold_first_idx = 0;
        s_drift = 0;
        s_drift_first_idx = 0;
        s_drift_parts[0] = '\0';
        s_post_load_r1_valid = 0;
    }
    s_pass = (s_pass == 0) ? 1 : (s_pass + 1);
    s_phase = SC_REPLAY;
    {
        ScBoundary post;
        memset(&post, 0, sizeof(post));
        sc_capture_digests(&post);
        if (s_pass == 1) {
            s_post_load_r1 = post;
            s_post_load_r1_valid = 1;
            memcpy(s_post_load_r1_gpr, cpu->gpr, sizeof(s_post_load_r1_gpr));
            s_post_load_r1_pc = cpu->pc;
            s_post_load_r1_cyc = psx_cycle_count;
            s_post_load_r1_host_frame = s_frame_count;
            fprintf(stderr,
                    "psxrecomp: selfcheck win#%u replay#1 begin pc=0x%08x "
                    "span=%u post_load core=%08x ram=%08x cpu=%08x "
                    "clk=%08x cyc=%llu host_f=%llu ovl=%d freeze=%d\n",
                    (unsigned)s_window_no, (unsigned)cpu->pc,
                    (unsigned)s_span, (unsigned)post.core.core,
                    (unsigned)post.core.ram, (unsigned)post.core.cpu,
                    (unsigned)post.core.clock_irq,
                    (unsigned long long)psx_cycle_count,
                    (unsigned long long)s_frame_count,
                    overlay_loader_registered_count(),
                    overlay_loader_load_frozen());
        } else if (s_post_load_r1_valid) {
            char parts[128];
            int n = sc_diff_parts(&s_post_load_r1, &post, parts,
                                  sizeof(parts));
            fprintf(stderr,
                    "psxrecomp: selfcheck win#%u replay#%d begin pc=0x%08x "
                    "span=%u post_load %s parts=%s core=%08x/%08x "
                    "ram=%08x/%08x cpu=%08x/%08x clk=%08x/%08x "
                    "cyc=%llu/%llu host_f=%llu/%llu ovl=%d freeze=%d\n",
                    (unsigned)s_window_no, s_pass, (unsigned)cpu->pc,
                    (unsigned)s_span, n ? "MISMATCH" : "MATCH",
                    n ? parts : "-",
                    (unsigned)s_post_load_r1.core.core,
                    (unsigned)post.core.core,
                    (unsigned)s_post_load_r1.core.ram,
                    (unsigned)post.core.ram,
                    (unsigned)s_post_load_r1.core.cpu,
                    (unsigned)post.core.cpu,
                    (unsigned)s_post_load_r1.core.clock_irq,
                    (unsigned)post.core.clock_irq,
                    (unsigned long long)s_post_load_r1_cyc,
                    (unsigned long long)psx_cycle_count,
                    (unsigned long long)s_post_load_r1_host_frame,
                    (unsigned long long)s_frame_count,
                    overlay_loader_registered_count(),
                    overlay_loader_load_frozen());
            if (n) {
                char tag[48];
                snprintf(tag, sizeof(tag), "win#%u post_load",
                         (unsigned)s_window_no);
                sc_dump_gpr_diff(s_post_load_r1_gpr, cpu->gpr, tag);
            } else if (memcmp(s_post_load_r1_gpr, cpu->gpr,
                              sizeof(s_post_load_r1_gpr)) != 0) {
                /* Digests clear PC; catch GPR-only forks digests miss. */
                char tag[48];
                snprintf(tag, sizeof(tag), "win#%u post_load_gpr",
                         (unsigned)s_window_no);
                sc_dump_gpr_diff(s_post_load_r1_gpr, cpu->gpr, tag);
            }
            if (cpu->pc != s_post_load_r1_pc) {
                fprintf(stderr,
                        "psxrecomp: selfcheck win#%u post_load PC "
                        "0x%08x vs 0x%08x\n",
                        (unsigned)s_window_no, (unsigned)s_post_load_r1_pc,
                        (unsigned)cpu->pc);
            }
        }
        fflush(stderr);
    }
    psx_scheduler_resume_at(cpu->pc);
}

void psx_selfcheck_flush_load(void)
{
    if (s_phase != SC_LOAD_WAIT || !s_load_after_present)
        return;
    if (psx_netplay_active())
        return;
    /* Prefer span-end present: same VBlank boundary for every rewind. */
    sc_do_load(s_cpu, "present");
}

/* Same gate as rb_resume_pc_ok — psx_is_dispatchable still accepts 0xB0. */
static int sc_resume_pc_ok(uint32_t pc)
{
    uint32_t phys;
    if (!psx_is_dispatchable(pc))
        return 0;
    if ((pc & 3u) != 0u)
        return 0;
    if (pc == 0x80000080u || pc == 0xbfc00180u || pc == 0x80000000u)
        return 0;
    if ((pc & 0xfff00000u) == 0xbfc00000u)
        return 1;
    phys = pc & 0x1fffffffu;
    if (phys >= 0x1000u && phys < 0x200000u)
        return 1;
    return 0;
}

static uint32_t sc_pick_snap_pc(struct CPUState *cpu, uint32_t hint)
{
    const uint32_t cands[5] = {
        hint,
        psx_last_irq_check_pc(),
        psx_compiled_irq_resume_pc(),
        cpu ? cpu->gpr[31] : 0u,
        cpu ? cpu->pc : 0u,
    };
    int i;
    for (i = 0; i < 5; ++i) {
        if (sc_resume_pc_ok(cands[i]))
            return cands[i];
    }
    return 0;
}

void psx_selfcheck_poll(struct CPUState *cpu, uint32_t resume_pc)
{
    if (s_phase != SC_SAVE_WAIT && s_phase != SC_LOAD_WAIT)
        return;
    if (!cpu || psx_netplay_active())
        return;

    if (s_phase == SC_SAVE_WAIT) {
        CPUState snap;
        uint32_t snap_pc = sc_pick_snap_pc(cpu, resume_pc);
        if (!snap_pc)
            return; /* retry — reject 0xB0 / vector junk (win#138 class) */
        snap = *cpu;
        snap.pc = snap_pc;
        sc_free_blob();
        if (!boot_state_save_buffer_raw(&snap, s_bios_checksum, s_entry_pc,
                                        &s_blob, &s_blob_len)) {
            sc_cancel("snap save failed");
            return;
        }
        s_snap_boundary = s_boundary;
        s_snap_csv = interrupts_get_cycles_since_vblank();
        s_snap_host_frame = s_frame_count;
        s_phase = SC_RECORD;
        fprintf(stderr,
                "psxrecomp: selfcheck win#%u snap boundary=%u pc=0x%08x "
                "bytes=%zu span=%u csv=%u host_f=%llu%s\n",
                (unsigned)s_window_no, (unsigned)s_boundary,
                (unsigned)snap_pc, s_blob_len, (unsigned)s_span,
                (unsigned)s_snap_csv, (unsigned long long)s_snap_host_frame,
                (snap_pc != resume_pc) ? " (picked)" : "");
        fflush(stderr);
        return;
    }

    /* LOAD_WAIT: span-end flush owns the load. BB-poll is fallback only
     * (e.g. if present was skipped). Never load from fast-poll while a
     * present flush is armed — that was the #2/#3 load-site skew. */
    if (s_load_after_present)
        return;
    sc_do_load(cpu, "poll");
}
