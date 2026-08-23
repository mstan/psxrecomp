/* Portable generate → rebuild (--no-pgo from setup) → relaunch host. */

#include "psxrecomp_codegen_host.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dirent.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <spawn.h>
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <unistd.h>
extern char** environ;
#endif

/* Forward decls — used by toolchain cache helpers before their definitions. */
static int rmtree_path(const char* path);
static int mkdir_p(const char* path);
static int cmake_path_runs(const char* cmake_path);
static int toolchain_bin_is_healthy(const char* bin);
static void heal_broken_toolchain_pointers(void);
static void clear_project_toolchain_stamp(void);
static int host_paths_same_file(const char* a, const char* b);
static void prune_old_toolchain_tags(const char* keep_pack);
#if defined(_WIN32)
static int junction_dir(const char* link_path, const char* target_path);
static int run_cmdline_wait(const char* cmdline, DWORD* out_code);
#endif

static const PsxrecompCodegenHostConfig* g_cfg;
static char g_project_root[1024];
static char g_cli_path[1100];
static char g_game_toml[1100];
static char g_python[512];
static char g_cmake[512];
static char g_build_dir[1100];
static char g_exe_path[1100];
static char g_helper_path[1100];
static char g_cmake_target[256];
static char g_exe_basename[256];
static char g_display[128];
static char g_toolchain_bin[1400];
/* Last ensure-toolchain JSONL result path (bin/); shared-cache fallback. */
static char g_cli_toolchain_bin[1400];
static int g_ready;
static int g_relaunch_is_helper;
/* Wizard BIOS pick (survives cwd-relative bios.cfg misses on Windows). */
static char g_wizard_bios[1100];
/* Set when heal removes a broken latest/ / stamp at wizard open. */
static char g_tc_repair_note[320];
/* Set when host_persist_setup receives an explicit bios_path (including ""
 * for OpenBIOS). Distinguishes intentional OpenBIOS clear from "unset". */
static int g_wizard_bios_explicit;

static const char* cfg_or(const char* v, const char* d) {
    return (v && v[0]) ? v : d;
}

