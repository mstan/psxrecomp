#include "overlay_capture.h"
#include "overlay_loader.h"
#include "dirty_ram_interp.h"
#include "code_provider.h"
#include "crc32.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <SDL.h>

/* ---- Capture set --------------------------------------------------------
 * Tracks which physical addresses have received CD DMA loads since game
 * handoff.  Used for dedup (state machine) and A-layer compilation queue.
 * The JSON output uses dirty_ram regions, not individual DMA blocks.
 * -------------------------------------------------------------------------*/

typedef enum {
    OV_QUEUED    = 0,  /* captured, not yet compiled              */
    OV_COMPILING = 1,  /* background thread working on it         */
    OV_COMPILED  = 2,  /* DLL written, dispatch table patched     */
} OvState;

typedef struct {
    uint32_t  load_addr;   /* physical RAM address                */
    uint32_t  size;        /* byte count of this DMA block        */
    uint8_t  *bytes;       /* unpatched copy taken at DMA time    */
    OvState   state;
} OvEntry;

#define MAX_OVERLAYS 4096

static OvEntry  s_entries[MAX_OVERLAYS];
static int      s_count    = 0;
static char     s_out_dir[512];
static int      s_active   = 0;
static int      s_enabled  = 0;   /* config gate; off unless overlay cache enabled */

/* ---- Base64 encoder ----------------------------------------------------- */

static const char k_b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void write_b64(FILE *f, const uint8_t *data, size_t len)
{
    size_t i;
    for (i = 0; i < len; i += 3) {
        uint32_t b = (uint32_t)data[i] << 16;
        if (i + 1 < len) b |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) b |= (uint32_t)data[i + 2];
        fputc(k_b64[(b >> 18) & 0x3F], f);
        fputc(k_b64[(b >> 12) & 0x3F], f);
        fputc(i + 1 < len ? k_b64[(b >>  6) & 0x3F] : '=', f);
        fputc(i + 2 < len ? k_b64[(b      ) & 0x3F] : '=', f);
    }
}

/* ---- Public API --------------------------------------------------------- */

void overlay_capture_set_out_dir(const char *out_dir)
{
    strncpy(s_out_dir, out_dir, sizeof(s_out_dir) - 1);
    s_out_dir[sizeof(s_out_dir) - 1] = '\0';
}

void overlay_capture_init(const char *out_dir)
{
    overlay_capture_set_out_dir(out_dir);
}

void overlay_capture_set_enabled(int enabled)
{
    s_enabled = enabled ? 1 : 0;
}

void overlay_capture_on_dma(uint32_t load_addr, uint32_t size,
                             const uint8_t *bytes)
{
    int i;
    OvEntry *e;
    extern int fntrace_is_game_started(void);

    if (!s_enabled) return;   /* overlay cache disabled in config */
    if (size == 0) return;

    /* Auto-activate on the first post-game-handoff DMA. */
    if (!s_active) {
        if (!fntrace_is_game_started()) return;
        s_active = 1;
    }

    /* Dedup by load_addr. */
    for (i = 0; i < s_count; i++) {
        if (s_entries[i].load_addr == load_addr)
            return;
    }

    if (s_count >= MAX_OVERLAYS) return;

    e = &s_entries[s_count++];
    e->load_addr = load_addr;
    e->size      = size;
    e->state     = OV_QUEUED;
    e->bytes     = (uint8_t *)malloc(size);
    if (e->bytes) memcpy(e->bytes, bytes, size);

    /* A-1: check if a compiled DLL exists in cache for these bytes. */
    overlay_loader_check_cache(load_addr, size, bytes);
    /* A-2: push onto compilation queue (to be added in A-2). */
}

/* ---- JSON output — assembled dirty_ram regions -------------------------- */

/* Emit every dirty-page run within [win_lo_page, win_hi_page) as a capture
 * region. Runs are clamped to the window so region_start keys are stable
 * across window boundaries: the kernel window must never merge with
 * boot-text pages (both can be dirty and adjacent — kernel via CPU stores,
 * boot-text via overlay CD DMA), and a boot-text run must never merge
 * across the overlay-region floor (existing [FLOOR,...) DLL keys would
 * shift). Dirty boot-text runs ARE captured — post-baseline they are live
 * overlay code (see dirty_ram_interp.h window model). */
