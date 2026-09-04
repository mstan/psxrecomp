#include "dma_gpu_ll.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t ram[64];
static uint32_t emitted[16];
static uint32_t emitted_count;
static uint32_t completed;
static uint32_t hit_limit;

static uint32_t resolve(void *opaque, uint32_t address) {
    (void)opaque;
    return address & 0xFCu;
}

static uint32_t read_word(void *opaque, uint32_t address) {
    (void)opaque;
    return ram[(address & 0xFCu) >> 2];
}

static int begin_node(void *opaque, uint32_t address, uint32_t word_count) {
    (void)opaque;
    (void)address;
    (void)word_count;
    return 1;
}

static void emit_word(void *opaque, uint32_t address, uint32_t word) {
    (void)opaque;
    (void)address;
    emitted[emitted_count++] = word;
}

static void complete(void *opaque, int limited) {
    (void)opaque;
    completed++;
    hit_limit = (uint32_t)limited;
}

static const DMAGPULinkedListOps ops = {
    resolve, read_word, NULL, begin_node, emit_word, complete
};

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

int main(void) {
    DMAGPULinkedList state;
    memset(ram, 0, sizeof(ram));

    /* A multi-word node must fetch one live RAM word at each DMA word
     * boundary. In particular, a CPU rewrite after word 0 transfers must be
     * visible when DMA reaches word 1. */
    ram[0x40 / 4] = 0x03FFFFFFu;
    ram[0x44 / 4] = 0x11111111u;
    ram[0x48 / 4] = 0x22222222u;
    ram[0x4C / 4] = 0x33333333u;

    /* A late service call must consume every elapsed boundary. */
    dma_gpu_ll_start(&state, 0x40u, 8u);
    dma_gpu_ll_advance(&state, 16u, &ops, NULL);
    CHECK(emitted_count == 3u);
    CHECK(completed == 1u && hit_limit == 0u && !state.active);

    memset(emitted, 0, sizeof(emitted));
    emitted_count = completed = hit_limit = 0u;

    dma_gpu_ll_start(&state, 0x40u, 8u);
    dma_gpu_ll_advance(&state, 1u, &ops, NULL);
    CHECK(emitted_count == 0u);
    CHECK(dma_gpu_ll_cycles_to_event(&state) == 1u);
    dma_gpu_ll_advance(&state, 1u, &ops, NULL);
    CHECK(emitted_count == 1u && emitted[0] == 0x11111111u);
    ram[0x48 / 4] = 0xA5A5A5A5u;
    dma_gpu_ll_advance(&state, 1u, &ops, NULL);
    CHECK(emitted_count == 2u && emitted[1] == 0xA5A5A5A5u);
    dma_gpu_ll_advance(&state, 1u, &ops, NULL);
    CHECK(emitted_count == 3u && emitted[2] == 0x33333333u);
    CHECK(completed == 1u && hit_limit == 0u && !state.active);

    memset(emitted, 0, sizeof(emitted));
    emitted_count = completed = hit_limit = 0u;

    /* Node 0 at 0x00 points to node 1 at 0x20. Node 1 terminates. */
    ram[0x00 / 4] = 0x01000020u;
    ram[0x04 / 4] = 0x11111111u;
    ram[0x20 / 4] = 0x01FFFFFFu;
    ram[0x24 / 4] = 0x22222222u;

    dma_gpu_ll_start(&state, 0x00u, 8u);
    /* Each header and payload word costs one clock. Setup costs nothing. */
    CHECK(dma_gpu_ll_cycles_to_event(&state) == 1u);
    CHECK(emitted_count == 0u);

    /* Setup occurs with the header service. The payload keeps its own event. */
    dma_gpu_ll_advance(&state, 1u, &ops, NULL);
    CHECK(emitted_count == 0u);
    CHECK(dma_gpu_ll_cycles_to_event(&state) == 1u);
    dma_gpu_ll_advance(&state, 1u, &ops, NULL);
    CHECK(emitted_count == 1u && emitted[0] == 0x11111111u);

    /* This is what the sliced walker exists for, and the cost change must not
     * weaken it: the CPU changes a later packet after DMA has started, and DMA
     * must read the NEW value when it reaches that packet. */
    ram[0x24 / 4] = 0xA5A5A5A5u;
    CHECK(dma_gpu_ll_cycles_to_event(&state) == 1u);
    dma_gpu_ll_advance(&state, 1u, &ops, NULL);
    CHECK(emitted_count == 1u);
    CHECK(completed == 0u);
    dma_gpu_ll_advance(&state, 1u, &ops, NULL);
    CHECK(emitted_count == 2u && emitted[1] == 0xA5A5A5A5u);
    CHECK(completed == 1u && hit_limit == 0u && !state.active);

    /* An empty ordering table costs one header clock per node. */
    {
        const uint32_t NODES = 12u;
        memset(ram, 0, sizeof(ram));
        for (uint32_t i = 0; i < NODES - 1u; i++)
            ram[i] = ((i + 1u) * 4u);              /* empty node -> next */
        ram[NODES - 1u] = 0x00FFFFFFu;             /* empty terminator */

        completed = hit_limit = emitted_count = 0u;
        dma_gpu_ll_start(&state, 0x00u, 64u);
        uint32_t spent = 0u;
        while (state.active && spent < 1000u) {
            uint32_t d = dma_gpu_ll_cycles_to_event(&state);
            dma_gpu_ll_advance(&state, d, &ops, NULL);
            spent += d;
        }
        CHECK(completed == 1u && hit_limit == 0u);
        CHECK(state.nodes_processed == NODES);
        CHECK(state.total_words == NODES);         /* header words only */
        CHECK(spent == NODES);                     /* nodes + 0 payload words */
    }

    /* A malformed cycle is bounded and reports the safety stop. */
    memset(ram, 0, sizeof(ram));
    completed = hit_limit = 0u;
    ram[0x00 / 4] = 0x00000000u;
    dma_gpu_ll_start(&state, 0x00u, 1u);
    dma_gpu_ll_advance(&state, 1u, &ops, NULL);
    dma_gpu_ll_advance(&state, 1u, &ops, NULL);
    CHECK(completed == 1u && hit_limit == 1u && !state.active);

    puts("dma_gpu_linked_list_timing_test: PASS");
    return 0;
}