static int path_is_file(const char* path) {
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

static int path_is_absolute(const char* path) {
    if (!path || !path[0]) return 0;
#if defined(_WIN32)
    if (path[0] == '/' || path[0] == '\\') return 1;
    if (path[0] && path[1] == ':') return 1;
    return 0;
#else
    return path[0] == '/';
#endif
}

static int join_path(char* out, size_t cap, const char* a, const char* b);
static int dirname_copy(char* out, size_t cap, const char* path);
static int absolutize_existing_file(char* out, size_t cap, const char* path);

static int path_is_dir(const char* path) {
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static int join_path(char* out, size_t cap, const char* a, const char* b) {
    size_t na = strlen(a);
    int need_slash = na > 0 && a[na - 1] != '/' && a[na - 1] != '\\';
    int n = snprintf(out, cap, "%s%s%s", a, need_slash ? "/" : "", b);
    return n > 0 && (size_t)n < cap;
}

#if defined(_WIN32)
/* Microsoft Store Python redirects %LOCALAPPDATA% writes into
 * Packages\PythonSoftwareFoundation.Python.*\LocalCache\Local\...
 * Map a virtual LocalAppData path to those on-disk mirrors. */
static int store_python_localcache_mirror(const char* virtual_path, char* out,
                                          size_t cap) {
    const char* local = getenv("LOCALAPPDATA");
    if (!local || !local[0] || !virtual_path || !virtual_path[0])
        return 0;
    size_t llen = strlen(local);
    if (_strnicmp(virtual_path, local, (int)llen) != 0)
        return 0;
    const char* suffix = virtual_path + llen;
    if (*suffix != '\\' && *suffix != '/')
        return 0;
    while (*suffix == '\\' || *suffix == '/')
        ++suffix;
    if (!suffix[0])
        return 0;

    char packages[1100];
    if (!join_path(packages, sizeof(packages), local, "Packages"))
        return 0;
    char pattern[1200];
    snprintf(pattern, sizeof(pattern),
             "%s\\PythonSoftwareFoundation.Python.*", packages);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    int found = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        char mirror[1400];
        int n = snprintf(mirror, sizeof(mirror),
                         "%s\\%s\\LocalCache\\Local\\%s", packages,
                         fd.cFileName, suffix);
        if (n <= 0 || (size_t)n >= sizeof(mirror))
            continue;
        DWORD attr = GetFileAttributesA(mirror);
        if (attr != INVALID_FILE_ATTRIBUTES) {
            snprintf(out, cap, "%s", mirror);
            found = 1;
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

/* Prefer the real on-disk directory (handles Store Python redirection). */
static int resolve_existing_dir(const char* path, char* out, size_t cap) {
    if (path && path[0] && path_is_dir(path)) {
        snprintf(out, cap, "%s", path);
        return 1;
    }
    char alt[1400];
    if (path && store_python_localcache_mirror(path, alt, sizeof(alt)) &&
        path_is_dir(alt)) {
        snprintf(out, cap, "%s", alt);
        return 1;
    }
    return 0;
}

static int python_path_is_store(const char* path) {
    if (!path || !path[0])
        return 0;
    char lower[1100];
    size_t n = strlen(path);
    if (n >= sizeof(lower))
        n = sizeof(lower) - 1;
    for (size_t i = 0; i < n; ++i) {
        char c = path[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    lower[n] = '\0';
    return strstr(lower, "windowsapps") != NULL ||
           strstr(lower, "pythonsoftwarefoundation") != NULL;
}

/* Run a short cmdline and capture the first stdout line (trimmed). */
static int capture_cmd_first_line(const char* cmdline, char* out, size_t cap) {
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    char mutable_cmd[1024];
    snprintf(mutable_cmd, sizeof(mutable_cmd), "%s", cmdline);
    if (!CreateProcessA(NULL, mutable_cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return 0;
    }
    CloseHandle(wr);

    char buf[512];
    size_t got = 0;
    DWORD n = 0;
    out[0] = '\0';
    while (ReadFile(rd, buf, sizeof(buf), &n, NULL) && n > 0) {
        for (DWORD i = 0; i < n; ++i) {
            char c = buf[i];
            if (c == '\r')
                continue;
            if (c == '\n') {
                out[got] = '\0';
                got = cap; /* mark complete */
                break;
            }
            if (got + 1 < cap)
                out[got++] = c;
        }
        if (got >= cap)
            break;
    }
    if (got > 0 && got < cap)
        out[got] = '\0';
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 8000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return out[0] != '\0';
}
#else
static int resolve_existing_dir(const char* path, char* out, size_t cap) {
    if (path && path[0] && path_is_dir(path)) {
        snprintf(out, cap, "%s", path);
        return 1;
    }
    return 0;
}

#endif

static int dirname_copy(char* out, size_t cap, const char* path) {
    size_t n = strlen(path);
    while (n > 0 && (path[n - 1] == '/' || path[n - 1] == '\\'))
        --n;
    while (n > 0 && path[n - 1] != '/' && path[n - 1] != '\\')
        --n;
    while (n > 0 && (path[n - 1] == '/' || path[n - 1] == '\\'))
        --n;
    if (n == 0) {
        if (cap < 2) return 0;
        out[0] = '.';
        out[1] = '\0';
        return 1;
    }
    if (n >= cap) return 0;
    memcpy(out, path, n);
    out[n] = '\0';
    return 1;
}

/* Resolve an existing file to an absolute path. Tries the string as-is, then
 * under g_project_root, then next to g_exe_path. Relative bios.cfg lines like
 * "bios/SCPH1001.BIN" otherwise break after relaunch when cwd is build/. */
static int absolutize_existing_file(char* out, size_t cap, const char* path) {
    char cand[1100];
    if (!out || cap < 2 || !path || !path[0]) return 0;
    out[0] = '\0';

    for (int pass = 0; pass < 3; ++pass) {
        const char* candidate = NULL;
        if (pass == 0) {
            candidate = path;
        } else if (pass == 1) {
            if (path_is_absolute(path) || !g_project_root[0]) continue;
            if (!join_path(cand, sizeof(cand), g_project_root, path)) continue;
            candidate = cand;
        } else {
            char dir[1100];
            if (path_is_absolute(path) || !g_exe_path[0]) continue;
            if (!dirname_copy(dir, sizeof(dir), g_exe_path)) continue;
            if (!join_path(cand, sizeof(cand), dir, path)) continue;
            candidate = cand;
        }
        if (!candidate || !path_is_file(candidate)) continue;
#if defined(_WIN32)
        {
            DWORD n = GetFullPathNameA(candidate, (DWORD)cap, out, NULL);
            if (n > 0 && n < (DWORD)cap) return 1;
        }
#else
        /* glibc _FORTIFY_SOURCE aborts if the realpath destination is smaller
         * than PATH_MAX (4096), even when the resolved path fits. Allocate. */
        {
            char* rp = realpath(candidate, NULL);
            if (rp) {
                snprintf(out, cap, "%s", rp);
                free(rp);
                return 1;
            }
        }
#endif
        if ((size_t)snprintf(out, cap, "%s", candidate) < cap) return 1;
    }
    return 0;
}

static int resolve_cli_path(const char* root, char* out, size_t cap) {
    const char* candidates[] = {
        cfg_or(g_cfg->psxrecomp_cli_relpath, "psxrecomp/psxrecomp_cli.py"),
        "psxrecomp/psxrecomp_cli.py",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (!candidates[i] || !candidates[i][0])
            continue;
        if (!join_path(out, cap, root, candidates[i]))
            continue;
        if (path_is_file(out))
            return 1;
    }
    return 0;
}

static int looks_like_project_root(const char* root) {
    char cli[1100], toml[1100];
    if (!join_path(toml, sizeof(toml), root,
                   cfg_or(g_cfg->seed_cfg_relpath, "game.toml")))
        return 0;
    if (!path_is_file(toml))
        return 0;
    return resolve_cli_path(root, cli, sizeof(cli));
}

static int find_on_path(const char* name, char* out, size_t cap) {
#if defined(_WIN32)
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "where %s >nul 2>nul", name);
    if (system(cmd) == 0) {
        snprintf(out, cap, "%s", name);
        return 1;
    }
#else
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", name);
    if (system(cmd) == 0) {
        snprintf(out, cap, "%s", name);
        return 1;
    }
#endif
    return 0;
}

/* resolve_toolchain_bin is defined later; pack python lives beside bin/. */
static int resolve_toolchain_bin(char* out, size_t cap);

/* cmake-clang-v1 1.0.6+: python-build-standalone under <pack>/python/. */
static int python_exe_under_py_root(const char* py_root, char* out, size_t cap) {
    char cand[1200];
    if (!py_root || !py_root[0] || !path_is_dir(py_root))
        return 0;
#if defined(_WIN32)
    if (join_path(cand, sizeof(cand), py_root, "python.exe") &&
        path_is_file(cand) && !python_path_is_store(cand)) {
        snprintf(out, cap, "%s", cand);
        return 1;
    }
    if (join_path(cand, sizeof(cand), py_root, "python3.exe") &&
        path_is_file(cand) && !python_path_is_store(cand)) {
        snprintf(out, cap, "%s", cand);
        return 1;
    }
#endif
    if (join_path(cand, sizeof(cand), py_root, "bin/python3") &&
        path_is_file(cand)) {
        snprintf(out, cap, "%s", cand);
        return 1;
    }
    if (join_path(cand, sizeof(cand), py_root, "bin/python") &&
        path_is_file(cand)) {
        snprintf(out, cap, "%s", cand);
        return 1;
    }
#if defined(__APPLE__)
#if defined(__aarch64__)
    if (join_path(cand, sizeof(cand), py_root,
                  "aarch64-apple-darwin/bin/python3") &&
        path_is_file(cand)) {
        snprintf(out, cap, "%s", cand);
        return 1;
    }
#else
    if (join_path(cand, sizeof(cand), py_root,
                  "x86_64-apple-darwin/bin/python3") &&
        path_is_file(cand)) {
        snprintf(out, cap, "%s", cand);
        return 1;
    }
#endif
#endif
    return 0;
}

static int find_toolchain_python(char* out, size_t cap) {
    char bin[1400], pack[1400], py_root[1400];
    if (!resolve_toolchain_bin(bin, sizeof(bin)))
        return 0;
    if (!dirname_copy(pack, sizeof(pack), bin) || !pack[0])
        return 0;
    if (!join_path(py_root, sizeof(py_root), pack, "python"))
        return 0;
    return python_exe_under_py_root(py_root, out, cap);
}

static int python_env_usable(const char* env) {
    if (!env || !env[0] || !path_is_file(env))
        return 0;
#if defined(_WIN32)
    if (python_path_is_store(env))
        return 0;
#endif
    return 1;
}

/* Prefer portable pack CPython (RetComM / cmake-clang-v1), then system. */
static int find_python(char* out, size_t cap) {
    const char* env = getenv("RETCOMM_PYTHON");
    if (python_env_usable(env)) {
        snprintf(out, cap, "%s", env);
        return 1;
    }
    env = getenv("PYTHON");
    if (python_env_usable(env)) {
        snprintf(out, cap, "%s", env);
        return 1;
    }
    if (find_toolchain_python(out, cap))
        return 1;
#if defined(_WIN32)
    /* Prefer python.org / py-launcher installs over the Microsoft Store
     * stub: Store Python redirects LocalAppData writes into LocalCache. */
    char resolved[1100];
    if (capture_cmd_first_line(
            "py -3 -c \"import sys; print(sys.executable)\"", resolved,
            sizeof(resolved)) &&
        path_is_file(resolved) && !python_path_is_store(resolved)) {
        snprintf(out, cap, "%s", resolved);
        return 1;
    }
    if (capture_cmd_first_line(
            "python -c \"import sys; print(sys.executable)\"", resolved,
            sizeof(resolved)) &&
        path_is_file(resolved) && !python_path_is_store(resolved)) {
        snprintf(out, cap, "%s", resolved);
        return 1;
    }
    const char* candidates[] = {"python.exe", "python3.exe", "py.exe"};
#else
    const char* candidates[] = {"python3", "python"};
#endif
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (!find_on_path(candidates[i], out, cap))
            continue;
#if defined(_WIN32)
        if (python_path_is_store(out))
            continue;
#endif
        return 1;
    }
    return 0;
}

static int toolchain_bin_has_cmake(const char* bin, char* out, size_t cap) {
    char cmake[1200];
#if defined(_WIN32)
    if (join_path(cmake, sizeof(cmake), bin, "cmake.exe") && path_is_file(cmake)) {
        snprintf(out, cap, "%s", bin);
        return 1;
    }
#else
    if (join_path(cmake, sizeof(cmake), bin, "cmake") && path_is_file(cmake)) {
        snprintf(out, cap, "%s", bin);
        return 1;
    }
#endif
    return 0;
}

static int resolve_toolchain_bin_under(const char* wrap, char* out, size_t cap) {
    char cand[1100], cmake[1200], root[1400];
    if (!wrap || !wrap[0])
        return 0;
    if (!resolve_existing_dir(wrap, root, sizeof(root)))
        return 0;
    wrap = root;
    if (join_path(cand, sizeof(cand), wrap, "bin") &&
        toolchain_bin_has_cmake(cand, out, cap))
        return 1;
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    char pattern[1200];
    snprintf(pattern, sizeof(pattern), "%s\\*", wrap);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    int found = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (fd.cFileName[0] == '.')
            continue;
        char nested[1100], nbin[1100];
        if (!join_path(nested, sizeof(nested), wrap, fd.cFileName))
            continue;
        if (!join_path(nbin, sizeof(nbin), nested, "bin"))
            continue;
        if (join_path(cmake, sizeof(cmake), nbin, "cmake.exe") && path_is_file(cmake)) {
            snprintf(out, cap, "%s", nbin);
            found = 1;
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
#else
    DIR* dir = opendir(wrap);
    if (!dir)
        return 0;
    int found = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        char nested[1100], nbin[1100];
        if (!join_path(nested, sizeof(nested), wrap, ent->d_name))
            continue;
        if (!path_is_dir(nested))
            continue;
        if (!join_path(nbin, sizeof(nbin), nested, "bin"))
            continue;
        if (join_path(cmake, sizeof(cmake), nbin, "cmake") && path_is_file(cmake)) {
            snprintf(out, cap, "%s", nbin);
            found = 1;
            break;
        }
    }
    closedir(dir);
    return found;
#endif
}

/* Prefer tagged installs (CLI writes latest/ / offline/) then any child. */
static int resolve_toolchain_cache_base(const char* base, char* out, size_t cap) {
    static const char* prefer[] = {"latest", "offline", NULL};
    char cand[1100];
    if (!base || !base[0])
        return 0;
    for (int i = 0; prefer[i]; ++i) {
        if (join_path(cand, sizeof(cand), base, prefer[i]) &&
            resolve_toolchain_bin_under(cand, out, cap))
            return 1;
    }
    return resolve_toolchain_bin_under(base, out, cap);
}

/* Collect …/cmake-clang-v1 cache roots (same order as toolchain_pack.py).
 * Honors RETCOMM_TOOLCHAIN_CACHE / RETCOMM_DATA_HOME; searches both XDG and
 * ~/.local/share so a pack installed by install.sh is always found. */
static int collect_toolchain_cache_bases(char bases[][1400], int max_n) {
    int n = 0;
    const char* cache = getenv("RETCOMM_TOOLCHAIN_CACHE");
    if (cache && cache[0] && n < max_n) {
        snprintf(bases[n++], 1400, "%s", cache);
    }
    const char* data = getenv("RETCOMM_DATA_HOME");
    if (data && data[0] && n < max_n) {
        if (join_path(bases[n], 1400, data, "toolchains/cmake-clang-v1"))
            ++n;
    }
#if defined(_WIN32)
    const char* local = getenv("LOCALAPPDATA");
    if (local && local[0]) {
        if (n < max_n &&
            join_path(bases[n], 1400, local, "retcomm/toolchains/cmake-clang-v1"))
            ++n;
        if (n < max_n &&
            join_path(bases[n], 1400, local,
                      "psxrecomp/toolchains/cmake-clang-v1"))
            ++n;
        /* Store Python may have written only into LocalCache mirrors. */
        for (int i = 0, lim = n; i < lim && n < max_n; ++i) {
            char mirror[1400];
            if (store_python_localcache_mirror(bases[i], mirror,
                                              sizeof(mirror)))
                snprintf(bases[n++], 1400, "%s", mirror);
        }
    }
#endif
    const char* xdg = getenv("XDG_DATA_HOME");
    const char* home = getenv("HOME");
    if (xdg && xdg[0]) {
        if (n < max_n &&
            join_path(bases[n], 1400, xdg, "retcomm/toolchains/cmake-clang-v1"))
            ++n;
        if (n < max_n &&
            join_path(bases[n], 1400, xdg,
                      "psxrecomp/toolchains/cmake-clang-v1"))
            ++n;
    }
    if (home && home[0]) {
        if (n < max_n &&
            join_path(bases[n], 1400, home,
                      ".local/share/retcomm/toolchains/cmake-clang-v1"))
            ++n;
        if (n < max_n &&
            join_path(bases[n], 1400, home,
                      ".local/share/psxrecomp/toolchains/cmake-clang-v1"))
            ++n;
    }
    /* Dedup in-place. */
    int w = 0;
    for (int i = 0; i < n; ++i) {
        int dup = 0;
        for (int j = 0; j < w; ++j) {
#if defined(_WIN32)
            if (_stricmp(bases[i], bases[j]) == 0) {
#else
            if (strcmp(bases[i], bases[j]) == 0) {
#endif
                dup = 1;
                break;
            }
        }
        if (!dup) {
            if (w != i)
                snprintf(bases[w], 1400, "%s", bases[i]);
            ++w;
        }
    }
    return w;
}

/* Preferred install root for new downloads (first retcomm cache base). */
static int preferred_toolchain_cache_root(char* out, size_t cap) {
    char bases[12][1400];
    int n = collect_toolchain_cache_bases(bases, 12);
    for (int i = 0; i < n; ++i) {
        if (strstr(bases[i], "retcomm") != NULL) {
            snprintf(out, cap, "%s", bases[i]);
            return 1;
        }
    }
    if (n < 1)
        return 0;
    snprintf(out, cap, "%s", bases[0]);
    return 1;
}

static int resolve_shared_toolchain_cache(char* out, size_t cap) {
    char bases[12][1400];
    int n = collect_toolchain_cache_bases(bases, 12);
    for (int i = 0; i < n; ++i) {
        if (resolve_toolchain_cache_base(bases[i], out, cap))
            return 1;
    }
    return 0;
}

/* CLI writes project_root/toolchain/.psxrecomp-bin with the bin path. */
static int resolve_toolchain_stamp(char* out, size_t cap) {
    char stamp[1200], line[1400], resolved[1400];
    FILE* f;
    if (!g_project_root[0])
        return 0;
    if (!join_path(stamp, sizeof(stamp), g_project_root,
                   "toolchain/.psxrecomp-bin"))
        return 0;
#if defined(_WIN32)
    {
        char alt[1400];
        if (!path_is_file(stamp) &&
            store_python_localcache_mirror(stamp, alt, sizeof(alt)) &&
            path_is_file(alt))
            snprintf(stamp, sizeof(stamp), "%s", alt);
    }
#endif
    if (!path_is_file(stamp))
        return 0;
    f = fopen(stamp, "r");
    if (!f)
        return 0;
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                 line[n - 1] == ' '))
        line[--n] = '\0';
    if (!line[0])
        return 0;
    if (resolve_existing_dir(line, resolved, sizeof(resolved)) &&
        toolchain_bin_has_cmake(resolved, out, cap))
        return 1;
    return 0;
}

static int resolve_toolchain_bin(char* out, size_t cap) {
    /* Fresh ensure-toolchain result (bin directory). */
    if (g_cli_toolchain_bin[0]) {
        char resolved[1400];
        if (resolve_existing_dir(g_cli_toolchain_bin, resolved,
                                 sizeof(resolved)) &&
            toolchain_bin_has_cmake(resolved, out, cap))
            return 1;
    }

    const char* env_keys[] = {
        "RETCOMM_TOOLCHAIN_DIR", "PSXRECOMP_TOOLCHAIN_DIR", "TOOLCHAIN_DIR",
        "BPE_TOOLCHAIN_DIR", NULL};
    for (int i = 0; env_keys[i]; ++i) {
        const char* e = getenv(env_keys[i]);
        if (e && e[0] && resolve_toolchain_bin_under(e, out, cap))
            return 1;
    }
    if (resolve_toolchain_stamp(out, cap))
        return 1;
    if (g_project_root[0]) {
        char wrap[1100];
        if (join_path(wrap, sizeof(wrap), g_project_root, "toolchain") &&
            resolve_toolchain_bin_under(wrap, out, cap))
            return 1;
    }
    /* CLI downloads land here — must match toolchain_pack.py. */
    if (resolve_shared_toolchain_cache(out, cap))
        return 1;
    return 0;
}

static void activate_toolchain_path(void) {
    char pack_root[1400];
    g_toolchain_bin[0] = '\0';
    if (!resolve_toolchain_bin(g_toolchain_bin, sizeof(g_toolchain_bin)))
        return;
    /* Pack root (parent of bin/) — Windows cmake-clang-v1 ships zlib here. */
    pack_root[0] = '\0';
    (void)dirname_copy(pack_root, sizeof(pack_root), g_toolchain_bin);

    /* Prepend pack python/ (Windows) or python/bin (Unix), then bin/. */
    char py_path_dir[1400];
    char py_exe[1400];
    py_path_dir[0] = '\0';
    py_exe[0] = '\0';
    if (pack_root[0]) {
        char py_root[1400];
        if (join_path(py_root, sizeof(py_root), pack_root, "python") &&
            python_exe_under_py_root(py_root, py_exe, sizeof(py_exe))) {
#if defined(_WIN32)
            /* PBS Windows: <pack>/python/python.exe */
            snprintf(py_path_dir, sizeof(py_path_dir), "%s", py_root);
#else
            /* PBS Unix: <pack>/python/bin/python3 */
            if (!dirname_copy(py_path_dir, sizeof(py_path_dir), py_exe))
                py_path_dir[0] = '\0';
#endif
        }
    }

    const char* old = getenv("PATH");
#if defined(_WIN32)
    char neu[8192];
    if (py_path_dir[0])
        snprintf(neu, sizeof(neu), "%s;%s%s%s", py_path_dir, g_toolchain_bin,
                 old ? ";" : "", old ? old : "");
    else
        snprintf(neu, sizeof(neu), "%s%s%s", g_toolchain_bin, old ? ";" : "",
                 old ? old : "");
    _putenv_s("PATH", neu);
    if (py_exe[0])
        _putenv_s("RETCOMM_PYTHON", py_exe);
#else
    char neu[8192];
    if (py_path_dir[0])
        snprintf(neu, sizeof(neu), "%s:%s%s%s", py_path_dir, g_toolchain_bin,
                 old ? ":" : "", old ? old : "");
    else
        snprintf(neu, sizeof(neu), "%s%s%s", g_toolchain_bin, old ? ":" : "",
                 old ? old : "");
    setenv("PATH", neu, 1);
    if (py_exe[0])
        setenv("RETCOMM_PYTHON", py_exe, 1);
#endif
    if (pack_root[0]) {
#if defined(_WIN32)
        _putenv_s("RETCOMM_TOOLCHAIN_DIR", pack_root);
        _putenv_s("ZLIB_ROOT", pack_root);
        {
            const char* prev = getenv("CMAKE_PREFIX_PATH");
            char pref[8192];
            if (prev && prev[0] && !strstr(prev, pack_root))
                snprintf(pref, sizeof(pref), "%s;%s", pack_root, prev);
            else if (!prev || !prev[0])
                snprintf(pref, sizeof(pref), "%s", pack_root);
            else
                pref[0] = '\0';
            if (pref[0])
                _putenv_s("CMAKE_PREFIX_PATH", pref);
        }
#else
        setenv("RETCOMM_TOOLCHAIN_DIR", pack_root, 1);
        setenv("ZLIB_ROOT", pack_root, 1);
        {
            const char* prev = getenv("CMAKE_PREFIX_PATH");
            char pref[8192];
            if (prev && prev[0] && !strstr(prev, pack_root))
                snprintf(pref, sizeof(pref), "%s:%s", pack_root, prev);
            else if (!prev || !prev[0])
                snprintf(pref, sizeof(pref), "%s", pack_root);
            else
                pref[0] = '\0';
            if (pref[0])
                setenv("CMAKE_PREFIX_PATH", pref, 1);
        }
        /* Linux packs 1.0.4+: bundled libxml2.so.2 for lld / thin LTO. */
        {
            char libdir[1500];
            if (join_path(libdir, sizeof(libdir), pack_root, "lib") &&
                path_is_dir(libdir)) {
                const char* prev = getenv("LD_LIBRARY_PATH");
                char llp[8192];
                if (prev && prev[0] && !strstr(prev, libdir))
                    snprintf(llp, sizeof(llp), "%s:%s", libdir, prev);
                else if (!prev || !prev[0])
                    snprintf(llp, sizeof(llp), "%s", libdir);
                else
                    llp[0] = '\0';
                if (llp[0])
                    setenv("LD_LIBRARY_PATH", llp, 1);
            }
        }
#endif
    }
}

static int find_cmake(char* out, size_t cap) {
    const char* env = getenv("CMAKE");
    if (env && env[0] && path_is_file(env)) {
        snprintf(out, cap, "%s", env);
        return 1;
    }
    char tc[1100], cand[1200];
    if (resolve_toolchain_bin(tc, sizeof(tc))) {
#if defined(_WIN32)
        if (join_path(cand, sizeof(cand), tc, "cmake.exe") && path_is_file(cand)) {
            snprintf(out, cap, "%s", cand);
            return 1;
        }
#else
        if (join_path(cand, sizeof(cand), tc, "cmake") && path_is_file(cand)) {
            snprintf(out, cap, "%s", cand);
            return 1;
        }
#endif
    }
#if defined(_WIN32)
    return find_on_path("cmake.exe", out, cap);
#else
    return find_on_path("cmake", out, cap);
#endif
}

/* Walk start and up to 10 parents for game.toml + psxrecomp_cli.py. */
static int walk_up_for_project_root(const char* start, char* out, size_t cap) {
    char cur[1024];
    if (!start || !start[0])
        return 0;
    snprintf(cur, sizeof(cur), "%s", start);
    for (int i = 0; i < 10; ++i) {
        if (looks_like_project_root(cur)) {
            snprintf(out, cap, "%s", cur);
            return 1;
        }
        char parent[1024];
        if (!dirname_copy(parent, sizeof(parent), cur))
            break;
        if (strcmp(parent, cur) == 0)
            break;
        snprintf(cur, sizeof(cur), "%s", parent);
    }
    return 0;
}

/* Directory containing this process's executable (not cwd). Dolphin / .desktop
 * launches leave cwd as $HOME — VIDEO/PGO/setup must still find the zip tree.
 * Linux: $APPIMAGE parent, else /proc/self/exe. Windows: GetModuleFileName. */
static int resolve_host_exe_dir(char* out, size_t cap) {
    char exe[1100];
    if (!out || cap < 2)
        return 0;
    out[0] = '\0';
    exe[0] = '\0';

#if defined(_WIN32)
    {
        DWORD n = GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe));
        if (n == 0 || n >= (DWORD)sizeof(exe))
            return 0;
    }
#else
    {
        const char* appimg = getenv("APPIMAGE");
        if (appimg && appimg[0] && path_is_file(appimg)) {
            snprintf(exe, sizeof(exe), "%s", appimg);
        } else {
            char* rp = realpath("/proc/self/exe", NULL);
            if (!rp)
                return 0;
            snprintf(exe, sizeof(exe), "%s", rp);
            free(rp);
        }
    }
#endif
    if (!exe[0])
        return 0;
    return dirname_copy(out, cap, exe);
}

/* Last path segment of `path` (points into `path`; not nul-trimmed copy). */
static const char* path_base_name(const char* path) {
    const char* base = path ? path : "";
    for (const char* p = base; *p; ++p) {
        if ((*p == '/' || *p == '\\') && p[1])
            base = p + 1;
    }
    return base;
}

static int name_is_releases(const char* name) {
    if (!name || !name[0]) return 0;
#if defined(_WIN32)
    return _stricmp(name, "releases") == 0;
#else
    return strcmp(name, "releases") == 0;
#endif
}

/* RetComM stages Play under apps/<title>/releases/<tag>/ while the generate
 * tree lives at apps/<title>/src/current/. Walking parents of the release dir
 * never visits that sibling — probe it explicitly. */
static int try_retcomm_src_current(const char* start, char* out, size_t cap) {
    char cur[1024];
    if (!start || !start[0] || !out || cap < 2)
        return 0;
    snprintf(cur, sizeof(cur), "%s", start);
    for (int i = 0; i < 8; ++i) {
        char parent[1024];
        if (!dirname_copy(parent, sizeof(parent), cur))
            break;

        char cand[1100];
        /* …/releases  →  …/src/current */
        if (name_is_releases(path_base_name(cur))) {
            if (join_path(cand, sizeof(cand), parent, "src/current") &&
                looks_like_project_root(cand)) {
                snprintf(out, cap, "%s", cand);
                return 1;
            }
        }
        /* …/releases/<tag>  →  …/src/current */
        if (name_is_releases(path_base_name(parent))) {
            char install[1024];
            if (dirname_copy(install, sizeof(install), parent) &&
                join_path(cand, sizeof(cand), install, "src/current") &&
                looks_like_project_root(cand)) {
                snprintf(out, cap, "%s", cand);
                return 1;
            }
        }

        if (strcmp(parent, cur) == 0)
            break;
        snprintf(cur, sizeof(cur), "%s", parent);
    }
    return 0;
}

static int discover_project_root(char* out, size_t cap) {
    const char* env_name =
        cfg_or(g_cfg->project_root_env, "PSXRECOMP_PROJECT_ROOT");
    const char* env = getenv(env_name);
    if (env && env[0] && looks_like_project_root(env)) {
        snprintf(out, cap, "%s", env);
        return 1;
    }

    /* 1) cwd (terminal launches from the unzip / repo root). */
    char start[1024];
#if defined(_WIN32)
    if (!GetCurrentDirectoryA((DWORD)sizeof(start), start))
        start[0] = '\0';
#else
    if (!getcwd(start, sizeof(start)))
        start[0] = '\0';
#endif
    if (start[0] && walk_up_for_project_root(start, out, cap))
        return 1;
    if (start[0] && try_retcomm_src_current(start, out, cap))
        return 1;

    /* 2) exe dir (GUI double-click: cwd is often $HOME / Desktop). */
    char exe_dir[1024];
    if (resolve_host_exe_dir(exe_dir, sizeof(exe_dir)) &&
        walk_up_for_project_root(exe_dir, out, cap))
        return 1;
    if (resolve_host_exe_dir(exe_dir, sizeof(exe_dir)) &&
        try_retcomm_src_current(exe_dir, out, cap))
        return 1;

    return 0;
}

static int resolve_build_paths(void) {
    const char* env_name =
        cfg_or(g_cfg->build_dir_env, "PSXRECOMP_BUILD_DIR");
    const char* env = getenv(env_name);
    if (env && env[0]) {
        snprintf(g_build_dir, sizeof(g_build_dir), "%s", env);
    } else {
        const char* names[] = {
            cfg_or(g_cfg->build_dir_name, "build"),
            "build-release",
            "build",
            "build-ci",
        };
        int found = 0;
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            if (!names[i] || !names[i][0])
                continue;
            char cand[1100];
            if (!join_path(cand, sizeof(cand), g_project_root, names[i]))
                continue;
            if (path_is_dir(cand)) {
                snprintf(g_build_dir, sizeof(g_build_dir), "%s", cand);
                found = 1;
                break;
            }
        }
        /* Setup zips ship without a pre-made build tree. Prefer the configured
         * name so rebuild can cmake -B it on first Generate & rebuild. */
        if (!found) {
            const char* prefer = cfg_or(g_cfg->build_dir_name, "build-release");
            if (!join_path(g_build_dir, sizeof(g_build_dir), g_project_root,
                           prefer))
                return 0;
        }
    }

    char exe_name[300];
#if defined(_WIN32)
    snprintf(exe_name, sizeof(exe_name), "%s.exe", g_exe_basename);
#else
    snprintf(exe_name, sizeof(exe_name), "%s", g_exe_basename);
#endif
    return join_path(g_exe_path, sizeof(g_exe_path), g_build_dir, exe_name);
}

static int bios_backends_missing(void) {
    char openbios[1100], scph[1100];
    if (!join_path(openbios, sizeof(openbios), g_project_root,
                   "psxrecomp/generated/OpenBIOS_dispatch.c"))
        return 1;
    if (!join_path(scph, sizeof(scph), g_project_root,
                   "psxrecomp/generated/SCPH1001_dispatch.c"))
        return 1;
    return !(path_is_file(openbios) || path_is_file(scph));
}

int psxrecomp_codegen_host_sources_missing(
    const PsxrecompCodegenHostConfig* cfg) {
    if (!cfg || !cfg->cmake_target || !cfg->exe_basename)
        return 0;
    g_cfg = cfg;
    if (!g_project_root[0] &&
        !discover_project_root(g_project_root, sizeof(g_project_root)))
        return 0;
    char marker[1100];
    if (!join_path(marker, sizeof(marker), g_project_root,
                   cfg_or(cfg->gen_marker_relpath,
                          "generated/SLUS_011.89_dispatch.c")))
        return 1;
    if (!path_is_file(marker))
        return 1;
    return bios_backends_missing();
}

static int read_line_file(const char* path, char* out, size_t cap) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(out, (int)cap, f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    size_t n = strlen(out);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' ||
                 out[n - 1] == ' ' || out[n - 1] == '\t'))
        out[--n] = '\0';
    return n > 0;
}

