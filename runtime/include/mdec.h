#ifndef PSXRECOMP_V4_MDEC_H
#define PSXRECOMP_V4_MDEC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void mdec_init(void);
uint32_t mdec_read(uint32_t addr);
void mdec_write(uint32_t addr, uint32_t value);

void mdec_dma_write_word(uint32_t value);
uint32_t mdec_dma_read_word(void);
int mdec_dma_write_ready(void);
int mdec_dma_read_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_V4_MDEC_H */
