/* tex_pack.h — HD texture replacement / dumping (framework).
 *
 * A reusable psxrecomp feature: identify each texture the game uploads to VRAM
 * by content hash, so a high-resolution replacement image can be substituted at
 * draw time. The on-disk format is deliberately **identical to Beetle PSX HW's**
 * so existing packs authored for RetroArch are drop-in:
 *
 *     <disc dir>/<disc stem>-texture-replacements/<texhash>-<palhash>.png
 *     <disc dir>/<disc stem>-texture-dump/<texhash>-<palhash>.png
 *
 * where `texhash` is the CRC32 of the raw 16-bit VRAM words of the rectangle the
 * game uploaded via GP0(A0), and `palhash` is the CRC32 of the CLUT row the draw
 * samples it through. Both are lowercase `%x` (no zero padding). Compatibility
 * rests on three things matching Beetle exactly, all verified:
 *   - the polynomial (0xEDB88320, init/xorout 0xFFFFFFFF — crc32_compute);
 *   - the hashed byte range (the upload rect, row-major, X wrapped at 1024);
 *   - the rect a draw is matched against (the UV-limited sample rect, see
 *     tex_pack_on_textured_prim).
 *
 * Where this sits: uploads, fills, copies and textured draws are all observed
 * through the gr_* renderer facade (gpu_render.c), so tracking is
 * backend-agnostic — software, OpenGL and Vulkan feed the same tracker. This
 * mirrors how the [[vram_patch]] layer in text_xlate.cpp rides the GP0(A0)
 * completion hook.
 *
 * Lifetime model (v1): a tracked upload dies wholesale as soon as anything
 * overwrites any part of its rect. Beetle instead splits a partially-overwritten
 * rect into surviving subregions; that is deferred, and the dump-diff against a
 * Beetle dump of the same play session is what measures the cost.
 */
#ifndef PSXRECOMP_TEX_PACK_H
#define PSXRECOMP_TEX_PACK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-time init, called once the disc path is resolved. `disc_path` is the
 * loaded .cue/.bin/.chd — the pack and dump folder names derive from its stem,
 * matching Beetle's `retro_cd_base_name`. `dir_override`, when non-empty,
 * replaces the disc's directory as the parent of both folders. Cheap no-op (and
 * tex_pack_active() stays 0) when both flags are 0. */
void tex_pack_init(const char *disc_path, int enable_replace, int enable_dump,
                   const char *dir_override);
void tex_pack_shutdown(void);

/* 1 when either replacement or dumping is on. The gr_* hooks test this first so
 * a disabled build costs one predictable branch per primitive. */
int tex_pack_active(void);

/* ---- VRAM tracking (driven from the gr_* facade) ---- */

/* A completed CPU→VRAM image transfer (GP0 0xA0). `pixels` is the staged
 * w*h halfword rectangle — exactly the buffer whose CRC32 is the texture hash. */
void tex_pack_on_upload(int x, int y, int w, int h, const uint16_t *pixels);

/* Any other write to VRAM (fill, VRAM→VRAM copy, rendered-to region). Kills
 * every tracked upload the rect touches and drops stale CLUT hashes. */
void tex_pack_invalidate(int x, int y, int w, int h);

/* GP0(E2h) texture window, raw 20-bit parameter. */
void tex_pack_set_texture_window(uint32_t raw);

/* ---- per-primitive texture identity ---- */

/* Called for every textured primitive with its inclusive, post-wrap UV bounds
 * (lim = {lo_u, lo_v, hi_u, hi_v}, as produced by psx_uv_tri_limits /
 * psx_uv_rect_limits in gpu_uv.h) and the primitive's CLUT and texpage. */
void tex_pack_on_textured_prim(const int lim[4], uint16_t clut_x, uint16_t clut_y,
                               uint16_t texpage);

/* Debug-server surface. subcmd:
 *   "stats"    -> counters + folder paths + pack/dump coverage
 *   "uploads"  -> JSON array of the live tracked uploads {x,y,w,h,hash}
 *   "dumped"   -> JSON array of the <hash>-<pal> keys written this session
 *   "missing"  -> pack entries never matched by a draw yet (authoring aid)
 * Returns bytes written into out (<= cap). */
int tex_pack_debug_json(const char *subcmd, char *out, int cap);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_TEX_PACK_H */
