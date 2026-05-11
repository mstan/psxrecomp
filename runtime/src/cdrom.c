/*
 * cdrom.c — PS1 CD-ROM controller (adapted from psxrecomp-v3)
 *
 * Registers at 0x1F801800-0x1F801803 with index-based register banking.
 * Reference: nocash PSX specs — CDROM Controller section
 *
 * v4 adaptation: removed event_deliver (HLE), removed CPUState dependency,
 * removed hard-coded RAM writes, removed fprintf. IRQ delivery is via
 * i_stat bit 2 (IRQ_CDROM); the recompiled BIOS exception handler
 * processes events natively.
 */

#include "cdrom.h"
#include <string.h>
#include <stdlib.h>

/* C wrappers for the C++ ISOReader (defined in iso_reader_c.cpp) */
extern void* iso_open(const char* path);
extern int iso_read_sector(void* handle, uint32_t lba, uint8_t* buffer, int size);
extern void iso_close(void* handle);

/* I_STAT owned by memory.c — set bit 2 for CDROM IRQ */
extern uint32_t i_stat;
extern uint32_t g_debug_current_func_addr;
extern uint32_t g_debug_last_store_pc;
extern uint64_t s_frame_count;

/* CD-ROM state */
static uint8_t index_reg;
static uint8_t stat_reg;
static uint8_t irq_enable;
static uint8_t irq_flag;

/* Parameter FIFO */
#define PARAM_FIFO_SIZE 16
static uint8_t param_fifo[PARAM_FIFO_SIZE];
static int param_count;

/* Response FIFO */
#define RESPONSE_FIFO_SIZE 16
static uint8_t response_fifo[RESPONSE_FIFO_SIZE];
static int response_read;
static int response_count;

/* Data buffer. PSY-Q's whole-sector mode reads the 12-byte sector
 * header separately, then the 2048-byte user payload. */
#define SECTOR_SIZE 2048
#define SECTOR_HEADER_SIZE 12
#define SECTOR_BUFFER_SIZE (SECTOR_HEADER_SIZE + SECTOR_SIZE)
static uint8_t sector_buffer[SECTOR_BUFFER_SIZE];
static int sector_read_pos;
static int sector_available;
static int sector_size;

/* Seek target */
static uint8_t seek_min, seek_sec, seek_sect;

/* Read state */
static int reading;
static int read_min, read_sec, read_sect;
static uint8_t mode_reg;

static int sector_delay_cycles(void) {
    /* PS1 CPU is 33.8688 MHz. CD-ROM sectors arrive at 75 Hz in 1x
     * mode, or twice that rate when SetMode bit 7 enables double speed. */
    return (mode_reg & 0x80) ? 225792 : 451584;
}

/* Pending command */
typedef struct {
    uint8_t cmd;
    int pending;
    int delay;
    int phase;
} PendingCmd;
static PendingCmd pending;

/* ISO reader */
static void* iso_handle = NULL;

static CDROMTraceEntry cdrom_trace[CDROM_TRACE_CAP];
static uint64_t cdrom_trace_seq;

static void trace_cdrom(uint8_t kind, uint32_t addr, uint32_t val, uint8_t width) {
    CDROMTraceEntry *e = &cdrom_trace[cdrom_trace_seq % CDROM_TRACE_CAP];
    e->seq = cdrom_trace_seq++;
    e->kind = kind;
    e->addr = addr;
    e->val = val;
    e->width = width;
    e->func = g_debug_current_func_addr;
    e->pc = g_debug_last_store_pc;
    e->frame = (uint32_t)s_frame_count;
    e->i_stat = i_stat;
    e->index_reg = index_reg;
    e->stat_reg = stat_reg;
    e->irq_enable = irq_enable;
    e->irq_flag = irq_flag;
    e->param_count = (uint8_t)param_count;
    e->response_read = (uint8_t)response_read;
    e->response_count = (uint8_t)response_count;
    e->sector_available = (uint8_t)sector_available;
    e->sector_read_pos = sector_read_pos;
    e->pending_cmd = pending.cmd;
    e->pending_pending = (uint8_t)pending.pending;
    e->pending_delay = pending.delay;
    e->reading = (uint8_t)reading;
}

static int has_disc(void) {
    return iso_handle != NULL;
}

/* CD status bits */
#define CDSTAT_ERROR    0x01
#define CDSTAT_MOTOR    0x02
#define CDSTAT_SEEKERR  0x04
#define CDSTAT_IDERROR  0x08
#define CDSTAT_SHELL    0x10
#define CDSTAT_READ     0x20
#define CDSTAT_SEEK     0x40
#define CDSTAT_PLAY     0x80

