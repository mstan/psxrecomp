#include "boot_state.h"
#include "overlay_api.h"   /* PSX_OVERLAY_CODEGEN_HASH / _ABI_TAG / _CODEGEN_VER */
#include "dirty_ram_interp.h"
#include "gpu.h"           /* gpu_get_vram — CPU-auth mirror under dual-raster   */
#include "gpu_render.h"    /* gr_vram_transfer_in / gr_vram_transfer_out          */
#include "gpu_vram_dirty.h"
#include "cpu_state.h"     /* gte_canonicalize_cpu_state after CPU wire restore   */
#include "interrupts.h"
#include "psx_cycles.h"
#include "psx_icache.h"    /* g_psx_icache_tv — fetch-cost tags in BS_SEC_ICACHE */
#include "psx_ram.h"
#include "pst_wire.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "tex_pack.h"
#include "mod_plugins.h"
#if defined(_WIN32)
#  include <windows.h>
#else
#  include <time.h>
#endif

static double boot_state_mono_ms(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
#endif
}

/* Compress payloads at/above this size (RAM/VRAM/SPU/dirty dominate I/O). */
#define BOOT_STATE_ZLIB_MIN 256u

#define SPAD_SIZE  (1024u)
#define VRAM_W     1024
#define VRAM_H     512
#define VRAM_SIZE  ((uint32_t)(VRAM_W * VRAM_H * 2))  /* 1 MB, 16bpp */

/* ---- core accessors (existing runtime modules) ---- */
extern uint8_t*  memory_get_ram_ptr(void);
extern uint32_t  memory_get_ram_bytes(void);
extern uint8_t*  memory_get_scratchpad_ptr(void);
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
extern uint32_t mdec_snapshot_bytes(void);
extern void     mdec_snapshot_write(uint8_t* p);
extern int      mdec_snapshot_read(const uint8_t* p, uint32_t len);

/* CPU regs wire: 32+3+32+32+32 LE u32 = 131 * 4 = 524 bytes (no padding). */
#define CPU_REGS_WIRE_BYTES (524u)
/* Timer wire: 3*u16 + 3*u32 + 3*u16 + 3*i32 + 3*u32 = 48 bytes (no pad holes). */
#define TIMER_REGS_WIRE_BYTES (48u)

/* ---- deferred capture state (armed before first boot, fired at handoff) ---- */
static char     s_capture_path[512];
static uint32_t s_capture_checksum;
static uint32_t s_capture_entry_pc;

/* §96: persistent VRAM mirror for raw ring snaps — patch dirty scanlines only. */
static uint16_t s_vram_mirror[VRAM_W * VRAM_H];
static int      s_vram_mirror_valid;
static uint32_t s_last_vram_dirty_rows;
static int      s_last_vram_incremental;

uint32_t boot_state_last_vram_dirty_rows(void)
{
    return s_last_vram_dirty_rows;
}

int boot_state_last_vram_incremental(void)
{
    return s_last_vram_incremental;
}

void boot_state_vram_mirror_reset(void)
{
    s_vram_mirror_valid = 0;
    s_last_vram_dirty_rows = VRAM_H;
    s_last_vram_incremental = 0;
}

/* Build s_vram_mirror from live CPU VRAM using dirty rows when possible.
 * Always emits a full 1 MiB BS_SEC_VRAM (loads stay independent).
 * Only used while gpu_vram_dirty_tracking() (rollback netplay). */
