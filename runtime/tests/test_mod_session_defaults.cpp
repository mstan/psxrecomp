#include "mod_session_defaults.h"

#include <cassert>
#include <iostream>

int main() {
    ModSessionDefaults configured;
    configured.video_renderer = 2;
    configured.video_vsync = -1;
    configured.bezel = "qirex";

    ModSessionDefaults enabled = configured;
    enabled.video_renderer = 1;
    enabled.video_vsync = 0;
    enabled.aspect_num = 21;
    enabled.aspect_den = 9;
    enabled.adaptive_aspect = true;
    enabled.adaptive_max_num = 32;
    enabled.adaptive_max_den = 9;
    enabled.auto_skip_fmv = 1;
    enabled.bezel = "auricom";
    enabled.frame_interpolation = 1;
    enabled.frame_interpolation_fps = 240;
    enabled.frame_interpolation_blend = 1;
    enabled.crtc_multiplier = 4;
    enabled.crtc_period_override = 338688;

    enabled = mod_session_restore_defaults(configured);
    assert(enabled.video_renderer == 2);
    assert(enabled.video_vsync == -1);
    assert(enabled.aspect_num == 4 && enabled.aspect_den == 3);
    assert(!enabled.adaptive_aspect);
    assert(enabled.adaptive_max_num == 16 && enabled.adaptive_max_den == 9);
    assert(enabled.auto_skip_fmv == 0);
    assert(enabled.bezel == "qirex");
    assert(enabled.frame_interpolation == 0);
    assert(enabled.frame_interpolation_fps == 0);
    assert(enabled.frame_interpolation_blend == 0);
    assert(enabled.crtc_multiplier == 1);
    assert(enabled.crtc_period_override == 0);

    ModSessionDefaults launcher = enabled;
    launcher.video_renderer = 1;
    launcher.aspect_num = 16;
    launcher.aspect_den = 9;
    launcher.frame_interpolation = 1;
    launcher.frame_interpolation_fps = 144;
    launcher.auto_skip_fmv = 1;
    launcher.video_vsync = 0;
    launcher.bezel = "pirhana";
    launcher.crtc_multiplier = 8;
    mod_session_refresh_launcher_defaults(configured, launcher,
                                          /*auto_skip_offered=*/true);
    assert(configured.video_renderer == 1);
    assert(configured.aspect_num == 16 && configured.aspect_den == 9);
    assert(configured.frame_interpolation == 1);
    assert(configured.frame_interpolation_fps == 144);
    assert(configured.auto_skip_fmv == 1);
    /* Non-launcher fields remain config-owned even when the previous plugin
     * session left different live values behind. */
    assert(configured.video_vsync == -1);
    assert(configured.bezel == "qirex");
    assert(configured.crtc_multiplier == 1);

    std::cout << "mod session default transition tests passed\n";
    return 0;
}
