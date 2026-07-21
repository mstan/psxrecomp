/* starvation_ring.c — diagnostic-only.
 *
 * Always-on ring of the last 16K SIO/MMIO events with full SIO state
 * snapshot at each event. Watchdog flushes the ring to a file and
 * aborts cleanly when host wall-clock since the last
 * debug_server_poll exceeds the threshold (TCP listener has stopped
 * draining the connection — symptom of dispatch-loop starvation).
 *
 * The dumped file is plain JSON-lines (one event per line) so it can
 * be inspected with grep/jq even after the process terminated. */

#include "starvation_ring.h"
#include "psx_cycles.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
static uint64_t host_us_now(void) {
    LARGE_INTEGER f, c;
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart * 1000000ULL) / freq.QuadPart);
}
#else
#include <sys/time.h>
static uint64_t host_us_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
#endif

extern uint32_t g_debug_current_func_addr;
extern uint32_t g_debug_last_store_pc;
extern uint32_t i_stat;
extern uint32_t i_mask;
extern int psx_get_in_exception(void);

#if STARVATION_RING_ENABLED

static StarvationEntry s_ring[STARVATION_RING_CAP];
static uint64_t        s_seq = 0;
static uint64_t        s_last_heartbeat_us = 0;
static int             s_dump_done = 0;

/* Watchdog threshold: 4 seconds of wall-clock without a heartbeat is
 * "TCP died, BIOS still running" territory. Real BIOS pad polling
 * fires VBlank ~60Hz so debug_server_poll runs at ~60Hz. 4 seconds
 * is well past any normal stall.
 *
 * Override with env PSX_STARVATION_TIMEOUT_US (microseconds; 0 disables the
 * watchdog) — needed while bringing up a renderer whose per-op submit model is
 * deliberately slow (a single heavy frame can legitimately exceed 4 s). */
#define STARVATION_TIMEOUT_US_DEFAULT (4 * 1000000ULL)
static uint64_t starvation_timeout_us(void) {
    static int resolved = 0;
    static uint64_t timeout = STARVATION_TIMEOUT_US_DEFAULT;
    if (!resolved) {
        const char *e = getenv("PSX_STARVATION_TIMEOUT_US");
        if (e && *e) timeout = strtoull(e, NULL, 10);
        resolved = 1;
    }
    return timeout;
}

void starvation_watchdog_heartbeat(void) {
    s_last_heartbeat_us = host_us_now();
}

void starvation_ring_reset(void) {
    memset(s_ring, 0, sizeof(s_ring));
    s_seq = 0;
    s_last_heartbeat_us = 0;
    s_dump_done = 0;
}

void starvation_ring_record(uint8_t kind, uint8_t tx, uint8_t rx,
                            uint16_t ctrl, uint16_t stat,
                            int shift_active, int shift_remaining,
                            int tx_buffered, int pending_ack,
                            int ack_remaining,
                            uint8_t bus_owner, uint32_t bus_byte_index,
                            uint8_t active_device, uint8_t mc_state,
                            uint8_t pad_state, uint8_t selected_slot,
                            int g_sio_timing_active) {
    StarvationEntry *e = &s_ring[s_seq & (STARVATION_RING_CAP - 1)];
    e->seq                  = s_seq++;
    e->psx_cycle_count      = psx_get_cycle_count();
    e->host_us              = host_us_now();
    e->current_func         = g_debug_current_func_addr;
    e->last_store_pc        = g_debug_last_store_pc;
    e->sio_ctrl             = ctrl;
    e->sio_stat             = stat;
    e->tx_data              = tx;
    e->rx_data              = rx;
    e->kind                 = kind;
    e->in_exception         = (uint8_t)psx_get_in_exception();
    e->shift_active         = (uint8_t)shift_active;
    e->tx_buffered          = (uint8_t)tx_buffered;
    e->pending_ack          = (uint8_t)pending_ack;
    e->bus_owner            = bus_owner;
    e->active_device        = active_device;
    e->mc_state             = mc_state;
    e->pad_state            = pad_state;
    e->selected_slot        = selected_slot;
    e->g_sio_timing_active  = (uint8_t)g_sio_timing_active;
    e->tx_rdy_visible       = (uint8_t)((stat >> 0) & 1);
    e->tx_em_visible        = (uint8_t)((stat >> 2) & 1);
    e->shift_remaining      = shift_remaining;
    e->ack_remaining        = ack_remaining;
    e->bus_byte_index       = bus_byte_index;
    e->i_stat               = i_stat;
    e->i_mask               = i_mask;
}

