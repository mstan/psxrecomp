#include "overlay_capture.h"
#include "overlay_loader.h"
#include "dirty_ram_interp.h"
#include "code_provider.h"
#include "crc32.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <SDL.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <direct.h>
#  include <io.h>
#  define CAPTURE_MKDIR(path) _mkdir(path)
#  define CAPTURE_FSYNC(file) _commit(_fileno(file))
#  define CAPTURE_PID() ((unsigned long)GetCurrentProcessId())
#else
#  include <sys/stat.h>
#  include <unistd.h>
#  define CAPTURE_MKDIR(path) mkdir((path), 0755)
#  define CAPTURE_FSYNC(file) fsync(fileno(file))
#  define CAPTURE_PID() ((unsigned long)getpid())
#endif

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
static char     s_capture_path[768];
static int      s_active   = 0;
static int      s_enabled  = 0;   /* config gate; off unless overlay cache enabled */
static SDL_mutex *s_commit_mutex;
static SDL_atomic_t s_snapshot_seq;
static uint64_t s_latest_commit_seq;

static int      s_history_enabled = 0;
static char     s_history_addendum[600];
static char     s_history_persist_dir[600];
static char     s_history_game_id[64];
static char     s_history_session[64];
static uint32_t s_history_sequence;
static uint64_t s_history_last_sig;
static int      s_history_written;
static SDL_atomic_t s_autocap_write_state; /* 0 idle, 1 writing, 2 complete */

static uint64_t capture_next_sequence(void)
{
    /* Never contend with the writer's long-held file commit mutex here: this
     * runs at a pre-write boundary on the emulation thread. */
    return (uint32_t)SDL_AtomicAdd(&s_snapshot_seq, 1) + 1u;
}

static int preserve_snapshot_async(uint32_t scope_lo, uint32_t scope_hi);
static int preserve_writer_start(void);

static int capture_process_id(void)
{
#ifdef _WIN32
    return _getpid();
#else
    return (int)getpid();
#endif
}

static void sanitize_filename(char *dst, size_t dst_size, const char *src)
{
    size_t out = 0;
    if (!dst_size) return;
    while (src && *src && out + 1 < dst_size) {
        unsigned char c = (unsigned char)*src++;
        dst[out++] = (char)(isalnum(c) || c == '-' || c == '_' ? c : '_');
    }
    dst[out] = '\0';
}

static int sync_file(FILE *f)
{
    if (fflush(f) != 0) return 0;
#ifdef _WIN32
    return _commit(_fileno(f)) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

static int atomic_replace_file(const char *tmp_path, const char *path)
{
#ifdef _WIN32
    return MoveFileExA(tmp_path, path,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(tmp_path, path) == 0;
#endif
}

static uint64_t capture_file_sig(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint64_t h = 1469598103934665603ull;
    unsigned char buf[65536];
    size_t n, i;
    if (!f) return 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        for (i = 0; i < n; i++) { h ^= buf[i]; h *= 1099511628211ull; }
    if (ferror(f)) h = 0;
    fclose(f);
    return h;
}

/* Directory portion of `path` (no trailing separator); "." when `path` has
 * no separator. Colocates durable-history bookkeeping (the addendum.jsonl
 * and optional persist_dir copies) with whichever canonical capture manifest
 * path is currently configured, whether that came from
 * overlay_capture_set_out_dir or the game-specific overlay_capture_set_path. */
static void capture_path_dirname(char *dst, size_t dst_size, const char *path)
{
    const char *cut = strrchr(path, '/');
#ifdef _WIN32
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!cut || bslash > cut)) cut = bslash;
#endif
    if (!dst_size) return;
    if (cut) {
        size_t len = (size_t)(cut - path);
        if (len >= dst_size) len = dst_size - 1;
        memcpy(dst, path, len);
        dst[len] = '\0';
    } else {
        snprintf(dst, dst_size, ".");
    }
}

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
    snprintf(s_capture_path, sizeof(s_capture_path),
             "%s/overlay_captures.json", out_dir ? out_dir : ".");
    if (!s_commit_mutex) s_commit_mutex = SDL_CreateMutex();
    (void)preserve_writer_start();
}

int overlay_capture_set_path(const char *path)
{
    const char *resolved = (path && path[0]) ? path : "overlay_captures.json";
    if (strlen(resolved) >= sizeof(s_capture_path)) {
        fprintf(stderr, "psxrecomp: overlay capture path is too long (%zu bytes)\n",
                strlen(resolved));
        return 0;
    }
    snprintf(s_capture_path, sizeof(s_capture_path), "%s", resolved);
    if (!s_commit_mutex) s_commit_mutex = SDL_CreateMutex();
    (void)preserve_writer_start();
    return 1;
}

void overlay_capture_init(const char *out_dir)
{
    overlay_capture_set_out_dir(out_dir);
}

void overlay_capture_set_enabled(int enabled)
{
    s_enabled = enabled ? 1 : 0;
}

