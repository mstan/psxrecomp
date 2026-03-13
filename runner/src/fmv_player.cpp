/* fmv_player.cpp — PS1 STR v2 FMV decoder
 *
 * MDEC decode ported directly from FFmpeg libavcodec/mdec.c
 * IDCT ported from FFmpeg libavcodec/simple_idct_template.c (BIT_DEPTH=8)
 * VLC tables verbatim from FFmpeg libavcodec/mpeg12data.c
 *
 * Sector demux (STR header parsing, XA routing) unchanged.
 */
#include "fmv_player.h"
#include "automation.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <windows.h>

/* --- external interface ---------------------------------------------------- */
extern "C" void fmv_vram_upload(int x, int y, int w, int h, const uint16_t* data);
extern "C" void psx_present_frame(void);
extern "C" void xa_audio_seek(uint32_t lba);
extern "C" void fmv_force_display_area(int x, int y, int w, int h);

/* --- sector constants ------------------------------------------------------ */
static constexpr int RAW_SECTOR   = 2352;
static constexpr int SUBHDR_OFF   = 16;
static constexpr int STR_HDR_OFF  = 24;
static constexpr int STR_HDR_LEN  = 32;
static constexpr int PAYLOAD_OFF  = STR_HDR_OFF + STR_HDR_LEN; /* = 56 */
static constexpr int PAYLOAD_SIZE = 0x7E0; /* = 2016, FFmpeg VIDEO_DATA_CHUNK_SIZE */
static constexpr int FRAME_HDR_BYTES = 8;  /* quant_scale + magic + qscale + version */

/* --- VLC tables verbatim from FFmpeg mpeg12data.c -------------------------- */
/* ff_mpeg1_vlc_table[113][2] = {code, len}.
 * Entries 0..110: AC coefficients. Entry 111: escape {0x01,6}. Entry 112: EOB {0x02,2}. */
static const uint16_t mpeg1_vlc_table[113][2] = {
 { 0x3, 2 }, { 0x4, 4 }, { 0x5, 5 }, { 0x6, 7 },
 { 0x26, 8 }, { 0x21, 8 }, { 0xa, 10 }, { 0x1d, 12 },
 { 0x18, 12 }, { 0x13, 12 }, { 0x10, 12 }, { 0x1a, 13 },
 { 0x19, 13 }, { 0x18, 13 }, { 0x17, 13 }, { 0x1f, 14 },
 { 0x1e, 14 }, { 0x1d, 14 }, { 0x1c, 14 }, { 0x1b, 14 },
 { 0x1a, 14 }, { 0x19, 14 }, { 0x18, 14 }, { 0x17, 14 },
 { 0x16, 14 }, { 0x15, 14 }, { 0x14, 14 }, { 0x13, 14 },
 { 0x12, 14 }, { 0x11, 14 }, { 0x10, 14 }, { 0x18, 15 },
 { 0x17, 15 }, { 0x16, 15 }, { 0x15, 15 }, { 0x14, 15 },
 { 0x13, 15 }, { 0x12, 15 }, { 0x11, 15 }, { 0x10, 15 },
 { 0x3, 3 }, { 0x6, 6 }, { 0x25, 8 }, { 0xc, 10 },
 { 0x1b, 12 }, { 0x16, 13 }, { 0x15, 13 }, { 0x1f, 15 },
 { 0x1e, 15 }, { 0x1d, 15 }, { 0x1c, 15 }, { 0x1b, 15 },
 { 0x1a, 15 }, { 0x19, 15 }, { 0x13, 16 }, { 0x12, 16 },
 { 0x11, 16 }, { 0x10, 16 }, { 0x5, 4 }, { 0x4, 7 },
 { 0xb, 10 }, { 0x14, 12 }, { 0x14, 13 }, { 0x7, 5 },
 { 0x24, 8 }, { 0x1c, 12 }, { 0x13, 13 }, { 0x6, 5 },
 { 0xf, 10 }, { 0x12, 12 }, { 0x7, 6 }, { 0x9, 10 },
 { 0x12, 13 }, { 0x5, 6 }, { 0x1e, 12 }, { 0x14, 16 },
 { 0x4, 6 }, { 0x15, 12 }, { 0x7, 7 }, { 0x11, 12 },
 { 0x5, 7 }, { 0x11, 13 }, { 0x27, 8 }, { 0x10, 13 },
 { 0x23, 8 }, { 0x1a, 16 }, { 0x22, 8 }, { 0x19, 16 },
 { 0x20, 8 }, { 0x18, 16 }, { 0xe, 10 }, { 0x17, 16 },
 { 0xd, 10 }, { 0x16, 16 }, { 0x8, 10 }, { 0x15, 16 },
 { 0x1f, 12 }, { 0x1a, 12 }, { 0x19, 12 }, { 0x17, 12 },
 { 0x16, 12 }, { 0x1f, 13 }, { 0x1e, 13 }, { 0x1d, 13 },
 { 0x1c, 13 }, { 0x1b, 13 }, { 0x1f, 16 }, { 0x1e, 16 },
 { 0x1d, 16 }, { 0x1c, 16 }, { 0x1b, 16 },
 { 0x1, 6 }, /* 111: escape */
 { 0x2, 2 }, /* 112: EOB   */
};

