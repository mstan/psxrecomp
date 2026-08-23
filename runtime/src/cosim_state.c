#ifdef PSX_COSIM
/* cosim_state.c — full guest-architectural-state canonical hash. See COSIM_ORACLE.md
 * and cosim_state.h. Active ONLY in the PSX_COSIM build; empty TU otherwise (safe in
 * the shared source list).
 *
 * Principle: hash exactly the state a real PSX exposes to the running program plus the
 * device-timing quantities that determine WHEN a guest-visible bit flips — never our
 * emulator's internal bookkeeping (e.g. total_checks / dispatch_count / cooldown are
 * counted in psx_check_interrupts CALLS, which legitimately differ between the compiled
 * and interp backends by call frequency; hashing them would report a false first
 * divergence). The genuine timing quantity that matters is cycles_since_vblank (guest
 * cycles), which is included. This exclusion list is refined by the injected-divergence
 * and compiled-vs-compiled gates.
 */
#include "cosim_state.h"
#include <stdlib.h>
#include <string.h>

/* ---- FNV-1a 64 ---- */
#define FNV_OFF 1469598103934665603ULL
#define FNV_PRM 1099511628211ULL
static inline uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= FNV_PRM; }
    return h;
}
static inline uint64_t fnv_u32(uint64_t h, uint32_t v) { return fnv(h, &v, 4); }
static inline uint64_t fnv_u64(uint64_t h, uint64_t v) { return fnv(h, &v, 8); }

/* ---- runtime state accessors (existing modules) ---- */
extern CPUState *debug_cpu_ptr;
extern uint8_t  *memory_get_ram_ptr(void);
extern uint8_t  *memory_get_scratchpad_ptr(void);
extern uint32_t  i_stat;
extern uint32_t  i_mask;
extern uint64_t  psx_cycle_count;
extern uint32_t  dirty_ram_get_bitmap_word(uint32_t word_index);
extern uint32_t  dirty_ram_get_bitmap_word_count(void);
extern void timers_get_snapshot(uint16_t counter[3], uint32_t mode[3],
                                uint16_t target[3], int32_t irq_line[3],
                                uint32_t frac[3]);
extern uint32_t gpu_cosim_snapshot_bytes(void); extern void gpu_cosim_snapshot_write(uint8_t*);
extern uint32_t spu_snapshot_bytes(void);   extern void spu_snapshot_write(uint8_t*);
extern uint8_t* spu_get_ram_ptr(void);      extern uint32_t spu_get_ram_bytes(void);
extern uint32_t cdrom_snapshot_bytes(void); extern void cdrom_snapshot_write(uint8_t*);
extern uint32_t dma_snapshot_bytes(void);   extern void dma_snapshot_write(uint8_t*);
extern uint32_t sio_snapshot_bytes(void);   extern void sio_snapshot_write(uint8_t*);
extern uint32_t sio1_snapshot_bytes(void);  extern void sio1_snapshot_write(uint8_t*);
/* interrupts.c: fold the genuine guest-timing statics (NOT the call-count artifacts). */
extern uint64_t interrupts_cosim_hash(uint64_t seed);
/* renderer-agnostic VRAM readback (sw renderer = cheap memcpy). */
extern void gr_vram_transfer_out(int x, int y, int w, int h, uint16_t *dst);

#define RAM_SIZE   (2u * 1024u * 1024u)
#define SPAD_SIZE  (1024u)
#define PAGE       4096u
#define RAM_PAGES  (RAM_SIZE / PAGE)      /* 512 */
#define VRAM_W 1024
#define VRAM_H 512
#define VRAM_SIZE ((uint32_t)(VRAM_W * VRAM_H * 2))   /* 1 MB */
#define VRAM_PAGES (VRAM_SIZE / PAGE)     /* 256 */

/* ---- incremental RAM page hashes ---- */
static uint64_t s_ram_page_h[RAM_PAGES];
static uint8_t  s_ram_page_dirty[RAM_PAGES];
static int      s_ram_all_dirty = 1;

/* ---- VRAM: full-recompute when dirty (draws are periodic, not per-block) ---- */
static uint64_t s_vram_h;
static int      s_vram_dirty = 1;
static uint16_t s_vram_buf[VRAM_W * VRAM_H];

/* ---- reusable device-blob scratch ---- */
static uint8_t *s_blob;
static uint32_t s_blob_cap;