void overlay_capture_configure_history(int enabled, const char *persist_dir,
                                       const char *game_id)
{
    time_t now;
    struct tm tm_now;
    char stamp[32];
    s_history_enabled = enabled ? 1 : 0;
    s_history_persist_dir[0] = '\0';
    s_history_sequence = 0;
    s_history_last_sig = 0;
    s_history_written = 0;
    sanitize_filename(s_history_game_id, sizeof(s_history_game_id),
                      game_id && game_id[0] ? game_id : "UNKNOWN");
    if (persist_dir && persist_dir[0]) {
        snprintf(s_history_persist_dir, sizeof(s_history_persist_dir), "%s",
                 persist_dir);
    }
    {
        char dir[512];
        capture_path_dirname(dir, sizeof(dir),
                             s_capture_path[0] ? s_capture_path
                                               : "overlay_captures.json");
        snprintf(s_history_addendum, sizeof(s_history_addendum),
                 "%s/overlay_captures.addendum.jsonl", dir);
    }

    now = time(NULL);
#ifdef _WIN32
    gmtime_s(&tm_now, &now);
#else
    gmtime_r(&now, &tm_now);
#endif
    strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", &tm_now);
    snprintf(s_history_session, sizeof(s_history_session), "%s-p%d",
             stamp, capture_process_id());
    if (s_history_enabled) {
        fprintf(stdout,
                "psxrecomp: durable overlay capture history enabled (%s%s%s)\n",
                s_history_addendum,
                s_history_persist_dir[0] ? "; immutable dir=" : "",
                s_history_persist_dir[0] ? s_history_persist_dir : "");
    }
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

    /* This table reconstructs only the CURRENT DMA image for the legacy CRC
     * helper. Executed historical variants live in the additive snapshot
     * journal; retaining every DMA payload here caused unbounded session heap
     * growth and made A->B->A report stale B bytes. */
    for (i = 0; i < s_count; i++) {
        if (s_entries[i].load_addr == load_addr) {
            if (s_entries[i].size == size && s_entries[i].bytes &&
                memcmp(s_entries[i].bytes, bytes, size) == 0)
                return;
            uint8_t *replacement = (uint8_t *)malloc(size);
            if (!replacement) return;
            memcpy(replacement, bytes, size);
            free(s_entries[i].bytes);
            s_entries[i].bytes = replacement;
            s_entries[i].size = size;
            s_entries[i].state = OV_QUEUED;
            overlay_loader_check_cache(load_addr, size, bytes);
            return;
        }
    }

    if (s_count >= MAX_OVERLAYS) return;

    e = &s_entries[s_count++];
    e->load_addr = load_addr;
    e->size      = size;
    e->state     = OV_QUEUED;
    e->bytes     = (uint8_t *)malloc(size);
    if (!e->bytes) { s_count--; return; }
    memcpy(e->bytes, bytes, size);

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
                              const uint32_t *dispatch_pc_bitmap,
                              const uint32_t *exec_pc_bitmap,
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
            /* Carry one coherent guard instruction beyond a dirty-page run.
             * A MIPS branch/jump at the final word of the run (...FFC) always
             * executes its delay slot in the next page (...000).  Supplying
             * that word lets overlay codegen emit exact semantics and lets its
             * range manifest hash/watch the slot. Keep the established region
             * start. A four-byte overlap across an artificial kernel/boot or
             * boot/overlay capture-window boundary is required: those windows
             * stabilize region keys, but they are not MIPS execution barriers.
             * Adjacent dirty pages were already folded into this run. */
            const uint32_t ram_size = 2u * 1024u * 1024u;
            if (phys <= ram_size && size <= ram_size - phys &&
                phys + size <= ram_size - 4u)
                size += 4u;
            uint32_t virt = 0x80000000u | (phys & 0x1FFFFFu);
            in_run = 0;

            /* Seeds: only per-PC interpreter hits — execution-verified. */
            int nexec = 0;
            int ndisp = 0;
            for (uint32_t ep = phys; ep < phys + size; ep += 4u) {
                uint32_t wi = ep >> 2;
                if ((dispatch_pc_bitmap[wi >> 5] >> (wi & 31u)) & 1u)
                    ndisp++;
                if ((exec_pc_bitmap[wi >> 5] >> (wi & 31u)) & 1u)
                    nexec++;
            }

            /* Skip pure-data regions — nothing to compile. */
            if (ndisp == 0 && nexec == 0) {
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
            int emitted_exec = 0;
            for (uint32_t ep = phys; ep < phys + size; ep += 4u) {
                uint32_t wi = ep >> 2;
                if (!((exec_pc_bitmap[wi >> 5] >> (wi & 31u)) & 1u))
                    continue;
                uint32_t seed_virt = 0x80000000u | ep;
                if (emitted_exec++) fprintf(f, ", ");
                fprintf(f, "\"0x%08X\"", seed_virt);
            }
            fprintf(f, "],\n");

            fprintf(f, "    \"dispatch_entry_pcs\": [");
            int emitted_dispatch = 0;
            for (uint32_t ep = phys; ep < phys + size; ep += 4u) {
                uint32_t wi = ep >> 2;
                if (!((dispatch_pc_bitmap[wi >> 5] >> (wi & 31u)) & 1u))
                    continue;
                uint32_t seed_virt = 0x80000000u | ep;
                if (emitted_dispatch++) fprintf(f, ", ");
                fprintf(f, "\"0x%08X\"", seed_virt);
            }
            fprintf(f, "],\n");

            fprintf(f, "    \"function_entry_pcs\": [],\n");

            fprintf(f, "    \"seeds\": [");
            emitted_dispatch = 0;
            for (uint32_t ep = phys; ep < phys + size; ep += 4u) {
                uint32_t wi = ep >> 2;
                if (!((dispatch_pc_bitmap[wi >> 5] >> (wi & 31u)) & 1u))
                    continue;
                uint32_t seed_virt = 0x80000000u | ep;
                if (emitted_dispatch++) fprintf(f, ", ");
                fprintf(f, "\"0x%08X\"", seed_virt);
            }
            fprintf(f, "]\n");
            fprintf(f, "  }");
        }
    }
}

