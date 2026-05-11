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
#include "mdec.h"
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

/* Expose RAM pointer for oracle comparison (find_first_divergence). */
uint8_t *memory_get_ram_ptr(void) { return ram; }

/* ---- Dirty-page tracking for install-at-runtime code (CLAUDE.md Rule 18) ----
 *
 * The PS1 BIOS writes 4-instruction dispatch stubs into kernel RAM at runtime
 * (e.g. RAM 0xCF0 for the SIO data-byte handler).  A static recompiler can't
 * see those instructions because they don't exist at compile time.  We track
 * which RAM pages have been written-to since boot and route any psx_dispatch
 * landing in such a page through a small MIPS interpreter (dirty_ram_interp.c).
 *
 * Granularity: 4 KB pages.  Ordinary CPU writes only mark the kernel-code
 * region (RAM 0..0xFFFF) where BIOS install stubs live.  CD-ROM DMA can also
 * mark arbitrary RAM ranges as executable candidates, which covers game
 * overlays loaded from disc without treating every data write as code.
 *
 * Future option (Option B, see docs/dynamic_handler_install.md): when a page
 * goes dirty, JIT-compile its bytes via StrictTranslator instead of running
 * an interpreter.  Pros: one source of MIPS semantics, hot install stubs run
 * as native compiled C.  Cons: gcc-at-runtime build dep, ~200 ms compile latency
 * stall on first dispatch, file I/O on hot path, cache-invalidation complexity,
 * Windows MinGW + dlopen friction.  Revisit only if install stubs become a
 * measurable hot path; today they're cold-path glue (~4k instructions per
 * directory-load is sub-microsecond to interpret). */
#define DIRTY_RAM_KERNEL_TRACK_BYTES 0x10000u
#define DIRTY_RAM_PAGE_SHIFT    12          /* 4 KB pages */
#define DIRTY_RAM_PAGE_COUNT    (RAM_SIZE >> DIRTY_RAM_PAGE_SHIFT)
#define DIRTY_RAM_BITMAP_WORDS  ((DIRTY_RAM_PAGE_COUNT + 31u) / 32u)
static uint32_t dirty_ram_bitmap[DIRTY_RAM_BITMAP_WORDS];

static inline void dirty_ram_mark_page(uint32_t phys) {
    if (phys >= RAM_SIZE) return;
    uint32_t page = phys >> DIRTY_RAM_PAGE_SHIFT;
    dirty_ram_bitmap[page >> 5] |= (1u << (page & 31u));
}

static inline void dirty_ram_mark_kernel_write(uint32_t phys) {
    if (phys >= DIRTY_RAM_KERNEL_TRACK_BYTES) return;
    dirty_ram_mark_page(phys);
}

void dirty_ram_mark_executable_range(uint32_t phys, uint32_t len) {
    if (len == 0 || phys >= RAM_SIZE) return;
    uint32_t end = phys + len - 1u;
    if (end >= RAM_SIZE || end < phys) end = RAM_SIZE - 1u;

    uint32_t first_page = phys >> DIRTY_RAM_PAGE_SHIFT;
    uint32_t last_page = end >> DIRTY_RAM_PAGE_SHIFT;
    for (uint32_t page = first_page; page <= last_page; page++) {
        dirty_ram_bitmap[page >> 5] |= (1u << (page & 31u));
    }
}

int dirty_ram_is_dirty(uint32_t phys) {
    if (phys >= RAM_SIZE) return 0;
    uint32_t page = phys >> DIRTY_RAM_PAGE_SHIFT;
    return (dirty_ram_bitmap[page >> 5] >> (page & 31u)) & 1u;
}

uint32_t dirty_ram_get_bitmap(void) { return dirty_ram_bitmap[0]; }

uint32_t dirty_ram_get_bitmap_word(uint32_t word_index) {
    if (word_index >= DIRTY_RAM_BITMAP_WORDS) return 0;
    return dirty_ram_bitmap[word_index];
}

uint32_t dirty_ram_get_bitmap_word_count(void) {
    return DIRTY_RAM_BITMAP_WORDS;
}

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

/* ---- Card protocol trace: tracks I_MASK bit 7 transitions ---- */
#define IMASK_TRACE_CAP 4096
typedef struct {
    uint32_t old_mask;
    uint32_t new_mask;
    uint32_t caller;     /* g_debug_current_func_addr */
    uint32_t store_pc;   /* g_debug_last_store_pc — exact PC of the SW/SH */
    uint8_t  width;      /* 16 or 32 */
    uint8_t  bit7_set;   /* 1 if this write SET bit 7 */
    uint8_t  bit7_clear; /* 1 if this write CLEARED bit 7 */
    uint8_t  in_exc;
} ImaskTraceEntry;
static ImaskTraceEntry imask_trace[IMASK_TRACE_CAP];
static int imask_trace_idx = 0;
static int imask_trace_count = 0;
static int imask_bit7_set_count = 0;
static int imask_bit7_clear_count = 0;

static void imask_trace_record(uint32_t old_val, uint32_t new_val, uint8_t width) {
    extern uint32_t g_debug_current_func_addr;
    extern uint32_t g_debug_last_store_pc;
    extern int psx_get_in_exception(void);
    ImaskTraceEntry *e = &imask_trace[imask_trace_idx];
    e->old_mask   = old_val;
    e->new_mask   = new_val;
    e->caller     = g_debug_current_func_addr;
    e->store_pc   = g_debug_last_store_pc;
    e->width      = width;
    e->bit7_set   = (!(old_val & 0x80) && (new_val & 0x80)) ? 1 : 0;
    e->bit7_clear = ((old_val & 0x80) && !(new_val & 0x80)) ? 1 : 0;
    e->in_exc     = (uint8_t)psx_get_in_exception();
    if (e->bit7_set) imask_bit7_set_count++;
    if (e->bit7_clear) imask_bit7_clear_count++;
    imask_trace_idx = (imask_trace_idx + 1) % IMASK_TRACE_CAP;
    imask_trace_count++;
}

