/* load_accel.c -- verified enhancement-phase load-time acceleration.
 *
 * PsyQ's VSync(-1) is used as a timeout clock by CD_sync/CD_ready loops.  The
 * library routine returns its RAM VBlank counter, but its common prologue first
 * reads GPUSTAT and Timer1 even though neither value reaches the mode=-1 result.
 * Those reads are architecturally side-effect-free yet expensive in the host
 * runtime because every MMIO access catches all devices up to the current cycle.
 *
 * This helper reproduces the verified Tomba/PsyQ query path instruction by
 * instruction: load-delay timing, stack traffic, I-cache fetches, and all three
 * interrupt checkpoints.  Only the two unused MMIO handlers are suppressed.
 * Non-query calls, precise diagnostics, and cosim always use the original LLE.
 * Config-verified CD wait-loop call sites may additionally advance guest time
 * to the next deliverable device event.  That elides empty host-side polling
 * while preserving the device clock, event order, and subsequent IRQ delivery.
 */
#include "load_accel.h"

#include "cdrom.h"
#include "cpu_state.h"
#include "interrupts.h"
#include "psx_cyc.h"

#include <stdio.h>

extern uint32_t g_debug_last_store_pc;
extern int g_precise_mode;

static int s_enabled = 1;
static int s_horizon_enabled = 1;
static int s_extra_horizon_enabled = 1;
static int s_horizon_any = 0;
static uint64_t s_calls;
static uint64_t s_handled;
static uint64_t s_fallback_mode;
static uint64_t s_fallback_diagnostic;
static uint64_t s_fallback_layout;
static uint64_t s_horizon_hits;
static uint64_t s_horizon_cycles;
static uint32_t s_horizon_sites[48];
static uint32_t s_horizon_site_count;
static uint64_t s_extra_horizon_hits;
static uint64_t s_extra_horizon_cycles;
static uint32_t s_extra_horizon_sites[48];
static uint32_t s_extra_horizon_site_count;
static uint32_t s_cfg_func;
static uint32_t s_cfg_counter;
static uint32_t s_cfg_gpustat_ptr;
static uint32_t s_cfg_timer1_ptr;
static uint32_t s_cfg_timer1_cache;

#ifdef PSX_ENABLE_BLOCK_CYCLES
#define VQ_STEP(cpu_, mask_) psx_cyc_step((cpu_), (mask_))
#else
#define VQ_STEP(cpu_, mask_) do { (void)(cpu_); (void)(mask_); } while (0)
#endif

void psx_vsync_query_hle_configure(uint32_t func, uint32_t counter_addr,
                                   uint32_t gpustat_ptr_addr,
                                   uint32_t timer1_ptr_addr,
                                   uint32_t timer1_cache_addr) {
    s_cfg_func = func;
    s_cfg_counter = counter_addr;
    s_cfg_gpustat_ptr = gpustat_ptr_addr;
    s_cfg_timer1_ptr = timer1_ptr_addr;
    s_cfg_timer1_cache = timer1_cache_addr;
}

int psx_vsync_query_hle_try(CPUState* cpu, uint32_t dispatch_addr) {
    if (!s_cfg_func || dispatch_addr != s_cfg_func) return 0;
    return psx_vsync_query_hle_enter(cpu, s_cfg_func, s_cfg_counter,
                                     s_cfg_gpustat_ptr, s_cfg_timer1_ptr,
                                     s_cfg_timer1_cache);
}

void psx_vsync_query_hle_set_enabled(int on) { s_enabled = on ? 1 : 0; }
void psx_vsync_query_hle_set_horizon_enabled(int on) {
    s_horizon_enabled = on ? 1 : 0;
}
void psx_vsync_query_hle_set_extra_horizon_enabled(int on) {
    s_extra_horizon_enabled = on ? 1 : 0;
}
void psx_vsync_query_hle_set_horizon_any(int on) { s_horizon_any = on ? 1 : 0; }

void psx_vsync_query_hle_add_event_horizon_site(uint32_t return_pc) {
    if (return_pc == 0u) return;
    for (uint32_t i = 0; i < s_horizon_site_count; i++)
        if (s_horizon_sites[i] == return_pc) return;
    if (s_horizon_site_count <
        (uint32_t)(sizeof(s_horizon_sites) / sizeof(s_horizon_sites[0])))
        s_horizon_sites[s_horizon_site_count++] = return_pc;
}