static int write_line_file(const char* path, const char* line) {
    FILE* f;
    if (!path || !path[0])
        return 0;
    if (!line || !line[0]) {
        remove(path);
        return 1;
    }
    f = fopen(path, "w");
    if (!f)
        return 0;
    fprintf(f, "%s\n", line);
    fclose(f);
    return 1;
}

/* Sidecars are loaded next to argv[0]. Setup host is often zip-root while the
 * rebuilt binary lives under build/ — write beside the game binary too. */
static void write_sidecar_near_exe(const char* near_exe, const char* name,
                                   const char* value) {
    char dir[1100], path[1200];
    if (!near_exe || !near_exe[0] || !name || !name[0])
        return;
    if (!dirname_copy(dir, sizeof(dir), near_exe))
        return;
    if (!join_path(path, sizeof(path), dir, name))
        return;
    write_line_file(path, value ? value : "");
}

/* IEEE CRC-32 (zlib / Ethernet) — SCPH-1001 identity for setup discovery. */
static uint32_t host_crc32(const unsigned char* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    size_t i, j;
    for (i = 0; i < len; ++i) {
        crc ^= data[i];
        for (j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return ~crc;
}

static int retail_bios_file_ok_c(const char* path) {
    FILE* f;
    long size;
    unsigned char* buf;
    uint32_t crc;
    if (!path || !path[0] || !path_is_file(path)) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    size = ftell(f);
    if (size != 512 * 1024) { fclose(f); return 0; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    buf = (unsigned char*)malloc((size_t)size);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    crc = host_crc32(buf, (size_t)size);
    free(buf);
    return crc == 0x37157331u; /* SCPH-1001 */
}

/* Prefer a player-supplied SCPH1001 next to the project/exe for Generate.
 * Missing → leave empty (OpenBIOS). Does not override an explicit OpenBIOS. */
static int discover_retail_bios_c(char* out, size_t cap) {
    static const char* names[] = {
        "SCPH1001.BIN", "scph1001.bin", "SCPH-1001.BIN", "scph-1001.bin",
        "SCPH1001.bin", "scph1001.BIN",
    };
    static const char* subs[] = {
        "bios", "", "system", "firmware", "psxrecomp/bios", "psxrecomp-v4/bios",
    };
    char roots[3][1100];
    int nroots = 0;
    int r, s, n;
    if (g_project_root[0]) {
        snprintf(roots[nroots], sizeof(roots[0]), "%s", g_project_root);
        ++nroots;
    }
    if (g_exe_path[0]) {
        char dir[1100];
        if (dirname_copy(dir, sizeof(dir), g_exe_path)) {
            snprintf(roots[nroots], sizeof(roots[0]), "%s", dir);
            ++nroots;
        }
    }
    for (r = 0; r < nroots; ++r) {
        char walk[1100];
        int depth;
        snprintf(walk, sizeof(walk), "%s", roots[r]);
        for (depth = 0; depth < 6 && walk[0]; ++depth) {
            for (s = 0; s < (int)(sizeof(subs) / sizeof(subs[0])); ++s) {
                char dir[1200];
                if (subs[s][0]) {
                    if (!join_path(dir, sizeof(dir), walk, subs[s])) continue;
                } else {
                    snprintf(dir, sizeof(dir), "%s", walk);
                }
                for (n = 0; n < (int)(sizeof(names) / sizeof(names[0])); ++n) {
                    char cand[1300];
                    if (!join_path(cand, sizeof(cand), dir, names[n])) continue;
                    if (!retail_bios_file_ok_c(cand)) continue;
                    if (!absolutize_existing_file(out, cap, cand))
                        snprintf(out, cap, "%s", cand);
                    return 1;
                }
            }
            {
                char parent[1100];
                if (!dirname_copy(parent, sizeof(parent), walk)) break;
                if (strcmp(parent, walk) == 0) break;
                snprintf(walk, sizeof(walk), "%s", parent);
            }
        }
    }
    out[0] = '\0';
    return 0;
}

static int bios_cfg_sidecar_exists(void) {
    char cand[1100];
    char dir[1100];
    if (g_project_root[0] &&
        join_path(cand, sizeof(cand), g_project_root, "bios.cfg") &&
        path_is_file(cand))
        return 1;
    if (g_exe_path[0] && dirname_copy(dir, sizeof(dir), g_exe_path) &&
        join_path(cand, sizeof(cand), dir, "bios.cfg") && path_is_file(cand))
        return 1;
    if (path_is_file("bios.cfg")) return 1;
    return 0;
}

static int resolve_bios_arg(char* out, size_t cap) {
    char cand[1100];
    char line[1100];
    char abs[1100];
    if (g_wizard_bios[0] && absolutize_existing_file(abs, sizeof(abs),
                                                     g_wizard_bios)) {
        snprintf(out, cap, "%s", abs);
        snprintf(g_wizard_bios, sizeof(g_wizard_bios), "%s", abs);
        return 1;
    }
    if (join_path(cand, sizeof(cand), g_project_root, "bios.cfg") &&
        read_line_file(cand, line, sizeof(line)) &&
        absolutize_existing_file(out, cap, line))
        return 1;
    if (g_exe_path[0]) {
        char dir[1100];
        if (dirname_copy(dir, sizeof(dir), g_exe_path) &&
            join_path(cand, sizeof(cand), dir, "bios.cfg") &&
            read_line_file(cand, line, sizeof(line)) &&
            absolutize_existing_file(out, cap, line))
            return 1;
    }
    if (read_line_file("bios.cfg", line, sizeof(line)) &&
        absolutize_existing_file(out, cap, line))
        return 1;
    /* bios.cfg present but empty = intentional OpenBIOS — do not rediscover. */
    if (g_wizard_bios_explicit || bios_cfg_sidecar_exists()) {
        out[0] = '\0';
        return 0;
    }
    /* No remembered pick — adopt SCPH1001 if present; else OpenBIOS (empty). */
    if (discover_retail_bios_c(out, cap)) {
        snprintf(g_wizard_bios, sizeof(g_wizard_bios), "%s", out);
        if (g_project_root[0] &&
            join_path(cand, sizeof(cand), g_project_root, "bios.cfg"))
            write_line_file(cand, out);
        write_line_file("bios.cfg", out);
        if (g_exe_path[0])
            write_sidecar_near_exe(g_exe_path, "bios.cfg", out);
        fprintf(stderr, "psxrecomp-codegen: setup adopted retail BIOS %s\n",
                out);
        return 1;
    }
    out[0] = '\0';
    return 0;
}

/* bios_path NULL = leave bios.cfg untouched; "" = clear (OpenBIOS); else write. */
static int host_persist_setup(void* ctx, const char* rom_path,
                              const char* bios_path) {
    char path[1200];
    char abs_bios[1100];
    (void)ctx;
    if (bios_path) {
        g_wizard_bios_explicit = 1;
        if (bios_path[0] &&
            absolutize_existing_file(abs_bios, sizeof(abs_bios), bios_path)) {
            snprintf(g_wizard_bios, sizeof(g_wizard_bios), "%s", abs_bios);
        } else {
            g_wizard_bios[0] = '\0';
        }
        if (g_project_root[0] &&
            join_path(path, sizeof(path), g_project_root, "bios.cfg"))
            write_line_file(path, g_wizard_bios[0] ? g_wizard_bios : "");
        write_line_file("bios.cfg", g_wizard_bios[0] ? g_wizard_bios : "");
        if (g_exe_path[0])
            write_sidecar_near_exe(g_exe_path, "bios.cfg",
                                   g_wizard_bios[0] ? g_wizard_bios : "");
        if (g_build_dir[0]) {
            char build_exe[1200];
            if (join_path(build_exe, sizeof(build_exe), g_build_dir,
                          g_exe_basename))
                write_sidecar_near_exe(build_exe, "bios.cfg",
                                       g_wizard_bios[0] ? g_wizard_bios : "");
        }
    }
    if (rom_path && rom_path[0]) {
        if (g_project_root[0] &&
            join_path(path, sizeof(path), g_project_root, "disc.cfg"))
            write_line_file(path, rom_path);
        write_line_file("disc.cfg", rom_path);
        if (g_exe_path[0])
            write_sidecar_near_exe(g_exe_path, "disc.cfg", rom_path);
        if (g_build_dir[0]) {
            char build_exe[1200];
            if (join_path(build_exe, sizeof(build_exe), g_build_dir,
                          g_exe_basename))
                write_sidecar_near_exe(build_exe, "disc.cfg", rom_path);
        }
    }
    return 0;
}

static void persist_relaunch_sidecars(const char* near_exe,
                                      const char* disc_path) {
    char bios_line[1100];
    char project_sidecar[1200];
    char abs_bios[1100];

    if (disc_path && disc_path[0]) {
        write_sidecar_near_exe(near_exe, "disc.cfg", disc_path);
        write_line_file("disc.cfg", disc_path);
        if (g_project_root[0] &&
            join_path(project_sidecar, sizeof(project_sidecar), g_project_root,
                      "disc.cfg"))
            write_line_file(project_sidecar, disc_path);
    }

    bios_line[0] = '\0';
    if (g_wizard_bios[0] &&
        absolutize_existing_file(abs_bios, sizeof(abs_bios), g_wizard_bios)) {
        snprintf(bios_line, sizeof(bios_line), "%s", abs_bios);
    } else if (g_wizard_bios_explicit) {
        /* Intentional OpenBIOS: clear sidecars — do not revive a stale
         * bios.cfg (e.g. root-owned file that remove() could not delete). */
        write_sidecar_near_exe(near_exe, "bios.cfg", "");
        write_line_file("bios.cfg", "");
        if (g_project_root[0] &&
            join_path(project_sidecar, sizeof(project_sidecar), g_project_root,
                      "bios.cfg"))
            write_line_file(project_sidecar, "");
        if (g_build_dir[0]) {
            char build_exe[1200];
            if (join_path(build_exe, sizeof(build_exe), g_build_dir,
                          g_exe_basename))
                write_sidecar_near_exe(build_exe, "bios.cfg", "");
        }
        return;
    } else {
        char line[1100];
        line[0] = '\0';
        if (!read_line_file("bios.cfg", line, sizeof(line)) &&
            g_project_root[0] &&
            join_path(project_sidecar, sizeof(project_sidecar), g_project_root,
                      "bios.cfg"))
            read_line_file(project_sidecar, line, sizeof(line));
        if (line[0])
            absolutize_existing_file(bios_line, sizeof(bios_line), line);
    }
    if (bios_line[0]) {
        snprintf(g_wizard_bios, sizeof(g_wizard_bios), "%s", bios_line);
        write_sidecar_near_exe(near_exe, "bios.cfg", bios_line);
        write_line_file("bios.cfg", bios_line);
        if (g_project_root[0] &&
            join_path(project_sidecar, sizeof(project_sidecar), g_project_root,
                      "bios.cfg"))
            write_line_file(project_sidecar, bios_line);
    }
}

static int json_get_string(const char* line, const char* key, char* out,
                           size_t out_cap) {
    char pattern[96];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char* p = strstr(line, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_cap) {
        if (*p == '\\' && p[1]) {
            ++p;
            out[i++] = *p++;
            continue;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

static int json_get_number(const char* line, const char* key, double* out) {
    char pattern[96];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* p = strstr(line, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p == ' ') ++p;
    char* end = NULL;
    double v = strtod(p, &end);
    if (end == p) return 0;
    *out = v;
    return 1;
}

static void handle_progress_line(const char* line,
                                 RecompLauncherCPrepareProgressFn on_progress,
                                 void* progress_ctx) {
    if (!line || line[0] != '{')
        return;
    char event[64] = "";
    json_get_string(line, "event", event, sizeof(event));
    /* Capture ensure-toolchain result even when UI progress is absent. */
    if (strcmp(event, "result") == 0) {
        char tb[1400], resolved[1400];
        if (json_get_string(line, "toolchain_bin", tb, sizeof(tb)) && tb[0] &&
            resolve_existing_dir(tb, resolved, sizeof(resolved))) {
            snprintf(g_cli_toolchain_bin, sizeof(g_cli_toolchain_bin), "%s",
                     resolved);
            /* Env expects pack root (parent of bin/), not bin/ itself. */
            char pack_root[1400];
            if (dirname_copy(pack_root, sizeof(pack_root), resolved) &&
                pack_root[0]) {
#if defined(_WIN32)
                _putenv_s("PSXRECOMP_TOOLCHAIN_DIR", pack_root);
#else
                setenv("PSXRECOMP_TOOLCHAIN_DIR", pack_root, 1);
#endif
            }
        }
    }
    if (!on_progress)
        return;
    if (strcmp(event, "phase") == 0) {
        char message[240] = "";
        char phase[64] = "";
        double pct = -1.0;
        json_get_string(line, "message", message, sizeof(message));
        json_get_string(line, "phase", phase, sizeof(phase));
        if (!json_get_number(line, "pct", &pct))
            pct = -1.0;
        if (!message[0] && phase[0])
            snprintf(message, sizeof(message), "%s", phase);
        on_progress(progress_ctx, (float)pct, message[0] ? message : NULL);
    } else if (strcmp(event, "log") == 0 || strcmp(event, "error") == 0) {
        char message[240] = "";
        if (json_get_string(line, "message", message, sizeof(message)))
            on_progress(progress_ctx, -1.0f, message);
    }
}

/* Last-lines capture so a failed CLI run can say WHY in the wizard, which has
 * no console: the child's stderr shares the progress pipe, JSON progress rows
 * contribute their "message", raw rows (compiler/traceback text) contribute
 * as-is, and the most recent error-looking line is appended to err_msg. This
 * is what turns "psxrecomp rebuild failed (exit 1)" into "… failed (exit 1):
 * Could NOT find OpenGL (missing: OPENGL_INCLUDE_DIR)". */
typedef struct {
    char last[480];
    char last_err[480];
} CliTail;

static int cli_tail_line_is_error(const char* s) {
    return strstr(s, "rror") != NULL || strstr(s, "ailed") != NULL ||
           strstr(s, "FAILED") != NULL || strstr(s, "Traceback") != NULL ||
           strstr(s, "fatal") != NULL || strstr(s, "Fatal") != NULL;
}

static void cli_tail_note(CliTail* t, const char* line) {
    char msg[480];
    const char* rec = line;
    int is_error_event = 0;
    if (line[0] == '{') {
        /* sdk_progress emits compact JSON: {"event":"error","message":…}. */
        is_error_event = strstr(line, "\"event\":\"error\"") != NULL;
        if (!json_get_string(line, "message", msg, sizeof(msg)))
            return; /* structured row without text (e.g. result) */
        rec = msg;
    }
    if (!rec[0])
        return;
    snprintf(t->last, sizeof(t->last), "%s", rec);
    if (is_error_event || cli_tail_line_is_error(rec))
        snprintf(t->last_err, sizeof(t->last_err), "%s", rec);
}

static void cli_fail_msg(char* err_msg, size_t err_cap, const char* fail_label,
                         long code, const CliTail* t) {
    const char* why = t->last_err[0] ? t->last_err : t->last;
    if (code == 3) {
        snprintf(err_msg, err_cap, "Disc verification failed (wrong dump).");
        return;
    }
    if (why[0])
        snprintf(err_msg, err_cap, "%s failed (exit %ld): %s", fail_label,
                 code, why);
    else
        snprintf(err_msg, err_cap, "%s failed (exit %ld).", fail_label, code);
}

#if defined(_WIN32)
static int run_cli_win(const char* cmdline,
                       RecompLauncherCPrepareProgressFn on_progress,
                       void* progress_ctx, char* err_msg, size_t err_cap,
                       const char* fail_label) {
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        snprintf(err_msg, err_cap, "CreatePipe failed.");
        return 0;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    /* stderr shares the pipe: Python tracebacks and raw tool output feed the
     * CliTail capture instead of vanishing (the wizard has no console). */
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    char mutable_cmd[4096];
    snprintf(mutable_cmd, sizeof(mutable_cmd), "%s", cmdline);
    if (!CreateProcessA(NULL, mutable_cmd, NULL, NULL, TRUE, 0, NULL,
                        g_project_root, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        snprintf(err_msg, err_cap, "Failed to spawn %s.", fail_label);
        return 0;
    }
    CloseHandle(wr);

    char buf[512];
    /* Long JSONL rows (toolchain paths under LocalAppData) need headroom. */
    char line[16384];
    size_t line_len = 0;
    DWORD n = 0;
    CliTail tail;
    memset(&tail, 0, sizeof(tail));
    while (ReadFile(rd, buf, sizeof(buf), &n, NULL) && n > 0) {
        for (DWORD i = 0; i < n; ++i) {
            char c = buf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                line[line_len] = '\0';
                handle_progress_line(line, on_progress, progress_ctx);
                cli_tail_note(&tail, line);
                line_len = 0;
                continue;
            }
            if (line_len + 1 < sizeof(line))
                line[line_len++] = c;
        }
    }
    if (line_len) {
        line[line_len] = '\0';
        handle_progress_line(line, on_progress, progress_ctx);
        cli_tail_note(&tail, line);
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code == 0) return 1;
    cli_fail_msg(err_msg, err_cap, fail_label, (long)code, &tail);
    return 0;
}
#else
static int run_cli_posix(char* const argv[],
                         RecompLauncherCPrepareProgressFn on_progress,
                         void* progress_ctx, char* err_msg, size_t err_cap,
                         const char* fail_label) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        snprintf(err_msg, err_cap, "pipe() failed: %s", strerror(errno));
        return 0;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    /* stderr shares the pipe: Python tracebacks and raw tool output feed the
     * CliTail capture instead of vanishing (the wizard has no console). */
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);

    pid_t pid = 0;
    int rc = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    if (rc != 0) {
        close(pipefd[0]);
        snprintf(err_msg, err_cap, "Failed to spawn %s: %s", fail_label,
                 strerror(rc));
        return 0;
    }

    FILE* out = fdopen(pipefd[0], "r");
    if (!out) {
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        snprintf(err_msg, err_cap, "fdopen failed.");
        return 0;
    }
    char line[16384];
    CliTail tail;
    memset(&tail, 0, sizeof(tail));
    while (fgets(line, sizeof(line), out)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        handle_progress_line(line, on_progress, progress_ctx);
        cli_tail_note(&tail, line);
    }
    fclose(out);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        snprintf(err_msg, err_cap, "waitpid failed: %s", strerror(errno));
        return 0;
    }
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    if (code == 0) return 1;
    cli_fail_msg(err_msg, err_cap, fail_label, (long)code, &tail);
    return 0;
}
#endif

/* ---- Host-native toolchain install (no Store Python AppData redirect) ---- */

static const char* k_tc_repo = "TechnicallyComputers/retcomm-toolchains";

static const char* toolchain_zip_asset_name(void) {
#if defined(_WIN32)
    return "cmake-clang-v1-windows-x64.zip";
#elif defined(__APPLE__)
    return "cmake-clang-v1-macos-universal.zip";
#else
    return "cmake-clang-v1-linux-x64.zip";
#endif
}

/* Optional floor via RETCOMM_TOOLCHAIN_MIN_VERSION. Wizard/default is empty:
 * download GitHub /releases/latest and accept any usable pack (no per-title
 * version pinning to maintain). */
static const char* toolchain_min_version(void) {
    const char* env = getenv("RETCOMM_TOOLCHAIN_MIN_VERSION");
    if (env && env[0])
        return env;
    return "";
}

/* Parse leading dotted integers from a version / tag (optional leading 'v'). */
static int version_cmp(const char* a, const char* b) {
    const char* pa = a ? a : "";
    const char* pb = b ? b : "";
    if ((pa[0] == 'v' || pa[0] == 'V') && pa[1] >= '0' && pa[1] <= '9')
        ++pa;
    if ((pb[0] == 'v' || pb[0] == 'V') && pb[1] >= '0' && pb[1] <= '9')
        ++pb;
    for (;;) {
        long va = 0, vb = 0;
        int ha = 0, hb = 0;
        while (*pa >= '0' && *pa <= '9') {
            va = va * 10 + (*pa - '0');
            ++pa;
            ha = 1;
        }
        while (*pb >= '0' && *pb <= '9') {
            vb = vb * 10 + (*pb - '0');
            ++pb;
            hb = 1;
        }
        if (!ha && !hb)
            return 0;
        if (va != vb)
            return va < vb ? -1 : 1;
        if (*pa == '.')
            ++pa;
        if (*pb == '.')
            ++pb;
        if (!ha || !hb)
            return ha ? 1 : (hb ? -1 : 0);
    }
}

static int read_pack_version(const char* pack_root, char* out, size_t cap) {
    char meta[1400], buf[4096];
    FILE* f;
    const char* p;
    size_t n;
    if (!pack_root || !pack_root[0] || !out || cap < 2)
        return 0;
    out[0] = '\0';
    if (!join_path(meta, sizeof(meta), pack_root, "retcomm-toolchain.json"))
        return 0;
    f = fopen(meta, "rb");
    if (!f)
        return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    p = strstr(buf, "\"version\"");
    if (!p)
        return 0;
    p = strchr(p + 9, '"');
    if (!p)
        return 0;
    ++p;
    {
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < cap) {
            out[i++] = *p++;
        }
        out[i] = '\0';
        return i > 0;
    }
}

static int pack_meets_min_version(const char* pack_root) {
    char ver[64];
    const char* need = toolchain_min_version();
    if (!need || !need[0])
        return 1;
    if (!read_pack_version(pack_root, ver, sizeof(ver)))
        return 0;
    return version_cmp(ver, need) >= 0;
}

static int mkdir_p(const char* path) {
    char tmp[1400];
    size_t n;
    if (!path || !path[0])
        return 0;
    n = strlen(path);
    if (n >= sizeof(tmp))
        return 0;
    memcpy(tmp, path, n + 1);
#if defined(_WIN32)
    {
        char* p = tmp;
        if (n >= 2 && tmp[1] == ':')
            p = tmp + 2;
        while (*p == '\\' || *p == '/')
            ++p;
        for (; *p; ++p) {
            if (*p == '\\' || *p == '/') {
                char save = *p;
                *p = '\0';
                if (tmp[0])
                    CreateDirectoryA(tmp, NULL);
                *p = save;
            }
        }
        return CreateDirectoryA(tmp, NULL) ||
               GetLastError() == ERROR_ALREADY_EXISTS || path_is_dir(tmp);
    }
#else
    {
        char* p = tmp;
        if (*p == '/')
            ++p;
        for (; *p; ++p) {
            if (*p == '/') {
                *p = '\0';
                if (mkdir(tmp, 0755) != 0 && errno != EEXIST && !path_is_dir(tmp))
                    return 0;
                *p = '/';
            }
        }
        return mkdir(tmp, 0755) == 0 || errno == EEXIST || path_is_dir(tmp);
    }
#endif
}

static int rmtree_path(const char* path) {
    char cmd[1600];
    if (!path || !path[0] || !path_is_dir(path))
        return 1;
#if defined(_WIN32)
    snprintf(cmd, sizeof(cmd), "cmd.exe /c rmdir /s /q \"%s\"", path);
#else
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", path);
#endif
    return system(cmd) == 0 || !path_is_dir(path);
}

#if defined(_WIN32)
static int run_cmdline_wait(const char* cmdline, DWORD* out_code) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char mutable_cmd[8192];
    DWORD code = 1;
    HANDLE hin, hout, herr;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    /* GUI launchers often have INVALID std handles. Passing those with
     * STARTF_USESTDHANDLES makes console children (cmake/curl) fail even when
     * the exe is fine — do not inherit broken handles. */
    hin = GetStdHandle(STD_INPUT_HANDLE);
    hout = GetStdHandle(STD_OUTPUT_HANDLE);
    herr = GetStdHandle(STD_ERROR_HANDLE);
    if (hin != INVALID_HANDLE_VALUE && hin != NULL &&
        hout != INVALID_HANDLE_VALUE && hout != NULL &&
        herr != INVALID_HANDLE_VALUE && herr != NULL) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = hin;
        si.hStdOutput = hout;
        si.hStdError = herr;
    }
    snprintf(mutable_cmd, sizeof(mutable_cmd), "%s", cmdline);
    if (!CreateProcessA(NULL, mutable_cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi))
        return 0;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (out_code)
        *out_code = code;
    return 1;
}
#endif

