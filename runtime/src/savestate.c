/* savestate.c — user save states. The runtime UI opens from the save-state menu.
 * See savestate.h.
 *
 * Wraps boot_state.c's full-machine serializer. Requests are staged by the SDL
 * key handler / debug server and executed by savestate_poll at a block-leader
 * boundary (in_exception == 0), where cpu->pc is a valid resume PC. A load
 * restores the full machine then unwinds to the scheduler and re-dispatches. */

#include "savestate.h"
#include "boot_state.h"
#include "cdrom.h"
#include "gpu.h"
#include "interrupts.h"
#include "psx_cycles.h"
#include "psx_netplay.h"
#include "psx_netplay_rb.h"
#include "psx_scheduler.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#endif

static double savestate_mono_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
#endif
}

static char     s_dir[512];
static char     s_root[512];          /* memcard/save root (pre-token) */
static char     s_bios_token[32];     /* "openbios" / "scph1001", or empty */
static uint32_t s_openbios_wordsum;   /* bundled OpenBIOS wordsum for migrate */
static uint32_t s_bios_checksum;
static uint32_t s_entry_pc;
static int      s_configured   = 0;
static int      s_save_pending = -1;   /* slot, or -1 */
static int      s_load_pending = -1;
static int      s_load_completed = 0;
static int      s_load_failed = 0;
static int      s_save_failed = 0;
static uint32_t s_last_save_pc = 0;
static int      s_save_defer_slot = -1;
static double   s_save_defer_t0 = 0.0;
static uint8_t *s_load_blob = NULL;   /* optional in-memory .pst for netplay */
static size_t   s_load_blob_len = 0;

typedef struct SavestateThumbHeader {
    char magic[4];
    uint32_t w;
    uint32_t h;
} SavestateThumbHeader;

/* Mid-FMV / present-edge IRQ fast paths often pass resume_pc=0 (cpu->pc is
 * parked). Prefer sticky BB / compiled latches over writing a null resume. */
static uint32_t savestate_resolve_resume_pc(const CPUState* cpu, uint32_t hint)
{
    const uint32_t cands[6] = {
        hint,
        cpu ? cpu->pc : 0u,
        psx_compiled_irq_resume_pc(),
        psx_last_irq_check_pc(),
        psx_netplay_rb_sticky_bb_pc(),
        cpu ? cpu->gpr[31] : 0u,
    };
    int i;
    for (i = 0; i < 6; ++i) {
        uint32_t pc = cands[i];
        if (!pc || (pc & 3u) != 0u)
            continue;
        if (pc == 0x80000080u || pc == 0xbfc00180u || pc == 0x80000000u)
            continue;
        if (psx_is_dispatchable(pc))
            return pc;
    }
    return 0;
}

static int savestate_resume_pc_ok(uint32_t pc)
{
    return pc != 0u && (pc & 3u) == 0u && psx_is_dispatchable(pc) &&
           pc != 0x80000080u && pc != 0xbfc00180u && pc != 0x80000000u;
}

extern int psx_hle_scheduler_enabled(void);

/* Create each path component (mkdir -p). Single-level mkdir fails for
 * "saves/netplay" when parent "saves" is missing. */
static void ensure_dir(const char* dir) {
    char tmp[512];
    size_t len;
    size_t i;
    if (!dir || !dir[0]) return;
    strncpy(tmp, dir, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    len = strlen(tmp);
    while (len > 1 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\')) {
        tmp[--len] = '\0';
    }
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
#ifdef _WIN32
            /* Keep drive prefix "C:" intact — do not mkdir("C:"). */
            if (i == 2 && tmp[1] == ':')
                continue;
#endif
            tmp[i] = '\0';
#ifdef _WIN32
            (void)_mkdir(tmp);
#else
            (void)mkdir(tmp, 0755);
#endif
            tmp[i] = '/';
        }
    }
#ifdef _WIN32
    (void)_mkdir(tmp);
#else
    (void)mkdir(tmp, 0755);
#endif
}

static void clear_load_blob(void) {
    free(s_load_blob);
    s_load_blob = NULL;
    s_load_blob_len = 0;
}

