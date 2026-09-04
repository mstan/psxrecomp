#ifndef PSXRECOMP_PSX_MEMORY_H
#define PSXRECOMP_PSX_MEMORY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One compile-time main-RAM geometry contract for every runtime subsystem.
 * A target selects retail 2 MiB mirroring or unique 8 MiB decoding. */
#define PSX_MAIN_RAM_RETAIL_BYTES   0x00200000u
#define PSX_MAIN_RAM_EXPANDED_BYTES 0x00800000u

#ifndef PSX_MAIN_RAM_BYTES
#define PSX_MAIN_RAM_BYTES PSX_MAIN_RAM_RETAIL_BYTES
#endif

#if PSX_MAIN_RAM_BYTES != PSX_MAIN_RAM_RETAIL_BYTES && \
    PSX_MAIN_RAM_BYTES != PSX_MAIN_RAM_EXPANDED_BYTES
#error "PSX_MAIN_RAM_BYTES must select 2 MiB or 8 MiB"
#endif

#define PSX_MAIN_RAM_MASK (PSX_MAIN_RAM_BYTES - 1u)
#define PSX_MAIN_RAM_WORD_MASK (PSX_MAIN_RAM_MASK & ~3u)

/* Strip KUSEG/KSEG0/KSEG1 and canonicalize within the 8 MiB DRAM decode
 * window. Retail targets fold all four aliases; expanded targets preserve all
 * 23 address bits. */
static inline uint32_t psx_ram_canonical_offset(uint32_t address) {
    return (address & 0x1FFFFFFFu) & PSX_MAIN_RAM_MASK;
}

static inline int psx_ram_resolve(uint32_t address, uint32_t width,
                                  uint32_t *offset) {
    const uint32_t phys = address & 0x1FFFFFFFu;
    uint32_t off;
    if (phys >= PSX_MAIN_RAM_EXPANDED_BYTES || width == 0u) return 0;
    off = phys & PSX_MAIN_RAM_MASK;
    if (width > PSX_MAIN_RAM_BYTES - off) return 0;
    if (offset) *offset = off;
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif
