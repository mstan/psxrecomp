/* memory.c — Phase 2 PS1 memory system.
 *
 * Physical address routing:
 *   0x00000000..0x001FFFFF — 2 MB RAM
 *   0x1F800000..0x1F8003FF — 1 KB scratchpad
 *   0x1F801000..0x1F803FFF — MMIO (fatal abort)
 *   0x1FC00000..0x1FC7FFFF — 512 KB BIOS ROM (read-only)
 *   Everything else         — fatal abort (unmapped)
 */

#include "cpu_state.h"
#include "cdrom.h"
#include "dma.h"
#include "gpu.h"
#include "sio.h"
#include "spu.h"
#include "timers.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE        (2 * 1024 * 1024)
#define SCRATCHPAD_SIZE 1024
#define BIOS_ROM_SIZE   (512 * 1024)

static uint8_t ram[RAM_SIZE];
static uint8_t scratchpad[SCRATCHPAD_SIZE];
static uint8_t bios_rom[BIOS_ROM_SIZE];

/* Memory control registers: 0x1F801000..0x1F80103F (16 words) + 0x1F801060 (RAM size).
 * Includes expansion base/size, COM_DELAY, SPU_DELAY, CDROM_DELAY etc. */
static uint32_t mem_ctrl[16];   /* indices 0..15 → addresses 0x1F801000..0x1F80103C */
static uint32_t ram_size_reg;   /* 0x1F801060 */

/* Cache control register (KSEG2: 0xFFFE0130). */
static uint32_t cache_ctrl;

/* Pointer to cpu->cop0[12] (SR).  Set once at init.
 * Used by write functions to check the IsC (Isolate Cache) bit.
 * When IsC is set, RAM/scratchpad writes are silently dropped — the
 * real R3000A sends them to the data cache only. */
static const uint32_t *sr_ptr;

/* Interrupt controller — non-static so hardware subsystems can set I_STAT bits. */
uint32_t i_stat;  /* 0x1F801070 — interrupt status (AND-acknowledge semantics) */
uint32_t i_mask;  /* 0x1F801074 — interrupt enable mask */

/* Tier 1 write-trace hooks (implemented in debug_server.c). */
extern void debug_server_trace_write_check(uint32_t phys, uint32_t old_val,
                                           uint32_t new_val, uint8_t width);
extern void debug_server_trace_mmio_write(uint32_t addr, uint32_t val, uint8_t width);

static inline uint32_t read_ram_word(uint32_t phys) {
    return  (uint32_t)ram[phys]
         | ((uint32_t)ram[phys + 1] << 8)
         | ((uint32_t)ram[phys + 2] << 16)
         | ((uint32_t)ram[phys + 3] << 24);
}
static inline uint16_t read_ram_half(uint32_t phys) {
    return (uint16_t)ram[phys] | ((uint16_t)ram[phys + 1] << 8);
}

/* SPU registers are now handled by spu.c */

void memory_set_sr_ptr(const uint32_t *p) { sr_ptr = p; }

void memory_init(const char* bios_path) {
    memset(ram, 0, sizeof(ram));
    memset(scratchpad, 0, sizeof(scratchpad));

    FILE* f = fopen(bios_path, "rb");
    if (!f) {
        fprintf(stderr, "FATAL: cannot open BIOS file: %s\n", bios_path);
        exit(1);
    }
    size_t n = fread(bios_rom, 1, BIOS_ROM_SIZE, f);
    fclose(f);
    if (n != BIOS_ROM_SIZE) {
        fprintf(stderr, "FATAL: BIOS file %s is %zu bytes (expected %d)\n",
                bios_path, n, BIOS_ROM_SIZE);
        exit(1);
    }
}

static void mmio_fatal(uint32_t vaddr, uint32_t phys, const char* op) {
    fprintf(stderr, "MMIO %s @ 0x%08X (phys 0x%08X)\n", op, vaddr, phys);
    fflush(stderr);
    FILE* cf = fopen("psx_crash.txt", "w");
    if (cf) { fprintf(cf, "MMIO %s @ 0x%08X (phys 0x%08X)\n", op, vaddr, phys); fclose(cf); }
    exit(1);
}

static void mmio_unimplemented(uint32_t addr, const char* op) {
    fprintf(stderr, "UNIMPLEMENTED MMIO %s @ 0x%08X\n", op, addr);
    fflush(stderr);
    /* Also write to a crash file for capture when stderr is lost. */
    FILE* cf = fopen("psx_crash.txt", "w");
    if (cf) {
        fprintf(cf, "UNIMPLEMENTED MMIO %s @ 0x%08X\n", op, addr);
        fclose(cf);
    }
    exit(1);
}

