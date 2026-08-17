/*
 * Framework-owned 8 MB main RAM, available to every game.
 *
 * Retail PS1 DRAM is 2 MiB mirrored 4× across 0x00000000–0x007FFFFF. Some
 * homebrew / enhancement patches park heaps above 2 MB (Wipeout 3 enhanced,
 * TM4 later). This opt-in enables 8 MB capacity and registers the full high
 * window as unique DRAM. Instruction PCs on still-aliased pages fold to the
 * low 2 MiB; unique high code runs via dirty-RAM until overlay-AOT covers it.
 * Default is off. Both netplay peers must match.
 */
#include "mod_plugins.h"

static void builtin_8mb_ram_activate(void) {
    (void)psx_mod_set_main_ram_8mb(1);
}

PSX_MOD_CONSTRUCTOR(psx_register_builtin_ram_plugins) {
    (void)psx_mod_register_activation_plugin(
        "psx.8mb-ram", builtin_8mb_ram_activate);
}