static void write_json_window(FILE *f, uint32_t win_lo_page,
                              uint32_t win_hi_page, int *first_region,
                              const uint32_t *bitmap,
                              const DirtyRamPcEntry *pc_table,
                              const DirtyRamPcEntry *exec_pc_table,
                              const uint8_t *ram_base)
{
    uint32_t page_sz = 4096u;
    uint32_t page, run_start;
    int      in_run;

    in_run    = 0;
    run_start = 0;

    for (page = win_lo_page; page <= win_hi_page; page++) {
        int dirty = 0;
        if (page < win_hi_page) {
            uint32_t word = bitmap[page >> 5];
            dirty = (word >> (page & 31u)) & 1u;
        }

        if (dirty && !in_run) {
            in_run    = 1;
            run_start = page;
        } else if (!dirty && in_run) {
            uint32_t phys = run_start * page_sz;
            uint32_t size = (page - run_start) * page_sz;
            uint32_t virt = 0x80000000u | (phys & 0x1FFFFFu);
            int j;

            in_run = 0;

            /* Seeds: only per-PC interpreter hits — execution-verified. */
#define MAX_CAPTURE_PCS DIRTY_RAM_PC_TABLE_SIZE
            uint32_t *executed = (uint32_t *)malloc(
                MAX_CAPTURE_PCS * sizeof(uint32_t));
            uint32_t *dispatch = (uint32_t *)malloc(
                MAX_CAPTURE_PCS * sizeof(uint32_t));
            int nexec = 0;
            int ndisp = 0;
            if (!executed || !dispatch) {
                free(executed);
                free(dispatch);
                continue;
            }

            for (j = 0; j < DIRTY_RAM_PC_TABLE_SIZE; j++) {
                const DirtyRamPcEntry *pe = &pc_table[j];
                if (pe->pc == 0 || pe->hits == 0) continue;
                if (pe->pc < phys || pe->pc >= phys + size) continue;
                dispatch[ndisp++] = pe->pc;
            }
            for (j = 0; j < DIRTY_RAM_PC_TABLE_SIZE; j++) {
                const DirtyRamPcEntry *pe = &exec_pc_table[j];
                if (pe->pc == 0 || pe->hits == 0) continue;
                if (pe->pc < phys || pe->pc >= phys + size) continue;
                executed[nexec++] = pe->pc;
            }

            /* Skip pure-data regions — nothing to compile. */
            if (ndisp == 0 && nexec == 0) {
                free(executed);
                free(dispatch);
                continue;
            }

            /* Execution-time capture (§2.2 fix): snapshot the overlay region
             * from LIVE RAM, i.e. the bytes as they ACTUALLY EXECUTE — after the
             * game's load-time fixups/relocation. The old code assembled
             * "unpatched" bytes from DMA-time blocks, which omitted relocated
             * jump tables and other fixed-up data: the recompiler then could not
             * resolve `jr`-jump-tables (their address tables were wrong), fell
             * back to call_by_address, and native diverged from the interpreter
             * (the village→overworld blue screen). Live RAM is the faithful
             * image. (Capture at a COHERENT moment — one overlay freshly loaded,
             * via overlay_capture_dump — to avoid merging overlay generations.) */
            if (!*first_region) fprintf(f, ",\n");
            *first_region = 0;

            fprintf(f, "  {\n");
            fprintf(f, "    \"schema\": \"psxrecomp overlay capture v2\",\n");
            fprintf(f, "    \"load_addr\": \"0x%08X\",\n", virt);
            fprintf(f, "    \"size\": %u,\n", size);
            fprintf(f, "    \"bytes_b64\": \"");
            write_b64(f, ram_base + phys, size);
            fprintf(f, "\",\n");

            fprintf(f, "    \"executed_pcs\": [");
            for (j = 0; j < nexec; j++) {
                uint32_t seed_virt = 0x80000000u | (executed[j] & 0x1FFFFFu);
                if (j > 0) fprintf(f, ", ");
                fprintf(f, "\"0x%08X\"", seed_virt);
            }
            fprintf(f, "],\n");

            fprintf(f, "    \"dispatch_entry_pcs\": [");
            for (j = 0; j < ndisp; j++) {
                uint32_t seed_virt = 0x80000000u | (dispatch[j] & 0x1FFFFFu);
                if (j > 0) fprintf(f, ", ");
                fprintf(f, "\"0x%08X\"", seed_virt);
            }
            fprintf(f, "],\n");

            fprintf(f, "    \"function_entry_pcs\": [],\n");

            fprintf(f, "    \"seeds\": [");
            for (j = 0; j < ndisp; j++) {
                uint32_t seed_virt = 0x80000000u | (dispatch[j] & 0x1FFFFFu);
                if (j > 0) fprintf(f, ", ");
                fprintf(f, "\"0x%08X\"", seed_virt);
            }
            fprintf(f, "]\n");
            fprintf(f, "  }");
            free(executed);
            free(dispatch);
        }
    }
}