static void unmapped_fatal(uint32_t vaddr, uint32_t phys, const char* op) {
    /* On real PS1 hardware, reads from unmapped addresses return open bus
     * (typically the last value on the data bus, or 0xFFFFFFFF).
     * The BIOS intentionally probes unmapped regions (RAM size detection,
     * expansion hardware detection). Fatal abort would prevent normal boot. */
    (void)vaddr; (void)phys; (void)op;
}

/* --- MMIO read/write helpers --- */

static uint32_t mmio_read32(uint32_t addr) {
    /* Memory control: 0x1F801000..0x1F801020 */
    if (addr >= 0x1F801000u && addr <= 0x1F80103Cu) {
        return mem_ctrl[(addr - 0x1F801000u) >> 2];
    }
    /* SIO: 0x1F801040..0x1F80105F */
    if (addr >= 0x1F801040u && addr <= 0x1F80105Fu) {
        return sio_read(addr);
    }
    /* RAM size: 0x1F801060 */
    if (addr == 0x1F801060u) {
        return ram_size_reg;
    }
    /* Interrupts: 0x1F801070, 0x1F801074 */
    if (addr == 0x1F801070u) { sio_tick(); return i_stat; }
    if (addr == 0x1F801074u) return i_mask;
    /* DMA: 0x1F801080..0x1F8010FF */
    if (addr >= 0x1F801080u && addr <= 0x1F8010FFu) {
        return dma_read(addr);
    }
    /* Timers: 0x1F801100..0x1F80112F */
    if (addr >= 0x1F801100u && addr <= 0x1F80112Fu) {
        return timers_read(addr);
    }
    /* CDROM: 0x1F801800..0x1F801803 */
    if (addr >= 0x1F801800u && addr <= 0x1F801803u) {
        return cdrom_read(addr);
    }
    /* GPU: 0x1F801810 (GPUREAD), 0x1F801814 (GPUSTAT) */
    if (addr == 0x1F801810u) return gpu_read_gpuread();
    if (addr == 0x1F801814u) return gpu_read_gpustat();
    /* MDEC: 0x1F801820, 0x1F801824 */
    if (addr == 0x1F801820u || addr == 0x1F801824u) return 0;
    /* SPU: 0x1F801C00..0x1F801FFF */
    if (addr >= 0x1F801C00u && addr <= 0x1F801FFFu) {
        return spu_read(addr);
    }
    /* Expansion 2 / POST: 0x1F802000..0x1F802FFF */
    if (addr >= 0x1F802000u && addr <= 0x1F802FFFu) {
        return 0;
    }
    mmio_fatal(addr, addr, "READ32");
    return 0;
}

static void mmio_write32(uint32_t addr, uint32_t val) {
    debug_server_trace_mmio_write(addr, val, 4);
    /* Memory control: 0x1F801000..0x1F801020 */
    if (addr >= 0x1F801000u && addr <= 0x1F80103Cu) {
        mem_ctrl[(addr - 0x1F801000u) >> 2] = val;
        return;
    }
    /* SIO: 0x1F801040..0x1F80105F */
    if (addr >= 0x1F801040u && addr <= 0x1F80105Fu) {
        sio_write(addr, val);
        return;
    }
    /* RAM size: 0x1F801060 */
    if (addr == 0x1F801060u) {
        ram_size_reg = val;
        return;
    }
    /* Interrupts: 0x1F801070, 0x1F801074 */
    if (addr == 0x1F801070u) { i_stat &= val; return; } /* AND-acknowledge */
    if (addr == 0x1F801074u) { i_mask = val & 0x7FFu; return; }
    /* DMA: 0x1F801080..0x1F8010FF */
    if (addr >= 0x1F801080u && addr <= 0x1F8010FFu) {
        dma_write(addr, val);
        return;
    }
    /* Timers: 0x1F801100..0x1F80112F */
    if (addr >= 0x1F801100u && addr <= 0x1F80112Fu) {
        timers_write(addr, val);
        return;
    }
    /* CDROM: 0x1F801800..0x1F801803 */
    if (addr >= 0x1F801800u && addr <= 0x1F801803u) {
        cdrom_write(addr, val);
        return;
    }
    /* GPU GP0: 0x1F801810, GP1: 0x1F801814 */
    if (addr == 0x1F801810u) { gpu_write_gp0(val); return; }
    if (addr == 0x1F801814u) { gpu_write_gp1(val); return; }
    /* MDEC: 0x1F801820, 0x1F801824 */
    if (addr == 0x1F801820u || addr == 0x1F801824u) return;
    /* SPU: 0x1F801C00..0x1F801FFF */
    if (addr >= 0x1F801C00u && addr <= 0x1F801FFFu) {
        spu_write(addr, val);
        return;
    }
    /* Expansion 2 / POST: 0x1F802000..0x1F802FFF */
    if (addr >= 0x1F802000u && addr <= 0x1F802FFFu) {
        return; /* POST port — ignore */
    }
    mmio_fatal(addr, addr, "WRITE32");
}

