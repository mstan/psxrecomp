#include "netplay_state_digest.h"
#include "cdrom.h"
#include "crc32.h"
#include "dirty_ram_interp.h"
#include "gpu.h"
#include "interrupts.h"
#include "psx_cycles.h"
#include "psx_icache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t* memory_get_ram_ptr(void);
extern uint32_t i_stat;
extern uint32_t i_mask;
extern void timers_get_snapshot(uint16_t counter[3], uint32_t mode[3],
                                uint16_t target[3], int32_t irq_line[3],
                                uint32_t frac[3]);
extern uint32_t gpu_snapshot_bytes(void);
extern void     gpu_snapshot_write(uint8_t *p);
extern uint32_t spu_snapshot_bytes(void);
extern void     spu_snapshot_write(uint8_t *p);
extern uint8_t* spu_get_ram_ptr(void);
extern uint32_t spu_get_ram_bytes(void);
extern uint32_t mdec_snapshot_bytes(void);
extern void     mdec_snapshot_write(uint8_t *p);
extern uint8_t *memory_get_scratchpad_ptr(void);
extern uint32_t dma_snapshot_bytes(void);
extern void     dma_snapshot_write(uint8_t *p);
extern uint32_t sio_snapshot_bytes(void);
extern void     sio_snapshot_write(uint8_t *p);
extern void     sio_snapshot_section_ends(uint32_t out[5]);

#define NP_SPAD_SIZE 1024u

static uint32_t digest_module(uint32_t (*bytes_fn)(void), void (*write_fn)(uint8_t *))
{
    static uint8_t *buf;
    static uint32_t cap;
    uint32_t n;
    if (!bytes_fn || !write_fn)
        return 0u;
    n = bytes_fn();
    if (n == 0u)
        return 0u;
    if (n > cap) {
        uint8_t *nbuf = (uint8_t *)realloc(buf, n);
        if (!nbuf)
            return 0u;
        buf = nbuf;
        cap = n;
    }
    write_fn(buf);
    return crc32_compute(buf, n);
}

#define NP_RAM_SIZE (2u * 1024u * 1024u)

uint32_t netplay_cdrom_digest(void)
{
    static uint8_t *buf;
    static uint32_t cap;
    uint32_t n = cdrom_snapshot_bytes();
    if (n == 0u)
        return 0u;
    if (n > cap) {
        uint8_t *nbuf = (uint8_t *)realloc(buf, n);
        if (!nbuf)
            return 0u;
        buf = nbuf;
        cap = n;
    }
    cdrom_snapshot_write(buf);
    return crc32_compute(buf, n);
}

uint32_t netplay_av_digest(void)
{
    /* GPU regs + full VRAM — GL/VK readback was forking this while core RAM
     * still matched (pin zlib sizes ~1.33M vs ~1.12M). */
    static uint8_t *gbuf;
    static uint32_t gcap;
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t gn = gpu_snapshot_bytes();
    const uint16_t *vram = gpu_get_vram();

    if (gn > 0u) {
        if (gn > gcap) {
            uint8_t *nbuf = (uint8_t *)realloc(gbuf, gn);
            if (!nbuf)
                return 0u;
            gbuf = nbuf;
            gcap = gn;
        }
        gpu_snapshot_write(gbuf);
        crc = crc32_update(crc, gbuf, gn);
    }
    if (vram)
        crc = crc32_update(crc, (const uint8_t *)vram, 1024u * 512u * 2u);
    return crc ^ 0xFFFFFFFFu;
}

