#include "boot_state.h"
#include "overlay_api.h"   /* PSX_OVERLAY_CODEGEN_HASH / _ABI_TAG / _CODEGEN_VER */
#include "gpu_render.h"    /* gr_vram_transfer_in / gr_vram_transfer_out          */
#include "pst_wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE   (2u * 1024u * 1024u)
#define SPAD_SIZE  (1024u)
#define VRAM_W     1024
#define VRAM_H     512
#define VRAM_SIZE  ((uint32_t)(VRAM_W * VRAM_H * 2))  /* 1 MB, 16bpp */

/* ---- core accessors (existing runtime modules) ---- */
extern uint8_t*  memory_get_ram_ptr(void);
extern uint8_t*  memory_get_scratchpad_ptr(void);
extern uint32_t  dirty_ram_get_bitmap_word(uint32_t word_index);
extern uint32_t  dirty_ram_get_bitmap_word_count(void);
extern void      dirty_ram_set_bitmap_words(const uint32_t* words, uint32_t count);
extern uint32_t  i_stat;
extern uint32_t  i_mask;
extern uint64_t  psx_cycle_count;
extern void timers_get_snapshot(uint16_t counter[3], uint32_t mode[3],
                                uint16_t target[3], int32_t irq_line[3],
                                uint32_t frac[3]);
extern void timers_set_snapshot(const uint16_t counter[3], const uint32_t mode[3],
                                const uint16_t target[3], const int32_t irq_line[3],
                                const uint32_t frac[3]);

/* ---- per-subsystem complete-state accessors (defined in each module) ---- */
extern uint32_t gpu_snapshot_bytes(void);
extern void     gpu_snapshot_write(uint8_t* p);
extern int      gpu_snapshot_read(const uint8_t* p, uint32_t len);
extern uint32_t spu_snapshot_bytes(void);
extern void     spu_snapshot_write(uint8_t* p);
extern int      spu_snapshot_read(const uint8_t* p, uint32_t len);
extern uint8_t* spu_get_ram_ptr(void);
extern uint32_t spu_get_ram_bytes(void);
extern uint32_t cdrom_snapshot_bytes(void);
extern void     cdrom_snapshot_write(uint8_t* p);
extern int      cdrom_snapshot_read(const uint8_t* p, uint32_t len);
extern uint32_t dma_snapshot_bytes(void);
extern void     dma_snapshot_write(uint8_t* p);
extern int      dma_snapshot_read(const uint8_t* p, uint32_t len);
extern uint32_t sio_snapshot_bytes(void);
extern void     sio_snapshot_write(uint8_t* p);
extern int      sio_snapshot_read(const uint8_t* p, uint32_t len);

/* CPU regs wire: 32+3+32+32+32 LE u32 = 131 * 4 = 524 bytes (no padding). */
#define CPU_REGS_WIRE_BYTES (524u)
/* Timer wire: 3*u16 + 3*u32 + 3*u16 + 3*i32 + 3*u32 = 48 bytes (no pad holes). */
#define TIMER_REGS_WIRE_BYTES (48u)

/* ---- deferred capture state (armed before first boot, fired at handoff) ---- */
static char     s_capture_path[512];
static uint32_t s_capture_checksum;
static uint32_t s_capture_entry_pc;

static int fwrite_all(FILE* f, const void* p, size_t n) {
    return !n || fwrite(p, 1, n, f) == n;
}

static int write_header_le(FILE* f, const BootStateHeader* h) {
    uint8_t buf[BOOT_STATE_HEADER_WIRE_BYTES];
    PstW w;
    pst_w_init(&w, buf, sizeof buf);
    if (!pst_w_u32(&w, h->magic) ||
        !pst_w_u32(&w, h->version) ||
        !pst_w_u32(&w, h->bios_checksum) ||
        !pst_w_u32(&w, h->entry_pc) ||
        !pst_w_u32(&w, h->codegen_hash) ||
        !pst_w_i32(&w, h->abi_tag) ||
        !pst_w_u32(&w, h->codegen_ver) ||
        !pst_w_u32(&w, h->section_count) ||
        !pst_w_u32(&w, h->reserved) ||
        w.written != BOOT_STATE_HEADER_WIRE_BYTES)
        return 0;
    return fwrite_all(f, buf, sizeof buf);
}

