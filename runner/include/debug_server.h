/*
 * debug_server.h — TCP debug server for PSX recomp projects
 *
 * Non-blocking TCP server on localhost (default port 4370).
 * JSON-over-newline protocol, polled once per frame from psx_present_frame().
 * Includes a 36000-frame ring buffer for retroactive state queries.
 *
 * Modeled after nesrecomp/runner/src/debug_server.c and
 * segagenesisrecomp-v2/cmd_server.c
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Ring buffer frame record ---- */

#define FRAME_HISTORY_CAP 36000   /* ~10 min @ 60fps */

typedef struct {
    uint32_t frame_number;

    /* CPU state (32 GPRs + PC + HI + LO) */
    uint32_t gpr[32];
    uint32_t pc;
    uint32_t hi, lo;

    /* GPU display state */
    uint16_t display_area_x, display_area_y;
    int16_t  draw_offset_x, draw_offset_y;
    uint8_t  h_resolution;      /* 0=256, 1=320, 2=512, 3=640 */
    uint8_t  v_resolution;      /* 0=240, 1=480i */
    uint8_t  display_enable;    /* 0=on, 1=off */
    uint8_t  _gpu_pad;

    /* Overlay state */
    uint32_t overlay_base;      /* 0 if no overlay active */
    uint32_t overlay_pc;        /* last interpreter PC, or 0 */

    /* Input */
    uint16_t pad_buttons;       /* PS1 16-bit pad bitmask */

    /* Misc */
    uint16_t _reserved;
    uint32_t dispatch_miss_count;

    /* Game-specific (filled by game_fill_frame_record hook) */
    uint8_t  game_data[32];

    /* Last function name (recomp or interpreter) */
    char     last_func[32];
} PSXFrameRecord;

/* ---- Public API ---- */

/* Initialize the server. Call once at startup.
 * port=0 uses the default (4370). */
void debug_server_init(int port);

/* Set the GLFW window handle for the pause loop.
 * Must be called after renderer init. */
void debug_server_set_glfw_window(void *win);

/* Poll for incoming connections and commands. Non-blocking.
 * Call once per frame from psx_present_frame(). */
void debug_server_poll(void);

/* Record the current frame's state into the ring buffer.
 * Call once per frame after incrementing g_ps1_frame. */
void debug_server_record_frame(void);

/* Block while paused, polling TCP + GLFW events.
 * Call from psx_present_frame() after recording. */
void debug_server_wait_if_paused(void);

/* Graceful shutdown. Call at exit. */
void debug_server_shutdown(void);

/* Check if a TCP client is connected. */
int debug_server_is_connected(void);

/* ---- Watchpoint notifications ---- */

/* Check all watchpoints against current RAM values.
 * Sends JSON notification for any changes. */
void debug_server_check_watchpoints(void);

/* ---- Input override ---- */

/* Returns >= 0 if the debug server wants to override controller input,
 * -1 if no override is active. */
int debug_server_get_input_override(void);

/* ---- Public send helpers (for game command handlers) ---- */

/* Send a complete JSON line to the connected client. */
void debug_server_send_line(const char *json);

/* Send a formatted JSON line (printf-style) to the connected client. */
void debug_server_send_fmt(const char *fmt, ...);

/* ---- GPU state accessors (implemented in main_runner.cpp) ---- */

void     psx_debug_get_display_area(uint16_t *x, uint16_t *y);
void     psx_debug_get_draw_offset(int16_t *x, int16_t *y);
void     psx_debug_get_display_mode(uint8_t *h_res, uint8_t *v_res, uint8_t *enable);
uint32_t psx_debug_get_gpustat(void);
int      psx_debug_read_vram(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                              uint16_t *out, int max_pixels);

/* ---- Dispatch miss count (implemented in runtime.c) ---- */

uint32_t psx_get_dispatch_miss_count(void);

#ifdef __cplusplus
}
#endif