static int sync_vram_mirror_for_save(void)
{
    const uint16_t *live = gpu_get_vram();
    uint32_t dirty_n;
    uint32_t y;

    if (!gpu_vram_dirty_tracking()) {
        /* Should not be called offline — full refresh fallback. */
        if (live)
            memcpy(s_vram_mirror, live, VRAM_SIZE);
        else
            gr_vram_transfer_out(0, 0, VRAM_W, VRAM_H, s_vram_mirror);
        s_vram_mirror_valid = 1;
        s_last_vram_dirty_rows = VRAM_H;
        s_last_vram_incremental = 0;
        return 1;
    }

    dirty_n = gpu_vram_dirty_row_count();
    s_last_vram_dirty_rows = dirty_n;

    if (!live) {
        gr_vram_transfer_out(0, 0, VRAM_W, VRAM_H, s_vram_mirror);
        s_vram_mirror_valid = 1;
        s_last_vram_dirty_rows = VRAM_H;
        s_last_vram_incremental = 0;
        gpu_vram_dirty_clear();
        return 1;
    }

    if (!s_vram_mirror_valid || dirty_n >= VRAM_H) {
        memcpy(s_vram_mirror, live, VRAM_SIZE);
        s_vram_mirror_valid = 1;
        s_last_vram_incremental = 0;
    } else if (dirty_n == 0u) {
        /* Mirror already matches live. */
        s_last_vram_incremental = 1;
    } else {
        const uint64_t *mask = gpu_vram_dirty_mask();
        for (y = 0; y < VRAM_H; y++) {
            if (mask[y >> 6] & ((uint64_t)1u << (y & 63u))) {
                memcpy(s_vram_mirror + (size_t)y * VRAM_W,
                       live + (size_t)y * VRAM_W,
                       (size_t)VRAM_W * sizeof(uint16_t));
            }
        }
        s_last_vram_incremental = 1;
    }

    if (gpu_vram_dirty_verify_enabled()) {
        uint16_t *full = (uint16_t *)malloc(VRAM_SIZE);
        if (full) {
            gr_vram_transfer_out(0, 0, VRAM_W, VRAM_H, full);
            if (memcmp(full, s_vram_mirror, VRAM_SIZE) != 0) {
                fprintf(stderr,
                        "psxrecomp: VRAM dirty VERIFY FAIL dirty_rows=%u "
                        "incr=%d — forcing full mirror\n",
                        (unsigned)dirty_n, s_last_vram_incremental);
                fflush(stderr);
                memcpy(s_vram_mirror, full, VRAM_SIZE);
                s_last_vram_incremental = 0;
                s_last_vram_dirty_rows = VRAM_H;
            }
            free(full);
        }
    }

    gpu_vram_dirty_clear();
    return 1;
}

/* File or growable memory sink — both save paths share one serializer. */
typedef struct BsOut {
    FILE*    f;       /* non-NULL => write to file */
    uint8_t* data;    /* memory sink (owned by caller / save_buffer) */
    size_t   len;
    size_t   cap;
    int      no_zlib; /* 1 => always raw sections (netplay snap ring) */
    uint32_t sections;/* sections actually written; backpatched into the header.
                       * The old hardcoded section_count=16 made every appended
                       * section a poison pill: the loader stopped at 16 and a
                       * LATER section's bytes read as garbage (the TEXPACK trap). */
} BsOut;

static int bs_write(BsOut* o, const void* p, size_t n) {
    if (!n) return 1;
    if (o->f)
        return fwrite(p, 1, n, o->f) == n;
    if (o->len + n > o->cap) {
        size_t nc = o->cap ? o->cap * 2u : (256u * 1024u);
        uint8_t* nd;
        while (nc < o->len + n) {
            if (nc > (SIZE_MAX / 2u)) return 0;
            nc *= 2u;
        }
        nd = (uint8_t*)realloc(o->data, nc);
        if (!nd) return 0;
        o->data = nd;
        o->cap = nc;
    }
    memcpy(o->data + o->len, p, n);
    o->len += n;
    return 1;
}

static int write_header_le(BsOut* o, const BootStateHeader* h) {
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
    return bs_write(o, buf, sizeof buf);
}

static int write_section_raw(BsOut* o, uint32_t tag, uint32_t flags,
                             const void* data, uint64_t len) {
    uint8_t hdr[16];
    PstW w;
    pst_w_init(&w, hdr, sizeof hdr);
    if (!pst_w_u32(&w, tag) || !pst_w_u32(&w, flags) || !pst_w_u64(&w, len))
        return 0;
    if (!bs_write(o, hdr, sizeof hdr)) return 0;
    if (len && !bs_write(o, data, (size_t)len)) return 0;
    o->sections++;
    return 1;
}

/* Prefer zlib for large blobs (smaller disk + faster load on slow storage).
 * Falls back to raw if compressBound/compress fails.
 * o->no_zlib skips compress entirely (in-memory netplay ring). */
static int write_section(BsOut* o, uint32_t tag, const void* data, uint64_t len) {
    if (!data && len) return 0;
    if (!o->no_zlib && len >= BOOT_STATE_ZLIB_MIN && len <= 0xffffffffu) {
        uLong bound = compressBound((uLong)len);
        uint8_t* packed = (uint8_t*)malloc(4u + (size_t)bound);
        if (packed) {
            PstW lw;
            uLong dest_len = bound;
            pst_w_init(&lw, packed, 4);
            if (pst_w_u32(&lw, (uint32_t)len) &&
                compress2(packed + 4, &dest_len, (const Bytef*)data, (uLong)len,
                          Z_BEST_SPEED) == Z_OK) {
                uint64_t payload = 4u + (uint64_t)dest_len;
                int ok = write_section_raw(o, tag, BOOT_STATE_SEC_ZLIB,
                                           packed, payload);
                free(packed);
                return ok;
            }
            free(packed);
        }
    }
    return write_section_raw(o, tag, 0u, data, len);
}