static int read_header_le(FILE* f, BootStateHeader* h) {
    uint8_t buf[BOOT_STATE_HEADER_WIRE_BYTES];
    PstR r;
    if (fread(buf, 1, sizeof buf, f) != sizeof buf) return 0;
    pst_r_init(&r, buf, sizeof buf);
    memset(h, 0, sizeof *h);
    if (!pst_r_u32(&r, &h->magic) ||
        !pst_r_u32(&r, &h->version) ||
        !pst_r_u32(&r, &h->bios_checksum) ||
        !pst_r_u32(&r, &h->entry_pc) ||
        !pst_r_u32(&r, &h->codegen_hash) ||
        !pst_r_i32(&r, &h->abi_tag) ||
        !pst_r_u32(&r, &h->codegen_ver) ||
        !pst_r_u32(&r, &h->section_count) ||
        !pst_r_u32(&r, &h->reserved))
        return 0;
    return 1;
}

static int write_section(FILE* f, uint32_t tag, const void* data, uint64_t len) {
    uint8_t hdr[16];
    PstW w;
    pst_w_init(&w, hdr, sizeof hdr);
    if (!pst_w_u32(&w, tag) || !pst_w_u32(&w, 0u) || !pst_w_u64(&w, len))
        return 0;
    if (!fwrite_all(f, hdr, sizeof hdr)) return 0;
    if (len && !fwrite_all(f, data, (size_t)len)) return 0;
    return 1;
}

static int write_module_section(FILE* f, uint32_t tag,
                                uint32_t (*bytes)(void),
                                void (*write)(uint8_t*)) {
    uint32_t n = bytes();
    uint8_t* buf = (uint8_t*)malloc(n ? n : 1);
    if (!buf) return 0;
    write(buf);
    int ok = write_section(f, tag, buf, n);
    free(buf);
    return ok;
}

static int write_cpu_section(FILE* f, const CPUState* cpu) {
    uint8_t buf[CPU_REGS_WIRE_BYTES];
    PstW w;
    pst_w_init(&w, buf, sizeof buf);
    for (int i = 0; i < 32; i++)
        if (!pst_w_u32(&w, cpu->gpr[i])) return 0;
    if (!pst_w_u32(&w, cpu->pc) || !pst_w_u32(&w, cpu->hi) || !pst_w_u32(&w, cpu->lo))
        return 0;
    for (int i = 0; i < 32; i++)
        if (!pst_w_u32(&w, cpu->cop0[i])) return 0;
    for (int i = 0; i < 32; i++)
        if (!pst_w_u32(&w, cpu->gte_data[i])) return 0;
    for (int i = 0; i < 32; i++)
        if (!pst_w_u32(&w, cpu->gte_ctrl[i])) return 0;
    if (w.written != CPU_REGS_WIRE_BYTES) return 0;
    return write_section(f, BS_SEC_CPU, buf, CPU_REGS_WIRE_BYTES);
}

static int write_timer_section(FILE* f) {
    uint16_t counter[3], target[3];
    uint32_t mode[3], frac[3];
    int32_t irq_line[3];
    uint8_t buf[TIMER_REGS_WIRE_BYTES];
    PstW w;
    timers_get_snapshot(counter, mode, target, irq_line, frac);
    pst_w_init(&w, buf, sizeof buf);
    for (int i = 0; i < 3; i++)
        if (!pst_w_u16(&w, counter[i])) return 0;
    for (int i = 0; i < 3; i++)
        if (!pst_w_u32(&w, mode[i])) return 0;
    for (int i = 0; i < 3; i++)
        if (!pst_w_u16(&w, target[i])) return 0;
    for (int i = 0; i < 3; i++)
        if (!pst_w_i32(&w, irq_line[i])) return 0;
    for (int i = 0; i < 3; i++)
        if (!pst_w_u32(&w, frac[i])) return 0;
    if (w.written != TIMER_REGS_WIRE_BYTES) return 0;
    return write_section(f, BS_SEC_TIMER, buf, TIMER_REGS_WIRE_BYTES);
}

