#ifndef PSXRECOMP_V4_TIMERS_H
#define PSXRECOMP_V4_TIMERS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TIMER_BASE 0x1F801100

void timers_init(void);

/* MMIO read/write for 0x1F801100..0x1F80112F.
 * Accepts 16-bit or 32-bit access (caller zero-extends). */
uint32_t timers_read(uint32_t addr);
void     timers_write(uint32_t addr, uint32_t value);

/* Advance all timers by guest CPU cycles. Used by native block-cycle builds. */
void timers_advance(uint32_t cycles);

/* Legacy coarse tick used by interpreter/oracle builds. */
void timers_tick(int cycles);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_V4_TIMERS_H */
