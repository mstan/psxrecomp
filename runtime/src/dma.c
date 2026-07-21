/* dma.c — PS1 DMA controller simulation (Phase 3).
 *
 * Implements all 7 channel register reads/writes, DPCR, DICR,
 * and transfer execution for:
 *   - Ch2 (GPU): block mode and linked-list mode
 *   - Ch6 (OTC): ordering table clear
 *
 * Most channels execute synchronously when CHCR start bit is written. MDEC
 * request-mode transfers are advanced from the guest cycle clock so games
 * which synchronize video decode through DMA busy/request state see realistic
 * backpressure.
 * Reference: nocash PSX specs, DuckStation src/core/dma.cpp
 */

#include "dma.h"
#include "cdrom.h"
#include "crash_trace.h"
#include "dirty_ram_interp.h"
#include "gpu.h"
#include "mdec.h"
#include "overlay_capture.h"
#include "spu.h"
#include "audio_trace.h"
#include "event_ring.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Memory access — defined in memory.c */
extern uint32_t psx_read_word(uint32_t addr);
extern void     psx_write_word(uint32_t addr, uint32_t val);

/* Interrupt status — defined in memory.c */
extern uint32_t i_stat;
/* Central IRQ-raise choke point (interrupts.c) — also records the device ring. */
extern void psx_irq_raise(uint32_t bit, uint32_t detail);
extern uint32_t g_debug_current_func_addr;
extern uint32_t g_debug_last_store_pc;
extern uint64_t s_frame_count;

/* ---- Per-channel registers ---- */

typedef struct {
    uint32_t madr;  /* +0x00: Memory address */
    uint32_t bcr;   /* +0x04: Block control */
    uint32_t chcr;  /* +0x08: Channel control */
} DMAChannel;

static DMAChannel channels[7];

typedef struct {
    uint8_t active;
    uint8_t debug_started;
    uint32_t total_words;
    uint32_t remaining_words;
    uint32_t cycles_accum;
    uint32_t start_addr;   /* madr at transfer start (CD overlay capture) */
} DMAAsyncChannel;

static DMAAsyncChannel mdec_async[2];
static DMAAsyncChannel cdrom_async;

/* ---- CD DMA transfer log ---- */
/* Every forward CH3 DMA that lands below 0x1C0000 (game data region) records
 * (setloc_lba, dest_addr, size). Transfers to 0x1C0000+ are FMV/streaming
 * buffers and are excluded to keep the ring focused on overlay loads.
 * Surfaced via the cd_read_log TCP command; pairs with overlay_dump to map
 * overlay regions back to disc positions for extract_overlays.py. */
#define CD_DMA_LOG_CAP 65536
typedef struct { int lba; uint32_t dest; uint32_t size; } CdDmaEntry;
static CdDmaEntry cd_dma_log[CD_DMA_LOG_CAP];
static uint32_t   cd_dma_log_head  = 0;
static uint32_t   cd_dma_log_total = 0;

static void cd_dma_log_push(int lba, uint32_t dest, uint32_t size) {
    cd_dma_log[cd_dma_log_head % CD_DMA_LOG_CAP].lba  = lba;
    cd_dma_log[cd_dma_log_head % CD_DMA_LOG_CAP].dest = dest;
    cd_dma_log[cd_dma_log_head % CD_DMA_LOG_CAP].size = size;
    cd_dma_log_head = (cd_dma_log_head + 1) % CD_DMA_LOG_CAP;
    cd_dma_log_total++;
}

uint32_t cd_dma_log_get_total(void) { return cd_dma_log_total; }
void     cd_dma_log_get_entry(uint32_t idx, int *lba, uint32_t *dest, uint32_t *size) {
    uint32_t cap   = CD_DMA_LOG_CAP;
    uint32_t oldest = cd_dma_log_total > cap ? cd_dma_log_total - cap : 0;
    if (idx < oldest || idx >= cd_dma_log_total) { *lba = -1; return; }
    uint32_t slot = idx % cap;
    *lba  = cd_dma_log[slot].lba;
    *dest = cd_dma_log[slot].dest;
    *size = cd_dma_log[slot].size;
}

#define DMA_MDEC_IN_CYCLES_PER_WORD   1u
#define DMA_MDEC_OUT_CYCLES_PER_WORD 14u
#define DMA_GPU_CYCLES_PER_WORD       1u
#define DMA_CDROM_CYCLES_PER_WORD     1u
/* SPU DMA (ch4) per-word cost. Faithful to the Beetle/mednafen oracle, which
 * charges `extra_cyc_overhead = 47` per word plus the universal 1 cyc/word in
 * RunChannel (mednafen/psx/dma.cpp:267,294,456) => 48 cyc/word. Previously ch4
 * completed in ZERO cycles (instant complete_transfer), so the DMA-completion
 * IRQ fired the same cycle as the kick. Once the I-cache cycle model lands, the
 * BIOS SPU-init loop's instruction timing interleaves with that instant IRQ into
 * an exception re-entry storm (MMX6 boot wedge: kick ch4 -> 0-cyc done -> IRQ ->
 * ack -> re-kick, forever). Deferring completion the faithful ~48 cyc/word breaks
 * the storm and matches hardware (the SPU RAM payload still moves immediately;
 * only the busy-bit clear + completion IRQ are deferred). */
#define DMA_SPU_CYCLES_PER_WORD      48u

/* DMA-execution provenance flags (read by memory.c's psx_write_word d44_note probe
 * for the MMX6 VSync-callback-pointer corruption hunt). g_dma_exec_depth>0 means a
 * DMA is currently moving data through psx_write_word, so a RAM write seen there is
 * DMA-sourced (not a CPU/dirty-interp store, whose g_debug_last_store_pc is accurate).
 * cur_ch/cur_madr/cur_bcr name the in-flight channel + its destination, so a wrong
 * DMA destination clobbering kernel data becomes directly visible. */
int      g_dma_exec_depth = 0;
int      g_dma_cur_ch     = -1;
uint32_t g_dma_cur_madr   = 0;
uint32_t g_dma_cur_bcr    = 0;
/* Guest PC that kicked the in-flight DMA (the CHCR store that set start/busy),
 * so write-provenance (wtrace / parity note_write) attributes DMA-sourced RAM
 * writes to the code that INITIATED the transfer, not to g_debug_last_store_pc
 * (which for an async transfer is a stale, unrelated CPU store). Captured at the
 * kick in trigger_dma_transfer and re-published for async channels whose RAM
 * writes run in a later advance_*() step. 0 = unknown. */
uint32_t g_dma_initiator_pc = 0;
static uint32_t s_dma_ch_initiator_pc[7] = {0};

typedef struct {
    uint8_t active;
    uint32_t total_words;
    uint32_t cycles_remaining;
} DMADelayedComplete;

static DMADelayedComplete delayed_complete[7];

/* ---- Global registers ---- */

static uint32_t dpcr;  /* 0x1F8010F0: DMA control (enable bits) */
static uint32_t dicr;  /* 0x1F8010F4: DMA interrupt control */

