/* savestate.c — user save states (F1-F12 / Shift+F1-F12). See savestate.h.
 *
 * Wraps boot_state.c's full-machine serializer. Requests are staged by the SDL
 * key handler / debug server and executed by savestate_poll at a block-leader
 * boundary (in_exception == 0), where cpu->pc is a valid resume PC. A load
 * restores the full machine then unwinds to the scheduler and re-dispatches. */

#include "savestate.h"
#include "boot_state.h"
#include "psx_cycles.h"
#include "psx_netplay.h"
#include "psx_scheduler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

static char     s_dir[512];
static uint32_t s_bios_checksum;
static uint32_t s_entry_pc;
static int      s_configured   = 0;
static int      s_save_pending = -1;   /* slot, or -1 */
static int      s_load_pending = -1;
static int      s_load_completed = 0;

extern int psx_hle_scheduler_enabled(void);

static void ensure_dir(const char* dir) {
    if (!dir || !dir[0]) return;
#ifdef _WIN32
    (void)_mkdir(dir);
#else
    (void)mkdir(dir, 0755);
#endif
}

void savestate_configure(const char* dir, uint32_t bios_checksum, uint32_t entry_pc) {
    if (dir && dir[0]) {
        strncpy(s_dir, dir, sizeof(s_dir) - 1);
        s_dir[sizeof(s_dir) - 1] = '\0';
        ensure_dir(s_dir);
    } else {
        s_dir[0] = '\0';
    }
    s_bios_checksum = bios_checksum;
    s_entry_pc      = entry_pc;
    s_configured    = 1;
}

const char* savestate_dir(void) {
    return s_dir;
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
    if (!data || size == 0) return 0;
    if (!savestate_slot_path(slot, path, sizeof(path))) return 0;
    ensure_dir(s_dir);
    f = fopen(path, "wb");
    if (!f) return 0;
    if (fwrite(data, 1, size, f) != size) {
        fclose(f);
        remove(path);
        return 0;
    }
    if (fflush(f) != 0 || fclose(f) != 0) {
        remove(path);
        return 0;
    }
    return 1;
}

static int netplay_guest_user_blocked(void) {
    return psx_netplay_active() && !psx_netplay_is_host();
}

static int request_save_inner(int slot) {
    if (!s_configured) { fprintf(stderr, "savestate: not configured\n"); return 0; }
    if (slot < 0 || slot >= SAVESTATE_SLOTS) return 0;
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
    s_load_pending = slot;
    return 1;
}

int savestate_request_save(int slot) {
    if (netplay_guest_user_blocked()) return 0;
    return request_save_inner(slot);
}

int savestate_request_load(int slot) {
    if (netplay_guest_user_blocked()) return 0;
    return request_load_inner(slot);
}

int savestate_request_save_protocol(int slot) {
    return request_save_inner(slot);
}

int savestate_request_load_protocol(int slot) {
    return request_load_inner(slot);
}

int savestate_pending(void) {
    return (s_save_pending >= 0 || s_load_pending >= 0) ? 1 : 0;
}

int savestate_take_load_completed(void) {
    int v = s_load_completed;
    s_load_completed = 0;
    return v;
}

void savestate_poll(CPUState* cpu, uint32_t resume_pc) {
    if (s_save_pending < 0 && s_load_pending < 0) return;   /* hot path: nothing staged */

    if (s_save_pending >= 0) {
        int slot = s_save_pending;
        s_save_pending = -1;
        char path[600];
        if (savestate_slot_path(slot, path, sizeof(path))) {
            /* Save the exact resume PC (cpu->pc is 0 mid-block; resume_pc is the
             * block leader the interrupt path would resume at). */
            CPUState snap = *cpu;
            snap.pc = resume_pc;
            int ok = boot_state_save(&snap, s_bios_checksum, s_entry_pc, path);
            fprintf(stderr, "savestate: %s slot %d @ pc=0x%08X -> %s\n",
                    ok ? "SAVED" : "SAVE FAILED", slot, (unsigned)resume_pc, path);
        }
    }

    if (s_load_pending >= 0) {
        int slot = s_load_pending;
        s_load_pending = -1;
        char path[600];
        if (!savestate_slot_path(slot, path, sizeof(path))) return;
        if (boot_state_load(path, s_bios_checksum, s_entry_pc, cpu)) {
            psx_cycles_resync_after_restore();
            fprintf(stderr, "savestate: LOADED slot %d -> resuming pc=0x%08X\n",
                    slot, (unsigned)cpu->pc);
            /* Netplay post-load barrier observes this before the longjmp. */
            s_load_completed = 1;
            /* Restage FBO/present latch so the restored frame is visible
             * immediately (avoids disabled-display blank latch + stale smooth). */
            psx_frontend_on_savestate_loaded();
            /* Unwind to the scheduler and re-dispatch the restored PC. Never
             * returns; abandons the suspended CPS frames on the current stack. */
            psx_scheduler_resume_at(cpu->pc);
        } else {
            fprintf(stderr, "savestate: LOAD FAILED slot %d (missing/mismatched) %s\n",
                    slot, path);
        }
    }
}
