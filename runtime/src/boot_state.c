#include "boot_state.h"
#include "overlay_api.h"   /* PSX_OVERLAY_CODEGEN_HASH / _ABI_TAG / _CODEGEN_VER */
#include "gpu_render.h"    /* gr_vram_transfer_in / gr_vram_transfer_out          */
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

/*
 * CPU register section payload — a fixed, build-stable layout (guest-
 * architectural state, portable across rebuilds). Mirrors the CPUState fields
 * the old v1 header carried, but as an explicit section.
 */
typedef struct {
    uint32_t gpr[32];
    uint32_t pc, hi, lo;
    uint32_t cop0[32];
    uint32_t gte_data[32];
    uint32_t gte_ctrl[32];
} CpuRegs;

/* Timer section payload — the 5 arrays the timer module snapshots. */
typedef struct {
    uint16_t counter[3];
    uint32_t mode[3];
    uint16_t target[3];
    int32_t  irq_line[3];
    uint32_t frac[3];
} TimerRegs;

/* ---- deferred capture state (armed before first boot, fired at handoff) ---- */
static char     s_capture_path[512];
static uint32_t s_capture_checksum;
static uint32_t s_capture_entry_pc;

/* ============================ SAVE ============================ */

static int write_section(FILE* f, uint32_t tag, const void* data, uint64_t len) {
    uint32_t hdr[2] = { tag, 0u };  /* tag, pad */
    if (fwrite(hdr, sizeof hdr, 1, f) != 1) return 0;
    if (fwrite(&len, sizeof len, 1, f) != 1) return 0;
    if (len && fwrite(data, 1, (size_t)len, f) != (size_t)len) return 0;
    return 1;
}

/* Write a section whose payload is produced by a module's _write into a
 * heap buffer of size _bytes. Returns 0 on any failure. */
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

    int ok = (fwrite(&h, sizeof h, 1, f) == 1);

    /* CPU */
    if (ok) {
        CpuRegs c;
        memcpy(c.gpr, cpu->gpr, sizeof c.gpr);
        c.pc = cpu->pc; c.hi = cpu->hi; c.lo = cpu->lo;
        memcpy(c.cop0,     cpu->cop0,     sizeof c.cop0);
        memcpy(c.gte_data, cpu->gte_data, sizeof c.gte_data);
        memcpy(c.gte_ctrl, cpu->gte_ctrl, sizeof c.gte_ctrl);
        ok = write_section(f, BS_SEC_CPU, &c, sizeof c);
    }
    /* Main RAM / scratchpad */
    if (ok) ok = write_section(f, BS_SEC_RAM,  memory_get_ram_ptr(),        RAM_SIZE);
    if (ok) ok = write_section(f, BS_SEC_SPAD, memory_get_scratchpad_ptr(), SPAD_SIZE);
    /* Interrupt controller */
    if (ok) {
        uint32_t irq[2] = { i_stat, i_mask };
        ok = write_section(f, BS_SEC_IRQ, irq, sizeof irq);
    }
    /* Timers */
    if (ok) {
        TimerRegs t;
        timers_get_snapshot(t.counter, t.mode, t.target, t.irq_line, t.frac);
        ok = write_section(f, BS_SEC_TIMER, &t, sizeof t);
    }
    /* Guest clock */
    if (ok) {
        uint64_t cyc = psx_cycle_count;
        ok = write_section(f, BS_SEC_CLOCK, &cyc, sizeof cyc);
    }
    /* GPU registers */
    if (ok) ok = write_module_section(f, BS_SEC_GPU, gpu_snapshot_bytes, gpu_snapshot_write);
    /* VRAM — via the renderer-agnostic hook (reads back the GL/VK FBO truth,
     * or the software array, depending on the active backend). */
    if (ok) {
        uint16_t* vbuf = (uint16_t*)malloc(VRAM_SIZE);
        if (!vbuf) ok = 0;
        else {
            gr_vram_transfer_out(0, 0, VRAM_W, VRAM_H, vbuf);
            ok = write_section(f, BS_SEC_VRAM, vbuf, VRAM_SIZE);
            free(vbuf);
        }
    }
    /* SPU registers + voices, then SPU RAM */
    if (ok) ok = write_module_section(f, BS_SEC_SPU, spu_snapshot_bytes, spu_snapshot_write);
    if (ok) ok = write_section(f, BS_SEC_SPURAM, spu_get_ram_ptr(), spu_get_ram_bytes());
    /* CD-ROM / DMA / SIO controller state */
    if (ok) ok = write_module_section(f, BS_SEC_CDROM, cdrom_snapshot_bytes, cdrom_snapshot_write);
    if (ok) ok = write_module_section(f, BS_SEC_DMA,   dma_snapshot_bytes,   dma_snapshot_write);
    if (ok) ok = write_module_section(f, BS_SEC_SIO,   sio_snapshot_bytes,   sio_snapshot_write);
    /* Dirty-RAM page bitmap (which guest pages hold runtime-written code) */
    if (ok) {
        uint32_t wc = dirty_ram_get_bitmap_word_count();
        uint32_t* db = (uint32_t*)malloc((wc ? wc : 1) * sizeof(uint32_t));
        if (!db) ok = 0;
        else {
            for (uint32_t i = 0; i < wc; i++) db[i] = dirty_ram_get_bitmap_word(i);
            ok = write_section(f, BS_SEC_DIRTY, db, (uint64_t)wc * sizeof(uint32_t));
            free(db);
        }
    }

    fclose(f);
    if (ok)
        fprintf(stdout, "boot_state: complete snapshot saved -> %s\n", path);
    else
        remove(path);  /* don't leave a truncated/partial snapshot behind */
    return ok;
}

