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

/* One-time init, called once the disc path is resolved.
 *
 * `disc_path` is the loaded .cue/.bin/.chd; the pack and dump folder names
 * derive from its stem, matching Beetle's `retro_cd_base_name`. `dir_override`,
 * when non-empty, replaces the disc's directory as the parent of both folders.
 *
 * `pack_dir`, when non-empty, is used DIRECTLY as the replacement folder and
 * overrides that derivation entirely. This is the managed-pack path: a launcher
 * keeps packs in its own store under an arbitrary id, and there is no reason to
 * force such a folder to be named after the disc. When it is empty the Beetle
 * drop-in convention applies unchanged, so a pack dropped next to the disc
 * still just works. The dump folder always follows the disc-stem convention.
 *
 * Cheap no-op (and tex_pack_active() stays 0) when both flags are 0. */
/* Add a config-owned exclusion (call after tex_pack_init). */
void tex_pack_add_excluded(uint32_t hash);
void tex_pack_init(const char *disc_path, int enable_replace, int enable_dump,
                   const char *dir_override, const char *pack_dir);
void tex_pack_shutdown(void);

/* Savestate section: rects+hashes of tracked uploads (pixels rebuilt from the
 * restored VRAM and hash-verified at apply). Apply AFTER VRAM is restored. */
uint32_t tex_pack_state_bytes(void);
void     tex_pack_state_write(uint8_t *p);
void     tex_pack_state_apply(const uint8_t *p, uint64_t len,
                              const uint16_t *vram /* NULL = CPU mirror */);

/* Write <pack dir>/coverage.json: how much of the active pack the session
 * actually drew, plus the entries it never asked for. Safe to call from
 * std::atexit and safe to call more than once; a no-op when no pack is active.
 * The launcher reads this to show per-pack coverage without running the game. */
void tex_pack_write_coverage(void);

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

/* ---- replacement lookup (the substitution path) ---- */

/* A decoded replacement plus everything needed to address it from a primitive's
 * texture-page UVs:
 *
 *     s = (u - origin_u) / src_w      t = (v - origin_v) / src_h
 *
 * i.e. the source texture occupies src_w x src_h TEXELS starting at
 * (origin_u, origin_v) within the texture page, and the replacement image
 * covers exactly that region at whatever resolution it happens to be. The
 * caller never needs to know the upscale factor.
 *
 * `pixels` is RGBA8, width*height*4, owned by tex_pack and stable for the
 * process lifetime. `gl_handle` points at a slot the backend may use to cache
 * an uploaded texture name so it uploads each image once; tex_pack only ever
 * reads it as an opaque value. `id` identifies the entry for that caching. */
typedef struct {
    const unsigned char *pixels;
    int width, height;       /* replacement image, pixels */
    int src_w, src_h;        /* source texture, texels    */
    int origin_u, origin_v;  /* source origin in the page, texels */
    unsigned long long id;
    unsigned *gl_handle;
    /* 1 = this image was matched on TEXTURE hash alone because the pack ships
     * no file for the draw's CLUT. It is a single-colour mask, so the shape is
     * right but the colour is whatever palette variant happened to be on disk;
     * the renderer must take the colour from the live CLUT instead. */
    int recolour;
    /* Valid only when recolour is set. ink_index is the palette entry this
     * image inks, used for HD ink that overhangs a hole texel; ref_max is the
     * image's own full-ink max channel in 0..1, which the shader divides by to
     * recover the antialiasing ramp. */
    int   ink_index;
    float ref_max;
} TexPackRepl;

/* 1 when this primitive has a replacement (decoding it on first use), 0
 * otherwise. Cheap no-op unless replacement is enabled and the pack is
 * non-empty. */
int tex_pack_lookup_replacement(const int lim[4], uint16_t clut_x, uint16_t clut_y,
                                uint16_t texpage, TexPackRepl *out);

/* Live enable/disable for substitution, independent of the pack being loaded.
 * Exists so an A/B costs a command instead of a rebuild — every wrong guess
 * about why replacement broke the HUD cost a full build-and-relaunch cycle,
 * and half of those could have been settled in ten seconds. -1 = query. */
int tex_pack_replace_enabled(int set);

/* Census of primitives that were actually ARMED with a replacement, keyed by
 * texture and carrying the primitive's SCREEN bounds.
 *
 * The counters say how many draws were replaced but not WHICH, and that is the
 * open question: the HUD's own font atlases are never matched, yet the HUD
 * disappears whenever substitution is on. If a HUD-region primitive shows up
 * here, it is being drawn through some other texture's replacement image. */
void tex_pack_note_armed(unsigned long long id, int x0, int y0, int x1, int y1);

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
