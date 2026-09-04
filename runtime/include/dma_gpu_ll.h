#ifndef PSXRECOMP_DMA_GPU_LL_H
#define PSXRECOMP_DMA_GPU_LL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DMA_GPU_LL_PHASE_IDLE = 0,
    DMA_GPU_LL_PHASE_HEADER = 1,
    DMA_GPU_LL_PHASE_SETUP = 2,
    DMA_GPU_LL_PHASE_PAYLOAD = 3
};

typedef struct {
    uint8_t active;
    uint8_t phase;
    uint8_t emit_node;
    uint8_t hit_limit;
    uint32_t start_addr;
    uint32_t current_addr;
    uint32_t next_addr;
    uint32_t word_count;
    uint32_t payload_index;
    uint32_t cycles_remaining;
    uint32_t nodes_processed;
    uint32_t max_nodes;
    uint32_t total_words;
    uint32_t empty_rank;
} DMAGPULinkedList;

typedef struct {
    uint32_t (*resolve_address)(void *opaque, uint32_t address);
    uint32_t (*read_word)(void *opaque, uint32_t address);
    void (*observe_header)(void *opaque, uint32_t address, uint32_t header);
    int (*begin_node)(void *opaque, uint32_t address, uint32_t word_count);
    void (*emit_word)(void *opaque, uint32_t address, uint32_t word);
    void (*complete)(void *opaque, int hit_limit);
} DMAGPULinkedListOps;

void dma_gpu_ll_start(DMAGPULinkedList *state, uint32_t start_addr,
                      uint32_t max_nodes);
void dma_gpu_ll_cancel(DMAGPULinkedList *state);
uint32_t dma_gpu_ll_cycles_to_event(const DMAGPULinkedList *state);
void dma_gpu_ll_advance(DMAGPULinkedList *state, uint32_t cycles,
                        const DMAGPULinkedListOps *ops, void *opaque);

#ifdef __cplusplus
}
#endif

#endif