/* ============================ SAVE ============================ */

int boot_state_save(const CPUState* cpu, uint32_t bios_checksum,
                    uint32_t entry_pc, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;

    BootStateHeader h;
    memset(&h, 0, sizeof h);
    h.magic         = BOOT_STATE_MAGIC;
    h.version       = BOOT_STATE_VERSION;
    h.bios_checksum = bios_checksum;
    h.entry_pc      = entry_pc;
    h.codegen_hash  = (uint32_t)PSX_OVERLAY_CODEGEN_HASH;
    h.abi_tag       = (int32_t)PSX_OVERLAY_ABI_TAG;
    h.codegen_ver   = (uint32_t)PSX_OVERLAY_CODEGEN_VER;
    h.section_count = 14;

    int ok = write_header_le(f, &h);

    if (ok) ok = write_cpu_section(f, cpu);
    if (ok) ok = write_section(f, BS_SEC_RAM,  memory_get_ram_ptr(),        RAM_SIZE);
    if (ok) ok = write_section(f, BS_SEC_SPAD, memory_get_scratchpad_ptr(), SPAD_SIZE);
    if (ok) {
        uint8_t irq[8];
        PstW w;
        pst_w_init(&w, irq, sizeof irq);
        ok = pst_w_u32(&w, i_stat) && pst_w_u32(&w, i_mask) &&
             write_section(f, BS_SEC_IRQ, irq, 8);
    }
    if (ok) ok = write_timer_section(f);
    if (ok) {
        uint8_t cyc[8];
        PstW w;
        pst_w_init(&w, cyc, sizeof cyc);
        ok = pst_w_u64(&w, psx_cycle_count) &&
             write_section(f, BS_SEC_CLOCK, cyc, 8);
    }
    if (ok) ok = write_module_section(f, BS_SEC_GPU, gpu_snapshot_bytes, gpu_snapshot_write);
    if (ok) {
        uint16_t* vbuf = (uint16_t*)malloc(VRAM_SIZE);
        if (!vbuf) ok = 0;
        else {
            gr_vram_transfer_out(0, 0, VRAM_W, VRAM_H, vbuf);
            /* VRAM is uint16 LE guest pixels — emit as LE u16 stream. */
            uint8_t* wire = (uint8_t*)malloc(VRAM_SIZE);
            if (!wire) ok = 0;
            else {
                PstW w;
                pst_w_init(&w, wire, VRAM_SIZE);
                ok = pst_w_pod(&w, vbuf, VRAM_SIZE, 2) &&
                     write_section(f, BS_SEC_VRAM, wire, VRAM_SIZE);
                free(wire);
            }
            free(vbuf);
        }
    }
    if (ok) ok = write_module_section(f, BS_SEC_SPU, spu_snapshot_bytes, spu_snapshot_write);
    if (ok) ok = write_section(f, BS_SEC_SPURAM, spu_get_ram_ptr(), spu_get_ram_bytes());
    if (ok) ok = write_module_section(f, BS_SEC_CDROM, cdrom_snapshot_bytes, cdrom_snapshot_write);
    if (ok) ok = write_module_section(f, BS_SEC_DMA,   dma_snapshot_bytes,   dma_snapshot_write);
    if (ok) ok = write_module_section(f, BS_SEC_SIO,   sio_snapshot_bytes,   sio_snapshot_write);
    if (ok) {
        uint32_t wc = dirty_ram_get_bitmap_word_count();
        uint64_t nbytes = (uint64_t)wc * 4u;
        uint8_t* db = (uint8_t*)malloc(nbytes ? (size_t)nbytes : 1);
        if (!db) ok = 0;
        else {
            PstW w;
            pst_w_init(&w, db, (size_t)nbytes);
            ok = 1;
            for (uint32_t i = 0; ok && i < wc; i++)
                ok = pst_w_u32(&w, dirty_ram_get_bitmap_word(i));
            if (ok) ok = write_section(f, BS_SEC_DIRTY, db, nbytes);
            free(db);
        }
    }

    fclose(f);
    if (!ok)
        remove(path);
    return ok;
}