#define DICR_WRITE_MASK 0x00FF807Fu
#define DICR_RESET_MASK 0x7F000000u

static DMATraceEntry dma_trace[DMA_TRACE_CAP];
static uint64_t dma_trace_seq;
static DMACDROMHistoryEntry cdrom_dma_history[DMA_CDROM_HISTORY_CAP];
static uint64_t cdrom_dma_history_seq;
static DMACDROMHistoryEntry cdrom_dma_active_entry;
static uint8_t cdrom_dma_history_active;

static uint32_t dicr_read_value(uint32_t v);

static void trace_dma(uint32_t kind, int ch, uint32_t total_words,
                      uint32_t dicr_before, uint32_t i_stat_before) {
    DMATraceEntry *e = &dma_trace[dma_trace_seq % DMA_TRACE_CAP];
    e->seq = dma_trace_seq++;
    e->frame = (uint32_t)s_frame_count;
    e->kind = kind;
    e->channel = (uint32_t)ch;
    e->total_words = total_words;
    e->addr = 0;
    e->val = 0;
    e->mask = 0;
    e->madr = (ch >= 0 && ch < 7) ? channels[ch].madr : 0;
    e->bcr = (ch >= 0 && ch < 7) ? channels[ch].bcr : 0;
    e->chcr = (ch >= 0 && ch < 7) ? channels[ch].chcr : 0;
    e->dpcr = dpcr;
    e->dicr_before = dicr_read_value(dicr_before);
    e->dicr_after = dicr_read_value(dicr);
    e->i_stat_before = i_stat_before;
    e->i_stat_after = i_stat;
    e->func = g_debug_current_func_addr;
    e->pc = g_debug_last_store_pc;
}

static void trace_dma_reg_write(uint32_t addr, uint32_t val, uint32_t mask,
                                uint32_t dicr_before,
                                uint32_t i_stat_before) {
    DMATraceEntry *e = &dma_trace[dma_trace_seq % DMA_TRACE_CAP];
    e->seq = dma_trace_seq++;
    e->frame = (uint32_t)s_frame_count;
    e->kind = 'W';
    e->channel = 0xFFFFFFFFu;
    e->total_words = 0;
    e->addr = addr;
    e->val = val;
    e->mask = mask;
    e->madr = 0;
    e->bcr = 0;
    e->chcr = 0;
    e->dpcr = dpcr;
    e->dicr_before = dicr_read_value(dicr_before);
    e->dicr_after = dicr_read_value(dicr);
    e->i_stat_before = i_stat_before;
    e->i_stat_after = i_stat;
    e->func = g_debug_current_func_addr;
    e->pc = g_debug_last_store_pc;
}

/* ---- Helpers ---- */

static void update_master_irq(void) {
    /* DICR bit 31 (master IRQ flag) is read-only, calculated as:
     * bit31 = bit15 OR (bit23 AND ((bits16-22 AND bits24-30) != 0))
     *
     * We store bits 0-30 in dicr and compute bit 31 on read.
     *
     * I_STAT bit 3 is set on the aggregate DICR bit31 0->1 transition.
     * Per-channel DICR flags are latched by complete_transfer() only
     * when both master IRQ and that channel IRQ are enabled. */
}

static uint32_t dicr_master_flag(uint32_t v) {
    return ((v & (1u << 15)) ||
            ((v & (1u << 23)) && (v & DICR_RESET_MASK))) ? (1u << 31) : 0;
}

static uint32_t dicr_read_value(uint32_t v) {
    return (v & ~(1u << 31)) | dicr_master_flag(v);
}

static void raise_dma_irq_on_master_edge(uint32_t before) {
    /* IRQ3 is latched only when DICR's aggregate master flag transitions
     * from 0 to 1. Pending channel flags keep bit 31 high, but do not
     * continuously re-latch I_STAT after software acknowledges IRQ3. */
    if (!dicr_master_flag(before) && dicr_master_flag(dicr)) {
        psx_irq_raise(3, (dicr >> 24) & 0x7Fu);  /* detail = DICR per-channel IRQ flags */
    }
}

static void start_cdrom_dma_capture(uint32_t requested_words) {
    CDROMDebugState s;
    cdrom_debug_snapshot(&s);

    memset(&cdrom_dma_active_entry, 0, sizeof(cdrom_dma_active_entry));
    cdrom_dma_active_entry.seq = cdrom_dma_history_seq++;
    cdrom_dma_active_entry.frame_start = (uint32_t)s_frame_count;
    cdrom_dma_active_entry.start_addr = channels[3].madr & 0x1FFFFCu;
    cdrom_dma_active_entry.final_addr = cdrom_dma_active_entry.start_addr;
    cdrom_dma_active_entry.requested_words = requested_words;
    cdrom_dma_active_entry.bcr = channels[3].bcr;
    cdrom_dma_active_entry.chcr = channels[3].chcr;
    cdrom_dma_active_entry.dpcr = dpcr;
    cdrom_dma_active_entry.dicr_start = dicr_read_value(dicr);
    cdrom_dma_active_entry.i_stat_start = i_stat;
    cdrom_dma_active_entry.func = g_debug_current_func_addr;
    cdrom_dma_active_entry.pc = g_debug_last_store_pc;
    cdrom_dma_active_entry.lba = s.last_sector_lba;
    cdrom_dma_active_entry.sector_size =
        (s.sector_size > 0) ? s.sector_size : s.last_sector_size;
    cdrom_dma_active_entry.sector_read_pos_start = s.sector_read_pos;
    cdrom_dma_active_entry.mode =
        s.last_sector_mode ? s.last_sector_mode : s.mode_reg;
    cdrom_dma_active_entry.sector_available_start =
        (uint8_t)(s.sector_available ? 1 : 0);
    cdrom_dma_history_active = 1;
}

static void record_cdrom_dma_word(uint32_t word) {
    if (!cdrom_dma_history_active) return;

    DMACDROMHistoryEntry *e = &cdrom_dma_active_entry;
    if (e->first_count < DMA_CDROM_HISTORY_WORDS) {
        e->first_words[e->first_count++] = word;
    }

    if (e->last_count < DMA_CDROM_HISTORY_WORDS) {
        e->last_words[e->last_count++] = word;
    } else {
        memmove(e->last_words, e->last_words + 1,
                sizeof(e->last_words[0]) * (DMA_CDROM_HISTORY_WORDS - 1));
        e->last_words[DMA_CDROM_HISTORY_WORDS - 1] = word;
    }

    e->moved_words++;
}

static void finish_cdrom_dma_capture(uint32_t final_addr, uint8_t completed) {
    if (!cdrom_dma_history_active) return;

    CDROMDebugState s;
    cdrom_debug_snapshot(&s);

    cdrom_dma_active_entry.frame_end = (uint32_t)s_frame_count;
    cdrom_dma_active_entry.final_addr = final_addr & 0x1FFFFCu;
    cdrom_dma_active_entry.dicr_end = dicr_read_value(dicr);
    cdrom_dma_active_entry.i_stat_end = i_stat;
    cdrom_dma_active_entry.sector_read_pos_end = s.sector_read_pos;
    cdrom_dma_active_entry.sector_available_end =
        (uint8_t)(s.sector_available ? 1 : 0);
    cdrom_dma_active_entry.completed = completed ? 1 : 0;

    cdrom_dma_history[
        cdrom_dma_active_entry.seq % DMA_CDROM_HISTORY_CAP] =
        cdrom_dma_active_entry;
    cdrom_dma_history_active = 0;
}