/* ff_mpeg12_level[111] — AC coefficient magnitudes (all positive) */
static const int8_t mpeg12_level[111] = {
  1,  2,  3,  4,  5,  6,  7,  8,
  9, 10, 11, 12, 13, 14, 15, 16,
 17, 18, 19, 20, 21, 22, 23, 24,
 25, 26, 27, 28, 29, 30, 31, 32,
 33, 34, 35, 36, 37, 38, 39, 40,
  1,  2,  3,  4,  5,  6,  7,  8,
  9, 10, 11, 12, 13, 14, 15, 16,
 17, 18,  1,  2,  3,  4,  5,  1,
  2,  3,  4,  1,  2,  3,  1,  2,
  3,  1,  2,  3,  1,  2,  1,  2,
  1,  2,  1,  2,  1,  2,  1,  2,
  1,  2,  1,  2,  1,  2,  1,  2,
  1,  1,  1,  1,  1,  1,  1,  1,
  1,  1,  1,  1,  1,  1,  1,
};

/* ff_mpeg12_run[111] — zero-run before each AC coefficient */
static const int8_t mpeg12_run[111] = {
  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,
  1,  1,  1,  1,  1,  1,  1,  1,
  1,  1,  1,  1,  1,  1,  1,  1,
  1,  1,  2,  2,  2,  2,  2,  3,
  3,  3,  3,  4,  4,  4,  5,  5,
  5,  6,  6,  6,  7,  7,  8,  8,
  9,  9, 10, 10, 11, 11, 12, 12,
 13, 13, 14, 14, 15, 15, 16, 16,
 17, 18, 19, 20, 21, 22, 23, 24,
 25, 26, 27, 28, 29, 30, 31,
};

/* ff_mpeg1_default_intra_matrix[:64] — quantization matrix */
static const uint16_t intra_matrix[64] = {
     8, 16, 19, 22, 26, 27, 29, 34,
    16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48,
    26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83,
};

/* ff_zigzag_direct — VLC scan position → 8×8 coefficient index */
static const uint8_t zigzag_direct[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};

/* YCbCr->RGB fixed-point coefficients ×2^16 */
static const int64_t CR_R =  91893LL;
static const int64_t CB_G = -22525LL;
static const int64_t CR_G = -46812LL;
static const int64_t CB_B = 116224LL;