static int shared_toolchain_latest_dir(char* out, size_t cap) {
    char cache[1400];
    if (!preferred_toolchain_cache_root(cache, sizeof(cache)))
        return 0;
    return join_path(out, cap, cache, "latest");
}

/* True when root/bin/cmake(.exe) exists (no nested unwrap). */
static int pack_root_has_cmake_direct(const char* root) {
    char bin[1400], cmake[1400];
    if (!root || !root[0])
        return 0;
    if (!join_path(bin, sizeof(bin), root, "bin"))
        return 0;
#if defined(_WIN32)
    return join_path(cmake, sizeof(cmake), bin, "cmake.exe") && path_is_file(cmake);
#else
    return join_path(cmake, sizeof(cmake), bin, "cmake") && path_is_file(cmake);
#endif
}

/* Resolve pack root with bin/cmake — flat or single nested child (zip layout). */
static int unwrap_toolchain_pack_root(const char* in, char* out, size_t cap) {
    char bin[1400];
    if (!in || !in[0] || !out || cap < 2)
        return 0;
    if (pack_root_has_cmake_direct(in)) {
        snprintf(out, cap, "%s", in);
        return 1;
    }
    if (resolve_toolchain_bin_under(in, bin, sizeof(bin)) &&
        dirname_copy(out, cap, bin))
        return pack_root_has_cmake_direct(out);
    return 0;
}

static int pack_root_has_cmake(const char* root) {
    char unwrapped[1400];
    return unwrap_toolchain_pack_root(root, unwrapped, sizeof(unwrapped));
}

/* Read retcomm-toolchain.json "version" (best-effort; empty if missing). */
static void read_toolchain_pack_version(const char* pack_root, char* out,
                                        size_t cap) {
    char path[1400], buf[2048];
    FILE* f;
    out[0] = '\0';
    if (!pack_root || !pack_root[0] || !out || cap < 2)
        return;
    if (!join_path(path, sizeof(path), pack_root, "retcomm-toolchain.json"))
        return;
    f = fopen(path, "r");
    if (!f)
        return;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    const char* p = strstr(buf, "\"version\"");
    if (!p)
        return;
    p = strchr(p + 9, '"');
    if (!p)
        return;
    ++p;
    const char* end = strchr(p, '"');
    if (!end || end <= p)
        return;
    size_t len = (size_t)(end - p);
    if (len >= cap)
        len = cap - 1;
    memcpy(out, p, len);
    out[len] = '\0';
}

