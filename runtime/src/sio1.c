/*
 * sio1.c -- PS1 Serial Port (SIO1) device core, 0x1F801050-0x1F80105F.
 *
 * Pure instance implementation: injected clock (`now` parameters), injected
 * IRQ callback, link peer via psx_link.h. No runtime dependencies -- links
 * standalone into unit tests, and two instances coexist (dual-console).
 *
 * Register semantics: nocash psx-spx "Serial Port (SIO1)". Audit + known
 * simplifications: accuracy/axis4_sio1_serial.md (D1..D8).
 *
 * Model summary:
 *  - Byte-framed shifter: a character transfers atomically after char_cycles
 *    derived live from MODE/BAUD (D6). No bit-level observability.
 *  - STAT baud timer (bits 11..31) is computed lazily at read from
 *    (now - baud_anchor) mod reload -- rollback-safe, no per-cycle tick.
 *  - IRQ8: one latch (STAT.9), three enable-gated edges (RX depth, TX ready,
 *    DSR rise); cleared only by CTRL.4 ACK (D1).
 */
#include "sio1.h"

#include <stdlib.h>
#include <string.h>

struct Sio1Device {
    /* register file */
    uint16_t mode;
    uint16_t ctrl;
    uint16_t baud;
    uint16_t stat_sticky;       /* PARITY | OVERRUN | BADSTOP | IRQ latch */
    uint64_t baud_anchor;

    /* TX pipeline */
    uint8_t  tx_buf;
    uint8_t  tx_buf_full;
    uint8_t  shift_active;
    uint8_t  shift_byte;
    uint32_t shift_remaining;   /* cycles until the character hits the wire */

    /* RX FIFO */
    uint8_t  rx_fifo[SIO1_RX_FIFO_DEPTH];
    uint8_t  rx_head;
    uint8_t  rx_count;
    uint8_t  rx_last;           /* empty-FIFO read value (D5) */

    /* handshake edge tracking */
    uint8_t  dsr_prev;

    /* derived (recomputed on MODE/BAUD/RESET write + snap load, never
     * serialized) */
    uint32_t bit_cyc;
    uint32_t char_cyc;

    /* wiring */
    PsxLinkEndpoint *ep;
    Sio1IrqFn irq_fn;
    void     *irq_user;
    uint64_t  last_now;

    /* telemetry (meta section; excluded from netplay digests) */
    uint32_t tx_chars;
    uint32_t rx_chars;
    uint32_t overruns;
    uint32_t irqs;
};

/* ===== derived timing ==================================================== */

static uint32_t reload_factor(uint16_t mode) {
    switch (mode & 3u) {
    case 2:  return 16u;
    case 3:  return 64u;
    default: return 1u;   /* 0 and 1 both mean MUL1 */
    }
}

static void recompute_timing(Sio1Device *d) {
    /* psx-spx: bit period = MAX((Reload * Factor) AND NOT 1, 1). */
    uint32_t bc = ((uint32_t)d->baud * reload_factor(d->mode)) & ~1u;
    uint32_t data_bits = 5u + ((d->mode >> 2) & 3u);
    uint32_t parity = (d->mode & 0x10u) ? 1u : 0u;
    /* stop bits in half-bit units so 1.5 is exact; field 0 treated as 1 */
    static const uint32_t stop_hb[4] = { 2u, 2u, 3u, 4u };
    uint32_t halfbits = 2u * (1u + data_bits + parity) +
                        stop_hb[(d->mode >> 6) & 3u];
    if (bc == 0) bc = 1;
    d->bit_cyc = bc;
    d->char_cyc = (bc * halfbits) / 2u;
    if (d->char_cyc == 0) d->char_cyc = 1;
}

uint32_t sio1_device_bit_cycles(const Sio1Device *d)  { return d->bit_cyc; }
uint32_t sio1_device_char_cycles(const Sio1Device *d) { return d->char_cyc; }

/* ===== lifecycle ========================================================= */

static PsxLinkEndpoint *dev_ep(const Sio1Device *d) {
    return d->ep ? d->ep : psx_link_null();
}