static uint16_t mmio_read16(uint32_t addr) {
    /* SIO: 0x1F801040..0x1F80105F */
    if (addr >= 0x1F801040u && addr <= 0x1F80105Fu) {
        return (uint16_t)sio_read(addr);
    }
    /* Interrupts */
    if (addr == 0x1F801070u) { sio_tick(); return (uint16_t)i_stat; }
    if (addr == 0x1F801074u) return (uint16_t)i_mask;
    /* Timers: 0x1F801100..0x1F80112F */
    if (addr >= 0x1F801100u && addr <= 0x1F80112Fu) {
        return (uint16_t)timers_read(addr);
    }
    /* DMA: 0x1F801080..0x1F8010FF */
    if (addr >= 0x1F801080u && addr <= 0x1F8010FFu) {
        uint32_t val = dma_read(addr & ~3u);
        return (addr & 2) ? (uint16_t)(val >> 16) : (uint16_t)val;
    }
    /* SPU: 0x1F801C00..0x1F801FFF */
    if (addr >= 0x1F801C00u && addr <= 0x1F801FFFu) {
        return (uint16_t)spu_read(addr);
    }
    mmio_fatal(addr, addr, "READ16");
    return 0;
}

static void mmio_write16(uint32_t addr, uint16_t val) {
    debug_server_trace_mmio_write(addr, (uint32_t)val, 2);
    /* SIO: 0x1F801040..0x1F80105F */
    if (addr >= 0x1F801040u && addr <= 0x1F80105Fu) {
        sio_write(addr, val);
        return;
    }
    /* Interrupts */
    if (addr == 0x1F801070u) { i_stat &= val; return; }
    if (addr == 0x1F801074u) { i_mask = val & 0x7FFu; return; }
    /* Timers: 0x1F801100..0x1F80112F */
    if (addr >= 0x1F801100u && addr <= 0x1F80112Fu) {
        timers_write(addr, val);
        return;
    }
    /* DMA: 0x1F801080..0x1F8010FF */
    if (addr >= 0x1F801080u && addr <= 0x1F8010FFu) {
        uint32_t aligned = addr & ~3u;
        uint32_t cur = dma_read(aligned);
        if (addr & 2)
            cur = (cur & 0x0000FFFFu) | ((uint32_t)val << 16);
        else
            cur = (cur & 0xFFFF0000u) | (uint32_t)val;
        dma_write(aligned, cur);
        return;
    }
    /* SPU: 0x1F801C00..0x1F801FFF */
    if (addr >= 0x1F801C00u && addr <= 0x1F801FFFu) {
        spu_write(addr, val);
        return;
    }
    mmio_fatal(addr, addr, "WRITE16");
}

static uint8_t mmio_read8(uint32_t addr) {
    /* Interrupts: 0x1F801070..0x1F801077 (I_STAT, I_MASK) */
    if (addr >= 0x1F801070u && addr <= 0x1F801077u) {
        if (addr < 0x1F801074u) sio_tick();
        uint32_t val = (addr < 0x1F801074u) ? i_stat : i_mask;
        return (uint8_t)(val >> (8 * (addr & 3)));
    }
    /* SIO: 0x1F801040..0x1F80104F */
    if (addr >= 0x1F801040u && addr <= 0x1F80104Fu) {
        return (uint8_t)sio_read(addr & ~3u);
    }
    /* DMA: 0x1F801080..0x1F8010FF — byte reads return the corresponding
     * byte of the 32-bit register.  The BIOS shell reads DICR (0x1F8010F4)
     * as individual bytes during interrupt handling. */
    if (addr >= 0x1F801080u && addr <= 0x1F8010FFu) {
        uint32_t aligned = addr & ~3u;
        uint32_t val = dma_read(aligned);
        return (uint8_t)(val >> (8 * (addr & 3)));
    }
    /* CDROM: 0x1F801800..0x1F801803 */
    if (addr >= 0x1F801800u && addr <= 0x1F801803u) {
        return (uint8_t)cdrom_read(addr);
    }
    /* Expansion 2 / POST: 0x1F802000..0x1F802FFF */
    if (addr >= 0x1F802000u && addr <= 0x1F802FFFu) {
        return 0;
    }
    mmio_fatal(addr, addr, "READ8");
    return 0;
}

