#ifndef PSXRECOMP_V4_SIO_H
#define PSXRECOMP_V4_SIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SIO0 register base: 0x1F801040 */
#define SIO_BASE 0x1F801040

void sio_init(void);

/* MMIO read/write (0x1F801040-0x1F80104E) */
uint32_t sio_read(uint32_t addr);
void sio_write(uint32_t addr, uint32_t value);

/* Advance SIO timing by one step. Called from psx_check_interrupts().
 * Fires pending IRQ7 after the transfer delay expires. */
void sio_tick(void);

/* Update pad button state. Buttons use PS1 convention: 0=pressed, 1=released.
   Bit layout: SELECT, L3, R3, START, UP, RIGHT, DOWN, LEFT,
               L2, R2, L1, R1, TRIANGLE, CIRCLE, CROSS, SQUARE */
void sio_set_pad_state(uint16_t buttons);

/* Connect a pad to a slot (0=port1, 1=port2). By default no pads are
 * connected during initial BIOS boot. */
void sio_connect_pad(int slot);

/* Return current pad button state (for debug server). */
uint16_t sio_get_pad_buttons(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_V4_SIO_H */
