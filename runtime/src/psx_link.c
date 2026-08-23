/*
 * psx_link.c -- deterministic serial-link endpoint backends for SIO1.
 *
 * Backends: null (no cable), loopback (self-wired), crossover (two SIO1
 * devices back-to-back in one process). All are pure state machines driven
 * by the guest cycle passed into each op -- see the determinism contract in
 * psx_link.h. This TU has NO runtime dependencies (links standalone into
 * unit tests).
 */
#include "psx_link.h"

#include <stdlib.h>
#include <string.h>

/* ===== shared ring ======================================================= */

typedef struct LinkRing {
    uint8_t  byte[PSX_LINK_RING_DEPTH];
    uint64_t due[PSX_LINK_RING_DEPTH];
    uint32_t head;      /* oldest entry */
    uint32_t count;
} LinkRing;

static void ring_reset(LinkRing *r) { r->head = 0; r->count = 0; }

static int ring_push(LinkRing *r, uint8_t b, uint64_t due) {
    uint32_t slot;
    if (r->count >= PSX_LINK_RING_DEPTH) return 0;   /* wire overflow: drop */
    slot = (r->head + r->count) % PSX_LINK_RING_DEPTH;
    r->byte[slot] = b;
    r->due[slot]  = due;
    r->count++;
    return 1;
}

static int ring_pop_due(LinkRing *r, uint8_t *out, uint64_t now) {
    if (r->count == 0) return 0;
    if (r->due[r->head] > now) return 0;
    *out = r->byte[r->head];
    r->head = (r->head + 1u) % PSX_LINK_RING_DEPTH;
    r->count--;
    return 1;
}

static int ring_peek_due(const LinkRing *r, uint64_t *due) {
    if (r->count == 0) return 0;
    *due = r->due[r->head];
    return 1;
}

/* Snapshot wire: u32 count, then per entry u8 byte + u64 due-delta (due -
 * now, saturated at 0 for overdue entries). */
static uint32_t ring_snap_bytes(const LinkRing *r) {
    return 4u + r->count * 9u;
}

static void wr_u32(uint8_t **p, uint32_t v) {
    (*p)[0] = (uint8_t)v; (*p)[1] = (uint8_t)(v >> 8);
    (*p)[2] = (uint8_t)(v >> 16); (*p)[3] = (uint8_t)(v >> 24);
    *p += 4;
}
static void wr_u64(uint8_t **p, uint64_t v) {
    for (int i = 0; i < 8; i++) { (*p)[i] = (uint8_t)(v >> (8 * i)); }
    *p += 8;
}
static uint32_t rd_u32(const uint8_t **p) {
    uint32_t v = (uint32_t)(*p)[0] | ((uint32_t)(*p)[1] << 8) |
                 ((uint32_t)(*p)[2] << 16) | ((uint32_t)(*p)[3] << 24);
    *p += 4;
    return v;
}
static uint64_t rd_u64(const uint8_t **p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)(*p)[i] << (8 * i);
    *p += 8;
    return v;
}

static void ring_snap_write(const LinkRing *r, uint8_t **p, uint64_t now) {
    wr_u32(p, r->count);
    for (uint32_t i = 0; i < r->count; i++) {
        uint32_t slot = (r->head + i) % PSX_LINK_RING_DEPTH;
        uint64_t delta = (r->due[slot] > now) ? (r->due[slot] - now) : 0u;
        *(*p)++ = r->byte[slot];
        wr_u64(p, delta);
    }
}

static int ring_snap_read(LinkRing *r, const uint8_t **p, const uint8_t *end,
                          uint64_t now) {
    uint32_t n;
    if ((size_t)(end - *p) < 4u) return 0;
    n = rd_u32(p);
    if (n > PSX_LINK_RING_DEPTH) return 0;
    if ((size_t)(end - *p) < (size_t)n * 9u) return 0;
    ring_reset(r);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b = *(*p)++;
        uint64_t delta = rd_u64(p);
        ring_push(r, b, now + delta);
    }
    return 1;
}

/* ===== null backend ====================================================== */

