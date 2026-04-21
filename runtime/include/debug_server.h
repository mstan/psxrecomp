/*
 * debug_server.h -- TCP debug server for PSX recomp v4
 *
 * Non-blocking TCP server on localhost (default port 4370).
 * JSON-over-newline protocol, polled once per vblank.
 * Includes a 36000-frame ring buffer for retroactive state queries.
 *
 * Same public API names as nesrecomp/snesrecomp debug servers
 * so TCP.md and DEBUG.md documentation is reusable across projects.
 */
#ifndef PSXRECOMP_V4_DEBUG_SERVER_H
#define PSXRECOMP_V4_DEBUG_SERVER_H

#include <stdint.h>

#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Ring buffer frame record ---- */

#define FRAME_HISTORY_CAP 36000   /* ~10 min @ 60fps */
#define MAX_FRAME_DIFFS   32
#define RAM_SNAPSHOT_REGIONS 4
#define RAM_SNAPSHOT_SIZE    128

typedef struct {
    uint16_t addr;
    uint8_t  mine;
    uint8_t  theirs;
} FrameDiffEntry;

typedef struct {
    uint32_t frame_number;
    int      verify_pass;           /* 1=pass, 0=fail, -1=not checked */
    int      diff_count;

    /* ---- MIPS CPU state ---- */
    uint32_t gpr[32];
    uint32_t hi, lo;
    uint32_t cop0_sr;
    uint32_t cop0_cause;
    uint32_t cop0_epc;

    /* ---- Interrupt state ---- */
    uint32_t i_stat;
    uint32_t i_mask;

    /* ---- GPU display state ---- */
    uint16_t display_area_x, display_area_y;
    uint16_t display_w, display_h;
    uint8_t  display_disabled;

    /* ---- SIO state ---- */
    uint16_t pad_buttons;
    uint16_t sio_stat;
    uint16_t sio_ctrl;

    /* ---- Timing ---- */
    uint32_t dispatch_count;
    uint64_t total_dispatches;

    /* ---- Game-specific (filled by hook) ---- */
    uint8_t  game_data[32];

    /* ---- Last function name ---- */
    char     last_func[32];

    /* ---- Configurable RAM snapshot regions ---- */
    uint32_t snapshot_addr[RAM_SNAPSHOT_REGIONS];
    uint8_t  snapshot_data[RAM_SNAPSHOT_REGIONS][RAM_SNAPSHOT_SIZE];

    /* ---- Divergence diffs (verify mode) ---- */
    FrameDiffEntry diffs[MAX_FRAME_DIFFS];
} PSXFrameRecord;

/* ---- Public API (same names as NES version) ---- */

/* Initialize the server. Call once at startup.
 * port=0 uses the default (4370). */
void debug_server_init(int port);

/* Poll for incoming connections and commands. Non-blocking.
 * Call once per vblank. */
void debug_server_poll(void);

/* Record the current frame's state into the ring buffer.
 * Call after vblank processing. */
void debug_server_record_frame(void);

/* Block while paused, polling TCP + SDL events.
 * Call from vblank callback before frame processing. */
void debug_server_wait_if_paused(void);

/* Graceful shutdown. Call at exit. */
void debug_server_shutdown(void);

/* Check if a TCP client is connected. */
int debug_server_is_connected(void);

/* ---- Tier 1 reverse debugger: write trace ---- */

/* Current dispatch target — set by psx_dispatch() before each call.
 * Captured by the write-trace hook for function attribution. */
extern uint32_t g_debug_current_func_addr;

/* Multi-range write-trace check.  Called from memory.c write paths.
 * Iterates up to 8 configurable address ranges; records matching
 * writes into the 1M-entry ring buffer with function + frame attribution. */
void debug_server_trace_write_check(uint32_t phys, uint32_t old_val,
                                    uint32_t new_val, uint8_t width);

/* MMIO write trace — separate ring buffer for 0x1F801xxx writes. */
void debug_server_trace_mmio_write(uint32_t addr, uint32_t val, uint8_t width);

/* ---- Watchpoint notifications ---- */

void debug_server_check_watchpoints(void);

/* ---- Input override ---- */

/* Returns >= 0 if the debug server wants to override pad input,
 * -1 if no override is active. Value is PS1 16-bit button mask. */
int debug_server_get_input_override(void);

/* ---- Send helpers (for extensions) ---- */

/* Set the CPU state pointer for register queries. Call after CPU init. */
void debug_server_set_cpu(CPUState *cpu);

void debug_server_send_line(const char *json);
void debug_server_send_fmt(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_V4_DEBUG_SERVER_H */
