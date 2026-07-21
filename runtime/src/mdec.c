#include "mdec.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern uint64_t s_frame_count;

/* FMV-activity detector: frame stamp of the newest colour (15/24-bit) MDEC
 * decode. Streamed video decodes every frame; texture decompression uses the
 * 4/8-bit luma path and does not stamp. */
static uint64_t mdec_last_color_decode_frame = (uint64_t)0 - 1000u;

enum {
    MDEC_CMD_NOP = 0,
    MDEC_CMD_DECODE = 1,
    MDEC_CMD_SET_QUANT = 2,
    MDEC_CMD_SET_SCALE = 3
};

enum {
    MDEC_EVT_RESET = 1,
    MDEC_EVT_CTRL_WRITE,
    MDEC_EVT_CMD_BEGIN,
    MDEC_EVT_CMD_DONE,
    MDEC_EVT_DECODE_DONE,
    MDEC_EVT_DMA_IN_START,
    MDEC_EVT_DMA_IN_END,
    MDEC_EVT_DMA_OUT_START,
    MDEC_EVT_DMA_OUT_END,
    MDEC_EVT_OUTPUT_DRAINED,
    MDEC_EVT_READ_UNDERFLOW
};

enum {
    MDEC_STOP_NONE = 0,
    MDEC_STOP_INPUT_END = 1,
    MDEC_STOP_CR = 2,
    MDEC_STOP_CB = 3,
    MDEC_STOP_Y0 = 4,
    MDEC_STOP_Y1 = 5,
    MDEC_STOP_Y2 = 6,
    MDEC_STOP_Y3 = 7
};

/* Zig-zag scatter table — Beetle ZigZag[64] (mdec.cpp:115), the column-major
 * order that pairs with Beetle's IDCT matrix transpose + IDCT_1D_Multi below.
 * (Our old table was the row-major transpose of this; it only decoded correctly
 * because our old IDCT was correspondingly transposed. The faithful pipeline
 * uses Beetle's table + matrix + IDCT together so the result is byte-exact.) */
static const uint8_t zigzag_to_linear[64] = {
    0x00, 0x08, 0x01, 0x02, 0x09, 0x10, 0x18, 0x11,
    0x0a, 0x03, 0x04, 0x0b, 0x12, 0x19, 0x20, 0x28,
    0x21, 0x1a, 0x13, 0x0c, 0x05, 0x06, 0x0d, 0x14,
    0x1b, 0x22, 0x29, 0x30, 0x38, 0x31, 0x2a, 0x23,
    0x1c, 0x15, 0x0e, 0x07, 0x0f, 0x16, 0x1d, 0x24,
    0x2b, 0x32, 0x39, 0x3a, 0x33, 0x2c, 0x25, 0x1e,
    0x17, 0x1f, 0x26, 0x2d, 0x34, 0x3b, 0x3c, 0x35,
    0x2e, 0x27, 0x2f, 0x36, 0x3d, 0x3e, 0x37, 0x3f
};

typedef struct MDECState {
    uint32_t command;
    uint32_t expected_halfwords;
    uint32_t input_count;
    uint16_t *input;
    uint32_t input_cap;

    uint8_t *output;
    uint32_t output_size;
    uint32_t output_pos;
    uint32_t output_cap;

    uint8_t y_quant[64];
    uint8_t uv_quant[64];
    int16_t scale[64];

    uint8_t output_bit15;
    uint8_t output_signed;
    uint8_t output_depth;
    uint8_t current_block;
    uint8_t busy;
    uint8_t input_full;
    uint8_t enable_dma_in;
    uint8_t enable_dma_out;

    uint32_t last_status;
    uint32_t decode_macroblocks;
    uint32_t decode_blocks;
    uint32_t decode_stop_reason;
    uint32_t decode_input_pos;
    uint32_t decode_input_end;
    uint32_t dma_in_words;
    uint32_t dma_out_words;
    uint32_t dma_read_underflows;
} MDECState;

static MDECState mdec;

#define MDEC_TRACE_CAP 4096u
static MDECDebugEvent mdec_trace[MDEC_TRACE_CAP];
static uint64_t mdec_trace_seq;
static uint32_t mdec_trace_head;

