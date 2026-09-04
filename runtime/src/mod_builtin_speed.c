/*
 * Framework-owned loading-speed mods, available to every game.
 *
 * Loading acceleration is a property of the emulated hardware, not of any
 * particular disc, so these two live here instead of being copied into each
 * title's mods/ tree. The manifests in mods/builtin/packages target game_id
 * "*", and runtime.cmake stages them beside every game's own catalog.
 *
 * The two are deliberately independent: host pacing is safe but speeds the
 * game up during a load, while CD speed shortens loads with the game running
 * at normal speed. A speedrun route that depends on real-time behaviour during
 * a load needs the latter and must not be forced to take the former
 * (GH TombaRecomp#5).
 */
#include "mod_plugins.h"

#include <stdlib.h>
#include <string.h>

#define PKG_FAST "psx.enhancement.fast-loading"
#define PKG_CD   "psx.enhancement.cd-speed"

static long speed_option_number(const char* pkg, const char* feature,
                                const char* id, long fallback) {
    char text[32] = "";
    if (!psx_mod_option_value(pkg, feature, id, text, sizeof text) || !text[0])
        return fallback;
    char* end = NULL;
    const long value = strtol(text, &end, 10);
    if (!end || *end != '\0' || value <= 0) return fallback;
    return value;
}

static int speed_option_flag(const char* pkg, const char* feature,
                             const char* id) {
    char text[16] = "";
    return psx_mod_option_value(pkg, feature, id, text, sizeof text) &&
           strcmp(text, "true") == 0;
}

static void builtin_fast_loading_activate(void) {
    /* Uncapped is expressed to the runtime as multiplier 0. Multiplier 1 is
     * authentic pacing -- legal, so a player can park the value there without
     * disabling the whole feature. */
    const unsigned multiplier =
        speed_option_flag(PKG_FAST, "fast-loading", "uncapped")
            ? 0u
            : (unsigned)speed_option_number(PKG_FAST, "fast-loading",
                                            "multiplier", 4);
    (void)psx_mod_set_load_acceleration(multiplier, 0u);
}

static void builtin_cd_speed_activate(void) {
    if (speed_option_flag(PKG_CD, "cd-speed", "instant")) {
        /* Divisor 0 selects the bounded instant scheduler; only the budget
         * matters in that mode. */
        (void)psx_mod_set_disc_speed(
            0u, (unsigned)speed_option_number(PKG_CD, "cd-speed",
                                              "instant_budget", 32));
        return;
    }
    (void)psx_mod_set_disc_speed(
        (unsigned)speed_option_number(PKG_CD, "cd-speed", "multiplier", 4), 0u);
}

PSX_MOD_CONSTRUCTOR(psx_register_builtin_speed_plugins) {
    (void)psx_mod_register_activation_plugin(
        "psx.fast-loading", builtin_fast_loading_activate);
    (void)psx_mod_register_activation_plugin(
        "psx.cd-speed", builtin_cd_speed_activate);
}