static void mmio_write8(uint32_t addr, uint8_t val) {
    debug_server_trace_mmio_write(addr, (uint32_t)val, 1);
    /* SIO: 0x1F801040..0x1F80105F */
    if (addr >= 0x1F801040u && addr <= 0x1F80105Fu) {
        sio_write(addr & ~3u, (uint32_t)val);
        return;
    }
    /* DMA: 0x1F801080..0x1F8010FF — byte writes update the corresponding
     * byte of the 32-bit register.  Needed for DICR byte-level access. */
    if (addr >= 0x1F801080u && addr <= 0x1F8010FFu) {
        uint32_t aligned = addr & ~3u;
        uint32_t cur = dma_read(aligned);
        uint32_t shift = 8 * (addr & 3);
        cur = (cur & ~(0xFFu << shift)) | ((uint32_t)val << shift);
        dma_write(aligned, cur);
        return;
    }
    /* CDROM: 0x1F801800..0x1F801803 */
    if (addr >= 0x1F801800u && addr <= 0x1F801803u) {
        cdrom_write(addr, val);
        return;
    }
    /* Expansion 2 / POST: 0x1F802000..0x1F802FFF */
    if (addr >= 0x1F802000u && addr <= 0x1F802FFFu) {
        return;
    }
    mmio_fatal(addr, addr, "WRITE8");
}

/* --- Read functions --- */

uint32_t psx_read_word(uint32_t addr) {
    /* KSEG2 cache control — before physical translation. */
    if (addr == 0xFFFE0130u) return cache_ctrl;

    uint32_t phys = addr & 0x1FFFFFFFu;

    if (phys < RAM_SIZE) {
        return  (uint32_t)ram[phys]
             | ((uint32_t)ram[phys + 1] << 8)
             | ((uint32_t)ram[phys + 2] << 16)
             | ((uint32_t)ram[phys + 3] << 24);
    }
    /* Expansion 1: 0x1F000000..0x1F7FFFFF — no device, open bus */
    if (phys >= 0x1F000000u && phys <= 0x1F7FFFFFu) {
        return 0xFFFFFFFFu;
    }
    if (phys >= 0x1F800000u && phys <= 0x1F8003FFu) {
        uint32_t off = phys - 0x1F800000u;
        return  (uint32_t)scratchpad[off]
             | ((uint32_t)scratchpad[off + 1] << 8)
             | ((uint32_t)scratchpad[off + 2] << 16)
             | ((uint32_t)scratchpad[off + 3] << 24);
    }
    if (phys >= 0x1F801000u && phys <= 0x1F803FFFu) {
        return mmio_read32(phys);
    }
    if (phys >= 0x1FC00000u && phys <= 0x1FC7FFFFu) {
        uint32_t off = phys - 0x1FC00000u;
        return  (uint32_t)bios_rom[off]
             | ((uint32_t)bios_rom[off + 1] << 8)
             | ((uint32_t)bios_rom[off + 2] << 16)
             | ((uint32_t)bios_rom[off + 3] << 24);
    }
    unmapped_fatal(addr, phys, "READ");
    return 0;
}

void psx_write_word(uint32_t addr, uint32_t val) {
    /* KSEG2 cache control — before physical translation. */
    if (addr == 0xFFFE0130u) { cache_ctrl = val; return; }

    /* IsC (Isolate Cache): when set, writes go to D-cache only.
     * We have no cache model, so silently discard RAM/scratchpad writes. */
    if (sr_ptr && (*sr_ptr & 0x10000u)) return;

    uint32_t phys = addr & 0x1FFFFFFFu;

    if (phys < RAM_SIZE) {
        debug_server_trace_write_check(phys, read_ram_word(phys), val, 4);
        ram[phys]     = (uint8_t)(val);
        ram[phys + 1] = (uint8_t)(val >> 8);
        ram[phys + 2] = (uint8_t)(val >> 16);
        ram[phys + 3] = (uint8_t)(val >> 24);
        return;
    }
    /* Expansion 1: 0x1F000000..0x1F7FFFFF — ignore writes */
    if (phys >= 0x1F000000u && phys <= 0x1F7FFFFFu) return;
    if (phys >= 0x1F800000u && phys <= 0x1F8003FFu) {
        uint32_t off = phys - 0x1F800000u;
        scratchpad[off]     = (uint8_t)(val);
        scratchpad[off + 1] = (uint8_t)(val >> 8);
        scratchpad[off + 2] = (uint8_t)(val >> 16);
        scratchpad[off + 3] = (uint8_t)(val >> 24);
        return;
    }
    if (phys >= 0x1F801000u && phys <= 0x1F803FFFu) {
        mmio_write32(phys, val);
        return;
    }
    if (phys >= 0x1FC00000u && phys <= 0x1FC7FFFFu) {
        /* ROM: silently ignore writes */
        return;
    }
    unmapped_fatal(addr, phys, "WRITE");
}

