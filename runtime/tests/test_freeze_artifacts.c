#include "freeze_artifacts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define TEST_MKDIR(p) _mkdir(p)
#define TEST_RMDIR(p) _rmdir(p)
#define TEST_SEP "\\"
#else
#include <unistd.h>
#define TEST_MKDIR(p) mkdir((p), 0755)
#define TEST_RMDIR(p) rmdir(p)
#define TEST_SEP "/"
#endif

static int failures;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

static int exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void touch_file(const char *path) {
    FILE *f = fopen(path, "wb");
    check(f != NULL, "create fixture");
    if (f) { fputs("{}", f); fclose(f); }
}

int main(void) {
    char dir[256], nested[320], path[512], matching_dir[512];
#ifndef _WIN32
    char matching_link[512], link_target[512];
#endif
    unsigned keep = 0;
#ifdef _WIN32
    snprintf(dir, sizeof(dir), "freeze_artifacts_test_%lu_%llu",
             (unsigned long)GetCurrentProcessId(), (unsigned long long)time(NULL));
#else
    snprintf(dir, sizeof(dir), "freeze_artifacts_test_%ld_%llu",
             (long)getpid(), (unsigned long long)time(NULL));
#endif
    snprintf(nested, sizeof(nested), "%s%sout", dir, TEST_SEP);
    TEST_MKDIR(dir);

    check(freeze_artifacts_dump_path(path, sizeof(path), nested,
                                     "bad/../backend", 123) == 1,
          "build dump path and nested directory");
    check(strstr(path, "bad____backend") != NULL, "sanitize backend path separators");

    for (int i = 1; i <= 5; ++i) {
        snprintf(path, sizeof(path), "%s%spsx_freeze_dump_runtime_%d.json",
                 nested, TEST_SEP, i);
        touch_file(path);
#ifdef _WIN32
        Sleep(20);
#else
        sleep(1);
#endif
    }
    snprintf(path, sizeof(path), "%s%spsx_freeze_dump_runtime_0.json.tmp", nested, TEST_SEP);
    touch_file(path);
    snprintf(matching_dir, sizeof(matching_dir),
             "%s%spsx_freeze_dump_runtime_directory.json", nested, TEST_SEP);
    check(TEST_MKDIR(matching_dir) == 0, "create exact-name directory fixture");
#ifndef _WIN32
    snprintf(link_target, sizeof(link_target), "%s%sprotected-target.txt", nested, TEST_SEP);
    touch_file(link_target);
    snprintf(matching_link, sizeof(matching_link),
             "%s%spsx_freeze_dump_runtime_link.json", nested, TEST_SEP);
    check(symlink("protected-target.txt", matching_link) == 0,
          "create exact-name symlink fixture");
#endif
    {
        char protected_path[512];
        snprintf(protected_path, sizeof(protected_path), "%s%spsx_freeze_dump_runtime_0.json.tmp",
                 nested, TEST_SEP);
        check(freeze_artifacts_prune(nested, 2) == 3, "remove exactly three old dumps");
        check(exists(protected_path), "leave non-matching suffix untouched");
        check(exists(matching_dir), "leave exact-name directory untouched");
#ifndef _WIN32
        check(exists(link_target), "leave symlink target untouched");
        {
            struct stat st;
            check(lstat(matching_link, &st) == 0 && S_ISLNK(st.st_mode),
                  "leave exact-name symlink untouched");
        }
#endif
    }
    snprintf(path, sizeof(path), "%s%spsx_freeze_dump_runtime_5.json", nested, TEST_SEP);
    check(exists(path), "preserve newest dump");
    snprintf(path, sizeof(path), "%s%spsx_freeze_dump_runtime_4.json", nested, TEST_SEP);
    check(exists(path), "preserve second-newest dump");
    snprintf(path, sizeof(path), "%s%spsx_freeze_dump_runtime_3.json", nested, TEST_SEP);
    check(!exists(path), "prune older dump");

#ifdef _WIN32
    _putenv_s("PSX_FREEZE_DUMP_KEEP", "2");
    _putenv_s("PSX_FREEZE_DUMP_DIR", "chosen-dir");
#else
    setenv("PSX_FREEZE_DUMP_KEEP", "2", 1);
    setenv("PSX_FREEZE_DUMP_DIR", "chosen-dir", 1);
#endif
    freeze_artifacts_config(path, sizeof(path), &keep);
    check(strcmp(path, "chosen-dir") == 0 && keep == 2u, "read environment configuration");
#ifdef _WIN32
    _putenv_s("PSX_FREEZE_DUMP_KEEP", "0");
#else
    setenv("PSX_FREEZE_DUMP_KEEP", "0", 1);
#endif
    freeze_artifacts_config(path, sizeof(path), &keep);
    check(keep == FREEZE_ARTIFACT_DEFAULT_KEEP, "reject unbounded keep configuration");

    /* Cleanup exact fixtures only. */
    for (int i = 1; i <= 5; ++i) {
        snprintf(path, sizeof(path), "%s%spsx_freeze_dump_runtime_%d.json", nested, TEST_SEP, i);
        remove(path);
    }
    snprintf(path, sizeof(path), "%s%spsx_freeze_dump_runtime_0.json.tmp", nested, TEST_SEP);
    remove(path);
    TEST_RMDIR(matching_dir);
#ifndef _WIN32
    unlink(matching_link);
    remove(link_target);
#endif
    TEST_RMDIR(nested);
    TEST_RMDIR(dir);
    return failures ? 1 : 0;
}