Sio1Device *sio1_device_create(void) {
    Sio1Device *d = (Sio1Device *)calloc(1, sizeof(*d));
    if (!d) return NULL;
    sio1_device_reset(d, 0);
    return d;
}

void sio1_device_destroy(Sio1Device *d) { free(d); }

void sio1_device_attach(Sio1Device *d, PsxLinkEndpoint *ep) {
    d->ep = ep;
    d->dsr_prev = (uint8_t)(dev_ep(d)->ops->get_lines(dev_ep(d)->self) &
                            PSX_LINK_DSR);
}

void sio1_device_set_irq(Sio1Device *d, Sio1IrqFn fn, void *user) {
    d->irq_fn = fn;
    d->irq_user = user;
}

void sio1_device_reset(Sio1Device *d, uint64_t now) {
    PsxLinkEndpoint *ep = dev_ep(d);
    d->mode = 0;
    d->ctrl = 0;
    d->baud = 0;
    d->stat_sticky = 0;
    d->baud_anchor = now;
    d->tx_buf = 0;
    d->tx_buf_full = 0;
    d->shift_active = 0;
    d->shift_byte = 0;
    d->shift_remaining = 0;
    d->rx_head = 0;
    d->rx_count = 0;
    d->rx_last = 0;
    recompute_timing(d);
    /* Drop our outputs (and in-flight chars we owned) on the wire side. */
    ep->ops->set_lines(ep->self, 0);
    ep->ops->reset(ep->self);
    d->dsr_prev = (uint8_t)(ep->ops->get_lines(ep->self) & PSX_LINK_DSR);
    d->last_now = now;
}

/* ===== IRQ latch ========================================================= */

static void irq_edge(Sio1Device *d, uint32_t detail) {
    if (d->stat_sticky & SIO1_STAT_IRQ) return;   /* already latched (D1) */
    d->stat_sticky |= SIO1_STAT_IRQ;
    d->irqs++;
    if (d->irq_fn) d->irq_fn(d->irq_user, detail);
}

static uint32_t rx_irq_threshold(const Sio1Device *d) {
    return 1u << ((d->ctrl >> 8) & 3u);
}

/* ===== TX path =========================================================== */

static void tx_try_start(Sio1Device *d) {
    if (d->shift_active || !d->tx_buf_full || !(d->ctrl & SIO1_CTRL_TXEN))
        return;
    d->shift_byte = d->tx_buf;
    d->tx_buf_full = 0;
    d->shift_active = 1;
    d->shift_remaining = d->char_cyc;
    /* TXRDY1 rises (buffer free again): TX IRQ edge if enabled. */
    if (d->ctrl & SIO1_CTRL_TX_IRQ) irq_edge(d, SIO1_IRQ_DETAIL_TX);
}

static void tx_complete(Sio1Device *d, uint64_t at_cycle) {
    PsxLinkEndpoint *ep = dev_ep(d);
    ep->ops->tx(ep->self, d->shift_byte, at_cycle);
    d->tx_chars++;
    d->shift_active = 0;
    tx_try_start(d);
    if (!d->shift_active) {
        /* TXDONE rises: TX IRQ edge if enabled. */
        if (d->ctrl & SIO1_CTRL_TX_IRQ) irq_edge(d, SIO1_IRQ_DETAIL_TX);
    }
}

/* ===== RX path =========================================================== */

static void rx_push(Sio1Device *d, uint8_t b) {
    if (d->rx_count >= SIO1_RX_FIFO_DEPTH) {
        /* Overrun: keep the oldest 8, drop the NEW byte, latch STAT.4. */
        d->stat_sticky |= SIO1_STAT_OVERRUN;
        d->overruns++;
        return;
    }
    d->rx_fifo[(d->rx_head + d->rx_count) % SIO1_RX_FIFO_DEPTH] = b;
    d->rx_count++;
    d->rx_chars++;
    if ((d->ctrl & SIO1_CTRL_RX_IRQ) &&
        (uint32_t)d->rx_count == rx_irq_threshold(d))
        irq_edge(d, SIO1_IRQ_DETAIL_RX);
}

