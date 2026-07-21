#include "overlay_api.h"

/* ---- Overlay dispatch shim (inserted by compile_overlays.py) ----------- */
static OverlayCallbacks g_cbs;
static uint32_t s_pending_cycles;

#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
void overlay_flush_cycles(void) {
    uint32_t cycles = s_pending_cycles;
    s_pending_cycles = 0;
    if (cycles && g_cbs.advance_cycles) g_cbs.advance_cycles(cycles);
}

/* Call-contract state (ABI v2): generated code reads/bumps the runtime's
 * bail state through these pointers (cpu_state.h PSX_OVERLAY_DLL_BUILD
 * branch).  They start at local dummies so a pre-init call can't fault. */
static int      s_bail_dummy;
static uint64_t s_ctr_dummy0, s_ctr_dummy1;
int      *g_psx_call_bail_p    = &s_bail_dummy;
uint64_t *g_psx_bail_first_p   = &s_ctr_dummy0;
uint64_t *g_psx_bail_resolved_p = &s_ctr_dummy1;

#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
int overlay_abi(void) { return PSX_OVERLAY_ABI_VERSION | (PSX_OVERLAY_FLAVOR << 16); }

#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
void overlay_init(const OverlayCallbacks *cbs) {
    g_cbs = *cbs;
    if (g_cbs.call_bail_flag) g_psx_call_bail_p    = g_cbs.call_bail_flag;
    if (g_cbs.bail_first)     g_psx_bail_first_p   = g_cbs.bail_first;
    if (g_cbs.bail_resolved)  g_psx_bail_resolved_p = g_cbs.bail_resolved;
}

void psx_dispatch_call(CPUState *cpu, uint32_t addr, uint32_t ra) {
    overlay_flush_cycles();
    (void)ra;
    g_cbs.dispatch_call(cpu, addr, cpu->gpr[31]);
}
void psx_check_interrupts(CPUState *cpu) {
    overlay_flush_cycles();
    g_cbs.check_interrupts(cpu);
}
void psx_check_interrupts_at(CPUState *cpu, uint32_t resume_pc) {
    overlay_flush_cycles();
    if (g_cbs.check_interrupts_at) g_cbs.check_interrupts_at(cpu, resume_pc);
    else g_cbs.check_interrupts(cpu);
}
void psx_advance_cycles(uint32_t cycles) {
    if (cycles > UINT32_MAX - s_pending_cycles) overlay_flush_cycles();
    s_pending_cycles += cycles;
}
void gte_execute(CPUState *cpu, uint32_t cmd) {
    overlay_flush_cycles();
    g_cbs.gte_execute(cpu, cmd);
}
int psx_syscall(CPUState *cpu, uint32_t code) {
    overlay_flush_cycles();
    return g_cbs.psx_syscall(cpu, code);
}
void psx_native_bad_entry(CPUState *cpu, uint32_t owner, uint32_t pc) {
    overlay_flush_cycles();
    if (g_cbs.psx_native_bad_entry) g_cbs.psx_native_bad_entry(cpu, owner, pc);
}
void psx_unknown_dispatch(CPUState *cpu, uint32_t addr, uint32_t phys) {
    overlay_flush_cycles();
    g_cbs.psx_unknown_dispatch(cpu, addr, phys);
}
void debug_server_log_call_entry(uint32_t func_addr) {
    if (g_cbs.log_call_entry) g_cbs.log_call_entry(func_addr);
}
void psx_restore_state_escape(void) {
    overlay_flush_cycles();
    if (g_cbs.psx_restore_state_escape) g_cbs.psx_restore_state_escape();
}
/* RFE escape marker (ABI v12): emitted at every `rfe`, including in
 * overlay-resident exception-context code. Mutates the runtime's shared
 * RFE-pending state, so it MUST forward (a local no-op would swallow
 * exception returns). Unconditional — v12 hosts always supply it; the
 * ABI gate rejects mixes that don't. */
void psx_rfe_mark_escape(void) {
    overlay_flush_cycles();
    g_cbs.rfe_mark_escape();
}
/* Faithful-timing functions (ABI v9): overlay code built with PSX_ENABLE_BLOCK_CYCLES
 * emits these; forward to the runtime's real impls so native overlays charge cycles on
 * the SAME timeline as the interp/BIOS (a local copy would diverge). NULL-guarded so a
 * v9 DLL stays safe on a host that predates a given callback. */
