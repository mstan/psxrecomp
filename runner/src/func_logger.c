/*
 * func_logger.c — Runtime function discovery logger
 *
 * Maintains a sorted set of unique function addresses discovered during
 * interpreter execution. Writes to discovered_functions.log on demand.
 */
#include "func_logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FUNCS 8192

static uint32_t s_addrs[MAX_FUNCS];
static int      s_count = 0;
static int      s_sorted = 1;

void func_logger_init(void)
{
    s_count = 0;
    s_sorted = 1;
}

void func_logger_log(uint32_t addr, uint32_t caller_pc)
{
    (void)caller_pc;

    /* Quick dedup check against recent entries (hot path) */
    for (int i = s_count - 1; i >= 0 && i >= s_count - 16; i--) {
        if (s_addrs[i] == addr) return;
    }

    /* Full dedup */
    for (int i = 0; i < s_count; i++) {
        if (s_addrs[i] == addr) return;
    }

    if (s_count >= MAX_FUNCS) {
        static int s_warned = 0;
        if (!s_warned) {
            fprintf(stderr, "[func_logger] capacity reached (%d), new entries dropped\n", MAX_FUNCS);
            s_warned = 1;
        }
        return;
    }

    s_addrs[s_count++] = addr;
    s_sorted = 0;

    /* Periodic progress */
    if (s_count <= 20 || s_count % 100 == 0) {
        fprintf(stderr, "[func_logger] discovered %d unique functions (latest: 0x%08X)\n",
                s_count, addr);
    }
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t va = *(const uint32_t *)a;
    uint32_t vb = *(const uint32_t *)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

static void ensure_sorted(void)
{
    if (!s_sorted && s_count > 1) {
        qsort(s_addrs, s_count, sizeof(uint32_t), cmp_u32);
        s_sorted = 1;
    }
}

void func_logger_dump(const char *path)
{
    ensure_sorted();

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[func_logger] cannot write %s\n", path);
        return;
    }

    fprintf(f, "# Discovered function entry points (%d unique)\n", s_count);
    fprintf(f, "# Feed to recompiler: PSXRecomp.exe <exe> --extra-funcs %s\n", path);
    for (int i = 0; i < s_count; i++) {
        fprintf(f, "0x%08X\n", s_addrs[i]);
    }

    fclose(f);
    fprintf(stderr, "[func_logger] wrote %d addresses to %s\n", s_count, path);
}

void func_logger_shutdown(void)
{
    s_count = 0;
}

int func_logger_count(void)
{
    return s_count;
}

int func_logger_json(char *buf, int buf_size)
{
    ensure_sorted();

    int pos = 0;
    pos += snprintf(buf + pos, buf_size - pos, "[");
    for (int i = 0; i < s_count && pos < buf_size - 20; i++) {
        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, buf_size - pos, "\"0x%08X\"", s_addrs[i]);
    }
    pos += snprintf(buf + pos, buf_size - pos, "]");
    return pos;
}