static int write_module_section(BsOut* o, uint32_t tag,
                                uint32_t (*bytes)(void),
                                void (*write)(uint8_t*)) {
    uint32_t n = bytes();
    uint8_t* buf = (uint8_t*)malloc(n ? n : 1);
    if (!buf) return 0;
    write(buf);
    int ok = write_section(o, tag, buf, n);
    free(buf);
    return ok;
}

static int write_cpu_section(BsOut* o, const CPUState* cpu) {
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
    return write_section(o, BS_SEC_CPU, buf, CPU_REGS_WIRE_BYTES);
}

static int write_timer_section(BsOut* o) {
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
    return write_section(o, BS_SEC_TIMER, buf, TIMER_REGS_WIRE_BYTES);
}

/* ============================ SAVE ============================ */

/* Rewind's async-readback override: when set, the next full-VRAM section is
 * serialized from this caller-owned buffer (a PBO readback kicked one frame
 * earlier) instead of gr_vram_transfer_out's synchronous GL pipeline drain.
 * One-shot per save; the caller clears it after boot_state_save_buffer_raw. */
static const uint16_t *s_vram_save_override = NULL;
void boot_state_set_vram_override(const uint16_t *vram)
{
    s_vram_save_override = vram;
}

/* Classic full VRAM section (offline / zlib / tracking off). */
static int write_vram_section_full(BsOut *o)
{
    uint16_t *vbuf = (uint16_t *)malloc(VRAM_SIZE);
    int ok;
    if (!vbuf)
        return 0;
    if (s_vram_save_override)
        memcpy(vbuf, s_vram_save_override, VRAM_SIZE);
    else
        gr_vram_transfer_out(0, 0, VRAM_W, VRAM_H, vbuf);
    s_last_vram_dirty_rows = VRAM_H;
    s_last_vram_incremental = 0;
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    ok = write_section(o, BS_SEC_VRAM, vbuf, VRAM_SIZE);
#else
    {
        uint8_t *wire = (uint8_t *)malloc(VRAM_SIZE);
        if (!wire) {
            free(vbuf);
            return 0;
        }
        {
            PstW w;
            pst_w_init(&w, wire, VRAM_SIZE);
            ok = pst_w_pod(&w, vbuf, VRAM_SIZE, 2) &&
                 write_section(o, BS_SEC_VRAM, wire, VRAM_SIZE);
        }
        free(wire);
    }
#endif
    free(vbuf);
    return ok;
}

