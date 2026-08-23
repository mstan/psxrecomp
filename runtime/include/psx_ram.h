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

/* DRAM banks — one per emulated console. Bank 0 is the static array every
 * single-console run uses; dual_machine.c creates bank 1 and swaps the live
 * pointer at a machine switch instead of copying 8 MiB through a snapshot. */
#define PSX_MEMORY_MAX_BANKS 2
int      memory_ram_bank_create(int slot);
int      memory_ram_bank_activate(int slot);
int      memory_ram_bank_live(void);
uint8_t *memory_ram_bank_ptr(int slot);
int      psx_ram_8mb_active(void);
/* Drop a previous launch's request before mod activation (rematch / launcher). */
void     psx_ram_reset_size_request(void);

/* High-bank unique-page bitmap (defined in memory.c). Index 0 = page 512
 * (phys 0x200000). Exposed so the map helpers below inline into the generated
 * game C: psx_cyc.h's load fast path calls psx_ram_map_read on every guest
 * LW/LH and the runtime is built without LTO, so an out-of-line definition
 * cost a real call per load in the hottest loop in the emulator. */
#define PSX_RAM_HIGH_PAGE0    (PSX_RAM_2MB >> 12)                  /* 512  */
#define PSX_RAM_HIGH_PAGES    ((PSX_RAM_8MB - PSX_RAM_2MB) >> 12)  /* 1536 */
#define PSX_RAM_HIGH_BITWORDS ((PSX_RAM_HIGH_PAGES + 31u) / 32u)
extern uint32_t g_psx_ram_high_unique[PSX_RAM_HIGH_BITWORDS];

static inline int psx_ram_high_page_unique(uint32_t page) {
    uint32_t i, bit;
    if (page < PSX_RAM_HIGH_PAGE0 || page >= (PSX_RAM_8MB >> 12))
        return 1;
    i = page - PSX_RAM_HIGH_PAGE0;
    bit = 1u << (i & 31u);
    return (g_psx_ram_high_unique[i >> 5] & bit) != 0;
}

/* Canon physical offset for a main-RAM read (0 .. window). */
static inline uint32_t psx_ram_map_read(uint32_t phys) {
    phys &= 0x1FFFFFFFu;
    if (phys >= PSX_RAM_WINDOW)
        return phys;
    if (g_psx_ram_size <= PSX_RAM_2MB)
        return phys & (PSX_RAM_2MB - 1u);
    if (phys < PSX_RAM_2MB)
        return phys;
    if (psx_ram_high_page_unique(phys >> 12))
        return phys;
    return phys & (PSX_RAM_2MB - 1u);
}

/* Canon physical offset for a main-RAM store (folds unregistered high). */
static inline uint32_t psx_ram_map_write(uint32_t phys) {
    return psx_ram_map_read(phys);
}

/* Fold main-RAM code PCs to the low 2 MiB mirror when the high page is still
 * aliased (not registered unique). Unique high code keeps its real PC.
 * Inline because the generated dispatch table calls this on EVERY dispatch
 * (psx_game_find_entry) and the runtime is built without LTO. */
static inline uint32_t psx_ram_canon_code_addr_inline(uint32_t addr) {
    uint32_t seg = addr & 0xE0000000u;
    uint32_t phys = addr & 0x1FFFFFFFu;
    /* Fold high→low only while the page is still a 2 MiB mirror alias.
     * Registered unique high pages may hold real enhancement code (Wipeout
     * 0x80781xxx); folding those onto low RAM executes the wrong bytes. */
    if (phys < PSX_RAM_WINDOW && phys >= PSX_RAM_2MB &&
        !psx_ram_high_page_unique(phys >> 12))
        phys &= (PSX_RAM_2MB - 1u);
    return seg | phys;
}
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