static int write_json_snapshot(const char *path, uint32_t bw,
                               const uint32_t *bitmap,
                               const DirtyRamPcEntry *pc_table,
                               const DirtyRamPcEntry *exec_pc_table,
                               const uint8_t *ram_base)
{
    FILE    *f;
    uint32_t page_sz;
    int      first_region;
    page_sz = 4096u;
    f = fopen(path, "w");
    if (!f) return 0;

    fprintf(f, "[\n");
    first_region = 1;

    /* Kernel window, then dirty boot-text, then the overlay region (see
     * dirty_ram_interp.h for the window model). The boot-text window
     * [KERNEL_WINDOW_END, FLOOR) emits only genuinely-overlaid runs: the
     * game-start baseline cleared the EXE-load false positives, so a dirty
     * page there is runtime overlay code by definition. Windows stay
     * separate so runs clamp at the boundaries and region_start keys match
     * try_load_region's clamped walkback (existing overlay-region DLL keys
     * are unchanged). */
    write_json_window(f, 0u,
                      DIRTY_RAM_KERNEL_WINDOW_END / page_sz, &first_region,
                      bitmap, pc_table, exec_pc_table, ram_base);
    write_json_window(f, DIRTY_RAM_KERNEL_WINDOW_END / page_sz,
                      OVERLAY_REGION_FLOOR / page_sz, &first_region,
                      bitmap, pc_table, exec_pc_table, ram_base);
    write_json_window(f, OVERLAY_REGION_FLOOR / page_sz, bw * 32u,
                      &first_region, bitmap, pc_table, exec_pc_table, ram_base);

    fprintf(f, "\n]\n");
    fclose(f);
    return 1;
}

void overlay_capture_write_json(void)
{
    extern uint8_t *memory_get_ram_ptr(void);
    char path[600];
    uint32_t bw = dirty_ram_get_bitmap_word_count();
    uint32_t *bitmap;
    if (!s_active) return;
    bitmap = (uint32_t *)malloc((size_t)bw * sizeof(uint32_t));
    if (!bitmap) return;
    for (uint32_t i = 0; i < bw; i++) bitmap[i] = dirty_ram_get_bitmap_word(i);
    snprintf(path, sizeof(path), "%s/overlay_captures.json", s_out_dir);
    write_json_snapshot(path, bw, bitmap, g_dirty_ram_pc_table,
                        g_dirty_ram_exec_pc_table, memory_get_ram_ptr());
    free(bitmap);
}

int overlay_capture_count(void)
{
    return s_count;
}

/* ---- Variant-capture automation (step 2.8) -------------------------------
 * Trigger = sustained dirty-RAM-interp pressure inside a capture window
 * (g_dirty_window_dispatches), sampled every AUTOCAP_CHECK_FRAMES vblanks,
 * gated on NOT loading (cdrom_load_in_progress — captures must be taken at
 * a coherent moment, never mid-load) plus a cooldown and a session cap.
 * On fire: write overlay_captures.json and kick the configured background
 * compile (autocompile.c). Dedup is the tool's job — compile_overlays.py
 * skips image CRCs already in the cache — and the loop self-limits: once
 * the hot code goes native the pressure signal drops to zero and no more
 * captures fire. */