static void rx_drain(Sio1Device *d, uint64_t now) {
    PsxLinkEndpoint *ep = dev_ep(d);
    uint8_t b;
    while (ep->ops->rx(ep->self, &b, now)) {
        if (d->ctrl & SIO1_CTRL_RXEN)
            rx_push(d, b);
        /* RXEN clear: receiver disabled, char lost at the wire (D4). */
    }
}

static uint8_t rx_pop(Sio1Device *d) {
    if (d->rx_count == 0)
        return d->rx_last;               /* stale-bus stand-in (D5) */
    d->rx_last = d->rx_fifo[d->rx_head];
    d->rx_head = (uint8_t)((d->rx_head + 1u) % SIO1_RX_FIFO_DEPTH);
    d->rx_count--;
    return d->rx_last;
}

/* ===== handshake lines =================================================== */

static void lines_publish(Sio1Device *d) {
    PsxLinkEndpoint *ep = dev_ep(d);
    uint32_t out = 0;
    if (d->ctrl & SIO1_CTRL_DTR) out |= PSX_LINK_DTR;
    if (d->ctrl & SIO1_CTRL_RTS) out |= PSX_LINK_RTS;
    ep->ops->set_lines(ep->self, out);
}

static void lines_sample(Sio1Device *d) {
    PsxLinkEndpoint *ep = dev_ep(d);
    uint8_t dsr = (uint8_t)(ep->ops->get_lines(ep->self) & PSX_LINK_DSR);
    if (dsr && !d->dsr_prev && (d->ctrl & SIO1_CTRL_DSR_IRQ))
        irq_edge(d, SIO1_IRQ_DETAIL_DSR);
    d->dsr_prev = dsr;
}

/* ===== advance / event distance ========================================= */

void sio1_device_advance(Sio1Device *d, uint32_t cycles, uint64_t now) {
    uint64_t at = now - cycles;
    /* Complete as many shifts as this window covers (chunking normally lands
     * exactly on the boundary; be robust to bigger jumps). */
    while (d->shift_active && cycles >= d->shift_remaining) {
        at += d->shift_remaining;
        cycles -= d->shift_remaining;
        d->shift_remaining = 0;
        tx_complete(d, at);
    }
    if (d->shift_active)
        d->shift_remaining -= cycles;
    rx_drain(d, now);
    lines_sample(d);
    d->last_now = now;
}

uint32_t sio1_device_cycles_to_irq(const Sio1Device *d, uint32_t i_mask) {
    uint32_t best = 0xFFFFFFFFu;
    uint64_t due;
    if (!d) return best;
    if (!(i_mask & (1u << 8))) return best;     /* IRQ_SIO1 masked */
    if (d->shift_active && d->shift_remaining < best)
        best = d->shift_remaining;
    {
        PsxLinkEndpoint *ep = dev_ep(d);
        if (ep->ops->rx_peek(ep->self, &due)) {
            uint32_t dt = (due > d->last_now)
                              ? (uint32_t)(due - d->last_now) : 0u;
            if (dt < best) best = dt;
        }
    }
    return best;
}

int sio1_device_active(const Sio1Device *d) {
    uint64_t due;
    if (!d) return 0;
    if (d->shift_active || d->tx_buf_full) return 1;
    return dev_ep((Sio1Device *)d)->ops->rx_peek(dev_ep((Sio1Device *)d)->self,
                                                 &due);
}

/* ===== STAT assembly ===================================================== */

static uint32_t stat_value(const Sio1Device *d, uint64_t now) {
    PsxLinkEndpoint *ep = dev_ep(d);
    uint32_t lines = ep->ops->get_lines(ep->self);
    uint32_t v = (uint32_t)d->stat_sticky;
    if (!d->tx_buf_full)                     v |= SIO1_STAT_TXRDY1;
    if (d->rx_count > 0)                     v |= SIO1_STAT_RXNE;
    if (!d->tx_buf_full && !d->shift_active) v |= SIO1_STAT_TXDONE;
    if (lines & PSX_LINK_DSR)                v |= SIO1_STAT_DSR;
    if (lines & PSX_LINK_CTS)                v |= SIO1_STAT_CTS;
    {
        /* Baud timer (bits 11..31): decrements from reload-1 to 0 at the
         * half-bit rate, computed lazily (D7). */
        uint32_t reload = d->bit_cyc / 2u;
        uint32_t t;
        if (reload == 0) reload = 1;
        t = (uint32_t)((reload - 1u) -
                       (uint32_t)((now - d->baud_anchor) % reload));
        v |= (t & 0x1FFFFFu) << 11;
    }
    return v;
}

