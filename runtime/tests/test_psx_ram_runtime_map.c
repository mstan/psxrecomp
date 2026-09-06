#include "psx_ram.h"

#include <stdint.h>
#include <stdio.h>

uint32_t g_psx_ram_size = PSX_RAM_2MB;
uint32_t g_psx_ram_mask = PSX_RAM_2MB - 1u;
uint32_t g_psx_ram_high_unique[PSX_RAM_HIGH_BITWORDS];

static int check(int cond, const char *label) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", label);
        return 0;
    }
    return 1;
}

int main(void) {
    const uint32_t high = 0x00201000u;
    const uint32_t high_page = high >> 12;
    uint32_t i = high_page - PSX_RAM_HIGH_PAGE0;
    int ok = 1;

    ok &= check(psx_ram_map_read(high) == 0x00001000u,
                "retail maps high mirror to low RAM");
    ok &= check(psx_ram_canon_code_addr_inline(0x80201000u) == 0x80001000u,
                "retail folds high code PC to low mirror");

    g_psx_ram_size = PSX_RAM_8MB;
    g_psx_ram_mask = PSX_RAM_8MB - 1u;
    ok &= check(psx_ram_map_read(high) == 0x00001000u,
                "unregistered 8 MB high page still mirrors low RAM");

    g_psx_ram_high_unique[i >> 5] |= 1u << (i & 31u);
    ok &= check(psx_ram_map_read(high) == high,
                "registered 8 MB high page maps uniquely");
    ok &= check(psx_ram_canon_code_addr_inline(0x80201000u) == 0x80201000u,
                "registered high code PC remains unique");

    return ok ? 0 : 1;
}
