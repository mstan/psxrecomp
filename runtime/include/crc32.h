#pragma once
#include <stdint.h>
#include <stddef.h>

/* Compute CRC32 (IEEE 802.3) over a byte buffer. */
uint32_t crc32_compute(const uint8_t *data, size_t len);