static int channel_enabled(int ch) {
    /* DPCR: each channel has 4 bits, bit 3 of each group is enable.
     * Ch0 = bits 0-3, Ch1 = bits 4-7, etc. Enable = bit (ch*4 + 3). */
    return (dpcr >> (ch * 4 + 3)) & 1;
}

static int channel_irq_flag_armed(int ch) {
    /* DICR channel completion flags (24+n) are latched only when both
     * the per-channel interrupt bit and the master DMA interrupt bit are
     * enabled at completion time. Old flags still contribute to bit31
     * until acknowledged, but masked/master-disabled completions do not
     * create new stale flags. */
    return ((dicr >> (16 + ch)) & 1u) && ((dicr >> 23) & 1u);
}

static uint32_t transfer_word_count(int ch) {
    uint32_t chcr = channels[ch].chcr;
    uint32_t sync_mode = (chcr >> 9) & 3;
    uint32_t block_size = channels[ch].bcr & 0xFFFFu;
    uint32_t block_count = (channels[ch].bcr >> 16) & 0xFFFFu;

    if (ch == 3 && block_size == 0) {
        return cdrom_dma_sector_word_count();
    }

    if (sync_mode == 1) {
        if (block_size == 0) block_size = 0x10000u;
        if (block_count == 0) block_count = 1u;
        return block_size * block_count;
    }

    if (block_size == 0) block_size = 0x10000u;
    return block_size;
}

/* A single DMA kick can never legitimately move more data than the larger
 * of RAM (2 MB) or VRAM (1 MB): 0x80000 words. A bigger count means the
 * MADR/BCR/CHCR the guest programmed are corrupt; executing it would grind
 * through billions of masked-wrap accesses feeding garbage to the device
 * sinks. Halt diagnosably with rings intact instead. Linked-list GPU
 * transfers (ch2 sync mode 2) don't use BCR, so length isn't checked there
 * (the node walker has its own MAX_NODES cycle cap). */
#define DMA_MAX_TRANSFER_WORDS 0x80000u

static void validate_transfer_length(int ch) {
    uint32_t sync_mode = (channels[ch].chcr >> 9) & 3u;
    if (ch == 2 && sync_mode == 2) return;
    uint32_t words = transfer_word_count(ch);
    if (words > DMA_MAX_TRANSFER_WORDS) {
        static char reason[160];
        snprintf(reason, sizeof(reason),
                 "DMA ch%d insane transfer length 0x%X words "
                 "(MADR=0x%08X BCR=0x%08X CHCR=0x%08X)",
                 ch, words, channels[ch].madr, channels[ch].bcr,
                 channels[ch].chcr);
        psx_fatal_halt(reason);
    }
}

static void complete_transfer(int ch) {
    uint32_t dicr_before = dicr;
    uint32_t i_stat_before = i_stat;
    channels[ch].chcr &= ~((1u << 24) | (1u << 28));
    if (channel_irq_flag_armed(ch)) {
        dicr |= (1u << (24 + ch));
        raise_dma_irq_on_master_edge(dicr_before);
    }
    trace_dma('C', ch, 0, dicr_before, i_stat_before);
    event_ring_record_aux(EV_DMA_DONE, (uint8_t)ch, channels[ch].chcr);
    event_ring_record_aux(EV_DEQ, (uint8_t)(SRC_DMA0 + ch), channels[ch].chcr);
}

/* ---- Transfer execution ---- */

static void cancel_async_transfer(int ch) {
    if (ch >= 0 && ch < 2) {
        mdec_async[ch].active = 0;
        mdec_async[ch].debug_started = 0;
        mdec_async[ch].total_words = 0;
        mdec_async[ch].remaining_words = 0;
        mdec_async[ch].cycles_accum = 0;
    }
    if (ch == 3) {
        finish_cdrom_dma_capture(channels[3].madr & 0x1FFFFCu, 0);
        cdrom_async.active = 0;
        cdrom_async.debug_started = 0;
        cdrom_async.total_words = 0;
        cdrom_async.remaining_words = 0;
        cdrom_async.cycles_accum = 0;
    }
    if (ch >= 0 && ch < 7) {
        delayed_complete[ch].active = 0;
        delayed_complete[ch].total_words = 0;
        delayed_complete[ch].cycles_remaining = 0;
    }
}

static void schedule_delayed_complete(int ch, uint32_t total_words,
                                      uint32_t cycles_per_word) {
    if (total_words == 0 || cycles_per_word == 0) {
        complete_transfer(ch);
        return;
    }

    uint64_t cycles = (uint64_t)total_words * (uint64_t)cycles_per_word;
    if (cycles > UINT32_MAX) cycles = UINT32_MAX;

    delayed_complete[ch].active = 1;
    delayed_complete[ch].total_words = total_words;
    delayed_complete[ch].cycles_remaining = (uint32_t)cycles;
    event_ring_record_aux(EV_DMA_SCHED, (uint8_t)ch, channels[ch].chcr);
}

static void advance_delayed_complete(int ch, uint32_t cycles) {
    DMADelayedComplete *d = &delayed_complete[ch];
    if (!d->active) return;
    if (cycles < d->cycles_remaining) {
        d->cycles_remaining -= cycles;
        return;
    }

    d->active = 0;
    d->total_words = 0;
    d->cycles_remaining = 0;
    complete_transfer(ch);
}

static void start_async_mdec_transfer(int ch) {
    DMAAsyncChannel *a = &mdec_async[ch];
    if (a->active) return;

    a->active = 1;
    a->debug_started = 0;
    a->total_words = transfer_word_count(ch);
    a->remaining_words = a->total_words;
    a->cycles_accum = 0;

    if (ch == 0) {
        mdec_debug_dma_in_start(channels[0].madr & 0x1FFFFCu, a->remaining_words);
    } else {
        mdec_debug_dma_out_start(channels[1].madr & 0x1FFFFCu, a->remaining_words);
    }
    a->debug_started = 1;
}

static void finish_async_mdec_transfer(int ch, uint32_t final_addr, uint32_t total_words) {
    if (ch == 0) {
        mdec_debug_dma_in_end(final_addr, total_words);
    } else {
        mdec_debug_dma_out_end(final_addr, total_words);
    }
    cancel_async_transfer(ch);
    complete_transfer(ch);
}

static void start_async_cdrom_transfer(void) {
    DMAAsyncChannel *a = &cdrom_async;
    if (a->active) return;

    a->active = 1;
    a->debug_started = 0;
    a->total_words = transfer_word_count(3);
    a->remaining_words = a->total_words;
    a->cycles_accum = 0;
    a->start_addr = channels[3].madr & 0x1FFFFCu;
    start_cdrom_dma_capture(a->total_words);

    if (a->total_words == 0) {
        finish_cdrom_dma_capture(channels[3].madr & 0x1FFFFCu, 1);
        cancel_async_transfer(3);
        complete_transfer(3);
    }
}

