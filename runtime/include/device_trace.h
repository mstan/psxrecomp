/* device_trace.h — general two-process DEVICE-EVENT cycle ring.
 *
 * Purpose: name the device/IRQ event whose guest-cycle timing diverges between
 * psx-runtime (code under test) and psx-beetle (independent oracle). Where
 * parity_trace answers "which control-flow / gate-input diverged", this answers
 * the level below it: "which hardware event (CD sector IRQ / DMA completion /
 * RootCounter / VBlank / SPU) fired at a different guest cycle" — the producer
 * of an over-fast or over-slow wait.
 *
 * Model (CLAUDE.md ring-buffer doctrine): ALWAYS-ON from boot, records one row
 * per hardware IRQ-raise edge (the single choke point each backend funnels its
 * device interrupts through — native psx_irq_raise(); Beetle mednafen
 * IRQ_Assert rising edge), stamped with the deterministic guest-cycle ruler.
 * A probe QUERIES the ring for a cycle window; it never arms-then-times.
 *
 * Generality: nothing here is title-specific. The SAME TU compiles into both
 * binaries and emits IDENTICAL rows, so tools/devtrace_diff.py pulls both
 * timelines and aligns them by guest cycle to find the first divergent event.
 * This is the reusable device-timing audit substrate for the whole ecosystem
 * (see recomp-template); the MMX6 cutscene asset-load over-fast wedge is its
 * first user.
 */
#ifndef DEVICE_TRACE_H
#define DEVICE_TRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* IRQ source = the PSX I_STAT bit index. Identical on both backends (mednafen
 * irq.h uses the same numbering). */
typedef enum {
    DEV_IRQ_VBLANK  = 0,
    DEV_IRQ_GPU     = 1,
    DEV_IRQ_CDROM   = 2,
    DEV_IRQ_DMA     = 3,
    DEV_IRQ_TIMER0  = 4,
    DEV_IRQ_TIMER1  = 5,
    DEV_IRQ_TIMER2  = 6,
    DEV_IRQ_SIO0    = 7,
    DEV_IRQ_SIO1    = 8,
    DEV_IRQ_SPU     = 9,
    DEV_IRQ_PIO     = 10,
} dev_irq_source_t;

typedef struct {
    uint64_t seq;       /* monotonic event index since boot */
    uint64_t cycle;     /* absolute guest CPU cycles at the raise (the ruler) */
    uint32_t frame;     /* host video frame at the raise */
    uint32_t source;    /* dev_irq_source_t — the I_STAT bit raised */
    uint32_t detail;    /* source-specific: DMA channel / timer index / 0 */
} DevEvent;

void device_trace_arm(int on);     /* on=1 begins recording (idempotent) */
void device_trace_reset(void);     /* clear ring (keeps armed state) */
int  device_trace_is_armed(void);  /* hot-path gate: cheap one-branch check */

/* Record one device-IRQ raise edge. No-op unless armed. Stamps cycle+frame via
 * the process's guest-cycle/frame accessors (shared with parity_trace). */
void device_trace_note(uint32_t source, uint32_t detail);

/* Readback: copies up to max_rows NEWEST events, oldest-first, into out[]. */
uint32_t device_trace_get(DevEvent* out, uint32_t max_rows);
uint64_t device_trace_total(void);
const char* device_source_str(uint32_t source);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_TRACE_H */