/* Peek .pst header bios_checksum (BOOT_STATE wire: magic, version, checksum). */
static int pst_peek_bios_checksum(const char* path, uint32_t* out_cksum) {
    FILE* f;
    uint8_t hdr[12];
    uint32_t magic, version, cksum;
    if (!path || !out_cksum) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    magic   = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16) |
              ((uint32_t)hdr[3] << 24);
    version = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) | ((uint32_t)hdr[6] << 16) |
              ((uint32_t)hdr[7] << 24);
    cksum   = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) | ((uint32_t)hdr[10] << 16) |
              ((uint32_t)hdr[11] << 24);
    if (magic != BOOT_STATE_MAGIC) return 0;
    if (version < BOOT_STATE_VERSION_MIN_READ || version > BOOT_STATE_VERSION)
        return 0;
    *out_cksum = cksum;
    return 1;
}

static int is_legacy_pst_name(const char* name) {
    const char* p;
    int digits;
    if (!name || strncmp(name, "state_", 6) != 0) return 0;
    p = name + 6;
    digits = 0;
    while (isxdigit((unsigned char)*p)) {
        digits++;
        p++;
    }
    if (digits < 1 || digits > 8) return 0;
    if (strncmp(p, "_slot", 5) != 0) return 0;
    p += 5;
    if (!isdigit((unsigned char)p[0]) || !isdigit((unsigned char)p[1])) return 0;
    if (strcmp(p + 2, ".pst") != 0) return 0;
    return 1;
}