uint32_t psx_cyc_load_word(CPUState *cpu, uint32_t addr, uint32_t rt, uint32_t reg_mask) {
    overlay_flush_cycles();
    return g_cbs.cyc_load_word ? g_cbs.cyc_load_word(cpu, addr, rt, reg_mask) : cpu->read_word(addr);
}
uint16_t psx_cyc_load_half(CPUState *cpu, uint32_t addr, uint32_t rt, uint32_t reg_mask) {
    overlay_flush_cycles();
    return g_cbs.cyc_load_half ? g_cbs.cyc_load_half(cpu, addr, rt, reg_mask) : cpu->read_half(addr);
}
uint8_t psx_cyc_load_byte(CPUState *cpu, uint32_t addr, uint32_t rt, uint32_t reg_mask) {
    overlay_flush_cycles();
    return g_cbs.cyc_load_byte ? g_cbs.cyc_load_byte(cpu, addr, rt, reg_mask) : cpu->read_byte(addr);
}
uint32_t psx_cyc_lwc2_read(CPUState *cpu, uint32_t addr) {
    overlay_flush_cycles();
    return g_cbs.cyc_lwc2_read ? g_cbs.cyc_lwc2_read(cpu, addr) : cpu->read_word(addr);
}
void psx_icache_fetch(CPUState *cpu, uint32_t addr) {
    overlay_flush_cycles();
    if (g_cbs.icache_fetch) g_cbs.icache_fetch(cpu, addr);
}
void psx_muldiv_set(CPUState *cpu, uint32_t latency) {
    overlay_flush_cycles();
    if (g_cbs.muldiv_set) g_cbs.muldiv_set(cpu, latency);
}
void psx_muldiv_stall(CPUState *cpu) {
    overlay_flush_cycles();
    if (g_cbs.muldiv_stall) g_cbs.muldiv_stall(cpu);
}
uint32_t psx_mult_latency_s(uint32_t rs) {
    return g_cbs.mult_latency_s ? g_cbs.mult_latency_s(rs) : 0u;
}
uint32_t psx_mult_latency_u(uint32_t rs) {
    return g_cbs.mult_latency_u ? g_cbs.mult_latency_u(rs) : 0u;
}
void psx_gte_stall(CPUState *cpu) {
    overlay_flush_cycles();
    if (g_cbs.gte_stall) g_cbs.gte_stall(cpu);
}
void psx_gte_read(CPUState *cpu, uint32_t rt) {
    overlay_flush_cycles();
    if (g_cbs.gte_read) g_cbs.gte_read(cpu, rt);
}
int psx_slice_block(CPUState *cpu, uint32_t block_addr, uint32_t bcyc, int side_effects) {
    overlay_flush_cycles();
    return g_cbs.slice_block ? g_cbs.slice_block(cpu, block_addr, bcyc, side_effects) : 0;
}
/* GTE special-register accessors (ABI v10). The emitter routes masked,
 * sign-extended, aliased/FIFO, read-only, precision-sensitive, flag/IR, and
 * derived registers through these as DIRECT calls, so any
 * GTE-heavy overlay (Tomba2 lava / Trolley attract demos) fails to link
 * without them — undefined gte_read_ctrl was the signature that silently
 * blocked all new overlay coverage. GTE state is SHARED runtime state; a
 * local fallback would fork that timeline, so these forward
 * unconditionally (ABI v10 hosts always supply them; the v10 gate rejects
 * DLL/host mixes that don't).
 * Proven bit-transparent registers retain direct CPUState-array fast paths;
 * the shared emitter classification header is the source of truth. */
uint32_t gte_read_data(CPUState *cpu, uint8_t reg) {
    overlay_flush_cycles();
    return g_cbs.gte_read_data(cpu, reg);
}
uint32_t gte_read_ctrl(CPUState *cpu, uint8_t reg) {
    overlay_flush_cycles();
    return g_cbs.gte_read_ctrl(cpu, reg);
}
void gte_write_data(CPUState *cpu, uint8_t reg, uint32_t value) {
    overlay_flush_cycles();
    g_cbs.gte_write_data(cpu, reg, value);
}
void gte_write_ctrl(CPUState *cpu, uint8_t reg, uint32_t value) {
    overlay_flush_cycles();
    g_cbs.gte_write_ctrl(cpu, reg, value);
}
/* g_debug_last_store_pc: a provenance breadcrumb the emitter writes before stores.
 * Overlays are built PSX_NO_DEBUG_TOOLS (the runtime's only consumer is debug tooling),
 * so a local definition satisfies the link; overlay-store provenance is simply not
 * tracked in a no-debug-tools build (by design). */