static int write_json_snapshot(const char *path, uint32_t bw,
                               const uint32_t *bitmap,
                               const uint32_t *dispatch_pc_bitmap,
                               const uint32_t *exec_pc_bitmap,
                               const uint8_t *ram_base)
{
    FILE    *f;
    uint32_t page_sz;
    int      first_region;
    page_sz = 4096u;
    f = fopen(path, "wb");
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
                      bitmap, dispatch_pc_bitmap, exec_pc_bitmap, ram_base);
    write_json_window(f, DIRTY_RAM_KERNEL_WINDOW_END / page_sz,
                      OVERLAY_REGION_FLOOR / page_sz, &first_region,
                      bitmap, dispatch_pc_bitmap, exec_pc_bitmap, ram_base);
    write_json_window(f, OVERLAY_REGION_FLOOR / page_sz, bw * 32u,
                      &first_region, bitmap, dispatch_pc_bitmap,
                      exec_pc_bitmap, ram_base);

    fprintf(f, "\n]\n");
    {
        int ok = !ferror(f) && sync_file(f);
        if (fclose(f) != 0) ok = 0;
        return ok;
    }
}

static int capture_copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out;
    unsigned char buf[65536];
    size_t n;
    int ok = 1;
    if (!in) return 0;
    out = fopen(dst, "wb");
    if (!out) { fclose(in); return 0; }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    }
    if (ferror(in)) ok = 0;
    if (ok && (fflush(out) != 0 || CAPTURE_FSYNC(out) != 0)) ok = 0;
    if (fclose(out) != 0) ok = 0;
    fclose(in);
    if (!ok) remove(dst);
    return ok;
}

static int capture_files_equal(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    unsigned char ba[65536], bb[65536];
    int equal = 1;
    if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); return 0; }
    for (;;) {
        size_t na = fread(ba, 1, sizeof(ba), fa);
        size_t nb = fread(bb, 1, sizeof(bb), fb);
        if (na != nb || (na && memcmp(ba, bb, na) != 0)) { equal = 0; break; }
        if (na == 0) break;
    }
    if (ferror(fa) || ferror(fb)) equal = 0;
    fclose(fa); fclose(fb);
    return equal;
}

static int copy_file_atomic(const char *src_path, const char *dst_path)
{
    char tmp_path[700];
    unsigned char buf[65536];
    FILE *src, *dst;
    size_t n;
    int ok = 1;
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp-p%d", dst_path,
             capture_process_id());
    src = fopen(src_path, "rb");
    if (!src) return 0;
    dst = fopen(tmp_path, "wb");
    if (!dst) { fclose(src); return 0; }
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) { ok = 0; break; }
    }
    if (ferror(src)) ok = 0;
    fclose(src);
    if (ok) ok = sync_file(dst);
    if (fclose(dst) != 0) ok = 0;
    if (!ok || !atomic_replace_file(tmp_path, dst_path)) {
        remove(tmp_path);
        return 0;
    }
    return 1;
}

/* Append the canonical JSON array as one minified JSONL record. Whitespace is
 * stripped only outside strings, so bytes_b64 remains byte-for-byte intact.
 * A crash can damage only the final line; every earlier launch remains a valid
 * independently parseable record. */
static int append_history_record(const char *snapshot_path, const char *reason,
                                 uint64_t sig, uint32_t sequence)
{
    FILE *src = fopen(snapshot_path, "rb");
    FILE *dst, *tail;
    int c, in_string = 0, escape = 0, ok = 1;
    int needs_separator = 0;
    if (!src) return 0;
    tail = fopen(s_history_addendum, "rb");
    if (tail) {
        if (fseek(tail, -1, SEEK_END) == 0) {
            c = fgetc(tail);
            needs_separator = c != '\n';
        }
        fclose(tail);
    }
    dst = fopen(s_history_addendum, "ab");
    if (!dst) { fclose(src); return 0; }
    /* Quarantine an incomplete final record from a previous hard kill. */
    if (needs_separator && fputc('\n', dst) == EOF) ok = 0;
    if (ok && fprintf(dst,
            "{\"schema\":\"psxrecomp overlay capture addendum v1\","
            "\"game\":\"%s\",\"session\":\"%s\",\"sequence\":%u,"
            "\"reason\":\"%s\",\"fnv64\":\"%016llX\",\"captures\":",
            s_history_game_id, s_history_session, sequence,
            reason ? reason : "snapshot", (unsigned long long)sig) < 0) ok = 0;
    while (ok && (c = fgetc(src)) != EOF) {
        if (in_string) {
            if (fputc(c, dst) == EOF) { ok = 0; break; }
            if (escape) escape = 0;
            else if (c == '\\') escape = 1;
            else if (c == '"') in_string = 0;
        } else {
            if (c == '"') {
                in_string = 1;
                if (fputc(c, dst) == EOF) { ok = 0; break; }
            } else if (!isspace((unsigned char)c)) {
                if (fputc(c, dst) == EOF) { ok = 0; break; }
            }
        }
    }
    if (ferror(src) || in_string) ok = 0;
    fclose(src);
    if (ok && fprintf(dst, "}\n") < 0) ok = 0;
    if (ok) ok = sync_file(dst);
    if (fclose(dst) != 0) ok = 0;
    return ok;
}

static int write_json_string(FILE *dst, const char *value)
{
    const unsigned char *p = (const unsigned char *)(value ? value : "");
    if (fputc('"', dst) == EOF) return 0;
    for (; *p; p++) {
        if (*p == '"' || *p == '\\') {
            if (fputc('\\', dst) == EOF || fputc(*p, dst) == EOF) return 0;
        } else if (*p < 0x20) {
            if (fprintf(dst, "\\u%04X", (unsigned)*p) < 0) return 0;
        } else if (fputc(*p, dst) == EOF) {
            return 0;
        }
    }
    return fputc('"', dst) != EOF;
}

/* Dev persistence already owns an atomically published immutable full JSON
 * snapshot. Append only a durable reference to it instead of embedding the
 * same multi-megabyte capture array again every autocap interval. Production
 * configs without persist_dir continue to use the self-contained v1 record. */