static void finish_async_cdrom_transfer(uint32_t final_addr) {
    finish_cdrom_dma_capture(final_addr, 1);
    DMAAsyncChannel *a = &cdrom_async;
    uint32_t step       = (channels[3].chcr >> 1) & 1u;
    uint32_t load_start = a->start_addr;
    uint32_t size       = a->total_words * 4u;

    /* Log and capture game-data transfers only.
     * Transfers to 0x1C0000+ are FMV/streaming buffers; skip them. */
    if (!step && size > 0 && load_start < 0x1C0000u) {
        /* The DMA word loop wraps inside the 2 MB RAM mask; the capture
         * path below takes a flat ram+offset span and must not follow the
         * wrap past the end of host RAM. */
        if (size > 0x200000u - load_start)
            size = 0x200000u - load_start;
        int lba = cdrom_get_setloc_lba();
        if (lba >= 0) cd_dma_log_push(lba, load_start, size);

        /* B-1: capture overlay bytes into the write-once capture set.
         * overlay_capture_on_dma auto-activates after game handoff and is a
         * no-op unless the overlay cache is enabled in config. */
        extern uint8_t *memory_get_ram_ptr(void);
        uint8_t *ram = memory_get_ram_ptr();
        overlay_capture_on_dma(load_start, size, ram + load_start);
    }

    channels[3].madr = final_addr;
    cancel_async_transfer(3);
    complete_transfer(3);
}

static void advance_mdec_channel(int ch, uint32_t cycles) {
    DMAAsyncChannel *a = &mdec_async[ch];
    if (!a->active) return;
    if (!((channels[ch].chcr >> 24) & 1u) || !channel_enabled(ch)) return;

    uint32_t chcr = channels[ch].chcr;
    uint32_t direction = chcr & 1u;
    uint32_t step = (chcr >> 1) & 1u;
    int32_t addr_step = step ? -4 : 4;
    uint32_t cycles_per_word = (ch == 0) ? DMA_MDEC_IN_CYCLES_PER_WORD : DMA_MDEC_OUT_CYCLES_PER_WORD;

    if ((ch == 0 && direction == 0) || (ch == 1 && direction != 0)) {
        uint32_t words = a->total_words;
        uint32_t addr = channels[ch].madr & 0x1FFFFCu;
        finish_async_mdec_transfer(ch, addr, words);
        return;
    }

    if (ch == 0) {
        if (!mdec_dma_write_ready()) return;
    } else {
        if (!mdec_dma_read_ready()) return;
    }

    if (cycles > UINT32_MAX - a->cycles_accum) {
        a->cycles_accum = UINT32_MAX;
    } else {
        a->cycles_accum += cycles;
    }

    uint32_t words_budget = a->cycles_accum / cycles_per_word;
    if (words_budget == 0) return;

    uint32_t addr = channels[ch].madr & 0x1FFFFCu;
    uint32_t moved = 0;
    /* ch1 (MDEC→RAM) writes guest RAM here in a deferred step; mark it as
     * DMA-sourced so write-provenance tags these writes with ch1 + the kick PC
     * (ch0 only reads RAM, so it needs no marking). Mirrors the CDROM async. */
    int mdec_writes_ram = (ch == 1);
    if (mdec_writes_ram) {
        g_dma_exec_depth++;
        g_dma_cur_ch = 1; g_dma_cur_bcr = channels[1].bcr;
        g_dma_initiator_pc = s_dma_ch_initiator_pc[1];
    }
    /* Contiguous +4 ch0 (RAM→MDEC): feed via LE burst helper. Guest halfwords
     * + decode triggers match the per-word loop; wraps / decrementing MADR
     * and ch1 (needs psx_write_word watchers) stay on the word path. */
    if (ch == 0 && addr_step == 4) {
        extern uint8_t *memory_get_ram_ptr(void);
        uint8_t *ram = memory_get_ram_ptr();
        while (a->remaining_words > 0 && words_budget > 0) {
            if (!mdec_dma_write_ready()) break;
            uint32_t n = a->remaining_words < words_budget
                       ? a->remaining_words : words_budget;
            uint32_t max_by_ram = (0x200000u - addr) / 4u;
            if (max_by_ram == 0) break;
            if (n > max_by_ram) n = max_by_ram;
            uint32_t got =
                mdec_dma_write_words((const uint32_t *)(ram + addr), n);
            if (got == 0) break;
            addr = (addr + got * 4u) & 0x1FFFFCu;
            a->remaining_words -= got;
            words_budget -= got;
            moved += got;
        }
    } else {
        while (a->remaining_words > 0 && words_budget > 0) {
            if (ch == 0) {
                if (!mdec_dma_write_ready()) break;
                mdec_dma_write_word(psx_read_word(addr));
            } else {
                if (!mdec_dma_read_ready()) break;
                g_dma_cur_madr = addr;
                psx_write_word(addr, mdec_dma_read_word());
            }

            addr = (addr + addr_step) & 0x1FFFFCu;
            a->remaining_words--;
            words_budget--;
            moved++;
        }
    }
    if (mdec_writes_ram) { g_dma_cur_ch = -1; g_dma_exec_depth--; }

    if (moved == 0) return;

    a->cycles_accum -= moved * cycles_per_word;
    channels[ch].madr = addr;

    if (a->remaining_words == 0) {
        finish_async_mdec_transfer(ch, addr, a->total_words);
    }
}

static void execute_ch0_mdec_in(void) {
    uint32_t chcr = channels[0].chcr;
    uint32_t direction = chcr & 1;           /* 1=from RAM to MDEC */
    uint32_t step = (chcr >> 1) & 1;
    uint32_t total_words = transfer_word_count(0);
    uint32_t addr = channels[0].madr & 0x1FFFFCu;
    int32_t addr_step = step ? -4 : 4;

    if (direction != 0) {
        mdec_debug_dma_in_start(addr, total_words);
        if (addr_step == 4 && total_words > 0 &&
            addr + total_words * 4u <= 0x200000u) {
            extern uint8_t *memory_get_ram_ptr(void);
            uint32_t got = mdec_dma_write_words(
                (const uint32_t *)(memory_get_ram_ptr() + addr), total_words);
            addr = (addr + got * 4u) & 0x1FFFFCu;
            /* If the FIFO stalled mid-burst, finish any remainder word-wise. */
            for (uint32_t i = got; i < total_words; i++) {
                mdec_dma_write_word(psx_read_word(addr));
                addr = (addr + 4u) & 0x1FFFFCu;
            }
        } else {
            for (uint32_t i = 0; i < total_words; i++) {
                mdec_dma_write_word(psx_read_word(addr));
                addr = (addr + addr_step) & 0x1FFFFCu;
            }
        }
        mdec_debug_dma_in_end(addr, total_words);
        channels[0].madr = addr;
    }

    complete_transfer(0);
}

