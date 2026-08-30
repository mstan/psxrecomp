#ifndef PSXRECOMP_MOD_SESSION_DEFAULTS_H
#define PSXRECOMP_MOD_SESSION_DEFAULTS_H

#include <cstdint>
#include <string>

/* Settings owned by the title config or launcher, temporarily overlaid by
 * activation callbacks. A lobby rematch must start from this snapshot: a
 * newly-disabled plugin has no callback with which to undo its prior session. */
struct ModSessionDefaults {
    int video_renderer = 0;
    int video_vsync = 1;
    int aspect_num = 4;
    int aspect_den = 3;
    bool adaptive_aspect = false;
    int adaptive_max_num = 16;
    int adaptive_max_den = 9;
    int auto_skip_fmv = 0;
    std::string bezel;
    int frame_interpolation = 0;
    int frame_interpolation_fps = 0;
    int frame_interpolation_blend = 0;
    std::uint32_t crtc_multiplier = 1;
    std::uint32_t crtc_period_override = 0;
};

/* Kept as a value operation so transition behavior is independently testable
 * without linking the SDL frontend. */
inline ModSessionDefaults mod_session_restore_defaults(
    const ModSessionDefaults &defaults) {
    return defaults;
}

inline void mod_session_refresh_launcher_defaults(
    ModSessionDefaults &defaults,
    const ModSessionDefaults &launcher,
    bool auto_skip_offered) {
    defaults.video_renderer = launcher.video_renderer;
    defaults.aspect_num = launcher.aspect_num;
    defaults.aspect_den = launcher.aspect_den;
    defaults.frame_interpolation = launcher.frame_interpolation;
    defaults.frame_interpolation_fps = launcher.frame_interpolation_fps;
    if (auto_skip_offered)
        defaults.auto_skip_fmv = launcher.auto_skip_fmv;
}

#endif
