#ifndef RNET_SHA1_H
#define RNET_SHA1_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void rnet_sha1(const uint8_t *data, size_t len, uint8_t out[20]);

#ifdef __cplusplus
}
#endif

#endif