static int boot_state_save_to(BsOut* o, const CPUState* cpu,
                              uint32_t bios_checksum, uint32_t entry_pc) {
    BootStateHeader h;
    int ok;
    memset(&h, 0, sizeof h);
    h.magic         = BOOT_STATE_MAGIC;
    h.version       = BOOT_STATE_VERSION;
    h.bios_checksum = bios_checksum;
    h.entry_pc      = entry_pc;
    h.codegen_hash  = (uint32_t)PSX_OVERLAY_CODEGEN_HASH;
    h.abi_tag       = (int32_t)PSX_OVERLAY_ABI_TAG;
    h.codegen_ver   = (uint32_t)PSX_OVERLAY_CODEGEN_VER;
    /* Backpatched after the last section lands (see below). */
    h.section_count = 0;

    size_t hdr_off = o->f ? (size_t)ftell(o->f) : o->len;
    o->sections = 0;
    ok = write_header_le(o, &h);

    /* Mod-plan guard rides FIRST: applying it is a pure compare, so a
     * mismatched load rejects before any machine state is touched. */
    if (ok) {
        const char* fp = psx_mod_runtime_fingerprint_cstr();
        ok = write_section(o, BS_SEC_MODSET, fp, (uint64_t)strlen(fp));
    }

    if (ok) ok = write_cpu_section(o, cpu);
    if (ok) ok = write_section(o, BS_SEC_RAM,  memory_get_ram_ptr(),        memory_get_ram_bytes());
    if (ok) ok = write_section(o, BS_SEC_SPAD, memory_get_scratchpad_ptr(), SPAD_SIZE);
    if (ok) {
        /* 12B: i_stat, i_mask, cycles_since_vblank. Zeroing csv on warm load
         * rebased every tip to phase 0 and forked MotK wait-loop resim
         * (IRQ at CD54 vs CDA0). Selfcheck already restored csv out-of-band. */
        uint8_t irq[12];
        PstW w;
        pst_w_init(&w, irq, sizeof irq);
        ok = pst_w_u32(&w, i_stat) && pst_w_u32(&w, i_mask) &&
             pst_w_u32(&w, interrupts_get_cycles_since_vblank()) &&
             write_section(o, BS_SEC_IRQ, irq, 12);
    }
    if (ok) ok = write_timer_section(o);
    if (ok) {
        uint8_t cyc[8];
        PstW w;
        pst_w_init(&w, cyc, sizeof cyc);
        /* Publish deferred load-charge batch before snapshotting the clock. */
        psx_cyc_batch_flush();
        ok = pst_w_u64(&w, psx_cycle_count) &&
             write_section(o, BS_SEC_CLOCK, cyc, 8);
    }
    if (ok) ok = write_module_section(o, BS_SEC_GPU, gpu_snapshot_bytes, gpu_snapshot_write);
    if (ok) {
        /* §96 incremental mirror only while RB dirty-tracking is on.
         * Offline / delay-sync / zlib disk: classic full transfer_out. */
        if (o->no_zlib && gpu_vram_dirty_tracking()) {
            ok = sync_vram_mirror_for_save();
            if (ok) {
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
                ok = write_section(o, BS_SEC_VRAM, s_vram_mirror, VRAM_SIZE);
#else
                {
                    uint8_t* wire = (uint8_t*)malloc(VRAM_SIZE);
                    if (!wire) ok = 0;
                    else {
                        PstW w;
                        pst_w_init(&w, wire, VRAM_SIZE);
                        ok = pst_w_pod(&w, s_vram_mirror, VRAM_SIZE, 2) &&
                             write_section(o, BS_SEC_VRAM, wire, VRAM_SIZE);
                        free(wire);
                    }
                }
#endif
            }
        } else {
            ok = write_vram_section_full(o);
        }
    }
    if (ok) ok = write_module_section(o, BS_SEC_SPU, spu_snapshot_bytes, spu_snapshot_write);
    if (ok) ok = write_section(o, BS_SEC_SPURAM, spu_get_ram_ptr(), spu_get_ram_bytes());
    if (ok) ok = write_module_section(o, BS_SEC_CDROM, cdrom_snapshot_bytes, cdrom_snapshot_write);
    if (ok) ok = write_module_section(o, BS_SEC_DMA,   dma_snapshot_bytes,   dma_snapshot_write);
    if (ok) ok = write_module_section(o, BS_SEC_SIO,   sio_snapshot_bytes,   sio_snapshot_write);
    if (ok) ok = write_module_section(o, BS_SEC_MDEC,  mdec_snapshot_bytes,  mdec_snapshot_write);
    if (ok) {
        /* I-cache tags: warm loads must replay with the fetch-cost state the
         * live timeline had, or miss cycles differ per peer/retry and IRQ
         * delivery forks a few wait-loop iterations (MotK abort@940). */
        uint8_t ib[1024u * 4u];
        PstW w;
        pst_w_init(&w, ib, sizeof ib);
        ok = 1;
        for (uint32_t i = 0; ok && i < 1024u; i++)
            ok = pst_w_u32(&w, g_psx_icache_tv[i]);
        if (ok) ok = write_section(o, BS_SEC_ICACHE, ib, sizeof ib);
    }
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
            if (ok) ok = write_section(o, BS_SEC_DIRTY, db, nbytes);
            free(db);
        }
    }
    /* HD texture pack tracker: rects+hashes only; pixels rebuild from the
     * restored VRAM at apply (see tex_pack_state_apply). Zero bytes when the
     * pack is inactive — the section is simply absent. */
    if (ok) {
        uint32_t tb = tex_pack_state_bytes();
        if (tb) {
            uint8_t* buf = (uint8_t*)malloc(tb);
            if (!buf) ok = 0;
            else {
                tex_pack_state_write(buf);
                ok = write_section(o, BS_SEC_TEXPACK, buf, tb);
                free(buf);
            }
        }
    }
    /* Backpatch the real section count over the header's placeholder. */
    if (ok) {
        uint8_t le[4];
        le[0] = (uint8_t)(o->sections & 0xFF);
        le[1] = (uint8_t)((o->sections >> 8) & 0xFF);
        le[2] = (uint8_t)((o->sections >> 16) & 0xFF);
        le[3] = (uint8_t)((o->sections >> 24) & 0xFF);
        if (o->f) {
            long end_pos = ftell(o->f);
            if (end_pos < 0 ||
                fseek(o->f, (long)hdr_off + 28, SEEK_SET) != 0 ||
                fwrite(le, 1, 4, o->f) != 4 ||
                fseek(o->f, end_pos, SEEK_SET) != 0)
                ok = 0;
        } else {
            if (hdr_off + 32 <= o->len)
                memcpy(o->data + hdr_off + 28, le, 4);
            else
                ok = 0;
        }
    }
    return ok;
}