static const char *kind_name(uint8_t k) {
    switch (k) {
    case SR_EVT_TX_DATA_WRITE: return "TX_WRITE";
    case SR_EVT_RX_DATA_READ:  return "RX_READ";
    case SR_EVT_STAT_READ:     return "STAT_READ";
    case SR_EVT_CTRL_WRITE:    return "CTRL_WRITE";
    case SR_EVT_MODE_WRITE:    return "MODE_WRITE";
    case SR_EVT_BAUD_WRITE:    return "BAUD_WRITE";
    case SR_EVT_SHIFT_START:   return "SHIFT_START";
    case SR_EVT_BUFFER_LOAD:   return "BUFFER_LOAD";
    case SR_EVT_TX_DROPPED:    return "TX_DROPPED";
    case SR_EVT_SHIFT_DONE:    return "SHIFT_DONE";
    case SR_EVT_ACK_FIRE:      return "ACK_FIRE";
    case SR_EVT_SELECT_ASSERT: return "SELECT_ASSERT";
    case SR_EVT_SELECT_DEASS:  return "SELECT_DEASSERT";
    case SR_EVT_RESET:         return "RESET";
    case SR_EVT_PC_SAMPLE:     return "PC_SAMPLE";
    default: return "?";
    }
}

/* PC sampler. Records current PC + in_exc + i_stat/i_mask without
 * touching SIO state — used to localize busy-wait loops that don't
 * touch MMIO. Caller responsible for throttling (~every 1M cycles). */
void starvation_ring_pc_sample(void) {
    StarvationEntry *e = &s_ring[s_seq & (STARVATION_RING_CAP - 1)];
    e->seq                  = s_seq++;
    e->psx_cycle_count      = psx_get_cycle_count();
    e->host_us              = host_us_now();
    e->current_func         = g_debug_current_func_addr;
    e->last_store_pc        = g_debug_last_store_pc;
    e->sio_ctrl             = 0;
    e->sio_stat             = 0;
    e->tx_data              = 0;
    e->rx_data              = 0;
    e->kind                 = SR_EVT_PC_SAMPLE;
    e->in_exception         = (uint8_t)psx_get_in_exception();
    e->shift_active         = 0;
    e->tx_buffered          = 0;
    e->pending_ack          = 0;
    e->bus_owner            = 0;
    e->active_device        = 0;
    e->mc_state             = 0;
    e->pad_state            = 0;
    e->selected_slot        = 0;
    e->g_sio_timing_active  = 0;
    e->tx_rdy_visible       = 0;
    e->tx_em_visible        = 0;
    e->shift_remaining      = 0;
    e->ack_remaining        = 0;
    e->bus_byte_index       = 0;
    e->i_stat               = i_stat;
    e->i_mask               = i_mask;
}

uint64_t starvation_ring_total(void) { return s_seq; }

int starvation_ring_get(uint64_t seq, StarvationEntry *out) {
    if (seq >= s_seq) return 0;                                /* not written yet */
    if (s_seq - seq > STARVATION_RING_CAP) return 0;           /* evicted */
    *out = s_ring[seq & (STARVATION_RING_CAP - 1)];
    if (out->seq != seq) return 0;                             /* raced an eviction */
    return 1;
}