void psx_vsync_query_hle_add_extra_event_horizon_site(uint32_t return_pc) {
    if (return_pc == 0u) return;
    for (uint32_t i = 0; i < s_extra_horizon_site_count; i++)
        if (s_extra_horizon_sites[i] == return_pc) return;
    if (s_extra_horizon_site_count <
        (uint32_t)(sizeof(s_extra_horizon_sites) /
                   sizeof(s_extra_horizon_sites[0])))
        s_extra_horizon_sites[s_extra_horizon_site_count++] = return_pc;
}

static int is_event_horizon_site(uint32_t return_pc) {
    for (uint32_t i = 0; i < s_horizon_site_count; i++)
        if (s_horizon_sites[i] == return_pc) return 1;
    for (uint32_t i = 0; i < s_extra_horizon_site_count; i++)
        if (s_extra_horizon_sites[i] == return_pc) return 2;
    return 0;
}

int psx_vsync_query_hle_enter(CPUState* cpu, uint32_t func,
                              uint32_t counter_addr, uint32_t gpustat_ptr_addr,
                              uint32_t timer1_ptr_addr,
                              uint32_t timer1_cache_addr) {
    s_calls++;
    if (!s_enabled || cpu->gpr[4] != 0xFFFFFFFFu) {
        s_fallback_mode++;
        return 0;
    }
#ifdef PSX_COSIM
    s_fallback_diagnostic++;
    return 0;
#else
    if (g_precise_mode) {
        s_fallback_diagnostic++;
        return 0;
    }
#endif

    /* Fail closed if the verified PsyQ globals were repointed at runtime. */
    if (cpu->read_word(gpustat_ptr_addr) != 0x1F801814u ||
        cpu->read_word(timer1_ptr_addr) != 0x1F801110u) {
        s_fallback_layout++;
        return 0;
    }

    s_handled++;

    /* Event horizon: verified CD wait-loop return PCs only, and only while
     * cdrom_load_in_progress() (excludes XA/STR). Advancing on every VSync(-1)
     * during MotK mode=0xA2 reads (horizon_any + drive_reading) raced guest
     * time past the movie — mdec_decode_count stayed 0 / display disabled. */
    int horizon_kind = is_event_horizon_site(cpu->gpr[31]);
    int horizon_ok = 0;
    if (s_horizon_enabled && cdrom_load_in_progress()) {
        if (s_horizon_any)
            horizon_ok = 1;
        else if (horizon_kind == 1)
            horizon_ok = 1;
        else if (horizon_kind == 2 && s_extra_horizon_enabled)
            horizon_ok = 1;
    }
    if (horizon_ok) {
        uint32_t dist = cycles_to_next_event();
        /* Pending VBlank in I_STAT makes cycles_to_next_event()==0 even when the
         * CD wait's next sector is far out. Prefer the CD schedule then. */
        if (dist <= 64u) {
            uint32_t cd = cdrom_cycles_to_irq(0xFFFFFFFFu);
            if (cd > 64u && cd != 0xFFFFFFFFu) dist = cd;
        }
        if (dist > 64u && dist != 0xFFFFFFFFu && dist <= 1200000u) {
            psx_advance_cycles(dist);
            if (horizon_kind == 2) {
                s_extra_horizon_hits++;
                s_extra_horizon_cycles += dist;
            } else {
                s_horizon_hits++;
                s_horizon_cycles += dist;
            }
        }
    }

#ifdef PSX_ENABLE_BLOCK_CYCLES
    psx_icache_fetch(cpu, func + 0x00u);
#endif
    VQ_STEP(cpu, 0x4u); cpu->gpr[2] = 0x80090000u;                 /* +00 lui v0 */
    cpu->gpr[2] = psx_cyc_load_word(cpu, gpustat_ptr_addr, 2u, 0x4u); /* +04 lw */
    VQ_STEP(cpu, 0x8u); cpu->gpr[3] = 0x80090000u;                 /* +08 lui v1 */
    cpu->gpr[3] = psx_cyc_load_word(cpu, timer1_ptr_addr, 3u, 0x8u); /* +0C lw */

#ifdef PSX_ENABLE_BLOCK_CYCLES
    psx_icache_fetch(cpu, func + 0x10u);
#endif
    VQ_STEP(cpu, 0x20000000u); cpu->gpr[29] -= 32u;                /* +10 addiu sp */
    VQ_STEP(cpu, 0xA0000000u); g_debug_last_store_pc = func + 0x14u;
    cpu->write_word(cpu->gpr[29] + 24u, cpu->gpr[31]);              /* +14 sw ra */
    VQ_STEP(cpu, 0x20020000u); g_debug_last_store_pc = func + 0x18u;
    cpu->write_word(cpu->gpr[29] + 20u, cpu->gpr[17]);              /* +18 sw s1 */
    VQ_STEP(cpu, 0x20010000u); g_debug_last_store_pc = func + 0x1Cu;
    cpu->write_word(cpu->gpr[29] + 16u, cpu->gpr[16]);              /* +1C sw s0 */

#ifdef PSX_ENABLE_BLOCK_CYCLES
    psx_icache_fetch(cpu, func + 0x20u);
#endif
    psx_cyc_load_word_timing_only(cpu, 0x1F801814u, 16u, 0x4u);     /* +20 GPUSTAT */
    cpu->gpr[16] = 0u;                                              /* value unused */
    psx_cyc_load_word_timing_only(cpu, 0x1F801110u, 2u, 0x8u);      /* +24 Timer1 */
    cpu->gpr[2] = 0u;                                               /* value unused */
    VQ_STEP(cpu, 0x8u); cpu->gpr[3] = 0x80090000u;                 /* +28 lui v1 */
    cpu->gpr[3] = psx_cyc_load_word(cpu, timer1_cache_addr, 3u, 0x8u); /* +2C */

#ifdef PSX_ENABLE_BLOCK_CYCLES
    psx_icache_fetch(cpu, func + 0x30u);
#endif
    VQ_STEP(cpu, 0x1u);                                             /* +30 nop */
    VQ_STEP(cpu, 0xCu); cpu->gpr[2] -= cpu->gpr[3];                 /* +34 subu */
    VQ_STEP(cpu, 0x10u);                                           /* +38 bgez a0 */
    VQ_STEP(cpu, 0x20004u); cpu->gpr[17] = cpu->gpr[2] & 0xFFFFu;  /* +3C */
    psx_check_interrupts_at(cpu, func + 0x40u);

#ifdef PSX_ENABLE_BLOCK_CYCLES
    psx_icache_fetch(cpu, func + 0x40u);
#endif
    VQ_STEP(cpu, 0x4u); cpu->gpr[2] = 0x80090000u;                 /* +40 lui v0 */
    cpu->gpr[2] = psx_cyc_load_word(cpu, counter_addr, 2u, 0x4u);   /* +44 counter */
    VQ_STEP(cpu, 0x0u);                                            /* +48 j epilogue */
    VQ_STEP(cpu, 0x1u);                                            /* +4C nop */
    psx_check_interrupts_at(cpu, func + 0x130u);

#ifdef PSX_ENABLE_BLOCK_CYCLES
    psx_icache_fetch(cpu, func + 0x130u);
#endif
    cpu->gpr[31] = psx_cyc_load_word(cpu, cpu->gpr[29] + 24u, 31u, 0x20000000u);
    cpu->gpr[17] = psx_cyc_load_word(cpu, cpu->gpr[29] + 20u, 17u, 0x20000000u);
    cpu->gpr[16] = psx_cyc_load_word(cpu, cpu->gpr[29] + 16u, 16u, 0x20000000u);
    VQ_STEP(cpu, 0x20000000u); cpu->gpr[29] += 32u;
#ifdef PSX_ENABLE_BLOCK_CYCLES
    psx_icache_fetch(cpu, func + 0x140u);
#endif
    {
        uint32_t ra = cpu->gpr[31];
        VQ_STEP(cpu, 0x80000001u);                                 /* jr ra */
        VQ_STEP(cpu, 0x1u);                                        /* delay nop */
        psx_check_interrupts_at(cpu, ra);
        cpu->pc = ra;
    }
    return 1;
}