static int path_exists(const char* path) {
#ifdef _WIN32
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

/* One-shot: move loose <root>/state_*_slot*.pst into openbios/ or scph1001/
 * by header bios_checksum. Unknown/unreadable → scph1001. */
static void migrate_legacy_pst_by_bios(const char* root, uint32_t openbios_wordsum) {
    char marker[560];
    char src[600];
    char dest_dir[560];
    char dest[620];
    int moved = 0;
    int skipped = 0;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pattern[560];
#else
    DIR* d;
    struct dirent* ent;
#endif

    if (!root || !root[0]) return;
    snprintf(marker, sizeof(marker), "%s/.pst_bios_isolated", root);
    if (path_exists(marker)) return;

    ensure_dir(root);

#ifdef _WIN32
    snprintf(pattern, sizeof(pattern), "%s/state_*.pst", root);
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const char* name = fd.cFileName;
            uint32_t cksum = 0;
            const char* token;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (!is_legacy_pst_name(name)) continue;
            snprintf(src, sizeof(src), "%s/%s", root, name);
            if (!pst_peek_bios_checksum(src, &cksum))
                token = "scph1001";
            else if (openbios_wordsum != 0 && cksum == openbios_wordsum)
                token = "openbios";
            else
                token = "scph1001";
            snprintf(dest_dir, sizeof(dest_dir), "%s/%s", root, token);
            ensure_dir(dest_dir);
            snprintf(dest, sizeof(dest), "%s/%s", dest_dir, name);
            if (path_exists(dest)) {
                skipped++;
                continue;
            }
            if (MoveFileA(src, dest))
                moved++;
            else
                skipped++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    d = opendir(root);
    if (d) {
        while ((ent = readdir(d)) != NULL) {
            const char* name = ent->d_name;
            uint32_t cksum = 0;
            const char* token;
            if (!is_legacy_pst_name(name)) continue;
            {
                struct stat st;
                snprintf(src, sizeof(src), "%s/%s", root, name);
                if (stat(src, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            }
            if (!pst_peek_bios_checksum(src, &cksum))
                token = "scph1001";
            else if (openbios_wordsum != 0 && cksum == openbios_wordsum)
                token = "openbios";
            else
                token = "scph1001";
            snprintf(dest_dir, sizeof(dest_dir), "%s/%s", root, token);
            ensure_dir(dest_dir);
            snprintf(dest, sizeof(dest), "%s/%s", dest_dir, name);
            if (path_exists(dest)) {
                skipped++;
                continue;
            }
            if (rename(src, dest) == 0)
                moved++;
            else
                skipped++;
        }
        closedir(d);
    }
#endif

    {
        FILE* mf = fopen(marker, "wb");
        if (mf) {
            fprintf(mf,
                    "psxrecomp-pst-bios-isolated-v1\n"
                    "moved=%d\n"
                    "skipped=%d\n",
                    moved, skipped);
            fclose(mf);
        }
    }
    if (moved > 0 || skipped > 0) {
        printf("psxrecomp: savestate BIOS isolate — migrated %d legacy .pst "
               "(skipped %d) under %s\n",
               moved, skipped, root);
        fflush(stdout);
    }
}

void savestate_configure(const char* dir, uint32_t bios_checksum, uint32_t entry_pc,
                         const char* bios_token, uint32_t openbios_wordsum) {
    s_bios_checksum = bios_checksum;
    s_entry_pc      = entry_pc;
    s_configured    = 1;

    if (!dir || !dir[0]) {
        s_dir[0] = '\0';
        return;
    }

    if (bios_token && bios_token[0]) {
        size_t n;
        strncpy(s_root, dir, sizeof(s_root) - 1);
        s_root[sizeof(s_root) - 1] = '\0';
        n = strlen(s_root);
        while (n > 1 && (s_root[n - 1] == '/' || s_root[n - 1] == '\\'))
            s_root[--n] = '\0';
        strncpy(s_bios_token, bios_token, sizeof(s_bios_token) - 1);
        s_bios_token[sizeof(s_bios_token) - 1] = '\0';
        s_openbios_wordsum = openbios_wordsum;
        migrate_legacy_pst_by_bios(s_root, openbios_wordsum);
        if (snprintf(s_dir, sizeof(s_dir), "%s/%s", s_root, bios_token) >=
            (int)sizeof(s_dir)) {
            strncpy(s_dir, s_root, sizeof(s_dir) - 1);
            s_dir[sizeof(s_dir) - 1] = '\0';
        }
        ensure_dir(s_dir);
    } else {
        /* Netplay guest sandbox / already-scoped path: do not clear the
         * personal root/token remembered from the last bios-scoped configure. */
        strncpy(s_dir, dir, sizeof(s_dir) - 1);
        s_dir[sizeof(s_dir) - 1] = '\0';
        ensure_dir(s_dir);
    }
}

const char* savestate_dir(void) {
    return s_dir;
}

const char* savestate_root_dir(void) {
    return s_root;
}

const char* savestate_bios_token(void) {
    return s_bios_token;
}

uint32_t savestate_openbios_wordsum(void) {
    return s_openbios_wordsum;
}

void savestate_get_integrity(uint32_t* bios_checksum, uint32_t* entry_pc) {
    if (bios_checksum) *bios_checksum = s_bios_checksum;
    if (entry_pc) *entry_pc = s_entry_pc;
}

int savestate_slot_path(int slot, char* out, size_t cap) {
    if (!s_configured || !out || cap == 0) return 0;
    if (slot < 0 || slot >= SAVESTATE_SLOTS) return 0;
    /* Keyed by entry_pc so slots from different games in a shared dir never
     * collide; boot_state_load also rejects a mismatched entry_pc internally. */
    snprintf(out, cap, "%s%sstate_%08X_slot%02d.pst",
             s_dir, (s_dir[0] ? "/" : ""), (unsigned)s_entry_pc, slot);
    return 1;
}

static int savestate_thumb_path(int slot, char* out, size_t cap) {
    if (!s_configured || !out || cap == 0) return 0;
    if (slot < 0 || slot >= SAVESTATE_SLOTS) return 0;
    snprintf(out, cap, "%s%sstate_%08X_slot%02d.thumb",
             s_dir, (s_dir[0] ? "/" : ""), (unsigned)s_entry_pc, slot);
    return 1;
}

int savestate_slot_exists(int slot) {
    char path[600];
    FILE* f;
    long sz;
    if (!savestate_slot_path(slot, path, sizeof(path))) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    sz = ftell(f);
    fclose(f);
    return sz > 0;
}

int savestate_slot_mtime(int slot, int64_t* out_time) {
    char path[600];
    if (out_time) *out_time = 0;
    if (!savestate_slot_path(slot, path, sizeof(path))) return 0;
#ifdef _WIN32
    {
        struct _stat64 st;
        if (_stat64(path, &st) != 0 || st.st_size <= 0) return 0;
        if (out_time) *out_time = (int64_t)st.st_mtime;
        return 1;
    }
#else
    {
        struct stat st;
        if (stat(path, &st) != 0 || st.st_size <= 0) return 0;
        if (out_time) *out_time = (int64_t)st.st_mtime;
        return 1;
    }
#endif
}

int savestate_capture_thumb(int slot) {
    char path[600];
    FILE* f;
    SavestateThumbHeader hdr;
    uint32_t thumb[SAVESTATE_THUMB_W * SAVESTATE_THUMB_H];
    GpuDisplayInfo di;
    uint32_t dw, dh, x, y;
    if (!savestate_thumb_path(slot, path, sizeof(path))) return 0;
    gpu_get_display_info(&di);
    dw = di.width ? di.width : 320u;
    dh = di.height ? di.height : 240u;
    for (y = 0; y < SAVESTATE_THUMB_H; y++) {
        uint32_t sy = y * dh / SAVESTATE_THUMB_H;
        for (x = 0; x < SAVESTATE_THUMB_W; x++) {
            uint32_t sx = x * dw / SAVESTATE_THUMB_W;
            thumb[y * SAVESTATE_THUMB_W + x] = di.disabled
                ? 0xFF000000u
                : (gpu_display_pixel_argb(&di, sx, sy) | 0xFF000000u);
        }
    }
    hdr.magic[0] = 'P';
    hdr.magic[1] = 'S';
    hdr.magic[2] = 'T';
    hdr.magic[3] = 'H';
    hdr.w = SAVESTATE_THUMB_W;
    hdr.h = SAVESTATE_THUMB_H;
    f = fopen(path, "wb");
    if (!f) return 0;
    if (fwrite(&hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        fwrite(thumb, sizeof(uint32_t),
               SAVESTATE_THUMB_W * SAVESTATE_THUMB_H, f) !=
            SAVESTATE_THUMB_W * SAVESTATE_THUMB_H) {
        fclose(f);
        remove(path);
        return 0;
    }
    return fclose(f) == 0;
}

int savestate_read_thumb(int slot, uint32_t* out_argb, int out_w, int out_h) {
    char path[600];
    FILE* f;
    SavestateThumbHeader hdr;
    size_t pixels;
    if (!out_argb || out_w != SAVESTATE_THUMB_W ||
        out_h != SAVESTATE_THUMB_H)
        return 0;
    if (!savestate_thumb_path(slot, path, sizeof(path))) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr.magic, "PSTH", 4) != 0 ||
        hdr.w != SAVESTATE_THUMB_W || hdr.h != SAVESTATE_THUMB_H) {
        fclose(f);
        return 0;
    }
    pixels = (size_t)SAVESTATE_THUMB_W * (size_t)SAVESTATE_THUMB_H;
    if (fread(out_argb, sizeof(uint32_t), pixels, f) != pixels) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}

int savestate_slot_compatible(int slot, char* reason, size_t reason_cap) {
    uint8_t* data = NULL;
    size_t size = 0;
    int ok;
    if (reason && reason_cap)
        reason[0] = '\0';
    if (!s_configured) {
        if (reason && reason_cap)
            snprintf(reason, reason_cap, "not_configured");
        return 0;
    }
    if (!savestate_read_slot(slot, &data, &size) || !data) {
        if (reason && reason_cap)
            snprintf(reason, reason_cap, "missing");
        return 0;
    }
    ok = boot_state_check_buffer(data, size, s_bios_checksum, s_entry_pc,
                                 reason, reason_cap);
    free(data);
    return ok;
}

int savestate_read_slot(int slot, uint8_t** data_out, size_t* size_out) {
    char path[600];
    FILE* f;
    long sz;
    uint8_t* buf;
    if (!data_out || !size_out) return 0;
    *data_out = NULL;
    *size_out = 0;
    if (!savestate_slot_path(slot, path, sizeof(path))) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    sz = ftell(f);
    if (sz <= 0 || (size_t)sz > 8u * 1024u * 1024u) { fclose(f); return 0; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    *data_out = buf;
    *size_out = (size_t)sz;
    return 1;
}

int savestate_write_slot(int slot, const void* data, size_t size) {
    char path[600];
    FILE* f;
    size_t wrote;
    if (!data || size == 0) return 0;
    if (!savestate_slot_path(slot, path, sizeof(path))) {
        fprintf(stderr,
                "savestate: write_slot=%d failed (not configured / bad slot) "
                "dir='%s' configured=%d\n",
                slot, s_dir, s_configured);
        return 0;
    }
    ensure_dir(s_dir);
    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "savestate: write_slot fopen('%s') failed: %s\n",
                path, strerror(errno));
        return 0;
    }
    wrote = fwrite(data, 1, size, f);
    if (wrote != size) {
        fprintf(stderr,
                "savestate: write_slot fwrite('%s') %zu/%zu failed: %s\n",
                path, wrote, size, strerror(errno));
        fclose(f);
        remove(path);
        return 0;
    }
    if (fflush(f) != 0 || fclose(f) != 0) {
        fprintf(stderr, "savestate: write_slot flush/close('%s') failed: %s\n",
                path, strerror(errno));
        remove(path);
        return 0;
    }
    return 1;
}

/* User APIs during netplay: guests cannot initiate; host must use
 * psx_netplay_request_* so peers hash-probe and sync over STATE_*. */
static int netplay_user_blocked(void) {
    extern int psx_link_pair_follower_mode(void);
    if (psx_link_pair_follower_mode()) {
        fprintf(stderr, "savestate: blocked — PSX-Link follower state is "
                        "driven by its netplay client\n");
        return 1;
    }
    if (!psx_netplay_active()) return 0;
    if (!psx_netplay_is_host()) {
        fprintf(stderr, "savestate: netplay guest cannot save/load (host-only)\n");
        return 1;
    }
    fprintf(stderr,
            "savestate: during netplay use host Shift+F / F (synced path)\n");
    return 1;
}

static int request_save_inner(int slot) {
    if (!s_configured) { fprintf(stderr, "savestate: not configured\n"); return 0; }
    if (slot < 0 || slot >= SAVESTATE_SLOTS) return 0;
    s_save_failed = 0;
    s_last_save_pc = 0; /* block netplay transfer until this write stamps a PC */
    s_save_defer_slot = -1;
    s_save_pending = slot;
    return 1;
}

static int request_load_inner(int slot) {
    if (!s_configured) { fprintf(stderr, "savestate: not configured\n"); return 0; }
    if (slot < 0 || slot >= SAVESTATE_SLOTS) return 0;
    if (!psx_hle_scheduler_enabled()) {
        /* LLE (host-fiber) mode: the restore longjmp target lives on the
         * scheduler fiber; cross-fiber unwind is unsafe. HLE is the default. */
        fprintf(stderr, "savestate: load requires the HLE scheduler (default); "
                        "PSX_HLE_SCHEDULER=0 run cannot load states.\n");
        return 0;
    }
    s_load_failed = 0;
    s_load_completed = 0;
    s_load_pending = slot;
    return 1;
}

int savestate_request_save(int slot) {
    if (netplay_user_blocked()) return 0;
    return request_save_inner(slot);
}

int savestate_request_load(int slot) {
    if (netplay_user_blocked()) return 0;
    return request_load_inner(slot);
}

int savestate_request_save_protocol(int slot) {
    /* Follow-host sync: guests must write the host-authoritative .pst. */
    return request_save_inner(slot);
}

int savestate_request_load_protocol(int slot) {
    /* Follow-host sync: guests must apply the host-authoritative .pst. */
    clear_load_blob();
    return request_load_inner(slot);
}

int savestate_request_load_blob_protocol(const void* data, size_t size) {
    uint8_t* copy;
    if (!s_configured) {
        fprintf(stderr, "savestate: load_blob — not configured\n");
        return 0;
    }
    if (!data || size == 0 || size > 64u * 1024u * 1024u)
        return 0;
    if (!psx_hle_scheduler_enabled()) {
        fprintf(stderr, "savestate: load_blob requires the HLE scheduler\n");
        return 0;
    }
    copy = (uint8_t*)malloc(size);
    if (!copy) {
        fprintf(stderr, "savestate: load_blob malloc(%zu) failed\n", size);
        return 0;
    }
    memcpy(copy, data, size);
    clear_load_blob();
    s_load_blob = copy;
    s_load_blob_len = size;
    s_load_failed = 0;
    s_load_completed = 0;
    s_load_pending = 0; /* non-negative: poll will prefer the blob */
    return 1;
}

int savestate_pending(void) {
    return (s_save_pending >= 0 || s_load_pending >= 0) ? 1 : 0;
}

int savestate_take_load_completed(void) {
    int v = s_load_completed;
    s_load_completed = 0;
    return v;
}

int savestate_take_load_failed(void) {
    int v = s_load_failed;
    s_load_failed = 0;
    return v;
}

int savestate_take_save_failed(void) {
    int v = s_save_failed;
    s_save_failed = 0;
    return v;
}

uint32_t savestate_last_save_pc(void) {
    return s_last_save_pc;
}

void savestate_poll(CPUState* cpu, uint32_t resume_pc) {
    if (s_save_pending < 0 && s_load_pending < 0) return;   /* hot path: nothing staged */

    if (s_save_pending >= 0) {
        int slot = s_save_pending;
        char path[600];
        uint32_t pc = savestate_resolve_resume_pc(cpu, resume_pc);
        /* hint==0 ALSO defers, even when the resolver found a plausible-looking
         * substitute. The substitute chain ends in sticky-BB latches and $ra,
         * which pass the sanity check while being the wrong place to resume:
         * the first F7 of a session saved such a state, it loaded, ran for
         * ~150M instructions off the rails, and died at PC=0 -- poison that
         * looks valid at save time and only fails minutes later. Deferring
         * reuses the existing retry: the save completes at the next poll where
         * a real block-leader PC is published, or fails LOUDLY after the
         * timeout instead of writing a corrupt state silently. */
        if (resume_pc == 0u || !savestate_resume_pc_ok(pc)) {
            /* FMV/present edges often poll with hint=0; wait briefly for a
             * sticky BB / IRQ latch rather than writing pc=0 poison. */
            const double now = savestate_mono_ms();
            if (s_save_defer_slot != slot) {
                s_save_defer_slot = slot;
                s_save_defer_t0 = now;
                fprintf(stderr,
                        "savestate: deferring slot %d — no safe resume PC "
                        "(hint=0x%08X)\n",
                        slot, (unsigned)resume_pc);
            }
            if (now - s_save_defer_t0 < 2000.0)
                return;
            s_save_pending = -1;
            s_save_defer_slot = -1;
            s_last_save_pc = 0;
            s_save_failed = 1;
            fprintf(stderr,
                    "savestate: SAVE FAILED slot %d — no safe resume PC "
                    "(hint=0x%08X)\n",
                    slot, (unsigned)resume_pc);
            psx_frontend_on_savestate_notify(0, slot, 0);
        } else {
            s_save_pending = -1;
            s_save_defer_slot = -1;
            if (pc != resume_pc && resume_pc == 0u) {
                fprintf(stderr,
                        "savestate: slot %d resume_pc was 0 — using fallback "
                        "pc=0x%08X\n",
                        slot, (unsigned)pc);
            }
            if (savestate_slot_path(slot, path, sizeof(path))) {
                /* Save the exact resume PC (cpu->pc is 0 mid-block; resume_pc is
                 * the block leader the interrupt path would resume at). */
                CPUState snap = *cpu;
                snap.pc = pc;
                int ok = boot_state_save(&snap, s_bios_checksum, s_entry_pc, path);
                if (ok) {
                    s_last_save_pc = pc;
                    s_save_failed = 0;
                    (void)savestate_capture_thumb(slot);
                } else {
                    s_last_save_pc = 0;
                    s_save_failed = 1;
                }
                fprintf(stderr, "savestate: %s slot %d @ pc=0x%08X -> %s\n",
                        ok ? "SAVED" : "SAVE FAILED", slot, (unsigned)pc, path);
                psx_frontend_on_savestate_notify(0, slot, ok);
            } else {
                s_last_save_pc = 0;
                s_save_failed = 1;
                psx_frontend_on_savestate_notify(0, slot, 0);
            }
        }
    }

    if (s_load_pending >= 0) {
        int slot = s_load_pending;
        int loaded = 0;
        s_load_pending = -1;
        char path[600];
        const double t_load0 = savestate_mono_ms();
        double t_after_boot = t_load0;
        double t_after_frontend = t_load0;
        path[0] = '\0';
        if (s_load_blob && s_load_blob_len > 0) {
            const size_t blob_len = s_load_blob_len;
            loaded = boot_state_load_buffer(s_load_blob, blob_len,
                                            s_bios_checksum, s_entry_pc, cpu);
            clear_load_blob();
            if (!loaded) {
                fprintf(stderr,
                        "savestate: LOAD FAILED blob (%zu bytes, entry=%08X)\n",
                        blob_len, (unsigned)s_entry_pc);
                s_load_failed = 1;
                psx_frontend_on_savestate_notify(1, slot, 0);
            }
        } else if (savestate_slot_path(slot, path, sizeof(path))) {
            loaded = boot_state_load(path, s_bios_checksum, s_entry_pc, cpu);
            if (!loaded) {
                fprintf(stderr,
                        "savestate: LOAD FAILED slot %d %s\n",
                        slot, path);
                s_load_failed = 1;
                psx_frontend_on_savestate_notify(1, slot, 0);
            }
        } else {
            fprintf(stderr, "savestate: LOAD FAILED slot %d (no path)\n", slot);
            s_load_failed = 1;
            psx_frontend_on_savestate_notify(1, slot, 0);
        }
        if (loaded && !savestate_resume_pc_ok(cpu->pc)) {
            fprintf(stderr,
                    "savestate: LOAD FAILED slot %d — resume pc=0x%08X "
                    "(null/undispatchable)%s\n",
                    slot, (unsigned)cpu->pc, path[0] ? "" : " [blob]");
            loaded = 0;
            s_load_failed = 1;
            psx_frontend_on_savestate_notify(1, slot, 0);
        }
        if (loaded) {
            t_after_boot = savestate_mono_ms();
            psx_cycles_resync_after_restore(cpu);
            /* Drop absolute-cycle IRQ cooldowns / VBlank phase from the
             * pre-load host timeline (cycle rewind would otherwise blackout
             * VBlank delivery for however long the user played past the save). */
            interrupts_resync_after_restore();
            /* Collapse restored / imminent CD second-response debt (ReadTOC,
             * Init, seeks) so the picture does not freeze for ~1s after the
             * restored frame presents. */
            cdrom_accelerate_after_savestate();
            /* Netplay post-load barrier observes this before the longjmp. */
            s_load_completed = 1;
            /* Restage FBO/present latch so the restored frame is visible
             * immediately (avoids disabled-display blank latch + stale smooth). */
            psx_frontend_on_savestate_loaded();
            t_after_frontend = savestate_mono_ms();
            fprintf(stderr,
                    "savestate: LOADED slot %d -> resuming pc=0x%08X "
                    "(boot=%.1f frontend=%.1f poll_total=%.1f ms)%s\n",
                    slot, (unsigned)cpu->pc,
                    t_after_boot - t_load0,
                    t_after_frontend - t_after_boot,
                    t_after_frontend - t_load0,
                    path[0] ? "" : " [blob]");
            psx_frontend_on_savestate_notify(1, slot, 1);
            /* Unwind to the scheduler and re-dispatch the restored PC. Never
             * returns; abandons the suspended CPS frames on the current stack. */
            psx_scheduler_resume_at(cpu->pc);
        }
    }
}
