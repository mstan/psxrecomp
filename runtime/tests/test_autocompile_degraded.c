/*
 * A broken overlay autocompile must SAY SO on a channel that exists.
 *
 * Every diagnostic in autocompile.c is written to stdout, and the shipped
 * runtime links -mwindows: a GUI-subsystem binary with no console. So those
 * warnings reach nobody on the builds people actually run, and a misconfigured
 * `overlay_autocompile_cmd` degrades the whole session to the interpreter in
 * silence. That really happened: a fresh worktree ran with runs=8 fails=8
 * shard_ok=0 and dispatch_native=0 for an entire session, and the only symptom
 * was "it feels slow" - which invalidated a performance comparison.
 *
 * The contract under test: when the command names a file that does not exist,
 * autocompile_configure() records WHY, and autocompile_status_json() carries it
 * out over the TCP debug server as degraded/degraded_reason. Configure-time,
 * not after three failed spawns - the point is to know before any compile is
 * attempted.
 *
 * Build/run: ctest -R autocompile_degraded_test
 */
#include "autocompile.h"
#include "overlay_loader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

/* autocompile.c calls into the overlay loader when it publishes images; none of
 * that runs here (we never spawn a child), but the symbols must resolve.
 * Signatures must match overlay_loader.h exactly. */
struct OverlayPreparedImage { unsigned id; };
OverlayPreparedImage *overlay_loader_prepare_published(const char *dll_path) {
    (void)dll_path; return NULL;
}
int  overlay_loader_commit_published(OverlayPreparedImage *image) { (void)image; return 0; }
void overlay_loader_discard_prepared(OverlayPreparedImage *image) { (void)image; }
void overlay_loader_rescan(void) { }

static int failures;

#define CHECK(cond, msg) do {                                                 \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); failures++; }        \
} while (0)

/* Does the status JSON advertise degraded, and mention `needle`? */
static void expect_status(int want_degraded, const char *needle) {
    char json[4096];
    int n = autocompile_status_json(json, (int)sizeof json);
    CHECK(n > 0 && n < (int)sizeof json, "status json fits");
    const char *flag = want_degraded ? "\"degraded\":1" : "\"degraded\":0";
    if (!strstr(json, flag)) {
        fprintf(stderr, "FAIL: expected %s in status\n  got: %.400s\n", flag, json);
        failures++;
    }
    if (needle && !strstr(json, needle)) {
        fprintf(stderr, "FAIL: expected reason to mention '%s'\n  got: %.400s\n",
                needle, json);
        failures++;
    }
}

int main(void) {
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(sizeof cwd, cwd);

    /* --- a command naming a recompiler that does not exist --------------- */
    /* This is the exact real-world shape: the interpreter resolves fine, the
     * script path may resolve fine, and the --recompiler argument points at a
     * build directory the documented build recipe never produces. */
    char cmd[2048];
    snprintf(cmd, sizeof cmd,
             "py -3 tools/compile_overlays.py --captures build/overlay_captures.json "
             "--recompiler %s\\definitely_not_here\\psxrecomp-game.exe "
             "--out-dir build/cache", cwd);
    autocompile_configure(cmd, cwd);
    CHECK(autocompile_degraded_reason() != NULL,
          "a missing --recompiler path is reported as degraded");
    if (autocompile_degraded_reason())
        CHECK(strstr(autocompile_degraded_reason(), "--recompiler") != NULL,
              "the reason names the offending flag");
    expect_status(1, "--recompiler");

    /* The reason must survive being read repeatedly and must not be
     * overwritten by a later, less-specific cause - the FIRST cause is the
     * useful one. */
    const char *first = autocompile_degraded_reason();
    char kept[600];
    snprintf(kept, sizeof kept, "%s", first ? first : "");
    autocompile_configure(cmd, cwd);
    CHECK(autocompile_degraded_reason() != NULL &&
          strcmp(autocompile_degraded_reason(), kept) == 0,
          "the first recorded cause is retained");

    /* --- the JSON must stay parseable even with a long reason ------------ */
    char json[4096];
    int n = autocompile_status_json(json, (int)sizeof json);
    CHECK(n > 0, "status json emitted");
    CHECK(json[0] == '{' && json[n - 1] == '}', "status json is a complete object");
    CHECK(strstr(json, "\"degraded_reason\":\"") != NULL,
          "degraded_reason field present");
    /* The reason is embedded in a JSON string: it must not contain a raw quote
     * or backslash that would break the object for every consumer. */
    const char *r = strstr(json, "\"degraded_reason\":\"");
    if (r) {
        r += strlen("\"degraded_reason\":\"");
        int escaped_ok = 1;
        for (const char *p = r; *p; p++) {
            if (*p == '"') break;                 /* properly terminated */
            if (*p == '\\') { p++; continue; }    /* escape pair */
            if ((unsigned char)*p < 0x20) { escaped_ok = 0; break; }
        }
        CHECK(escaped_ok, "reason contains no raw control characters");
    }

    if (failures) {
        fprintf(stderr, "FAILED (%d)\n", failures);
        return 1;
    }
    printf("test_autocompile_degraded: all checks passed\n");
    return 0;
}
