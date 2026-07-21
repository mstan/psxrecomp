#ifndef OVERLAY_CAPTURE_H
#define OVERLAY_CAPTURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* overlay_capture — B-1 implementation of the overlay capture set.
 *
 * On every CD DMA completion into game-code RAM (dest < 0x1C0000), the runtime
 * calls overlay_capture_on_dma(). The in-memory DMA set tracks the current
 * image; executed outgoing variants are preserved as coherent immutable
 * snapshots before replacement. At clean process exit,
 * overlay_capture_write_json() writes the latest coherent manifest too.
 */

/* Call once at startup (before the run loop) with the exe directory.
 * Sets the output path for overlay_captures.json.  Capture is not yet
 * active — it activates automatically on the first post-game DMA transfer. */
void overlay_capture_set_out_dir(const char *out_dir);

/* Set the canonical capture manifest path directly. The runtime also keeps an
 * immutable additive history in <path>.d/ for future shard regeneration. */
int overlay_capture_set_path(const char *path);

/* Legacy init alias kept for compatibility; same as set_out_dir. */
void overlay_capture_init(const char *out_dir);

/* Enable/disable overlay capture. Off by default; main() enables it only when
 * the overlay cache is turned on in [runtime] config. When disabled,
 * overlay_capture_on_dma() and overlay_capture_write_json() are no-ops. */
void overlay_capture_set_enabled(int enabled);

/* Opt-in durable history. When enabled, every changed coherent latest snapshot
 * is appended to overlay_captures.addendum.jsonl beside the executable. If
 * persist_dir is non-empty, the same snapshot is also copied atomically to one
 * new immutable JSON file there. game_id is used only in safe filenames. */
void overlay_capture_configure_history(int enabled, const char *persist_dir,
                                       const char *game_id);

/* Call from dma.c execute_ch3_cdrom() after every forward CH3 transfer with
 * load_start < 0x1C0000 and fntrace_is_game_started().
 * load_addr is the physical RAM destination, size is byte count,
 * bytes points to the RAM buffer at that address (already written by DMA). */
void overlay_capture_on_dma(uint32_t load_addr, uint32_t size,
                             const uint8_t *bytes);

/* Called before a to-RAM CD DMA overwrites its destination. If that span held
 * executed code, journal the coherent outgoing bytes/evidence first. */
void overlay_capture_before_dma(uint32_t load_addr, uint32_t size);

/* Write overlay_captures.json to the directory supplied at init time.
 * Safe to call even if no overlays were captured (writes nothing).
 * Called from shutdown_runtime() in main.cpp. */
void overlay_capture_write_json(void);
/* Join any background snapshot writer before process shutdown. */
void overlay_capture_wait_pending(void);

/* Returns number of unique overlays captured so far. */
int overlay_capture_count(void);

/* Variant-capture automation (step 2.8): per-vblank tick that fires an
 * automatic capture + background compile when the dirty-RAM interpreter
 * shows sustained pressure inside a capture window (an uncovered variant
 * being executed) at a coherent (not-loading) moment. Enabled by main()
 * when [runtime] overlay_autocompile_cmd is configured. */
void overlay_autocapture_set_enabled(int on);
void overlay_autocapture_tick(void);
/* Join any in-flight atomic writer before the final shutdown snapshot. */
void overlay_autocapture_shutdown(void);
void overlay_autocapture_get_status(int *enabled, uint32_t *triggers,
                                    uint64_t *last_delta);

/* Compute CRC32 of the DMA-time bytes for the region [region_start,
 * region_start+region_size).  Zero-filled for gaps between DMA blocks,
 * exactly as overlay_captures.json bytes_b64 is assembled.  Use this
 * in overlay_loader to build a consistent DLL filename without reading
 * live RAM (which has scatter-load gap contamination). */
uint32_t overlay_capture_get_region_crc(uint32_t region_start,
                                         uint32_t region_size);

#ifdef __cplusplus
}
#endif

#endif /* OVERLAY_CAPTURE_H */