/* ============================ LOAD ============================ */

/* Apply one decoded section. Returns 1 on success, 0 on any failure (which
 * aborts the whole restore -> normal boot). */
static int apply_section(uint32_t tag, const uint8_t* p, uint32_t len,
                         CPUState* cpu, uint32_t entry_pc) {
    switch (tag) {
    case BS_SEC_CPU: {
        if (len != sizeof(CpuRegs)) return 0;
        const CpuRegs* c = (const CpuRegs*)p;
        memcpy(cpu->gpr,      c->gpr,      sizeof cpu->gpr);
        cpu->pc = entry_pc;   /* always enter at the game entry, never a mid-PC */
        cpu->hi = c->hi; cpu->lo = c->lo;
        memcpy(cpu->cop0,     c->cop0,     sizeof cpu->cop0);
        memcpy(cpu->gte_data, c->gte_data, sizeof cpu->gte_data);
        memcpy(cpu->gte_ctrl, c->gte_ctrl, sizeof cpu->gte_ctrl);
        return 1;
    }
    case BS_SEC_RAM:
        if (len != RAM_SIZE) return 0;
        memcpy(memory_get_ram_ptr(), p, RAM_SIZE);
        {   /* Kernel-bless: RAM changed wholesale — force re-verification. */
            extern void psx_kernel_bless_note_range(uint32_t phys, uint32_t l);
            psx_kernel_bless_note_range(0, RAM_SIZE);
        }
        return 1;
    case BS_SEC_SPAD:
        if (len != SPAD_SIZE) return 0;
        memcpy(memory_get_scratchpad_ptr(), p, SPAD_SIZE);
        return 1;
    case BS_SEC_IRQ: {
        if (len != 2 * sizeof(uint32_t)) return 0;
        uint32_t v[2]; memcpy(v, p, sizeof v);
        i_stat = v[0]; i_mask = v[1];
        return 1;
    }
    case BS_SEC_TIMER: {
        if (len != sizeof(TimerRegs)) return 0;
        const TimerRegs* t = (const TimerRegs*)p;
        timers_set_snapshot(t->counter, t->mode, t->target, t->irq_line, t->frac);
        return 1;
    }
    case BS_SEC_CLOCK:
        if (len != sizeof(uint64_t)) return 0;
        memcpy(&psx_cycle_count, p, sizeof(uint64_t));
        return 1;
    case BS_SEC_GPU:
        return gpu_snapshot_read(p, len);
    case BS_SEC_VRAM:
        if (len != VRAM_SIZE) return 0;
        /* Renderer-agnostic upload: stages the GL/VK FBO upload or writes the
         * software array, so the restored frame is correct on every backend. */
        gr_vram_transfer_in(0, 0, VRAM_W, VRAM_H, (const uint16_t*)p);
        return 1;
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
        if (len % sizeof(uint32_t)) return 0;
        dirty_ram_set_bitmap_words((const uint32_t*)p, len / (uint32_t)sizeof(uint32_t));
        return 1;
    }
    default:
        return 0;  /* unknown tag -> incomplete/foreign format, reject */
    }
}