static void execute_ch1_mdec_out(void) {
    uint32_t chcr = channels[1].chcr;
    uint32_t direction = chcr & 1;           /* 0=from MDEC to RAM */
    uint32_t step = (chcr >> 1) & 1;
    uint32_t total_words = transfer_word_count(1);
    uint32_t addr = channels[1].madr & 0x1FFFFCu;
    int32_t addr_step = step ? -4 : 4;

    if (direction == 0) {
        mdec_debug_dma_out_start(addr, total_words);
        for (uint32_t i = 0; i < total_words; i++) {
            psx_write_word(addr, mdec_dma_read_word());
            addr = (addr + addr_step) & 0x1FFFFCu;
        }
        mdec_debug_dma_out_end(addr, total_words);
        channels[1].madr = addr;
    }

    complete_transfer(1);
}

static uint32_t execute_ch2_gpu(void) {
    uint32_t chcr = channels[2].chcr;
    uint32_t direction = chcr & 1;           /* 0=to RAM, 1=from RAM (to device) */
    uint32_t step = (chcr >> 1) & 1;         /* 0=forward(+4), 1=backward(-4) */
    uint32_t sync_mode = (chcr >> 9) & 3;    /* 0=burst, 1=block, 2=linked-list */
    uint32_t actual_words = 0;

    if (direction == 0) {
        /* GPU → RAM (VRAM read): read pixel data via GPUREAD.
         * A prior GP0(C0h) command must have set up the VRAM read region. */
        if (sync_mode == 1) {
            uint32_t block_size = channels[2].bcr & 0xFFFF;
            uint32_t block_count = (channels[2].bcr >> 16) & 0xFFFF;
            uint32_t total_words = block_size * block_count;
            uint32_t addr = channels[2].madr & 0x1FFFFCu;
            int32_t  addr_step = step ? -4 : 4;
            for (uint32_t i = 0; i < total_words; i++) {
                uint32_t pixel_data = gpu_read_gpuread();
                psx_write_word(addr, pixel_data);
                addr = (addr + addr_step) & 0x1FFFFCu;
            }
            channels[2].madr = addr;
            actual_words = total_words;
        }
        return actual_words;
    }

    /* direction == 1: RAM → GPU */
    if (sync_mode == 1) {
        /* Block mode: BCR bits 0-15 = block size (words), bits 16-31 = block count */
        uint32_t block_size = channels[2].bcr & 0xFFFF;
        uint32_t block_count = (channels[2].bcr >> 16) & 0xFFFF;
        uint32_t total_words = block_size * block_count;
        uint32_t addr = channels[2].madr & 0x1FFFFCu; /* mask to RAM, word-aligned */
        int32_t  addr_step = step ? -4 : 4;

        for (uint32_t i = 0; i < total_words; i++) {
            uint32_t word = psx_read_word(addr);
            gpu_set_gp0_source(addr);
            gpu_write_gp0(word);
            addr = (addr + addr_step) & 0x1FFFFCu;
        }
        channels[2].madr = addr;
        actual_words = total_words;
    } else if (sync_mode == 2) {
        /* Linked-list mode: follow ordering table in RAM.
         * Each node: bits 24-31 = number of words following header,
         *            bits 0-23  = next node address (0xFFFFFF = end).
         * The words following the header are sent to GP0. */
        gpu_ws_begin_linked_list();
        uint32_t addr = channels[2].madr & 0x1FFFFCu;
        uint32_t safety = 0;
        const uint32_t MAX_NODES = 0x40000; /* prevent infinite loops */

        for (;;) {
            if (safety++ > MAX_NODES) {
                /* Abort this transfer — linked list has a cycle or is corrupt.
                 * Don't crash; the shell may recover on the next frame. */
                break;
            }

            uint32_t header = psx_read_word(addr);
            uint32_t num_words = (header >> 24) & 0xFF;
            uint32_t word_addr = (addr + 4) & 0x1FFFFCu;
            actual_words += 1u;

            for (uint32_t i = 0; i < num_words; i++) {
                uint32_t word = psx_read_word(word_addr);
                gpu_set_gp0_source(word_addr);
                gpu_write_gp0(word);
                word_addr = (word_addr + 4) & 0x1FFFFCu;
            }
            actual_words += num_words;

            /* Next node */
            uint32_t next = header & 0xFFFFFFu;
            if (next == 0xFFFFFFu) {
                channels[2].madr = 0x00FFFFFFu;
                break; /* end of list */
            }
            addr = next & 0x1FFFFCu;
        }
        if (safety > MAX_NODES) {
            channels[2].madr = addr;
        }
    } else {
        /* Burst mode (sync_mode == 0) */
        uint32_t word_count = channels[2].bcr & 0xFFFF;
        if (word_count == 0) word_count = 0x10000; /* 0 means 0x10000 */
        uint32_t addr = channels[2].madr & 0x1FFFFCu;
        int32_t  addr_step = step ? -4 : 4;

        for (uint32_t i = 0; i < word_count; i++) {
            uint32_t word = psx_read_word(addr);
            gpu_set_gp0_source(addr);
            gpu_write_gp0(word);
            addr = (addr + addr_step) & 0x1FFFFCu;
        }
        channels[2].madr = addr;
        actual_words = word_count;
    }

    return actual_words;
}

static void execute_ch3_cdrom(void) {
    uint32_t chcr = channels[3].chcr;
    uint32_t direction = chcr & 1;           /* 0=to RAM, 1=from RAM */

    if (direction != 0) {
        channels[3].chcr &= ~((1u << 24) | (1u << 28));
        return;
    }

    start_async_cdrom_transfer();
}

/* Returns the number of words moved so the caller can schedule a faithful
 * delayed completion (DMA_SPU_CYCLES_PER_WORD). The payload moves immediately
 * (SPU RAM is correct the instant this returns); only the busy-bit clear and
 * completion IRQ are deferred by schedule_delayed_complete. */
static uint32_t execute_ch4_spu(void) {
    uint32_t chcr = channels[4].chcr;
    uint32_t direction = chcr & 1;           /* 1=from RAM to SPU, 0=SPU to RAM */
    uint32_t step = (chcr >> 1) & 1;
    uint32_t total_words = transfer_word_count(4);
    uint32_t addr = channels[4].madr & 0x1FFFFCu;
    int32_t addr_step = step ? -4 : 4;

    if (direction != 0) {
        for (uint32_t i = 0; i < total_words; i++) {
            spu_dma_write(psx_read_word(addr));
            addr = (addr + addr_step) & 0x1FFFFCu;
        }
        /* One aggregated event per SPU-bound transfer (per-word would flood
         * the ring: a full sound-bank upload is ~128k words). */
        audio_trace_event(AUDIO_EV_DMA_WRITE, total_words,
                          channels[4].madr & 0x1FFFFCu);
    } else {
        for (uint32_t i = 0; i < total_words; i++) {
            psx_write_word(addr, 0);
            addr = (addr + addr_step) & 0x1FFFFCu;
        }
    }

    channels[4].madr = addr;
    return total_words;
}

