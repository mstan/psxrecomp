#ifndef PSX_SAVESTATE_H
#define PSX_SAVESTATE_H

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
 * timers/IRQ/clock/dirty-bitmap, with an integrity key so a state can never load
 * into an incompatible build). The only additions here are: per-slot file paths,
 * deferred execution at a safe block-leader boundary (so cpu->pc is a valid
 * resume PC and in_exception == 0), and a restore that unwinds to the scheduler
 * and re-dispatches (psx_scheduler_resume_at).
 */

#define SAVESTATE_SLOTS 12

/* Configure the slot directory + integrity key (from main, after config load).
 * dir = the per-game memcard/save dir; files land at <dir>/state_slotNN.pst. */
void savestate_configure(const char* dir, uint32_t bios_checksum, uint32_t entry_pc);

/* Stage a save/load of slot [0..SAVESTATE_SLOTS-1]. Executed at the next safe
 * boundary by savestate_poll (called every block from psx_check_interrupts).
 * Safe to call from the SDL key handler or a debug-server command. */
/* Stage a save/load for the next block boundary. Returns 1 if staged, 0 if
 * the request cannot be honored (not configured, bad slot, or — for load —
 * an LLE host-fiber run, where the cross-fiber unwind is unsafe). */
int savestate_request_save(int slot);
int savestate_request_load(int slot);

/* Called every block from psx_check_interrupts (in_exception == 0). If a save is
 * pending, serialize with cpu->pc = resume_pc; if a load is pending, restore and
 * longjmp to the scheduler (never returns in that case). Near-free when idle. */
void savestate_poll(CPUState* cpu, uint32_t resume_pc);

#ifdef __cplusplus
}
#endif

#endif /* PSX_SAVESTATE_H */
