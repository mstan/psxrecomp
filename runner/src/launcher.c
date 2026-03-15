/*
 * launcher.c — Disc discovery, CRC32 verification, and main() entry point.
 *
 * Responsibilities:
 *   1. Accept EXE + CUE paths from argv (backwards-compatible).
 *   2. If no positional args: check disc.cfg next to the exe for the last-used CUE path.
 *   3. If still no path: open a Windows file-picker dialog.
 *   4. Derive the EXE path from <cue_dir>/<game_get_exe_filename()>.
 *   5. CRC32-verify the file against game_get_expected_crc32() (skip if 0).
 *   6. On success: persist CUE path to disc.cfg, then call psxrecomp_runner_run().
 *
 * disc.cfg is stored in the same directory as the exe (GetModuleFileNameA).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <commdlg.h>
#  pragma comment(lib, "comdlg32.lib")
#endif

#include "game_extras.h"
#include "crc32.h"

/* Declared in main_runner.cpp */
void psxrecomp_runner_run(int argc, char **argv);

/* ---- disc.cfg helpers ---- */

/* Build path: <exe_dir>/disc.cfg */
static void get_disc_cfg_path(char *out, int max_len) {
#ifdef _WIN32
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    /* strip filename, keep trailing backslash */
    char *last_sep = strrchr(exe_path, '\\');
    if (last_sep) *(last_sep + 1) = '\0';
    snprintf(out, max_len, "%sdisc.cfg", exe_path);
#else
    /* Fallback: current directory */
    snprintf(out, max_len, "disc.cfg");
#endif
}

static void disc_cfg_read(char *path_out, int max_len) {
    char cfg_path[512];
    get_disc_cfg_path(cfg_path, sizeof(cfg_path));
    FILE *f = fopen(cfg_path, "r");
    if (!f) { path_out[0] = '\0'; return; }
    if (!fgets(path_out, max_len, f)) path_out[0] = '\0';
    fclose(f);
    /* strip trailing newline */
    int len = (int)strlen(path_out);
    while (len > 0 && (path_out[len-1] == '\n' || path_out[len-1] == '\r'))
        path_out[--len] = '\0';
}

static void disc_cfg_write(const char *cue_path) {
    char cfg_path[512];
    get_disc_cfg_path(cfg_path, sizeof(cfg_path));
    FILE *f = fopen(cfg_path, "w");
    if (!f) return;
    fprintf(f, "%s\n", cue_path);
    fclose(f);
}

/* ---- File picker ---- */

/* Returns 1 on success (path written to out), 0 on cancel. */
static int pick_disc_file(char *out, int max_len) {
#ifdef _WIN32
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    out[0] = '\0';
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = NULL;
    ofn.lpstrFilter = "PS1 Disc Images (*.cue)\0*.cue\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = out;
    ofn.nMaxFile    = (DWORD)max_len;
    ofn.lpstrTitle  = "Select PS1 Disc Image";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
#else
    fprintf(stderr, "[Launcher] No disc specified and no file picker available on this platform.\n");
    fprintf(stderr, "Usage: %s <exe> <cue>\n", "PSXRecompGame");
    return 0;
#endif
}

/* ---- CRC32 verification ---- */

static int verify_disc(const char *path, uint32_t expected_crc) {
    if (expected_crc == 0) return 1; /* skip */

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[Launcher] Cannot open '%s'\n", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    uint8_t *data = (uint8_t *)malloc((size_t)sz);
    if (!data) { fclose(f); return 0; }
    fread(data, 1, (size_t)sz, f);
    fclose(f);

    uint32_t actual = crc32_compute(data, (size_t)sz);
    free(data);

    if (actual != expected_crc) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Disc CRC32 mismatch!\n\nExpected: %08X\nGot:      %08X\n\n"
            "Please select the correct disc image.",
            expected_crc, actual);
        fprintf(stderr, "[Launcher] %s\n", msg);
#ifdef _WIN32
        MessageBoxA(NULL, msg, "Wrong Disc", MB_ICONWARNING | MB_OK);
#endif
        return 0;
    }
    return 1;
}

/* ---- Derive EXE path from CUE directory ---- */

