/*
 * psx_cpu_pin.h -- PSX-Link pair CPU placement.
 *
 * A link-lobby machine runs TWO full console simulations: the netplay client
 * (driver) and the spawned headless follower. Both carry an emulation thread
 * that needs most of a core to hold the 20 ms PAL tick budget; when the
 * kernel lands them on SMT siblings of the same physical core they gate each
 * other and the whole 4-seat session drops below 50 fps (lockstep couples
 * every machine to the slowest one).
 *
 * psx_cpu_pin_link_role() pins the CALLING thread (the emulation thread) to
 * a dedicated physical core: role 0 (driver) takes the fastest eligible
 * core, role 1 (follower) the second-fastest. Both processes enumerate the
 * same topology, so the picks never collide. Threads created AFTER a pin
 * inherit it -- helper threads that must float (present thread) call
 * psx_cpu_pin_unpin_self() to restore the pre-pin process mask.
 *
 * PSX_LINK_PIN=0        disable
 * PSX_LINK_PIN=<a>,<b>  explicit logical CPU ids (driver=a, follower=b)
 *
 * Linux only; a no-op elsewhere.
 */
#ifndef PSX_CPU_PIN_H
#define PSX_CPU_PIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* role: 0 = link driver (netplay client), 1 = link follower. Call from the
 * emulation thread once all session service threads exist (they must not
 * inherit the pin). Logs the placement (or why it declined). */
void psx_cpu_pin_link_role(int role);

/* Restore the calling thread to the process's pre-pin affinity mask. For
 * threads spawned from a pinned emulation thread that should float. Safe to
 * call when no pin ever happened. */
void psx_cpu_pin_unpin_self(void);

#ifdef __cplusplus
}
#endif
#endif /* PSX_CPU_PIN_H */
