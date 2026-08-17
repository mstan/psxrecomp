#ifndef PSX_RAM_H
#define PSX_RAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Retail DRAM is 2 MiB, mirrored 4× across the 8 MiB window. The 8 MB
 * hardware mod maps unique DRAM across registered high ranges (enhancement
 * heaps). Capacity is always 8 MiB so g_psx_ram never moves.
 *
 * Data: unregistered high banks keep 2 MiB mirror fold for loads/stores.
 * Registered pages are unique host DRAM.
 *
 * Code: PCs on still-aliased high pages fold to the low 2 MiB for AOT;
 * unique high pages keep their real PC (may run via dirty-RAM interp). */
#define PSX_RAM_2MB      0x00200000u
#define PSX_RAM_8MB      0x00800000u
#define PSX_RAM_CAPACITY PSX_RAM_8MB
#define PSX_RAM_WINDOW   0x00800000u
/* Default unique range for psx_mod_set_main_ram_8mb: entire high window.
 * Narrowing the top bank for AOT fold corrupted Wipeout enhanced race
 * start (stores folded onto overlay RAM → jalr 0x80800000). */

/* Live installed size / fold mask. 2 MB + 0x1FFFFF until a mod requests 8 MB
 * and memory_init applies it. DMA and CPU paths must use these, not a
 * compile-time 2 MB constant. */
extern uint32_t g_psx_ram_size;
extern uint32_t g_psx_ram_mask;

uint32_t memory_get_ram_bytes(void);
int      psx_ram_8mb_active(void);
/* Drop a previous launch's request before mod activation (rematch / launcher). */
void     psx_ram_reset_size_request(void);

/* Canon physical offset for a main-RAM read (0 .. window). */
uint32_t psx_ram_map_read(uint32_t phys);
/* Canon physical offset for a main-RAM store (folds unregistered high). */
uint32_t psx_ram_map_write(uint32_t phys);
/* Mark [addr, addr+len) unique in 8 MB mode (enhancement heaps). */
void     psx_ram_register_unique(uint32_t addr, uint32_t len);
/* Fold main-RAM code PCs to the low 2 MiB mirror when the high page is still
 * aliased (not registered unique). Unique high code keeps its real PC. */
uint32_t psx_ram_canon_code_addr(uint32_t addr);
/* After boot-state RAM restore: re-apply registration / divergence marks. */
void     psx_ram_resync_high_after_restore(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_RAM_H */