static void trace_event(uint32_t kind, uint32_t value) {
    MDECDebugEvent *e = &mdec_trace[mdec_trace_head];
    e->seq = mdec_trace_seq++;
    e->frame = (uint32_t)s_frame_count;
    e->kind = kind;
    e->value = value;
    e->command = mdec.command;
    e->input_count = mdec.input_count;
    e->expected_halfwords = mdec.expected_halfwords;
    e->output_size = mdec.output_size;
    e->output_pos = mdec.output_pos;
    e->macroblocks = mdec.decode_macroblocks;
    e->blocks = mdec.decode_blocks;
    e->stop_reason = mdec.decode_stop_reason;
    e->underruns = mdec.dma_read_underflows;
    mdec_trace_head = (mdec_trace_head + 1u) % MDEC_TRACE_CAP;
}

static int16_t sign_extend_10(uint16_t value) {
    return (int16_t)((int16_t)(value << 6) >> 6);
}

static int clamp_int(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static void clear_output(void) {
    mdec.output_size = 0;
    mdec.output_pos = 0;
}

static int ensure_input_capacity(uint32_t halfwords) {
    if (halfwords <= mdec.input_cap) return 1;
    uint32_t new_cap = mdec.input_cap ? mdec.input_cap : 256u;
    while (new_cap < halfwords) new_cap *= 2u;
    uint16_t *new_input = (uint16_t *)realloc(mdec.input, new_cap * sizeof(uint16_t));
    if (!new_input) return 0;
    mdec.input = new_input;
    mdec.input_cap = new_cap;
    return 1;
}

static int ensure_output_capacity(uint32_t bytes) {
    if (bytes <= mdec.output_cap) return 1;
    uint32_t new_cap = mdec.output_cap ? mdec.output_cap : 4096u;
    while (new_cap < bytes) new_cap *= 2u;
    uint8_t *new_output = (uint8_t *)realloc(mdec.output, new_cap);
    if (!new_output) return 0;
    mdec.output = new_output;
    mdec.output_cap = new_cap;
    return 1;
}

static void append_byte(uint8_t value) {
    if (!ensure_output_capacity(mdec.output_size + 1u)) return;
    mdec.output[mdec.output_size++] = value;
}

/* Reserve `bytes` of output room once (MotK FMV macroblocks write 512/768
 * bytes each). Avoids ensure_output_capacity per channel byte. */
static uint8_t *output_reserve(uint32_t bytes) {
    if (!ensure_output_capacity(mdec.output_size + bytes)) return NULL;
    return mdec.output + mdec.output_size;
}

static uint8_t input_byte(uint32_t byte_index) {
    uint16_t hw = mdec.input[byte_index >> 1];
    return (byte_index & 1u) ? (uint8_t)(hw >> 8) : (uint8_t)hw;
}

static void finish_command(void) {
    mdec.expected_halfwords = 0;
    mdec.input_count = 0;
    mdec.busy = 0;
    mdec.input_full = 0;
    trace_event(MDEC_EVT_CMD_DONE, mdec.command);
}

static void soft_reset(void) {
    uint16_t *input = mdec.input;
    uint32_t input_cap = mdec.input_cap;
    uint8_t *output = mdec.output;
    uint32_t output_cap = mdec.output_cap;
    uint8_t y_quant[64];
    uint8_t uv_quant[64];
    int16_t scale[64];

    memcpy(y_quant, mdec.y_quant, sizeof(y_quant));
    memcpy(uv_quant, mdec.uv_quant, sizeof(uv_quant));
    memcpy(scale, mdec.scale, sizeof(scale));

    memset(&mdec, 0, sizeof(mdec));
    mdec.input = input;
    mdec.input_cap = input_cap;
    mdec.output = output;
    mdec.output_cap = output_cap;
    memcpy(mdec.y_quant, y_quant, sizeof(mdec.y_quant));
    memcpy(mdec.uv_quant, uv_quant, sizeof(mdec.uv_quant));
    memcpy(mdec.scale, scale, sizeof(mdec.scale));
    mdec.output_depth = 3;
    mdec.current_block = 4;
}

/* Sign-extend the low `bits` of v to a full int (Beetle sign_x_to_s32). */
static int sign_x_to_s32(int bits, int v) {
    int shift = 32 - bits;
    return (int)(((int32_t)((uint32_t)v << shift)) >> shift);
}

/* 9-bit mask then clamp to int8 (Beetle Mask9ClampS8, mdec.cpp:230). The MDEC
 * keeps intermediate samples to 9 bits before the ±127 clamp, so a value outside
 * the 9-bit window WRAPS before clamping — reproducing the hardware ringing. */
static int mask9_clamp_s8(int v) {
    v = sign_x_to_s32(9, v);
    if (v < -128) v = -128;
    if (v >  127) v =  127;
    return v;
}

/* Faithful R3000A MDEC IDCT (Beetle IDCT/IDCT_1D_Multi, mdec.cpp:243-291).
 * Two separable 1-D passes over the >>3 scale matrix: pass 1 keeps int16 and
 * transposes (out[x*8+col]); pass 2 clamps to int8 via Mask9ClampS8. Rounding
 * is (sum + 0x4000) >> 15 (vs the old (sum+0xFFF)/0x2000 + per-tap /8, whose
 * bias differed from hardware → the washed/wrong-contrast FMV). The block buffer
 * holds int8-range samples on return. */
static void idct_block(int16_t *block) {
    /* DC-only fast path: bit-identical to the loops below when AC is zero
     * (common in dark/flat FMV macroblocks). Pass1 only fills tmp[*][0];
     * pass2 only multiplies by scale[*][0]. */
    int ac = 0;
    for (int i = 1; i < 64; i++) ac |= block[i];
    if (!ac) {
        int16_t tmp0[8];
        int dc = block[0];
        for (int x = 0; x < 8; x++) {
            int sum = dc * (int)mdec.scale[x * 8];
            tmp0[x] = (int16_t)((sum + 0x4000) >> 15);
        }
        for (int col = 0; col < 8; col++) {
            for (int x = 0; x < 8; x++) {
                int sum = (int)tmp0[col] * (int)mdec.scale[x * 8];
                block[col * 8 + x] =
                    (int16_t)mask9_clamp_s8((sum + 0x4000) >> 15);
            }
        }
        return;
    }

    int16_t tmp[64];
    /* pass 1 — int16 out, transposed. Skip all-zero source columns
     * (sparse AC after zig-zag); zero column ⇒ zero tmp[*][col]. */
    for (int col = 0; col < 8; col++) {
        const int16_t *src = block + col * 8;
        int col_or = (int)src[0] | (int)src[1] | (int)src[2] | (int)src[3]
                   | (int)src[4] | (int)src[5] | (int)src[6] | (int)src[7];
        if (!col_or) {
            for (int x = 0; x < 8; x++) tmp[x * 8 + col] = 0;
            continue;
        }
        for (int x = 0; x < 8; x++) {
            int sum = 0;
            const int16_t *sc = mdec.scale + x * 8;
            for (int u = 0; u < 8; u++)
                sum += (int)src[u] * (int)sc[u];
            tmp[x * 8 + col] = (int16_t)((sum + 0x4000) >> 15);
        }
    }
    /* pass 2 — int8 out (Mask9ClampS8), no transpose. Same zero-row skip. */
    for (int col = 0; col < 8; col++) {
        const int16_t *src = tmp + col * 8;
        int col_or = (int)src[0] | (int)src[1] | (int)src[2] | (int)src[3]
                   | (int)src[4] | (int)src[5] | (int)src[6] | (int)src[7];
        if (!col_or) {
            for (int x = 0; x < 8; x++) block[col * 8 + x] = 0;
            continue;
        }
        for (int x = 0; x < 8; x++) {
            int sum = 0;
            const int16_t *sc = mdec.scale + x * 8;
            for (int u = 0; u < 8; u++)
                sum += (int)src[u] * (int)sc[u];
            block[col * 8 + x] = (int16_t)mask9_clamp_s8((sum + 0x4000) >> 15);
        }
    }
}

static int decode_rle_block(int16_t *block, const uint8_t *quant,
                            uint32_t *pos, uint32_t end) {
    memset(block, 0, 64 * sizeof(int16_t));
    if (*pos >= end) return 0;
    mdec.decode_blocks++;

    uint16_t word = mdec.input[(*pos)++];
    while (word == 0xFE00u && *pos < end) {
        word = mdec.input[(*pos)++];
    }

    /* Dequant in Beetle's <<4 fixed-point domain (mdec.cpp:439-485), clamp
     * ±0x4000. DC uses quant[0] with no qscale; AC uses qscale*quant[k]. Each
     * nonzero coeff gets the sign-magnitude rounding bias (ci<0 ? +8 : -8) the
     * old +4/÷8 model omitted, and the <<4 domain feeds the >>3 IDCT matrix. */
    uint32_t qscale = (word >> 10) & 0x3Fu;
    uint32_t k = 0;
    int ci = sign_extend_10(word & 0x03FFu);
    int q  = (int)quant[0];
    int tmp = (q != 0) ? (((ci * q) << 4) + (ci ? (ci < 0 ? 8 : -8) : 0))
                       : ((ci * 2) << 4);
    block[0] = (int16_t)clamp_int(tmp, -0x4000, 0x3FFF);

    while (*pos < end && k < 63u) {
        word = mdec.input[(*pos)++];
        if (word == 0xFE00u) break;

        k += ((word >> 10) & 0x3Fu) + 1u;
        if (k >= 64u) break;

        ci = sign_extend_10(word & 0x03FFu);
        q  = (int)qscale * (int)quant[k];
        tmp = (q != 0) ? ((((ci * q) >> 3) << 4) + (ci ? (ci < 0 ? 8 : -8) : 0))
                       : ((ci * 2) << 4);
        block[zigzag_to_linear[k]] = (int16_t)clamp_int(tmp, -0x4000, 0x3FFF);
    }

    idct_block(block);
    return 1;
}

static uint8_t to_output_u8(int value) {
    value = clamp_int(value, -128, 127);
    if (mdec.output_signed) return (uint8_t)(int8_t)value;
    return (uint8_t)(value + 128);
}

/* 8-bit unsigned channel → 5-bit, Beetle RGB_to_RGB555 rounding (mdec.cpp:306).
 * Beetle's RGB_to_RGB555 takes uint8 params, so the ^0x80 result is truncated to
 * 0..255 BEFORE the round/shift — `c` here is already that uint8. */
static int rgb_to_555_chan(uint8_t c) {
    int v = (c + 4) >> 3;
    if (v > 0x1F) v = 0x1F;
    return v;
}

/* Emit one YCbCr pixel into a pre-reserved output cursor (bit-identical to
 * the former append_byte path). Returns advanced cursor. */
static uint8_t *emit_rgb_pixel(uint8_t *out, int y, int cr, int cb) {
    /* Beetle YCbCr_to_RGB (mdec.cpp:293-304): /256 coeffs (359,-88/-183,454),
     * +0x80 rounding, the reduced-precision GREEN mask (-88*cb &~0x1F, -183*cr
     * &~0x07) — the hardware quirk our old /1024 path lacked, the main green-hue
     * error — Mask9ClampS8, then ^0x80 to unsigned 0..255. */
    int r = mask9_clamp_s8(y + (((359 * cr) + 0x80) >> 8));
    int g = mask9_clamp_s8(y + ((((-88 * cb) & ~0x1F) + ((-183 * cr) & ~0x07) + 0x80) >> 8));
    int b = mask9_clamp_s8(y + (((454 * cb) + 0x80) >> 8));
    int ru = r ^ 0x80, gu = g ^ 0x80, bu = b ^ 0x80;   /* signed → unsigned */

    if (mdec.output_depth == 3) {
        /* 16bpp (mdec.cpp:397-418): RGB555 then pixel_xor = bit15(0x8000) |
         * signed(0x4210 = MSB of each 5-bit channel). */
        uint16_t packed = (uint16_t)(rgb_to_555_chan(ru)
                                     | (rgb_to_555_chan(gu) << 5)
                                     | (rgb_to_555_chan(bu) << 10));
        uint16_t pixel_xor = (uint16_t)((mdec.output_bit15 ? 0x8000u : 0u)
                                        | (mdec.output_signed ? 0x4210u : 0u));
        packed ^= pixel_xor;
        out[0] = (uint8_t)packed;
        out[1] = (uint8_t)(packed >> 8);
        return out + 2;
    }
    /* 24bpp (mdec.cpp:370-393): rgb_xor = signed ? 0x80 : 0x00. */
    uint8_t rgb_xor = mdec.output_signed ? 0x80u : 0x00u;
    out[0] = (uint8_t)(ru ^ rgb_xor);
    out[1] = (uint8_t)(gu ^ rgb_xor);
    out[2] = (uint8_t)(bu ^ rgb_xor);
    return out + 3;
}

static void append_luma_block(const int16_t *yblk) {
    uint8_t *out = output_reserve(64u);
    if (!out) return;
    for (int i = 0; i < 64; i++) out[i] = to_output_u8(yblk[i]);
    mdec.output_size += 64u;
}

static void append_color_macroblock(const int16_t *crblk, const int16_t *cbblk,
                                    const int16_t yblk[4][64]) {
    /* 16×16: 512 bytes @15bpp, 768 @24bpp — reserve once per MB. */
    uint32_t need = (mdec.output_depth == 3) ? 512u : 768u;
    uint8_t *out = output_reserve(need);
    if (!out) return;
    uint8_t *p = out;
    for (int py = 0; py < 16; py++) {
        for (int px = 0; px < 16; px++) {
            int y_index = (py >= 8 ? 2 : 0) + (px >= 8 ? 1 : 0);
            int lx = px & 7;
            int ly = py & 7;
            int chroma = (px >> 1) + (py >> 1) * 8;
            p = emit_rgb_pixel(p, yblk[y_index][lx + ly * 8],
                               crblk[chroma], cbblk[chroma]);
        }
    }
    mdec.output_size += need;
}

/* Monotonic count of decode invocations — the frontend FMV detector samples
 * this per display-frame to tell "MDEC is actively producing frames" (FMV)
 * from idle. */
static volatile uint32_t g_mdec_decode_count = 0;
uint32_t mdec_get_decode_count(void) { return g_mdec_decode_count; }

static void execute_decode(void) {
    uint32_t pos = 0;
    uint32_t end = mdec.input_count;
    g_mdec_decode_count++;
    clear_output();
    mdec.decode_macroblocks = 0;
    mdec.decode_blocks = 0;
    mdec.decode_stop_reason = MDEC_STOP_NONE;
    mdec.decode_input_pos = 0;
    mdec.decode_input_end = end;
    mdec.dma_out_words = 0;
    mdec.dma_read_underflows = 0;

    if (mdec.output_depth < 2) {
        int16_t yblk[64];
        while (pos < end && decode_rle_block(yblk, mdec.y_quant, &pos, end)) {
            append_luma_block(yblk);
            mdec.decode_macroblocks++;
        }
        mdec.decode_stop_reason = (pos >= end) ? MDEC_STOP_INPUT_END : MDEC_STOP_Y0;
        mdec.decode_input_pos = pos;
        trace_event(MDEC_EVT_DECODE_DONE, mdec.output_size);
        return;
    }

    while (pos < end) {
        int16_t crblk[64];
        int16_t cbblk[64];
        int16_t yblk[4][64];
        if (!decode_rle_block(crblk, mdec.uv_quant, &pos, end)) { mdec.decode_stop_reason = MDEC_STOP_CR; break; }
        if (!decode_rle_block(cbblk, mdec.uv_quant, &pos, end)) { mdec.decode_stop_reason = MDEC_STOP_CB; break; }
        if (!decode_rle_block(yblk[0], mdec.y_quant, &pos, end)) { mdec.decode_stop_reason = MDEC_STOP_Y0; break; }
        if (!decode_rle_block(yblk[1], mdec.y_quant, &pos, end)) { mdec.decode_stop_reason = MDEC_STOP_Y1; break; }
        if (!decode_rle_block(yblk[2], mdec.y_quant, &pos, end)) { mdec.decode_stop_reason = MDEC_STOP_Y2; break; }
        if (!decode_rle_block(yblk[3], mdec.y_quant, &pos, end)) { mdec.decode_stop_reason = MDEC_STOP_Y3; break; }
        append_color_macroblock(crblk, cbblk, yblk);
        mdec.decode_macroblocks++;
    }
    if (pos >= end && mdec.decode_stop_reason == MDEC_STOP_NONE) {
        mdec.decode_stop_reason = MDEC_STOP_INPUT_END;
    }
    mdec.decode_input_pos = pos;
    /* FMV detector: stamp colour (15/24-bit) decodes only — streamed video.
     * The 4/8-bit luma path above is texture decompression, not video. */
    mdec_last_color_decode_frame = s_frame_count;
    trace_event(MDEC_EVT_DECODE_DONE, mdec.output_size);
}

static void execute_command(void) {
    uint32_t op = mdec.command >> 29;
    if (op == MDEC_CMD_SET_QUANT) {
        for (uint32_t i = 0; i < 64u; i++) {
            mdec.y_quant[i] = input_byte(i);
        }
        if (mdec.command & 1u) {
            for (uint32_t i = 0; i < 64u; i++) {
                mdec.uv_quant[i] = input_byte(64u + i);
            }
        } else {
            memcpy(mdec.uv_quant, mdec.y_quant, sizeof(mdec.uv_quant));
        }
    } else if (op == MDEC_CMD_SET_SCALE) {
        /* Load the IDCT matrix exactly as Beetle (mdec.cpp:647): store each entry
         * TRANSPOSED ([(i&7)<<3 | (i>>3)&7]) and pre-shifted >>3 (arithmetic), so
         * IDCT_1D_Multi above can index it [x*8+u] directly with no per-tap /8. */
        for (uint32_t i = 0; i < 64u; i++) {
            uint32_t t = ((i & 7u) << 3) | ((i >> 3) & 7u);
            mdec.scale[t] = (int16_t)((int16_t)mdec.input[i] >> 3);
        }
    } else if (op == MDEC_CMD_DECODE) {
        execute_decode();
    }

    finish_command();
}

static void begin_command(uint32_t value) {
    mdec.command = value;
    mdec.output_bit15 = (uint8_t)((value >> 25) & 1u);
    mdec.output_signed = (uint8_t)((value >> 26) & 1u);
    mdec.output_depth = (uint8_t)((value >> 27) & 3u);
    mdec.current_block = 4;
    mdec.input_count = 0;
    mdec.input_full = 0;
    mdec.busy = 1;

    switch (value >> 29) {
        case MDEC_CMD_DECODE:
            mdec.expected_halfwords = (value & 0xFFFFu) * 2u;
            break;
        case MDEC_CMD_SET_QUANT:
            mdec.expected_halfwords = (value & 1u) ? 64u : 32u;
            break;
        case MDEC_CMD_SET_SCALE:
            mdec.expected_halfwords = 64u;
            break;
        default:
            mdec.expected_halfwords = 0;
            break;
    }

    if (mdec.expected_halfwords == 0 || !ensure_input_capacity(mdec.expected_halfwords)) {
        finish_command();
    } else {
        trace_event(MDEC_EVT_CMD_BEGIN, value);
    }
}

static void write_data(uint32_t value) {
    if (mdec.busy && mdec.input_count < mdec.expected_halfwords) {
        mdec.input[mdec.input_count++] = (uint16_t)value;
        if (mdec.input_count < mdec.expected_halfwords) {
            mdec.input[mdec.input_count++] = (uint16_t)(value >> 16);
        }
        if (mdec.input_count >= mdec.expected_halfwords) {
            execute_command();
        }
        return;
    }

    begin_command(value);
}

int mdec_recently_active(uint32_t within_frames) {
    return (uint64_t)(s_frame_count - mdec_last_color_decode_frame) <= within_frames;
}

void mdec_init(void) {
    memset(&mdec, 0, sizeof(mdec));
    memset(mdec_trace, 0, sizeof(mdec_trace));
    mdec_trace_seq = 0;
    mdec_trace_head = 0;
    /* Rematch resets s_frame_count; a stale stamp makes mdec_recently_active
     * wrap and lie for the whole next session. */
    mdec_last_color_decode_frame = (uint64_t)0 - 1000u;
    for (int i = 0; i < 64; i++) {
        mdec.y_quant[i] = 1;
        mdec.uv_quant[i] = 1;
    }
    mdec.output_depth = 3;
    mdec.current_block = 4;
}

uint32_t mdec_read(uint32_t addr) {
    uint32_t offset = addr & 7u;
    if (offset == 0) {
        return mdec_dma_read_word();
    }

    uint32_t remaining_words = 0;
    if (mdec.busy && mdec.expected_halfwords > mdec.input_count) {
        remaining_words = (mdec.expected_halfwords - mdec.input_count + 1u) / 2u;
    }

    uint32_t status = remaining_words ? ((remaining_words - 1u) & 0xFFFFu) : 0xFFFFu;
    status |= ((uint32_t)mdec.current_block & 7u) << 16;
    status |= ((uint32_t)mdec.output_bit15 & 1u) << 23;
    status |= ((uint32_t)mdec.output_signed & 1u) << 24;
    status |= ((uint32_t)mdec.output_depth & 3u) << 25;
    int write_ready = mdec_dma_write_ready();
    if (!write_ready) status |= 1u << 30;
    if (mdec.enable_dma_out && mdec_dma_read_ready()) status |= 1u << 27;
    if (mdec.enable_dma_in && write_ready) status |= 1u << 28;
    if (mdec.busy) status |= 1u << 29;
    if (mdec.output_pos >= mdec.output_size) status |= 1u << 31;
    mdec.last_status = status;
    return status;
}

void mdec_write(uint32_t addr, uint32_t value) {
    uint32_t offset = addr & 7u;
    if (offset == 0) {
        write_data(value);
        return;
    }

    if (value & 0x80000000u) {
        soft_reset();
        trace_event(MDEC_EVT_RESET, value);
    }
    mdec.enable_dma_in = (uint8_t)((value >> 30) & 1u);
    mdec.enable_dma_out = (uint8_t)((value >> 29) & 1u);
    trace_event(MDEC_EVT_CTRL_WRITE, value);
}

void mdec_dma_write_word(uint32_t value) {
    write_data(value);
}

/* Feed up to `max_words` from a contiguous LE word source (DMA ch0). Stops
 * early if the FIFO becomes not-ready after a decode completes mid-burst.
 * Guest bytes + decode triggers match N× mdec_dma_write_word. */
uint32_t mdec_dma_write_words(const uint32_t *src, uint32_t max_words) {
    uint32_t moved = 0;
    while (moved < max_words) {
        if (!mdec_dma_write_ready()) break;
        /* Fast fill while a DECODE/QUANT/SCALE command is collecting input —
         * same halfword packing + single execute_command as write_data. */
        if (mdec.busy && mdec.input_count < mdec.expected_halfwords) {
            uint32_t need_hw = mdec.expected_halfwords - mdec.input_count;
            uint32_t need_words = (need_hw + 1u) / 2u;
            uint32_t n = max_words - moved;
            if (n > need_words) n = need_words;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t value = src[moved++];
                mdec.input[mdec.input_count++] = (uint16_t)value;
                if (mdec.input_count < mdec.expected_halfwords) {
                    mdec.input[mdec.input_count++] = (uint16_t)(value >> 16);
                }
            }
            if (mdec.input_count >= mdec.expected_halfwords) {
                execute_command();
            }
            continue;
        }
        write_data(src[moved++]);
    }
    return moved;
}

