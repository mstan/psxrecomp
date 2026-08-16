#ifndef PSX_RAM_H
#define PSX_RAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Retail DRAM is 2 MiB, mirrored 4× across the 8 MiB window. The 8 MB
 * hardware mod (DuckStation-style) maps unique DRAM across that window
 * instead. Capacity is always 8 MiB so g_psx_ram never moves. */
#define PSX_RAM_2MB      0x00200000u
#define PSX_RAM_8MB      0x00800000u
#define PSX_RAM_CAPACITY PSX_RAM_8MB
#define PSX_RAM_WINDOW   0x00800000u

/* Live installed size / fold mask. 2 MB + 0x1FFFFF until a mod requests 8 MB
 * and memory_init applies it. DMA and CPU paths must use these, not a
 * compile-time 2 MB constant. */
extern uint32_t g_psx_ram_size;
extern uint32_t g_psx_ram_mask;

uint32_t memory_get_ram_bytes(void);
int      psx_ram_8mb_active(void);
/* Drop a previous launch's request before mod activation (rematch / launcher). */
void     psx_ram_reset_size_request(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_RAM_H */
