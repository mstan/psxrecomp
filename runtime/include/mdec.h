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

typedef struct MDECDebugState {
    uint32_t command;
    uint32_t expected_halfwords;
    uint32_t input_count;
    uint32_t output_size;
    uint32_t output_pos;
    uint32_t output_depth;
    uint32_t output_signed;
    uint32_t output_bit15;
    uint32_t busy;
    uint32_t input_full;
    uint32_t enable_dma_in;
    uint32_t enable_dma_out;
    uint32_t last_status;
    uint32_t decode_macroblocks;
    uint32_t decode_blocks;
    uint32_t decode_stop_reason;
    uint32_t decode_input_pos;
    uint32_t decode_input_end;
    uint32_t dma_in_words;
    uint32_t dma_out_words;
    uint32_t dma_read_underflows;
} MDECDebugState;

typedef struct MDECDebugEvent {
    uint64_t seq;
    uint32_t frame;
    uint32_t kind;
    uint32_t value;
    uint32_t command;
    uint32_t input_count;
    uint32_t expected_halfwords;
    uint32_t output_size;
    uint32_t output_pos;
    uint32_t macroblocks;
    uint32_t blocks;
    uint32_t stop_reason;
    uint32_t underruns;
} MDECDebugEvent;

void mdec_debug_get_state(MDECDebugState *out);
uint64_t mdec_debug_get_event_total(void);
uint32_t mdec_debug_copy_events(uint64_t seq_lo, uint64_t seq_hi,
                                MDECDebugEvent *out, uint32_t max_count);
void mdec_debug_clear(void);
void mdec_debug_dma_in_start(uint32_t addr, uint32_t words);
void mdec_debug_dma_in_end(uint32_t addr, uint32_t words);
void mdec_debug_dma_out_start(uint32_t addr, uint32_t words);
void mdec_debug_dma_out_end(uint32_t addr, uint32_t words);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_V4_MDEC_H */