int boot_state_save(const CPUState* cpu, uint32_t bios_checksum,
                    uint32_t entry_pc, const char* path) {
    BsOut o;
    FILE* f = fopen(path, "wb");
    int ok;
    if (!f) return 0;
    memset(&o, 0, sizeof o);
    o.f = f;
    ok = boot_state_save_to(&o, cpu, bios_checksum, entry_pc);
    fclose(f);
    if (!ok)
        remove(path);
    return ok;
}

static int boot_state_save_buffer_ex(const CPUState* cpu, uint32_t bios_checksum,
                                     uint32_t entry_pc, uint8_t** out_data,
                                     size_t* out_len, int no_zlib) {
    BsOut o;
    if (!out_data || !out_len) return 0;
    *out_data = NULL;
    *out_len = 0;
    memset(&o, 0, sizeof o);
    o.no_zlib = no_zlib ? 1 : 0;
    /* Compressed MotK ~1.3–1.5 MiB; raw ~3.5–4 MiB (2 MB RAM) or ~9.5 MiB (8 MB). */
    o.cap = no_zlib ? (12u * 1024u * 1024u) : (2u * 1024u * 1024u);
    o.data = (uint8_t*)malloc(o.cap);
    if (!o.data) return 0;
    if (!boot_state_save_to(&o, cpu, bios_checksum, entry_pc)) {
        free(o.data);
        return 0;
    }
    *out_data = o.data;
    *out_len = o.len;
    return 1;
}

int boot_state_save_buffer(const CPUState* cpu, uint32_t bios_checksum,
                           uint32_t entry_pc, uint8_t** out_data,
                           size_t* out_len) {
    return boot_state_save_buffer_ex(cpu, bios_checksum, entry_pc, out_data,
                                     out_len, 0);
}