static void derive_exe_path(const char *cue_path, char *exe_out, int max_len) {
    /* Find last separator in cue_path */
    const char *last_sep = NULL;
    for (const char *p = cue_path; *p; p++) {
        if (*p == '/' || *p == '\\') last_sep = p;
    }
    if (last_sep) {
        int dir_len = (int)(last_sep - cue_path + 1);
        snprintf(exe_out, max_len, "%.*s%s", dir_len, cue_path, game_get_exe_filename());
    } else {
        snprintf(exe_out, max_len, "%s", game_get_exe_filename());
    }
}

/* ---- Count positional (non-flag) args ---- */

static int count_positional(int argc, char **argv) {
    int count = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') count++;
        else if (!strcmp(argv[i], "--script") || !strcmp(argv[i], "--record") ||
                 !strcmp(argv[i], "--load-snapshot"))
            i++; /* skip value */
        else if (!strcmp(argv[i], "--save-snapshot"))
            i += 2; /* skip FRAME + FILE */
    }
    return count;
}

/* ---- main ---- */

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    int positional_count = count_positional(argc, argv);

    /* Backwards-compatible: both EXE and CUE given on command line */
    if (positional_count >= 2) {
        psxrecomp_runner_run(argc, argv);
        return 0;
    }

    /* New launcher flow: pick CUE, derive EXE */
    static char cue_path[512];
    static char exe_path[512];
    uint32_t expected_crc = game_get_expected_crc32();

    if (positional_count == 1) {
        /* Single positional arg — treat as CUE path */
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] != '-') {
                strncpy(cue_path, argv[i], sizeof(cue_path) - 1);
                break;
            }
        }
        if (expected_crc != 0 && !verify_disc(cue_path, expected_crc)) {
            fprintf(stderr, "[Launcher] Warning: CRC mismatch for '%s' — continuing anyway\n",
                    cue_path);
        }
    } else {
        /* No positional args — try disc.cfg, then file picker */
        disc_cfg_read(cue_path, sizeof(cue_path));

        int valid = 0;
        while (!valid) {
            if (cue_path[0] == '\0') {
                if (!pick_disc_file(cue_path, sizeof(cue_path))) {
                    fprintf(stderr, "[Launcher] No disc selected — exiting.\n");
                    return 1;
                }
            }

            /* Verify the disc */
            if (verify_disc(cue_path, expected_crc)) {
                valid = 1;
            } else {
                /* Wrong file — clear path and pick again */
                cue_path[0] = '\0';
            }
        }

        disc_cfg_write(cue_path);
        printf("[Launcher] Disc: %s\n", cue_path);
    }

    /* Derive EXE path from CUE directory */
    derive_exe_path(cue_path, exe_path, sizeof(exe_path));

    /* Verify EXE file exists */
    {
        FILE *f = fopen(exe_path, "rb");
        if (!f) {
            char msg[512];
            snprintf(msg, sizeof(msg),
                "Cannot find EXE file:\n%s\n\n"
                "Expected '%s' in the same directory as the CUE file.",
                exe_path, game_get_exe_filename());
            fprintf(stderr, "[Launcher] %s\n", msg);
#ifdef _WIN32
            MessageBoxA(NULL, msg, "Missing EXE", MB_ICONERROR | MB_OK);
#endif
            return 1;
        }
        fclose(f);
    }

    /* Build new argv: argv[0], exe_path, cue_path, then any flags from original argv */
    char *new_argv[64];
    int new_argc = 0;
    new_argv[new_argc++] = argv[0];
    new_argv[new_argc++] = exe_path;
    new_argv[new_argc++] = cue_path;
    for (int i = 1; i < argc && new_argc < 63; i++) {
        if (argv[i][0] == '-') {
            new_argv[new_argc++] = argv[i];
            /* Copy flag values */
            if ((!strcmp(argv[i], "--script") || !strcmp(argv[i], "--record") ||
                 !strcmp(argv[i], "--load-snapshot")) && i + 1 < argc) {
                new_argv[new_argc++] = argv[++i];
            } else if (!strcmp(argv[i], "--save-snapshot") && i + 2 < argc) {
                new_argv[new_argc++] = argv[++i];
                new_argv[new_argc++] = argv[++i];
            }
        }
        /* Skip positional args — we already inserted exe_path and cue_path */
    }
    new_argv[new_argc] = NULL;

    psxrecomp_runner_run(new_argc, new_argv);
    return 0;
}
