/* pst_wire.h — little-endian field cursors for portable .pst / boot_state blobs.
 *
 * Never memcpy whole host structs that may contain padding. Emit fixed-width
 * LE primitives so Win/Linux x86_64 and macOS ARM produce identical bytes.
 */
#ifndef PSX_PST_WIRE_H
#define PSX_PST_WIRE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PstW {
    uint8_t *p;
    uint8_t *end;
    size_t   written; /* advances even when p==NULL (size probe) */
} PstW;

typedef struct PstR {
    const uint8_t *p;
    const uint8_t *end;
} PstR;

static inline void pst_w_init(PstW *w, uint8_t *buf, size_t cap) {
    w->p = buf;
    w->end = buf ? buf + cap : NULL;
    w->written = 0;
}

static inline void pst_r_init(PstR *r, const uint8_t *buf, size_t len) {
    r->p = buf;
    r->end = buf + len;
}

static inline int pst_w_bytes(PstW *w, const void *src, size_t n) {
    if (!w) return 0;
    if (w->p) {
        if (!w->end || (size_t)(w->end - w->p) < n) return 0;
        if (n && src) memcpy(w->p, src, n);
        w->p += n;
    }
    w->written += n;
    return 1;
}

static inline int pst_r_bytes(PstR *r, void *dst, size_t n) {
    if (!r || !r->p || (size_t)(r->end - r->p) < n) return 0;
    if (n && dst) memcpy(dst, r->p, n);
    r->p += n;
    return 1;
}

static inline int pst_w_u8(PstW *w, uint8_t v) { return pst_w_bytes(w, &v, 1); }
static inline int pst_r_u8(PstR *r, uint8_t *v) { return pst_r_bytes(r, v, 1); }

static inline int pst_w_u16(PstW *w, uint16_t v) {
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    return pst_w_bytes(w, b, 2);
}
static inline int pst_r_u16(PstR *r, uint16_t *v) {
    uint8_t b[2];
    if (!pst_r_bytes(r, b, 2)) return 0;
    if (v) *v = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
    return 1;
}

static inline int pst_w_u32(PstW *w, uint32_t v) {
    uint8_t b[4] = {
        (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)
    };
    return pst_w_bytes(w, b, 4);
}
static inline int pst_r_u32(PstR *r, uint32_t *v) {
    uint8_t b[4];
    if (!pst_r_bytes(r, b, 4)) return 0;
    if (v)
        *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
             ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 1;
}

static inline int pst_w_u64(PstW *w, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
    return pst_w_bytes(w, b, 8);
}
static inline int pst_r_u64(PstR *r, uint64_t *v) {
    uint8_t b[8];
    if (!pst_r_bytes(r, b, 8)) return 0;
    if (v) {
        uint64_t x = 0;
        for (int i = 0; i < 8; i++) x |= (uint64_t)b[i] << (8 * i);
        *v = x;
    }
    return 1;
}

static inline int pst_w_i16(PstW *w, int16_t v) { return pst_w_u16(w, (uint16_t)v); }
static inline int pst_r_i16(PstR *r, int16_t *v) {
    uint16_t u;
    if (!pst_r_u16(r, &u)) return 0;
    if (v) *v = (int16_t)u;
    return 1;
}

static inline int pst_w_i32(PstW *w, int32_t v) { return pst_w_u32(w, (uint32_t)v); }
static inline int pst_r_i32(PstR *r, int32_t *v) {
    uint32_t u;
    if (!pst_r_u32(r, &u)) return 0;
    if (v) *v = (int32_t)u;
    return 1;
}

/* bool / flag → single byte 0/1 */
static inline int pst_w_bool(PstW *w, int v) {
    return pst_w_u8(w, v ? 1u : 0u);
}
static inline int pst_r_bool(PstR *r, int *v) {
    uint8_t b;
    if (!pst_r_u8(r, &b)) return 0;
    if (v) *v = b ? 1 : 0;
    return 1;
}

/* LE-encode a host POD field: 1/2/4/8-byte scalar, or array of those units. */
static inline int pst_w_pod(PstW *w, const void *src, size_t nbytes, size_t elem) {
    const uint8_t *s = (const uint8_t *)src;
    if (!elem || (nbytes % elem) != 0) return 0;
    if (elem == 1) return pst_w_bytes(w, src, nbytes);
    for (size_t off = 0; off < nbytes; off += elem) {
        if (elem == 2) {
            uint16_t v;
            memcpy(&v, s + off, 2);
            if (!pst_w_u16(w, v)) return 0;
        } else if (elem == 4) {
            uint32_t v;
            memcpy(&v, s + off, 4);
            if (!pst_w_u32(w, v)) return 0;
        } else if (elem == 8) {
            uint64_t v;
            memcpy(&v, s + off, 8);
            if (!pst_w_u64(w, v)) return 0;
        } else {
            return 0;
        }
    }
    return 1;
}

static inline int pst_r_pod(PstR *r, void *dst, size_t nbytes, size_t elem) {
    uint8_t *d = (uint8_t *)dst;
    if (!elem || (nbytes % elem) != 0) return 0;
    if (elem == 1) return pst_r_bytes(r, dst, nbytes);
    for (size_t off = 0; off < nbytes; off += elem) {
        if (elem == 2) {
            uint16_t v;
            if (!pst_r_u16(r, &v)) return 0;
            memcpy(d + off, &v, 2);
        } else if (elem == 4) {
            uint32_t v;
            if (!pst_r_u32(r, &v)) return 0;
            memcpy(d + off, &v, 4);
        } else if (elem == 8) {
            uint64_t v;
            if (!pst_r_u64(r, &v)) return 0;
            memcpy(d + off, &v, 8);
        } else {
            return 0;
        }
    }
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* PSX_PST_WIRE_H */
