#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct SDL_Window;

/* Load assets/psxrecomp.png beside the exe (or SDL base path) and apply as
 * the window / taskbar icon. No-op when the PNG is missing. */
void psx_apply_window_icon(struct SDL_Window *window, const char *argv0);

/* The resolved path psx_apply_window_icon() would use, or "" when no icon PNG
 * was found. Exists so the launcher can carry the SAME icon as the game
 * window: recomp-ui takes a host-supplied path (GameInfo.window_icon_path)
 * rather than repeating this search, which is what stops the two from
 * drifting apart the next time the search order changes. The result is cached
 * on first call and the returned pointer stays valid for the process. */
const char *psx_window_icon_path(const char *argv0);

#ifdef __cplusplus
}
#endif
