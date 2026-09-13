#ifndef PSX_RAM_H
#define PSX_RAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Retail DRAM is 2 MiB, mirrored 4x across the 8 MiB decode window. The
 * opt-in 8 MB RAM enhancement keeps an 8 MiB host backing and switches the
 * live map to unique high pages only after a trusted package requests it.
 */
#define PSX_RAM_2MB      0x00200000u
#define PSX_RAM_8MB      0x00800000u
#define PSX_RAM_CAPACITY PSX_RAM_8MB
#define PSX_RAM_WINDOW   0x00800000u

extern uint32_t g_psx_ram_size;
extern uint32_t g_psx_ram_mask;

uint32_t memory_get_ram_bytes(void);
int      psx_ram_8mb_active(void);
void     psx_ram_reset_size_request(void);

#define PSX_RAM_HIGH_PAGE0    (PSX_RAM_2MB >> 12)
#define PSX_RAM_HIGH_PAGES    ((PSX_RAM_8MB - PSX_RAM_2MB) >> 12)
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

static inline uint32_t psx_ram_map_write(uint32_t phys) {
    return psx_ram_map_read(phys);
}

static inline uint32_t psx_ram_canon_code_addr_inline(uint32_t addr) {
    uint32_t seg = addr & 0xE0000000u;
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < PSX_RAM_WINDOW && phys >= PSX_RAM_2MB &&
        !psx_ram_high_page_unique(phys >> 12))
        phys &= (PSX_RAM_2MB - 1u);
    return seg | phys;
}

void     psx_ram_register_unique(uint32_t addr, uint32_t len);
uint32_t psx_ram_canon_code_addr(uint32_t addr);
void     psx_ram_resync_high_after_restore(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_RAM_H */