static void sanitize_toolchain_tag(const char* ver, char* out, size_t cap) {
    size_t j = 0;
    if (!out || cap < 2) return;
    if (!ver || !ver[0]) {
        snprintf(out, cap, "offline");
        return;
    }
    for (size_t i = 0; ver[i] && j + 1 < cap; ++i) {
        unsigned char c = (unsigned char)ver[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
            out[j++] = (char)c;
        else
            out[j++] = '_';
    }
    out[j] = '\0';
    if (!out[0])
        snprintf(out, cap, "offline");
}

/* Point cache_root/latest at pack_root (symlink preferred; copy fallback).
 * Never install pack contents *into* latest/ — latest is pointer-only. */
static int set_toolchain_latest_pointer(const char* cache_root,
                                        const char* pack_root) {
    char latest[1400], resolved_pack[1400];
    if (!cache_root || !pack_root || !pack_root[0])
        return 0;
    if (!unwrap_toolchain_pack_root(pack_root, resolved_pack,
                                   sizeof(resolved_pack)))
        return 0;
    if (!join_path(latest, sizeof(latest), cache_root, "latest"))
        return 0;
    /* Already points at a usable pack (symlink or real dir with bin/). */
    if (pack_root_has_cmake_direct(latest)) {
#if !defined(_WIN32)
        char latest_res[1400], pack_res[1400];
        if (realpath(latest, latest_res) && realpath(resolved_pack, pack_res) &&
            strcmp(latest_res, pack_res) == 0)
            return 1;
#endif
        if (strcmp(latest, resolved_pack) == 0)
            return 1;
    }
    rmtree_path(latest);
#if defined(_WIN32)
    if (junction_dir(latest, resolved_pack))
        return 1;
    {
        char cmd[3200];
        DWORD code = 1;
        mkdir_p(latest);
        snprintf(cmd, sizeof(cmd),
                 "cmd.exe /c robocopy \"%s\" \"%s\" /E /NFL /NDL /NJH /NJS /nc "
                 "/ns /np",
                 resolved_pack, latest);
        if (run_cmdline_wait(cmd, &code) && code <= 7 &&
            pack_root_has_cmake_direct(latest))
            return 1;
    }
#else
    if (symlink(resolved_pack, latest) == 0)
        return 1;
    {
        char cmd[3200];
        snprintf(cmd, sizeof(cmd), "cp -a \"%s\" \"%s\"", resolved_pack, latest);
        if (system(cmd) == 0 && pack_root_has_cmake_direct(latest))
            return 1;
    }
#endif
    return pack_root_has_cmake_direct(latest);
}

static int write_project_toolchain_stamp(const char* bin_dir) {
    char tc_dir[1200], stamp[1200];
    FILE* f;
    if (!g_project_root[0] || !bin_dir || !bin_dir[0])
        return 0;
    if (!join_path(tc_dir, sizeof(tc_dir), g_project_root, "toolchain"))
        return 0;
    mkdir_p(tc_dir);
    if (!join_path(stamp, sizeof(stamp), tc_dir, ".psxrecomp-bin"))
        return 0;
    f = fopen(stamp, "w");
    if (!f)
        return 0;
    fprintf(f, "%s\n", bin_dir);
    fclose(f);
    return 1;
}

static int activate_installed_pack_root(const char* pack_root) {
    char root[1400], bin[1400];
    if (!unwrap_toolchain_pack_root(pack_root, root, sizeof(root)))
        return 0;
    if (!join_path(bin, sizeof(bin), root, "bin"))
        return 0;
    snprintf(g_cli_toolchain_bin, sizeof(g_cli_toolchain_bin), "%s", bin);
#if defined(_WIN32)
    _putenv_s("RETCOMM_TOOLCHAIN_DIR", root);
    _putenv_s("PSXRECOMP_TOOLCHAIN_DIR", root);
#else
    setenv("RETCOMM_TOOLCHAIN_DIR", root, 1);
    setenv("PSXRECOMP_TOOLCHAIN_DIR", root, 1);
#endif
    write_project_toolchain_stamp(bin);
    activate_toolchain_path();
    /* File presence is not enough — cmake and clang/lld must actually run
     * (missing DLLs / libicu / MOTW used to pass find_cmake then fail later). */
    if (!find_cmake(g_cmake, sizeof(g_cmake)))
        return 0;
    if (!cmake_path_runs(g_cmake))
        return 0;
    return toolchain_bin_is_healthy(bin);
}

#if defined(_WIN32)
/* Point a real LocalAppData path at a Store-Python LocalCache pack (no copy). */
static int junction_dir(const char* link_path, const char* target_path) {
    char cmd[3200];
    DWORD code = 1;
    char parent[1400];
    if (!link_path || !target_path || !path_is_dir(target_path))
        return 0;
    if (path_is_dir(link_path) || path_is_file(link_path)) {
        /* Replace broken/empty dir; keep a usable pack. */
        if (pack_root_has_cmake(link_path))
            return 1;
        rmtree_path(link_path);
    }
    if (!dirname_copy(parent, sizeof(parent), link_path))
        return 0;
    mkdir_p(parent);
    snprintf(cmd, sizeof(cmd), "cmd.exe /c mklink /J \"%s\" \"%s\"", link_path,
             target_path);
    if (!run_cmdline_wait(cmd, &code))
        return 0;
    return code == 0 && path_is_dir(link_path);
}

static int find_store_localcache_pack_root(char* out, size_t cap) {
    const char* local = getenv("LOCALAPPDATA");
    char packages[1100], pattern[1200];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    if (!local || !local[0])
        return 0;
    if (!join_path(packages, sizeof(packages), local, "Packages"))
        return 0;
    snprintf(pattern, sizeof(pattern),
             "%s\\PythonSoftwareFoundation.Python.*", packages);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        const char* suffixes[] = {
            "LocalCache\\Local\\retcomm\\toolchains\\cmake-clang-v1\\latest",
            "LocalCache\\Local\\retcomm\\toolchains\\cmake-clang-v1\\offline",
            "LocalCache\\Local\\psxrecomp\\toolchains\\cmake-clang-v1\\latest",
            "LocalCache\\Local\\psxrecomp\\toolchains\\cmake-clang-v1\\offline",
            NULL};
        int i;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        for (i = 0; suffixes[i]; ++i) {
            char cand[1400], nested[1400];
            snprintf(cand, sizeof(cand), "%s\\%s\\%s", packages, fd.cFileName,
                     suffixes[i]);
            if (pack_root_has_cmake(cand)) {
                snprintf(out, cap, "%s", cand);
                FindClose(h);
                return 1;
            }
            /* Nested single child with bin/cmake.exe */
            if (path_is_dir(cand)) {
                WIN32_FIND_DATAA kd;
                char kp[1400];
                HANDLE hk;
                snprintf(kp, sizeof(kp), "%s\\*", cand);
                hk = FindFirstFileA(kp, &kd);
                if (hk == INVALID_HANDLE_VALUE)
                    continue;
                do {
                    if (!(kd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                        continue;
                    if (kd.cFileName[0] == '.')
                        continue;
                    if (!join_path(nested, sizeof(nested), cand, kd.cFileName))
                        continue;
                    if (pack_root_has_cmake(nested)) {
                        snprintf(out, cap, "%s", nested);
                        FindClose(hk);
                        FindClose(h);
                        return 1;
                    }
                } while (FindNextFileA(hk, &kd));
                FindClose(hk);
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
}

/* allow_copy=0: point this process at LocalCache (cheap).
 * allow_copy=1: also junction/robocopy into real %LOCALAPPDATA% (ensure). */
static int harvest_store_python_toolchain(int allow_copy) {
    char cache_root[1400], real_latest[1400], proj_tc[1200];
    if (!find_store_localcache_pack_root(cache_root, sizeof(cache_root)))
        return 0;
    if (!allow_copy)
        return activate_installed_pack_root(cache_root);
    if (!shared_toolchain_latest_dir(real_latest, sizeof(real_latest)))
        return activate_installed_pack_root(cache_root);
    if (!junction_dir(real_latest, cache_root) &&
        !pack_root_has_cmake(real_latest)) {
        /* Junction failed — robocopy into the real tree. */
        char parent[1400], cmd[3200];
        DWORD code = 1;
        if (!dirname_copy(parent, sizeof(parent), real_latest))
            return activate_installed_pack_root(cache_root);
        mkdir_p(parent);
        rmtree_path(real_latest);
        mkdir_p(real_latest);
        snprintf(cmd, sizeof(cmd),
                 "cmd.exe /c robocopy \"%s\" \"%s\" /E /NFL /NDL /NJH /NJS /nc "
                 "/ns /np",
                 cache_root, real_latest);
        if (!run_cmdline_wait(cmd, &code) || code > 7 ||
            !pack_root_has_cmake(real_latest))
            return activate_installed_pack_root(cache_root);
    }
    if (g_project_root[0] &&
        join_path(proj_tc, sizeof(proj_tc), g_project_root, "toolchain")) {
        if (!pack_root_has_cmake(proj_tc))
            junction_dir(proj_tc, pack_root_has_cmake(real_latest)
                                      ? real_latest
                                      : cache_root);
    }
    return activate_installed_pack_root(
        pack_root_has_cmake(real_latest) ? real_latest : cache_root);
}

static int host_download_url_to_file(const char* url, const char* dest,
                                     char* err_msg, size_t err_cap) {
    char cmd[4096];
    DWORD code = 1;
    char parent[1400];
    if (!dirname_copy(parent, sizeof(parent), dest)) {
        snprintf(err_msg, err_cap, "Bad download destination.");
        return 0;
    }
    mkdir_p(parent);
    DeleteFileA(dest);
    /* Windows 10+ ships curl.exe. -L follows GitHub release redirects. */
    snprintf(cmd, sizeof(cmd),
             "curl.exe -fsSL --retry 3 --retry-delay 2 -o \"%s\" \"%s\"", dest,
             url);
    if (!run_cmdline_wait(cmd, &code) || code != 0) {
        snprintf(err_msg, err_cap,
                 "Toolchain download failed (curl exit %lu). Check network / "
                 "curl.exe.",
                 (unsigned long)code);
        return 0;
    }
    return path_is_file(dest);
}

static int host_extract_zip(const char* zip_path, const char* dest_dir,
                            char* err_msg, size_t err_cap) {
    char cmd[3200];
    DWORD code = 1;
    char parent[1400];
    if (!path_is_file(zip_path)) {
        snprintf(err_msg, err_cap, "Toolchain zip not found: %s", zip_path);
        return 0;
    }
    if (!dirname_copy(parent, sizeof(parent), dest_dir)) {
        snprintf(err_msg, err_cap, "Bad extract destination.");
        return 0;
    }
    mkdir_p(parent);
    rmtree_path(dest_dir);
    mkdir_p(dest_dir);
    /* tar.exe on Windows 10+ extracts .zip */
    snprintf(cmd, sizeof(cmd), "tar.exe -xf \"%s\" -C \"%s\"", zip_path,
             dest_dir);
    if (!run_cmdline_wait(cmd, &code) || code != 0) {
        snprintf(err_msg, err_cap, "Failed to extract toolchain zip (tar exit %lu).",
                 (unsigned long)code);
        return 0;
    }
    if (pack_root_has_cmake(dest_dir))
        return 1;
    /* Single nested directory layout — resolve_toolchain_bin_under handles it. */
    {
        WIN32_FIND_DATAA fd;
        char pattern[1400], child[1400];
        HANDLE h;
        snprintf(pattern, sizeof(pattern), "%s\\*", dest_dir);
        h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    continue;
                if (fd.cFileName[0] == '.')
                    continue;
                if (!join_path(child, sizeof(child), dest_dir, fd.cFileName))
                    continue;
                if (pack_root_has_cmake(child)) {
                    FindClose(h);
                    return 1;
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    snprintf(err_msg, err_cap, "Toolchain zip missing bin/cmake.exe.");
    return 0;
}
#else
static int host_download_url_to_file(const char* url, const char* dest,
                                     char* err_msg, size_t err_cap) {
    char cmd[4096];
    char parent[1400];
    if (!dirname_copy(parent, sizeof(parent), dest)) {
        snprintf(err_msg, err_cap, "Bad download destination.");
        return 0;
    }
    mkdir_p(parent);
    unlink(dest);
    snprintf(cmd, sizeof(cmd),
             "curl -fsSL --retry 3 --retry-delay 2 -o \"%s\" \"%s\"", dest, url);
    if (system(cmd) != 0) {
        snprintf(err_msg, err_cap, "Toolchain download failed (curl).");
        return 0;
    }
    return path_is_file(dest);
}

static int host_extract_zip(const char* zip_path, const char* dest_dir,
                            char* err_msg, size_t err_cap) {
    char cmd[3200];
    char parent[1400];
    if (!path_is_file(zip_path)) {
        snprintf(err_msg, err_cap, "Toolchain zip not found: %s", zip_path);
        return 0;
    }
    if (!dirname_copy(parent, sizeof(parent), dest_dir)) {
        snprintf(err_msg, err_cap, "Bad extract destination.");
        return 0;
    }
    mkdir_p(parent);
    rmtree_path(dest_dir);
    mkdir_p(dest_dir);
    snprintf(cmd, sizeof(cmd), "unzip -q \"%s\" -d \"%s\"", zip_path, dest_dir);
    if (system(cmd) != 0) {
        snprintf(cmd, sizeof(cmd), "tar -xf \"%s\" -C \"%s\"", zip_path,
                 dest_dir);
        if (system(cmd) != 0) {
            snprintf(err_msg, err_cap, "Failed to extract toolchain zip.");
            return 0;
        }
    }
    if (pack_root_has_cmake(dest_dir))
        return 1;
    /* Nested child with bin/ is fine — resolve_toolchain_bin_under handles it. */
    {
        DIR* dir = opendir(dest_dir);
        struct dirent* ent;
        if (!dir) {
            snprintf(err_msg, err_cap, "Toolchain zip missing bin/cmake.");
            return 0;
        }
        while ((ent = readdir(dir)) != NULL) {
            char child[1400];
            if (ent->d_name[0] == '.')
                continue;
            if (!join_path(child, sizeof(child), dest_dir, ent->d_name))
                continue;
            if (path_is_dir(child) && pack_root_has_cmake(child)) {
                closedir(dir);
                return 1;
            }
        }
        closedir(dir);
    }
    snprintf(err_msg, err_cap, "Toolchain zip missing bin/cmake.");
    return 0;
}
#endif

static int link_or_stamp_project_toolchain(const char* pack_root) {
    char root[1400], proj_tc[1200], bin[1400];
    if (!unwrap_toolchain_pack_root(pack_root, root, sizeof(root)))
        return 0;
    if (!join_path(bin, sizeof(bin), root, "bin"))
        return 0;
#if defined(_WIN32)
    if (g_project_root[0] &&
        join_path(proj_tc, sizeof(proj_tc), g_project_root, "toolchain")) {
        if (!pack_root_has_cmake(proj_tc))
            junction_dir(proj_tc, root);
    }
#else
    if (g_project_root[0] &&
        join_path(proj_tc, sizeof(proj_tc), g_project_root, "toolchain")) {
        if (!pack_root_has_cmake(proj_tc) && !path_is_dir(proj_tc)) {
            char cmd[2800];
            snprintf(cmd, sizeof(cmd), "ln -s \"%s\" \"%s\"", root, proj_tc);
            (void)system(cmd);
        }
    }
#endif
    write_project_toolchain_stamp(bin);
    return 1;
}

/* True when *child* is *parent* or a path under *parent* (dir prefix). */
static int path_is_under_dir(const char* child, const char* parent) {
    char ca[1400], pa[1400];
    size_t n;
    if (!child || !child[0] || !parent || !parent[0])
        return 0;
#if defined(_WIN32)
    {
        DWORD nc = GetFullPathNameA(child, (DWORD)sizeof(ca), ca, NULL);
        DWORD np = GetFullPathNameA(parent, (DWORD)sizeof(pa), pa, NULL);
        if (nc == 0 || nc >= (DWORD)sizeof(ca) || np == 0 ||
            np >= (DWORD)sizeof(pa))
            return 0;
    }
#else
    {
        char* rc = realpath(child, NULL);
        char* rp = realpath(parent, NULL);
        if (!rc || !rp) {
            free(rc);
            free(rp);
            return 0;
        }
        snprintf(ca, sizeof(ca), "%s", rc);
        snprintf(pa, sizeof(pa), "%s", rp);
        free(rc);
        free(rp);
    }
#endif
    n = strlen(pa);
#if defined(_WIN32)
    if (_strnicmp(ca, pa, (int)n) != 0)
        return 0;
#else
    if (strncmp(ca, pa, n) != 0)
        return 0;
#endif
    if (ca[n] == '\0')
        return 1;
    return ca[n] == '/' || ca[n] == '\\';
}

/* After a successful install into the managed cache, drop other versioned
 * <tag>/ siblings (and *.broken quarantines). Keeps *keep_pack*, latest/, and
 * dot-directories (staging). Does not touch RETCOMM_TOOLCHAIN_DIR overrides
 * outside the preferred install root. */
static void prune_old_toolchain_tags(const char* keep_pack) {
    char cache_root[1400];
    int removed = 0;
    if (!keep_pack || !keep_pack[0])
        return;
    if (!preferred_toolchain_cache_root(cache_root, sizeof(cache_root)))
        return;
    if (!path_is_dir(cache_root))
        return;
    if (!path_is_under_dir(keep_pack, cache_root) &&
        !host_paths_same_file(keep_pack, cache_root))
        return;

#if defined(_WIN32)
    {
        WIN32_FIND_DATAA fd;
        HANDLE h;
        char pattern[1400];
        snprintf(pattern, sizeof(pattern), "%s\\*", cache_root);
        h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE)
            return;
        do {
            char child[1400];
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                continue;
            if (fd.cFileName[0] == '.')
                continue;
            if (_stricmp(fd.cFileName, "latest") == 0)
                continue;
            if (!join_path(child, sizeof(child), cache_root, fd.cFileName))
                continue;
            if (path_is_under_dir(keep_pack, child) ||
                host_paths_same_file(keep_pack, child) ||
                path_is_under_dir(child, keep_pack))
                continue;
            if (rmtree_path(child))
                removed = 1;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    {
        DIR* d = opendir(cache_root);
        struct dirent* ent;
        if (!d)
            return;
        while ((ent = readdir(d)) != NULL) {
            char child[1400];
            struct stat st;
            if (ent->d_name[0] == '.')
                continue;
            if (strcmp(ent->d_name, "latest") == 0)
                continue;
            if (!join_path(child, sizeof(child), cache_root, ent->d_name))
                continue;
            if (lstat(child, &st) != 0)
                continue;
            if (!S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode))
                continue;
            if (path_is_under_dir(keep_pack, child) ||
                host_paths_same_file(keep_pack, child) ||
                path_is_under_dir(child, keep_pack))
                continue;
            if (rmtree_path(child))
                removed = 1;
        }
        closedir(d);
    }
#endif
    (void)removed;
}

static int host_install_toolchain_from_zip(
    const char* zip_path, RecompLauncherCPrepareProgressFn on_progress,
    void* progress_ctx, char* err_msg, size_t err_cap) {
    /* Match retcomm-toolchains install.sh:
     *   …/cmake-clang-v1/<tag>/   (real pack with bin/)
     *   …/cmake-clang-v1/latest → <tag>   (pointer only)
     * Never extract the zip *into* latest/ — resolvers expect latest/bin. */
    char cache_root[1400], staging[1400], pack[1400], ver[64], tag[80];
    char dest[1400], latest[1400];
    if (!preferred_toolchain_cache_root(cache_root, sizeof(cache_root))) {
        snprintf(err_msg, err_cap, "Cannot resolve shared toolchain directory.");
        return 0;
    }
    mkdir_p(cache_root);
    if (!join_path(staging, sizeof(staging), cache_root, ".staging-host")) {
        snprintf(err_msg, err_cap, "Staging path too long.");
        return 0;
    }
    if (on_progress)
        on_progress(progress_ctx, 0.45f, "Extracting portable cmake/clang…");
    if (!host_extract_zip(zip_path, staging, err_msg, err_cap))
        return 0;
    if (!unwrap_toolchain_pack_root(staging, pack, sizeof(pack))) {
        snprintf(err_msg, err_cap, "Extracted toolchain but cmake was not found.");
        rmtree_path(staging);
        return 0;
    }
    read_toolchain_pack_version(pack, ver, sizeof(ver));
    sanitize_toolchain_tag(ver[0] ? ver : "offline", tag, sizeof(tag));
    /* Never use "latest" as a tag directory name — reserved for the pointer. */
    if (strcmp(tag, "latest") == 0)
        snprintf(tag, sizeof(tag), "offline");
    if (!join_path(dest, sizeof(dest), cache_root, tag)) {
        snprintf(err_msg, err_cap, "Install path too long.");
        rmtree_path(staging);
        return 0;
    }
    if (on_progress)
        on_progress(progress_ctx, 0.7f, "Installing into shared toolchain cache…");
    rmtree_path(dest);
    mkdir_p(cache_root);
    /* Move usable pack to <tag>/ (flat bin/ at dest). */
    if (strcmp(pack, staging) == 0) {
#if defined(_WIN32)
        if (!MoveFileExA(staging, dest, MOVEFILE_COPY_ALLOWED |
                                            MOVEFILE_REPLACE_EXISTING)) {
            char cmd[3200];
            DWORD code = 1;
            mkdir_p(dest);
            snprintf(cmd, sizeof(cmd),
                     "cmd.exe /c robocopy \"%s\" \"%s\" /E /NFL /NDL /NJH /NJS "
                     "/nc /ns /np /MOVE",
                     staging, dest);
            (void)run_cmdline_wait(cmd, &code);
        }
#else
        if (rename(staging, dest) != 0) {
            char cmd[3200];
            snprintf(cmd, sizeof(cmd), "mv \"%s\" \"%s\"", staging, dest);
            if (system(cmd) != 0) {
                snprintf(cmd, sizeof(cmd), "cp -a \"%s\" \"%s\"", staging, dest);
                (void)system(cmd);
                rmtree_path(staging);
            }
        }
#endif
    } else {
        /* Nested zip: move the child with bin/ up to dest. */
#if defined(_WIN32)
        if (!MoveFileExA(pack, dest, MOVEFILE_COPY_ALLOWED |
                                         MOVEFILE_REPLACE_EXISTING)) {
            char cmd[3200];
            DWORD code = 1;
            mkdir_p(dest);
            snprintf(cmd, sizeof(cmd),
                     "cmd.exe /c robocopy \"%s\" \"%s\" /E /NFL /NDL /NJH /NJS "
                     "/nc /ns /np /MOVE",
                     pack, dest);
            (void)run_cmdline_wait(cmd, &code);
        }
#else
        if (rename(pack, dest) != 0) {
            char cmd[3200];
            snprintf(cmd, sizeof(cmd), "mv \"%s\" \"%s\"", pack, dest);
            if (system(cmd) != 0) {
                snprintf(cmd, sizeof(cmd), "cp -a \"%s\" \"%s\"", pack, dest);
                (void)system(cmd);
            }
        }
#endif
        rmtree_path(staging);
    }
    if (!unwrap_toolchain_pack_root(dest, pack, sizeof(pack))) {
        snprintf(err_msg, err_cap, "Installed toolchain missing bin/cmake.");
        return 0;
    }
    if (!set_toolchain_latest_pointer(cache_root, pack)) {
        /* Pointer is best-effort — pack at <tag>/ is still usable. */
    }
    if (on_progress)
        on_progress(progress_ctx, 0.85f, "Activating toolchain…");
    link_or_stamp_project_toolchain(pack);
    if (activate_installed_pack_root(pack) ||
        (join_path(latest, sizeof(latest), cache_root, "latest") &&
         activate_installed_pack_root(latest))) {
        if (on_progress)
            on_progress(progress_ctx, 0.92f,
                        "Removing older toolchain installs…");
        prune_old_toolchain_tags(pack);
        return 1;
    }
    snprintf(err_msg, err_cap,
             "Extracted toolchain but cmake.exe is missing or will not run "
             "(bin\\cmake.exe --version failed).");
    return 0;
}

static int host_download_and_install_toolchain(
    RecompLauncherCPrepareProgressFn on_progress, void* progress_ctx,
    char* err_msg, size_t err_cap) {
    char url[512], zip_path[1400], tmp_dir[1100];
    const char* asset = toolchain_zip_asset_name();
    snprintf(url, sizeof(url),
             "https://github.com/%s/releases/latest/download/%s", k_tc_repo,
             asset);
#if defined(_WIN32)
    {
        char tmp[512];
        DWORD n = GetTempPathA(sizeof(tmp), tmp);
        if (n == 0 || n >= sizeof(tmp)) {
            snprintf(err_msg, err_cap, "GetTempPath failed.");
            return 0;
        }
        snprintf(tmp_dir, sizeof(tmp_dir), "%spsxrecomp-tc-%lu", tmp,
                 (unsigned long)GetCurrentProcessId());
    }
#else
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/psxrecomp-tc-%d", (int)getpid());
#endif
    mkdir_p(tmp_dir);
    if (!join_path(zip_path, sizeof(zip_path), tmp_dir, asset)) {
        snprintf(err_msg, err_cap, "Temp path too long.");
        return 0;
    }
    if (on_progress)
        on_progress(progress_ctx, 0.1f, "Downloading portable cmake/clang…");
    if (!host_download_url_to_file(url, zip_path, err_msg, err_cap)) {
        rmtree_path(tmp_dir);
        return 0;
    }
    if (!host_install_toolchain_from_zip(zip_path, on_progress, progress_ctx,
                                         err_msg, err_cap)) {
        rmtree_path(tmp_dir);
        return 0;
    }
    rmtree_path(tmp_dir);
    return 1;
}

/* Promote a usable legacy psxrecomp cache into the shared retcomm tree. */
static int migrate_legacy_psxrecomp_toolchain(void) {
    char legacy[1400], dest[1400], parent[1400];
#if defined(_WIN32)
    const char* local = getenv("LOCALAPPDATA");
    if (!local || !local[0])
        return 0;
    if (!join_path(legacy, sizeof(legacy), local,
                   "psxrecomp/toolchains/cmake-clang-v1/latest"))
        return 0;
#else
    const char* xdg = getenv("XDG_DATA_HOME");
    const char* home = getenv("HOME");
    if (xdg && xdg[0]) {
        if (!join_path(legacy, sizeof(legacy), xdg,
                       "psxrecomp/toolchains/cmake-clang-v1/latest"))
            return 0;
    } else if (home && home[0]) {
        if (!join_path(legacy, sizeof(legacy), home,
                       ".local/share/psxrecomp/toolchains/cmake-clang-v1/latest"))
            return 0;
    } else {
        return 0;
    }
#endif
    if (!pack_root_has_cmake(legacy))
        return 0;
    if (!shared_toolchain_latest_dir(dest, sizeof(dest)))
        return 0;
    if (pack_root_has_cmake(dest))
        return 1;
    if (!dirname_copy(parent, sizeof(parent), dest))
        return 0;
    mkdir_p(parent);
#if defined(_WIN32)
    if (junction_dir(dest, legacy))
        return 1;
    {
        char cmd[3200];
        DWORD code = 1;
        mkdir_p(dest);
        snprintf(cmd, sizeof(cmd),
                 "cmd.exe /c robocopy \"%s\" \"%s\" /E /NFL /NDL /NJH /NJS /nc "
                 "/ns /np",
                 legacy, dest);
        if (run_cmdline_wait(cmd, &code) && code <= 7 && pack_root_has_cmake(dest))
            return 1;
    }
#else
    {
        char cmd[2800];
        snprintf(cmd, sizeof(cmd), "ln -sfn \"%s\" \"%s\"", legacy, dest);
        if (system(cmd) == 0 && pack_root_has_cmake(dest))
            return 1;
        snprintf(cmd, sizeof(cmd), "cp -a \"%s\" \"%s\"", legacy, dest);
        if (system(cmd) == 0 && pack_root_has_cmake(dest))
            return 1;
    }
#endif
    return 0;
}

static int pack_root_from_bin(const char* bin, char* out, size_t cap) {
    return bin && bin[0] && dirname_copy(out, cap, bin);
}

static int run_cmd_exit_zero(const char* cmdline) {
#if defined(_WIN32)
    DWORD code = 1;
    if (!run_cmdline_wait(cmdline, &code))
        return 0;
    return code == 0;
#else
    int st = system(cmdline);
    return st == 0;
#endif
}

/* True when cmake_path exists and ``cmake --version`` succeeds. */
static int cmake_path_runs(const char* cmake_path) {
    char cmd[1600];
    if (!cmake_path || !cmake_path[0] || !path_is_file(cmake_path))
        return 0;
#if defined(_WIN32)
    /* CreateProcess does not honor >NUL — use cmd /C. Nested quotes are the
     * documented form for a quoted exe path under cmd /C. */
    snprintf(cmd, sizeof(cmd), "cmd.exe /C \"\"%s\" --version >NUL 2>&1\"",
             cmake_path);
#else
    snprintf(cmd, sizeof(cmd), "\"%s\" --version >/dev/null 2>&1", cmake_path);
#endif
    return run_cmd_exit_zero(cmd);
}

/* True when pack bin/ can compile+link a tiny C program (catches broken
 * ld.lld / missing libicuuc.so.* etc. that cmake --version still passes). */
static int toolchain_bin_compiler_works(const char* bin) {
    char clang[1200], lld[1200], src[1400], exe[1400], cmd[4096];
    FILE* f;
    int ok;
    if (!bin || !bin[0])
        return 0;
#if defined(_WIN32)
    if (!join_path(clang, sizeof(clang), bin, "clang.exe") || !path_is_file(clang))
        return 0;
    if (!join_path(lld, sizeof(lld), bin, "ld.lld.exe") || !path_is_file(lld)) {
        /* Some Windows packs only ship lld.exe — accept either. */
        if (!join_path(lld, sizeof(lld), bin, "lld.exe") || !path_is_file(lld))
            return 0;
    }
#else
    if (!join_path(clang, sizeof(clang), bin, "clang") || !path_is_file(clang))
        return 0;
    if (!join_path(lld, sizeof(lld), bin, "ld.lld") || !path_is_file(lld))
        return 0;
#endif
    if (!cmake_path_runs(clang))
        return 0;

#if defined(_WIN32)
    {
        char tdir[512];
        DWORD tn = GetTempPathA(sizeof(tdir), tdir);
        if (tn == 0 || tn >= sizeof(tdir))
            return 0;
        snprintf(src, sizeof(src), "%spsxrecomp-tc-probe-%lu.c", tdir,
                 (unsigned long)GetCurrentProcessId());
        snprintf(exe, sizeof(exe), "%spsxrecomp-tc-probe-%lu.exe", tdir,
                 (unsigned long)GetCurrentProcessId());
    }
#else
    snprintf(src, sizeof(src), "/tmp/psxrecomp-tc-probe-%d.c", (int)getpid());
    snprintf(exe, sizeof(exe), "/tmp/psxrecomp-tc-probe-%d", (int)getpid());
#endif

    f = fopen(src, "wb");
    if (!f)
        return 0;
    fputs("int main(void){return 0;}\n", f);
    fclose(f);

#if defined(_WIN32)
    /* Prepend pack bin so clang picks this tree's lld, not a system linker. */
    snprintf(cmd, sizeof(cmd),
             "cmd.exe /C \"set \"PATH=%s;%%PATH%%\" && \"%s\" \"%s\" -o \"%s\" "
             ">NUL 2>&1\"",
             bin, clang, src, exe);
#else
    /* Prefer the pack linker explicitly so PATH cannot hide a broken lld. */
    (void)lld; /* used via -fuse-ld when present; path already validated */
    snprintf(cmd, sizeof(cmd),
             "env PATH=\"%s:$PATH\" \"%s\" -fuse-ld=lld \"%s\" -o \"%s\" "
             ">/dev/null 2>&1",
             bin, clang, src, exe);
#endif
    ok = run_cmd_exit_zero(cmd);
#if defined(_WIN32)
    DeleteFileA(src);
    DeleteFileA(exe);
#else
    unlink(src);
    unlink(exe);
#endif
    return ok;
}

static int toolchain_bin_is_healthy(const char* bin) {
    char cmake[1200];
    if (!bin || !bin[0])
        return 0;
#if defined(_WIN32)
    if (!join_path(cmake, sizeof(cmake), bin, "cmake.exe") ||
        !cmake_path_runs(cmake))
        return 0;
#else
    if (!join_path(cmake, sizeof(cmake), bin, "cmake") || !cmake_path_runs(cmake))
        return 0;
#endif
    return toolchain_bin_compiler_works(bin);
}

static void clear_project_toolchain_stamp(void) {
    char stamp[1200];
    if (!g_project_root[0])
        return;
    if (!join_path(stamp, sizeof(stamp), g_project_root,
                   "toolchain/.psxrecomp-bin"))
        return;
#if defined(_WIN32)
    DeleteFileA(stamp);
#else
    unlink(stamp);
#endif
}

/* True when path is …/latest or …/latest/bin (host unpack / pointer). */
static int path_is_toolchain_latest_leaf(const char* path) {
    const char* base;
    size_t n;
    if (!path || !path[0])
        return 0;
    n = strlen(path);
    while (n > 1 && (path[n - 1] == '/' || path[n - 1] == '\\'))
        --n;
    base = path;
    for (size_t i = 0; i < n; ++i) {
        if (path[i] == '/' || path[i] == '\\')
            base = path + i + 1;
    }
    if (strncmp(base, "latest", 6) == 0 &&
        (base[6] == '\0' || base[6] == '/' || base[6] == '\\'))
        return 1;
    if (strcmp(base, "bin") != 0)
        return 0;
    /* parent directory name == latest */
    {
        const char* p = base;
        while (p > path && p[-1] != '/' && p[-1] != '\\')
            --p;
        if (p <= path)
            return 0;
        --p;
        while (p > path && p[-1] != '/' && p[-1] != '\\')
            --p;
        return strncmp(p, "latest", 6) == 0 &&
               (p[6] == '/' || p[6] == '\\' || p[6] == '\0');
    }
}

/* Remove unusable latest/ under shared cache roots (heal before re-download).
 * Also rejects packs where cmake runs but clang/ld.lld cannot link (Linux ICU
 * / missing libxml2). Versioned <tag>/ siblings are pruned after a successful
 * install/update (see prune_old_toolchain_tags). */
static void heal_broken_toolchain_pointers(void) {
    char bases[12][1400];
    int n = collect_toolchain_cache_bases(bases, 12);
    int removed = 0;
    for (int i = 0; i < n; ++i) {
        char latest[1400], root[1400], bin[1400];
        int present = 0;
        if (!join_path(latest, sizeof(latest), bases[i], "latest"))
            continue;
#if defined(_WIN32)
        present = path_is_dir(latest) || path_is_file(latest);
#else
        {
            struct stat st;
            present = (lstat(latest, &st) == 0);
        }
#endif
        if (!present)
            continue;
        if (unwrap_toolchain_pack_root(latest, root, sizeof(root)) &&
            join_path(bin, sizeof(bin), root, "bin") &&
            toolchain_bin_is_healthy(bin))
            continue;
        rmtree_path(latest);
#if !defined(_WIN32)
        unlink(latest); /* dangling symlink after rmtree no-op */
#endif
        removed = 1;
    }
    if (removed) {
        clear_project_toolchain_stamp();
        g_toolchain_bin[0] = '\0';
        g_cli_toolchain_bin[0] = '\0';
        g_cmake[0] = '\0';
        snprintf(g_tc_repair_note, sizeof(g_tc_repair_note),
                 "Removed a broken portable toolchain cache (compiler/linker "
                 "failed a smoke test). Download the latest pack to continue.");
    }
}

/* If the active resolution points at an unhealthy pack, drop stamp / latest
 * (or quarantine a versioned <tag>/ so resolve cannot keep re-selecting it). */
static void discard_unhealthy_active_toolchain(void) {
    char pack[1400];
    if (!g_toolchain_bin[0])
        return;
    if (toolchain_bin_is_healthy(g_toolchain_bin))
        return;
    if (pack_root_from_bin(g_toolchain_bin, pack, sizeof(pack))) {
        if (path_is_toolchain_latest_leaf(pack) ||
            path_is_toolchain_latest_leaf(g_toolchain_bin)) {
            rmtree_path(pack);
#if !defined(_WIN32)
            unlink(pack);
#endif
        } else if (path_is_dir(pack)) {
            char quarantine[1500];
            snprintf(quarantine, sizeof(quarantine), "%s.broken", pack);
            rmtree_path(quarantine);
#if defined(_WIN32)
            if (!MoveFileExA(pack, quarantine, MOVEFILE_REPLACE_EXISTING))
                rmtree_path(pack);
#else
            if (rename(pack, quarantine) != 0)
                rmtree_path(pack);
#endif
        }
    }
    clear_project_toolchain_stamp();
    g_toolchain_bin[0] = '\0';
    g_cli_toolchain_bin[0] = '\0';
    g_cmake[0] = '\0';
    if (!g_tc_repair_note[0]) {
        snprintf(g_tc_repair_note, sizeof(g_tc_repair_note),
                 "Portable toolchain is installed but cannot compile/link "
                 "(path or library error). Redownload the latest pack.");
    }
}

static int active_toolchain_meets_min(void) {
    char pack[1400];
    if (!g_toolchain_bin[0] && !resolve_toolchain_bin(g_toolchain_bin,
                                                      sizeof(g_toolchain_bin)))
        return 0;
    if (!pack_root_from_bin(g_toolchain_bin, pack, sizeof(pack)))
        return 0;
    return pack_meets_min_version(pack);
}

static int host_portable_cmake_ready(void) {
    if (!find_cmake(g_cmake, sizeof(g_cmake)))
        return 0;
    if (!cmake_path_runs(g_cmake))
        return 0;
    if (!active_toolchain_meets_min())
        return 0;
    if (!g_toolchain_bin[0] &&
        !resolve_toolchain_bin(g_toolchain_bin, sizeof(g_toolchain_bin)))
        return 0;
    return toolchain_bin_is_healthy(g_toolchain_bin);
}

static const char* host_toolchain_repair_note(void) {
    return g_tc_repair_note[0] ? g_tc_repair_note : NULL;
}

static int host_toolchain_is_ready(void) {
    int attempt;
    if (!g_project_root[0])
        return 0;
    g_tc_repair_note[0] = '\0';
    migrate_legacy_psxrecomp_toolchain();
    /* Wizard open: drop broken latest/ before treating the pack as ready. */
    heal_broken_toolchain_pointers();
    for (attempt = 0; attempt < 3; ++attempt) {
        activate_toolchain_path();
        if (host_portable_cmake_ready())
            return 1;
        if (!g_toolchain_bin[0])
            break;
        discard_unhealthy_active_toolchain();
    }
#if defined(_WIN32)
    /* Reuse a Store-Python LocalCache install without copying. */
    if (harvest_store_python_toolchain(0)) {
        char pack[1400];
        if (pack_root_from_bin(g_cli_toolchain_bin[0] ? g_cli_toolchain_bin
                                                     : g_toolchain_bin,
                               pack, sizeof(pack)) &&
            pack_root_has_cmake(pack) && pack_meets_min_version(pack) &&
            host_portable_cmake_ready())
            return 1;
    }
#endif
    return 0;
}

/* Fill *out with the installed pack's version string (retcomm-toolchain.json). */
static int host_local_toolchain_version(char* out, size_t cap) {
    char pack[1400];
    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    activate_toolchain_path();
    if (!g_toolchain_bin[0] &&
        !resolve_toolchain_bin(g_toolchain_bin, sizeof(g_toolchain_bin)))
        return 0;
    if (!pack_root_from_bin(g_toolchain_bin, pack, sizeof(pack)))
        return 0;
    return read_pack_version(pack, out, cap);
}

/* Parse "tag_name":"…" or a …/releases/tag/<ver> URL into *out. */
static int parse_github_release_tag(const char* text, char* out, size_t cap) {
    const char* p;
    size_t i;
    if (!text || !out || cap < 2)
        return 0;
    out[0] = '\0';
    p = strstr(text, "\"tag_name\"");
    if (p) {
        p = strchr(p + 10, '"');
        if (!p)
            return 0;
        ++p;
        i = 0;
        while (*p && *p != '"' && i + 1 < cap)
            out[i++] = *p++;
        out[i] = '\0';
        return i > 0;
    }
    p = strstr(text, "/releases/tag/");
    if (!p)
        return 0;
    p += strlen("/releases/tag/");
    i = 0;
    while (*p && *p != '"' && *p != '\'' && *p != ' ' && *p != '\n' &&
           *p != '\r' && *p != '?' && *p != '#' && i + 1 < cap)
        out[i++] = *p++;
    out[i] = '\0';
    return i > 0;
}

/* Query GitHub for the latest cmake-clang-v1 release tag (short timeout). */
static int host_remote_toolchain_version(char* out, size_t cap) {
    char url[320], tmp[1400], buf[8192];
    FILE* f;
    size_t n;
    char err[256];
    if (!out || cap < 2)
        return 0;
    out[0] = '\0';
    snprintf(url, sizeof(url),
             "https://api.github.com/repos/%s/releases/latest", k_tc_repo);
#if defined(_WIN32)
    {
        char tdir[512];
        DWORD tn = GetTempPathA(sizeof(tdir), tdir);
        if (tn == 0 || tn >= sizeof(tdir))
            return 0;
        snprintf(tmp, sizeof(tmp), "%spsxrecomp-tc-latest-%lu.json", tdir,
                 (unsigned long)GetCurrentProcessId());
    }
#else
    snprintf(tmp, sizeof(tmp), "/tmp/psxrecomp-tc-latest-%d.json", (int)getpid());
#endif
    /* GitHub API wants a UA; keep the request short so wizard open stays snappy. */
#if defined(_WIN32)
    {
        char cmd[4096];
        DWORD code = 1;
        DeleteFileA(tmp);
        snprintf(cmd, sizeof(cmd),
                 "curl.exe -fsSL --connect-timeout 5 --max-time 15 "
                 "-A psxrecomp-codegen -H \"Accept: application/vnd.github+json\" "
                 "-o \"%s\" \"%s\"",
                 tmp, url);
        if (!run_cmdline_wait(cmd, &code) || code != 0 || !path_is_file(tmp)) {
            /* Fallback via cmd so stdout redirect works under CreateProcess. */
            DeleteFileA(tmp);
            snprintf(cmd, sizeof(cmd),
                     "cmd.exe /C \"curl.exe -fsSIL --connect-timeout 5 "
                     "--max-time 15 -A psxrecomp-codegen -o NUL "
                     "-w %%{url_effective} "
                     "https://github.com/%s/releases/latest > \"%s\"\"",
                     k_tc_repo, tmp);
            if (!run_cmdline_wait(cmd, &code) || code != 0 || !path_is_file(tmp))
                return 0;
        }
    }
#else
    {
        char cmd[4096];
        unlink(tmp);
        snprintf(cmd, sizeof(cmd),
                 "curl -fsSL --connect-timeout 5 --max-time 15 "
                 "-A psxrecomp-codegen -H 'Accept: application/vnd.github+json' "
                 "-o '%s' '%s'",
                 tmp, url);
        if (system(cmd) != 0 || !path_is_file(tmp)) {
            unlink(tmp);
            snprintf(cmd, sizeof(cmd),
                     "curl -fsSIL --connect-timeout 5 --max-time 15 "
                     "-A psxrecomp-codegen -o /dev/null -w '%%{url_effective}' "
                     "'https://github.com/%s/releases/latest' > '%s'",
                     k_tc_repo, tmp);
            if (system(cmd) != 0 || !path_is_file(tmp))
                return 0;
        }
    }
#endif
    (void)err;
    f = fopen(tmp, "rb");
    if (!f) {
#if defined(_WIN32)
        DeleteFileA(tmp);
#else
        unlink(tmp);
#endif
        return 0;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
#if defined(_WIN32)
    DeleteFileA(tmp);
#else
    unlink(tmp);
#endif
    buf[n] = '\0';
    return parse_github_release_tag(buf, out, cap);
}

/* 1 = installed pack is older than GitHub /releases/latest (prompt to update).
 * 0 = up to date, not installed, offline, or RETCOMM_TOOLCHAIN_SKIP_UPDATE.
 * Always fills local/remote when discoverable. */
static int host_toolchain_update_available(char* local_ver, size_t local_cap,
                                           char* remote_ver, size_t remote_cap) {
    const char* skip = getenv("RETCOMM_TOOLCHAIN_SKIP_UPDATE");
    if (local_ver && local_cap)
        local_ver[0] = '\0';
    if (remote_ver && remote_cap)
        remote_ver[0] = '\0';
    if (skip && skip[0] && skip[0] != '0')
        return 0;
    if (!g_project_root[0])
        return 0;
    migrate_legacy_psxrecomp_toolchain();
    (void)host_local_toolchain_version(local_ver, local_cap);
    if (!host_remote_toolchain_version(remote_ver, remote_cap))
        return 0;
    if (!local_ver || !local_ver[0])
        return 0; /* missing install → page 0 is "install", not "update" */
    if (!remote_ver || !remote_ver[0])
        return 0;
    return version_cmp(local_ver, remote_ver) < 0;
}

/* Download or offline-install cmake-clang-v1 (wizard page 0 / rebuild fallback).
 * Prefer host-native curl/tar so Microsoft Store Python cannot redirect the
 * unpack into Packages\\...\\LocalCache. Installs into the shared RetComM
 * cache: %LOCALAPPDATA%/retcomm/toolchains/cmake-clang-v1/…
 * Broken latest/ stamps are healed, then GitHub /releases/latest is fetched.
 *
 * download: 0 = zip/cache only, 1 = download if missing, 2 = force latest. */
static int host_ensure_toolchain_with_progress(
    int download, const char* zip_path, char* err_msg, size_t err_cap,
    RecompLauncherCPrepareProgressFn on_progress, void* progress_ctx) {
    const int force = (download == 2);
    if (!g_project_root[0]) {
        snprintf(err_msg, err_cap, "Project root is not available.");
        return 0;
    }
    migrate_legacy_psxrecomp_toolchain();
    activate_toolchain_path();
    if (!force && host_portable_cmake_ready())
        return 1;

#if defined(_WIN32)
    if (!force) {
        if (on_progress)
            on_progress(progress_ctx, 0.02f,
                        "Checking for an existing portable toolchain…");
        if (harvest_store_python_toolchain(1)) {
            activate_toolchain_path();
            if (host_portable_cmake_ready())
                return 1;
        }
    }
#endif

    if (on_progress)
        on_progress(progress_ctx, 0.03f,
                    force ? "Preparing toolchain update…"
                          : "Repairing broken toolchain cache if needed…");
    heal_broken_toolchain_pointers();
    clear_project_toolchain_stamp();
    g_toolchain_bin[0] = '\0';
    g_cli_toolchain_bin[0] = '\0';
    g_cmake[0] = '\0';
    activate_toolchain_path();
    if (!force && host_portable_cmake_ready())
        return 1;

    if (zip_path && zip_path[0]) {
        if (on_progress)
            on_progress(progress_ctx, 0.05f, "Installing toolchain from zip…");
        if (host_install_toolchain_from_zip(zip_path, on_progress, progress_ctx,
                                            err_msg, err_cap)) {
            if (host_portable_cmake_ready())
                return 1;
            snprintf(err_msg, err_cap,
                     "Toolchain zip installed but cmake will not run"
                     "%s. Try Unblock on the zip, or set "
                     "RETCOMM_TOOLCHAIN_DIR to a pack whose bin\\cmake.exe "
                     "passes --version.",
                     (toolchain_min_version()[0]
                          ? " (or is below RETCOMM_TOOLCHAIN_MIN_VERSION)"
                          : ""));
            return 0;
        }
        return 0;
    }

    if (download) {
        /* Fetch GitHub /releases/latest (no per-title version pin). */
        if (on_progress)
            on_progress(progress_ctx, 0.05f,
                        force ? "Downloading toolchain update…"
                              : "Downloading latest portable cmake/clang…");
        if (host_download_and_install_toolchain(on_progress, progress_ctx,
                                                err_msg, err_cap)) {
            if (host_portable_cmake_ready())
                return 1;
            /* Install saw bin/cmake.exe; ready check failed (won't run / min). */
            snprintf(err_msg, err_cap,
                     "Downloaded toolchain is present but cmake will not run"
                     "%s. Check "
                     "%%LOCALAPPDATA%%\\retcomm\\toolchains\\cmake-clang-v1\\"
                     "<tag>\\bin\\cmake.exe --version, or set "
                     "RETCOMM_TOOLCHAIN_DIR.",
                     (toolchain_min_version()[0]
                          ? " (or is below RETCOMM_TOOLCHAIN_MIN_VERSION)"
                          : ""));
            return 0;
        }
        return 0;
    }

    snprintf(err_msg, err_cap,
             "No portable toolchain found. Enable automatic download, pick a "
             "cmake-clang-v1 zip, or set RETCOMM_TOOLCHAIN_DIR.");
    return 0;
}

/* Rebuild-time fallback if the wizard step was skipped / cache pruned. */
static int host_ensure_toolchain(RecompLauncherCPrepareProgressFn on_progress,
                                 void* progress_ctx, char* err_msg,
                                 size_t err_cap) {
    return host_ensure_toolchain_with_progress(1, NULL, err_msg, err_cap,
                                               on_progress, progress_ctx);
}

static int host_prepare_generate(const char* source_path, char* out_path,
                                 size_t out_cap, char* err_msg, size_t err_cap,
                                 RecompLauncherCPrepareProgressFn on_progress,
                                 void* progress_ctx) {
    if (!g_ready) {
        snprintf(err_msg, err_cap, "Local codegen tools are not available.");
        return 0;
    }
    if (!source_path || !source_path[0]) {
        snprintf(err_msg, err_cap, "No disc selected.");
        return 0;
    }
    activate_toolchain_path();
    if (!find_python(g_python, sizeof(g_python))) {
        /* First-run / pruned cache: page 0 normally installed the pack; heal. */
        if (!host_ensure_toolchain(on_progress, progress_ctx, err_msg, err_cap))
            return 0;
        activate_toolchain_path();
        if (!find_python(g_python, sizeof(g_python))) {
            snprintf(err_msg, err_cap,
                     "No usable Python. Install the portable toolchain step "
                     "(includes CPython under python/), or set RETCOMM_PYTHON.");
            return 0;
        }
    }
    if (on_progress)
        on_progress(progress_ctx, 0.02f, "Starting psxrecomp generate…");

    /* Hand the CLI the launcher's staged disc + BIOS. Empty g_wizard_bios means
     * OpenBIOS unless setup can adopt a retail dump beside the install. */
    {
        char abs_bios[1100];
        if (g_wizard_bios[0] &&
            absolutize_existing_file(abs_bios, sizeof(abs_bios), g_wizard_bios))
            snprintf(g_wizard_bios, sizeof(g_wizard_bios), "%s", abs_bios);
        else if (!g_wizard_bios[0]) {
            /* Intentional OpenBIOS clear stays empty; otherwise adopt SCPH1001
             * from bios.cfg / discovery (resolve_bios_arg). */
            if (!g_wizard_bios_explicit) {
                char found[1100];
                if (resolve_bios_arg(found, sizeof(found)))
                    snprintf(g_wizard_bios, sizeof(g_wizard_bios), "%s", found);
            }
        } else {
            /* Staged path missing — fail clearly instead of silently OpenBIOS. */
            snprintf(err_msg, err_cap, "Staged BIOS not found: %s", g_wizard_bios);
            return 0;
        }
        host_persist_setup(NULL, source_path,
                           g_wizard_bios[0] ? g_wizard_bios : "");
    }

    char bios_path[1100];
    bios_path[0] = '\0';
    const int have_bios =
        g_wizard_bios[0] &&
        absolutize_existing_file(bios_path, sizeof(bios_path), g_wizard_bios);
    fprintf(stderr, "psxrecomp-codegen: generate disc=%s bios=%s\n", source_path,
            have_bios ? bios_path : "(OpenBIOS)");

#if defined(_WIN32)
    char cmdline[4096];
    if (have_bios) {
        snprintf(cmdline, sizeof(cmdline),
                 "\"%s\" \"%s\" generate --project-root \"%s\" --config \"%s\" "
                 "--disc \"%s\" --bios \"%s\" --json-progress",
                 g_python, g_cli_path, g_project_root, g_game_toml, source_path,
                 bios_path);
    } else {
        snprintf(cmdline, sizeof(cmdline),
                 "\"%s\" \"%s\" generate --project-root \"%s\" --config \"%s\" "
                 "--disc \"%s\" --json-progress",
                 g_python, g_cli_path, g_project_root, g_game_toml, source_path);
    }
    if (!run_cli_win(cmdline, on_progress, progress_ctx, err_msg, err_cap,
                     "psxrecomp generate"))
        return 0;
#else
    char* argv[16];
    int argc = 0;
    argv[argc++] = g_python;
    argv[argc++] = g_cli_path;
    argv[argc++] = "generate";
    argv[argc++] = "--project-root";
    argv[argc++] = g_project_root;
    argv[argc++] = "--config";
    argv[argc++] = g_game_toml;
    argv[argc++] = "--disc";
    argv[argc++] = (char*)source_path;
    if (have_bios) {
        argv[argc++] = "--bios";
        argv[argc++] = bios_path;
    }
    argv[argc++] = "--json-progress";
    argv[argc] = NULL;
    if (!run_cli_posix(argv, on_progress, progress_ctx, err_msg, err_cap,
                       "psxrecomp generate"))
        return 0;
#endif

    snprintf(out_path, out_cap, "%s", source_path);
    if (on_progress)
        on_progress(progress_ctx, 1.0f, "Generate complete");
    return 1;
}

#if defined(_WIN32)
static void bat_write_set(FILE* f, const char* name, const char* value) {
    fprintf(f, "set \"%s=", name);
    for (const char* p = value; *p; ++p) {
        if (*p == '%')
            fputc('%', f);
        fputc(*p, f);
    }
    fprintf(f, "\"\r\n");
}

/* force_pgo: Settings Optimize FMV (instrument → train → use). Else setup
 * rebuild with --no-pgo. disc_path required when force_pgo. */
static int write_windows_deferred_rebuild_helper(int force_pgo,
                                                 const char* disc_path,
                                                 char* err_msg,
                                                 size_t err_cap) {
    if (!join_path(g_helper_path, sizeof(g_helper_path), g_build_dir,
                   "recomp_deferred_rebuild.cmd")) {
        snprintf(err_msg, err_cap, "Failed to form helper path.");
        return 0;
    }
    /* Setup zips omit build-release/; create it before writing the .cmd. */
    if (!mkdir_p(g_build_dir)) {
        snprintf(err_msg, err_cap, "Failed to create build dir: %s",
                 g_build_dir);
        return 0;
    }
    FILE* f = fopen(g_helper_path, "wb");
    if (!f) {
        snprintf(err_msg, err_cap, "Failed to write rebuild helper: %s",
                 g_helper_path);
        return 0;
    }
    char pid_buf[32];
    snprintf(pid_buf, sizeof(pid_buf), "%lu",
             (unsigned long)GetCurrentProcessId());
    fprintf(f, "@echo off\r\n");
    fprintf(f, "setlocal EnableExtensions\r\n");
    fprintf(f, "title %s - %s\r\n", g_display,
            force_pgo ? "PGO optimize" : "rebuilding");
    bat_write_set(f, "PARENT_PID", pid_buf);
    bat_write_set(f, "PYTHON", g_python);
    bat_write_set(f, "CLI", g_cli_path);
    bat_write_set(f, "ROOT", g_project_root);
    bat_write_set(f, "CONFIG", g_game_toml);
    bat_write_set(f, "BUILD_DIR", g_build_dir);
    bat_write_set(f, "TARGET", g_cmake_target);
    bat_write_set(f, "EXE_BASE", g_exe_basename);
    bat_write_set(f, "EXE", g_exe_path);
    bat_write_set(f, "DISPLAY", g_display);
    if (force_pgo && disc_path && disc_path[0])
        bat_write_set(f, "DISC", disc_path);
    if (g_toolchain_bin[0])
        bat_write_set(f, "TC_BIN", g_toolchain_bin);
    fprintf(f,
            "echo Waiting for %%DISPLAY%% to exit...\r\n"
            ":waitloop\r\n"
            "tasklist /FI \"PID eq %%PARENT_PID%%\" 2>NUL | "
            "findstr /I \"%%PARENT_PID%%\" >NUL\r\n"
            "if not errorlevel 1 (\r\n"
            "  ping -n 2 127.0.0.1 >NUL\r\n"
            "  goto waitloop\r\n"
            ")\r\n"
            "echo Ensuring toolchain...\r\n"
            "cd /d \"%%ROOT%%\"\r\n"
            "if defined TC_BIN set \"PATH=%%TC_BIN%%;%%PATH%%\"\r\n"
            "\"%%PYTHON%%\" \"%%CLI%%\" ensure-toolchain --project-root \"%%ROOT%%\"\r\n"
            "if errorlevel 1 (\r\n"
            "  echo.\r\n"
            "  echo Toolchain missing. Download cmake-clang-v1 or set\r\n"
            "  echo RETCOMM_TOOLCHAIN_DIR / pass --toolchain-zip on rebuild.\r\n"
            "  pause\r\n"
            "  exit /b 1\r\n"
            ")\r\n");
    if (force_pgo) {
        fprintf(f,
                "echo PGO optimize (instrument + train + rebuild)...\r\n"
                "\"%%PYTHON%%\" \"%%CLI%%\" rebuild --project-root \"%%ROOT%%\" "
                "--config \"%%CONFIG%%\" --build-dir \"%%BUILD_DIR%%\" "
                "--target \"%%TARGET%%\" --exe-basename \"%%EXE_BASE%%\" "
                "--disc \"%%DISC%%\" --force-pgo --pgo-video\r\n");
    } else {
        fprintf(f,
                "echo Building...\r\n"
                "\"%%PYTHON%%\" \"%%CLI%%\" rebuild --project-root \"%%ROOT%%\" "
                "--config \"%%CONFIG%%\" --build-dir \"%%BUILD_DIR%%\" "
                "--target \"%%TARGET%%\" --exe-basename \"%%EXE_BASE%%\" "
                "--no-pgo --prune-after build-intermediates\r\n");
    }
    fprintf(f,
            "if errorlevel 1 (\r\n"
            "  echo.\r\n"
            "  echo Build failed. Fix the errors above, then rebuild manually.\r\n"
            "  pause\r\n"
            "  exit /b 1\r\n"
            ")\r\n"
            "echo Starting %%DISPLAY%%...\r\n"
            "start \"\" /D \"%%ROOT%%\" \"%%EXE%%\" --launcher\r\n"
            "endlocal\r\n");
    fclose(f);
    return 1;
}
#endif

static int host_rebuild_game_ex(const char* disc_path, int force_pgo,
                                char* out_exe_path, size_t out_cap,
                                char* err_msg, size_t err_cap,
                                RecompLauncherCPrepareProgressFn on_progress,
                                void* progress_ctx) {
    g_relaunch_is_helper = 0;
    if (!g_ready || !g_build_dir[0]) {
        snprintf(err_msg, err_cap, "CMake build environment is not available.");
        return 0;
    }
    if (force_pgo) {
        if (psxrecomp_codegen_host_sources_missing(g_cfg)) {
            snprintf(err_msg, err_cap,
                     "Generated game C is missing — finish Generate & rebuild "
                     "in the setup wizard first.");
            return 0;
        }
        if (!disc_path || !disc_path[0] || !path_is_file(disc_path)) {
            snprintf(err_msg, err_cap,
                     "Select a playable disc image before optimizing FMV.");
            return 0;
        }
    }

    if (!host_ensure_toolchain(on_progress, progress_ctx, err_msg, err_cap))
        return 0;

    activate_toolchain_path();
    if (!find_python(g_python, sizeof(g_python))) {
        snprintf(err_msg, err_cap,
                 "No usable Python. Install the portable toolchain step "
                 "(includes CPython under python/), or set RETCOMM_PYTHON.");
        return 0;
    }

#if defined(_WIN32)
    if (on_progress)
        on_progress(progress_ctx, 0.4f,
                    force_pgo ? "Scheduling Windows PGO optimize after exit…"
                              : "Scheduling Windows rebuild after exit…");
    if (!write_windows_deferred_rebuild_helper(force_pgo, disc_path, err_msg,
                                               err_cap))
        return 0;
    g_relaunch_is_helper = 1;
    snprintf(out_exe_path, out_cap, "%s", g_helper_path);
    if (on_progress)
        on_progress(progress_ctx, 1.0f,
                    force_pgo
                        ? "Exiting so Windows can run PGO train + rebuild…"
                        : "Exiting so Windows can rebuild safely…");
    return 1;
#else
    if (on_progress)
        on_progress(progress_ctx, 0.05f,
                    force_pgo ? "Starting FMV PGO optimize…"
                              : "Starting rebuild (cmake)…");

    char disc_arg_storage[1100];
    char* argv[40];
    int argc = 0;
    argv[argc++] = g_python;
    argv[argc++] = g_cli_path;
    argv[argc++] = "rebuild";
    argv[argc++] = "--project-root";
    argv[argc++] = g_project_root;
    argv[argc++] = "--config";
    argv[argc++] = g_game_toml;
    argv[argc++] = "--build-dir";
    argv[argc++] = g_build_dir;
    argv[argc++] = "--target";
    argv[argc++] = g_cmake_target;
    argv[argc++] = "--exe-basename";
    argv[argc++] = g_exe_basename;
    if (disc_path && disc_path[0]) {
        snprintf(disc_arg_storage, sizeof(disc_arg_storage), "%s", disc_path);
        argv[argc++] = "--disc";
        argv[argc++] = disc_arg_storage;
    }
    if (force_pgo) {
        argv[argc++] = "--force-pgo";
        argv[argc++] = "--pgo-video";
    } else {
        argv[argc++] = "--no-pgo";
        argv[argc++] = "--prune-after";
        argv[argc++] = "build-intermediates";
    }
    argv[argc++] = "--json-progress";
    argv[argc] = NULL;

    if (!run_cli_posix(argv, on_progress, progress_ctx, err_msg, err_cap,
                       force_pgo ? "psxrecomp PGO optimize"
                                 : "psxrecomp rebuild"))
        return 0;
    if (!path_is_file(g_exe_path)) {
        snprintf(err_msg, err_cap, "Build succeeded but binary missing: %s",
                 g_exe_path);
        return 0;
    }
    snprintf(out_exe_path, out_cap, "%s", g_exe_path);
    if (on_progress)
        on_progress(progress_ctx, 1.0f,
                    force_pgo ? "FMV optimize complete" : "Build complete");
    return 1;
#endif
}

static int host_rebuild_game(const char* disc_path, char* out_exe_path,
                             size_t out_cap, char* err_msg, size_t err_cap,
                             RecompLauncherCPrepareProgressFn on_progress,
                             void* progress_ctx) {
    return host_rebuild_game_ex(disc_path, 0, out_exe_path, out_cap, err_msg,
                                err_cap, on_progress, progress_ctx);
}

static int host_pgo_optimize(const char* disc_path, char* out_exe_path,
                             size_t out_cap, char* err_msg, size_t err_cap,
                             RecompLauncherCPrepareProgressFn on_progress,
                             void* progress_ctx) {
    return host_rebuild_game_ex(disc_path, 1, out_exe_path, out_cap, err_msg,
                                err_cap, on_progress, progress_ctx);
}

/* Settings → VIDEO → Apply FMV Timing Opt: regenerate C (picks up
 * load_charge_batch) then rebuild without PGO training. */
static int host_fmv_timing_optimize(const char* disc_path, char* out_exe_path,
                                    size_t out_cap, char* err_msg,
                                    size_t err_cap,
                                    RecompLauncherCPrepareProgressFn on_progress,
                                    void* progress_ctx) {
    char gen_out[512];
    if (!host_prepare_generate(disc_path, gen_out, sizeof(gen_out), err_msg,
                               err_cap, on_progress, progress_ctx))
        return 0;
    return host_rebuild_game_ex(disc_path, 0, out_exe_path, out_cap, err_msg,
                                err_cap, on_progress, progress_ctx);
}

static int host_self_exe_path(char* out, size_t cap) {
    if (!out || cap < 2)
        return 0;
    out[0] = '\0';
#if defined(_WIN32)
    {
        DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
        return n > 0 && n < (DWORD)cap;
    }
#else
    {
        const char* appimg = getenv("APPIMAGE");
        if (appimg && appimg[0] && path_is_file(appimg)) {
            snprintf(out, cap, "%s", appimg);
            return 1;
        }
        char* rp = realpath("/proc/self/exe", NULL);
        if (!rp)
            return 0;
        snprintf(out, cap, "%s", rp);
        free(rp);
        return out[0] != '\0';
    }
#endif
}

static int host_paths_same_file(const char* a, const char* b) {
    char fa[1100], fb[1100];
    if (!a || !a[0] || !b || !b[0])
        return 0;
#if defined(_WIN32)
    {
        DWORD na = GetFullPathNameA(a, (DWORD)sizeof(fa), fa, NULL);
        DWORD nb = GetFullPathNameA(b, (DWORD)sizeof(fb), fb, NULL);
        if (na == 0 || na >= (DWORD)sizeof(fa) || nb == 0 ||
            nb >= (DWORD)sizeof(fb))
            return 0;
        return _stricmp(fa, fb) == 0;
    }
#else
    {
        char* ra = realpath(a, NULL);
        char* rb = realpath(b, NULL);
        int same = ra && rb && strcmp(ra, rb) == 0;
        free(ra);
        free(rb);
        return same;
    }
#endif
}

/* Setup-host zip-root exe → build-release product (bios/mods/assets/settings). */
void psxrecomp_codegen_host_forward_if_built(
    const PsxrecompCodegenHostConfig* cfg, int argc, char** argv) {
#if defined(PSX_HAS_GAME_DISPATCH)
    /* Full game binary — already the product tree. */
    (void)cfg;
    (void)argc;
    (void)argv;
    return;
#else
    char self[1100];
    const char* no_fwd;
    const char* force_env;
    const char* force;

    if (!cfg || !cfg->cmake_target || !cfg->exe_basename)
        return;

    no_fwd = getenv("PSXRECOMP_NO_FORWARD");
    if (no_fwd && no_fwd[0] && no_fwd[0] != '0')
        return;

    g_cfg = cfg;
    snprintf(g_display, sizeof(g_display), "%s",
             cfg_or(cfg->display_name, "Game"));
    snprintf(g_cmake_target, sizeof(g_cmake_target), "%s", cfg->cmake_target);
    snprintf(g_exe_basename, sizeof(g_exe_basename), "%s", cfg->exe_basename);

    force_env = cfg_or(cfg->force_setup_env, "PSXRECOMP_FORCE_SETUP");
    force = getenv(force_env);
    if (force && force[0] && force[0] != '0')
        return;

    /* Still need Generate — stay on the setup host. */
    if (psxrecomp_codegen_host_sources_missing(cfg))
        return;

    if (!discover_project_root(g_project_root, sizeof(g_project_root)))
        return;
    if (!resolve_build_paths())
        return;
    if (!g_exe_path[0] || !path_is_file(g_exe_path))
        return;
    if (!host_self_exe_path(self, sizeof(self)))
        return;
    if (host_paths_same_file(self, g_exe_path))
        return;

    fprintf(stderr,
            "psxrecomp-codegen: setup host forwarding to product build:\n  %s\n",
            g_exe_path);

#if defined(_WIN32)
    {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        char cmd[4096];
        size_t pos = 0;
        int i;
        memset(&si, 0, sizeof(si));
        memset(&pi, 0, sizeof(pi));
        si.cb = sizeof(si);
        pos += (size_t)snprintf(cmd + pos, sizeof(cmd) - pos, "\"%s\"",
                                g_exe_path);
        /* Preserve caller argv (skip argv[0]); ensure --launcher so skip_launcher
         * in the product tree cannot hide the UI on first handoff. */
        {
            int has_launcher = 0;
            for (i = 1; i < argc && argv && argv[i]; ++i) {
                int n;
                if (strcmp(argv[i], "--launcher") == 0)
                    has_launcher = 1;
                n = snprintf(cmd + pos, sizeof(cmd) - pos, " \"%s\"", argv[i]);
                if (n <= 0 || (size_t)n >= sizeof(cmd) - pos)
                    break;
                pos += (size_t)n;
            }
            if (!has_launcher && pos + 12 < sizeof(cmd))
                snprintf(cmd + pos, sizeof(cmd) - pos, " --launcher");
        }
        if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL,
                            g_project_root[0] ? g_project_root : NULL, &si,
                            &pi)) {
            fprintf(stderr,
                    "psxrecomp-codegen: failed to start product exe (error %lu)\n",
                    (unsigned long)GetLastError());
            return;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ExitProcess(0);
    }
#else
    {
        char** args;
        int i;
        int narg = (argc > 0) ? argc : 1;
        int has_launcher = 0;
        for (i = 1; i < argc && argv && argv[i]; ++i) {
            if (strcmp(argv[i], "--launcher") == 0) {
                has_launcher = 1;
                break;
            }
        }
        args = (char**)calloc((size_t)narg + 2, sizeof(char*));
        if (!args)
            return;
        args[0] = g_exe_path;
        for (i = 1; i < argc && argv && argv[i]; ++i)
            args[i] = argv[i];
        if (!has_launcher)
            args[i++] = "--launcher";
        args[i] = NULL;
        if (g_project_root[0] && chdir(g_project_root) != 0) {
            fprintf(stderr, "psxrecomp-codegen: chdir(%s) failed: %s\n",
                    g_project_root, strerror(errno));
        }
        execv(g_exe_path, args);
        perror("psxrecomp-codegen: execv product exe failed");
        free(args);
    }
#endif
#endif /* !PSX_HAS_GAME_DISPATCH */
}

void psxrecomp_codegen_host_relaunch_or_exit(const char* disc_path) {
    char exe[512];
    const char* near_exe;
    if (!recomp_launcher_relaunch_exe(exe, sizeof(exe)) || !exe[0]) {
        fprintf(stderr, "psxrecomp-codegen: relaunch requested but no path\n");
        exit(1);
    }
    /* Prefer the final game binary (build/<exe>) over a Windows helper bat. */
    near_exe = g_exe_path[0] ? g_exe_path : exe;
    persist_relaunch_sidecars(near_exe, disc_path);

#if defined(_WIN32)
    {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        char cmd[1536];
        DWORD flags = 0;
        memset(&si, 0, sizeof(si));
        memset(&pi, 0, sizeof(pi));
        si.cb = sizeof(si);
        if (g_relaunch_is_helper) {
            fprintf(stderr,
                    "psxrecomp-codegen: starting deferred rebuild helper\n");
            snprintf(cmd, sizeof(cmd), "cmd.exe /C \"%s\"", exe);
            flags = CREATE_NEW_CONSOLE;
        } else {
            fprintf(stderr, "psxrecomp-codegen: relaunching %s\n", exe);
            snprintf(cmd, sizeof(cmd), "\"%s\" --launcher", exe);
        }
        if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, flags, NULL,
                            g_project_root, &si, &pi)) {
            fprintf(stderr, "psxrecomp-codegen: CreateProcess failed\n");
            exit(1);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ExitProcess(0);
    }
#else
    {
        if (g_project_root[0] && chdir(g_project_root) != 0) {
            fprintf(stderr, "psxrecomp-codegen: chdir(%s) failed: %s\n",
                    g_project_root, strerror(errno));
        }
        fprintf(stderr, "psxrecomp-codegen: relaunching %s\n", exe);
        char* args[] = {exe, "--launcher", NULL};
        execv(exe, args);
        perror("psxrecomp-codegen: execv failed");
        exit(1);
    }
#endif
}

void psxrecomp_codegen_host_apply(RecompLauncherCGameInfo* gi,
                                  const PsxrecompCodegenHostConfig* cfg) {
    if (!gi || !cfg || !cfg->cmake_target || !cfg->exe_basename)
        return;

#if !defined(PSX_HAS_SETUP_WIZARD)
    /* Build did not opt into the setup-wizard product surface
     * (-DPSX_SETUP_WIZARD=ON / ENABLE_SETUP_WIZARD). Leave GameInfo dark so
     * recomp-ui never opens first-run / Generate & rebuild. */
    (void)cfg;
    return;
#else

    g_cfg = cfg;
    g_ready = 0;
    g_relaunch_is_helper = 0;
    g_project_root[0] = '\0';
    g_cli_path[0] = '\0';
    g_game_toml[0] = '\0';
    g_python[0] = '\0';
    g_cmake[0] = '\0';
    g_build_dir[0] = '\0';
    g_exe_path[0] = '\0';
    g_helper_path[0] = '\0';
    g_toolchain_bin[0] = '\0';
    g_cli_toolchain_bin[0] = '\0';
    /* Keep g_wizard_bios across re-apply within the same process. */

    snprintf(g_display, sizeof(g_display), "%s",
             cfg_or(cfg->display_name, "Game"));
    snprintf(g_cmake_target, sizeof(g_cmake_target), "%s", cfg->cmake_target);
    snprintf(g_exe_basename, sizeof(g_exe_basename), "%s", cfg->exe_basename);

    const char* force_env =
        cfg_or(cfg->force_setup_env, "PSXRECOMP_FORCE_SETUP");
    const char* force = getenv(force_env);
    const int force_setup = force && force[0] && force[0] != '0';

    /* Master switch for recomp-ui: first-run wizard + Generate & rebuild. */
    gi->setup_wizard_supported = 1;
    /* Setup SDKs often link OpenBIOS only (retail C comes from Generate).
     * psx_bios_has_selectable() is then 0 and would hide the BIOS row — keep
     * the optional SCPH1001 picker so prepare can ingest a dump first. */
    gi->has_bios = 1;

    if (!discover_project_root(g_project_root, sizeof(g_project_root))) {
        /* Still force the wizard when generated/ is missing — discover may
         * fail if the process cwd is unrelated to the project tree. */
        if (force_setup) {
            gi->needs_setup = 1;
            gi->prepare_required_before_continue = 1;
        }
        return;
    }
    if (!resolve_cli_path(g_project_root, g_cli_path, sizeof(g_cli_path))) {
        if (psxrecomp_codegen_host_sources_missing(cfg) || force_setup) {
            gi->needs_setup = 1;
            gi->prepare_required_before_continue = 1;
        }
        return;
    }
    if (!join_path(g_game_toml, sizeof(g_game_toml), g_project_root,
                   cfg_or(cfg->game_toml_relpath, "game.toml")))
        return;
    if (!path_is_file(g_game_toml))
        return;

    /* Wire prepare/rebuild even when Python is not on PATH yet — wizard page 0
     * installs cmake-clang-v1 (1.0.6+ ships portable CPython under python/).
     * generate/rebuild re-resolve after activate_toolchain_path. */
    g_ready = 1;
    activate_toolchain_path();
    (void)find_python(g_python, sizeof(g_python));
    gi->persist_setup = host_persist_setup;
    gi->persist_setup_ctx = NULL;
    gi->prepare_with_progress = host_prepare_generate;
    gi->prepare_use_selected_rom = 1;
    /* Number prefix is applied in the setup UI (BIOS adds a step). */
    gi->prepare_section_title = "Generate BIOS + game C & rebuild";
    gi->prepare_busy_status = "Generating BIOS + game sources…";
    gi->prepare_success_status = "Sources ready — building…";

    /* Rebuild is offered whenever the build tree can be formed; the wizard
     * installs cmake-clang-v1 on page 0 before Generate & rebuild. */
    const int can_rebuild = resolve_build_paths();
    if (can_rebuild) {
        gi->prepare_disc_label = "Generate & rebuild…";
#if defined(_WIN32)
        gi->prepare_disc_note =
            cfg->prepare_note_windows
                ? cfg->prepare_note_windows
                : "Uses your disc with the local psxrecomp SDK to regenerate "
                  "generated/, then quits and rebuilds via a helper so the "
                  "running .exe is not locked.";
        gi->rebuild_busy_status = "Scheduling rebuild…";
        gi->rebuild_success_status =
            "Exiting for Windows rebuild — a console will finish the build…";
#else
        gi->prepare_disc_note =
            cfg->prepare_note
                ? cfg->prepare_note
                : "Uses your disc with the local psxrecomp SDK to regenerate "
                  "generated/, then runs cmake --build and restarts.";
        gi->rebuild_busy_status = "Building…";
        gi->rebuild_success_status = "Build complete — restarting…";
#endif
        gi->rebuild_with_progress = host_rebuild_game;
        gi->rebuild_after_prepare = 1;
        gi->relaunch_after_rebuild = 1;
        gi->setup_needs_toolchain = 1;
        gi->toolchain_is_ready = host_toolchain_is_ready;
        gi->ensure_toolchain_with_progress = host_ensure_toolchain_with_progress;
        gi->toolchain_update_available = host_toolchain_update_available;
        gi->toolchain_repair_note = host_toolchain_repair_note;
        /* Settings → SYSTEM: PGO on existing generated C (no wizard). */
        gi->pgo_optimize_with_progress = host_pgo_optimize;
#if defined(_WIN32)
        gi->pgo_busy_status = "Scheduling FMV PGO optimize…";
        gi->pgo_success_status =
            "Exiting for Windows PGO train + rebuild…";
#else
        gi->pgo_busy_status =
            "Optimizing FMV (instrument → train → rebuild)…";
        gi->pgo_success_status = "FMV optimize complete — restarting…";
#endif
        /* FMV timing (emitter load_charge_batch) generate+rebuild: host kept
         * but not exposed in Settings for now. */
    } else {
        gi->prepare_disc_label = "Generate sources…";
        gi->prepare_disc_note =
            cfg->prepare_note_no_cmake
                ? cfg->prepare_note_no_cmake
                : "Regenerates generated/ with the local psxrecomp SDK. "
                  "Build dir could not be resolved — rebuild manually, then "
                  "relaunch.";
        gi->prepare_success_status =
            "Sources generated. Rebuild manually, then relaunch.";
    }
    gi->persist_setup = host_persist_setup;
    gi->persist_setup_ctx = NULL;

    if (psxrecomp_codegen_host_sources_missing(cfg) || force_setup) {
        gi->needs_setup = 1;
        gi->prepare_required_before_continue = 1;
    }
#endif /* PSX_HAS_SETUP_WIZARD */
}