/* ---- gate-4 injection (applied to LIVE state so it flows into the hash naturally) -- */
static int32_t  s_inj_ram_phys = -1;  static uint8_t s_inj_ram_xor;
static int32_t  s_inj_reg = -1;       static uint32_t s_inj_reg_xor;

void cosim_state_reset(void) {
    memset(s_ram_page_h, 0, sizeof s_ram_page_h);
    memset(s_ram_page_dirty, 0, sizeof s_ram_page_dirty);
    s_ram_all_dirty = 1;
    s_vram_dirty = 1; s_vram_h = 0;
    s_inj_ram_phys = -1; s_inj_reg = -1;
}

void cosim_note_ram_write(uint32_t phys, uint32_t nbytes) {
    if (phys >= RAM_SIZE) return;
    uint32_t last = phys + (nbytes ? nbytes - 1 : 0);
    if (last >= RAM_SIZE) last = RAM_SIZE - 1;
    for (uint32_t p = phys / PAGE; p <= last / PAGE; p++) s_ram_page_dirty[p] = 1;
}
void cosim_note_vram_write(uint32_t byte_off, uint32_t nbytes) {
    (void)byte_off; (void)nbytes; s_vram_dirty = 1;
}
void cosim_inject_ram(uint32_t phys, uint8_t xor_val) { s_inj_ram_phys = (int32_t)phys; s_inj_ram_xor = xor_val; }
void cosim_inject_reg(int reg_index, uint32_t xor_val) { s_inj_reg = reg_index; s_inj_reg_xor = xor_val; }

static uint64_t blob_hash(uint64_t seed, uint32_t (*bytes)(void), void (*write)(uint8_t*)) {
    uint32_t n = bytes();
    if (n > s_blob_cap) { free(s_blob); s_blob = (uint8_t*)malloc(n); s_blob_cap = n; }
    if (n && !s_blob) return seed;   /* OOM: leave unchanged (will surface in gate) */
    if (n) write(s_blob);
    return fnv(fnv_u32(seed, n), s_blob, n);
}

static uint64_t hash_cpu(const CPUState *c) {
    uint64_t h = FNV_OFF;
    uint32_t gte_data[32];
    h = fnv(h, c->gpr, sizeof c->gpr);
    /* cpu->pc is DELIBERATELY EXCLUDED from the cross-backend hash. The compiled
     * backend does not keep cpu->pc current mid-execution — it writes pc only at block
     * transfers, and it is transiently 0 between dispatch calls — whereas the interp
     * keeps pc exact per-instruction. So at a mid-instruction cycle checkpoint the two
     * backends legitimately hold different pc values while being in the SAME
     * architectural state (verified: at the first flagged divergence, cp32, ONLY pc
     * differed; every gpr/cop0/hi/lo/micro-state matched). Including pc produced a false
     * first-divergence. This is NOT a blind spot: a REAL control-flow split shows up as a
     * differing gpr/memory value within one checkpoint. pc stays available via the `cpu`
     * TCP command for reporting. */
    h = fnv_u32(h, c->hi); h = fnv_u32(h, c->lo);
    h = fnv(h, c->cop0, sizeof c->cop0);
    memcpy(gte_data, c->gte_data, sizeof gte_data);
    /* Some GTE data registers are derived/read-special values. The compiled
     * backend reads these through gte_read_data(), while the dirty interpreter's
     * all-purpose gte_write_data() refreshes their backing cache as a side
     * effect. Hash the same canonical value the guest would read so stale cache
     * bits do not become oracle false positives. */
    CPUState *mc = (CPUState *)c;
    for (int i = 8; i <= 11; i++) gte_data[i] = gte_read_data(mc, (uint8_t)i);
    gte_data[15] = gte_read_data(mc, 15);
    gte_data[28] = gte_read_data(mc, 28);
    gte_data[29] = gte_read_data(mc, 29);
    gte_data[31] = gte_read_data(mc, 31);
    h = fnv(h, gte_data, sizeof gte_data);
    h = fnv(h, c->gte_ctrl, sizeof c->gte_ctrl);
    /* micro-state — the execution-critical fields boot_state omits */
    h = fnv_u64(h, c->muldiv_ts_done);
    h = fnv_u64(h, c->gte_ts_done);
    h = fnv(h, c->read_absorb, sizeof c->read_absorb);
    h = fnv(h, &c->read_absorb_which, 1);
    h = fnv(h, &c->read_fudge, 1);
    h = fnv(h, &c->ld_which_t, 1);
    h = fnv_u32(h, c->ld_absorb);
    /* EXCLUDED (host-only): read_word/.../write_byte fn ptrs. */
    return h;
}

