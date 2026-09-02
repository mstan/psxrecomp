#include "dma_gpu_ll.h"

#include <string.h>

/* PSX DMA2 linked-list timing used by the clean-room state machine. The header
 * read costs eight guest clocks. A non-empty node then has five setup clocks,
 * followed by one clock per payload word. Keeping these as separate events is
 * important: the guest CPU can change a packet after it starts DMA and before
 * DMA reaches that packet. */
#define DMA_GPU_LL_HEADER_CYCLES 8u
#define DMA_GPU_LL_SETUP_CYCLES  5u

static uint32_t resolve_address(const DMAGPULinkedListOps *ops, void *opaque,
                                uint32_t address) {
    return ops->resolve_address ? ops->resolve_address(opaque, address) : address;
}

static void finish(DMAGPULinkedList *state,
                   const DMAGPULinkedListOps *ops, void *opaque) {
    int hit_limit = state->hit_limit != 0;
    state->active = 0;
    state->phase = DMA_GPU_LL_PHASE_IDLE;
    state->cycles_remaining = 0;
    if (ops->complete) ops->complete(opaque, hit_limit);
}

void dma_gpu_ll_start(DMAGPULinkedList *state, uint32_t start_addr,
                      uint32_t max_nodes) {
    memset(state, 0, sizeof(*state));
    state->active = 1;
    state->phase = DMA_GPU_LL_PHASE_HEADER;
    state->start_addr = start_addr;
    state->current_addr = start_addr;
    state->cycles_remaining = DMA_GPU_LL_HEADER_CYCLES;
    state->max_nodes = max_nodes;
    state->empty_rank = UINT32_MAX;
}

void dma_gpu_ll_cancel(DMAGPULinkedList *state) {
    memset(state, 0, sizeof(*state));
}

uint32_t dma_gpu_ll_cycles_to_event(const DMAGPULinkedList *state) {
    if (!state->active) return UINT32_MAX;
    return state->cycles_remaining ? state->cycles_remaining : 1u;
}

void dma_gpu_ll_advance(DMAGPULinkedList *state, uint32_t cycles,
                        const DMAGPULinkedListOps *ops, void *opaque) {
    if (!state->active || cycles == 0 || !ops || !ops->read_word) return;
    if (cycles < state->cycles_remaining) {
        state->cycles_remaining -= cycles;
        return;
    }

    /* The runtime's deadline scheduler lands on each advertised boundary. If a
     * caller reaches us late, process one boundary only. This preserves the
     * same one-event-per-service contract as SIO, CD-ROM, and MDEC. */
    state->cycles_remaining = 0;

    if (state->phase == DMA_GPU_LL_PHASE_HEADER) {
        if (state->nodes_processed >= state->max_nodes) {
            state->hit_limit = 1;
            finish(state, ops, opaque);
            return;
        }

        state->current_addr = resolve_address(ops, opaque, state->current_addr);
        uint32_t header = ops->read_word(opaque, state->current_addr);
        state->word_count = header >> 24;
        state->next_addr = header & 0x00FFFFFFu;
        state->nodes_processed++;
        state->total_words++;
        if (state->word_count == 0)
            state->empty_rank = state->empty_rank == UINT32_MAX
                ? 0u : state->empty_rank + 1u;
        if (state->word_count != 0) {
            state->emit_node = 1u;
            state->phase = DMA_GPU_LL_PHASE_PAYLOAD;
            state->cycles_remaining = DMA_GPU_LL_SETUP_CYCLES;
            return;
        }

        state->emit_node = ops->begin_node
            ? (uint8_t)(ops->begin_node(opaque, state->current_addr, 0u) != 0)
            : 1u;

        if (state->next_addr == 0x00FFFFFFu) {
            finish(state, ops, opaque);
            return;
        }
        state->current_addr = resolve_address(ops, opaque, state->next_addr);
        state->cycles_remaining = DMA_GPU_LL_HEADER_CYCLES;
        return;
    }

    if (state->phase == DMA_GPU_LL_PHASE_PAYLOAD) {
        uint32_t word_addr = resolve_address(
            ops, opaque, state->current_addr + 4u);
        state->emit_node = ops->begin_node
            ? (uint8_t)(ops->begin_node(opaque, state->current_addr,
                                        state->word_count) != 0)
            : 1u;
        if (state->emit_node) {
            for (uint32_t i = 0; i < state->word_count; i++) {
                uint32_t word = ops->read_word(opaque, word_addr);
                if (ops->emit_word) ops->emit_word(opaque, word_addr, word);
                word_addr = resolve_address(ops, opaque, word_addr + 4u);
            }
        }
        state->total_words += state->word_count;

        if (state->next_addr == 0x00FFFFFFu) {
            state->phase = DMA_GPU_LL_PHASE_COMPLETE;
            state->cycles_remaining = state->word_count;
            return;
        }
        state->phase = DMA_GPU_LL_PHASE_HEADER;
        state->current_addr = resolve_address(ops, opaque, state->next_addr);
        state->cycles_remaining = state->word_count + DMA_GPU_LL_HEADER_CYCLES;
        return;
    }

    if (state->phase == DMA_GPU_LL_PHASE_COMPLETE) {
        finish(state, ops, opaque);
        return;
    }

    state->hit_limit = 1;
    finish(state, ops, opaque);
}