static void execute_ch6_otc(void) {
    /* OTC (Ordering Table Clear): writes a backward-linked list to RAM.
     * Node N = address of node N-1, node 0 = 0xFFFFFF (end marker).
     * Direction is always to-RAM, step is always backward.
     * BCR bits 0-15 = number of entries. */
    uint32_t num_entries = channels[6].bcr & 0xFFFF;
    if (num_entries == 0) num_entries = 0x10000;
    uint32_t addr = channels[6].madr & 0x1FFFFCu;

    for (uint32_t i = 0; i < num_entries; i++) {
        uint32_t val;
        if (i == num_entries - 1) {
            /* Last entry (first in memory): end marker */
            val = 0x00FFFFFFu;
        } else {
            /* Points to the previous entry (addr - 4) */
            val = (addr - 4) & 0x00FFFFFFu;
        }
        psx_write_word(addr, val);
        addr = (addr - 4) & 0x1FFFFCu;
    }

    complete_transfer(6);
}

static void try_execute(int ch) {
    uint32_t chcr = channels[ch].chcr;

    /* Transfer starts when bit 24 (start/busy) is set AND channel is enabled in DPCR */
    if (!((chcr >> 24) & 1)) return;
    if (!channel_enabled(ch)) return;

    channels[ch].chcr &= ~(1u << 28);
    trace_dma('S', ch, transfer_word_count(ch), dicr, i_stat);
    event_ring_record_aux(EV_DMA_KICK, (uint8_t)ch, channels[ch].chcr);
    event_ring_record_aux(EV_ENQ, (uint8_t)(SRC_DMA0 + ch), transfer_word_count(ch));

    /* After the kick is in the rings, refuse corrupt-length transfers. */
    validate_transfer_length(ch);

    /* Capture the kick PC (this CHCR store) so both the immediate (sync) writes
     * below and any deferred async writes for this channel attribute to it. */
    s_dma_ch_initiator_pc[ch] = g_debug_last_store_pc;
    g_dma_initiator_pc        = g_debug_last_store_pc;
    g_dma_exec_depth++;
    g_dma_cur_ch = ch; g_dma_cur_madr = channels[ch].madr; g_dma_cur_bcr = channels[ch].bcr;
    switch (ch) {
        case 0:
            start_async_mdec_transfer(0);
            break;
        case 1:
            start_async_mdec_transfer(1);
            break;
        case 2:
            schedule_delayed_complete(2, execute_ch2_gpu(),
                                      DMA_GPU_CYCLES_PER_WORD);
            break;
        case 3:
            execute_ch3_cdrom();
            break;
        case 4:
            schedule_delayed_complete(4, execute_ch4_spu(),
                                      DMA_SPU_CYCLES_PER_WORD);
            break;
        case 6:
            execute_ch6_otc();
            break;
        default: {
            /* Other channels not implemented yet — fatal if transfer is triggered */
            static char reason[96];
            snprintf(reason, sizeof(reason),
                     "DMA ch%d: transfer triggered but not implemented (CHCR=0x%08X)",
                     ch, chcr);
            psx_fatal_halt(reason);
        }
    }
    g_dma_exec_depth--;
    g_dma_cur_ch = -1;
}

/* ---- Public interface ---- */

uint32_t dma_get_dicr(void) { return dicr_read_value(dicr); }
uint32_t dma_get_dpcr(void) { return dpcr; }
int dma_cdrom_transfer_active(void) {
    return cdrom_async.active &&
           ((channels[3].chcr >> 24) & 1u) &&
           channel_enabled(3) &&
           ((channels[3].chcr & 1u) == 0);
}

/* Cycle-budgeted precise event slicing: guest CPU cycles until DMA raises a
 * DELIVERABLE IRQ (bit3 unmasked in i_mask). UINT32_MAX if none. Conservative
 * under-estimate: for async channels uses a 1-cycle/word floor minus cycles
 * already accumulated (always <= the true remaining, since per-word cost >= 1),
 * and the exact countdown for delayed-complete channels. Over-slicing on a
 * channel whose DICR completion is masked is safe. See PRECISE_IRQ_SLICE.md. */
uint32_t dma_cycles_to_irq(uint32_t i_mask) {
    if (!(i_mask & (1u << 3))) return 0xFFFFFFFFu;   /* IRQ_DMA masked */
    uint32_t best = 0xFFFFFFFFu;
    const DMAAsyncChannel *async_ch[3] = { &mdec_async[0], &mdec_async[1], &cdrom_async };
    for (int i = 0; i < 3; i++) {
        const DMAAsyncChannel *a = async_ch[i];
        if (!a->active || a->remaining_words == 0) continue;
        /* floor: rw*per_word - accum >= rw - accum (per_word >= 1). Clamp >=0. */
        uint32_t est = a->remaining_words > a->cycles_accum
                         ? (a->remaining_words - a->cycles_accum) : 0u;
        if (est < best) best = est;
    }
    for (int ch = 0; ch < 7; ch++) {
        if (delayed_complete[ch].active && delayed_complete[ch].cycles_remaining < best)
            best = delayed_complete[ch].cycles_remaining;
    }
    return best;
}

uint32_t dma_cycles_to_internal_event(void) {
    uint32_t best = 0xFFFFFFFFu;

    /* Async MDEC channels move one word whenever their cycle accumulator
     * reaches the channel cost. Completion-only scheduling batches many RAM
     * writes at a later service boundary, which is observably different when
     * the CPU polls the destination buffer. */
    for (int ch = 0; ch < 2; ch++) {
        const DMAAsyncChannel *a = &mdec_async[ch];
        if (!a->active || a->remaining_words == 0 ||
            !((channels[ch].chcr >> 24) & 1u) || !channel_enabled(ch))
            continue;
        uint32_t direction = channels[ch].chcr & 1u;
        if ((ch == 0 && direction == 0) || (ch == 1 && direction != 0))
            return 1u; /* invalid route is completed on the next DMA tick */
        if ((ch == 0 && !mdec_dma_write_ready()) ||
            (ch == 1 && !mdec_dma_read_ready()))
            continue;
        uint32_t cpw = ch == 0 ? DMA_MDEC_IN_CYCLES_PER_WORD
                               : DMA_MDEC_OUT_CYCLES_PER_WORD;
        uint32_t d = a->cycles_accum < cpw ? cpw - a->cycles_accum : 1u;
        if (d < best) best = d;
    }

    /* CD-ROM DMA writes guest RAM incrementally. Expose each word on time;
     * waiting only for the channel-complete IRQ can delay hundreds of writes. */
    if (cdrom_async.active && cdrom_async.remaining_words != 0 &&
        ((channels[3].chcr >> 24) & 1u) && channel_enabled(3)) {
        if ((channels[3].chcr & 1u) != 0) {
            return 1u; /* unsupported RAM->CD direction cancels next tick */
        }
        if (cdrom_dma_ready()) {
            uint32_t d = cdrom_async.cycles_accum < DMA_CDROM_CYCLES_PER_WORD
                       ? DMA_CDROM_CYCLES_PER_WORD - cdrom_async.cycles_accum
                       : 1u;
            if (d < best) best = d;
        }
    }

    for (int ch = 0; ch < 7; ch++) {
        if (delayed_complete[ch].active &&
            delayed_complete[ch].cycles_remaining < best)
            best = delayed_complete[ch].cycles_remaining;
    }
    return best;
}

