#include "mdec.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    MDEC_CMD_NOP = 0,
    MDEC_CMD_DECODE = 1,
    MDEC_CMD_SET_QUANT = 2,
    MDEC_CMD_SET_SCALE = 3
};

static const uint8_t zigzag_to_linear[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
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
} MDECState;

static MDECState mdec;

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

static uint8_t input_byte(uint32_t byte_index) {
    uint16_t hw = mdec.input[byte_index >> 1];
    return (byte_index & 1u) ? (uint8_t)(hw >> 8) : (uint8_t)hw;
}

static void finish_command(void) {
    mdec.expected_halfwords = 0;
    mdec.input_count = 0;
    mdec.busy = 0;
    mdec.input_full = 1;
}

static void idct_block(int16_t *block) {
    int16_t tmp[64];
    int16_t *src = block;
    int16_t *dst = tmp;

    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                int sum = 0;
                for (int z = 0; z < 8; z++) {
                    sum += (int)src[y + z * 8] * ((int)mdec.scale[x + z * 8] / 8);
                }
                dst[x + y * 8] = (int16_t)((sum + 0xFFF) / 0x2000);
            }
        }

        int16_t *swap = src;
        src = dst;
        dst = swap;
    }
}

static int decode_rle_block(int16_t *block, const uint8_t *quant,
                            uint32_t *pos, uint32_t end) {
    memset(block, 0, 64 * sizeof(int16_t));
    if (*pos >= end) return 0;

    uint16_t word = mdec.input[(*pos)++];
    while (word == 0xFE00u && *pos < end) {
        word = mdec.input[(*pos)++];
    }

    uint32_t qscale = (word >> 10) & 0x3Fu;
    uint32_t k = 0;
    int coeff = sign_extend_10(word & 0x03FFu);
    coeff = (qscale == 0) ? (coeff * 2) : (coeff * (int)quant[0]);
    block[0] = (int16_t)clamp_int(coeff, -0x400, 0x3FF);

    while (*pos < end && k < 63u) {
        word = mdec.input[(*pos)++];
        if (word == 0xFE00u) break;

        k += ((word >> 10) & 0x3Fu) + 1u;
        if (k >= 64u) break;

        coeff = sign_extend_10(word & 0x03FFu);
        if (qscale == 0) {
            coeff *= 2;
        } else {
            coeff = (coeff * (int)quant[k] * (int)qscale + 4) / 8;
        }
        block[zigzag_to_linear[k]] = (int16_t)clamp_int(coeff, -0x400, 0x3FF);
    }

    idct_block(block);
    return 1;
}

static uint8_t to_output_u8(int value) {
    value = clamp_int(value, -128, 127);
    if (mdec.output_signed) return (uint8_t)(int8_t)value;
    return (uint8_t)(value + 128);
}

static void append_rgb_pixel(int y, int cr, int cb) {
    int r = y + ((1436 * cr) >> 10);
    int g = y - ((352 * cb + 731 * cr) >> 10);
    int b = y + ((1815 * cb) >> 10);

    uint8_t ru = to_output_u8(r);
    uint8_t gu = to_output_u8(g);
    uint8_t bu = to_output_u8(b);

    if (mdec.output_depth == 3) {
        uint16_t packed = (uint16_t)(((bu >> 3) << 10) | ((gu >> 3) << 5) | (ru >> 3));
        if (mdec.output_bit15) packed |= 0x8000u;
        append_byte((uint8_t)packed);
        append_byte((uint8_t)(packed >> 8));
    } else {
        append_byte(ru);
        append_byte(gu);
        append_byte(bu);
    }
}

static void append_luma_block(const int16_t *yblk) {
    for (int i = 0; i < 64; i++) {
        append_byte(to_output_u8(yblk[i]));
    }
}

static void append_color_macroblock(const int16_t *crblk, const int16_t *cbblk,
                                    const int16_t yblk[4][64]) {
    for (int py = 0; py < 16; py++) {
        for (int px = 0; px < 16; px++) {
            int y_index = (py >= 8 ? 2 : 0) + (px >= 8 ? 1 : 0);
            int lx = px & 7;
            int ly = py & 7;
            int chroma = (px >> 1) + (py >> 1) * 8;
            append_rgb_pixel(yblk[y_index][lx + ly * 8], crblk[chroma], cbblk[chroma]);
        }
    }
}

static void execute_decode(void) {
    uint32_t pos = 0;
    uint32_t end = mdec.input_count;
    clear_output();

    if (mdec.output_depth < 2) {
        int16_t yblk[64];
        while (pos < end && decode_rle_block(yblk, mdec.y_quant, &pos, end)) {
            append_luma_block(yblk);
        }
        return;
    }

    while (pos < end) {
        int16_t crblk[64];
        int16_t cbblk[64];
        int16_t yblk[4][64];
        if (!decode_rle_block(crblk, mdec.uv_quant, &pos, end)) break;
        if (!decode_rle_block(cbblk, mdec.uv_quant, &pos, end)) break;
        if (!decode_rle_block(yblk[0], mdec.y_quant, &pos, end)) break;
        if (!decode_rle_block(yblk[1], mdec.y_quant, &pos, end)) break;
        if (!decode_rle_block(yblk[2], mdec.y_quant, &pos, end)) break;
        if (!decode_rle_block(yblk[3], mdec.y_quant, &pos, end)) break;
        append_color_macroblock(crblk, cbblk, yblk);
    }
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
        for (uint32_t i = 0; i < 64u; i++) {
            mdec.scale[i] = (int16_t)mdec.input[i];
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

void mdec_init(void) {
    memset(&mdec, 0, sizeof(mdec));
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
    if (mdec.enable_dma_out && mdec.output_pos < mdec.output_size) status |= 1u << 27;
    if (mdec.enable_dma_in && mdec.busy && mdec.input_count < mdec.expected_halfwords) status |= 1u << 28;
    if (mdec.busy) status |= 1u << 29;
    if (mdec.input_full) status |= 1u << 30;
    if (mdec.output_pos >= mdec.output_size) status |= 1u << 31;
    return status;
}

void mdec_write(uint32_t addr, uint32_t value) {
    uint32_t offset = addr & 7u;
    if (offset == 0) {
        write_data(value);
        return;
    }

    if (value & 0x80000000u) {
        free(mdec.input);
        free(mdec.output);
        memset(&mdec, 0, sizeof(mdec));
        mdec.output_depth = 3;
        mdec.current_block = 4;
        for (int i = 0; i < 64; i++) {
            mdec.y_quant[i] = 1;
            mdec.uv_quant[i] = 1;
        }
    }
    mdec.enable_dma_in = (uint8_t)((value >> 30) & 1u);
    mdec.enable_dma_out = (uint8_t)((value >> 29) & 1u);
}

void mdec_dma_write_word(uint32_t value) {
    write_data(value);
}

uint32_t mdec_dma_read_word(void) {
    uint32_t value = 0;
    for (uint32_t i = 0; i < 4u; i++) {
        if (mdec.output_pos < mdec.output_size) {
            value |= (uint32_t)mdec.output[mdec.output_pos++] << (i * 8u);
        }
    }
    if (mdec.output_pos >= mdec.output_size) {
        clear_output();
    }
    return value;
}

int mdec_dma_write_ready(void) {
    return !mdec.busy || mdec.input_count < mdec.expected_halfwords;
}

int mdec_dma_read_ready(void) {
    return mdec.output_pos < mdec.output_size;
}
