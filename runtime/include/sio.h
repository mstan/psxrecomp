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

/* ---- SIO byte-level trace ring buffer ---- */
#define SIO_TRACE_CAP 65536

typedef struct {
    uint32_t seq;           /* monotonic sequence number */
    uint8_t  tx;            /* byte written to TX */
    uint8_t  rx;            /* byte produced in RX */
    uint8_t  mc_state_pre;  /* mc_state BEFORE processing */
    uint8_t  mc_state_post; /* mc_state AFTER processing */
    uint8_t  dev_pre;       /* active_device BEFORE (0=NONE,1=PAD,2=MC) */
    uint8_t  dev_post;      /* active_device AFTER */
    uint16_t ctrl;          /* CTRL register value */
    uint32_t func_addr;     /* g_debug_current_func_addr */
    uint8_t  was_abort;     /* 1 if this byte caused mc_state abort */
    uint8_t  irq_countdown; /* sio_irq_countdown at entry */
    uint8_t  in_exception;  /* psx_get_in_exception() at time of byte */
    uint8_t  counter_7514; /* RAM[0x7514] at time of byte */
    uint32_t cop0_sr;       /* COP0 SR at time of byte (IEc=bit0, IM2=bit10) */
} SioTraceEntry;

/* Get pointer to ring buffer and current write index.
 * Returns number of entries ever written (seq of next write). */
uint32_t sio_get_trace(const SioTraceEntry **buf_out, int *write_idx_out);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_V4_SIO_H */