uint32_t dma_cycles_to_deliverable_irq(uint32_t i_mask) {
    if (!(i_mask & (1u << 3))) return 0xFFFFFFFFu;
    uint32_t best = 0xFFFFFFFFu;
    const int async_num[3] = { 0, 1, 3 };
    const DMAAsyncChannel *async_ch[3] = {
        &mdec_async[0], &mdec_async[1], &cdrom_async
    };
    for (int i = 0; i < 3; i++) {
        const DMAAsyncChannel *a = async_ch[i];
        if (!channel_irq_flag_armed(async_num[i]) ||
            !a->active || a->remaining_words == 0) continue;
        uint32_t est = a->remaining_words > a->cycles_accum
                         ? (a->remaining_words - a->cycles_accum) : 0u;
        if (est < best) best = est;
    }
    for (int ch = 0; ch < 7; ch++) {
        if (channel_irq_flag_armed(ch) && delayed_complete[ch].active &&
            delayed_complete[ch].cycles_remaining < best)
            best = delayed_complete[ch].cycles_remaining;
    }
    return best;
}

void dma_advance(uint32_t cycles) {
    if (cycles == 0) return;
    g_dma_exec_depth++;   /* async to-RAM DMA writes below run through psx_write_word */
    advance_mdec_channel(0, cycles);
    advance_mdec_channel(1, cycles);
    DMAAsyncChannel *a = &cdrom_async;
    if (dma_cdrom_transfer_active()) {
        uint32_t chcr = channels[3].chcr;
        uint32_t direction = chcr & 1u;
        uint32_t step = (chcr >> 1) & 1u;
        int32_t addr_step = step ? -4 : 4;

        if (direction != 0) {
            cancel_async_transfer(3);
            channels[3].chcr &= ~((1u << 24) | (1u << 28));
        } else if (cdrom_dma_ready()) {
            if (cycles > UINT32_MAX - a->cycles_accum) {
                a->cycles_accum = UINT32_MAX;
            } else {
                a->cycles_accum += cycles;
            }

            uint32_t words_budget = a->cycles_accum / DMA_CDROM_CYCLES_PER_WORD;
            uint32_t addr = channels[3].madr & 0x1FFFFCu;
            uint32_t moved = 0;
            g_dma_cur_ch = 3; g_dma_cur_bcr = channels[3].bcr;
            g_dma_initiator_pc = s_dma_ch_initiator_pc[3];  /* deferred: restore kick PC */
            /* Snapshot outgoing executed code at the last possible coherent
             * moment: after the async wait, immediately before the first RAM
             * word. Scheduling-time capture was too early because guest code
             * can continue executing while the CD device is not ready. */
            if (a->remaining_words == a->total_words && words_budget > 0 &&
                addr < 0x1C0000u) {
                uint32_t bytes = a->total_words * 4u;
                if (bytes > 0x200000u - addr) bytes = 0x200000u - addr;
                overlay_capture_before_dma(addr, bytes);
            }
            while (a->remaining_words > 0 && words_budget > 0 && cdrom_dma_ready()) {
                uint32_t word = cdrom_dma_read();
                g_dma_cur_madr = addr;
                psx_write_word(addr, word);
                record_cdrom_dma_word(word);
                dirty_ram_mark_executable_range(addr, 4);
                addr = (addr + addr_step) & 0x1FFFFCu;
                a->remaining_words--;
                words_budget--;
                moved++;
            }

            if (moved > 0) {
                a->cycles_accum -= moved * DMA_CDROM_CYCLES_PER_WORD;
                channels[3].madr = addr;
                if (a->remaining_words == 0) {
                    finish_async_cdrom_transfer(addr);
                }
            }
        }
    }
    /* Drive every delayed-complete channel (ch2 GPU + ch4 SPU today; any future
     * delayed channel is covered automatically — inactive slots no-op). */
    for (int ch = 0; ch < 7; ch++)
        advance_delayed_complete(ch, cycles);
    g_dma_cur_ch = -1;
    g_dma_exec_depth--;
}

void dma_init(void) {
    memset(channels, 0, sizeof(channels));
    memset(mdec_async, 0, sizeof(mdec_async));
    memset(&cdrom_async, 0, sizeof(cdrom_async));
    memset(delayed_complete, 0, sizeof(delayed_complete));
    dpcr = 0x07654321u; /* default: priorities set, no channels enabled */
    dicr = 0;
    dma_debug_clear_trace();
    dma_debug_clear_cdrom_history();
}

uint32_t dma_read(uint32_t addr) {
    /* DPCR */
    if (addr == 0x1F8010F0u) return dpcr;
    /* DICR */
    if (addr == 0x1F8010F4u) {
        return dma_get_dicr();
    }

    /* Per-channel registers: 0x1F801080 + ch*0x10 + offset */
    if (addr >= 0x1F801080u && addr <= 0x1F8010EFu) {
        uint32_t offset = addr - 0x1F801080u;
        int ch = offset / 0x10;
        int reg = offset % 0x10;

        if (ch > 6) goto bad;
        switch (reg) {
            case 0x00: return channels[ch].madr;
            case 0x04: return channels[ch].bcr;
            case 0x08: return channels[ch].chcr;
            case 0x0C: return 0;
            default: goto bad;
        }
    }

bad:
    /* Unmapped words inside the DMA register block (0x1F8010F8/0xFC, channel
     * reg offset 0x0C variants): real hardware open-buses them and Tomba2's
     * late-attract wild I/O sweep (BIOS bzero/read over a 0xDF80xxxx pointer)
     * reads straight through here. Beetle parity: return 0, no fault. */
    {
        extern uint64_t g_io_openbus_reads;
        g_io_openbus_reads++;
    }
    return 0;
}