static inline int clamp_u8(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/* ============================================================================
 * bswap16: byte-swap every 16-bit word so MSB is first in memory.
 * After this, reading the buffer byte-by-byte MSB-first gives the same bit
 * order as reading each LE 16-bit word with bits ordered from MSB to LSB.
 * This matches FFmpeg's bbdsp.bswap16_buf() call in mdec.c decode_frame().
 * ============================================================================ */
static void bswap16_buf(uint8_t* dst, const uint8_t* src, int nbytes) {
    int i = 0;
    for (; i + 1 < nbytes; i += 2) {
        dst[i]   = src[i + 1];
        dst[i + 1] = src[i];
    }
    if (i < nbytes) dst[i] = src[i];
}

/* ============================================================================
 * Bit reader: reads from a bswap16'd buffer, MSB-first.
 * Maintains a 32-bit cache filled from successive bytes.
 * ============================================================================ */
struct BitCtx {
    const uint8_t* buf;
    int  byte_pos;
    int  buf_size;      /* bytes */
    uint32_t cache;
    int  cache_bits;
};

static void bc_init(BitCtx* bc, const uint8_t* buf, int size) {
    bc->buf        = buf;
    bc->byte_pos   = 0;
    bc->buf_size   = size;
    bc->cache      = 0;
    bc->cache_bits = 0;
}

static void bc_refill(BitCtx* bc) {
    while (bc->cache_bits <= 24 && bc->byte_pos < bc->buf_size) {
        bc->cache |= (uint32_t)bc->buf[bc->byte_pos++] << (24 - bc->cache_bits);
        bc->cache_bits += 8;
    }
}

static inline uint32_t bc_show(BitCtx* bc, int n) {
    bc_refill(bc);
    return bc->cache >> (32 - n);
}

static inline void bc_skip(BitCtx* bc, int n) {
    bc->cache      <<= n;
    bc->cache_bits  -= n;
}

static inline uint32_t bc_get(BitCtx* bc, int n) {
    uint32_t v = bc_show(bc, n);
    bc_skip(bc, n);
    return v;
}

/* Signed n-bit read — sign-extends to int32 */
static inline int32_t bc_gets(BitCtx* bc, int n) {
    uint32_t v = bc_get(bc, n);
    /* left-shift to MSB, then arithmetic right-shift back = sign extension */
    return (int32_t)(v << (32 - n)) >> (32 - n);
}

/* ============================================================================
 * mdec_decode_block — port of mdec_decode_block_intra() from FFmpeg mdec.c
 *
 * version <= 2:  block[0] = 2*get_sbits(10) + 1024  (absolute DC)
 * AC VLC:        scan mpeg1_vlc_table, read 1 sign bit after VLC code
 *                dequantize: (|level| * qscale * intra_matrix[j]) >> 3
 *                apply sign after dequant (FFmpeg order)
 * Escape:        run = UBITS(6)+1, level = SBITS(10)
 *                dequant abs value, (val-1)|1, restore sign
 * ============================================================================ */
static void mdec_decode_block(BitCtx* bc, int16_t* block, int qscale) {
    /* DC (version <= 2 path from mdec.c line 72) */
    block[0] = (int16_t)(2 * bc_gets(bc, 10) + 1024);

    int i = 0;   /* scan position; 0 = DC, AC starts at 1 */

    for (;;) {
        /* Peek 17 bits for EOB/escape/VLC check */
        uint32_t peek = bc_show(bc, 17);

        /* EOB: code=0x02, len=2 → top 2 bits = 10 = 2 */
        if ((peek >> 15) == 2u) {
            bc_skip(bc, 2);
            break;
        }

        /* Escape: code=0x01, len=6 → top 6 bits = 000001 = 1 */
        if ((peek >> 11) == 1u) {
            bc_skip(bc, 6);
            int run   = (int)bc_get(bc, 6) + 1;   /* UBITS(6) + 1 */
            int level = (int)bc_gets(bc, 10);      /* SBITS(10)    */
            i += run;
            if (i > 63) break;
            int j = zigzag_direct[i];
            /* Dequantize by absolute value, apply (val-1)|1, restore sign */
            if (level < 0) {
                level = -level;
                level = (level * qscale * intra_matrix[j]) >> 3;
                level = (level - 1) | 1;
                level = -level;
            } else {
                level = (level * qscale * intra_matrix[j]) >> 3;
                level = (level - 1) | 1;
            }
            block[j] = (int16_t)level;
            continue;
        }

        /* VLC: linear scan of mpeg1_vlc_table[0..110] */
        bool found = false;
        for (int k = 0; k < 111; k++) {
            int len  = mpeg1_vlc_table[k][1];
            int code = mpeg1_vlc_table[k][0];
            /* Compare top 'len' bits of peek against the right-justified code */
            if ((peek >> (17 - len)) == (uint32_t)code) {
                bc_skip(bc, len);

                /* run = mpeg12_run[k] + 1 (FFmpeg ff_init_2d_vlc_rl adds +1) */
                i += mpeg12_run[k] + 1;
                if (i > 63) { found = true; break; }
                int j = zigzag_direct[i];

                /* Dequantize the magnitude */
                int abs_level = (int)mpeg12_level[k];
                abs_level = (abs_level * qscale * intra_matrix[j]) >> 3;

                /* Read 1 sign bit and apply: 0→positive, 1→negative
                 * FFmpeg: level = (level ^ SHOW_SBITS(1)) - SHOW_SBITS(1) */
                uint32_t s = bc_get(bc, 1);
                int32_t sv = -(int32_t)s;          /* 0 or 0xFFFFFFFF */
                int level_signed = (abs_level ^ sv) - sv;

                block[j] = (int16_t)level_signed;
                found = true;
                break;
            }
        }

        if (!found) {
            /* Unrecognized VLC — skip 1 bit to attempt resync */
            bc_skip(bc, 1);
        }
    }
}

/* ============================================================================
 * FFmpeg simple IDCT — ported from simple_idct_template.c, BIT_DEPTH=8
 *
 * Constants: W1=22725 … W7=4520  (cos(k*π/16)*√2*2^14 + 0.5)
 * W4 = 16383 ≈ cos(π/4)*√2*2^14
 * Row shift = 11, Column shift = 20, DC shift = 3
 *
 * Input:  int16_t block[64]   (row-major, dequantized DCT coefficients)
 * Output: uint8_t dest[8*linesize]  (clamped to [0,255])
 * ============================================================================ */
#define W1 22725u
#define W2 21407u
#define W3 19266u
#define W4 16383u
#define W5 12873u
#define W6  8867u
#define W7  4520u
#define ROW_SHIFT 11
#define COL_SHIFT 20
#define DC_SHIFT   3

static void idctRowCondDC(int16_t* row) {
    /* DC-only optimisation: if AC coefficients are all zero */
    if (!(row[1] | row[2] | row[3] | row[4] | row[5] | row[6] | row[7])) {
        int16_t dc = (int16_t)(row[0] << DC_SHIFT);
        row[0] = row[1] = row[2] = row[3] =
        row[4] = row[5] = row[6] = row[7] = dc;
        return;
    }

    uint32_t a0, a1, a2, a3, b0, b1, b2, b3;

    a0 = (uint32_t)(W4 * row[0]) + (1u << (ROW_SHIFT - 1));
    a1 = a0; a2 = a0; a3 = a0;

    a0 += (uint32_t)( W2 * row[2]);
    a1 += (uint32_t)( W6 * row[2]);
    a2 += (uint32_t)(-W6 * row[2]);
    a3 += (uint32_t)(-W2 * row[2]);

    b0 = (uint32_t)(W1 * row[1]);
    b1 = (uint32_t)(W3 * row[1]);
    b2 = (uint32_t)(W5 * row[1]);
    b3 = (uint32_t)(W7 * row[1]);

    b0 += (uint32_t)( W3 * row[3]);
    b1 += (uint32_t)(-W7 * row[3]);
    b2 += (uint32_t)(-W1 * row[3]);
    b3 += (uint32_t)(-W5 * row[3]);

    if (row[4] | row[5] | row[6] | row[7]) {
        a0 += (uint32_t)( W4 * row[4]) + (uint32_t)( W6 * row[6]);
        a1 += (uint32_t)(-W4 * row[4]) + (uint32_t)(-W2 * row[6]);
        a2 += (uint32_t)(-W4 * row[4]) + (uint32_t)( W2 * row[6]);
        a3 += (uint32_t)( W4 * row[4]) + (uint32_t)(-W6 * row[6]);

        b0 += (uint32_t)( W5 * row[5]) + (uint32_t)( W7 * row[7]);
        b1 += (uint32_t)(-W1 * row[5]) + (uint32_t)(-W5 * row[7]);
        b2 += (uint32_t)( W7 * row[5]) + (uint32_t)( W3 * row[7]);
        b3 += (uint32_t)( W3 * row[5]) + (uint32_t)(-W1 * row[7]);
    }

    row[0] = (int16_t)((int)(a0 + b0) >> ROW_SHIFT);
    row[7] = (int16_t)((int)(a0 - b0) >> ROW_SHIFT);
    row[1] = (int16_t)((int)(a1 + b1) >> ROW_SHIFT);
    row[6] = (int16_t)((int)(a1 - b1) >> ROW_SHIFT);
    row[2] = (int16_t)((int)(a2 + b2) >> ROW_SHIFT);
    row[5] = (int16_t)((int)(a2 - b2) >> ROW_SHIFT);
    row[3] = (int16_t)((int)(a3 + b3) >> ROW_SHIFT);
    row[4] = (int16_t)((int)(a3 - b3) >> ROW_SHIFT);
}

static void idctSparseColPut(uint8_t* dest, int linesize, int16_t* col) {
    uint32_t a0, a1, a2, a3, b0, b1, b2, b3;

    /* col[8*k] accesses row k of this column */
    a0 = (uint32_t)(W4 * col[0]) + (1u << (COL_SHIFT - 1));
    a1 = a0; a2 = a0; a3 = a0;

    a0 += (uint32_t)( W2 * col[16]);
    a1 += (uint32_t)( W6 * col[16]);
    a2 += (uint32_t)(-W6 * col[16]);
    a3 += (uint32_t)(-W2 * col[16]);

    b0 = (uint32_t)(W1 * col[8]);
    b1 = (uint32_t)(W3 * col[8]);
    b2 = (uint32_t)(W5 * col[8]);
    b3 = (uint32_t)(W7 * col[8]);

    b0 += (uint32_t)( W3 * col[24]);
    b1 += (uint32_t)(-W7 * col[24]);
    b2 += (uint32_t)(-W1 * col[24]);
    b3 += (uint32_t)(-W5 * col[24]);

    if (col[32]) {
        a0 += (uint32_t)( W4 * col[32]);
        a1 += (uint32_t)(-W4 * col[32]);
        a2 += (uint32_t)(-W4 * col[32]);
        a3 += (uint32_t)( W4 * col[32]);
    }
    if (col[40]) {
        b0 += (uint32_t)( W5 * col[40]);
        b1 += (uint32_t)(-W1 * col[40]);
        b2 += (uint32_t)( W7 * col[40]);
        b3 += (uint32_t)( W3 * col[40]);
    }
    if (col[48]) {
        a0 += (uint32_t)( W6 * col[48]);
        a1 += (uint32_t)(-W2 * col[48]);
        a2 += (uint32_t)( W2 * col[48]);
        a3 += (uint32_t)(-W6 * col[48]);
    }
    if (col[56]) {
        b0 += (uint32_t)( W7 * col[56]);
        b1 += (uint32_t)(-W5 * col[56]);
        b2 += (uint32_t)( W3 * col[56]);
        b3 += (uint32_t)(-W1 * col[56]);
    }

    dest[0] = (uint8_t)clamp_u8((int)(a0 + b0) >> COL_SHIFT); dest += linesize;
    dest[0] = (uint8_t)clamp_u8((int)(a1 + b1) >> COL_SHIFT); dest += linesize;
    dest[0] = (uint8_t)clamp_u8((int)(a2 + b2) >> COL_SHIFT); dest += linesize;
    dest[0] = (uint8_t)clamp_u8((int)(a3 + b3) >> COL_SHIFT); dest += linesize;
    dest[0] = (uint8_t)clamp_u8((int)(a3 - b3) >> COL_SHIFT); dest += linesize;
    dest[0] = (uint8_t)clamp_u8((int)(a2 - b2) >> COL_SHIFT); dest += linesize;
    dest[0] = (uint8_t)clamp_u8((int)(a1 - b1) >> COL_SHIFT); dest += linesize;
    dest[0] = (uint8_t)clamp_u8((int)(a0 - b0) >> COL_SHIFT);
}

/* ff_simple_idct_put: rows then columns, writes clamped uint8_t to dest */
static void simple_idct_put(uint8_t* dest, int linesize, int16_t* block) {
    for (int i = 0; i < 8; i++)
        idctRowCondDC(block + i * 8);
    for (int i = 0; i < 8; i++)
        idctSparseColPut(dest + i, linesize, block + i);
}

#undef W1
#undef W2
#undef W3
#undef W4
#undef W5
#undef W6
#undef W7
#undef ROW_SHIFT
#undef COL_SHIFT
#undef DC_SHIFT

/* ============================================================================
 * Decoder state
 * ============================================================================ */
static FILE*    g_fmv_bin  = nullptr;
static uint32_t g_fmv_lba  = 0;
static bool     g_fmv_active = false;
static int      g_fmv_width  = 0, g_fmv_height = 0;
static int      g_chunk_total        = 0;
static int      g_chunks_collected   = 0;
static int      g_consecutive_nosector = 0;
static int      g_frame_count  = 0;
static int      g_frame_log    = 0;
static int      g_fmv_debug_frames = 0;

static constexpr int MAX_CHUNKS = 32;
static constexpr int DEMUX_MAX  = MAX_CHUNKS * PAYLOAD_SIZE;
static uint8_t  g_demux[DEMUX_MAX];
static int      g_demux_bytes = 0;

static constexpr int FRAME_MAX_W = 320;
static constexpr int FRAME_MAX_H = 256;
static uint16_t g_rgb555[FRAME_MAX_W * FRAME_MAX_H];

/* bswap scratch buffer — same size as g_demux */
static uint8_t  g_bswap[DEMUX_MAX];

/* YCbCr planes: maximum macroblock grid = 20×16 macroblocks (320×256px)
 * luma:   320×256 = 81920 bytes
 * chroma: 160×128 = 20480 bytes each */
static uint8_t g_plane_y [320 * 256];
static uint8_t g_plane_cb[160 * 128];
static uint8_t g_plane_cr[160 * 128];

/* ============================================================================
 * decode_frame — port of FFmpeg mdec.c decode_frame()
 *
 * Exact algorithm from mdec.c:
 *   bswap16 the bitstream → init_get_bits → skip preamble → read qscale/version
 *   for mb_x in [0, mb_width): for mb_y in [0, mb_height):
 *     decode_mb with block_index[] = {5,4,0,1,2,3} → idct_put
 * ============================================================================ */
static void decode_frame(void) {
    if (g_demux_bytes < FRAME_HDR_BYTES) return;

    /* Read qscale from LE header bytes 4-5 (before bswap) */
    const int qscale = (int)(g_demux[4] | ((int)g_demux[5] << 8));

    const int w = g_fmv_width, h = g_fmv_height;
    if (w <= 0 || h <= 0 || w > FRAME_MAX_W || h > FRAME_MAX_H) {
        fprintf(stderr, "[FMV] Bad frame size %dx%d\n", w, h);
        return;
    }

    const int mbW    = (w + 15) / 16;
    const int mbH    = (h + 15) / 16;
    const int chromaW = mbW * 8;
    const int lumaW   = mbW * 16;

    /* bswap16 the bitstream (everything after the 8-byte STR frame header).
     * Matches FFmpeg mdec.c: bbdsp.bswap16_buf on the entire input packet. */
    const int bs_len = g_demux_bytes - FRAME_HDR_BYTES;
    if (bs_len <= 0) return;
    bswap16_buf(g_bswap, g_demux + FRAME_HDR_BYTES, bs_len);

    BitCtx bc;
    bc_init(&bc, g_bswap, bs_len);

    /* Macroblock decode — column-major (mb_x outer, mb_y inner), matching FFmpeg */
    /* Block decode order {5,4,0,1,2,3} = Cr, Cb, Y_TL, Y_TR, Y_BL, Y_BR */
    static const int block_index[6] = { 5, 4, 0, 1, 2, 3 };

    int16_t block[6][64];

    for (int mbX = 0; mbX < mbW; mbX++) {
        for (int mbY = 0; mbY < mbH; mbY++) {
            /* Clear all 6 blocks (bdsp.clear_blocks in FFmpeg) */
            memset(block, 0, sizeof(block));

            /* Decode 6 blocks in FFmpeg order */
            for (int bi = 0; bi < 6; bi++)
                mdec_decode_block(&bc, block[block_index[bi]], qscale);

            /* IDCT + place into planes — matches FFmpeg's idct_put() */
            /* Cr: block[5] → plane_cr at (mbX*8, mbY*8) */
            uint8_t* dcr = g_plane_cr + mbY * 8 * chromaW + mbX * 8;
            simple_idct_put(dcr, chromaW, block[5]);

            /* Cb: block[4] → plane_cb at (mbX*8, mbY*8) */
            uint8_t* dcb = g_plane_cb + mbY * 8 * chromaW + mbX * 8;
            simple_idct_put(dcb, chromaW, block[4]);

            /* Y: blocks 0-3 → plane_y at 2×2 sub-blocks within 16×16 MB */
            uint8_t* dy = g_plane_y + mbY * 16 * lumaW + mbX * 16;
            simple_idct_put(dy,                    lumaW, block[0]); /* TL */
            simple_idct_put(dy + 8,                lumaW, block[1]); /* TR */
            simple_idct_put(dy + 8 * lumaW,        lumaW, block[2]); /* BL */
            simple_idct_put(dy + 8 * lumaW + 8,    lumaW, block[3]); /* BR */
        }
    }

    /* YCbCr (JPEG range) → RGB555
     * Y, Cb, Cr all in [0,255]; Cb=Cr=128 is neutral chroma.
     * 4:2:0: each chroma sample covers a 2×2 luma region. */
    memset(g_rgb555, 0, sizeof(g_rgb555));
    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            int cx = px >> 1, cy = py >> 1;
            int Y  = g_plane_y [py * lumaW   + px];
            int Cb = g_plane_cb[cy * chromaW + cx] - 128;
            int Cr = g_plane_cr[cy * chromaW + cx] - 128;

            int R = clamp_u8(Y + (int)((CR_R * Cr) >> 16));
            int G = clamp_u8(Y + (int)((CB_G * Cb + CR_G * Cr) >> 16));
            int B = clamp_u8(Y + (int)((CB_B * Cb) >> 16));

            /* PS1 RGB555: bits [14:10]=B [9:5]=G [4:0]=R, bit15=0 */
            g_rgb555[py * w + px] = (uint16_t)(
                ((B >> 3) << 10) | ((G >> 3) << 5) | (R >> 3));
        }
    }

    /* Debug: dump first 5 frames as PPM for visual inspection */
    if (g_fmv_debug_frames < 5) {
        g_fmv_debug_frames++;
        char path[64];
        snprintf(path, sizeof(path), "C:/temp/fmv_frame_%03d.ppm", g_fmv_debug_frames);
        FILE* fp = fopen(path, "wb");
        if (fp) {
            fprintf(fp, "P6\n%d %d\n255\n", w, h);
            for (int i = 0; i < w * h; i++) {
                uint16_t pix = g_rgb555[i];
                fputc(((pix >>  0) & 0x1F) << 3, fp); /* R */
                fputc(((pix >>  5) & 0x1F) << 3, fp); /* G */
                fputc(((pix >> 10) & 0x1F) << 3, fp); /* B */
            }
            fclose(fp);
            printf("[FMV] Saved %s\n", path);
            fflush(stdout);
        }
    }
}

