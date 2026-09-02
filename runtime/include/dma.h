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

#ifndef PSXRECOMP_DMA_H
#define PSXRECOMP_DMA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void     dma_init(void);
uint32_t dma_read(uint32_t addr);
void     dma_write(uint32_t addr, uint32_t val);
void     dma_write_masked(uint32_t addr, uint32_t val, uint32_t mask);
void     dma_advance(uint32_t cycles);
/* Cycle-budgeted precise event slicing: guest CPU cycles until a DELIVERABLE
 * DMA IRQ (bit3 unmasked in i_mask). UINT32_MAX if none. */
uint32_t dma_cycles_to_irq(uint32_t i_mask);
/* Mask-blind scheduler boundary: next DMA word movement or delayed completion,
 * not merely the final IRQ. RAM-producing DMA must become visible at its
 * per-word due cycle even when guest code polls ordinary RAM instead of DICR. */
uint32_t dma_cycles_to_internal_event(void);
/* Strict deliverability form for wait-loop elision: unlike the conservative
 * scheduler bound, ignores channel completions whose DICR IRQ is disabled. */
uint32_t dma_cycles_to_deliverable_irq(uint32_t i_mask);
uint32_t dma_get_dicr(void);
uint32_t dma_get_dpcr(void);
int      dma_cdrom_transfer_active(void);

typedef struct DMAChannelDebugState {
    uint32_t madr;
    uint32_t bcr;
    uint32_t chcr;
    uint32_t active;
    uint32_t remaining_words;
    uint32_t cycles_accum;
} DMAChannelDebugState;

typedef struct DMADebugState {
    uint32_t dpcr;
    uint32_t dicr;
    DMAChannelDebugState channels[7];
} DMADebugState;

typedef struct DMATraceEntry {
    uint64_t seq;
    uint32_t frame;
    uint32_t kind;
    uint32_t channel;
    uint32_t total_words;
    uint32_t addr;
    uint32_t val;
    uint32_t mask;
    uint32_t madr;
    uint32_t bcr;
    uint32_t chcr;
    uint32_t dpcr;
    uint32_t dicr_before;
    uint32_t dicr_after;
    uint32_t i_stat_before;
    uint32_t i_stat_after;
    uint32_t func;
    uint32_t pc;
} DMATraceEntry;

#define DMA_TRACE_CAP (1 << 14)
#define DMA_CDROM_HISTORY_CAP (1 << 13)
#define DMA_CDROM_HISTORY_WORDS 16

typedef struct DMACDROMHistoryEntry {
    uint64_t seq;
    uint32_t frame_start;
    uint32_t frame_end;
    uint32_t start_addr;
    uint32_t final_addr;
    uint32_t requested_words;
    uint32_t moved_words;
    uint32_t bcr;
    uint32_t chcr;
    uint32_t dpcr;
    uint32_t dicr_start;
    uint32_t dicr_end;
    uint32_t i_stat_start;
    uint32_t i_stat_end;
    uint32_t func;
    uint32_t pc;
    int lba;
    int sector_size;
    int sector_read_pos_start;
    int sector_read_pos_end;
    uint8_t mode;
    uint8_t sector_available_start;
    uint8_t sector_available_end;
    uint8_t completed;
    uint8_t first_count;
    uint8_t last_count;
    uint32_t first_words[DMA_CDROM_HISTORY_WORDS];
    uint32_t last_words[DMA_CDROM_HISTORY_WORDS];
} DMACDROMHistoryEntry;

/* GPU (CH2) linked-list / ordering-table walk observability.
 *
 * The walk became cycle-paced rather than drained in one go, which means the
 * guest can now overwrite an ordering table that DMA has not finished reading.
 * These counters make the two failure modes countable instead of visual:
 *   - starts_dropped: the guest asked for a new OT transfer while the previous
 *     walk was still running. start_async_gpu_linked_list() returns early in
 *     that case, so a whole ordering table is never sent to GP0 and nothing
 *     said so. A non-zero value is a runtime bug, not a game quirk.
 *   - cycles_last / cycles_max: how long one walk occupies the guest clock.
 *     Compare against a frame (564480 cycles NTSC): a walk that spans a frame
 *     is a walk the guest is rebuilding underneath.
 */
/* One aborted walk. For ch2 only ONE call site can cancel — the guest store
 * that clears CHCR bit 24 (dma_write_masked) — so `pc` names the guest code
 * that abandoned the ordering table.
 *
 * `polls` is the discriminator between the two possible fixes: it counts the
 * guest's own reads of CHCR(2) during THIS walk. If the guest never read the
 * busy bit before clearing it, it believed the transfer had already finished,
 * and the defect is that the walk is paced too slowly. If it read the bit, saw
 * busy, and cleared anyway, the abort itself is what needs to match hardware. */
typedef struct {
    uint32_t pc;      /* guest PC of the CHCR store that aborted the walk */
    uint32_t chcr;    /* CHCR value after that store */
    uint32_t nodes;   /* ordering-table nodes walked before the abort */
    uint32_t words;   /* words already handed to GP0 */
    uint32_t cycles;  /* guest cycles the walk had been running */
    uint32_t polls;   /* guest reads of CHCR(2) during this walk */
} DMAGpuOtCancel;

#define DMA_GPU_OT_CANCEL_RING 8

typedef struct {
    uint64_t starts;
    uint64_t starts_dropped;
    uint64_t completes;
    uint64_t cancels;
    uint32_t nodes_last;
    uint32_t words_last;
    uint64_t cycles_last;
    uint64_t cycles_max;
    uint8_t  active;
    uint8_t  sync_drain;   /* PSX_GPU_LL_SYNC=1 diagnostic A/B lever */
    uint64_t chcr_reads_total;      /* guest reads of CHCR(2), lifetime */
    uint64_t chcr_reads_in_walk;    /* ... of those, while a walk was active */
    uint32_t initiator_pc;          /* guest store PC that kicked the walk */
    uint32_t cancel_ring_count;     /* entries written (may exceed the ring) */
    DMAGpuOtCancel cancel_ring[DMA_GPU_OT_CANCEL_RING];
} DMAGpuOtStats;

uint64_t dma_debug_get_trace(const DMATraceEntry** out_entries);
void dma_debug_clear_trace(void);
void dma_debug_get_state(DMADebugState* out);
void dma_debug_get_gpu_ot_stats(DMAGpuOtStats* out);
uint64_t dma_debug_get_cdrom_history(const DMACDROMHistoryEntry** out_entries);
void dma_debug_clear_cdrom_history(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_DMA_H */