void dma_write_masked(uint32_t addr, uint32_t val, uint32_t mask) {
    /* DPCR */
    if (addr == 0x1F8010F0u) {
        dpcr = (dpcr & ~mask) | (val & mask);
        return;
    }
    /* DICR: selected low/control bits are writable; bits 24-30 are write-1-to-acknowledge. */
    if (addr == 0x1F8010F4u) {
        /* Bits 0-5: unknown/unused but writable */
        /* Bits 6-14: unused/read-only */
        /* Bit 15: bus-error flag */
        /* Bits 16-22: per-channel IRQ enable */
        /* Bit 23: master IRQ enable */
        /* Bits 24-30: IRQ flags, write 1 to clear */
        /* Bit 31: master flag, read-only (computed) */
        uint32_t dicr_before = dicr;
        uint32_t i_stat_before = i_stat;
        uint32_t write_mask = DICR_WRITE_MASK & mask;
        uint32_t reset_mask = DICR_RESET_MASK & mask;
        dicr = (dicr & ~write_mask) | (val & write_mask);
        dicr &= ~(val & reset_mask);
        raise_dma_irq_on_master_edge(dicr_before);
        trace_dma_reg_write(addr, val, mask, dicr_before, i_stat_before);
        return;
    }

    /* Per-channel registers */
    if (addr >= 0x1F801080u && addr <= 0x1F8010EFu) {
        uint32_t offset = addr - 0x1F801080u;
        int ch = offset / 0x10;
        int reg = offset % 0x10;

        if (ch > 6) goto bad;
        switch (reg) {
            case 0x00:
                channels[ch].madr = (channels[ch].madr & ~mask) | (val & mask);
                return;
            case 0x04:
                channels[ch].bcr = (channels[ch].bcr & ~mask) | (val & mask);
                return;
            case 0x08:
                channels[ch].chcr = (channels[ch].chcr & ~mask) | (val & mask);
                if ((mask & (1u << 24)) && !((channels[ch].chcr >> 24) & 1u)) {
                    cancel_async_transfer(ch);
                }
                /* Writing CHCR's start bit set triggers transfer. */
                if ((mask & (1u << 24)) && ((channels[ch].chcr >> 24) & 1)) {
                    try_execute(ch);
                }
                return;
            case 0x0C:
                return;
            default:
                goto bad;
        }
    }

bad:
    /* See the read-side note: open-bus, Beetle parity. */
    {
        extern uint64_t g_io_openbus_writes;
        g_io_openbus_writes++;
    }
}

void dma_write(uint32_t addr, uint32_t val) {
    dma_write_masked(addr, val, 0xFFFFFFFFu);
}

uint64_t dma_debug_get_trace(const DMATraceEntry** out_entries) {
    if (out_entries) *out_entries = dma_trace;
    return dma_trace_seq;
}

void dma_debug_clear_trace(void) {
    memset(dma_trace, 0, sizeof(dma_trace));
    dma_trace_seq = 0;
}

uint64_t dma_debug_get_cdrom_history(const DMACDROMHistoryEntry** out_entries) {
    if (out_entries) *out_entries = cdrom_dma_history;
    return cdrom_dma_history_seq;
}

void dma_debug_clear_cdrom_history(void) {
    memset(cdrom_dma_history, 0, sizeof(cdrom_dma_history));
    memset(&cdrom_dma_active_entry, 0, sizeof(cdrom_dma_active_entry));
    cdrom_dma_history_seq = 0;
    cdrom_dma_history_active = 0;
}

void dma_debug_get_state(DMADebugState* out) {
    if (!out) return;
    out->dpcr = dpcr;
    out->dicr = dma_get_dicr();
    for (int i = 0; i < 7; i++) {
        out->channels[i].madr = channels[i].madr;
        out->channels[i].bcr = channels[i].bcr;
        out->channels[i].chcr = channels[i].chcr;
        out->channels[i].active =
            ((i < 2) ? mdec_async[i].active : 0) ||
            ((i == 3) ? cdrom_async.active : 0) ||
            delayed_complete[i].active;
        out->channels[i].remaining_words =
            (i < 2 && mdec_async[i].active) ? mdec_async[i].remaining_words :
            (i == 3 && cdrom_async.active) ? cdrom_async.remaining_words :
            delayed_complete[i].total_words;
        out->channels[i].cycles_accum =
            (i < 2 && mdec_async[i].active) ? mdec_async[i].cycles_accum :
            (i == 3 && cdrom_async.active) ? cdrom_async.cycles_accum :
            delayed_complete[i].cycles_remaining;
    }
}

/* ---- boot snapshot: complete DMA hardware state (LE field wire; see boot_state.h) ---- */
#include "pst_wire.h"

/* DMAChannel = 3×u32 (no pad). Async/delayed structs have host padding — field LE. */
#define DMA_ASYNC_WIRE (1u + 1u + 4u + 4u + 4u + 4u) /* 18 */
#define DMA_DELAY_WIRE (1u + 4u + 4u)                 /* 9 */
#define DMA_SNAP_WIRE_BYTES ( \
    (7u * 12u) + 4u + 4u + (2u * DMA_ASYNC_WIRE) + DMA_ASYNC_WIRE + (7u * DMA_DELAY_WIRE))

static int dma_w_async(PstW *w, const DMAAsyncChannel *a) {
    return pst_w_u8(w, a->active) && pst_w_u8(w, a->debug_started) &&
           pst_w_u32(w, a->total_words) && pst_w_u32(w, a->remaining_words) &&
           pst_w_u32(w, a->cycles_accum) && pst_w_u32(w, a->start_addr);
}
static int dma_r_async(PstR *r, DMAAsyncChannel *a) {
    return pst_r_u8(r, &a->active) && pst_r_u8(r, &a->debug_started) &&
           pst_r_u32(r, &a->total_words) && pst_r_u32(r, &a->remaining_words) &&
           pst_r_u32(r, &a->cycles_accum) && pst_r_u32(r, &a->start_addr);
}
static int dma_w_delay(PstW *w, const DMADelayedComplete *d) {
    return pst_w_u8(w, d->active) && pst_w_u32(w, d->total_words) &&
           pst_w_u32(w, d->cycles_remaining);
}
static int dma_r_delay(PstR *r, DMADelayedComplete *d) {
    return pst_r_u8(r, &d->active) && pst_r_u32(r, &d->total_words) &&
           pst_r_u32(r, &d->cycles_remaining);
}

uint32_t dma_snapshot_bytes(void) { return DMA_SNAP_WIRE_BYTES; }

void dma_snapshot_write(uint8_t *p) {
    PstW w;
    pst_w_init(&w, p, DMA_SNAP_WIRE_BYTES);
    for (int i = 0; i < 7; i++) {
        pst_w_u32(&w, channels[i].madr);
        pst_w_u32(&w, channels[i].bcr);
        pst_w_u32(&w, channels[i].chcr);
    }
    pst_w_u32(&w, dpcr);
    pst_w_u32(&w, dicr);
    dma_w_async(&w, &mdec_async[0]);
    dma_w_async(&w, &mdec_async[1]);
    dma_w_async(&w, &cdrom_async);
    for (int i = 0; i < 7; i++)
        dma_w_delay(&w, &delayed_complete[i]);
}

int dma_snapshot_read(const uint8_t *p, uint32_t len) {
    PstR r;
    if (len != DMA_SNAP_WIRE_BYTES) return 0;
    pst_r_init(&r, p, len);
    for (int i = 0; i < 7; i++) {
        if (!pst_r_u32(&r, &channels[i].madr) || !pst_r_u32(&r, &channels[i].bcr) ||
            !pst_r_u32(&r, &channels[i].chcr))
            return 0;
    }
    if (!pst_r_u32(&r, &dpcr) || !pst_r_u32(&r, &dicr)) return 0;
    if (!dma_r_async(&r, &mdec_async[0]) || !dma_r_async(&r, &mdec_async[1]) ||
        !dma_r_async(&r, &cdrom_async))
        return 0;
    for (int i = 0; i < 7; i++)
        if (!dma_r_delay(&r, &delayed_complete[i])) return 0;
    return 1;
}
