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
    resolve, read_word, begin_node, emit_word, complete
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

    /* Node 0 at 0x00 points to node 1 at 0x20. Node 1 terminates. */
    ram[0x00 / 4] = 0x01000020u;
    ram[0x04 / 4] = 0x11111111u;
    ram[0x20 / 4] = 0x01FFFFFFu;
    ram[0x24 / 4] = 0x22222222u;

    dma_gpu_ll_start(&state, 0x00u, 8u);
    CHECK(dma_gpu_ll_cycles_to_event(&state) == 8u);
    dma_gpu_ll_advance(&state, 7u, &ops, NULL);
    CHECK(emitted_count == 0u);

    /* Header becomes visible at cycle 8; payload waits for setup. */
    dma_gpu_ll_advance(&state, 1u, &ops, NULL);
    CHECK(emitted_count == 0u);
    CHECK(dma_gpu_ll_cycles_to_event(&state) == 5u);
    dma_gpu_ll_advance(&state, 5u, &ops, NULL);
    CHECK(emitted_count == 1u && emitted[0] == 0x11111111u);

    /* This is the regression: the CPU changes the later packet after DMA has
     * started. DMA must read the new value when it reaches that packet. */
    ram[0x24 / 4] = 0xA5A5A5A5u;
    CHECK(dma_gpu_ll_cycles_to_event(&state) == 9u);
    dma_gpu_ll_advance(&state, 9u, &ops, NULL);
    CHECK(emitted_count == 1u);
    dma_gpu_ll_advance(&state, 5u, &ops, NULL);
    CHECK(emitted_count == 2u && emitted[1] == 0xA5A5A5A5u);
    CHECK(completed == 0u);
    dma_gpu_ll_advance(&state, 1u, &ops, NULL);
    CHECK(completed == 1u && hit_limit == 0u && !state.active);

    /* A malformed cycle is bounded and reports the safety stop. */
    completed = hit_limit = 0u;
    ram[0x00 / 4] = 0x00000000u;
    dma_gpu_ll_start(&state, 0x00u, 1u);
    dma_gpu_ll_advance(&state, 8u, &ops, NULL);
    dma_gpu_ll_advance(&state, 8u, &ops, NULL);
    CHECK(completed == 1u && hit_limit == 1u && !state.active);

    puts("dma_gpu_linked_list_timing_test: PASS");
    return 0;
}