uint32_t sio1_device_peek_stat(const Sio1Device *d, uint64_t now) {
    return stat_value(d, now);
}
uint16_t sio1_device_peek_mode(const Sio1Device *d) { return d->mode; }
uint16_t sio1_device_peek_ctrl(const Sio1Device *d) { return d->ctrl; }
uint16_t sio1_device_peek_baud(const Sio1Device *d) { return d->baud; }

void sio1_device_get_counters(const Sio1Device *d, uint32_t *tx_chars,
                              uint32_t *rx_chars, uint32_t *overruns,
                              uint32_t *irqs) {
    if (tx_chars) *tx_chars = d->tx_chars;
    if (rx_chars) *rx_chars = d->rx_chars;
    if (overruns) *overruns = d->overruns;
    if (irqs)     *irqs     = d->irqs;
}

/* ===== register writes =================================================== */

static void mode_write(Sio1Device *d, uint16_t v) {
    d->mode = v & 0x00FFu;
    recompute_timing(d);
}

static void baud_write(Sio1Device *d, uint16_t v, uint64_t now) {
    d->baud = v;
    d->baud_anchor = now;    /* write reloads the timer */
    recompute_timing(d);
}

static void ctrl_write(Sio1Device *d, uint16_t v, uint64_t now) {
    uint16_t old = d->ctrl;
    /* Strobe order within one write: RESET, then ACK, then store. This is
     * what makes WipEout's CTRL=0x0050 read back 0 (see the accuracy doc). */
    if (v & SIO1_CTRL_RESET) {
        sio1_device_reset(d, now);
        old = 0;
    }
    if (v & SIO1_CTRL_ACK)
        d->stat_sticky &= (uint16_t)~(SIO1_STAT_PARITY | SIO1_STAT_OVERRUN |
                                      SIO1_STAT_BADSTOP | SIO1_STAT_IRQ);
    d->ctrl = v & SIO1_CTRL_STORED_MASK;
    lines_publish(d);
    /* TXEN 0->1 starts a byte buffered while TXEN was low. */
    if (!(old & SIO1_CTRL_TXEN) && (d->ctrl & SIO1_CTRL_TXEN))
        tx_try_start(d);
    /* Arm the DSR edge detector against the current level so enabling the
     * IRQ while DSR is already high does not fire (edge, not level). */
    lines_sample(d);
}

static void txdata_write(Sio1Device *d, uint8_t b) {
    d->tx_buf = b;
    d->tx_buf_full = 1;
    tx_try_start(d);
}

/* ===== MMIO with lane decode ============================================ */

uint32_t sio1_device_read(Sio1Device *d, uint32_t addr, uint32_t width,
                          uint64_t now) {
    uint32_t base = addr & ~3u;
    uint32_t off  = addr & 3u;
    uint32_t word;

    switch (base) {
    case 0x1F801050u:
        if (off != 0) return 0;
        {
            /* One pop per access, byte replicated into the read lanes (D2). */
            uint32_t b = rx_pop(d);
            word = b | (b << 8) | (b << 16) | (b << 24);
        }
        break;
    case 0x1F801054u:
        word = stat_value(d, now);
        break;
    case 0x1F801058u:
        word = (uint32_t)d->mode | ((uint32_t)d->ctrl << 16);
        break;
    case 0x1F80105Cu:
        word = (uint32_t)d->baud << 16;
        break;
    default:
        return 0;
    }
    word >>= 8u * off;
    if (width == 1) word &= 0xFFu;
    else if (width == 2) word &= 0xFFFFu;
    return word;
}

/* Read-modify-write one halfword lane for sub-halfword stores. */
static uint16_t lane_merge16(uint16_t cur, uint32_t off_in_half,
                             uint32_t width, uint32_t value) {
    if (width >= 2) return (uint16_t)value;
    if (off_in_half == 0)
        return (uint16_t)((cur & 0xFF00u) | (value & 0xFFu));
    return (uint16_t)((cur & 0x00FFu) | ((value & 0xFFu) << 8));
}

