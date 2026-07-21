#include "rnet_ws.h"
#include "rnet_sha1.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#endif

static const char *B64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(const uint8_t *in, size_t n, char *out)
{
    size_t i = 0, o = 0;
    while (i < n) {
        uint32_t v = (uint32_t)in[i++] << 16;
        if (i < n) {
            v |= (uint32_t)in[i++] << 8;
        }
        if (i < n) {
            v |= (uint32_t)in[i++];
        }
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = (i > n + (n % 3 == 1 ? 1 : 0) && (n % 3 == 1)) ? '=' : B64[(v >> 6) & 63];
        out[o++] = (n % 3 == 1) ? '=' : ((n % 3 == 2 && i >= n) ? '=' : B64[v & 63]);
    }
    /* Fix padding properly */
    {
        size_t full = (n / 3) * 3;
        size_t rem = n - full;
        o = 0;
        for (i = 0; i < full; i += 3) {
            uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
            out[o++] = B64[(v >> 18) & 63];
            out[o++] = B64[(v >> 12) & 63];
            out[o++] = B64[(v >> 6) & 63];
            out[o++] = B64[v & 63];
        }
        if (rem == 1) {
            uint32_t v = (uint32_t)in[full] << 16;
            out[o++] = B64[(v >> 18) & 63];
            out[o++] = B64[(v >> 12) & 63];
            out[o++] = '=';
            out[o++] = '=';
        } else if (rem == 2) {
            uint32_t v = ((uint32_t)in[full] << 16) | ((uint32_t)in[full + 1] << 8);
            out[o++] = B64[(v >> 18) & 63];
            out[o++] = B64[(v >> 12) & 63];
            out[o++] = B64[(v >> 6) & 63];
            out[o++] = '=';
        }
        out[o] = '\0';
    }
}

int rnet_ws_accept_key(const char *client_key, char out_b64[32])
{
    char concat[128];
    uint8_t digest[20];
    int n;
    if (!client_key || !out_b64) {
        return -1;
    }
    n = snprintf(concat, sizeof(concat), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", client_key);
    if (n <= 0 || (size_t)n >= sizeof(concat)) {
        return -1;
    }
    rnet_sha1((const uint8_t *)concat, (size_t)n, digest);
    b64_encode(digest, 20, out_b64);
    return 0;
}

static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t sent = 0;
    while (sent < len) {
#if defined(_WIN32)
        int n = send(fd, p + sent, (int)(len - sent), 0);
#else
        ssize_t n = send(fd, p + sent, len - sent, 0);
#endif
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

int rnet_ws_write_text(int fd, const char *text, int client_mask)
{
    size_t len;
    uint8_t hdr[14];
    size_t hlen = 0;
    uint8_t mask[4];
    uint8_t *payload = NULL;
    int rc;

    if (!text) {
        return -1;
    }
    len = strlen(text);
    hdr[0] = 0x81; /* FIN + text */
    if (len < 126) {
        hdr[1] = (uint8_t)((client_mask ? 0x80 : 0) | len);
        hlen = 2;
    } else if (len < 65536) {
        hdr[1] = (uint8_t)((client_mask ? 0x80 : 0) | 126);
        hdr[2] = (uint8_t)((len >> 8) & 0xff);
        hdr[3] = (uint8_t)(len & 0xff);
        hlen = 4;
    } else {
        return -1;
    }
    if (client_mask) {
        uint32_t r = (uint32_t)rand();
        mask[0] = (uint8_t)(r);
        mask[1] = (uint8_t)(r >> 8);
        mask[2] = (uint8_t)(r >> 16);
        mask[3] = (uint8_t)(r >> 24);
        memcpy(hdr + hlen, mask, 4);
        hlen += 4;
        payload = (uint8_t *)malloc(len);
        if (!payload) {
            return -1;
        }
        for (size_t i = 0; i < len; ++i) {
            payload[i] = (uint8_t)text[i] ^ mask[i & 3];
        }
    }
    if (send_all(fd, hdr, hlen) != 0) {
        free(payload);
        return -1;
    }
    rc = send_all(fd, client_mask ? (const void *)payload : (const void *)text, len);
    free(payload);
    return rc;
}

int rnet_ws_read_text(int fd, char *buf, size_t cap, int *closed)
{
    uint8_t h0, h1;
    uint8_t hdr[2];
    size_t plen = 0;
    uint8_t mask[4];
    int masked;
    size_t i;
#if defined(_WIN32)
    int n;
#else
    ssize_t n;
#endif

    if (closed) {
        *closed = 0;
    }
    n = recv(fd, (char *)hdr, 2, 0);
    if (n == 0) {
        if (closed) {
            *closed = 1;
        }
        return -1;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return -1;
    }
    if (n < 2) {
        return 0;
    }
    h0 = hdr[0];
    h1 = hdr[1];
    if ((h0 & 0x0f) == 0x8) {
        if (closed) {
            *closed = 1;
        }
        return -1;
    }
    if ((h0 & 0x0f) == 0x9) { /* ping -> pong */
        /* ignore payload for keepalive simplicity */
        return 0;
    }
    if ((h0 & 0x0f) != 0x1) {
        return -1;
    }
    masked = (h1 & 0x80) != 0;
    plen = (size_t)(h1 & 0x7f);
    if (plen == 126) {
        uint8_t ext[2];
        if (recv(fd, (char *)ext, 2, MSG_WAITALL) != 2) {
            return -1;
        }
        plen = ((size_t)ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        return -1;
    }
    if (masked) {
        if (recv(fd, (char *)mask, 4, MSG_WAITALL) != 4) {
            return -1;
        }
    }
    if (plen + 1 > cap) {
        return -1;
    }
    if (plen > 0) {
        size_t got = 0;
        while (got < plen) {
            n = recv(fd, buf + got, plen - got, 0);
            if (n <= 0) {
                return -1;
            }
            got += (size_t)n;
        }
    }
    if (masked) {
        for (i = 0; i < plen; ++i) {
            buf[i] = (char)((uint8_t)buf[i] ^ mask[i & 3]);
        }
    }
    buf[plen] = '\0';
    return (int)plen;
}