/* IRQ types */
#define CDIRQ_DATA_READY  1
#define CDIRQ_COMPLETE    2
#define CDIRQ_ACK         3
#define CDIRQ_DATA_END    4
#define CDIRQ_ERROR       5

static void response_clear(void) {
    response_read = 0;
    response_count = 0;
}

static void response_push(uint8_t val) {
    if (response_count < RESPONSE_FIFO_SIZE) {
        response_fifo[response_count++] = val;
    }
}

static void set_irq(int type) {
    irq_flag = (uint8_t)type;
    trace_cdrom('I', 0, (uint32_t)type, 0);
}

/* Fire CDROM IRQ into the interrupt controller */
static void fire_cdrom_irq(void) {
    if (irq_flag && (irq_enable & (1 << (irq_flag - 1)))) {
        i_stat |= (1u << 2); /* IRQ_CDROM */
        trace_cdrom('F', 0, irq_flag, 0);
    } else {
        trace_cdrom('f', 0, irq_flag, 0);
    }
}

static int bcd_to_bin(uint8_t bcd) {
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}

static int msf_to_lba(int m, int s, int f) {
    return (m * 60 + s) * 75 + f - 150;
}

static uint8_t bin_to_bcd(int val) {
    return (uint8_t)(((val / 10) << 4) | (val % 10));
}

static void read_sector_at(int min, int sec, int sect) {
    int lba = msf_to_lba(min, sec, sect);
    uint8_t user_data[SECTOR_SIZE];

    if (iso_handle) {
        if (!iso_read_sector(iso_handle, lba, user_data, SECTOR_SIZE)) {
            memset(user_data, 0, sizeof(user_data));
        }
    } else {
        memset(user_data, 0, sizeof(user_data));
    }

    memset(sector_buffer, 0, sizeof(sector_buffer));
    if (mode_reg & 0x20) {
        sector_buffer[0] = bin_to_bcd(min);
        sector_buffer[1] = bin_to_bcd(sec);
        sector_buffer[2] = bin_to_bcd(sect);
        sector_buffer[3] = 0x02; /* Mode 2 sector. */
        memcpy(sector_buffer + SECTOR_HEADER_SIZE, user_data, SECTOR_SIZE);
        sector_size = SECTOR_HEADER_SIZE + SECTOR_SIZE;
    } else {
        memcpy(sector_buffer, user_data, SECTOR_SIZE);
        sector_size = SECTOR_SIZE;
    }

    sector_read_pos = 0;
    sector_available = 1;
    trace_cdrom('S', 0, (uint32_t)lba, 0);
}

static void advance_msf(int* m, int* s, int* f) {
    (*f)++;
    if (*f >= 75) { *f = 0; (*s)++; }
    if (*s >= 60) { *s = 0; (*m)++; }
}