uint32_t mdec_dma_read_word(void) {
    uint32_t value = 0;
    uint32_t start_pos = mdec.output_pos;
    uint32_t avail = (start_pos < mdec.output_size)
                   ? (mdec.output_size - start_pos) : 0u;
    if (avail >= 4u) {
        memcpy(&value, mdec.output + start_pos, sizeof(value));
        mdec.output_pos = start_pos + 4u;
    } else {
        for (uint32_t i = 0; i < 4u; i++) {
            if (mdec.output_pos < mdec.output_size) {
                value |= (uint32_t)mdec.output[mdec.output_pos++] << (i * 8u);
            }
        }
    }
    mdec.dma_out_words++;
    if (start_pos >= mdec.output_size) {
        mdec.dma_read_underflows++;
        if (mdec.dma_read_underflows == 1u) {
            trace_event(MDEC_EVT_READ_UNDERFLOW, mdec.dma_out_words);
        }
    }
    if (mdec.output_pos >= mdec.output_size) {
        trace_event(MDEC_EVT_OUTPUT_DRAINED, mdec.output_size);
        clear_output();
    }
    return value;
}

/* Drain up to `max_words` into a contiguous LE destination (DMA ch1). */
uint32_t mdec_dma_read_words(uint32_t *dst, uint32_t max_words) {
    uint32_t moved = 0;
    while (moved < max_words && mdec.output_pos < mdec.output_size) {
        dst[moved] = mdec_dma_read_word();
        moved++;
    }
    return moved;
}

