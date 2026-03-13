#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open a PS1 BIN/CUE image.  Pass path to the .cue file.
 * Returns 1 on success, 0 on failure. */
int psx_cdrom_init(const char* cue_path);

/* Read one 2048-byte user-data sector at the given LBA into buffer.
 * Returns 1 on success, 0 on failure. */
int psx_cdrom_read_sector(uint32_t lba, uint8_t* buffer);

#ifdef __cplusplus
}
#endif
