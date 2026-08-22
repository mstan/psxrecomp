#ifndef PSX_SAVESTATE_H
#define PSX_SAVESTATE_H

#include <stddef.h>
#include <stdint.h>
#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * User save states (F7 save-state menu by default; 12 slots).
 *
 * A thin wrapper over boot_state.c's complete full-machine serializer
 * (boot_state_save / boot_state_load — CPU/RAM/scratchpad/VRAM/SPU/CDROM/DMA/SIO/
 * timers/IRQ/clock/dirty-bitmap). Format is BOOT_STATE_VERSION 3: little-endian
 * field wires portable across Win/Linux/macOS ARM (see pst_wire.h). Integrity
 * key still rejects incompatible builds. The only additions here are: per-slot paths,
 * deferred execution at a safe block-leader boundary (so cpu->pc is a valid
 * resume PC and in_exception == 0), and a restore that unwinds to the scheduler
 * and re-dispatches (psx_scheduler_resume_at).
 */

#define SAVESTATE_SLOTS 12
#define SAVESTATE_THUMB_W 128
#define SAVESTATE_THUMB_H 96

/* Configure the slot directory + integrity key (from main, after config load).
 *
 * When bios_token is non-empty (e.g. "openbios" / "scph1001"):
 *   dir is the per-game memcard/save ROOT; slots land at
 *   <dir>/<bios_token>/state_<entry_pc>_slotNN.pst.
 *   Loose legacy <dir>/state_*.pst files are migrated once by .pst header
 *   bios_checksum (openbios_wordsum → openbios/; else → scph1001/).
 *
 * When bios_token is NULL/empty: dir is used as-is (netplay guest sandbox /
 * leave restore of an already-scoped path). */
void savestate_configure(const char* dir, uint32_t bios_checksum, uint32_t entry_pc,
                         const char* bios_token, uint32_t openbios_wordsum);

/* Current slot directory (empty if not configured). */
const char* savestate_dir(void);

/* Memcard/save root remembered from the last bios-scoped configure
 * (empty until a non-empty bios_token was passed). */
const char* savestate_root_dir(void);

/* Last bios_token / openbios_wordsum from a bios-scoped configure
 * (token empty when never scoped; used to restore after netplay sandbox). */
const char* savestate_bios_token(void);
uint32_t savestate_openbios_wordsum(void);

/* Integrity key last passed to savestate_configure (for sandbox rebind). */
void savestate_get_integrity(uint32_t* bios_checksum, uint32_t* entry_pc);

/* Build the on-disk path for slot [0..SAVESTATE_SLOTS-1]. Returns 1 on success. */
int savestate_slot_path(int slot, char* out, size_t cap);

/* Read/write a slot file (malloc'd buffer for read). Returns 1 on success. */
int savestate_read_slot(int slot, uint8_t** data_out, size_t* size_out);
int savestate_write_slot(int slot, const void* data, size_t size);

/* 1 if the slot file exists and is non-empty. */
int savestate_slot_exists(int slot);

/* Slot file modified time, seconds since the Unix epoch. */
int savestate_slot_mtime(int slot, int64_t* out_time);

/* Per-slot screenshot thumbnails captured after a successful save. */
int savestate_capture_thumb(int slot);
int savestate_read_thumb(int slot, uint32_t* out_argb,
                         int out_w, int out_h);

/* 1 if the slot .pst header matches this build's integrity key (BIOS/entry/
 * codegen). 0 + optional reason when missing or stale — use before netplay
 * load probe so incompatible saves never enter the post-load barrier. */
int savestate_slot_compatible(int slot, char* reason, size_t reason_cap);

/* Stage a save/load of slot [0..SAVESTATE_SLOTS-1]. Executed at the next safe
 * boundary by savestate_poll (called every block from psx_check_interrupts).
 * Safe to call from the SDL key handler or a debug-server command.
 * Returns 1 if staged, 0 if refused (not configured, bad slot, LLE load, or
 * netplay guest — only the match host may initiate user save/load). */
int savestate_request_save(int slot);
int savestate_request_load(int slot);

/* Netplay follow-host sync only. Bypasses the guest user-initiation guard so
 * the guest can write/apply the host-authoritative slot during probe/transfer. */
int savestate_request_save_protocol(int slot);
int savestate_request_load_protocol(int slot);

/* Netplay LOAD transfer: stage an in-memory .pst (no disk write). Copied
 * internally; applied by savestate_poll like a normal slot load. */
int savestate_request_load_blob_protocol(const void* data, size_t size);

/* 1 while a staged save/load has not yet been consumed by savestate_poll. */
int savestate_pending(void);

/* Machine-readable lifecycle receipt for deterministic debug harnesses. */
void savestate_status_json(char* buf, size_t cap);

/* 1 once after a successful load restore (before scheduler longjmp). Clears. */
int savestate_take_load_completed(void);

/* 1 once after a staged load failed in savestate_poll (missing/mismatched).
 * Clears. Netplay uses this to abort the load barrier instead of hanging. */
int savestate_take_load_failed(void);

/* 1 once after a staged save failed in savestate_poll (no safe resume PC,
 * I/O error, etc.). Clears. Netplay aborts SAVE coord instead of transferring
 * a stale/null-PC .pst. */
int savestate_take_save_failed(void);

/* Resume PC stamped into the last successful slot save (0 if none / failed). */
uint32_t savestate_last_save_pc(void);

/* Frontend hook (main.cpp): restage VRAM present path after a successful load. */
void psx_frontend_on_savestate_loaded(void);
/* Rollback snap apply: depth24 hold clear + restage without FMV cutover thrash. */
void psx_frontend_on_rb_snap_loaded(void);

/* Frontend hook (main.cpp): host OSD toast after a user save/load settles.
 * is_load: 0 = save, 1 = load. slot is 0-based. ok: 1 on success. */
void psx_frontend_on_savestate_notify(int is_load, int slot, int ok);

/* Called every block from psx_check_interrupts (in_exception == 0). If a save is
 * pending, serialize with cpu->pc = resume_pc; if a load is pending, restore and
 * longjmp to the scheduler (never returns in that case). Near-free when idle. */
void savestate_poll(CPUState* cpu, uint32_t resume_pc);

#ifdef __cplusplus
}
#endif

#endif /* PSX_SAVESTATE_H */
