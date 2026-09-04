/* Portable recomp-ui setup host: disc → generate → cmake/PGO rebuild → relaunch.
 *
 * Games compile psxrecomp_codegen_host.c, fill PsxrecompCodegenHostConfig,
 * and call psxrecomp_codegen_host_apply() when building RecompLauncherCGameInfo.
 *
 * Requires recomp_launcher.h on the include path (recomp-ui submodule).
 */
#ifndef PSXRECOMP_CODEGEN_HOST_H
#define PSXRECOMP_CODEGEN_HOST_H

#include "recomp_launcher.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PsxrecompCodegenHostConfig {
    const char* display_name;

    /* Env vars (optional). Defaults: PSXRECOMP_PROJECT_ROOT / PSXRECOMP_BUILD_DIR /
     * PSXRECOMP_FORCE_SETUP when NULL. */
    const char* project_root_env;
    const char* build_dir_env;
    const char* force_setup_env;

    /* Paths relative to project root. NULL → defaults below. */
    const char* psxrecomp_cli_relpath; /* default: psxrecomp/psxrecomp_cli.py */
    const char* seed_cfg_relpath;      /* default: game.toml (root probe) */
    const char* game_toml_relpath;     /* default: game.toml */
    const char* gen_marker_relpath;    /* default: generated/SLUS_011.89_dispatch.c */
    const char* build_dir_name;        /* default: build */

    /* CMake / binary identity (required for auto-rebuild). */
    const char* cmake_target;  /* e.g. psx-runtime */
    const char* exe_basename;  /* no .exe */

    /* Optional UI copy overrides (NULL → generic defaults). */
    const char* prepare_note;
    const char* prepare_note_windows;
    const char* prepare_note_no_cmake;
} PsxrecompCodegenHostConfig;

void psxrecomp_codegen_host_apply(RecompLauncherCGameInfo* gi,
                                  const PsxrecompCodegenHostConfig* cfg);

int psxrecomp_codegen_host_sources_missing(
    const PsxrecompCodegenHostConfig* cfg);

void psxrecomp_codegen_host_relaunch_or_exit(const char* disc_path);

/* Setup-host only (no PSX_HAS_GAME_DISPATCH): if generated/ is present and
 * build-<dir>/<exe> exists, exec that binary (product tree with bios/, mods/,
 * assets/, settings) and do not return. Full builds are a no-op.
 * Skip with PSXRECOMP_NO_FORWARD=1 or the title force-setup env (=1). */
void psxrecomp_codegen_host_forward_if_built(
    const PsxrecompCodegenHostConfig* cfg, int argc, char** argv);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_CODEGEN_HOST_H */