/* ============================================================================
 * Public C interface (sector demux — unchanged from previous implementation)
 * ============================================================================ */
extern "C" {

void fmv_player_init(const char* bin_path) {
    if (g_fmv_bin) { fclose(g_fmv_bin); g_fmv_bin = nullptr; }
    g_fmv_bin = fopen(bin_path, "rb");
    if (!g_fmv_bin)
        fprintf(stderr, "[FMV] Cannot open BIN: %s\n", bin_path);
    else
        printf("[FMV] Opened %s\n", bin_path);
}

void fmv_player_seek(uint32_t lba) {
    g_fmv_lba              = lba;
    g_fmv_active           = true;
    g_demux_bytes          = 0;
    g_chunk_total          = 0;
    g_chunks_collected     = 0;
    g_consecutive_nosector = 0;
    g_frame_count          = 0;
    g_frame_log            = 0;
    g_fmv_width            = 0;
    g_fmv_height           = 0;
    g_fmv_debug_frames     = 0;
    printf("[FMV] Seek to LBA %u\n", lba);
    fflush(stdout);
}

int fmv_player_is_active(void) { return g_fmv_active ? 1 : 0; }

void fmv_player_stop(void) {
    g_fmv_active = false;
}

int fmv_player_tick(void) {
    if (!g_fmv_bin || !g_fmv_active) return 1;

    uint8_t raw[RAW_SECTOR];

    for (int attempts = 0; attempts < 200; attempts++) {
        long off = (long)((uint64_t)g_fmv_lba * RAW_SECTOR);
        if (fseek(g_fmv_bin, off, SEEK_SET) != 0 ||
            fread(raw, 1, RAW_SECTOR, g_fmv_bin) != RAW_SECTOR) {
            printf("[FMV] EOF at LBA %u after %d frames\n", g_fmv_lba, g_frame_count);
            fflush(stdout);
            g_fmv_active = false;
            xa_audio_seek(0);
            return 1;
        }

        uint8_t submode = raw[SUBHDR_OFF + 2];
        bool is_eof   = (submode & 0x80) != 0;
        bool is_video = (submode & 0x08) != 0;

        g_fmv_lba++;

        if (is_eof) {
            printf("[FMV] EOD sector after %d frames\n", g_frame_count);
            fflush(stdout);
            g_fmv_active = false;
            xa_audio_seek(0);
            return 1;
        }

        if (!is_video) {
            if (++g_consecutive_nosector > 100) {
                printf("[FMV] No video for 100 sectors — FMV done (%d frames)\n", g_frame_count);
                fflush(stdout);
                g_fmv_active = false;
                xa_audio_seek(0);
                return 1;
            }
            continue;
        }
        g_consecutive_nosector = 0;

        const uint8_t* v = raw + STR_HDR_OFF;
        uint16_t chunk_num   = (uint16_t)(v[4]  | (v[5]  << 8));
        uint16_t chunk_total = (uint16_t)(v[6]  | (v[7]  << 8));
        uint16_t width       = (uint16_t)(v[16] | (v[17] << 8));
        uint16_t height      = (uint16_t)(v[18] | (v[19] << 8));

        if (chunk_num == 0) {
            g_demux_bytes      = 0;
            g_chunks_collected = 0;
            g_chunk_total      = chunk_total;
            g_fmv_width        = width;
            g_fmv_height       = height;
        }

        int space = DEMUX_MAX - g_demux_bytes;
        int copy  = (PAYLOAD_SIZE < space) ? PAYLOAD_SIZE : space;
        if (copy > 0)
            memcpy(g_demux + g_demux_bytes, raw + PAYLOAD_OFF, copy);
        g_demux_bytes += PAYLOAD_SIZE;
        g_chunks_collected++;

        if (g_chunk_total > 0 && g_chunks_collected >= g_chunk_total) {
            decode_frame();
            g_frame_count++;

            if (++g_frame_log <= 5)
                printf("[FMV] Frame %d  %dx%d  qscale=%d\n",
                       g_frame_count, g_fmv_width, g_fmv_height,
                       (int)(g_demux[4] | (g_demux[5] << 8)));

            fmv_vram_upload(384, 256, g_fmv_width, g_fmv_height, g_rgb555);
            fmv_force_display_area(384, 256, g_fmv_width, g_fmv_height);

            /* Present + throttle to 15fps (authentic PS1 STR framerate).
             * CD-ROM 2x reads ~150 sectors/sec; ~10 sectors/frame = 15fps. */
            {
                static LARGE_INTEGER s_fmv_freq = {};
                static LARGE_INTEGER s_fmv_last = {};
                static bool s_fmv_init = false;
                if (!s_fmv_init) {
                    QueryPerformanceFrequency(&s_fmv_freq);
                    QueryPerformanceCounter(&s_fmv_last);
                    s_fmv_init = true;
                }

                psx_present_frame();

                if (!g_turbo) {
                    LARGE_INTEGER now;
                    LONGLONG target = s_fmv_freq.QuadPart / 15;
                    do { QueryPerformanceCounter(&now); }
                    while (now.QuadPart - s_fmv_last.QuadPart < target);
                    s_fmv_last = now;
                } else {
                    QueryPerformanceCounter(&s_fmv_last);
                }
            }

            return 1;
        }
    }

    return 0;
}

void fmv_player_shutdown(void) {
    if (g_fmv_bin) { fclose(g_fmv_bin); g_fmv_bin = nullptr; }
    g_fmv_active = false;
}

} /* extern "C" */