void psx_vsync_query_hle_stats_json(char* buf, int cap) {
    snprintf(buf, (size_t)cap,
             "\"enabled\":%d,\"horizon_enabled\":%d,"
             "\"extra_horizon_enabled\":%d,\"horizon_any\":%d,"
             "\"calls\":%llu,\"handled\":%llu,"
             "\"fallback_mode\":%llu,\"fallback_diagnostic\":%llu,"
             "\"fallback_layout\":%llu,\"horizon_sites\":%u,"
             "\"horizon_hits\":%llu,\"horizon_cycles\":%llu,"
             "\"extra_horizon_sites\":%u,\"extra_horizon_hits\":%llu,"
             "\"extra_horizon_cycles\":%llu",
             s_enabled, s_horizon_enabled, s_extra_horizon_enabled, s_horizon_any,
             (unsigned long long)s_calls,
             (unsigned long long)s_handled,
             (unsigned long long)s_fallback_mode,
             (unsigned long long)s_fallback_diagnostic,
             (unsigned long long)s_fallback_layout, s_horizon_site_count,
             (unsigned long long)s_horizon_hits,
             (unsigned long long)s_horizon_cycles,
             s_extra_horizon_site_count,
             (unsigned long long)s_extra_horizon_hits,
             (unsigned long long)s_extra_horizon_cycles);
}
