#ifndef PSX_SAVESTATE_H
#define PSX_SAVESTATE_H

#include <stddef.h>
#include <stdint.h>
#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * User save states (F1-F12 save, Shift+F1-F12 load; 12 slots).
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

/* Configure the slot directory + integrity key (from main, after config load).
 * dir = the per-game memcard/save dir; files land at
 * <dir>/state_<entry_pc>_slotNN.pst. */
void savestate_configure(const char* dir, uint32_t bios_checksum, uint32_t entry_pc);

/* Current slot directory (empty if not configured). */
const char* savestate_dir(void);

/* Integrity key last passed to savestate_configure (for sandbox rebind). */
void savestate_get_integrity(uint32_t* bios_checksum, uint32_t* entry_pc);

/* Build the on-disk path for slot [0..SAVESTATE_SLOTS-1]. Returns 1 on success. */
int savestate_slot_path(int slot, char* out, size_t cap);

/* Read/write a slot file (malloc'd buffer for read). Returns 1 on success. */
int savestate_read_slot(int slot, uint8_t** data_out, size_t* size_out);
int savestate_write_slot(int slot, const void* data, size_t size);

/* 1 if the slot file exists and is non-empty. */
int savestate_slot_exists(int slot);

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

/* 1 while a staged save/load has not yet been consumed by savestate_poll. */
int savestate_pending(void);

/* 1 once after a successful load restore (before scheduler longjmp). Clears. */
int savestate_take_load_completed(void);

/* Frontend hook (main.cpp): restage VRAM present path after a successful load. */
void psx_frontend_on_savestate_loaded(void);

/* Called every block from psx_check_interrupts (in_exception == 0). If a save is
 * pending, serialize with cpu->pc = resume_pc; if a load is pending, restore and
 * longjmp to the scheduler (never returns in that case). Near-free when idle. */
void savestate_poll(CPUState* cpu, uint32_t resume_pc);

#ifdef __cplusplus
}
#endif

#endif /* PSX_SAVESTATE_H */