int boot_state_load(const char* path, uint32_t bios_checksum,
                    uint32_t entry_pc, CPUState* cpu) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;

    BootStateHeader h;
    if (fread(&h, sizeof h, 1, f) != 1) { fclose(f); return 0; }

    /* Integrity key — ANY mismatch rejects the snapshot (rebuild-proof):
     * a user/app update changes codegen_hash / abi_tag / codegen_ver, so a
     * stale snapshot can never silently load into a new build. */
    if (h.magic         != BOOT_STATE_MAGIC               ||
        h.version       != BOOT_STATE_VERSION             ||
        h.bios_checksum != bios_checksum                  ||
        h.entry_pc      != entry_pc                       ||
        h.codegen_hash  != (uint32_t)PSX_OVERLAY_CODEGEN_HASH ||
        h.abi_tag       != (int32_t)PSX_OVERLAY_ABI_TAG   ||
        h.codegen_ver   != (uint32_t)PSX_OVERLAY_CODEGEN_VER) {
        fclose(f);
        fprintf(stdout, "boot_state: snapshot absent/stale -> normal boot + recapture\n");
        return 0;
    }

    /* Required-section coverage: every section must be present and applied, or
     * the restore is incomplete and we fall back to a normal boot. */
    const uint32_t required =
        (1u<<BS_SEC_CPU)|(1u<<BS_SEC_RAM)|(1u<<BS_SEC_SPAD)|(1u<<BS_SEC_IRQ)|
        (1u<<BS_SEC_TIMER)|(1u<<BS_SEC_CLOCK)|(1u<<BS_SEC_GPU)|(1u<<BS_SEC_VRAM)|
        (1u<<BS_SEC_SPU)|(1u<<BS_SEC_SPURAM)|(1u<<BS_SEC_CDROM)|(1u<<BS_SEC_DMA)|
        (1u<<BS_SEC_SIO)|(1u<<BS_SEC_DIRTY);
    uint32_t seen = 0;
    int ok = 1;

    for (uint32_t i = 0; ok && i < h.section_count; i++) {
        uint32_t shdr[2];
        uint64_t len;
        if (fread(shdr, sizeof shdr, 1, f) != 1) { ok = 0; break; }
        if (fread(&len, sizeof len, 1, f) != 1)  { ok = 0; break; }
        if (len > 64u * 1024u * 1024u)            { ok = 0; break; } /* sanity cap */
        uint8_t* buf = (uint8_t*)malloc(len ? (size_t)len : 1);
        if (!buf) { ok = 0; break; }
        if (len && fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); ok = 0; break; }
        uint32_t tag = shdr[0];
        if (!apply_section(tag, buf, (uint32_t)len, cpu, entry_pc)) ok = 0;
        else if (tag < 32) seen |= (1u << tag);
        free(buf);
    }
    fclose(f);

    if (!ok || (seen & required) != required) {
        fprintf(stdout, "boot_state: snapshot incomplete -> normal boot + recapture\n");
        return 0;
    }

    fprintf(stdout, "boot_state: complete snapshot restored, entering game at 0x%08X\n",
            entry_pc);
    return 1;
}

/* ============================ capture trigger ============================ */

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