int boot_state_save_buffer_raw(const CPUState* cpu, uint32_t bios_checksum,
                               uint32_t entry_pc, uint8_t** out_data,
                               size_t* out_len) {
    return boot_state_save_buffer_ex(cpu, bios_checksum, entry_pc, out_data,
                                     out_len, 1);
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
        /* Architectural normalize + drop host-only projection provenance that
         * belonged to the pre-load timeline (not part of the wire format). */
        gte_canonicalize_cpu_state(cpu);
        return 1;
    }
    case BS_SEC_RAM:
        if (len != memory_get_ram_bytes()) return 0;
        memcpy(memory_get_ram_ptr(), p, memory_get_ram_bytes());
        {
            extern void psx_kernel_bless_note_range(uint32_t phys, uint32_t l);
            psx_kernel_bless_note_range(0, memory_get_ram_bytes());
            psx_ram_resync_high_after_restore();
        }
        return 1;
    case BS_SEC_SPAD:
        if (len != SPAD_SIZE) return 0;
        memcpy(memory_get_scratchpad_ptr(), p, SPAD_SIZE);
        return 1;
    case BS_SEC_IRQ: {
        PstR r;
        uint32_t st, mk, csv;
        if (len != 8 && len != 12) return 0;
        pst_r_init(&r, p, len);
        if (!pst_r_u32(&r, &st) || !pst_r_u32(&r, &mk)) return 0;
        i_stat = st;
        i_mask = mk;
        if (len == 12) {
            if (!pst_r_u32(&r, &csv)) return 0;
            interrupts_set_cycles_since_vblank(csv);
        } else {
            /* Legacy UI/disk snaps: no phase — rebase like pre-csv saves. */
            interrupts_set_cycles_since_vblank(0);
        }
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
        if (len != VRAM_SIZE) return 0;
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
        /* Wire == host layout: upload straight from the section buffer. */
        gr_vram_transfer_in(0, 0, VRAM_W, VRAM_H, (const uint16_t*)p);
        if (gpu_vram_dirty_tracking()) {
            memcpy(s_vram_mirror, p, VRAM_SIZE);
            s_vram_mirror_valid = 1;
            gpu_vram_dirty_clear();
        } else {
            s_vram_mirror_valid = 0;
        }
        return 1;
#else
        {
            uint16_t* vbuf;
            PstR r;
            vbuf = (uint16_t*)malloc(VRAM_SIZE);
            if (!vbuf) return 0;
            pst_r_init(&r, p, len);
            if (!pst_r_pod(&r, vbuf, VRAM_SIZE, 2)) {
                free(vbuf);
                return 0;
            }
            gr_vram_transfer_in(0, 0, VRAM_W, VRAM_H, vbuf);
            if (gpu_vram_dirty_tracking()) {
                memcpy(s_vram_mirror, vbuf, VRAM_SIZE);
                s_vram_mirror_valid = 1;
                gpu_vram_dirty_clear();
            } else {
                s_vram_mirror_valid = 0;
            }
            free(vbuf);
            return 1;
        }
#endif
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
    case BS_SEC_MDEC:
        return mdec_snapshot_read(p, len);
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
    case BS_SEC_ICACHE: {
        PstR r;
        if (len != 1024u * 4u) return 0;
        pst_r_init(&r, p, len);
        for (uint32_t i = 0; i < 1024u; i++)
            if (!pst_r_u32(&r, &g_psx_icache_tv[i])) return 0;
        return 1;
    }
    case BS_SEC_MODSET: {
        /* Pure compare — no state touched. A state saved under a different
         * enabled mod set must not apply: RAM layout / patched code paths
         * differ and the machine resumes into garbage (observed as a null-PC
         * recovery spin). Section order puts this first, so the reject lands
         * before any mutation. */
        const char* live = psx_mod_runtime_fingerprint_cstr();
        size_t live_len = strlen(live);
        if (live_len != (size_t)len || (len && memcmp(live, p, (size_t)len) != 0)) {
            fprintf(stderr,
                    "savestate: REFUSED - state's mod set differs from this "
                    "session's (state %.*s... vs live %.8s...). Relaunch with "
                    "the matching mods enabled to load it.\n",
                    (int)(len < 8 ? len : 8), (const char*)p, live);
            return 0;
        }
        return 1;
    }
    case BS_SEC_TEXPACK: {
        /* Applies AFTER BS_SEC_VRAM in stream order; pixels rebuild from the
         * freshly restored CPU VRAM mirror and are hash-verified inside. */
        tex_pack_state_apply(p, len, NULL);
        return 1;
    }
    default:
        /* Unknown section: SKIP, never fail. This was `return 0`, which made
         * every state written by a build with one extra section a poison pill
         * for every build without it -- and worse than unloadable: the apply
         * loop had already restored RAM/VRAM/CPU by the time it hit the
         * unknown tag, so the refusal left a HALF-RESTORED machine that
         * wedged or died at PC=0. A sectioned state format exists precisely
         * so readers can step over what they do not know. */
        return 1;
    }
}

static int boot_state_parse_header(const uint8_t* file, size_t file_len,
                                   BootStateHeader* h_out) {
    PstR hr;
    if (!file || !h_out || file_len < BOOT_STATE_HEADER_WIRE_BYTES ||
        file_len > 64u * 1024u * 1024u) {
        return 0;
    }
    pst_r_init(&hr, file, BOOT_STATE_HEADER_WIRE_BYTES);
    memset(h_out, 0, sizeof(*h_out));
    if (!pst_r_u32(&hr, &h_out->magic) ||
        !pst_r_u32(&hr, &h_out->version) ||
        !pst_r_u32(&hr, &h_out->bios_checksum) ||
        !pst_r_u32(&hr, &h_out->entry_pc) ||
        !pst_r_u32(&hr, &h_out->codegen_hash) ||
        !pst_r_i32(&hr, &h_out->abi_tag) ||
        !pst_r_u32(&hr, &h_out->codegen_ver) ||
        !pst_r_u32(&hr, &h_out->section_count) ||
        !pst_r_u32(&hr, &h_out->reserved)) {
        return 0;
    }
    return 1;
}

static void boot_state_append_reason(char* reason, size_t reason_cap,
                                     const char* part) {
    size_t n;
    if (!reason || reason_cap == 0 || !part || !part[0]) return;
    n = strlen(reason);
    if (n + 1 >= reason_cap) return;
    if (n > 0) {
        reason[n++] = ',';
        reason[n] = '\0';
        if (n + 1 >= reason_cap) return;
    }
    snprintf(reason + n, reason_cap - n, "%s", part);
}

