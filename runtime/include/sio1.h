/*
 * sio1.h -- PS1 Serial Port (SIO1) controller, 0x1F801050-0x1F80105F.
 *
 * The link-cable serial port -- a separate device from SIO0 (pads/memcards,
 * sio.h). Register semantics follow nocash psx-spx "Serial Port (SIO1)";
 * see accuracy/axis4_sio1_serial.md for the audit and known simplifications.
 *
 * Two API layers:
 *   1. Sio1Device -- a pure instance (injected clock, injected IRQ callback,
 *      link endpoint attached via psx_link.h). No runtime dependencies:
 *      unit tests link sio1.c + psx_link.c standalone, and two instances can
 *      coexist (the dual-console driver needs exactly that).
 *   2. sio1_* singleton wrapper (sio1_runtime.c) -- wires the instance to
 *      psx_cycle_count / psx_irq_raise / config for the live machine.
 */
#ifndef PSX_SIO1_H
#define PSX_SIO1_H

#include <stdint.h>

#include "psx_link.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SIO1_BASE 0x1F801050u

/* STAT bits (low half; 11..31 = baud timer, computed lazily at read). */
#define SIO1_STAT_TXRDY1   (1u << 0)
#define SIO1_STAT_RXNE     (1u << 1)
#define SIO1_STAT_TXDONE   (1u << 2)
#define SIO1_STAT_PARITY   (1u << 3)
#define SIO1_STAT_OVERRUN  (1u << 4)
#define SIO1_STAT_BADSTOP  (1u << 5)
#define SIO1_STAT_DSR      (1u << 7)
#define SIO1_STAT_CTS      (1u << 8)
#define SIO1_STAT_IRQ      (1u << 9)

/* CTRL bits. */
#define SIO1_CTRL_TXEN     (1u << 0)
#define SIO1_CTRL_DTR      (1u << 1)
#define SIO1_CTRL_RXEN     (1u << 2)
#define SIO1_CTRL_TXLEVEL  (1u << 3)
#define SIO1_CTRL_ACK      (1u << 4)   /* strobe, reads back 0 */
#define SIO1_CTRL_RTS      (1u << 5)
#define SIO1_CTRL_RESET    (1u << 6)   /* strobe, reads back 0 */
#define SIO1_CTRL_TX_IRQ   (1u << 10)
#define SIO1_CTRL_RX_IRQ   (1u << 11)
#define SIO1_CTRL_DSR_IRQ  (1u << 12)
#define SIO1_CTRL_STORED_MASK 0x1FAFu

#define SIO1_RX_FIFO_DEPTH 8u

/* IRQ edge detail values (psx_irq_raise detail / device_trace). */
#define SIO1_IRQ_DETAIL_RX  1u
#define SIO1_IRQ_DETAIL_TX  2u
#define SIO1_IRQ_DETAIL_DSR 3u

/* ===== instance API ====================================================== */

typedef struct Sio1Device Sio1Device;
typedef void (*Sio1IrqFn)(void *user, uint32_t detail);

Sio1Device *sio1_device_create(void);
void        sio1_device_destroy(Sio1Device *d);
void        sio1_device_attach(Sio1Device *d, PsxLinkEndpoint *ep);
void        sio1_device_set_irq(Sio1Device *d, Sio1IrqFn fn, void *user);
/* Full block reset (power-on / CTRL.6). `now` re-anchors the baud timer. */
void        sio1_device_reset(Sio1Device *d, uint64_t now);

/* MMIO. `addr` is the UNALIGNED guest address (lane decode happens here --
 * MODE/CTRL share word 0x1058, BAUD shares 0x105C), `width` is 1/2/4,
 * `now` the current guest cycle. */
uint32_t sio1_device_read (Sio1Device *d, uint32_t addr, uint32_t width,
                           uint64_t now);
void     sio1_device_write(Sio1Device *d, uint32_t addr, uint32_t width,
                           uint32_t value, uint64_t now);

/* Advance device time by `cycles`, ending at guest cycle `now`. Completes TX
 * shifts, drains due inbound characters into the RX FIFO, samples handshake
 * lines, fires IRQ edges. At most safe to call with any chunk size; event
 * slicing keeps chunks landing on byte boundaries. */