int mdec_dma_write_ready(void) {
    if (mdec.output_pos < mdec.output_size) return 0;
    return !mdec.busy || mdec.input_count < mdec.expected_halfwords;
}

int mdec_dma_read_ready(void) {
    return mdec.output_pos < mdec.output_size;
}

void mdec_debug_get_state(MDECDebugState *out) {
    if (!out) return;
    out->command = mdec.command;
    out->expected_halfwords = mdec.expected_halfwords;
    out->input_count = mdec.input_count;
    out->output_size = mdec.output_size;
    out->output_pos = mdec.output_pos;
    out->output_depth = mdec.output_depth;
    out->output_signed = mdec.output_signed;
    out->output_bit15 = mdec.output_bit15;
    out->busy = mdec.busy;
    out->input_full = mdec.input_full;
    out->enable_dma_in = mdec.enable_dma_in;
    out->enable_dma_out = mdec.enable_dma_out;
    out->last_status = mdec.last_status;
    out->decode_macroblocks = mdec.decode_macroblocks;
    out->decode_blocks = mdec.decode_blocks;
    out->decode_stop_reason = mdec.decode_stop_reason;
    out->decode_input_pos = mdec.decode_input_pos;
    out->decode_input_end = mdec.decode_input_end;
    out->dma_in_words = mdec.dma_in_words;
    out->dma_out_words = mdec.dma_out_words;
    out->dma_read_underflows = mdec.dma_read_underflows;
}

