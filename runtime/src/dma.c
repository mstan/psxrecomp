/* dma.c — PS1 DMA controller simulation (Phase 3).
 *
 * Implements all 7 channel register reads/writes, DPCR, DICR,
 * and transfer execution for:
 *   - Ch2 (GPU): block mode and linked-list mode
 *   - Ch6 (OTC): ordering table clear
 *
 * Transfer is executed synchronously when CHCR start bit is written.
 * Reference: nocash PSX specs, DuckStation src/core/dma.cpp
 */

#include "dma.h"
#include "cdrom.h"
#include "dirty_ram_interp.h"
#include "gpu.h"
#include "mdec.h"
#include "spu.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Memory access — defined in memory.c */
extern uint32_t psx_read_word(uint32_t addr);
extern void     psx_write_word(uint32_t addr, uint32_t val);

/* Interrupt status — defined in memory.c */
extern uint32_t i_stat;

/* ---- Per-channel registers ---- */

typedef struct {
    uint32_t madr;  /* +0x00: Memory address */
    uint32_t bcr;   /* +0x04: Block control */
    uint32_t chcr;  /* +0x08: Channel control */
} DMAChannel;

static DMAChannel channels[7];

/* ---- Global registers ---- */

static uint32_t dpcr;  /* 0x1F8010F0: DMA control (enable bits) */
static uint32_t dicr;  /* 0x1F8010F4: DMA interrupt control */

/* ---- Helpers ---- */

static uint32_t prev_master_flag;  /* for edge detection */

static void update_master_irq(void) {
    /* DICR bit 31 (master IRQ flag) is read-only, calculated as:
     * bit31 = bit15 OR (bit23 AND ((bits16-22 AND bits24-30) != 0))
     *
     * We store bits 0-30 in dicr and compute bit 31 on read. */
    uint32_t force_irq = (dicr >> 15) & 1;
    uint32_t master_enable = (dicr >> 23) & 1;
    uint32_t enable_bits = (dicr >> 16) & 0x7F;
    uint32_t flag_bits = (dicr >> 24) & 0x7F;

    uint32_t master_flag = force_irq | (master_enable & ((enable_bits & flag_bits) != 0));

    dicr = (dicr & 0x7FFFFFFFu) | (master_flag << 31);

    /* I_STAT bit 3 is set by direct assertion in transfer completion
     * code (not through edge detection here).  The BIOS shell handler
     * only acknowledges ch3 in DICR, leaving ch2/ch6 flags high — so
     * edge detection on master_flag would fire once and then be stuck.
     * Direct assertion + handler AND-acknowledge gives exactly one IRQ
     * per DMA completion, matching real hardware behavior. */
    prev_master_flag = master_flag;
}

static int channel_enabled(int ch) {
    /* DPCR: each channel has 4 bits, bit 3 of each group is enable.
     * Ch0 = bits 0-3, Ch1 = bits 4-7, etc. Enable = bit (ch*4 + 3). */
    return (dpcr >> (ch * 4 + 3)) & 1;
}

static uint32_t transfer_word_count(int ch) {
    uint32_t chcr = channels[ch].chcr;
    uint32_t sync_mode = (chcr >> 9) & 3;
    uint32_t block_size = channels[ch].bcr & 0xFFFFu;
    uint32_t block_count = (channels[ch].bcr >> 16) & 0xFFFFu;

    if (sync_mode == 1) {
        if (block_size == 0) block_size = 0x10000u;
        if (block_count == 0) block_count = 1u;
        return block_size * block_count;
    }

    if (block_size == 0) block_size = 0x10000u;
    return block_size;
}

static void complete_transfer(int ch) {
    channels[ch].chcr &= ~((1u << 24) | (1u << 28));
    dicr |= (1u << (24 + ch));
    update_master_irq();
    i_stat |= (1u << 3);
}

/* ---- Transfer execution ---- */

