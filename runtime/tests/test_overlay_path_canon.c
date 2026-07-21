/* Boundary test for overlay_path_canon: a confinement check is only sound if
 * `..` segments, mixed separators, and reparse points all resolve BEFORE the
 * prefix compare. Builds a scratch tree under %TEMP% / $TMPDIR:
 *   <root>/cache/gcc/aaaaaaaa_bbbbbbbb.dll
 *   <root>/outside/evil.dll
 * and proves the canonical form of every escape spelling lands outside
 * <root>/cache while every legitimate spelling lands inside. */
#include "overlay_path_canon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0700)
#endif

static int s_fails = 0;

static void expect(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        s_fails++;
    }
}

/* Same contract as the loader's cache_path_has_root: prefix match plus a
 * separator, case-insensitive on the letters (NTFS). */
static int has_root(const char *path, const char *root) {
    size_t n = strlen(root);
    while (n && (root[n - 1] == '/' || root[n - 1] == '\\')) n--;
    for (size_t i = 0; i < n; i++) {
        char a = path[i], b = root[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if ((a == '/' || a == '\\') && (b == '/' || b == '\\')) continue;
        if (a != b) return 0;
    }
    return path[n] == '/' || path[n] == '\\';
}

static void touch(const char *path) {
    FILE *f = fopen(path, "wb");
    if (f) {
        fputc('x', f);
        fclose(f);
    }
}

int main(void) {
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMPDIR");
    if (!tmp) tmp = ".";
    char root[512], cache[512], gcc_dir[512], outside[512];
    snprintf(root, sizeof(root), "%s/canon_test_%u", tmp,
             (unsigned)(size_t)&s_fails);
    snprintf(cache, sizeof(cache), "%s/cache", root);
    snprintf(gcc_dir, sizeof(gcc_dir), "%s/gcc", cache);
    snprintf(outside, sizeof(outside), "%s/outside", root);
    MKDIR(root);
    MKDIR(cache);
    MKDIR(gcc_dir);
    MKDIR(outside);

    char good[512], evil[512];
    snprintf(good, sizeof(good), "%s/aaaaaaaa_bbbbbbbb.dll", gcc_dir);
    snprintf(evil, sizeof(evil), "%s/evil.dll", outside);
    touch(good);
    touch(evil);

    char canon_root[768], canon[768];
    expect(overlay_path_canonicalize(cache, canon_root, sizeof(canon_root)),
           "cache root canonicalizes");

    /* Legitimate path: canonical form stays inside the root. */
    expect(overlay_path_canonicalize(good, canon, sizeof(canon)),
           "good path canonicalizes");
    expect(has_root(canon, canon_root), "good path stays under root");

    /* Traversal escape: lexically starts with the root, resolves outside.
     * This is the exact spelling that passed the old lexical-only check. */
    char sneaky[1024];
    snprintf(sneaky, sizeof(sneaky), "%s/gcc/../../outside/evil.dll", cache);
    if (overlay_path_canonicalize(sneaky, canon, sizeof(canon)))
        expect(!has_root(canon, canon_root),
               "traversal spelling must NOT canonicalize under root");

    /* Mixed separators on the same escape. */
    snprintf(sneaky, sizeof(sneaky), "%s\\gcc\\..\\..\\outside\\evil.dll",
             cache);
    if (overlay_path_canonicalize(sneaky, canon, sizeof(canon)))
        expect(!has_root(canon, canon_root),
               "mixed-separator traversal must NOT land under root");

    /* Traversal that stays inside must still be accepted (no blocklist). */
    snprintf(sneaky, sizeof(sneaky), "%s/gcc/../gcc/aaaaaaaa_bbbbbbbb.dll",
             cache);
    expect(overlay_path_canonicalize(sneaky, canon, sizeof(canon)) &&
               has_root(canon, canon_root),
           "inside-root traversal still resolves under root");

    /* Nonexistent target fails closed. */
    snprintf(sneaky, sizeof(sneaky), "%s/gcc/missing.dll", cache);
    expect(!overlay_path_canonicalize(sneaky, canon, sizeof(canon)),
           "missing file fails closed");

    /* Undersized output buffer fails closed. */
    char tiny[4];
    expect(!overlay_path_canonicalize(good, tiny, sizeof(tiny)),
           "tiny buffer fails closed");

#ifdef _WIN32
    /* Junction inside the cache pointing outside: the canonical form of a
     * file reached THROUGH it must resolve outside the root. Junction
     * creation can fail without privileges; skip silently then. */
    char link_dir[512], via_link[1024], mkcmd[1600];
    snprintf(link_dir, sizeof(link_dir), "%s/jump", cache);
    snprintf(mkcmd, sizeof(mkcmd),
             "cmd /c mklink /J \"%s\" \"%s\" >nul 2>nul", link_dir, outside);
    if (system(mkcmd) == 0) {
        snprintf(via_link, sizeof(via_link), "%s/evil.dll", link_dir);
        if (overlay_path_canonicalize(via_link, canon, sizeof(canon)))
            expect(!has_root(canon, canon_root),
                   "junction-routed path must NOT land under root");
        RemoveDirectoryA(link_dir);   /* removes the LINK, never the target */
    }
#endif

    if (s_fails) {
        fprintf(stderr, "FAIL: %d canon boundary checks failed\n", s_fails);
        return 1;
    }
    puts("PASS: canonicalization resolves traversal/separators/reparse "
         "before the confinement compare and fails closed");
    return 0;
}
