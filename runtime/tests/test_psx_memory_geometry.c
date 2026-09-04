#include "psx_memory.h"

#include <stdio.h>

static int failures;

static void check(int condition, const char *name) {
    if (condition) {
        printf("PASS  %s\n", name);
    } else {
        fprintf(stderr, "FAIL  %s\n", name);
        failures++;
    }
}

int main(void) {
    uint32_t off = 0xFFFFFFFFu;

    check(psx_ram_resolve(0x80000000u, 4u, &off) && off == 0u,
          "KSEG0 aliases physical RAM");
    check(psx_ram_resolve(0xA0000100u, 4u, &off) && off == 0x100u,
          "KSEG1 aliases physical RAM");
    check(!psx_ram_resolve(0x00800000u, 4u, &off),
          "first byte beyond DRAM decode is rejected");
    check(!psx_ram_resolve(0x1F801000u, 4u, &off),
          "MMIO is never mistaken for DRAM");
    check(!psx_ram_resolve(PSX_MAIN_RAM_BYTES - 2u, 4u, &off),
          "cross-geometry word is rejected");

#if PSX_MAIN_RAM_BYTES == PSX_MAIN_RAM_RETAIL_BYTES
    check(psx_ram_canonical_offset(0x80612340u) == 0x00012340u,
          "retail canonicalization folds KSEG0 mirrors");
    check(psx_ram_resolve(0x00212340u, 4u, &off) && off == 0x00012340u,
          "retail geometry folds the second mirror");
    check(psx_ram_resolve(0x807FFFFCu, 4u, &off) && off == 0x001FFFFCu,
          "retail geometry folds the fourth mirror top");
#elif PSX_MAIN_RAM_BYTES == PSX_MAIN_RAM_EXPANDED_BYTES
    check(psx_ram_canonical_offset(0x80612340u) == 0x00612340u,
          "expanded canonicalization preserves all decoded address bits");
    check(psx_ram_resolve(0x00212340u, 4u, &off) && off == 0x00212340u,
          "expanded geometry uniquely decodes the second bank");
    check(psx_ram_resolve(0x807FFFFCu, 4u, &off) && off == 0x007FFFFCu,
          "expanded geometry uniquely decodes the eighth MiB top");
#endif

    return failures ? 1 : 0;
}