static void execute_ch0_mdec_in(void) {
    uint32_t chcr = channels[0].chcr;
    uint32_t direction = chcr & 1;           /* 1=from RAM to MDEC */
    uint32_t step = (chcr >> 1) & 1;
    uint32_t total_words = transfer_word_count(0);
    uint32_t addr = channels[0].madr & 0x1FFFFCu;
    int32_t addr_step = step ? -4 : 4;

    if (direction != 0) {
        for (uint32_t i = 0; i < total_words; i++) {
            mdec_dma_write_word(psx_read_word(addr));
            addr = (addr + addr_step) & 0x1FFFFCu;
        }
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
        for (uint32_t i = 0; i < total_words; i++) {
            psx_write_word(addr, mdec_dma_read_word());
            addr = (addr + addr_step) & 0x1FFFFCu;
        }
        channels[1].madr = addr;
    }

    complete_transfer(1);
}

static void execute_ch2_gpu(void) {
    uint32_t chcr = channels[2].chcr;
    uint32_t direction = chcr & 1;           /* 0=to RAM, 1=from RAM (to device) */
    uint32_t step = (chcr >> 1) & 1;         /* 0=forward(+4), 1=backward(-4) */
    uint32_t sync_mode = (chcr >> 9) & 3;    /* 0=burst, 1=block, 2=linked-list */

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
        }
        /* Transfer complete: clear start/busy bits, post IRQ directly. */
        channels[2].chcr &= ~((1u << 24) | (1u << 28));
        dicr |= (1u << (24 + 2));
        update_master_irq();
        i_stat |= (1u << 3);
        return;
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
            gpu_write_gp0(word);
            addr = (addr + addr_step) & 0x1FFFFCu;
        }
        channels[2].madr = addr;
    } else if (sync_mode == 2) {
        /* Linked-list mode: follow ordering table in RAM.
         * Each node: bits 24-31 = number of words following header,
         *            bits 0-23  = next node address (0xFFFFFF = end).
         * The words following the header are sent to GP0. */
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

            for (uint32_t i = 0; i < num_words; i++) {
                uint32_t word = psx_read_word(word_addr);
                gpu_write_gp0(word);
                word_addr = (word_addr + 4) & 0x1FFFFCu;
            }

            /* Next node */
            uint32_t next = header & 0xFFFFFFu;
            if (next == 0xFFFFFFu) break; /* end of list */
            addr = next & 0x1FFFFCu;
        }
    } else {
        /* Burst mode (sync_mode == 0) */
        uint32_t word_count = channels[2].bcr & 0xFFFF;
        if (word_count == 0) word_count = 0x10000; /* 0 means 0x10000 */
        uint32_t addr = channels[2].madr & 0x1FFFFCu;
        int32_t  addr_step = step ? -4 : 4;

        for (uint32_t i = 0; i < word_count; i++) {
            uint32_t word = psx_read_word(addr);
            gpu_write_gp0(word);
            addr = (addr + addr_step) & 0x1FFFFCu;
        }
        channels[2].madr = addr;
    }

    /* Transfer complete: clear start/busy/trigger bits, post IRQ directly. */
    channels[2].chcr &= ~((1u << 24) | (1u << 28));
    dicr |= (1u << (24 + 2));
    update_master_irq();
    i_stat |= (1u << 3);
}

static void execute_ch3_cdrom(void) {
    uint32_t chcr = channels[3].chcr;
    uint32_t direction = chcr & 1;           /* 0=to RAM, 1=from RAM */
    uint32_t step = (chcr >> 1) & 1;         /* 0=forward(+4), 1=backward(-4) */
    uint32_t sync_mode = (chcr >> 9) & 3;

    if (direction != 0) {
        channels[3].chcr &= ~((1u << 24) | (1u << 28));
        return;
    }

    uint32_t block_size = channels[3].bcr & 0xFFFF;
    uint32_t block_count = (channels[3].bcr >> 16) & 0xFFFF;
    uint32_t total_words;
    if (sync_mode == 1) {
        if (block_size == 0) block_size = 0x10000;
        if (block_count == 0) block_count = 1;
        total_words = block_size * block_count;
    } else {
        total_words = block_size;
        if (total_words == 0) total_words = 0x10000;
    }

    uint32_t addr = channels[3].madr & 0x1FFFFCu;
    int32_t addr_step = step ? -4 : 4;
    for (uint32_t i = 0; i < total_words; i++) {
        uint32_t word = cdrom_dma_read();
        psx_write_word(addr, word);
        dirty_ram_mark_executable_range(addr, 4);
        addr = (addr + addr_step) & 0x1FFFFCu;
    }

    channels[3].madr = addr;
    channels[3].chcr &= ~((1u << 24) | (1u << 28));
    dicr |= (1u << (24 + 3));
    update_master_irq();
    i_stat |= (1u << 3);
}