/* Getters for debug server */
int memory_get_imask_bit7_set_count(void) { return imask_bit7_set_count; }
int memory_get_imask_bit7_clear_count(void) { return imask_bit7_clear_count; }
const ImaskTraceEntry *memory_get_imask_trace(int *idx_out, int *count_out) {
    if (idx_out) *idx_out = imask_trace_idx;
    if (count_out) *count_out = imask_trace_count;
    return imask_trace;
}

/* Tier 1 write-trace hooks (implemented in debug_server.c). */
extern void debug_server_trace_write_check(uint32_t phys, uint32_t old_val,
                                           uint32_t new_val, uint8_t width);
extern void debug_server_trace_mmio_write(uint32_t addr, uint32_t val, uint8_t width);

/* Card-byte destination capture (Phase 3 audit). Always-on. */
extern int card_data_writes_check(uint32_t phys, uint32_t value, uint8_t width);

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
uint32_t memory_get_sr(void) { return sr_ptr ? *sr_ptr : 0; }

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
    if (addr == 0x1F801070u) { sio_tick(0); return i_stat; }
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
    if (addr == 0x1F801820u || addr == 0x1F801824u) return mdec_read(addr);
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
    if (addr == 0x1F801074u) { uint32_t old = i_mask; i_mask = val & 0x7FFu; imask_trace_record(old, i_mask, 32); return; }
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
    if (addr == 0x1F801820u || addr == 0x1F801824u) { mdec_write(addr, val); return; }
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
    if (addr == 0x1F801070u) { sio_tick(0); return (uint16_t)i_stat; }
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
    /* MDEC: 0x1F801820..0x1F801827 */
    if (addr >= 0x1F801820u && addr <= 0x1F801827u) {
        uint32_t val = mdec_read(addr & ~3u);
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
    if (addr == 0x1F801074u) { uint32_t old = i_mask; i_mask = val & 0x7FFu; imask_trace_record(old, i_mask, 16); return; }
    /* Timers: 0x1F801100..0x1F80112F */
    if (addr >= 0x1F801100u && addr <= 0x1F80112Fu) {
        timers_write(addr, val);
        return;
    }
    /* DMA: 0x1F801080..0x1F8010FF */
    if (addr >= 0x1F801080u && addr <= 0x1F8010FFu) {
        uint32_t aligned = addr & ~3u;
        uint32_t shift = (addr & 2) ? 16u : 0u;
        uint32_t mask = 0xFFFFu << shift;
        dma_write_masked(aligned, (uint32_t)val << shift, mask);
        return;
    }
    /* MDEC: 0x1F801820..0x1F801827 */
    if (addr >= 0x1F801820u && addr <= 0x1F801827u) {
        uint32_t aligned = addr & ~3u;
        uint32_t cur = mdec_read(aligned);
        if (addr & 2)
            cur = (cur & 0x0000FFFFu) | ((uint32_t)val << 16);
        else
            cur = (cur & 0xFFFF0000u) | (uint32_t)val;
        mdec_write(aligned, cur);
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
        if (addr < 0x1F801074u) sio_tick(0);
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
    /* MDEC: 0x1F801820..0x1F801827 */
    if (addr >= 0x1F801820u && addr <= 0x1F801827u) {
        uint32_t val = mdec_read(addr & ~3u);
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
        uint32_t shift = 8 * (addr & 3);
        uint32_t mask = 0xFFu << shift;
        dma_write_masked(aligned, (uint32_t)val << shift, mask);
        return;
    }
    /* MDEC: 0x1F801820..0x1F801827 */
    if (addr >= 0x1F801820u && addr <= 0x1F801827u) {
        uint32_t aligned = addr & ~3u;
        uint32_t cur = mdec_read(aligned);
        uint32_t shift = 8 * (addr & 3);
        cur = (cur & ~(0xFFu << shift)) | ((uint32_t)val << shift);
        mdec_write(aligned, cur);
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
        card_data_writes_check(phys, val, 4);
        dirty_ram_mark_kernel_write(phys);
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
        debug_server_trace_write_check(phys,
            (uint32_t)scratchpad[off]
          | ((uint32_t)scratchpad[off + 1] << 8)
          | ((uint32_t)scratchpad[off + 2] << 16)
          | ((uint32_t)scratchpad[off + 3] << 24),
            val, 4);
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
        card_data_writes_check(phys, (uint32_t)val, 2);
        dirty_ram_mark_kernel_write(phys);
        ram[phys]     = (uint8_t)(val);
        ram[phys + 1] = (uint8_t)(val >> 8);
        return;
    }
    if (phys >= 0x1F000000u && phys <= 0x1F7FFFFFu) return;
    if (phys >= 0x1F800000u && phys <= 0x1F8003FFu) {
        uint32_t off = phys - 0x1F800000u;
        debug_server_trace_write_check(phys,
            (uint32_t)scratchpad[off] | ((uint32_t)scratchpad[off + 1] << 8),
            (uint32_t)val, 2);
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
        card_data_writes_check(phys, (uint32_t)val, 1);
        dirty_ram_mark_kernel_write(phys);
        ram[phys] = val;
        return;
    }
    if (phys >= 0x1F000000u && phys <= 0x1F7FFFFFu) return;
    if (phys >= 0x1F800000u && phys <= 0x1F8003FFu) {
        debug_server_trace_write_check(phys, (uint32_t)scratchpad[phys - 0x1F800000u],
                                       (uint32_t)val, 1);
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