static int null_tx(void *s, uint8_t b, uint64_t c) {
    (void)s; (void)b; (void)c;
    return 0;
}
static int null_rx(void *s, uint8_t *o, uint64_t c) {
    (void)s; (void)o; (void)c;
    return 0;
}
static int null_rx_peek(void *s, uint64_t *d) {
    (void)s; (void)d;
    return 0;
}
static void null_set_lines(void *s, uint32_t l) { (void)s; (void)l; }
static uint32_t null_get_lines(void *s) { (void)s; return 0; }
static int null_connected(void *s) { (void)s; return 0; }
static void null_reset(void *s) { (void)s; }
static uint32_t null_snap_bytes(void *s) { (void)s; return 0; }
static void null_snap_write(void *s, uint8_t *p, uint64_t n) {
    (void)s; (void)p; (void)n;
}
static int null_snap_read(void *s, const uint8_t *p, uint32_t len,
                          uint64_t n) {
    (void)s; (void)p; (void)n;
    return len == 0;
}

static const PsxLinkOps s_null_ops = {
    null_tx, null_rx, null_rx_peek, null_set_lines, null_get_lines,
    null_connected, null_reset, null_snap_bytes, null_snap_write,
    null_snap_read,
};
static PsxLinkEndpoint s_null_ep = { &s_null_ops, NULL };

PsxLinkEndpoint *psx_link_null(void) { return &s_null_ep; }

/* ===== loopback backend ================================================== */

typedef struct Loopback {
    LinkRing ring;      /* own TX -> own RX */
    uint32_t out_lines;
    uint32_t latency;
    PsxLinkEndpoint ep;
} Loopback;

static int lb_tx(void *s, uint8_t b, uint64_t c) {
    Loopback *l = (Loopback *)s;
    return ring_push(&l->ring, b, c + l->latency);
}
static int lb_rx(void *s, uint8_t *o, uint64_t c) {
    return ring_pop_due(&((Loopback *)s)->ring, o, c);
}
static int lb_rx_peek(void *s, uint64_t *d) {
    return ring_peek_due(&((Loopback *)s)->ring, d);
}
static void lb_set_lines(void *s, uint32_t l) {
    ((Loopback *)s)->out_lines = l;
}
static uint32_t lb_get_lines(void *s) {
    /* Self-wired: DTR -> own DSR, RTS -> own CTS (bit layout matches). */
    return ((Loopback *)s)->out_lines;
}
static int lb_connected(void *s) { (void)s; return 1; }
static void lb_reset(void *s) {
    Loopback *l = (Loopback *)s;
    ring_reset(&l->ring);
    l->out_lines = 0;
}
static uint32_t lb_snap_bytes(void *s) {
    return 4u + ring_snap_bytes(&((Loopback *)s)->ring);
}
static void lb_snap_write(void *s, uint8_t *p, uint64_t now) {
    Loopback *l = (Loopback *)s;
    wr_u32(&p, l->out_lines);
    ring_snap_write(&l->ring, &p, now);
}
static int lb_snap_read(void *s, const uint8_t *p, uint32_t len,
                        uint64_t now) {
    Loopback *l = (Loopback *)s;
    const uint8_t *end = p + len;
    if (len < 4u) return 0;
    l->out_lines = rd_u32(&p);
    return ring_snap_read(&l->ring, &p, end, now) && p == end;
}

static const PsxLinkOps s_lb_ops = {
    lb_tx, lb_rx, lb_rx_peek, lb_set_lines, lb_get_lines,
    lb_connected, lb_reset, lb_snap_bytes, lb_snap_write, lb_snap_read,
};

PsxLinkEndpoint *psx_link_loopback_create(void) {
    Loopback *l = (Loopback *)calloc(1, sizeof(*l));
    if (!l) return NULL;
    l->ep.ops = &s_lb_ops;
    l->ep.self = l;
    return &l->ep;
}

void psx_link_loopback_destroy(PsxLinkEndpoint *ep) {
    if (ep && ep->ops == &s_lb_ops) free(ep->self);
}

/* ===== crossover backend ================================================= */

typedef struct XoverShared XoverShared;

typedef struct XoverEnd {
    XoverShared *sh;
    int          is_a;
    PsxLinkEndpoint ep;
} XoverEnd;

struct XoverShared {
    LinkRing a_to_b;
    LinkRing b_to_a;
    uint32_t a_out_lines;   /* A's DTR/RTS -> B's DSR/CTS */
    uint32_t b_out_lines;
    uint32_t latency;
    XoverEnd end_a;
    XoverEnd end_b;
};