/* Cadence/threshold retune 2026-07-06 (measured, Tomba2 attract): a single
 * interp dispatch can chain ~129K insns, so DISPATCH count under-reports
 * pressure by orders of magnitude — a scene burning >50% of wall time in the
 * interpreter produced ~250 dispatches per old 10s window, hovering at the
 * old 256 gate (triggers effectively never fired mid-scene). Pressure is now
 * EITHER signal: dispatch count (many short stubs) OR interpreted-insn count
 * (few long chains). ~250K insns/window ≈ interp >~2.5% of wall — worth a
 * capture. Faster cadence + short cooldown are safe: fires are still gated
 * on a coherent moment (not loading, provider idle) and the loop self-limits
 * as coverage converges (pressure → 0). */
/* There is deliberately NO lifetime trigger cap. A 64-fire "session backstop"
 * shipped 2026-07-06 and was exhausted ~4h into an attract soak, silently
 * freezing coverage growth with interp still ~50% of wall in 2D scenes —
 * convergence IS many fires over a long session. The runaway case a cap was
 * papering over (pressure that never converts: excluded PCs, failing shards
 * — fires every cooldown forever, spawning futile provider runs) is handled
 * by FUTILITY BACKOFF instead: a fire is skipped when the capture manifest
 * is byte-identical to the last compile request AND that request registered
 * no new candidates; skips retry on exponential backoff and any change in
 * either signal resets to normal cadence. Concurrency is already bounded
 * (cp->busy defers fires while a compile is in flight) and identical images
 * dedup downstream (compile_overlays.py skips cached CRCs). */
#define AUTOCAP_CHECK_FRAMES    120u   /* ~2 s between pressure samples     */
#define AUTOCAP_MIN_DISPATCHES  128u   /* catches ~120/s FMV helper gaps    */
#define AUTOCAP_MIN_INSNS       100000ull /* long compute gap pressure gate */
#define AUTOCAP_COOLDOWN_FRAMES 300u   /* >= ~5 s between auto-fires        */
#define AUTOCAP_BACKOFF_MAX     64u    /* futile-retry ceiling: 64*5s ≈ 5min */

static int      s_autocap_enabled    = 0;
static uint64_t s_autocap_last_check = 0;
static uint64_t s_autocap_last_fire  = 0;
static uint64_t s_autocap_last_disp  = 0;
static uint64_t s_autocap_last_insns = 0;
static uint32_t s_autocap_triggers   = 0;
static uint64_t s_autocap_last_delta = 0;
static uint64_t s_autocap_last_insns_delta = 0;
static uint64_t s_autocap_next_ok    = 0;    /* frame gate for next attempt  */
static uint64_t s_autocap_sig_at_req = 0;    /* manifest FNV at last request */
static int      s_autocap_reg_at_req = -1;   /* loader candidates at last req */
static uint32_t s_autocap_backoff    = 1;    /* cooldown multiplier (futile)  */
static uint32_t s_autocap_futile     = 0;    /* futile skips (telemetry)      */

typedef struct {
    uint8_t *ram;
    DirtyRamPcEntry *pc_table;
    DirtyRamPcEntry *exec_pc_table;
    uint32_t *bitmap;
    uint32_t bitmap_words;
    uint64_t manifest_sig;
    char path[600];
} AutocapWriteJob;
static SDL_atomic_t s_autocap_write_state; /* 0 idle, 1 writing, 2 complete */
static SDL_Thread *s_autocap_write_thread;
static AutocapWriteJob *s_autocap_write_job;
static int s_autocap_seeded_existing;

static uint64_t autocap_manifest_sig(void);

