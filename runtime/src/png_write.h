/* png_write.h — minimal, dependency-free RGB→PNG writer.
 *
 * A real PNG (8-bit truecolor) whose IDAT is a zlib stream of "stored"
 * (uncompressed) DEFLATE blocks: bigger on disk than a compressed PNG, but
 * accepted by every viewer (and the harness Read tool), and nothing new to link
 * against — which keeps the self-contained static binaries self-contained.
 *
 * Header-only with `static` linkage so both the runtime debug server and the
 * Beetle debug server (separate binaries / translation units) can emit PNG from
 * one source, with no shared object to add to either build. */
#ifndef PSX_PNG_WRITE_H
#define PSX_PNG_WRITE_H

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t s_png_crc_tbl[256];
static int      s_png_crc_init = 0;
static void png_crc_build(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        s_png_crc_tbl[n] = c;
    }
    s_png_crc_init = 1;
}
static uint32_t png_crc_update(uint32_t crc, const uint8_t *p, size_t n) {
    if (!s_png_crc_init) png_crc_build();
    for (size_t i = 0; i < n; i++) crc = s_png_crc_tbl[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}
static uint32_t png_adler32(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    /* process in blocks so the 16-bit sums never overflow before the mod */
    while (n) {
        size_t k = n < 5552 ? n : 5552;
        for (size_t i = 0; i < k; i++) { a += p[i]; b += a; }
        a %= 65521; b %= 65521; p += k; n -= k;
    }
    return (b << 16) | a;
}
static void png_put_be32(FILE *f, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 8), (uint8_t)v };
    fwrite(b, 1, 4, f);
}
static void png_chunk(FILE *f, const char *type, const uint8_t *data, size_t len) {
    png_put_be32(f, (uint32_t)len);
    uint32_t crc = 0xFFFFFFFFu;
    crc = png_crc_update(crc, (const uint8_t *)type, 4);
    crc = png_crc_update(crc, data, len);
    fwrite(type, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    png_put_be32(f, crc ^ 0xFFFFFFFFu);
}
/* Write an interleaved 8-bit, top-down buffer as a PNG. `chans` is 3 (truecolor
 * RGB) or 4 (truecolor RGBA). Returns 1 on success. */
static int png_write_n(FILE *f, const uint8_t *px, uint32_t w, uint32_t h, int chans) {
    static const uint8_t sig[8] = { 137,80,78,71,13,10,26,10 };
    if (chans != 3 && chans != 4) return 0;
    fwrite(sig, 1, 8, f);

    uint8_t ihdr[13];
    ihdr[0]=(uint8_t)(w>>24); ihdr[1]=(uint8_t)(w>>16); ihdr[2]=(uint8_t)(w>>8); ihdr[3]=(uint8_t)w;
    ihdr[4]=(uint8_t)(h>>24); ihdr[5]=(uint8_t)(h>>16); ihdr[6]=(uint8_t)(h>>8); ihdr[7]=(uint8_t)h;
    ihdr[8]=8;   /* bit depth   */
    ihdr[9]=(uint8_t)(chans == 4 ? 6 : 2);  /* color type 2 = RGB, 6 = RGBA */
    ihdr[10]=0;  /* compression */
    ihdr[11]=0;  /* filter      */
    ihdr[12]=0;  /* interlace   */
    png_chunk(f, "IHDR", ihdr, sizeof ihdr);

    /* Filtered raw scanlines: each row prefixed with filter byte 0 (None). */
    size_t stride = (size_t)w * (size_t)chans;
    size_t raw_len = (size_t)h * (1 + stride);
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw) return 0;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * (1 + stride);
        row[0] = 0;
        memcpy(row + 1, px + (size_t)y * stride, stride);
    }

    /* zlib stream: 2-byte header + stored DEFLATE blocks + 4-byte Adler32. */
    size_t nblocks = (raw_len + 65534) / 65535; if (nblocks == 0) nblocks = 1;
    size_t z_len = 2 + nblocks * 5 + raw_len + 4;
    uint8_t *z = (uint8_t *)malloc(z_len);
    if (!z) { free(raw); return 0; }
    size_t zi = 0;
    z[zi++] = 0x78;  /* CMF: 32K window, deflate */
    z[zi++] = 0x01;  /* FLG: check bits make 0x7801 a multiple of 31 */
    size_t off = 0;
    while (off < raw_len) {
        size_t n = raw_len - off; if (n > 65535) n = 65535;
        int final = (off + n >= raw_len);
        z[zi++] = (uint8_t)(final ? 1 : 0);          /* BFINAL | BTYPE=00 */
        z[zi++] = (uint8_t)(n & 0xFF); z[zi++] = (uint8_t)(n >> 8);
        uint16_t nl = (uint16_t)~n;
        z[zi++] = (uint8_t)(nl & 0xFF); z[zi++] = (uint8_t)(nl >> 8);
        memcpy(z + zi, raw + off, n); zi += n; off += n;
    }
    uint32_t ad = png_adler32(raw, raw_len);
    z[zi++] = (uint8_t)(ad >> 24); z[zi++] = (uint8_t)(ad >> 16);
    z[zi++] = (uint8_t)(ad >> 8);  z[zi++] = (uint8_t)ad;
    free(raw);

    png_chunk(f, "IDAT", z, zi);
    free(z);
    png_chunk(f, "IEND", NULL, 0);
    return 1;
}

/* Write an RGB (3 bytes/pixel, top-down) buffer as a PNG. Returns 1 on success. */
static int png_write_rgb(FILE *f, const uint8_t *rgb, uint32_t w, uint32_t h) {
    return png_write_n(f, rgb, w, h, 3);
}

/* Write an RGBA (4 bytes/pixel, top-down, straight alpha) buffer as a PNG.
 * Used by the HD texture dumper (tex_pack.cpp), which needs the alpha channel
 * to carry the PS1 transparent / opaque / semi-transparent distinction. */
static int png_write_rgba(FILE *f, const uint8_t *rgba, uint32_t w, uint32_t h) {
    return png_write_n(f, rgba, w, h, 4);
}

#endif /* PSX_PNG_WRITE_H */