int boot_state_check_buffer(const uint8_t* file, size_t file_len,
                            uint32_t bios_checksum, uint32_t entry_pc,
                            char* reason, size_t reason_cap) {
    BootStateHeader h;
    char part[96];

    if (reason && reason_cap)
        reason[0] = '\0';

    if (!file || file_len < BOOT_STATE_HEADER_WIRE_BYTES) {
        boot_state_append_reason(reason, reason_cap, "missing_or_truncated");
        return 0;
    }
    if (file_len > 64u * 1024u * 1024u) {
        boot_state_append_reason(reason, reason_cap, "too_large");
        return 0;
    }
    if (!boot_state_parse_header(file, file_len, &h)) {
        boot_state_append_reason(reason, reason_cap, "header_parse");
        return 0;
    }

    if (h.magic != BOOT_STATE_MAGIC) {
        snprintf(part, sizeof(part), "magic=%08X(want %08X)",
                 (unsigned)h.magic, (unsigned)BOOT_STATE_MAGIC);
        boot_state_append_reason(reason, reason_cap, part);
    }
    if (h.version < BOOT_STATE_VERSION_MIN_READ ||
        h.version > BOOT_STATE_VERSION) {
        snprintf(part, sizeof(part), "version=%u(want %u..%u)",
                 (unsigned)h.version, (unsigned)BOOT_STATE_VERSION_MIN_READ,
                 (unsigned)BOOT_STATE_VERSION);
        boot_state_append_reason(reason, reason_cap, part);
    }
    if (h.bios_checksum != bios_checksum) {
        snprintf(part, sizeof(part), "bios=%08X(want %08X)",
                 (unsigned)h.bios_checksum, (unsigned)bios_checksum);
        boot_state_append_reason(reason, reason_cap, part);
    }
    if (h.entry_pc != entry_pc) {
        snprintf(part, sizeof(part), "entry=%08X(want %08X)",
                 (unsigned)h.entry_pc, (unsigned)entry_pc);
        boot_state_append_reason(reason, reason_cap, part);
    }
    if (h.codegen_hash != (uint32_t)PSX_OVERLAY_CODEGEN_HASH) {
        snprintf(part, sizeof(part), "codegen_hash=%08X(want %08X)",
                 (unsigned)h.codegen_hash,
                 (unsigned)PSX_OVERLAY_CODEGEN_HASH);
        boot_state_append_reason(reason, reason_cap, part);
    }
    if (h.abi_tag != (int32_t)PSX_OVERLAY_ABI_TAG) {
        snprintf(part, sizeof(part), "abi_tag=%d(want %d)",
                 (int)h.abi_tag, (int)PSX_OVERLAY_ABI_TAG);
        boot_state_append_reason(reason, reason_cap, part);
    }
    if (h.codegen_ver != (uint32_t)PSX_OVERLAY_CODEGEN_VER) {
        snprintf(part, sizeof(part), "codegen_ver=%u(want %u)",
                 (unsigned)h.codegen_ver, (unsigned)PSX_OVERLAY_CODEGEN_VER);
        boot_state_append_reason(reason, reason_cap, part);
    }

    if (reason && reason_cap && reason[0])
        return 0;
    return 1;
}

