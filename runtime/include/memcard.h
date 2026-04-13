#ifndef PSXRECOMP_V4_MEMCARD_H
#define PSXRECOMP_V4_MEMCARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEMCARD_SIZE         (128 * 1024)   /* 128KB per card */
#define MEMCARD_SECTOR_SIZE  128
#define MEMCARD_SECTORS      1024

/* Initialize memcard subsystem. dir = directory for .mcd files (can be NULL). */
void memcard_init(const char* dir);

/* Read/write 128-byte sectors */
int memcard_read_sector(int card, int sector, uint8_t* buf);
int memcard_write_sector(int card, int sector, const uint8_t* buf);

/* Flush pending writes to disk */
void memcard_flush(int card);

/* Check if card is present */
int memcard_is_present(int card);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_V4_MEMCARD_H */
