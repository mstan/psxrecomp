#include "autocompile.h"
#include "overlay_loader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void autocompile_test_feed_output(const char *buf, int n);
int autocompile_test_start_preparer(void);
void autocompile_test_finish_input(void);
int autocompile_test_join_preparer(DWORD timeout_ms);
int autocompile_test_ready_count(void);
int autocompile_test_ready_highwater(void);
int autocompile_test_preparing_count(void);
void autocompile_test_discard_all(void);

struct OverlayPreparedImage {
    unsigned id;
};

static DWORD s_main_thread;
static LONG s_prepared;
static LONG s_committed;
static LONG s_discarded;
static LONG s_wrong_prepare_thread;
static LONG s_wrong_commit_thread;

OverlayPreparedImage *overlay_loader_prepare_published(const char *path) {
    OverlayPreparedImage *image =
        (OverlayPreparedImage *)calloc(1, sizeof(*image));
    if (!image) return NULL;
    image->id = (unsigned)strtoul(strrchr(path, '_') + 1, NULL, 16);
    if (GetCurrentThreadId() == s_main_thread)
        InterlockedIncrement(&s_wrong_prepare_thread);
    InterlockedIncrement(&s_prepared);
    return image;
}

int overlay_loader_commit_published(OverlayPreparedImage *image) {
    if (GetCurrentThreadId() != s_main_thread)
        InterlockedIncrement(&s_wrong_commit_thread);
    free(image);
    InterlockedIncrement(&s_committed);
    return 1;
}

void overlay_loader_discard_prepared(OverlayPreparedImage *image) {
    free(image);
    InterlockedIncrement(&s_discarded);
}

void overlay_loader_rescan(void) {}

typedef struct {
    const char *text;
    int length;
} FeedArgs;

static DWORD WINAPI feed_thread(void *opaque) {
    FeedArgs *args = (FeedArgs *)opaque;
    autocompile_test_feed_output(args->text, args->length);
    autocompile_test_finish_input();
    return 0;
}

int main(void) {
    enum { ITEM_COUNT = 300 };
    s_main_thread = GetCurrentThreadId();
    /* The shared runtime polls every title. A title with overlay autocompile
     * disabled must not touch the publication lock before configuration. */
    autocompile_poll_main();
    autocompile_configure("unused", ".");

    size_t capacity = (size_t)ITEM_COUNT * 96u;
    char *text = (char *)malloc(capacity);
    if (!text) return 1;
    int length = snprintf(text, capacity,
        "PSX_SHARD_RESULT ok=17 failed=0 skipped=4\n");
    for (unsigned i = 0; i < ITEM_COUNT; ++i) {
        length += snprintf(text + length, capacity - (size_t)length,
            "PSX_SHARD_PUBLISHED C:\\cache\\00010000_DEADBEEF_%08X.dll\n", i);
    }

    FeedArgs args = { text, length };
    if (!autocompile_test_start_preparer()) { free(text); return 1; }
    HANDLE worker = CreateThread(NULL, 0, feed_thread, &args, 0, NULL);
    if (!worker) { free(text); return 1; }
    DWORD deadline = GetTickCount() + 10000u;
    while (s_committed < ITEM_COUNT && GetTickCount() < deadline) {
        autocompile_poll_main();
        Sleep(1);
    }
    if (WaitForSingleObject(worker, 1000) != WAIT_OBJECT_0) {
        fprintf(stderr, "FAIL: bounded producer did not finish\n");
        return 1;
    }
    CloseHandle(worker);
    if (!autocompile_test_join_preparer(1000)) {
        fprintf(stderr, "FAIL: preparer did not exit\n");
        return 1;
    }
    /* Finalize the synthetic run.  The 300 publication lines are well over
     * AC_OUT_CAP, so the result marker at the beginning is no longer present
     * in output_tail.  Its counters must nevertheless survive streaming. */
    autocompile_poll_main();
    free(text);

    char status[2048];
    autocompile_status_json(status, sizeof(status));
    if (!strstr(status, "\"shard_ok\":17") ||
        !strstr(status, "\"shard_fail\":0") ||
        !strstr(status, "\"shard_skipped\":4") ||
        !strstr(status, "\"shard_result_seen\":1")) {
        fprintf(stderr, "FAIL: streamed shard result was lost after tail eviction: %s\n",
                status);
        return 1;
    }

    if (s_prepared != ITEM_COUNT || s_committed != ITEM_COUNT ||
        autocompile_test_ready_count() != 0 ||
        autocompile_test_preparing_count() != 0 ||
        autocompile_test_ready_highwater() > 1 ||
        s_wrong_prepare_thread != 0 || s_wrong_commit_thread != 0) {
        fprintf(stderr,
                "FAIL: bounded handoff prep=%ld commit=%ld ready=%d "
                "preparing=%d highwater=%d wrong=%ld/%ld\n",
                s_prepared, s_committed, autocompile_test_ready_count(),
                autocompile_test_preparing_count(),
                autocompile_test_ready_highwater(),
                s_wrong_prepare_thread, s_wrong_commit_thread);
        autocompile_test_discard_all();
        return 1;
    }

    /* A prepared-but-uncommitted item owns one reference and must be consumed
     * by reset/shutdown discard exactly once. */
    const char extra[] =
        "PSX_SHARD_PUBLISHED C:\\cache\\00010000_DEADBEEF_FFFFFFFF.dll\n";
    args.text = extra;
    args.length = (int)strlen(extra);
    if (!autocompile_test_start_preparer()) return 1;
    worker = CreateThread(NULL, 0, feed_thread, &args, 0, NULL);
    if (!worker) return 1;
    WaitForSingleObject(worker, INFINITE);
    CloseHandle(worker);
    if (!autocompile_test_join_preparer(1000)) return 1;
    autocompile_test_discard_all();
    if (s_discarded != 1 || autocompile_test_ready_count() != 0) {
        fprintf(stderr, "FAIL: prepared reference discard count=%ld\n", s_discarded);
        return 1;
    }

    /* Shutdown mid-run: feed markers, never signal input-done, and tear down
     * while the preparer may be anywhere in its loop. autocompile_shutdown
     * must join the worker (never abandon it) and conserve every reference:
     * prepared == committed + discarded, with nothing left queued. */
    {
        static char feed[8 * 96];
        int len = 0;
        for (unsigned i = 0; i < 8; ++i)
            len += snprintf(feed + len, sizeof(feed) - (size_t)len,
                "PSX_SHARD_PUBLISHED C:\\cache\\00010000_DEADBEEF_%08X.dll\n",
                0x1000u + i);
        if (!autocompile_test_start_preparer()) return 1;
        autocompile_test_feed_output(feed, len);
        autocompile_shutdown();   /* input never finished — stop must win */
    }
    if (autocompile_test_ready_count() != 0 ||
        autocompile_test_preparing_count() != 0 ||
        s_prepared != s_committed + s_discarded) {
        fprintf(stderr,
                "FAIL: shutdown conservation prep=%ld commit=%ld discard=%ld "
                "ready=%d preparing=%d\n",
                s_prepared, s_committed, s_discarded,
                autocompile_test_ready_count(),
                autocompile_test_preparing_count());
        return 1;
    }

    puts("PASS: publications prepare off-thread, backpressure at one mapped "
         "images, commit on the emulation thread, and shutdown conserves "
         "every prepared reference");
    return 0;
}
