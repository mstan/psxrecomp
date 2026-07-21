#include "rnet_sha1.h"

#include <string.h>

/* Public-domain SHA-1 (compact). */
typedef struct {
    uint32_t state[5];
    uint64_t bitcount;
    uint8_t buffer[64];
    size_t buffer_len;
} Sha1Ctx;

static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_process(Sha1Ctx *ctx, const uint8_t block[64])
{
    uint32_t w[80];
    uint32_t a, b, c, d, e, f, k, t;
    int i;
    for (i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (i = 16; i < 80; ++i) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    for (i = 0; i < 80; ++i) {
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
}

static void sha1_init(Sha1Ctx *ctx)
{
    ctx->state[0] = 0x67452301u;
    ctx->state[1] = 0xEFCDAB89u;
    ctx->state[2] = 0x98BADCFEu;
    ctx->state[3] = 0x10325476u;
    ctx->state[4] = 0xC3D2E1F0u;
    ctx->bitcount = 0;
    ctx->buffer_len = 0;
}

static void sha1_update(Sha1Ctx *ctx, const uint8_t *data, size_t len)
{
    size_t i = 0;
    ctx->bitcount += (uint64_t)len * 8u;
    while (i < len) {
        ctx->buffer[ctx->buffer_len++] = data[i++];
        if (ctx->buffer_len == 64) {
            sha1_process(ctx, ctx->buffer);
            ctx->buffer_len = 0;
        }
    }
}

static void sha1_final(Sha1Ctx *ctx, uint8_t out[20])
{
    size_t i;
    ctx->buffer[ctx->buffer_len++] = 0x80;
    if (ctx->buffer_len > 56) {
        while (ctx->buffer_len < 64) {
            ctx->buffer[ctx->buffer_len++] = 0;
        }
        sha1_process(ctx, ctx->buffer);
        ctx->buffer_len = 0;
    }
    while (ctx->buffer_len < 56) {
        ctx->buffer[ctx->buffer_len++] = 0;
    }
    for (i = 0; i < 8; ++i) {
        ctx->buffer[56 + i] = (uint8_t)((ctx->bitcount >> (56 - 8 * (int)i)) & 0xffu);
    }
    sha1_process(ctx, ctx->buffer);
    for (i = 0; i < 5; ++i) {
        out[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

void rnet_sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
    Sha1Ctx ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, out);
}
