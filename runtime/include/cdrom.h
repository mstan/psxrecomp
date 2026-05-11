#ifndef PSXRECOMP_V4_CDROM_H
#define PSXRECOMP_V4_CDROM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void cdrom_init(const char* cue_path);

/* MMIO read/write (0x1F801800-0x1F801803) */
uint32_t cdrom_read(uint32_t addr);
void cdrom_write(uint32_t addr, uint32_t value);

/* Advance CD-ROM state machine by guest CPU cycles. Sets i_stat IRQ_CDROM
 * when a command completes or data is ready. */
void cdrom_advance(uint32_t cycles);

/* Legacy frame-paced entry point for interpreter/oracle builds. */
void cdrom_tick(void);

/* DMA channel 3 interface */
uint32_t cdrom_dma_read(void);
int cdrom_dma_ready(void);

typedef struct CDROMDebugState {
    uint64_t seq;
    uint8_t index_reg;
    uint8_t stat_reg;
    uint8_t irq_enable;
    uint8_t irq_flag;
    uint8_t seek_min;
    uint8_t seek_sec;
    uint8_t seek_sect;
    uint8_t pending_cmd;
    int has_disc;
    int param_count;
    int response_read;
    int response_count;
    int sector_read_pos;
    int sector_available;
    int reading;
    int read_min;
    int read_sec;
    int read_sect;
    int pending_pending;
    int pending_delay;
    int pending_phase;
    uint32_t i_stat;
} CDROMDebugState;

typedef struct CDROMTraceEntry {
    uint64_t seq;
    uint32_t addr;
    uint32_t val;
    uint32_t func;
    uint32_t pc;
    uint32_t frame;
    uint32_t i_stat;
    uint8_t kind;
    uint8_t width;
    uint8_t index_reg;
    uint8_t stat_reg;
    uint8_t irq_enable;
    uint8_t irq_flag;
    uint8_t param_count;
    uint8_t response_read;
    uint8_t response_count;
    uint8_t sector_available;
    uint8_t pending_cmd;
    uint8_t pending_pending;
    uint8_t reading;
    uint8_t pad;
    int sector_read_pos;
    int pending_delay;
} CDROMTraceEntry;

#define CDROM_TRACE_CAP (1 << 16)

void cdrom_debug_snapshot(CDROMDebugState* out);
uint64_t cdrom_debug_get_trace(const CDROMTraceEntry** out_entries);
void cdrom_debug_clear_trace(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_V4_CDROM_H */
