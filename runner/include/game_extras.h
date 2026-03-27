#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Human-readable game name, shown in the window title */
const char *game_get_name(void);

/* PS1 address of the display thread entry function (initial value for g_display_entry) */
uint32_t game_get_display_entry(void);

/* Called once after EXE is loaded and runtime_init() completes */
void game_on_init(void);

/* Called every PS1 frame (VBlank equivalent) */
void game_on_frame(uint32_t frame_count);

/* Game-specific CLI argument handler. Returns 1 if consumed, 0 if not. */
int game_handle_arg(const char *key, const char *val);

/* One-line usage string for game-specific args, or NULL */
const char *game_arg_usage(void);

/* Filename of the headerless PS1 EXE (e.g. "SCUS_942.36_no_header").
 * The launcher looks for this file in the same directory as the CUE. */
const char *game_get_exe_filename(void);

/* Expected CRC32 of the disc image. Return 0 to skip verification. */
uint32_t game_get_expected_crc32(void);

/* Fill game-specific data in the debug frame record.
 * Called each frame from debug_server_record_frame().
 * Cast record to PSXFrameRecord* and write up to 32 bytes into game_data[]. */
void game_fill_frame_record(void *record);

/* Handle a game-specific TCP debug command.
 * Returns 1 if handled, 0 if not recognized.
 * Use debug_server_send_fmt() to send responses. */
int game_handle_debug_cmd(const char *cmd, int id, const char *json);

/* Called every frame after debug_server_record_frame + watchpoint checks.
 * For game-specific post-frame diagnostics. */
void game_post_frame(uint32_t frame_count);

#ifdef __cplusplus
}
#endif