void sio1_device_write(Sio1Device *d, uint32_t addr, uint32_t width,
                       uint32_t value, uint64_t now) {
    uint32_t base = addr & ~3u;
    uint32_t off  = addr & 3u;

    switch (base) {
    case 0x1F801050u:
        if (off != 0) return;
        txdata_write(d, (uint8_t)value);
        return;
    case 0x1F801054u:
        return;                              /* STAT is read-only */
    case 0x1F801058u:
        if (width == 4 && off == 0) {
            /* Combined store still sees MODE-before-CTRL ordering. */
            mode_write(d, (uint16_t)value);
            ctrl_write(d, (uint16_t)(value >> 16), now);
            return;
        }
        if (off < 2)
            mode_write(d, lane_merge16(d->mode, off, width, value));
        else
            ctrl_write(d, lane_merge16(d->ctrl, off - 2u, width, value), now);
        return;
    case 0x1F80105Cu:
        if (width == 4 && off == 0) {
            baud_write(d, (uint16_t)(value >> 16), now);
            return;
        }
        if (off >= 2)
            baud_write(d, lane_merge16(d->baud, off - 2u, width, value), now);
        return;                              /* 0x105C halfword is unused */
    default:
        return;
    }
}

/* ===== snapshot ========================================================== */
/* Three sections (cumulative ends via snap_section_ends):
 *   1. regs      -- mode/ctrl/baud/sticky/anchor-delta/rx_last
 *   2. fsm+wire  -- TX pipeline, RX FIFO, dsr_prev, endpoint blob ("pace";
 *                   netplay digests fold through the end of this section)
 *   3. meta      -- telemetry counters (excluded from digests)
 * Derived timing recomputes on load -- single source of truth. */

#define SIO1_SNAP_MAGIC 0x53494F31u  /* "SIO1" */

static void s_wr_u16(uint8_t **p, uint16_t v) {
    (*p)[0] = (uint8_t)v; (*p)[1] = (uint8_t)(v >> 8); *p += 2;
}
static void s_wr_u32(uint8_t **p, uint32_t v) {
    (*p)[0] = (uint8_t)v; (*p)[1] = (uint8_t)(v >> 8);
    (*p)[2] = (uint8_t)(v >> 16); (*p)[3] = (uint8_t)(v >> 24); *p += 4;
}
static void s_wr_u64(uint8_t **p, uint64_t v) {
    for (int i = 0; i < 8; i++) (*p)[i] = (uint8_t)(v >> (8 * i));
    *p += 8;
}
static uint16_t s_rd_u16(const uint8_t **p) {
    uint16_t v = (uint16_t)((*p)[0] | ((*p)[1] << 8)); *p += 2; return v;
}
static uint32_t s_rd_u32(const uint8_t **p) {
    uint32_t v = (uint32_t)(*p)[0] | ((uint32_t)(*p)[1] << 8) |
                 ((uint32_t)(*p)[2] << 16) | ((uint32_t)(*p)[3] << 24);
    *p += 4; return v;
}
static uint64_t s_rd_u64(const uint8_t **p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)(*p)[i] << (8 * i);
    *p += 8; return v;
}

#define SIO1_SNAP_REGS_BYTES (4u + 2u*3u + 2u + 8u + 1u)
#define SIO1_SNAP_FSM_FIXED  (1u+1u+1u+1u + 4u + SIO1_RX_FIFO_DEPTH + 1u+1u + 1u + 4u)
#define SIO1_SNAP_META_BYTES (4u * 4u)

static uint32_t snap_ep_bytes(Sio1Device *d) {
    PsxLinkEndpoint *ep = dev_ep(d);
    return ep->ops->snap_bytes(ep->self);
}

uint32_t sio1_device_snap_bytes(Sio1Device *d) {
    return SIO1_SNAP_REGS_BYTES + SIO1_SNAP_FSM_FIXED + snap_ep_bytes(d) +
           SIO1_SNAP_META_BYTES;
}