static void exec_command(uint8_t cmd) {
    trace_cdrom('C', 0, cmd, 0);
    response_clear();

    switch (cmd) {
    case 0x01: /* GetStat */
        if (has_disc()) {
            stat_reg |= CDSTAT_MOTOR;
        } else {
            stat_reg |= CDSTAT_SHELL;
        }
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        break;

    case 0x02: /* SetLoc */
        if (param_count >= 3) {
            seek_min = bcd_to_bin(param_fifo[0]);
            seek_sec = bcd_to_bin(param_fifo[1]);
            seek_sect = bcd_to_bin(param_fifo[2]);
        }
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        break;

    case 0x06: /* ReadN */
        if (!has_disc()) {
            response_push(stat_reg | CDSTAT_ERROR);
            set_irq(CDIRQ_ERROR);
            break;
        }
        read_min = seek_min;
        read_sec = seek_sec;
        read_sect = seek_sect;
        reading = 1;
        stat_reg |= CDSTAT_READ;
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        pending.cmd = 0x06;
        pending.pending = 1;
        pending.delay = sector_delay_cycles();
        pending.phase = 1;
        break;

    case 0x09: /* Pause */
        reading = 0;
        stat_reg &= ~CDSTAT_READ;
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        pending.cmd = 0x09;
        pending.pending = 1;
        pending.delay = 10000;
        pending.phase = 1;
        break;

    case 0x0A: /* Init */
        reading = 0;
        stat_reg = has_disc() ? CDSTAT_MOTOR : CDSTAT_SHELL;
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        pending.cmd = 0x0A;
        pending.pending = 1;
        pending.delay = 50000;
        pending.phase = 1;
        break;

    case 0x0C: /* Demute */
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        break;

    case 0x0E: /* SetMode */
        if (param_count >= 1) {
            mode_reg = param_fifo[0];
        }
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        break;

    case 0x15: /* SeekL */
        if (!has_disc()) {
            response_push(stat_reg | CDSTAT_ERROR | CDSTAT_SEEKERR);
            set_irq(CDIRQ_ERROR);
            break;
        }
        stat_reg |= CDSTAT_SEEK;
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        pending.cmd = 0x15;
        pending.pending = 1;
        pending.delay = 20000;
        pending.phase = 1;
        break;

    case 0x1A: /* GetID */
        /* GetID always sends INT3 (ACK) first, then a pending
         * second response: INT2 (COMPLETE) with disc ID if present,
         * or INT5 (ERROR) if no disc / lid open. */
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        pending.cmd = 0x1A;
        pending.pending = 1;
        pending.delay = 30000;
        pending.phase = 1;
        break;

    case 0x1B: /* ReadS */
        if (!has_disc()) {
            response_push(stat_reg | CDSTAT_ERROR);
            set_irq(CDIRQ_ERROR);
            break;
        }
        read_min = seek_min;
        read_sec = seek_sec;
        read_sect = seek_sect;
        reading = 1;
        stat_reg |= CDSTAT_READ;
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        pending.cmd = 0x1B;
        pending.pending = 1;
        pending.delay = sector_delay_cycles();
        pending.phase = 1;
        break;

    case 0x1E: /* ReadTOC */
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        pending.cmd = 0x1E;
        pending.pending = 1;
        pending.delay = 100000;
        pending.phase = 1;
        break;

    case 0x19: /* Test */
        if (param_count >= 1 && param_fifo[0] == 0x20) {
            response_push(0x97);
            response_push(0x01);
            response_push(0x10);
            response_push(0xC2);
            set_irq(CDIRQ_ACK);
        } else {
            response_push(stat_reg);
            set_irq(CDIRQ_ACK);
        }
        break;

    default:
        response_push(stat_reg | CDSTAT_ERROR);
        set_irq(CDIRQ_ERROR);
        break;
    }

    param_count = 0;

    /* Fire the CDROM IRQ for the immediate response.
     * Delayed responses (pending) fire from process_pending(). */
    fire_cdrom_irq();
}

static void process_pending(uint32_t cycles) {
    if (!pending.pending) return;

    if (cycles == 0) return;
    pending.delay -= (int)cycles;
    if (pending.delay > 0) return;

    pending.pending = 0;
    response_clear();

    switch (pending.cmd) {
    case 0x06: /* ReadN data ready */
    case 0x1B: /* ReadS data ready */
        if (reading) {
            read_sector_at(read_min, read_sec, read_sect);
            advance_msf(&read_min, &read_sec, &read_sect);
            response_push(stat_reg);
            set_irq(CDIRQ_DATA_READY);
            fire_cdrom_irq();
        }
        break;

    case 0x09: /* Pause complete */
        stat_reg &= ~CDSTAT_READ;
        response_push(stat_reg);
        set_irq(CDIRQ_COMPLETE);
        fire_cdrom_irq();
        break;

    case 0x0A: /* Init complete */
        response_push(stat_reg);
        set_irq(CDIRQ_COMPLETE);
        fire_cdrom_irq();
        break;

    case 0x15: /* SeekL complete */
        stat_reg &= ~CDSTAT_SEEK;
        response_push(stat_reg);
        set_irq(CDIRQ_COMPLETE);
        fire_cdrom_irq();
        break;

    case 0x1A: { /* GetID result */
        if (!has_disc()) {
            response_push(stat_reg | CDSTAT_IDERROR);
            response_push(0x80);
            set_irq(CDIRQ_ERROR);
        } else {
            response_push(stat_reg);
            response_push(0x00);
            response_push(0x20);
            response_push(0x00);
            response_push('S');
            response_push('C');
            response_push('E');
            response_push('I');
            set_irq(CDIRQ_COMPLETE);
        }
        fire_cdrom_irq();
        break;
    }

    case 0x1E: /* ReadTOC complete */
        response_push(stat_reg);
        set_irq(CDIRQ_COMPLETE);
        fire_cdrom_irq();
        break;

    default:
        break;
    }
}

void cdrom_init(const char* cue_path) {
    memset(param_fifo, 0, sizeof(param_fifo));
    memset(response_fifo, 0, sizeof(response_fifo));
    memset(sector_buffer, 0, sizeof(sector_buffer));

    index_reg = 0;
    irq_enable = 0x1F;
    irq_flag = 0;
    param_count = 0;
    response_read = 0;
    response_count = 0;
    sector_read_pos = 0;
    sector_size = 0;
    sector_available = 0;
    reading = 0;
    mode_reg = 0;
    pending.pending = 0;
    seek_min = seek_sec = seek_sect = 0;

    if (cue_path) {
        iso_handle = iso_open(cue_path);
    }

    stat_reg = has_disc() ? CDSTAT_MOTOR : CDSTAT_SHELL;
    cdrom_debug_clear_trace();
    trace_cdrom('N', 0, has_disc() ? 1u : 0u, 0);
}

