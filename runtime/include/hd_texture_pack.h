#ifndef PSXRECOMP_HD_TEXTURE_PACK_H
#define PSXRECOMP_HD_TEXTURE_PACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* External Beetle-style texture-pack loader and VRAM upload tracker.
 * The pack payload is never copied into or owned by the executable. */
typedef struct HdTexturePack HdTexturePack;

enum HdTextureDepth {
    HD_TEXTURE_DEPTH_4BPP = 0,
    HD_TEXTURE_DEPTH_8BPP = 1,
    HD_TEXTURE_DEPTH_16BPP = 2
};

enum HdTextureLookupStatus {
    HD_TEXTURE_LOOKUP_ERROR = -1,
    HD_TEXTURE_LOOKUP_NONE = 0,
    HD_TEXTURE_LOOKUP_FOUND = 1,
    HD_TEXTURE_LOOKUP_AMBIGUOUS = 2
};

typedef struct HdTexturePackInfo {
    const char* asset_root;       /* valid until hd_texture_pack_destroy */
    const char* replacement_root; /* valid until hd_texture_pack_destroy */
    size_t replacement_file_count;
    size_t unique_key_count;
    size_t ambiguous_key_count;
    size_t logical_mapping_count;
} HdTexturePackInfo;

typedef struct HdTexturePackEntry {
    uint32_t texture_hash;
    uint32_t palette_hash;
    const char* replacement_path; /* valid until hd_texture_pack_destroy */
    const char* logical_path;     /* empty when Hashes.ini has no mapping */
} HdTexturePackEntry;

/* A UV interval is inclusive. first > last means that the 8-bit texture
 * coordinate wraps through 255 -> 0. page_x/page_y are VRAM word coordinates.
 * vram must contain the complete 1024x512 native PSX VRAM image. */
typedef struct HdTextureDrawQuery {
    uint16_t page_x;
    uint16_t page_y;
    uint8_t depth; /* enum HdTextureDepth */
    uint8_t u_first;
    uint8_t u_last;
    uint8_t v_first;
    uint8_t v_last;
    uint16_t clut_x;
    uint16_t clut_y;
    const uint16_t* vram;
    size_t vram_word_count;
} HdTextureDrawQuery;

typedef struct HdTextureMatch {
    HdTexturePackEntry entry;
    uint64_t upload_serial;
    uint16_t upload_width_words;
    uint16_t upload_height;
    uint16_t source_word_x; /* first queried word within the original upload */
    uint16_t source_y;
} HdTextureMatch;

/* explicit_root wins. When it is NULL/empty, PSXRECOMP_HD_TEXTURE_ROOT is
 * consulted. Accepts either:
 *   root/Hashes.ini + exactly one <name>-texture-replacements directory, or
 *   the replacement directory directly (Hashes.ini is read from it or parent).
 * Returns 1 on success and leaves *out_pack NULL on failure. */
int hd_texture_pack_create(const char* explicit_root,
                           HdTexturePack** out_pack,
                           char* error,
                           size_t error_capacity);
void hd_texture_pack_destroy(HdTexturePack* pack);
void hd_texture_pack_get_info(const HdTexturePack* pack,
                              HdTexturePackInfo* out_info);

/* Key-only lookup. Ambiguous numeric aliases (for example 1-2.png and
 * 00000001-2.png) deliberately return AMBIGUOUS instead of selecting by
 * directory enumeration order. */
int hd_texture_pack_lookup(const HdTexturePack* pack,
                           uint32_t texture_hash,
                           uint32_t palette_hash,
                           HdTexturePackEntry* out_entry);

/* Standard reflected IEEE CRC-32 (polynomial 0xEDB88320, initial/final XOR
 * 0xFFFFFFFF), feeding each uint16_t explicitly low byte then high byte. */
uint32_t hd_texture_crc32_words_le(const uint16_t* words, size_t word_count);

/* Hashes the complete live CLUT: 16 entries for 4bpp or 256 for 8bpp. CLUT X
 * wraps at 1024 and Y wraps at 512. 16bpp has no palette and returns zero. */