void overlay_autocapture_set_enabled(int on) {
    s_autocap_enabled = on ? 1 : 0;
    /* Seed the shipped/previous-session contribution file before gameplay.
     * An unchanged first auto-capture can then back off without launching a
     * compiler merely because this process has no request history yet. */
    if (s_autocap_enabled && s_autocap_sig_at_req == 0) {
        uint64_t sig = autocap_manifest_sig();
        if (sig) {
            s_autocap_sig_at_req = sig;
            s_autocap_seeded_existing = 1;
        }
    }
}

void overlay_autocapture_get_status(int *enabled, uint32_t *triggers,
                                    uint64_t *last_delta) {
    if (enabled)    *enabled    = s_autocap_enabled;
    if (triggers)   *triggers   = s_autocap_triggers;
    if (last_delta) *last_delta = s_autocap_last_delta;
}

uint64_t overlay_autocapture_last_insns_delta(void) {
    return s_autocap_last_insns_delta;
}

void overlay_autocapture_get_futility(uint32_t *backoff, uint32_t *futile) {
    if (backoff) *backoff = s_autocap_backoff;
    if (futile)  *futile  = s_autocap_futile;
}

/* FNV-1a of the just-written capture manifest. The manifest is a pure
 * function of the capture inputs (live overlay bytes + observed seed-PC
 * sets, both grow-only), so an unchanged hash means a compile request
 * would redo exactly the work of the previous one. */
static uint64_t autocap_manifest_sig(void)
{
    char path[600];
    FILE *f;
    uint64_t h = 1469598103934665603ull;
    unsigned char buf[65536];
    size_t n, i;
    snprintf(path, sizeof(path), "%s/overlay_captures.json", s_out_dir);
    f = fopen(path, "rb");
    if (!f) return 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        for (i = 0; i < n; i++) { h ^= buf[i]; h *= 1099511628211ull; }
    fclose(f);
    return h;
}

static void autocap_write_job_free(AutocapWriteJob *job)
{
    if (!job) return;
    free(job->ram); free(job->pc_table); free(job->exec_pc_table);
    free(job->bitmap); free(job);
}

static int autocap_write_thread_main(void *opaque)
{
    AutocapWriteJob *job = (AutocapWriteJob *)opaque;
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
    if (write_json_snapshot(job->path, job->bitmap_words, job->bitmap,
                            job->pc_table, job->exec_pc_table, job->ram))
        job->manifest_sig = autocap_manifest_sig();
    SDL_AtomicSet(&s_autocap_write_state, 2);
    return 0;
}

/* Copy coherent inputs on the emulation thread, then perform base64 formatting,
 * file I/O, and verification reread on a worker. */
static int autocap_write_start(void)
{
    extern uint8_t *memory_get_ram_ptr(void);
    const size_t ram_size = 2u * 1024u * 1024u;
    uint32_t bw = dirty_ram_get_bitmap_word_count();
    AutocapWriteJob *job = (AutocapWriteJob *)calloc(1, sizeof(*job));
    if (!job) return 0;
    job->ram = (uint8_t *)malloc(ram_size);
    job->pc_table = (DirtyRamPcEntry *)malloc(sizeof(g_dirty_ram_pc_table));
    job->exec_pc_table = (DirtyRamPcEntry *)malloc(sizeof(g_dirty_ram_exec_pc_table));
    job->bitmap = (uint32_t *)malloc((size_t)bw * sizeof(uint32_t));
    if (!job->ram || !job->pc_table || !job->exec_pc_table || !job->bitmap) {
        autocap_write_job_free(job); return 0;
    }
    memcpy(job->ram, memory_get_ram_ptr(), ram_size);
    memcpy(job->pc_table, g_dirty_ram_pc_table, sizeof(g_dirty_ram_pc_table));
    memcpy(job->exec_pc_table, g_dirty_ram_exec_pc_table,
           sizeof(g_dirty_ram_exec_pc_table));
    for (uint32_t i = 0; i < bw; i++) job->bitmap[i] = dirty_ram_get_bitmap_word(i);
    job->bitmap_words = bw;
    snprintf(job->path, sizeof(job->path), "%s/overlay_captures.json", s_out_dir);
    s_autocap_write_job = job;
    SDL_AtomicSet(&s_autocap_write_state, 1);
    s_autocap_write_thread = SDL_CreateThread(autocap_write_thread_main,
                                               "overlay-capture-write", job);
    if (!s_autocap_write_thread) {
        SDL_AtomicSet(&s_autocap_write_state, 0);
        s_autocap_write_job = NULL;
        autocap_write_job_free(job);
        return 0;
    }
    return 1;
}

