/* dma.h — PS1 DMA controller simulation (Phase 3).
 *
 * 7 DMA channels:
 *   Ch0: MDEC in       0x1F801080
 *   Ch1: MDEC out      0x1F801090
 *   Ch2: GPU           0x1F8010A0
 *   Ch3: CDROM         0x1F8010B0
 *   Ch4: SPU           0x1F8010C0
 *   Ch5: PIO           0x1F8010D0
 *   Ch6: OTC           0x1F8010E0
 *
 * Global:
 *   DPCR: 0x1F8010F0   (DMA control — enable bits per channel)
 *   DICR: 0x1F8010F4   (DMA interrupt control)
 */

#ifndef PSXRECOMP_V4_DMA_H
#define PSXRECOMP_V4_DMA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void     dma_init(void);
uint32_t dma_read(uint32_t addr);
void     dma_write(uint32_t addr, uint32_t val);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_V4_DMA_H */
