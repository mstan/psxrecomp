/*
 * Framework-owned "skip FMVs" mod, available to every game.
 *
 * The runtime has been able to do this since auto_skip_fmv existed, but the
 * setting is deliberately inert on PSX: main.cpp holds skip_fmv_offered at
 * constexpr false and clears g_auto_skip_fmv right after config resolution,
 * with "Skip FMVs is mod-owned on PSX; ignoring the legacy Settings value".
 * That is the correct design -- the feature is a per-user choice with a real
 * behavioural effect, so it belongs in the mod plan where it can be toggled and
 * recorded, not in a config file a game ships. What was missing was the mod
 * itself, so the capability existed with no way to reach it.
 *
 * Activation runs after the final mod-plan commit, which is what makes this
 * work where the config key cannot: the guard that clears the legacy value has
 * already run by then.
 *
 * Detection is the framework's, not ours: a streaming FMV is XA audio plus MDEC
 * output, a documented signature, so nothing here is keyed to a frame count or
 * a per-title address.
 */
#include "mod_plugins.h"

/* main.cpp -- validates and applies; refuses anything but 0/1. */
extern int psx_mod_set_auto_skip_fmv(int enabled);

static void builtin_skip_fmv_activate(void) {
    (void)psx_mod_set_auto_skip_fmv(1);
}

PSX_MOD_CONSTRUCTOR(psx_register_builtin_skip_fmv_plugin) {
    (void)psx_mod_register_activation_plugin("psx.skip_fmv",
                                             builtin_skip_fmv_activate);
}