void sio1_device_snap_section_ends(Sio1Device *d, uint32_t out[3]) {
    out[0] = SIO1_SNAP_REGS_BYTES;
    out[1] = out[0] + SIO1_SNAP_FSM_FIXED + snap_ep_bytes(d);
    out[2] = out[1] + SIO1_SNAP_META_BYTES;
}

void sio1_device_snap_write(Sio1Device *d, uint8_t *p, uint64_t now) {
    PsxLinkEndpoint *ep = dev_ep(d);
    uint32_t ep_n = ep->ops->snap_bytes(ep->self);
    /* regs */
    s_wr_u32(&p, SIO1_SNAP_MAGIC);
    s_wr_u16(&p, d->mode);
    s_wr_u16(&p, d->ctrl);
    s_wr_u16(&p, d->baud);
    s_wr_u16(&p, d->stat_sticky);
    /* anchor as delta BACK from now (anchor <= now always) */
    s_wr_u64(&p, now - d->baud_anchor);
    *p++ = d->rx_last;
    /* fsm + wire */
    *p++ = d->tx_buf;
    *p++ = d->tx_buf_full;
    *p++ = d->shift_active;
    *p++ = d->shift_byte;
    s_wr_u32(&p, d->shift_remaining);
    memcpy(p, d->rx_fifo, SIO1_RX_FIFO_DEPTH);
    p += SIO1_RX_FIFO_DEPTH;
    *p++ = d->rx_head;
    *p++ = d->rx_count;
    *p++ = d->dsr_prev;
    s_wr_u32(&p, ep_n);
    ep->ops->snap_write(ep->self, p, now);
    p += ep_n;
    /* meta */
    s_wr_u32(&p, d->tx_chars);
    s_wr_u32(&p, d->rx_chars);
    s_wr_u32(&p, d->overruns);
    s_wr_u32(&p, d->irqs);
}

int sio1_device_snap_read(Sio1Device *d, const uint8_t *p, uint32_t len,
                          uint64_t now) {
    const uint8_t *end = p + len;
    PsxLinkEndpoint *ep = dev_ep(d);
    uint32_t ep_n;
    if (len < SIO1_SNAP_REGS_BYTES + SIO1_SNAP_FSM_FIXED + SIO1_SNAP_META_BYTES)
        return 0;
    if (s_rd_u32(&p) != SIO1_SNAP_MAGIC) return 0;
    d->mode        = s_rd_u16(&p);
    d->ctrl        = s_rd_u16(&p) & SIO1_CTRL_STORED_MASK;
    d->baud        = s_rd_u16(&p);
    d->stat_sticky = s_rd_u16(&p);
    d->baud_anchor = now - s_rd_u64(&p);
    d->rx_last     = *p++;
    d->tx_buf      = *p++;
    d->tx_buf_full = *p++ ? 1u : 0u;
    d->shift_active = *p++ ? 1u : 0u;
    d->shift_byte  = *p++;
    d->shift_remaining = s_rd_u32(&p);
    memcpy(d->rx_fifo, p, SIO1_RX_FIFO_DEPTH);
    p += SIO1_RX_FIFO_DEPTH;
    d->rx_head  = (uint8_t)(*p++ % SIO1_RX_FIFO_DEPTH);
    d->rx_count = *p++;
    if (d->rx_count > SIO1_RX_FIFO_DEPTH) return 0;
    d->dsr_prev = *p++ ? 1u : 0u;
    ep_n = s_rd_u32(&p);
    if ((size_t)(end - p) < (size_t)ep_n + SIO1_SNAP_META_BYTES) return 0;
    if (!ep->ops->snap_read(ep->self, p, ep_n, now)) return 0;
    p += ep_n;
    d->tx_chars = s_rd_u32(&p);
    d->rx_chars = s_rd_u32(&p);
    d->overruns = s_rd_u32(&p);
    d->irqs     = s_rd_u32(&p);
    recompute_timing(d);
    /* Re-publish outputs so the endpoint's line cells match CTRL after a
     * restore into a fresh crossover. */
    lines_publish(d);
    d->last_now = now;
    return p == end;
}