/* ============================ LOAD ============================ */

static int apply_section(uint32_t tag, const uint8_t* p, uint32_t len,
                         CPUState* cpu, uint32_t entry_pc) {
    switch (tag) {
    case BS_SEC_CPU: {
        PstR r;
        if (len != CPU_REGS_WIRE_BYTES) return 0;
        pst_r_init(&r, p, len);
        for (int i = 0; i < 32; i++)
            if (!pst_r_u32(&r, &cpu->gpr[i])) return 0;
        if (!pst_r_u32(&r, &cpu->pc) || !pst_r_u32(&r, &cpu->hi) ||
            !pst_r_u32(&r, &cpu->lo))
            return 0;
        (void)entry_pc;
        for (int i = 0; i < 32; i++)
            if (!pst_r_u32(&r, &cpu->cop0[i])) return 0;
        for (int i = 0; i < 32; i++)
            if (!pst_r_u32(&r, &cpu->gte_data[i])) return 0;
        for (int i = 0; i < 32; i++)
            if (!pst_r_u32(&r, &cpu->gte_ctrl[i])) return 0;
        return 1;
    }
    case BS_SEC_RAM:
        if (len != RAM_SIZE) return 0;
        memcpy(memory_get_ram_ptr(), p, RAM_SIZE);
        {
            extern void psx_kernel_bless_note_range(uint32_t phys, uint32_t l);
            psx_kernel_bless_note_range(0, RAM_SIZE);
        }
        return 1;
    case BS_SEC_SPAD:
        if (len != SPAD_SIZE) return 0;
        memcpy(memory_get_scratchpad_ptr(), p, SPAD_SIZE);
        return 1;
    case BS_SEC_IRQ: {
        PstR r;
        uint32_t st, mk;
        if (len != 8) return 0;
        pst_r_init(&r, p, len);
        if (!pst_r_u32(&r, &st) || !pst_r_u32(&r, &mk)) return 0;
        i_stat = st;
        i_mask = mk;
        return 1;
    }
    case BS_SEC_TIMER: {
        uint16_t counter[3], target[3];
        uint32_t mode[3], frac[3];
        int32_t irq_line[3];
        PstR r;
        if (len != TIMER_REGS_WIRE_BYTES) return 0;
        pst_r_init(&r, p, len);
        for (int i = 0; i < 3; i++)
            if (!pst_r_u16(&r, &counter[i])) return 0;
        for (int i = 0; i < 3; i++)
            if (!pst_r_u32(&r, &mode[i])) return 0;
        for (int i = 0; i < 3; i++)
            if (!pst_r_u16(&r, &target[i])) return 0;
        for (int i = 0; i < 3; i++)
            if (!pst_r_i32(&r, &irq_line[i])) return 0;
        for (int i = 0; i < 3; i++)
            if (!pst_r_u32(&r, &frac[i])) return 0;
        timers_set_snapshot(counter, mode, target, irq_line, frac);
        return 1;
    }
    case BS_SEC_CLOCK: {
        PstR r;
        uint64_t cyc;
        if (len != 8) return 0;
        pst_r_init(&r, p, len);
        if (!pst_r_u64(&r, &cyc)) return 0;
        psx_cycle_count = cyc;
        return 1;
    }
    case BS_SEC_GPU:
        return gpu_snapshot_read(p, len);
    case BS_SEC_VRAM: {
        uint16_t* vbuf;
        PstR r;
        if (len != VRAM_SIZE) return 0;
        vbuf = (uint16_t*)malloc(VRAM_SIZE);
        if (!vbuf) return 0;
        pst_r_init(&r, p, len);
        if (!pst_r_pod(&r, vbuf, VRAM_SIZE, 2)) {
            free(vbuf);
            return 0;
        }
        gr_vram_transfer_in(0, 0, VRAM_W, VRAM_H, vbuf);
        free(vbuf);
        return 1;
    }
    case BS_SEC_SPU:
        return spu_snapshot_read(p, len);
    case BS_SEC_SPURAM:
        if (len != spu_get_ram_bytes()) return 0;
        memcpy(spu_get_ram_ptr(), p, len);
        return 1;
    case BS_SEC_CDROM:
        return cdrom_snapshot_read(p, len);
    case BS_SEC_DMA:
        return dma_snapshot_read(p, len);
    case BS_SEC_SIO:
        return sio_snapshot_read(p, len);
    case BS_SEC_DIRTY: {
        uint32_t wc;
        uint32_t* words;
        PstR r;
        if (len % 4u) return 0;
        wc = len / 4u;
        words = (uint32_t*)malloc(len ? len : 1);
        if (!words) return 0;
        pst_r_init(&r, p, len);
        for (uint32_t i = 0; i < wc; i++) {
            if (!pst_r_u32(&r, &words[i])) {
                free(words);
                return 0;
            }
        }
        dirty_ram_set_bitmap_words(words, wc);
        free(words);
        return 1;
    }
    default:
        return 0;
    }
}