void netplay_core_digest_parts(const CPUState* cpu, NetplayCoreParts* out)
{
    uint32_t crc_cpu = 0xFFFFFFFFu;
    uint32_t crc_clk = 0xFFFFFFFFu;
    uint32_t crc_tim = 0xFFFFFFFFu;
    uint32_t crc_ram = 0xFFFFFFFFu;
    uint32_t crc_drt = 0xFFFFFFFFu;
    uint32_t fold = 0xFFFFFFFFu;
    uint16_t counter[3], target[3];
    uint32_t mode[3], frac[3];
    int32_t irq_line[3];
    uint32_t wc;
    uint32_t i;
    uint8_t* ram;
    uint32_t cpu_h, clk_h, tim_h, ram_h, drt_h;

    if (out)
        memset(out, 0, sizeof(*out));
    if (!cpu)
        return;

    /* Single pass per partition, then fold the five digests into core.
     * Old path streamed every byte into BOTH part CRCs and a combined CRC —
     * 2× full RDRAM (~4 MiB CRC/frame) and dominated rollback FPS. */
    crc_cpu = crc32_update(crc_cpu, (const uint8_t*)cpu->gpr, sizeof(cpu->gpr));
    crc_cpu = crc32_update(crc_cpu, (const uint8_t*)&cpu->pc, sizeof(cpu->pc));
    crc_cpu = crc32_update(crc_cpu, (const uint8_t*)&cpu->hi, sizeof(cpu->hi));
    crc_cpu = crc32_update(crc_cpu, (const uint8_t*)&cpu->lo, sizeof(cpu->lo));
    crc_cpu = crc32_update(crc_cpu, (const uint8_t*)&cpu->cop0[12], sizeof(uint32_t));
    crc_cpu = crc32_update(crc_cpu, (const uint8_t*)&cpu->cop0[13], sizeof(uint32_t));
    crc_cpu = crc32_update(crc_cpu, (const uint8_t*)&cpu->cop0[14], sizeof(uint32_t));
    crc_cpu = crc32_update(crc_cpu, (const uint8_t*)cpu->gte_data, sizeof(cpu->gte_data));
    crc_cpu = crc32_update(crc_cpu, (const uint8_t*)cpu->gte_ctrl, sizeof(cpu->gte_ctrl));

    {
        uint64_t cyc = psx_cycle_count;
        uint32_t csv = interrupts_get_cycles_since_vblank();
        crc_clk = crc32_update(crc_clk, (const uint8_t*)&cyc, sizeof(cyc));
        crc_clk = crc32_update(crc_clk, (const uint8_t*)&i_stat, sizeof(i_stat));
        crc_clk = crc32_update(crc_clk, (const uint8_t*)&i_mask, sizeof(i_mask));
        /* VBlank phase was invisible to FRAME_COMMIT — peers could agree on
         * core while holding different csv, then resim from zeroed phase. */
        crc_clk = crc32_update(crc_clk, (const uint8_t*)&csv, sizeof(csv));
        /* I-cache tags travel in BS_SEC_ICACHE; fold them so cache asymmetry
         * (fetch-cost fork) surfaces at compare time, not as a resim abort. */
        crc_clk = crc32_update(crc_clk, (const uint8_t*)g_psx_icache_tv,
                               sizeof(g_psx_icache_tv));
    }

    timers_get_snapshot(counter, mode, target, irq_line, frac);
    crc_tim = crc32_update(crc_tim, (const uint8_t*)counter, sizeof(counter));
    crc_tim = crc32_update(crc_tim, (const uint8_t*)mode, sizeof(mode));
    crc_tim = crc32_update(crc_tim, (const uint8_t*)target, sizeof(target));
    crc_tim = crc32_update(crc_tim, (const uint8_t*)irq_line, sizeof(irq_line));
    crc_tim = crc32_update(crc_tim, (const uint8_t*)frac, sizeof(frac));

    ram = memory_get_ram_ptr();
    if (ram)
        crc_ram = crc32_update(crc_ram, ram, NP_RAM_SIZE);

    wc = dirty_ram_get_bitmap_word_count();
    for (i = 0; i < wc; i++) {
        uint32_t w = dirty_ram_get_bitmap_word(i);
        crc_drt = crc32_update(crc_drt, (const uint8_t*)&w, sizeof(w));
    }

    cpu_h = crc_cpu ^ 0xFFFFFFFFu;
    clk_h = crc_clk ^ 0xFFFFFFFFu;
    tim_h = crc_tim ^ 0xFFFFFFFFu;
    ram_h = crc_ram ^ 0xFFFFFFFFu;
    drt_h = crc_drt ^ 0xFFFFFFFFu;
    fold = crc32_update(fold, (const uint8_t*)&cpu_h, sizeof(cpu_h));
    fold = crc32_update(fold, (const uint8_t*)&clk_h, sizeof(clk_h));
    fold = crc32_update(fold, (const uint8_t*)&tim_h, sizeof(tim_h));
    fold = crc32_update(fold, (const uint8_t*)&ram_h, sizeof(ram_h));
    fold = crc32_update(fold, (const uint8_t*)&drt_h, sizeof(drt_h));

    if (out) {
        out->cpu = cpu_h;
        out->clock_irq = clk_h;
        out->timers = tim_h;
        out->ram = ram_h;
        out->dirty = drt_h;
        out->core = fold ^ 0xFFFFFFFFu;
    }
}

uint32_t netplay_core_digest(const CPUState* cpu)
{
    NetplayCoreParts p;
    netplay_core_digest_parts(cpu, &p);
    return p.core;
}

uint32_t netplay_spu_regs_digest(void)
{
    return digest_module(spu_snapshot_bytes, spu_snapshot_write);
}

static int spu_dig_regs_only(void)
{
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("PSX_RB_SPU_DIG_REGS_ONLY");
        s = (e && e[0] == '1' && e[1] == '\0') ? 1 : 0;
        if (s) {
            fprintf(stderr,
                    "psxrecomp: rb SPU digest REGS-ONLY "
                    "(PSX_RB_SPU_DIG_REGS_ONLY=1 — skip SPU RAM in spu/aux)\n");
            fflush(stderr);
        }
    }
    return s;
}

