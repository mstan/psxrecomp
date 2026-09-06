/*
 * Framework-owned opt-in 8 MB main RAM.
 *
 * Retail PS1 DRAM is 2 MiB mirrored four times across 0x00000000-0x007FFFFF.
 * Some enhancement patches use the upper part of that window as real RAM.
 */
#include "mod_plugins.h"

static void builtin_8mb_ram_activate(void) {
    (void)psx_mod_set_main_ram_8mb(1);
}

PSX_MOD_CONSTRUCTOR(psx_register_builtin_ram_plugins) {
    (void)psx_mod_register_activation_plugin(
        "psx.8mb-ram", builtin_8mb_ram_activate);
}