uint16_t psx_read_half(uint32_t addr) {
    uint32_t phys = addr & 0x1FFFFFFFu;

    if (phys < RAM_SIZE) {
        return (uint16_t)ram[phys] | ((uint16_t)ram[phys + 1] << 8);
    }
    if (phys >= 0x1F000000u && phys <= 0x1F7FFFFFu) return 0xFFFFu;
    if (phys >= 0x1F800000u && phys <= 0x1F8003FFu) {
        uint32_t off = phys - 0x1F800000u;
        return (uint16_t)scratchpad[off] | ((uint16_t)scratchpad[off + 1] << 8);
    }
    if (phys >= 0x1F801000u && phys <= 0x1F803FFFu) {
        return mmio_read16(phys);
    }
    if (phys >= 0x1FC00000u && phys <= 0x1FC7FFFFu) {
        uint32_t off = phys - 0x1FC00000u;
        return (uint16_t)bios_rom[off] | ((uint16_t)bios_rom[off + 1] << 8);
    }
    unmapped_fatal(addr, phys, "READ");
    return 0;
}

void psx_write_half(uint32_t addr, uint16_t val) {
    if (sr_ptr && (*sr_ptr & 0x10000u)) return;

    uint32_t phys = addr & 0x1FFFFFFFu;

    if (phys < RAM_SIZE) {
        debug_server_trace_write_check(phys, (uint32_t)read_ram_half(phys), (uint32_t)val, 2);
        ram[phys]     = (uint8_t)(val);
        ram[phys + 1] = (uint8_t)(val >> 8);
        return;
    }
    if (phys >= 0x1F000000u && phys <= 0x1F7FFFFFu) return;
    if (phys >= 0x1F800000u && phys <= 0x1F8003FFu) {
        uint32_t off = phys - 0x1F800000u;
        scratchpad[off]     = (uint8_t)(val);
        scratchpad[off + 1] = (uint8_t)(val >> 8);
        return;
    }
    if (phys >= 0x1F801000u && phys <= 0x1F803FFFu) {
        mmio_write16(phys, val);
        return;
    }
    if (phys >= 0x1FC00000u && phys <= 0x1FC7FFFFu) {
        return; /* ROM: ignore */
    }
    unmapped_fatal(addr, phys, "WRITE");
}

uint8_t psx_read_byte(uint32_t addr) {
    uint32_t phys = addr & 0x1FFFFFFFu;

    if (phys < RAM_SIZE) {
        return ram[phys];
    }
    if (phys >= 0x1F000000u && phys <= 0x1F7FFFFFu) return 0xFFu;
    if (phys >= 0x1F800000u && phys <= 0x1F8003FFu) {
        return scratchpad[phys - 0x1F800000u];
    }
    if (phys >= 0x1F801000u && phys <= 0x1F803FFFu) {
        return mmio_read8(phys);
    }
    if (phys >= 0x1FC00000u && phys <= 0x1FC7FFFFu) {
        return bios_rom[phys - 0x1FC00000u];
    }
    unmapped_fatal(addr, phys, "READ");
    return 0;
}

void psx_write_byte(uint32_t addr, uint8_t val) {
    if (sr_ptr && (*sr_ptr & 0x10000u)) return;

    uint32_t phys = addr & 0x1FFFFFFFu;

    if (phys < RAM_SIZE) {
        debug_server_trace_write_check(phys, (uint32_t)ram[phys], (uint32_t)val, 1);
        ram[phys] = val;
        return;
    }
    if (phys >= 0x1F000000u && phys <= 0x1F7FFFFFu) return;
    if (phys >= 0x1F800000u && phys <= 0x1F8003FFu) {
        scratchpad[phys - 0x1F800000u] = val;
        return;
    }
    if (phys >= 0x1F801000u && phys <= 0x1F803FFFu) {
        mmio_write8(phys, val);
        return;
    }
    if (phys >= 0x1FC00000u && phys <= 0x1FC7FFFFu) {
        return; /* ROM: ignore */
    }
    unmapped_fatal(addr, phys, "WRITE");
}