uint32_t netplay_spu_digest(void)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t regs = netplay_spu_regs_digest();
    const uint8_t *ram;
    uint32_t ram_n;

    crc = crc32_update(crc, (const uint8_t *)&regs, sizeof(regs));
    if (spu_dig_regs_only())
        return crc ^ 0xFFFFFFFFu;
    ram = spu_get_ram_ptr();
    ram_n = spu_get_ram_bytes();
    if (ram && ram_n)
        crc = crc32_update(crc, ram, ram_n);
    return crc ^ 0xFFFFFFFFu;
}

uint32_t netplay_mdec_digest(void)
{
    return digest_module(mdec_snapshot_bytes, mdec_snapshot_write);
}

uint32_t netplay_aux_digest(void)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t spu = netplay_spu_digest();
    uint32_t mdec = netplay_mdec_digest();
    crc = crc32_update(crc, (const uint8_t *)&spu, sizeof(spu));
    crc = crc32_update(crc, (const uint8_t *)&mdec, sizeof(mdec));
    return crc ^ 0xFFFFFFFFu;
}

uint32_t netplay_spad_digest(void)
{
    const uint8_t *sp = memory_get_scratchpad_ptr();
    uint32_t crc = 0xFFFFFFFFu;
    if (sp)
        crc = crc32_update(crc, sp, NP_SPAD_SIZE);
    return crc ^ 0xFFFFFFFFu;
}

uint32_t netplay_dma_digest(void)
{
    return digest_module(dma_snapshot_bytes, dma_snapshot_write);
}

uint32_t netplay_sio_digest(void)
{
    /* Fold only through fsm_pace — exclude host-audit meta (byte_seq tracks
     * unreseeded sio_trace_seq and was false-forking MotK Replay digests). */
    static uint8_t *buf;
    static uint32_t cap;
    uint32_t n = sio_snapshot_bytes();
    uint32_t ends[5];
    uint32_t crc;

    if (!n) return 0;
    if (n > cap) {
        uint8_t *nb = (uint8_t *)realloc(buf, n);
        if (!nb) return 0;
        buf = nb;
        cap = n;
    }
    sio_snapshot_write(buf);
    sio_snapshot_section_ends(ends);
    if (ends[4] != n || ends[3] > ends[4])
        return digest_module(sio_snapshot_bytes, sio_snapshot_write);
    crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, buf, ends[3]);
    return crc ^ 0xFFFFFFFFu;
}

void netplay_sio_digest_parts(NetplaySioParts *out)
{
    static uint8_t *buf;
    static uint32_t cap;
    uint32_t n = sio_snapshot_bytes();
    uint32_t ends[5];
    uint32_t crc;

    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!n) return;
    if (n > cap) {
        uint8_t *nb = (uint8_t *)realloc(buf, n);
        if (!nb) return;
        buf = nb;
        cap = n;
    }
    sio_snapshot_write(buf);
    sio_snapshot_section_ends(ends);
    if (ends[4] != n || ends[0] > ends[1] || ends[1] > ends[2] ||
        ends[2] > ends[3] || ends[3] > ends[4])
        return; /* layout drifted — leave parts zero rather than lie */

    crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, buf, ends[0]);
    out->regs = crc ^ 0xFFFFFFFFu;
    crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, buf + ends[0], ends[1] - ends[0]);
    out->pads = crc ^ 0xFFFFFFFFu;
    crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, buf + ends[1], ends[2] - ends[1]);
    out->mc = crc ^ 0xFFFFFFFFu;
    crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, buf + ends[2], ends[3] - ends[2]);
    out->pace = crc ^ 0xFFFFFFFFu;
    crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, buf + ends[3], ends[4] - ends[3]);
    out->meta = crc ^ 0xFFFFFFFFu;
}

uint32_t netplay_baseline_ext_digest(void)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t aux = netplay_aux_digest();
    uint32_t cd = netplay_cdrom_digest();
    uint32_t spad = netplay_spad_digest();
    uint32_t dma = netplay_dma_digest();
    uint32_t sio = netplay_sio_digest();
    crc = crc32_update(crc, (const uint8_t *)&aux, sizeof(aux));
    crc = crc32_update(crc, (const uint8_t *)&cd, sizeof(cd));
    crc = crc32_update(crc, (const uint8_t *)&spad, sizeof(spad));
    crc = crc32_update(crc, (const uint8_t *)&dma, sizeof(dma));
    crc = crc32_update(crc, (const uint8_t *)&sio, sizeof(sio));
    return crc ^ 0xFFFFFFFFu;
}

uint32_t netplay_master_digest(const CPUState* cpu)
{
    uint32_t crc;
    uint32_t core;
    uint32_t cd;

    if (!cpu) return 0;
    /* Fold core + CD as raw words so audit can compare partitions. */
    core = netplay_core_digest(cpu);
    cd = netplay_cdrom_digest();
    crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, (const uint8_t*)&core, sizeof(core));
    crc = crc32_update(crc, (const uint8_t*)&cd, sizeof(cd));
    return crc ^ 0xFFFFFFFFu;
}
