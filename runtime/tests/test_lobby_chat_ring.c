/*
 * test_lobby_chat_ring.c — the lobby chat ring, without a lobby.
 *
 * The server echoes every line to everyone in the room INCLUDING the sender,
 * and that echo is the copy the ring keeps: the client never appends its own
 * send. So the order on screen is the room's order, "mine" is a question
 * about player ids rather than about who called send, and the ring's only
 * jobs are to keep the newest lines and to number them monotonically.
 *
 * This includes psx_lobby_client.c directly so it exercises the real
 * chat_push / chat_get rather than a copy of them.
 *
 * Build/run: ctest -R lobby_chat_ring_test
 * (registered in runtime/CMakeLists.txt, which owns the source list.)
 */
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <string.h>

#include "../src/psx_lobby_client.c"

/* The real recomp_net and the real WS helpers are linked in (see
 * runtime/CMakeLists.txt) rather than stubbed: nothing here reaches a socket,
 * so a wall of fakes would buy nothing and could drift from what the runtime
 * actually links. The one exception is the host clock, which lives in the
 * runtime's own main TU. */
uint64_t psx_host_mono_ms(void);
uint64_t psx_host_mono_ms(void)
{
    /* Monotonic and cheap. The ring does not read the clock; this only has to
     * exist and never go backwards. */
    static uint64_t t;
    return ++t;
}

static int g_failures;

static void ck(int cond, const char *what)
{
    if (!cond) { printf("FAIL: %s\n", what); g_failures++; }
}

static void case_room_order(void)
{
    PsxLobbyChatMsg got;
    printf("  room order\n");
    memset(&g_lc, 0, sizeof(g_lc));
    snprintf(g_lc.player_id, sizeof(g_lc.player_id), "%s", "me");

    chat_push("them", "Them", "first", 0);
    chat_push("me",   "Me",   "second", 0);
    chat_push("",     "",     "third", 1);

    ck(psx_lobby_chat_count() == 3, "three lines land");
    ck(psx_lobby_chat_get(0, &got) && strcmp(got.text, "first") == 0,
       "index 0 is the oldest");
    ck(psx_lobby_chat_get(2, &got) && strcmp(got.text, "third") == 0,
       "index 2 is the newest");

    ck(psx_lobby_chat_get(0, &got) && got.is_local == 0,
       "a peer's line is not local");
    ck(psx_lobby_chat_get(1, &got) && got.is_local == 1,
       "our own echoed line is local");
    ck(psx_lobby_chat_get(2, &got) && got.is_system == 1 && got.is_local == 0,
       "a system line is neither ours nor a player's");

    {
        uint32_t a, b;
        (void)psx_lobby_chat_get(0, &got); a = got.seq;
        (void)psx_lobby_chat_get(2, &got); b = got.seq;
        ck(b > a, "seq increases with arrival order");
    }
    ck(!psx_lobby_chat_get(3, &got), "reading past the end is refused");
    ck(!psx_lobby_chat_get(-1, &got), "so is a negative index");
}

static void case_wrap_drops_oldest(void)
{
    PsxLobbyChatMsg got;
    char buf[32];
    int i;
    printf("  wrap\n");
    memset(&g_lc, 0, sizeof(g_lc));
    snprintf(g_lc.player_id, sizeof(g_lc.player_id), "%s", "me");

    /* Overfill by ten. The ring must drop the OLDEST: a chat that discards
     * what was just said is worse than no chat at all. */
    for (i = 0; i < PSX_LOBBY_CHAT_RING + 10; ++i) {
        snprintf(buf, sizeof(buf), "line%d", i);
        chat_push("them", "Them", buf, 0);
    }
    ck(psx_lobby_chat_count() == PSX_LOBBY_CHAT_RING,
       "the ring stops at its capacity");
    ck(psx_lobby_chat_get(0, &got) && strcmp(got.text, "line10") == 0,
       "the oldest surviving line is the 11th sent");
    snprintf(buf, sizeof(buf), "line%d", PSX_LOBBY_CHAT_RING + 9);
    ck(psx_lobby_chat_get(PSX_LOBBY_CHAT_RING - 1, &got) &&
       strcmp(got.text, buf) == 0,
       "the newest line is the last one sent");
}

static void case_empty_and_clear(void)
{
    PsxLobbyChatMsg m;
    uint32_t before;
    printf("  empty/clear\n");
    memset(&g_lc, 0, sizeof(g_lc));

    chat_push("them", "Them", "", 0);
    chat_push("them", "Them", NULL, 0);
    ck(psx_lobby_chat_count() == 0, "an empty line is not a line");

    chat_push("them", "Them", "hello", 0);
    ck(psx_lobby_chat_count() == 1, "a real line is");
    (void)psx_lobby_chat_get(0, &m);
    before = m.seq;

    psx_lobby_chat_clear();
    ck(psx_lobby_chat_count() == 0, "clear empties the room log");

    chat_push("them", "Them", "new room", 0);
    (void)psx_lobby_chat_get(0, &m);
    /* seq must NOT restart. A UI tracking "newest seen" would otherwise
     * mistake the first line of a new room for one it had already scrolled
     * past, and never scroll to it. */
    ck(m.seq > before, "seq keeps counting across a clear");
}

static void case_long_line_is_truncated_not_dropped(void)
{
    PsxLobbyChatMsg got;
    char big[PSX_LOBBY_CHAT_TEXT_LEN * 2];
    printf("  long line\n");
    memset(&g_lc, 0, sizeof(g_lc));
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    chat_push("them", "Them", big, 0);
    ck(psx_lobby_chat_count() == 1, "an over-long line still arrives");
    ck(psx_lobby_chat_get(0, &got), "and reads back");
    ck(strlen(got.text) == PSX_LOBBY_CHAT_TEXT_LEN - 1,
       "clipped to the field, not past it");
    ck(got.text[PSX_LOBBY_CHAT_TEXT_LEN - 1] == '\0',
       "and still NUL-terminated");
}

int main(void)
{
    case_room_order();
    case_wrap_drops_oldest();
    case_empty_and_clear();
    case_long_line_is_truncated_not_dropped();
    if (g_failures == 0) {
        printf("lobby_chat_ring_test: ok\n");
        return 0;
    }
    fprintf(stderr, "lobby_chat_ring_test: %d failure(s)\n", g_failures);
    return 1;
}
