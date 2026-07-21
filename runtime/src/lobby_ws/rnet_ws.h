#ifndef RNET_WS_H
#define RNET_WS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal RFC6455 helpers (text frames, no extensions). */

int rnet_ws_accept_key(const char *client_key, char out_b64[32]);

/* Returns bytes written to out, or -1. */
int rnet_ws_write_text(int fd, const char *text, int client_mask);

/*
 * Read one text frame into buf (NUL-terminated). Returns payload length, 0 if
 * would-block/incomplete, -1 on close/error. *need_more stays set when partial.
 */
int rnet_ws_read_text(int fd, char *buf, size_t cap, int *closed);

#ifdef __cplusplus
}
#endif

#endif