uint64_t cosim_state_hash(CosimSubHashes *sub) {
    CosimSubHashes s; memset(&s, 0, sizeof s);
    CPUState *cpu = debug_cpu_ptr;
    uint8_t *ram = memory_get_ram_ptr();

    /* apply pending gate-4 injection to live state */
    if (s_inj_ram_phys >= 0 && (uint32_t)s_inj_ram_phys < RAM_SIZE) {
        ram[s_inj_ram_phys] ^= s_inj_ram_xor; cosim_note_ram_write((uint32_t)s_inj_ram_phys, 1);
        s_inj_ram_phys = -1;
    }
    if (s_inj_reg >= 0 && cpu) {
        if (s_inj_reg < 32) cpu->gpr[s_inj_reg] ^= s_inj_reg_xor;
        else if (s_inj_reg == 32) cpu->hi ^= s_inj_reg_xor;
        else if (s_inj_reg == 33) cpu->lo ^= s_inj_reg_xor;
        s_inj_reg = -1;
    }

    if (cpu) s.cpu = hash_cpu(cpu);

    /* RAM: recompute dirty pages, fold page hashes */
    for (uint32_t p = 0; p < RAM_PAGES; p++) {
        if (s_ram_all_dirty || s_ram_page_dirty[p]) {
            s_ram_page_h[p] = fnv(FNV_OFF, ram + (size_t)p * PAGE, PAGE);
            s_ram_page_dirty[p] = 0;
        }
    }
    s_ram_all_dirty = 0;
    s.ram = fnv(FNV_OFF, s_ram_page_h, sizeof s_ram_page_h);

    s.scratch = fnv(FNV_OFF, memory_get_scratchpad_ptr(), SPAD_SIZE);

    /* VRAM: EXCLUDED in v1. The guest state machine reads RAM + device registers to
     * make the FMV/handoff decision, never VRAM, so a VRAM divergence is downstream of
     * (not causal for) the first control-flow split — and a 1MB readback per cycle-
     * checkpoint would be prohibitively slow. Add incrementally (cosim_note_vram_write
     * page hashes) only if a gate shows a divergence that first surfaces in VRAM. */
    s.vram = 0;

    /* interrupt controller: guest-visible i_stat/i_mask + genuine timing statics */
    s.irqctl = interrupts_cosim_hash(fnv_u32(fnv_u32(FNV_OFF, i_stat), i_mask));

    /* timers */
    { uint16_t cnt[3],tgt[3]; uint32_t mode[3],frac[3]; int32_t irq[3];
      timers_get_snapshot(cnt, mode, tgt, irq, frac);
      uint64_t h = FNV_OFF;
      h = fnv(h,cnt,sizeof cnt); h = fnv(h,mode,sizeof mode); h = fnv(h,tgt,sizeof tgt);
      h = fnv(h,irq,sizeof irq); h = fnv(h,frac,sizeof frac); s.timers = h; }

    s.clock = fnv_u64(FNV_OFF, psx_cycle_count);

    s.gpu   = blob_hash(FNV_OFF, gpu_cosim_snapshot_bytes, gpu_cosim_snapshot_write);
    s.spu   = blob_hash(FNV_OFF, spu_snapshot_bytes,   spu_snapshot_write);
    s.spu   = fnv(s.spu, spu_get_ram_ptr(), spu_get_ram_bytes());
    s.cdrom = blob_hash(FNV_OFF, cdrom_snapshot_bytes, cdrom_snapshot_write);
    s.dma   = blob_hash(FNV_OFF, dma_snapshot_bytes,   dma_snapshot_write);
    s.sio   = blob_hash(FNV_OFF, sio_snapshot_bytes,   sio_snapshot_write);
    s.sio1  = blob_hash(FNV_OFF, sio1_snapshot_bytes,  sio1_snapshot_write);

    /* dirty-RAM page bitmap (affects dispatch/interp routing) */
    { uint64_t h = FNV_OFF; uint32_t wc = dirty_ram_get_bitmap_word_count();
      for (uint32_t i = 0; i < wc; i++) h = fnv_u32(h, dirty_ram_get_bitmap_word(i));
      s.dirty = h; }

    if (sub) *sub = s;

    /* fold everything into the top hash */
    uint64_t top = FNV_OFF;
    top = fnv(top, &s, sizeof s);
    return top;
}

#endif /* PSX_COSIM */