static LinkRing *xo_in_ring(XoverEnd *e) {
    return e->is_a ? &e->sh->b_to_a : &e->sh->a_to_b;
}
static LinkRing *xo_out_ring(XoverEnd *e) {
    return e->is_a ? &e->sh->a_to_b : &e->sh->b_to_a;
}

static int xo_tx(void *s, uint8_t b, uint64_t c) {
    XoverEnd *e = (XoverEnd *)s;
    return ring_push(xo_out_ring(e), b, c + e->sh->latency);
}
static int xo_rx(void *s, uint8_t *o, uint64_t c) {
    return ring_pop_due(xo_in_ring((XoverEnd *)s), o, c);
}
static int xo_rx_peek(void *s, uint64_t *d) {
    return ring_peek_due(xo_in_ring((XoverEnd *)s), d);
}
static void xo_set_lines(void *s, uint32_t l) {
    XoverEnd *e = (XoverEnd *)s;
    if (e->is_a) e->sh->a_out_lines = l;
    else         e->sh->b_out_lines = l;
}
static uint32_t xo_get_lines(void *s) {
    XoverEnd *e = (XoverEnd *)s;
    /* Peer's DTR/RTS arrive as our DSR/CTS (bit layout matches). */
    return e->is_a ? e->sh->b_out_lines : e->sh->a_out_lines;
}
static int xo_connected(void *s) { (void)s; return 1; }
static void xo_reset(void *s) {
    XoverEnd *e = (XoverEnd *)s;
    /* A block reset drops its own outputs and any chars it had in flight. */
    ring_reset(xo_out_ring(e));
    if (e->is_a) e->sh->a_out_lines = 0;
    else         e->sh->b_out_lines = 0;
}
/* Each endpoint snapshots its INBOUND ring + its OWN output lines; the pair
 * together covers both rings and both line cells with no double-capture. */
static uint32_t xo_snap_bytes(void *s) {
    XoverEnd *e = (XoverEnd *)s;
    return 4u + ring_snap_bytes(xo_in_ring(e));
}
static void xo_snap_write(void *s, uint8_t *p, uint64_t now) {
    XoverEnd *e = (XoverEnd *)s;
    wr_u32(&p, e->is_a ? e->sh->a_out_lines : e->sh->b_out_lines);
    ring_snap_write(xo_in_ring(e), &p, now);
}
static int xo_snap_read(void *s, const uint8_t *p, uint32_t len,
                        uint64_t now) {
    XoverEnd *e = (XoverEnd *)s;
    const uint8_t *end = p + len;
    uint32_t lines;
    if (len < 4u) return 0;
    lines = rd_u32(&p);
    if (e->is_a) e->sh->a_out_lines = lines;
    else         e->sh->b_out_lines = lines;
    return ring_snap_read(xo_in_ring(e), &p, end, now) && p == end;
}

static const PsxLinkOps s_xo_ops = {
    xo_tx, xo_rx, xo_rx_peek, xo_set_lines, xo_get_lines,
    xo_connected, xo_reset, xo_snap_bytes, xo_snap_write, xo_snap_read,
};

void psx_link_crossover_create(PsxLinkEndpoint **a, PsxLinkEndpoint **b) {
    XoverShared *sh = (XoverShared *)calloc(1, sizeof(*sh));
    if (!sh) { *a = NULL; *b = NULL; return; }
    sh->end_a.sh = sh; sh->end_a.is_a = 1;
    sh->end_a.ep.ops = &s_xo_ops; sh->end_a.ep.self = &sh->end_a;
    sh->end_b.sh = sh; sh->end_b.is_a = 0;
    sh->end_b.ep.ops = &s_xo_ops; sh->end_b.ep.self = &sh->end_b;
    *a = &sh->end_a.ep;
    *b = &sh->end_b.ep;
}

void psx_link_crossover_destroy(PsxLinkEndpoint *a, PsxLinkEndpoint *b) {
    (void)b;
    if (a && a->ops == &s_xo_ops)
        free(((XoverEnd *)a->self)->sh);
}

void psx_link_set_latency_cycles(PsxLinkEndpoint *ep, uint32_t cycles) {
    if (!ep) return;
    if (ep->ops == &s_lb_ops)
        ((Loopback *)ep->self)->latency = cycles;
    else if (ep->ops == &s_xo_ops)
        ((XoverEnd *)ep->self)->sh->latency = cycles;
}