static int append_history_reference(const char *persist_path, const char *reason,
                                    uint64_t sig, uint32_t sequence)
{
    FILE *dst, *tail;
    int c, needs_separator = 0, ok = 1;
    tail = fopen(s_history_addendum, "rb");
    if (tail) {
        if (fseek(tail, -1, SEEK_END) == 0) {
            c = fgetc(tail);
            needs_separator = c != '\n';
        }
        fclose(tail);
    }
    dst = fopen(s_history_addendum, "ab");
    if (!dst) return 0;
    if (needs_separator && fputc('\n', dst) == EOF) ok = 0;
    if (ok && fprintf(dst,
            "{\"schema\":\"psxrecomp overlay capture addendum v2\","
            "\"game\":\"%s\",\"session\":\"%s\",\"sequence\":%u,"
            "\"reason\":\"%s\",\"fnv64\":\"%016llX\",\"snapshot\":",
            s_history_game_id, s_history_session, sequence,
            reason ? reason : "snapshot", (unsigned long long)sig) < 0) ok = 0;
    if (ok) ok = write_json_string(dst, persist_path);
    if (ok && fputs("}\n", dst) == EOF) ok = 0;
    if (ok) ok = sync_file(dst);
    if (fclose(dst) != 0) ok = 0;
    return ok;
}

static void persist_history_snapshot(const char *snapshot_path,
                                     const char *reason, uint64_t sig)
{
    char persist_path[800];
    uint32_t sequence;
    int addendum_saved;
    int persist_saved = s_history_persist_dir[0] ? 0 : 1;
    if (!s_history_enabled || !sig) return;
    if (s_history_written && sig == s_history_last_sig) return;
    sequence = ++s_history_sequence;

    if (s_history_persist_dir[0]) {
        snprintf(persist_path, sizeof(persist_path),
                 "%s/%s_%s_%04u_%016llX.json",
                 s_history_persist_dir, s_history_game_id, s_history_session,
                 sequence, (unsigned long long)sig);
        if (copy_file_atomic(snapshot_path, persist_path)) persist_saved = 1;
        else fprintf(stderr,
            "psxrecomp: failed to persist overlay capture snapshot %s\n",
            persist_path);
    }
    if (s_history_persist_dir[0] && persist_saved)
        addendum_saved = append_history_reference(
            persist_path, reason, sig, sequence);
    else
        addendum_saved = append_history_record(
            snapshot_path, reason, sig, sequence);
    if (!addendum_saved)
        fprintf(stderr, "psxrecomp: failed to append overlay capture history %s\n",
                s_history_addendum);
    /* Retry an unchanged snapshot if either configured durable sink failed.
     * Repeated addendum records or immutable files are harmless: vault merges
     * are content-keyed and idempotent. */
    if (addendum_saved && persist_saved) {
        s_history_last_sig = sig;
        s_history_written = 1;
    }
}

/* Persist one coherent snapshot for three deliberately different roles:
 *   - capture_path is the legacy/latest manifest consumed by existing tools;
 *   - capture_path.d/<content-hash>.json is immutable additive history used
 *     by compile_overlays.py / the coverage vault;
 *   - (opt-in) overlay_captures.addendum.jsonl (+ persist_dir) is a durable
 *     session-tagged history log for auditing/regeneration across restarts.
 * compile_overlays.py unions the first two. Different byte variants at the
 * same load address therefore survive future captures, rebuilds, and
 * regenerations. */
static uint64_t capture_commit_temp(const char *temp_path, const char *reason,
                                    uint64_t sequence)
{
    char contribution_dir[900];
    char contribution[1024];
    char contribution_tmp[1040];
    FILE *probe;
    uint64_t sig = capture_file_sig(temp_path);
    int history_ok = 0;
    int base_ok = 1;
    if (!sig) { remove(temp_path); return 0; }
    if (s_commit_mutex) SDL_LockMutex(s_commit_mutex);

    snprintf(contribution_dir, sizeof(contribution_dir), "%s.d", s_capture_path);
    (void)CAPTURE_MKDIR(contribution_dir); /* already-exists is success */
    for (unsigned collision = 0;; collision++) {
        if (collision == 0)
            snprintf(contribution, sizeof(contribution), "%s/%016llX.json",
                     contribution_dir, (unsigned long long)sig);
        else
            snprintf(contribution, sizeof(contribution), "%s/%016llX-%u.json",
                     contribution_dir, (unsigned long long)sig, collision);
        probe = fopen(contribution, "rb");
        if (!probe) break;
        fclose(probe);
        if (capture_files_equal(temp_path, contribution)) break;
    }
    probe = fopen(contribution, "rb");
    if (probe) {
        fclose(probe); /* exact snapshot already retained */
        history_ok = 1;
    } else {
        snprintf(contribution_tmp, sizeof(contribution_tmp), "%s.%lu.%llu.tmp",
                 contribution, CAPTURE_PID(), (unsigned long long)sequence);
        if (capture_copy_file(temp_path, contribution_tmp) &&
            atomic_replace_file(contribution_tmp, contribution)) {
            history_ok = 1;
        } else {
            remove(contribution_tmp);
            fprintf(stderr,
                "psxrecomp: ERROR: additive capture history write failed: %s\n",
                contribution);
        }
    }
    /* Snapshots may be formatted concurrently, so only the newest snapshot is
     * allowed to replace the compatibility/latest path. Immutable history is
     * authoritative for additivity and is never discarded by this ordering. */
    if (sequence >= s_latest_commit_seq) {
        base_ok = atomic_replace_file(temp_path, s_capture_path);
        if (base_ok) s_latest_commit_seq = sequence;
        else {
            remove(temp_path);
            fprintf(stderr, "psxrecomp: ERROR: latest capture write failed: %s\n",
                    s_capture_path);
        }
    } else {
        remove(temp_path);
    }
    if (s_commit_mutex) SDL_UnlockMutex(s_commit_mutex);
    /* A durable immutable contribution is sufficient to advance the evidence
     * epoch. A failed latest-copy update is noisy but cannot lose that history.
     * Persist history from the immutable contribution file (not the racy
     * "latest" path, which another writer can replace at any time), and
     * outside the commit lock since durable-history I/O (an optional
     * persist_dir copy plus a JSONL append) can be comparatively slow. */
    if (history_ok) persist_history_snapshot(contribution, reason, sig);
    return history_ok ? sig : 0;
}

