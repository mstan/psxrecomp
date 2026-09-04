#ifndef PSX_GPU_VRAM_DIRTY_H
#define PSX_GPU_VRAM_DIRTY_H

/*
 * Per-scanline dirty tracking for the 1024×512 CPU VRAM mirror.
 *
 * Enabled only while rollback netplay is live (psx_netplay_rb_start →
 * shutdown). Offline / delay-sync: g_psx_vram_dirty_tracking stays 0 so
 * mark_* is an inlined no-op (expect-not-taken). Used by raw ring snaps
 * (§96) and as the foundation for Tier-2 dirty-region digests.
 *
 * A missed mark (while tracking) is silent corruption — bring-up can enable
 * shadow-verify via PSX_NET_VRAM_DIRTY_VERIFY=1.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GPU_VRAM_DIRTY_H 512u
#define GPU_VRAM_DIRTY_W 1024u

/* 0 offline / delay-sync; 1 after psx_netplay_rb_start. */
extern int g_psx_vram_dirty_tracking;

void gpu_vram_dirty_set_tracking(int on);
int  gpu_vram_dirty_tracking(void);

void gpu_vram_dirty_mark_row_impl(uint32_t y);
void gpu_vram_dirty_mark_rows(uint32_t y0, uint32_t h);
void gpu_vram_dirty_mark_rect(int x, int y, int w, int h);
void gpu_vram_dirty_mark_all(void);
void gpu_vram_dirty_clear(void);

int      gpu_vram_dirty_any(void);
uint32_t gpu_vram_dirty_row_count(void);
/* 512-bit mask: bit y set ⇒ scanline y dirty. Non-owning. */
const uint64_t *gpu_vram_dirty_mask(void);

/* 1 if PSX_NET_VRAM_DIRTY_VERIFY is set (non-zero). */
int gpu_vram_dirty_verify_enabled(void);

#if defined(__GNUC__) || defined(__clang__)
#  define GPU_VRAM_DIRTY_TRACKING_ON() \
      (__builtin_expect(g_psx_vram_dirty_tracking, 0))
#else
#  define GPU_VRAM_DIRTY_TRACKING_ON() (g_psx_vram_dirty_tracking)
#endif

/* Hot path: offline compiles to load + not-taken branch, no call. */
static inline void gpu_vram_dirty_mark_row(uint32_t y)
{
    if (!GPU_VRAM_DIRTY_TRACKING_ON())
        return;
    gpu_vram_dirty_mark_row_impl(y);
}

#ifdef __cplusplus
}
#endif

#endif /* PSX_GPU_VRAM_DIRTY_H */