uint64_t mdec_debug_get_event_total(void) {
    return mdec_trace_seq;
}

uint32_t mdec_debug_copy_events(uint64_t seq_lo, uint64_t seq_hi,
                                MDECDebugEvent *out, uint32_t max_count) {
    if (!out || max_count == 0) return 0;
    uint64_t oldest = (mdec_trace_seq > MDEC_TRACE_CAP) ? mdec_trace_seq - MDEC_TRACE_CAP : 0;
    if (seq_lo < oldest) seq_lo = oldest;
    if (seq_hi > mdec_trace_seq) seq_hi = mdec_trace_seq;
    uint32_t n = 0;
    for (uint64_t seq = seq_lo; seq < seq_hi && n < max_count; seq++) {
        out[n++] = mdec_trace[seq % MDEC_TRACE_CAP];
    }
    return n;
}

void mdec_debug_clear(void) {
    memset(mdec_trace, 0, sizeof(mdec_trace));
    mdec_trace_seq = 0;
    mdec_trace_head = 0;
}

void mdec_debug_dma_in_start(uint32_t addr, uint32_t words) {
    (void)addr;
    trace_event(MDEC_EVT_DMA_IN_START, words);
}

void mdec_debug_dma_in_end(uint32_t addr, uint32_t words) {
    (void)addr;
    mdec.dma_in_words += words;
    trace_event(MDEC_EVT_DMA_IN_END, words);
}

void mdec_debug_dma_out_start(uint32_t addr, uint32_t words) {
    (void)addr;
    trace_event(MDEC_EVT_DMA_OUT_START, words);
}

void mdec_debug_dma_out_end(uint32_t addr, uint32_t words) {
    (void)addr;
    trace_event(MDEC_EVT_DMA_OUT_END, words);
}
