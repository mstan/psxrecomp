/* savestate.c — user save states (F1-F12 / Shift+F1-F12). See savestate.h.
 *
 * Wraps boot_state.c's full-machine serializer. Requests are staged by the SDL
 * key handler / debug server and executed by savestate_poll at a block-leader
 * boundary (in_exception == 0), where cpu->pc is a valid resume PC. A load
 * restores the full machine then unwinds to the scheduler and re-dispatches. */

#include "savestate.h"
#include "boot_state.h"
#include "psx_cycles.h"
#include "psx_scheduler.h"
#include <stdio.h>
#include <string.h>

static char     s_dir[512];
static uint32_t s_bios_checksum;
static uint32_t s_entry_pc;
static int      s_configured   = 0;
static int      s_save_pending = -1;   /* slot, or -1 */
static int      s_load_pending = -1;

extern int psx_hle_scheduler_enabled(void);

void savestate_configure(const char* dir, uint32_t bios_checksum, uint32_t entry_pc) {
    if (dir && dir[0]) {
        strncpy(s_dir, dir, sizeof(s_dir) - 1);
        s_dir[sizeof(s_dir) - 1] = '\0';
    } else {
        s_dir[0] = '\0';
    }
    s_bios_checksum = bios_checksum;
    s_entry_pc      = entry_pc;
    s_configured    = 1;
}

static int slot_path(int slot, char* out, size_t cap) {
    if (slot < 0 || slot >= SAVESTATE_SLOTS) return 0;
    /* Keyed by entry_pc so slots from different games in a shared dir never
     * collide; boot_state_load also rejects a mismatched entry_pc internally. */
    snprintf(out, cap, "%s%sstate_%08X_slot%02d.pst",
             s_dir, (s_dir[0] ? "/" : ""), (unsigned)s_entry_pc, slot);
    return 1;
}

int savestate_request_save(int slot) {
    if (!s_configured) { fprintf(stderr, "savestate: not configured\n"); return 0; }
    if (slot < 0 || slot >= SAVESTATE_SLOTS) return 0;
    s_save_pending = slot;
    return 1;
}

int savestate_request_load(int slot) {
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

void savestate_poll(CPUState* cpu, uint32_t resume_pc) {
    if (s_save_pending < 0 && s_load_pending < 0) return;   /* hot path: nothing staged */

    if (s_save_pending >= 0) {
        int slot = s_save_pending;
        s_save_pending = -1;
        char path[600];
        if (slot_path(slot, path, sizeof(path))) {
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
        if (!slot_path(slot, path, sizeof(path))) return;
        if (boot_state_load(path, s_bios_checksum, s_entry_pc, cpu)) {
            psx_cycles_resync_after_restore();
            fprintf(stderr, "savestate: LOADED slot %d -> resuming pc=0x%08X\n",
                    slot, (unsigned)cpu->pc);
            /* Unwind to the scheduler and re-dispatch the restored PC. Never
             * returns; abandons the suspended CPS frames on the current stack. */
            psx_scheduler_resume_at(cpu->pc);
        } else {
            fprintf(stderr, "savestate: LOAD FAILED slot %d (missing/mismatched) %s\n",
                    slot, path);
        }
    }
}