static uint64_t write_and_commit_snapshot(uint32_t bw,
                                          const uint32_t *bitmap,
                                          const uint32_t *dispatch_pc_bitmap,
                                          const uint32_t *exec_pc_bitmap,
                                          const uint8_t *ram_base,
                                          const char *reason,
                                          uint64_t sequence)
{
    char temp_path[840];
    snprintf(temp_path, sizeof(temp_path), "%s.%lu.%llu.tmp", s_capture_path,
             CAPTURE_PID(), (unsigned long long)sequence);
    if (!write_json_snapshot(temp_path, bw, bitmap, dispatch_pc_bitmap,
                             exec_pc_bitmap, ram_base))
        return 0;
    return capture_commit_temp(temp_path, reason, sequence);
}

static uint64_t overlay_capture_write_current(const char *reason)
{
    extern uint8_t *memory_get_ram_ptr(void);
    uint32_t bw = dirty_ram_get_bitmap_word_count();
    uint32_t *bitmap;
    uint64_t sig;
    if (!s_active) return 0;
    bitmap = (uint32_t *)malloc((size_t)bw * sizeof(uint32_t));
    if (!bitmap) return 0;
    for (uint32_t i = 0; i < bw; i++) bitmap[i] = dirty_ram_get_bitmap_word(i);
    sig = write_and_commit_snapshot(bw, bitmap,
                                    g_dirty_ram_dispatch_pc_bitmap,
                                    g_dirty_ram_exec_pc_bitmap,
                                    memory_get_ram_ptr(),
                                    reason,
                                    capture_next_sequence());
    free(bitmap);
    return sig;
}

void overlay_capture_write_json(void)
{
    (void)overlay_capture_write_current("shutdown-or-manual");
}