uint32_t g_debug_last_store_pc;
/* Widescreen hooks (ABI v3): forward to the runtime's live widescreen state.
 * Identity fallback if the host predates the callbacks (NULL) — so a v3 DLL
 * stays correct (4:3) on an older host. */
int psx_ws_backdrop_x(int x) {
    return g_cbs.ws_backdrop_x ? g_cbs.ws_backdrop_x(x) : (int)(short)x;
}
int psx_ws_x_margin(void) {
    return g_cbs.ws_x_margin ? g_cbs.ws_x_margin() : 0;
}
/* Shared render-funnel screen-X cull widening ([widescreen.cull] auto_screen_x).
 * The recompiler emits `rt = psx_ws_cull_sltiu(sx, imm)` at every width-compare
 * cull site, INCLUDING in overlay-resident code, so the overlay DLL must define
 * it or it fails to link (undefined reference) -> no native shard -> that overlay
 * variant runs interpreted forever (the dwarf-village-style lag). Self-contained:
 * depends only on psx_ws_x_margin() above. Identity at 4:3 (margin 0). MUST stay
 * byte-for-byte identical to gpu.c psx_ws_cull_sltiu. */
int psx_ws_cull_sltiu(uint32_t sx, uint32_t imm) {
    int m = psx_ws_x_margin();
    return ((uint32_t)((int32_t)(int16_t)(uint16_t)sx + m)
            < (uint32_t)((int32_t)imm + 2 * m)) ? 1 : 0;
}
/* Signed-funnel variants (min/max + center±halfwidth idioms). Same contract:
 * self-contained, identity at margin 0, MUST stay byte-for-byte identical to
 * the gpu.c implementations. */
int psx_ws_cull_slti(uint32_t sx, uint32_t imm) {
    return ((int32_t)sx < (int32_t)imm + psx_ws_x_margin()) ? 1 : 0;
}
int psx_ws_cull_bltz(uint32_t v) {
    return ((int32_t)v < -psx_ws_x_margin()) ? 1 : 0;
}
int psx_ws_cull_vxrange(uint32_t x, uint32_t imm) {
    int32_t margin = psx_ws_x_margin();
    uint32_t bound = (uint32_t)(int32_t)(int16_t)(uint16_t)imm;
    return (((x + (uint32_t)margin) & 0xFFFFu) <
            (bound + 2u * (uint32_t)margin)) ? 1 : 0;
}
void psx_ws_sprite_tag(CPUState *cpu) {
    if (g_cbs.ws_sprite_tag) g_cbs.ws_sprite_tag(cpu);
}
/* Widescreen backdrop column-preload value (ABI v4). Identity fallback if the
 * host predates the callback, so the DLL stays correct (4:3) on an older host. */
uint32_t psx_ws_backdrop_value(uint32_t orig, int is_end, int window_cols) {
    return g_cbs.ws_backdrop_value ? g_cbs.ws_backdrop_value(orig, is_end, window_cols) : orig;
}
/* Typed native-wide signed player-X bound (ABI v13). The emitter rewrites a
 * configured [widescreen] signed_x_bound LUI site into
 * psx_ws_player_x_bound(vanilla) (Einhander player bounds), including in
 * overlay-resident code, so the DLL must define it or the shard fails to link
 * (undefined reference — the rfe/gte_read_ctrl class). Computes against the
 * host's live widescreen state; identity fallback (the vanilla constant, i.e.
 * 4:3 behavior) if the host predates the callback. */
int32_t psx_ws_player_x_bound(int32_t vanilla) {
    return g_cbs.ws_player_x_bound ? g_cbs.ws_player_x_bound(vanilla) : vanilla;
}
int32_t psx_ws_depth_bound(int32_t imm) {
    return g_cbs.ws_depth_bound ? g_cbs.ws_depth_bound(imm) : imm;
}
/* Sub-pixel GTE precision-store tracker (ABI v14). The emitter emits a direct
 * gte_precision_store_word() call for every swc2 (GTE store-word), including in
 * overlay-resident code, so the DLL must define it or the shard fails to link
 * (undefined reference — the rfe/gte_read_ctrl class). Forwards to the host's
 * stateful tracker (gte.cpp); no-op if the host predates the callback
 * (precision tracking is a widescreen enhancement, off by default — 4:3
 * behavior is unaffected). */
void gte_precision_store_word(uint32_t addr, uint8_t reg) {
    if (g_cbs.gte_precision_store_word) g_cbs.gte_precision_store_word(addr, reg);
}
/* ----------------------------------------------------------------------- */
