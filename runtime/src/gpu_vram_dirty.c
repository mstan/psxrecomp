#include "gpu_vram_dirty.h"

#include <stdlib.h>
#include <string.h>

int g_psx_vram_dirty_tracking = 0;

/* All-dirty until the first successful incremental checkpoint (while on). */
static uint64_t s_mask[GPU_VRAM_DIRTY_H / 64u];
static int      s_all = 1;
static int      s_verify = -1; /* -1 = unread env */

static void ensure_verify_env(void)
{
    const char *e;
    if (s_verify >= 0)
        return;
    e = getenv("PSX_NET_VRAM_DIRTY_VERIFY");
    s_verify = (e && e[0] && e[0] != '0') ? 1 : 0;
}

void gpu_vram_dirty_set_tracking(int on)
{
    if (on) {
        g_psx_vram_dirty_tracking = 1;
        /* First snap after enable must full-refresh the mirror. */
        memset(s_mask, 0xff, sizeof s_mask);
        s_all = 1;
    } else {
        g_psx_vram_dirty_tracking = 0;
        memset(s_mask, 0, sizeof s_mask);
        s_all = 0;
    }
}

int gpu_vram_dirty_tracking(void)
{
    return g_psx_vram_dirty_tracking ? 1 : 0;
}

void gpu_vram_dirty_mark_row_impl(uint32_t y)
{
    y &= (GPU_VRAM_DIRTY_H - 1u);
    s_mask[y >> 6] |= (uint64_t)1u << (y & 63u);
}

void gpu_vram_dirty_mark_rows(uint32_t y0, uint32_t h)
{
    uint32_t i;
    if (!g_psx_vram_dirty_tracking || h == 0u)
        return;
    if (h >= GPU_VRAM_DIRTY_H) {
        gpu_vram_dirty_mark_all();
        return;
    }
    for (i = 0; i < h; i++)
        gpu_vram_dirty_mark_row_impl(y0 + i);
}

void gpu_vram_dirty_mark_rect(int x, int y, int w, int h)
{
    (void)x;
    if (!g_psx_vram_dirty_tracking || w <= 0 || h <= 0)
        return;
    if (h >= (int)GPU_VRAM_DIRTY_H) {
        gpu_vram_dirty_mark_all();
        return;
    }
    gpu_vram_dirty_mark_rows((uint32_t)y, (uint32_t)h);
}

void gpu_vram_dirty_mark_all(void)
{
    if (!g_psx_vram_dirty_tracking)
        return;
    memset(s_mask, 0xff, sizeof s_mask);
    s_all = 1;
}

void gpu_vram_dirty_clear(void)
{
    if (!g_psx_vram_dirty_tracking)
        return;
    memset(s_mask, 0, sizeof s_mask);
    s_all = 0;
}

int gpu_vram_dirty_any(void)
{
    uint32_t i;
    if (!g_psx_vram_dirty_tracking)
        return 1; /* treat as fully dirty if someone asks while off */
    if (s_all)
        return 1;
    for (i = 0; i < (uint32_t)(sizeof s_mask / sizeof s_mask[0]); i++) {
        if (s_mask[i])
            return 1;
    }
    return 0;
}

uint32_t gpu_vram_dirty_row_count(void)
{
    uint32_t i, n = 0;
    if (!g_psx_vram_dirty_tracking || s_all)
        return GPU_VRAM_DIRTY_H;
    for (i = 0; i < GPU_VRAM_DIRTY_H; i++) {
        if (s_mask[i >> 6] & ((uint64_t)1u << (i & 63u)))
            n++;
    }
    return n;
}

const uint64_t *gpu_vram_dirty_mask(void)
{
    return s_mask;
}

int gpu_vram_dirty_verify_enabled(void)
{
    ensure_verify_env();
    return s_verify;
}
