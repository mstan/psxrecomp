#pragma once

#include <stdint.h>

#define PSX_NG_DESC_RAW_STORAGE 0u
#define PSX_NG_DESC_COLOR_STORAGE 1u
#define PSX_NG_DESC_RAW_SAMPLED 2u
#define PSX_NG_DESC_COLOR_SAMPLED 3u
#define PSX_NG_DESC_SCRATCH_STORAGE 4u
#define PSX_NG_DESC_CPU_SAMPLED 5u
#define PSX_NG_DESC_COUNT 6u

#define PSX_NG_PRIM_FILL 1u
#define PSX_NG_PRIM_SCRATCH_COPY_IN 2u
#define PSX_NG_PRIM_SCRATCH_COPY_OUT 3u
#define PSX_NG_PRIM_TRI 4u
#define PSX_NG_PRIM_LINE 5u
#define PSX_NG_PRIM_COLORIZE 6u
#define PSX_NG_PRIM_RECT 7u
#define PSX_NG_PRIM_TEX_RECT 8u

typedef struct PsxNgRasterRoot {
    uint32_t op;
    uint32_t textured;
    uint32_t gouraud;
    uint32_t raw_texture;

    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;

    int32_t sx;
    int32_t sy;
    int32_t dx;
    int32_t dy;

    int32_t clip_x1;
    int32_t clip_y1;
    int32_t clip_x2;
    int32_t clip_y2;

    int32_t mask_set;
    int32_t mask_check;
    int32_t semi_enabled;
    int32_t semi_mode;

    int32_t tw_mask_x;
    int32_t tw_mask_y;
    int32_t tw_off_x;
    int32_t tw_off_y;

    int32_t tpage_x;
    int32_t tpage_y;
    int32_t tex_depth;
    int32_t clut_x;
    int32_t clut_y;
    int32_t mod_r;
    int32_t mod_g;
    int32_t mod_b;

    float x0;
    float y0;
    float x1;
    float y1;
    float x2;
    float y2;

    float u0;
    float v0;
    float u1;
    float v1;
    float u2;
    float v2;

    uint32_t c0;
    uint32_t c1;
    uint32_t c2;
    uint32_t pad0;
} PsxNgRasterRoot;

typedef struct PsxNgPresentRoot {
    uint32_t texture_index;
    uint32_t sampler_index;
    int32_t src_x;
    int32_t src_y;
    int32_t src_w;
    int32_t src_h;
    int32_t dst_w;
    int32_t dst_h;
    int32_t force_4_3;
    int32_t pad0;
} PsxNgPresentRoot;