int boot_state_load_buffer(const uint8_t* file, size_t file_len,
                           uint32_t bios_checksum, uint32_t entry_pc,
                           CPUState* cpu) {
    const uint8_t* cur;
    const uint8_t* end;
    BootStateHeader h;
    char reject[256];
    const uint32_t required =
        (1u<<BS_SEC_CPU)|(1u<<BS_SEC_RAM)|(1u<<BS_SEC_SPAD)|(1u<<BS_SEC_IRQ)|
        (1u<<BS_SEC_TIMER)|(1u<<BS_SEC_CLOCK)|(1u<<BS_SEC_GPU)|(1u<<BS_SEC_VRAM)|
        (1u<<BS_SEC_SPU)|(1u<<BS_SEC_SPURAM)|(1u<<BS_SEC_CDROM)|(1u<<BS_SEC_DMA)|
        (1u<<BS_SEC_SIO)|(1u<<BS_SEC_MDEC)|(1u<<BS_SEC_DIRTY);
    uint32_t seen = 0;
    int ok = 1;
    const double t0 = boot_state_mono_ms();
    double inflate_ms = 0.0;
    double apply_ram_ms = 0.0;
    double apply_vram_ms = 0.0;
    double apply_spuram_ms = 0.0;
    double apply_other_ms = 0.0;

    if (!boot_state_check_buffer(file, file_len, bios_checksum, entry_pc,
                                 reject, sizeof(reject))) {
        fprintf(stderr, "boot_state: reject — %s\n",
                reject[0] ? reject : "unknown");
        return 0;
    }
    if (!boot_state_parse_header(file, file_len, &h))
        return 0;

    cur = file + BOOT_STATE_HEADER_WIRE_BYTES;
    end = file + file_len;

    for (uint32_t i = 0; ok && i < h.section_count; i++) {
        PstR sh;
        uint32_t tag = 0, pad = 0;
        uint64_t len = 0;
        const uint8_t* payload;
        uint8_t* inflated = NULL;
        const uint8_t* apply_ptr;
        uint32_t apply_len;
        double t_sec;

        if ((size_t)(end - cur) < 16u) { ok = 0; break; }
        pst_r_init(&sh, cur, 16);
        if (!pst_r_u32(&sh, &tag) || !pst_r_u32(&sh, &pad) || !pst_r_u64(&sh, &len)) {
            ok = 0; break;
        }
        cur += 16;
        if (len > 64u * 1024u * 1024u || (uint64_t)(end - cur) < len) {
            ok = 0; break;
        }
        payload = cur;
        cur += (size_t)len;

        if (h.version >= 4u && pad == BOOT_STATE_SEC_ZLIB) {
            PstR lr;
            uint32_t raw_len = 0;
            uLong dest_len;
            double t_inf;
            if (len < 4u) { ok = 0; break; }
            pst_r_init(&lr, payload, 4);
            if (!pst_r_u32(&lr, &raw_len) || raw_len == 0 ||
                raw_len > 64u * 1024u * 1024u) {
                ok = 0; break;
            }
            inflated = (uint8_t*)malloc(raw_len);
            if (!inflated) { ok = 0; break; }
            dest_len = (uLong)raw_len;
            t_inf = boot_state_mono_ms();
            if (uncompress(inflated, &dest_len, payload + 4,
                           (uLong)(len - 4u)) != Z_OK ||
                dest_len != (uLong)raw_len) {
                free(inflated);
                ok = 0;
                break;
            }
            inflate_ms += boot_state_mono_ms() - t_inf;
            apply_ptr = inflated;
            apply_len = raw_len;
        } else if (pad != 0u) {
            /* v3 requires pad==0; v4 unknown/extra flags are a hard reject. */
            ok = 0;
            break;
        } else {
            if (len > 0xffffffffu) { ok = 0; break; }
            apply_ptr = payload;
            apply_len = (uint32_t)len;
        }

        t_sec = boot_state_mono_ms();
        if (!apply_section(tag, apply_ptr, apply_len, cpu, entry_pc)) ok = 0;
        else if (tag < 32) seen |= (1u << tag);
        {
            double dt = boot_state_mono_ms() - t_sec;
            if (tag == BS_SEC_RAM) apply_ram_ms += dt;
            else if (tag == BS_SEC_VRAM) apply_vram_ms += dt;
            else if (tag == BS_SEC_SPURAM) apply_spuram_ms += dt;
            else apply_other_ms += dt;
        }
        free(inflated);
    }

    if (!ok || (seen & required) != required)
        return 0;

    /* RAM was memcpy'd; force overlay revalidation before resume. */
    overlay_watch_invalidate_after_ram_restore();

    {
        const double total_ms = boot_state_mono_ms() - t0;
        fprintf(stderr,
                "savestate: load_timing read=0.0 inflate=%.1f "
                "apply_ram=%.1f apply_vram=%.1f apply_spuram=%.1f "
                "apply_other=%.1f total=%.1f ms (file=%zu)\n",
                inflate_ms,
                apply_ram_ms, apply_vram_ms, apply_spuram_ms,
                apply_other_ms, total_ms, file_len);
    }
    return 1;
}

int boot_state_load(const char* path, uint32_t bios_checksum,
                    uint32_t entry_pc, CPUState* cpu) {
    FILE* f = fopen(path, "rb");
    long sz;
    uint8_t* file = NULL;
    size_t file_len = 0;
    int ok;
    const double t0 = boot_state_mono_ms();
    double t_after_read;

    if (!f) {
        fprintf(stderr, "boot_state: reject — missing %s\n",
                path ? path : "(null)");
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    sz = ftell(f);
    if (sz < (long)BOOT_STATE_HEADER_WIRE_BYTES || sz > 64L * 1024L * 1024L) {
        fprintf(stderr, "boot_state: reject — bad size %ld for %s\n",
                sz, path ? path : "(null)");
        fclose(f);
        return 0;
    }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    file_len = (size_t)sz;
    file = (uint8_t*)malloc(file_len);
    if (!file) { fclose(f); return 0; }
    if (fread(file, 1, file_len, f) != file_len) {
        free(file);
        fclose(f);
        return 0;
    }
    fclose(f);
    t_after_read = boot_state_mono_ms();
    (void)t0;
    (void)t_after_read;

    ok = boot_state_load_buffer(file, file_len, bios_checksum, entry_pc, cpu);
    free(file);
    return ok;
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