static void execute_ch4_spu(void) {
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
    } else {
        for (uint32_t i = 0; i < total_words; i++) {
            psx_write_word(addr, 0);
            addr = (addr + addr_step) & 0x1FFFFCu;
        }
    }

    channels[4].madr = addr;
    complete_transfer(4);
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

    /* Transfer complete, post IRQ directly. */
    channels[6].chcr &= ~((1u << 24) | (1u << 28));
    dicr |= (1u << (24 + 6));
    update_master_irq();
    i_stat |= (1u << 3);
}

static void try_execute(int ch) {
    uint32_t chcr = channels[ch].chcr;

    /* Transfer starts when bit 24 (start/busy) is set AND channel is enabled in DPCR */
    if (!((chcr >> 24) & 1)) return;
    if (!channel_enabled(ch)) return;

    switch (ch) {
        case 0:
            execute_ch0_mdec_in();
            break;
        case 1:
            execute_ch1_mdec_out();
            break;
        case 2:
            execute_ch2_gpu();
            break;
        case 3:
            execute_ch3_cdrom();
            break;
        case 4:
            execute_ch4_spu();
            break;
        case 6:
            execute_ch6_otc();
            break;
        default:
            /* Other channels not implemented yet — fatal if transfer is triggered */
            fprintf(stderr, "DMA ch%d: transfer triggered but not implemented (CHCR=0x%08X)\n", ch, chcr);
            fflush(stderr);
            exit(1);
    }
}

/* ---- Public interface ---- */

uint32_t dma_get_dicr(void) { update_master_irq(); return dicr; }
uint32_t dma_get_dpcr(void) { return dpcr; }

void dma_init(void) {
    memset(channels, 0, sizeof(channels));
    dpcr = 0x07654321u; /* default: priorities set, no channels enabled */
    dicr = 0;
    prev_master_flag = 0;
}

uint32_t dma_read(uint32_t addr) {
    /* DPCR */
    if (addr == 0x1F8010F0u) return dpcr;
    /* DICR */
    if (addr == 0x1F8010F4u) {
        update_master_irq();
        return dicr;
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
            default: goto bad;
        }
    }

bad:
    fprintf(stderr, "DMA read from unknown address 0x%08X\n", addr);
    fflush(stderr);
    exit(1);
    return 0;
}

void dma_write(uint32_t addr, uint32_t val) {
    /* DPCR */
    if (addr == 0x1F8010F0u) {
        dpcr = val;
        return;
    }
    /* DICR: bits 24-30 are write-1-to-acknowledge (clear), rest are writable */
    if (addr == 0x1F8010F4u) {
        /* Bits 0-14: unused / force-IRQ bits — writable */
        /* Bit 15: force IRQ — writable */
        /* Bits 16-23: enable flags — writable */
        /* Bits 24-30: IRQ flags — write 1 to CLEAR */
        /* Bit 31: master flag — read-only (computed) */
        uint32_t ack_bits = val & 0x7F000000u;  /* bits to clear */
        uint32_t write_bits = val & 0x00FFFFFFu; /* bits to set */
        dicr = (write_bits) | (dicr & 0x7F000000u & ~ack_bits);
        update_master_irq();
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
                channels[ch].madr = val;
                return;
            case 0x04:
                channels[ch].bcr = val;
                return;
            case 0x08:
                channels[ch].chcr = val;
                /* Writing CHCR with start bit set triggers transfer */
                if ((val >> 24) & 1) {
                    try_execute(ch);
                }
                return;
            default:
                goto bad;
        }
    }

bad:
    fprintf(stderr, "DMA write to unknown address 0x%08X = 0x%08X\n", addr, val);
    fflush(stderr);
    exit(1);
}