uint32_t hd_texture_hash_clut(const uint16_t* vram,
                              size_t vram_word_count,
                              uint16_t clut_x,
                              uint16_t clut_y,
                              uint8_t depth);

/* Upload bytes are row-major logical upload words, independent of destination
 * wrapping. Tracking invalidates/splits every intersected older residency.
 * Width <= 1024 and height <= 512; X/Y may wrap. */
int hd_texture_pack_track_upload(HdTexturePack* pack,
                                 uint16_t x,
                                 uint16_t y,
                                 uint16_t width_words,
                                 uint16_t height,
                                 const uint16_t* words,
                                 size_t word_count,
                                 uint32_t* out_texture_hash);
void hd_texture_pack_invalidate(HdTexturePack* pack,
                                uint16_t x,
                                uint16_t y,
                                uint16_t width_words,
                                uint16_t height);
void hd_texture_pack_reset_tracking(HdTexturePack* pack);

/* Beetle-style upload residency is host metadata, but it is required for an
 * HD replacement to keep matching after a savestate restores the same VRAM.
 * The portable little-endian state below records only upload identity and the
 * surviving residency fragments; decoded images and GL objects remain caches.
 * save allocates with malloc (caller frees). A NULL pack serializes a valid
 * empty tracker so states remain renderer-independent. check is side-effect
 * free; load has strong replacement semantics and also accepts a NULL pack
 * after validating the wire. */
int hd_texture_pack_tracking_state_save(const HdTexturePack* pack,
                                        uint8_t** out_data,
                                        size_t* out_size);
int hd_texture_pack_tracking_state_check(const uint8_t* data,
                                         size_t size);
int hd_texture_pack_tracking_state_load(HdTexturePack* pack,
                                        const uint8_t* data,
                                        size_t size);
size_t hd_texture_pack_tracking_upload_count(const HdTexturePack* pack);

/* Matches only when one uniquely-keyed tracked upload still contains every
 * word touched by the texture page/depth/UV query. Any pack-key or residency
 * ambiguity falls back explicitly via HD_TEXTURE_LOOKUP_AMBIGUOUS. */
int hd_texture_pack_match(HdTexturePack* pack,
                          const HdTextureDrawQuery* query,
                          HdTextureMatch* out_match);

/* Live GP0 draw adapter: derives page_x/page_y/depth from the PS1 texpage word
 * and applies the same deterministic residency/CLUT/UV match above. This keeps
 * the renderer's texpage interpretation in the focused unit-test surface. */
int hd_texture_pack_match_draw(HdTexturePack* pack,
                               uint16_t texpage,
                               uint16_t clut_x,
                               uint16_t clut_y,
                               uint8_t u_first,
                               uint8_t u_last,
                               uint8_t v_first,
                               uint8_t v_last,
                               const uint16_t* vram,
                               size_t vram_word_count,
                               HdTextureMatch* out_match);

/* Optional, bounded, single-worker on-demand PNG decode cache. The request is
 * asynchronous and low-priority on Windows. request_decode returns FOUND when
 * already ready, NONE when queued/in flight, and ERROR for an absent,
 * ambiguous, failed, or over-budget image. acquire_decoded returns FOUND only
 * when ready. A lease keeps pixels alive across cache eviction and pack
 * destruction; always release it with hd_texture_pixels_release. */
typedef struct HdTexturePixels {
    const uint8_t* rgba;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    void* lease;
} HdTexturePixels;

void hd_texture_pack_set_decode_budget(HdTexturePack* pack,
                                       size_t budget_bytes);
int hd_texture_pack_request_decode(HdTexturePack* pack,
                                   uint32_t texture_hash,
                                   uint32_t palette_hash);
int hd_texture_pack_acquire_decoded(HdTexturePack* pack,
                                    uint32_t texture_hash,
                                    uint32_t palette_hash,
                                    HdTexturePixels* out_pixels);
void hd_texture_pixels_release(HdTexturePixels* pixels);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_HD_TEXTURE_PACK_H */
