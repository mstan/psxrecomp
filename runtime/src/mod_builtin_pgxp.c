/*
 * Framework-owned PGXP precision mod, available to every game.
 *
 * Sub-pixel vertex precision + perspective-correct texturing is a property of
 * the emulated GTE/GPU pair, not of any particular disc, so it ships here and
 * mods/builtin/packages/psx.enhancement.pgxp targets game_id "*". Default off
 * (the faithful floor, docs/ENHANCEMENTS.md G1.9); enabling arms the value-
 * propagation engine (pgxp.cpp) with the user-validated defaults — tolerance
 * clamp 0.5px, cpu-mode per the mod option.
 *
 * Coverage note: the engine reaches near-total dataflow coverage on a binary
 * compiled with the PGXP hook variant (-DPSX_PGXP=1, PSX_PGXP_VARIANT); on a
 * base-flavour binary only the always-emitted swc2 tier and the interpreters
 * feed it, so the correction is weaker there but never harmful — every vertex
 * is validated against the exact packet word before it is believed.
 */
#include "mod_plugins.h"
#include "pgxp.h"

#include <string.h>

#define PKG_PGXP "psx.enhancement.pgxp"

/* gte.cpp / gpu.c — the two correction toggles this mod arms. */
extern void gte_geometry_correction_set(int enabled);
extern void gpu_texture_correction_set(int enabled);

static int pgxp_option_flag(const char* feature, const char* id) {
    char text[16] = "";
    return psx_mod_option_value(PKG_PGXP, feature, id, text, sizeof text) &&
           strcmp(text, "true") == 0;
}

static void builtin_pgxp_activate(void) {
    gte_geometry_correction_set(1);
    gpu_texture_correction_set(1);   /* also arms the shadow engine */
    pgxp_set_cpu_mode(pgxp_option_flag("pgxp", "cpu_mode"));
}

PSX_MOD_CONSTRUCTOR(psx_register_builtin_pgxp_plugin) {
    (void)psx_mod_register_activation_plugin("psx.pgxp",
                                             builtin_pgxp_activate);
}