void overlay_capture_before_dma(uint32_t load_addr, uint32_t size)
{
    if (!s_enabled || !s_active || size == 0) return;
    uint32_t lo = load_addr & 0x1FFFFFu;
    uint32_t hi = lo + size;
    if (lo >= 2u * 1024u * 1024u) return;
    if (hi > 2u * 1024u * 1024u || hi < lo) hi = 2u * 1024u * 1024u;

    /* Snapshot complete pages touched by this DMA. Page scope prevents sticky
     * dirty runs from turning a small sector replacement into a multi-megabyte
     * journal, while whole-page evidence catches a write to one half of a page
     * before it can hybridize code executed in the other half. */
    const uint32_t page_shift = 12u;
    uint32_t first_page = lo >> page_shift;
    uint32_t last_page = (hi - 1u) >> page_shift;
    uint32_t evidence_lo = first_page << page_shift;
    uint32_t evidence_hi = (last_page + 1u) << page_shift;

    /* Preserve an outgoing variant only when code in the affected/mergeable
     * region was interpreted. Clear only that region after the RAM+bitmap copy
     * is queued, leaving unrelated live variants in the current epoch. */
    int observed = 0;
    uint32_t first_word = evidence_lo >> 2;
    uint32_t last_word = evidence_hi >> 2;
    for (uint32_t wi = first_word; wi < last_word; wi++) {
        if ((g_dirty_ram_exec_pc_bitmap[wi >> 5] >> (wi & 31u)) & 1u) {
            observed = 1;
            break;
        }
    }
    if (!observed) return;
    if (!preserve_snapshot_async(evidence_lo, evidence_hi)) {
        /* Allocation/thread startup failure is rare and already exceptional.
         * Fall back to a synchronous durable commit rather than let the RAM
         * overwrite bind old evidence to new bytes. If storage itself fails,
         * report the unavoidable loss but still clear the stale association. */
        if (!overlay_capture_write_current("preserve-sync-fallback"))
            fprintf(stderr,
                "psxrecomp: ERROR: could not preserve outgoing overlay evidence; discarding stale epoch\n");
    }
    uint32_t first_bitmap_word = first_word >> 5;
    uint32_t bitmap_words = (last_word - first_word) >> 5;
    memset(&g_dirty_ram_exec_pc_bitmap[first_bitmap_word], 0,
           (size_t)bitmap_words * sizeof(uint32_t));
    memset(&g_dirty_ram_dispatch_pc_bitmap[first_bitmap_word], 0,
           (size_t)bitmap_words * sizeof(uint32_t));
    for (uint32_t page = first_page; page <= last_page; page++)
        g_dirty_ram_exec_page_bitmap[page >> 5] &= ~(1u << (page & 31u));
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
#define AUTOCAP_WRITE_RETRY_MAX_FRAMES 60u /* cap failed-I/O retry at ~1 s */

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
static uint64_t s_autocap_provider_sig_pending = 0;
static uint64_t s_autocap_provider_retry_frame = 0;
static unsigned s_autocap_provider_attempts = 0;

typedef struct {
    uint8_t *ram;
    uint32_t *dispatch_pc_bitmap;
    uint32_t *exec_pc_bitmap;
    uint32_t *bitmap;
    uint32_t bitmap_words;
    uint64_t manifest_sig;
    uint64_t sequence;
    uint64_t retry_frame;
    unsigned attempts;
} AutocapWriteJob;
static SDL_Thread *s_autocap_write_thread;
static AutocapWriteJob *s_autocap_write_job;

typedef struct PreserveWriteJob {
    AutocapWriteJob snapshot;
    struct PreserveWriteJob *next;
    unsigned attempts;
} PreserveWriteJob;
static SDL_mutex *s_preserve_mutex;
static SDL_cond *s_preserve_cond;
static SDL_Thread *s_preserve_thread;
static PreserveWriteJob *s_preserve_head;
static PreserveWriteJob *s_preserve_tail;
static int s_preserve_stop;

void overlay_autocapture_set_enabled(int on) {
    s_autocap_enabled = on ? 1 : 0;
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

#ifdef PSX_OVERLAY_CAPTURE_TEST
int overlay_capture_test_write_state(void) {
    return SDL_AtomicGet(&s_autocap_write_state);
}
unsigned overlay_capture_test_write_attempts(void) {
    return s_autocap_write_job ? s_autocap_write_job->attempts : 0u;
}
int overlay_capture_test_provider_pending(void) {
    return s_autocap_provider_sig_pending != 0;
}
#endif

/* FNV-1a of the just-written capture manifest. The manifest is a pure
 * function of the capture inputs (live overlay bytes + observed seed-PC
 * sets, both grow-only), so an unchanged hash means a compile request
 * would redo exactly the work of the previous one. */
static void autocap_write_job_free(AutocapWriteJob *job)
{
    if (!job) return;
    free(job->ram); free(job->dispatch_pc_bitmap); free(job->exec_pc_bitmap);
    free(job->bitmap); free(job);
}

static int autocap_write_thread_main(void *opaque)
{
    AutocapWriteJob *job = (AutocapWriteJob *)opaque;
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
    job->manifest_sig = write_and_commit_snapshot(
        job->bitmap_words, job->bitmap, job->dispatch_pc_bitmap,
        job->exec_pc_bitmap, job->ram, "autocap", job->sequence);
    SDL_AtomicSet(&s_autocap_write_state, 2);
    return 0;
}

static uint64_t autocap_write_retry_delay(unsigned attempts)
{
    uint64_t frames = (uint64_t)(attempts ? attempts : 1u) * 6u;
    return frames < AUTOCAP_WRITE_RETRY_MAX_FRAMES
         ? frames : AUTOCAP_WRITE_RETRY_MAX_FRAMES;
}

/* A durable contribution and a successfully started compiler request are two
 * separate commits. Retain the former's signature until request() accepts it;
 * otherwise a transient CreateProcess/pipe/thread failure would mark identical
 * evidence futile and suppress compilation forever. Returns nonzero while a
 * provider request is still pending. */
static int autocap_provider_request_try(const CodeProvider *cp, uint64_t frame)
{
    if (!s_autocap_provider_sig_pending) return 0;
    if (frame < s_autocap_provider_retry_frame) return 1;
    /* busy remains true through AC_DONE until the emulation thread has
     * committed every prepared DLL and completed the additive final rescan. */
    if (cp->busy && cp->busy()) return 1;
    int reg = overlay_loader_registered_count();
    if (s_autocap_provider_sig_pending == s_autocap_sig_at_req &&
        reg == s_autocap_reg_at_req) {
        s_autocap_futile++;
        if (s_autocap_backoff < AUTOCAP_BACKOFF_MAX)
            s_autocap_backoff <<= 1;
        s_autocap_next_ok = frame +
            (uint64_t)AUTOCAP_COOLDOWN_FRAMES * s_autocap_backoff;
        s_autocap_provider_sig_pending = 0;
        s_autocap_provider_retry_frame = 0;
        s_autocap_provider_attempts = 0;
        return 0;
    }
    if (cp->request && cp->request()) {
        s_autocap_backoff = 1;
        s_autocap_sig_at_req = s_autocap_provider_sig_pending;
        s_autocap_reg_at_req = reg;
        s_autocap_triggers++;
        s_autocap_provider_sig_pending = 0;
        s_autocap_provider_retry_frame = 0;
        s_autocap_provider_attempts = 0;
        return 0;
    }
    s_autocap_provider_attempts++;
    s_autocap_provider_retry_frame = frame +
        autocap_write_retry_delay(s_autocap_provider_attempts);
    return 1;
}

static int autocap_write_launch(AutocapWriteJob *job)
{
    job->manifest_sig = 0;
    s_autocap_write_job = job;
    SDL_AtomicSet(&s_autocap_write_state, 1);
    s_autocap_write_thread = SDL_CreateThread(autocap_write_thread_main,
                                               "overlay-capture-write", job);
    if (!s_autocap_write_thread) {
        SDL_AtomicSet(&s_autocap_write_state, 0);
        return 0;
    }
    return 1;
}

/* Copy coherent inputs on the emulation thread, then perform base64 formatting,
 * file I/O, and verification reread on a worker. */
static AutocapWriteJob *capture_snapshot_create(uint32_t scope_lo,
                                                uint32_t scope_hi,
                                                int scoped)
{
    extern uint8_t *memory_get_ram_ptr(void);
    const size_t ram_size = 2u * 1024u * 1024u;
    uint32_t bw = dirty_ram_get_bitmap_word_count();
    AutocapWriteJob *job = (AutocapWriteJob *)calloc(1, sizeof(*job));
    if (!job) return NULL;
    job->ram = (uint8_t *)malloc(ram_size);
    job->dispatch_pc_bitmap = (uint32_t *)malloc(
        sizeof(g_dirty_ram_dispatch_pc_bitmap));
    job->exec_pc_bitmap = (uint32_t *)malloc(sizeof(g_dirty_ram_exec_pc_bitmap));
    job->bitmap = (uint32_t *)malloc((size_t)bw * sizeof(uint32_t));
    if (!job->ram || !job->dispatch_pc_bitmap ||
        !job->exec_pc_bitmap || !job->bitmap) {
        autocap_write_job_free(job); return NULL;
    }
    memcpy(job->ram, memory_get_ram_ptr(), ram_size);
    memcpy(job->dispatch_pc_bitmap, g_dirty_ram_dispatch_pc_bitmap,
           sizeof(g_dirty_ram_dispatch_pc_bitmap));
    memcpy(job->exec_pc_bitmap, g_dirty_ram_exec_pc_bitmap,
           sizeof(g_dirty_ram_exec_pc_bitmap));
    if (scoped) {
        memset(job->bitmap, 0, (size_t)bw * sizeof(uint32_t));
        uint32_t first_page = scope_lo >> 12;
        uint32_t last_page = (scope_hi - 1u) >> 12;
        for (uint32_t page = first_page; page <= last_page; page++) {
            uint32_t first_exec_word = page * (4096u / 4u);
            int observed = 0;
            for (uint32_t b = 0; b < 4096u / 4u / 32u; b++) {
                if (job->exec_pc_bitmap[(first_exec_word >> 5) + b]) {
                    observed = 1;
                    break;
                }
            }
            if (observed)
                job->bitmap[page >> 5] |= 1u << (page & 31u);
        }
        /* Preserve a one-page EXECUTED halo on both sides of a scoped DMA
         * overwrite.  The overwritten page can contain either the ...FFC
         * control transfer or the ...000 delay slot; the paired instruction
         * may live in the neighboring page and must come from this same
         * coherent pre-write RAM snapshot.  Only executed neighbors qualify,
         * so a sector overwrite cannot pull unrelated dirty history into an
         * unbounded capture.  Evidence clearing remains limited to the pages
         * actually overwritten by DMA. */
        uint32_t halo_lo = first_page > 0u ? first_page - 1u : first_page;
        uint32_t ram_pages = (uint32_t)(ram_size >> 12);
        uint32_t halo_hi = last_page + 1u < ram_pages
                         ? last_page + 1u : last_page;
        for (uint32_t page = halo_lo; page <= halo_hi; page++) {
            if (page >= first_page && page <= last_page) continue;
            uint32_t first_exec_word = page * (4096u / 4u);
            int observed = 0;
            for (uint32_t b = 0; b < 4096u / 4u / 32u; b++) {
                if (job->exec_pc_bitmap[(first_exec_word >> 5) + b]) {
                    observed = 1;
                    break;
                }
            }
            if (observed)
                job->bitmap[page >> 5] |= 1u << (page & 31u);
        }
    } else {
        for (uint32_t i = 0; i < bw; i++)
            job->bitmap[i] = dirty_ram_get_bitmap_word(i);
    }
    job->bitmap_words = bw;
    job->sequence = capture_next_sequence();
    return job;
}

static int autocap_write_start(void)
{
    /* Periodic convergence needs executed code, not every sticky dirty page in
     * RAM. Evidence-scoping keeps the background manifest proportional to live
     * coverage and avoids multi-megabyte rewrites every cooldown. Shutdown still
     * emits the legacy full current snapshot once. */
    AutocapWriteJob *job = capture_snapshot_create(
        0, 2u * 1024u * 1024u, 1);
    if (!job) return 0;
    if (!autocap_write_launch(job)) {
        s_autocap_write_job = NULL;
        autocap_write_job_free(job);
        return 0;
    }
    return 1;
}

static int preserve_write_thread_main(void *opaque)
{
    (void)opaque;
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
    for (;;) {
        SDL_LockMutex(s_preserve_mutex);
        while (!s_preserve_head && !s_preserve_stop)
            SDL_CondWait(s_preserve_cond, s_preserve_mutex);
        if (!s_preserve_head && s_preserve_stop) {
            SDL_UnlockMutex(s_preserve_mutex);
            break;
        }
        PreserveWriteJob *job = s_preserve_head;
        s_preserve_head = job->next;
        if (!s_preserve_head) s_preserve_tail = NULL;
        SDL_UnlockMutex(s_preserve_mutex);

        job->snapshot.manifest_sig = write_and_commit_snapshot(
            job->snapshot.bitmap_words, job->snapshot.bitmap,
            job->snapshot.dispatch_pc_bitmap, job->snapshot.exec_pc_bitmap,
            job->snapshot.ram, "preserve-outgoing", job->snapshot.sequence);
        if (!job->snapshot.manifest_sig) {
            job->attempts++;
            SDL_LockMutex(s_preserve_mutex);
            int abandoning = s_preserve_stop && job->attempts >= 5u;
            if (!abandoning) {
                job->next = NULL;
                if (s_preserve_tail) s_preserve_tail->next = job;
                else s_preserve_head = job;
                s_preserve_tail = job;
            }
            SDL_UnlockMutex(s_preserve_mutex);
            if (abandoning) {
                fprintf(stderr,
                    "psxrecomp: ERROR: outgoing overlay snapshot could not be retained after shutdown retries\n");
                autocap_write_job_free(&job->snapshot);
            } else {
                SDL_Delay(job->attempts < 4u ? job->attempts * 100u : 1000u);
            }
            continue;
        }
        autocap_write_job_free(&job->snapshot);
    }
    return 0;
}

static int preserve_writer_start(void)
{
    if (!s_preserve_mutex) s_preserve_mutex = SDL_CreateMutex();
    if (!s_preserve_cond) s_preserve_cond = SDL_CreateCond();
    if (!s_preserve_mutex || !s_preserve_cond) return 0;
    SDL_LockMutex(s_preserve_mutex);
    if (!s_preserve_thread) {
        s_preserve_stop = 0;
        s_preserve_thread = SDL_CreateThread(preserve_write_thread_main,
                                             "overlay-preserve-write", NULL);
    }
    int ok = s_preserve_thread != NULL;
    SDL_UnlockMutex(s_preserve_mutex);
    return ok;
}

/* Transfer ownership of an already-coherent immutable snapshot to the
 * retrying FIFO. This is also the shutdown path for a periodic job whose
 * formatter/fsync/history commit failed: the live evidence epoch has moved on,
 * so the old bytes must be retried as-is rather than reconstructed or dropped. */
static int preserve_snapshot_enqueue_owned(AutocapWriteJob *snapshot)
{
    PreserveWriteJob *job;
    if (!snapshot || !preserve_writer_start()) return 0;
    job = (PreserveWriteJob *)calloc(1, sizeof(*job));
    if (!job) return 0;
    job->snapshot = *snapshot;
    free(snapshot);

    SDL_LockMutex(s_preserve_mutex);
    if (s_preserve_tail) s_preserve_tail->next = job;
    else s_preserve_head = job;
    s_preserve_tail = job;
    SDL_CondSignal(s_preserve_cond);
    SDL_UnlockMutex(s_preserve_mutex);
    return 1;
}

/* Capture coherent bytes before DMA mutates RAM, but keep base64, hashing,
 * copying, and fsync off the emulation thread. The one FIFO writer bounds I/O
 * concurrency; sequence-aware commits also order it against autocapture. */
static int preserve_snapshot_async(uint32_t scope_lo, uint32_t scope_hi)
{
    if (!preserve_writer_start()) return 0;
    AutocapWriteJob *snapshot = capture_snapshot_create(scope_lo, scope_hi, 1);
    if (!snapshot) return 0;
    if (preserve_snapshot_enqueue_owned(snapshot)) return 1;
    autocap_write_job_free(snapshot);
    return 0;
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
        SDL_AtomicSet(&s_autocap_write_state, 0);
        uint64_t sig = job ? job->manifest_sig : 0;
        if (!sig && job) {
            /* The queued snapshot owns the cleared evidence epoch. Keep that
             * immutable job alive and retry it; never merge it back into or
             * clear the newer live epoch that has accumulated meanwhile. */
            job->attempts++;
            job->retry_frame = s_frame_count +
                autocap_write_retry_delay(job->attempts);
            return;
        }
        s_autocap_write_job = NULL;
        if (sig) {
            s_autocap_provider_sig_pending = sig;
            s_autocap_provider_retry_frame = s_frame_count;
            s_autocap_provider_attempts = 0;
            (void)autocap_provider_request_try(cp, s_frame_count);
        }
        autocap_write_job_free(job);
        return;
    }

    /* A transient formatter/fsync/history-commit or thread-create failure must
     * not discard an already-cleared evidence epoch. Retry the same immutable
     * snapshot at bounded cadence before considering another periodic fire. */
    if (s_autocap_write_job) {
        AutocapWriteJob *job = s_autocap_write_job;
        if (SDL_AtomicGet(&s_autocap_write_state) == 0 &&
            s_frame_count >= job->retry_frame &&
            !autocap_write_launch(job)) {
            job->attempts++;
            job->retry_frame = s_frame_count +
                autocap_write_retry_delay(job->attempts);
        }
        return;
    }

    if (s_autocap_provider_sig_pending) {
        (void)autocap_provider_request_try(cp, s_frame_count);
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
    if (autocap_write_start()) {
        /* The queued job now owns this coherent evidence epoch. Start the next
         * one immediately so later periodic manifests contain only newly seen
         * PCs/pages instead of rewriting the entire session-wide union. Failed
         * jobs remain queued and retry; normal shutdown drains them. */
        memset(g_dirty_ram_exec_pc_bitmap, 0,
               sizeof(g_dirty_ram_exec_pc_bitmap));
        memset(g_dirty_ram_dispatch_pc_bitmap, 0,
               sizeof(g_dirty_ram_dispatch_pc_bitmap));
        memset(g_dirty_ram_exec_page_bitmap, 0,
               sizeof(g_dirty_ram_exec_page_bitmap));
    }
}

void overlay_capture_wait_pending(void)
{
    if (s_autocap_write_job) {
        if (s_autocap_write_thread)
            SDL_WaitThread(s_autocap_write_thread, NULL);
        s_autocap_write_thread = NULL;
        AutocapWriteJob *job = s_autocap_write_job;
        s_autocap_write_job = NULL;
        SDL_AtomicSet(&s_autocap_write_state, 0);
        if (!job->manifest_sig && preserve_snapshot_enqueue_owned(job)) {
            /* Owned by the retrying FIFO, which is drained just below. */
        } else {
            if (!job->manifest_sig) {
                fprintf(stderr,
                    "psxrecomp: ERROR: periodic overlay snapshot could not be queued for shutdown retry\n");
            }
            autocap_write_job_free(job);
        }
    }
    if (s_preserve_thread) {
        SDL_LockMutex(s_preserve_mutex);
        s_preserve_stop = 1;
        SDL_CondSignal(s_preserve_cond);
        SDL_UnlockMutex(s_preserve_mutex);
        SDL_WaitThread(s_preserve_thread, NULL);
        s_preserve_thread = NULL;
    }
}

void overlay_autocapture_shutdown(void)
{
    int state = SDL_AtomicGet(&s_autocap_write_state);
    if (state == 0) return;
    if (s_autocap_write_thread) {
        SDL_WaitThread(s_autocap_write_thread, NULL);
        s_autocap_write_thread = NULL;
    }
    /* The worker atomically published and durably archived the coherent
     * snapshot before setting state=2. No compiler request is needed while
     * shutting down; just release its copied inputs before the final snapshot. */
    autocap_write_job_free(s_autocap_write_job);
    s_autocap_write_job = NULL;
    SDL_AtomicSet(&s_autocap_write_state, 0);
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