int boot_state_load(const char* path, uint32_t bios_checksum,
                    uint32_t entry_pc, CPUState* cpu) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;

    BootStateHeader h;
    if (!read_header_le(f, &h)) { fclose(f); return 0; }

    if (h.magic         != BOOT_STATE_MAGIC               ||
        h.version       != BOOT_STATE_VERSION             ||
        h.bios_checksum != bios_checksum                  ||
        h.entry_pc      != entry_pc                       ||
        h.codegen_hash  != (uint32_t)PSX_OVERLAY_CODEGEN_HASH ||
        h.abi_tag       != (int32_t)PSX_OVERLAY_ABI_TAG   ||
        h.codegen_ver   != (uint32_t)PSX_OVERLAY_CODEGEN_VER) {
        fclose(f);
        return 0;
    }

    const uint32_t required =
        (1u<<BS_SEC_CPU)|(1u<<BS_SEC_RAM)|(1u<<BS_SEC_SPAD)|(1u<<BS_SEC_IRQ)|
        (1u<<BS_SEC_TIMER)|(1u<<BS_SEC_CLOCK)|(1u<<BS_SEC_GPU)|(1u<<BS_SEC_VRAM)|
        (1u<<BS_SEC_SPU)|(1u<<BS_SEC_SPURAM)|(1u<<BS_SEC_CDROM)|(1u<<BS_SEC_DMA)|
        (1u<<BS_SEC_SIO)|(1u<<BS_SEC_DIRTY);
    uint32_t seen = 0;
    int ok = 1;

    for (uint32_t i = 0; ok && i < h.section_count; i++) {
        uint8_t shdr[16];
        PstR hr;
        uint32_t tag = 0, pad = 0;
        uint64_t len = 0;
        if (fread(shdr, 1, sizeof shdr, f) != sizeof shdr) { ok = 0; break; }
        pst_r_init(&hr, shdr, sizeof shdr);
        if (!pst_r_u32(&hr, &tag) || !pst_r_u32(&hr, &pad) || !pst_r_u64(&hr, &len)) {
            ok = 0; break;
        }
        if (len > 64u * 1024u * 1024u) { ok = 0; break; }
        uint8_t* buf = (uint8_t*)malloc(len ? (size_t)len : 1);
        if (!buf) { ok = 0; break; }
        if (len && fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); ok = 0; break; }
        if (!apply_section(tag, buf, (uint32_t)len, cpu, entry_pc)) ok = 0;
        else if (tag < 32) seen |= (1u << tag);
        free(buf);
    }
    fclose(f);

    if (!ok || (seen & required) != required)
        return 0;
    return 1;
}

void boot_state_set_capture(const char* path, uint32_t bios_checksum,
                            uint32_t entry_pc) {
    strncpy(s_capture_path, path, sizeof(s_capture_path) - 1);
    s_capture_path[sizeof(s_capture_path) - 1] = '\0';
    s_capture_checksum = bios_checksum;
    s_capture_entry_pc = entry_pc;
}

void boot_state_trigger_capture(const CPUState* cpu) {
    if (!s_capture_path[0]) return;
    boot_state_save(cpu, s_capture_checksum, s_capture_entry_pc, s_capture_path);
    s_capture_path[0] = '\0';
}
