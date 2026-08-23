/*
 * dual_machine.h -- Stage B dual-console driver (serial-link bring-up).
 *
 * Runs TWO complete PSX guest machines in one process by cooperative
 * time-slicing over the existing boot_state snapshot swap: save the live
 * machine at a CPS block boundary, restore the other machine's blob, and
 * psx_scheduler_resume_at() into it. The two machines' SIO1 devices are
 * cross-wired by a psx_link crossover whose rings live in HOST memory and
 * deliberately survive switches (the wire is the channel between machines,
 * so BS_SEC_SIO1 apply is suppressed while dual mode is active).
 *
 * Scheduling invariant: |cycles(A) - cycles(B)| <= slice at every switch
 * decision. While the link is armed (DTR up, shift in flight, or chars
 * queued) the slice is one character time (both consoles see each byte
 * within one char of its true arrival); otherwise a coarse slice keeps the
 * swap tax low during menus/boot.
 *
 * Stage B limitations (documented, by design):
 *  - user savestates / rewind / netplay rollback are not dual-aware
 *    (rewind is shut down at activation);
 *  - host input is pushed to BOTH machines (seat split is Stage D);
 *  - only the "local" machine presents video / mixes audio.
 */
#ifndef PSX_DUAL_MACHINE_H
#define PSX_DUAL_MACHINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/* One load+branch gate for the per-edge poll in psx_check_interrupts:
 * nonzero from the moment dual mode is REQUESTED (config/env). */
extern int g_psx_dual_active;

/* Request dual-console mode (config [runtime.link] backend="crossover" or
 * env PSX_DUAL_CONSOLE=1). local_machine selects which machine presents
 * video/audio (0 or 1). Activation itself is lazy: the first poll with a
 * safe resume PC captures the shared boot state and arms the crossover. */
void psx_dual_machine_request(int local_machine);

/* Per-block-edge poll (interrupts.c). On a switch this does NOT return --
 * it longjmps into the other machine via psx_scheduler_resume_at. */
void psx_dual_machine_poll(struct CPUState *cpu, uint32_t resume_pc);

/* Veto a pending config-file dual request before activation
 * (PSX_DUAL_CONSOLE=0). No-op once two machines are running. */
void psx_dual_machine_cancel(void);

/* 1 = present/audio allowed (dual inactive, or the live machine is the
 * local one). Consulted by the vblank present path in main.cpp. */
int psx_dual_present_gate(void);

/* Runtime A/V ownership (F6 focus). Sets the machine psx_dual_present_gate()
 * lets through; the other console runs headless. */
void psx_dual_set_local_machine(int machine);
int  psx_dual_get_local_machine(void);

/* Input routing: which machine(s) receive the host pads. The excluded
 * machine is fed neutral pads. 0 = both (default), 1 = machine 0 only,
 * 2 = machine 1 only. Needed for asymmetric menu navigation (the link
 * master/slave handshake) while Stage B mirrors one set of host pads. */
void psx_dual_set_input_route(int route);
int  psx_dual_get_input_route(void);
/* 1 = host pads flow to the LIVE machine under the current route. */
int  psx_dual_input_allowed(void);

/* Diagnostics (debug server "dual_state"). */
int      psx_dual_machine_live(void);      /* -1 when inactive */
uint64_t psx_dual_machine_swaps(void);
void     psx_dual_machine_cycles(uint64_t out[2]);

#ifdef __cplusplus
}
#endif

#endif /* PSX_DUAL_MACHINE_H */
