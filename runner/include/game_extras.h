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

#ifdef __cplusplus
}
#endif