void starvation_ring_dump(const char *path) {
    if (s_dump_done) return;  /* dump exactly once */
    s_dump_done = 1;
    const char *file = path ? path : "starvation_dump.jsonl";
    FILE *f = fopen(file, "w");
    if (!f) return;
    uint64_t total = s_seq;
    uint64_t avail = total < STARVATION_RING_CAP ? total : STARVATION_RING_CAP;
    fprintf(f, "{\"meta\":{\"total\":%llu,\"shown\":%llu,"
            "\"last_heartbeat_us\":%llu,\"now_us\":%llu,"
            "\"psx_cycle_count\":%llu,"
            "\"current_func\":\"0x%08X\",\"last_store_pc\":\"0x%08X\","
            "\"in_exception\":%u,\"i_stat\":\"0x%08X\",\"i_mask\":\"0x%08X\"}}\n",
            (unsigned long long)total, (unsigned long long)avail,
            (unsigned long long)s_last_heartbeat_us,
            (unsigned long long)host_us_now(),
            (unsigned long long)psx_get_cycle_count(),
            g_debug_current_func_addr, g_debug_last_store_pc,
            (unsigned)psx_get_in_exception(),
            i_stat, i_mask);
    uint64_t start = total > avail ? total - avail : 0;
    for (uint64_t i = start; i < total; i++) {
        StarvationEntry *e = &s_ring[i & (STARVATION_RING_CAP - 1)];
        fprintf(f,
                "{\"seq\":%llu,\"kind\":\"%s\",\"cyc\":%llu,\"us\":%llu,"
                "\"func\":\"0x%08X\",\"pc\":\"0x%08X\","
                "\"ctrl\":\"0x%04X\",\"stat\":\"0x%04X\","
                "\"tx\":\"0x%02X\",\"rx\":\"0x%02X\","
                "\"in_exc\":%u,\"shift_act\":%u,\"shift_rem\":%d,"
                "\"buf\":%u,\"ack_pend\":%u,\"ack_rem\":%d,"
                "\"owner\":%u,\"bbidx\":%u,"
                "\"dev\":%u,\"mc\":%u,\"pad\":%u,\"slot\":%u,"
                "\"gact\":%u,\"tx_rdy\":%u,\"tx_em\":%u,"
                "\"i_stat\":\"0x%08X\",\"i_mask\":\"0x%08X\"}\n",
                (unsigned long long)e->seq, kind_name(e->kind),
                (unsigned long long)e->psx_cycle_count,
                (unsigned long long)e->host_us,
                e->current_func, e->last_store_pc,
                e->sio_ctrl, e->sio_stat,
                e->tx_data, e->rx_data,
                e->in_exception, e->shift_active, e->shift_remaining,
                e->tx_buffered, e->pending_ack, e->ack_remaining,
                e->bus_owner, e->bus_byte_index,
                e->active_device, e->mc_state, e->pad_state, e->selected_slot,
                e->g_sio_timing_active, e->tx_rdy_visible, e->tx_em_visible,
                e->i_stat, e->i_mask);
    }
    fclose(f);
}

void starvation_watchdog_check(void) {
    if (s_dump_done) return;
    if (s_last_heartbeat_us == 0) return;  /* not initialized yet */
    uint64_t timeout = starvation_timeout_us();
    if (timeout == 0) return;              /* watchdog disabled (renderer bring-up) */
    uint64_t now = host_us_now();
    if (now - s_last_heartbeat_us > timeout) {
        starvation_ring_dump(NULL);
        /* Abort cleanly so the dump file is preserved and the user knows
         * the runtime starved. */
        fprintf(stderr, "starvation_watchdog: %llu us without heartbeat — "
                "ring dumped to starvation_dump.jsonl, aborting\n",
                (unsigned long long)(now - s_last_heartbeat_us));
        fflush(stderr);
        exit(2);
    }
}

#else

void starvation_ring_record(uint8_t kind, uint8_t tx, uint8_t rx,
                            uint16_t ctrl, uint16_t stat,
                            int shift_active, int shift_remaining,
                            int tx_buffered, int pending_ack,
                            int ack_remaining,
                            uint8_t bus_owner, uint32_t bus_byte_index,
                            uint8_t active_device, uint8_t mc_state,
                            uint8_t pad_state, uint8_t selected_slot,
                            int g_sio_timing_active) {
    (void)kind; (void)tx; (void)rx; (void)ctrl; (void)stat;
    (void)shift_active; (void)shift_remaining;
    (void)tx_buffered; (void)pending_ack; (void)ack_remaining;
    (void)bus_owner; (void)bus_byte_index;
    (void)active_device; (void)mc_state; (void)pad_state;
    (void)selected_slot; (void)g_sio_timing_active;
}
void starvation_watchdog_heartbeat(void) {}
void starvation_watchdog_check(void) {}
void starvation_ring_dump(const char *path) { (void)path; }
void starvation_ring_reset(void) {}
void starvation_ring_pc_sample(void) {}
uint64_t starvation_ring_total(void) { return 0; }
int starvation_ring_get(uint64_t seq, StarvationEntry *out) { (void)seq; (void)out; return 0; }

#endif /* STARVATION_RING_ENABLED */
