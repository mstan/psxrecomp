/*
 * psx_link.h -- abstract serial-link endpoint for the SIO1 device.
 *
 * An endpoint is "the far side of the cable" as seen by one SIO1 block:
 * push a finished character onto the wire, poll characters whose delivery
 * cycle has arrived, and exchange the DTR/DSR + RTS/CTS handshake lines.
 *
 * ===== DETERMINISM CONTRACT (load-bearing, do not relax) =====
 * Every op is a pure function of endpoint state and the guest-cycle argument:
 *   - no wall-clock, no threads, no socket I/O inside any op;
 *   - rx/rx_peek/get_lines are idempotent for a fixed (state, cycle);
 *   - characters carry a due_cycle stamped on the GUEST timeline at tx();
 *     rx() releases an entry only once now >= due_cycle;
 *   - snapshots serialize due_cycle as a DELTA from the passed guest cycle
 *     (absolute cycles fork after a clock restore).
 * A future socket backend must ingest bytes on the netplay path at an agreed
 * guest-cycle boundary and stamp due_cycle from the agreed schedule, leaving
 * rx() a pure ring drain. This is what keeps SIO1 rollback/savestate safe.
 */
#ifndef PSX_LINK_H
#define PSX_LINK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Local outputs (set_lines). */
#define PSX_LINK_DTR (1u << 0)  /* local DTR -> peer DSR */
#define PSX_LINK_RTS (1u << 1)  /* local RTS -> peer CTS */
/* Remote inputs (get_lines). */
#define PSX_LINK_DSR (1u << 0)  /* peer DTR */
#define PSX_LINK_CTS (1u << 1)  /* peer RTS */

typedef struct PsxLinkOps {
    /* Shift one finished character onto the wire. `cycle` is the guest cycle
     * at which the transmit shift completed. Returns 1 if the wire took it,
     * 0 if it was discarded (no cable). */
    int      (*tx)(void *self, uint8_t byte, uint64_t cycle);
    /* Pop the next character with due_cycle <= `cycle`. Returns 1/0. */
    int      (*rx)(void *self, uint8_t *out, uint64_t cycle);
    /* Non-consuming: due_cycle of the oldest queued inbound character.
     * Returns 1 and writes *due, or 0 if the inbound queue is empty. */
    int      (*rx_peek)(void *self, uint64_t *due);
    void     (*set_lines)(void *self, uint32_t out_lines);   /* PSX_LINK_DTR/RTS */
    uint32_t (*get_lines)(void *self);                       /* PSX_LINK_DSR/CTS */
    int      (*connected)(void *self);
    void     (*reset)(void *self);
    /* Snapshot: inbound ring + own output lines, due_cycle as delta vs `now`. */
    uint32_t (*snap_bytes)(void *self);
    void     (*snap_write)(void *self, uint8_t *p, uint64_t now);
    int      (*snap_read)(void *self, const uint8_t *p, uint32_t len,
                          uint64_t now);
} PsxLinkOps;

typedef struct PsxLinkEndpoint {
    const PsxLinkOps *ops;
    void             *self;
} PsxLinkEndpoint;

/* Ring depth per direction (fixed; overrun at the DEVICE FIFO, not here —
 * at real link rates the wire never holds more than one char in flight). */
#define PSX_LINK_RING_DEPTH 64u

/* No cable: DSR/CTS low, tx discarded, rx never yields. */
PsxLinkEndpoint *psx_link_null(void);
/* Self-wired test cable: TX -> own RX, DTR -> own DSR, RTS -> own CTS. */
PsxLinkEndpoint *psx_link_loopback_create(void);
void             psx_link_loopback_destroy(PsxLinkEndpoint *ep);
/* Crossover cable between two SIO1 devices:
 * A.TX -> B.RX, A.DTR -> B.DSR, A.RTS -> B.CTS (and mirrored). */
void psx_link_crossover_create(PsxLinkEndpoint **a, PsxLinkEndpoint **b);
void psx_link_crossover_destroy(PsxLinkEndpoint *a, PsxLinkEndpoint *b);
/* Deterministic extra wire delay added to each character at tx(). */
void psx_link_set_latency_cycles(PsxLinkEndpoint *ep, uint32_t cycles);

#ifdef __cplusplus
}
#endif

#endif /* PSX_LINK_H */
