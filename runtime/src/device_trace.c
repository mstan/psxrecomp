/* device_trace.c — general two-process device-event cycle ring.
 * See device_trace.h. Self-contained (no runtime internals) so the SAME TU
 * compiles into both psx-runtime and psx-beetle and they emit identical rows. */
#include "device_trace.h"
#include <string.h>

#define DEVTRACE_RING_CAP (1u << 20)  /* 1M events (~24 MB); covers the whole
                                       * boot->wedge window so the divergent
                                       * event is in-window, never evicted. */

static DevEvent  s_ring[DEVTRACE_RING_CAP];
static uint64_t  s_seq   = 0;
static uint32_t  s_armed = 0;

/* Guest-cycle + frame accessors — the SAME the parity ring uses, defined once
 * per host process (native: main.cpp; Beetle: beetle_libretro.cpp). Reused so
 * there is exactly one notion of "guest cycle" / "frame" per process. */
extern uint32_t parity_host_frame(void);
extern uint64_t parity_host_cycle(void);

void device_trace_arm(int on)     { s_armed = on ? 1u : 0u; }
void device_trace_reset(void)     { s_seq = 0; memset(s_ring, 0, sizeof(s_ring)); }
int  device_trace_is_armed(void)  { return (int)s_armed; }
uint64_t device_trace_total(void) { return s_seq; }

const char* device_source_str(uint32_t source)
{
    switch (source) {
        case DEV_IRQ_VBLANK: return "vblank";
        case DEV_IRQ_GPU:    return "gpu";
        case DEV_IRQ_CDROM:  return "cdrom";
        case DEV_IRQ_DMA:    return "dma";
        case DEV_IRQ_TIMER0: return "timer0";
        case DEV_IRQ_TIMER1: return "timer1";
        case DEV_IRQ_TIMER2: return "timer2";
        case DEV_IRQ_SIO0:   return "sio0";
        case DEV_IRQ_SIO1:   return "sio1";
        case DEV_IRQ_SPU:    return "spu";
        case DEV_IRQ_PIO:    return "pio";
        default:             return "?";
    }
}

void device_trace_note(uint32_t source, uint32_t detail)
{
    if (!s_armed) return;
    DevEvent* e = &s_ring[s_seq & (DEVTRACE_RING_CAP - 1u)];
    e->seq    = s_seq;
    e->cycle  = parity_host_cycle();
    e->frame  = parity_host_frame();
    e->source = source;
    e->detail = detail;
    s_seq++;
}

uint32_t device_trace_get(DevEvent* out, uint32_t max_rows)
{
    if (!out || max_rows == 0) return 0;
    uint64_t total = s_seq;
    uint32_t avail = (total < DEVTRACE_RING_CAP) ? (uint32_t)total : DEVTRACE_RING_CAP;
    uint32_t n = (avail < max_rows) ? avail : max_rows;
    uint64_t start = total - n;
    for (uint32_t i = 0; i < n; i++)
        out[i] = s_ring[(start + i) & (DEVTRACE_RING_CAP - 1u)];
    return n;
}