void     sio1_device_advance(Sio1Device *d, uint32_t cycles, uint64_t now);

/* Conservative under-estimate of cycles until the next internal event
 * (TX shift completion / inbound char due). 0xFFFFFFFF = none pending or
 * IRQ8 masked in `i_mask`. Pass 0xFFFFFFFF as i_mask for mask-blind event
 * chunking. */
uint32_t sio1_device_cycles_to_irq(const Sio1Device *d, uint32_t i_mask);
int      sio1_device_active(const Sio1Device *d);

/* Derived timing (test/diag hooks). */
uint32_t sio1_device_bit_cycles (const Sio1Device *d);
uint32_t sio1_device_char_cycles(const Sio1Device *d);
/* Side-effect-free register peeks (debug server: the observer must not
 * perturb the observed -- same rule as sio.h). */
uint32_t sio1_device_peek_stat(const Sio1Device *d, uint64_t now);
uint16_t sio1_device_peek_mode(const Sio1Device *d);
uint16_t sio1_device_peek_ctrl(const Sio1Device *d);
uint16_t sio1_device_peek_baud(const Sio1Device *d);
void     sio1_device_get_counters(const Sio1Device *d, uint32_t *tx_chars,
                                  uint32_t *rx_chars, uint32_t *overruns,
                                  uint32_t *irqs);

/* Snapshot (device + attached endpoint). Timeline values serialize as deltas
 * from `now`. Section ends (cumulative offsets): [0]=regs, [1]=fsm+endpoint
 * (pace -- netplay digest folds through here), [2]=telemetry meta = total. */
uint32_t sio1_device_snap_bytes(Sio1Device *d);
void     sio1_device_snap_write(Sio1Device *d, uint8_t *p, uint64_t now);
int      sio1_device_snap_read (Sio1Device *d, const uint8_t *p, uint32_t len,
                                uint64_t now);
void     sio1_device_snap_section_ends(Sio1Device *d, uint32_t out[3]);

/* ===== singleton wrapper (sio1_runtime.c; live machine only) ============= */

void     sio1_init(void);
uint32_t sio1_read (uint32_t addr, uint32_t width);
void     sio1_write(uint32_t addr, uint32_t width, uint32_t value);
void     sio1_advance(uint32_t cycles);
uint32_t sio1_cycles_to_irq(uint32_t i_mask);

/* Hot-path guard for the advance/slicing paths (mirrors g_sio_timing_active):
 * nonzero only while a shift is in flight or inbound chars are queued. */
extern volatile int g_sio1_active;
/* Kill-switch: PSX_SIO1_REGS=0 restores the legacy fold-into-SIO0 decode
 * (reads 0 / writes dropped). Default 1. Read by memory.c dispatchers. */
extern int g_sio1_regs_enabled;

/* Boot-state / digest glue (BS_SEC_SIO1). */
uint32_t sio1_snapshot_bytes(void);
void     sio1_snapshot_write(uint8_t *p);
int      sio1_snapshot_read(const uint8_t *p, uint32_t len);
void     sio1_snapshot_section_ends(uint32_t out[3]);

/* Config ([runtime.link] / env): backend "null" | "loopback" | "crossover".
 * "crossover" is reserved for the dual-console driver, which attaches the
 * endpoints itself; selecting it here without that driver leaves "null".
 * Returns 1 on success. */
int  sio1_set_backend(const char *name);
void sio1_set_latency_cycles(uint32_t cycles);
const char *sio1_get_backend(void);

/* Debug-server peeks into the live singleton. */
Sio1Device *sio1_get_device(void);

/* Dual-console support (dual_machine.c): swap which per-machine device the
 * singleton wrapper drives, and suppress BS_SEC_SIO1 snapshot APPLY while
 * dual mode is active (the crossover wire is host-owned and must survive
 * machine switches byte-exact; writes still serialize normally). */
void sio1_dual_install(Sio1Device *d);
void sio1_dual_suppress_snapshot_apply(int on);

#ifdef __cplusplus
}
#endif

#endif /* PSX_SIO1_H */