void overlay_autocapture_tick(void)
{
    extern uint64_t s_frame_count;
    extern int cdrom_load_in_progress(void);
    const CodeProvider *cp = code_provider_active();

    if (SDL_AtomicGet(&s_autocap_write_state) == 2) {
        AutocapWriteJob *job = s_autocap_write_job;
        SDL_WaitThread(s_autocap_write_thread, NULL);
        s_autocap_write_thread = NULL;
        s_autocap_write_job = NULL;
        SDL_AtomicSet(&s_autocap_write_state, 0);
        uint64_t sig = job ? job->manifest_sig : 0;
        int reg = overlay_loader_registered_count();
        if (sig && sig == s_autocap_sig_at_req &&
            (s_autocap_seeded_existing || reg == s_autocap_reg_at_req)) {
            s_autocap_futile++;
            s_autocap_reg_at_req = reg;
            s_autocap_seeded_existing = 0;
            if (s_autocap_backoff < AUTOCAP_BACKOFF_MAX)
                s_autocap_backoff <<= 1;
            s_autocap_next_ok = s_frame_count +
                (uint64_t)AUTOCAP_COOLDOWN_FRAMES * s_autocap_backoff;
        } else if (sig) {
            s_autocap_seeded_existing = 0;
            s_autocap_backoff = 1;
            s_autocap_sig_at_req = sig;
            s_autocap_reg_at_req = reg;
            s_autocap_triggers++;
            if (cp->request) cp->request();
        }
        autocap_write_job_free(job);
        return;
    }

    if (!s_autocap_enabled || !s_active) return;
    if (SDL_AtomicGet(&s_autocap_write_state) != 0) return;
    if (s_frame_count - s_autocap_last_check < AUTOCAP_CHECK_FRAMES) return;
    s_autocap_last_check = s_frame_count;

    uint64_t disp  = g_dirty_window_dispatches;
    uint64_t delta = disp - s_autocap_last_disp;
    s_autocap_last_disp  = disp;
    s_autocap_last_delta = delta;
    uint64_t insns = g_dirty_ram_insns_run;
    uint64_t insns_delta = insns - s_autocap_last_insns;
    s_autocap_last_insns = insns;
    s_autocap_last_insns_delta = insns_delta;

    if (delta < AUTOCAP_MIN_DISPATCHES && insns_delta < AUTOCAP_MIN_INSNS)
        return;
    if (cdrom_load_in_progress()) return;          /* coherent moment only  */
    if (cp->busy && cp->busy()) return;
    if (s_frame_count < s_autocap_next_ok) return; /* cooldown (x backoff)  */

    /* Snapshot coherent inputs for the player-shareable coverage manifest;
     * a worker writes/hashes it, then a later vblank applies futility/backoff.
     * Unchanged content with no new candidates backs off without a provider run. */
    s_autocap_last_fire = s_frame_count;
    s_autocap_next_ok = s_frame_count + AUTOCAP_COOLDOWN_FRAMES;
    autocap_write_start();
}

uint32_t overlay_capture_get_region_crc(uint32_t region_start,
                                         uint32_t region_size)
{
    int j;
    uint8_t *image = (uint8_t *)calloc(region_size, 1);
    if (!image) return 0;

    for (j = 0; j < s_count; j++) {
        OvEntry *blk = &s_entries[j];
        if (!blk->bytes) continue;
        if (blk->load_addr < region_start ||
            blk->load_addr >= region_start + region_size) continue;
        uint32_t off     = blk->load_addr - region_start;
        uint32_t copy_sz = blk->size;
        if (off + copy_sz > region_size) copy_sz = region_size - off;
        memcpy(image + off, blk->bytes, copy_sz);
    }

    uint32_t crc = crc32_compute(image, region_size);
    free(image);
    return crc;
}
