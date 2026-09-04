/*
 * Framework-owned bezel presentation mod.
 *
 * The renderer primitive is generic, but artwork is an owner-selected resource.
 * A trusted plugin reads the path committed by the launcher and asks the runtime
 * to draw it behind the game image. With no selected path, the historical black
 * margins remain unchanged.
 */
#include "mod_plugins.h"

#include <stdio.h>

static void builtin_bezel_activate(void) {
    char path[1024] = "";
    if (!psx_mod_current_resource_path("artwork", path, sizeof path)) {
        fprintf(stdout, "psxrecomp: bezel artwork enabled with no image selected\n");
        return;
    }
    (void)psx_mod_set_bezel_artwork(path);
}

PSX_MOD_CONSTRUCTOR(psx_register_builtin_bezel_plugin) {
    (void)psx_mod_register_activation_plugin("psx.bezel",
                                             builtin_bezel_activate);
}