uint32_t cdrom_read(uint32_t addr) {
    uint32_t ret = 0;
    switch (addr) {
    case 0x1F801800: {
        uint8_t s = index_reg & 0x03;
        s |= (1 << 2); /* ADPCM empty */
        if (param_count == 0) s |= (1 << 3);
        if (param_count < PARAM_FIFO_SIZE) s |= (1 << 4);
        if (response_read < response_count) s |= (1 << 5);
        if (sector_available) s |= (1 << 6);
        ret = s;
        break;
    }

    case 0x1F801801:
        if (response_read < response_count) {
            ret = response_fifo[response_read++];
        }
        break;

    case 0x1F801802:
        if (sector_available && sector_read_pos < sector_size) {
            ret = sector_buffer[sector_read_pos++];
            if (sector_read_pos >= sector_size) {
                sector_available = 0;
            }
        }
        break;

    case 0x1F801803:
        if (index_reg == 0 || index_reg == 2) {
            ret = irq_enable;
        } else {
            ret = irq_flag | 0xE0;
        }
        break;

    default:
        ret = 0;
        break;
    }
    trace_cdrom('R', addr, ret, 1);
    return ret;
}

void cdrom_write(uint32_t addr, uint32_t value) {
    uint8_t val = (uint8_t)value;
    trace_cdrom('W', addr, val, 1);

    switch (addr) {
    case 0x1F801800:
        index_reg = val & 0x03;
        break;

    case 0x1F801801:
        if (index_reg == 0) {
            exec_command(val);
            fire_cdrom_irq();
        }
        break;

    case 0x1F801802:
        if (index_reg == 0) {
            if (param_count < PARAM_FIFO_SIZE) {
                param_fifo[param_count++] = val;
            }
        } else if (index_reg == 1) {
            irq_enable = val & 0x1F;
        }
        break;

    case 0x1F801803:
        if (index_reg == 1) {
            irq_flag &= ~(val & 0x1F);
            if (val & 0x40) {
                param_count = 0;
            }
        }
        break;

    default:
        break;
    }
}

void cdrom_advance(uint32_t cycles) {
    process_pending(cycles);

    if (reading && !sector_available && !pending.pending) {
        pending.cmd = 0x06;
        pending.pending = 1;
        pending.delay = sector_delay_cycles();
        pending.phase = 1;
    }
}

void cdrom_tick(void) {
    cdrom_advance(33868u);
}

uint32_t cdrom_dma_read(void) {
    uint32_t val = 0;
    if (sector_available && sector_read_pos + 4 <= sector_size) {
        memcpy(&val, sector_buffer + sector_read_pos, 4);
        sector_read_pos += 4;
        if (sector_read_pos >= sector_size) {
            sector_available = 0;
        }
    }
    trace_cdrom('D', 0, val, 4);
    return val;
}

int cdrom_dma_ready(void) {
    return sector_available && (sector_read_pos < SECTOR_SIZE);
}

void cdrom_debug_snapshot(CDROMDebugState* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->seq = cdrom_trace_seq;
    out->index_reg = index_reg;
    out->stat_reg = stat_reg;
    out->irq_enable = irq_enable;
    out->irq_flag = irq_flag;
    out->seek_min = seek_min;
    out->seek_sec = seek_sec;
    out->seek_sect = seek_sect;
    out->pending_cmd = pending.cmd;
    out->has_disc = has_disc();
    out->param_count = param_count;
    out->response_read = response_read;
    out->response_count = response_count;
    out->sector_read_pos = sector_read_pos;
    out->sector_available = sector_available;
    out->reading = reading;
    out->read_min = read_min;
    out->read_sec = read_sec;
    out->read_sect = read_sect;
    out->pending_pending = pending.pending;
    out->pending_delay = pending.delay;
    out->pending_phase = pending.phase;
    out->i_stat = i_stat;
}

uint64_t cdrom_debug_get_trace(const CDROMTraceEntry** out_entries) {
    if (out_entries) *out_entries = cdrom_trace;
    return cdrom_trace_seq;
}

void cdrom_debug_clear_trace(void) {
    memset(cdrom_trace, 0, sizeof(cdrom_trace));
    cdrom_trace_seq = 0;
}
