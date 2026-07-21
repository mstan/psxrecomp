#include "gte.h"
#include "cpu_state.h"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace PSXRecomp {
namespace GTE {

// ---------------------------------------------------------------------------
// Common helpers
// ---------------------------------------------------------------------------

// Perspective division: H / SZ3, scaled, saturated to 17 bits.
//
// The real PS1 GTE does NOT compute an exact H*0x20000/SZ3. It uses an
// Unsigned Newton-Raphson (UNR) reciprocal approximation driven by a 257-entry
// seed table (documented in PSX-SPX "GTE Division Inaccuracy"; identical to the
// mednafen/Beetle oracle in beetle-psx/mednafen/psx/gte.cpp). Exact division
// diverges from hardware by +/-1 (occasionally up to a few units) on ~25% of
// inputs, which is enough to flip games' distance/intensity threshold branches
// (e.g. Ape Escape's additive-glow CLUT semi-transparency bit). This is the
// faithful hardware algorithm, shared by every RTPS/RTPT caller.
static uint8_t s_gte_div_table[0x101];
static bool    s_gte_div_table_init = false;
static void gte_init_div_table() {
    for (uint32_t divisor = 0x8000; divisor < 0x10000; divisor += 0x80) {
        uint32_t xa = 512;
        for (unsigned i = 1; i < 5; i++)
            xa = (xa * (1024u * 512u - ((divisor >> 7) * xa))) >> 18;
        s_gte_div_table[(divisor >> 7) & 0xFF] =
            (uint8_t)(((xa + 1) >> 1) - 0x101);
    }
    s_gte_div_table[0x100] = s_gte_div_table[0xFF];
    s_gte_div_table_init = true;
}
static int32_t gte_calc_recip(uint16_t divisor) {
    int32_t x    = 0x101 + s_gte_div_table[(((divisor & 0x7FFF) + 0x40) >> 7)];
    int32_t tmp  = (((int32_t)divisor * -x) + 0x80) >> 8;
    int32_t tmp2 = ((x * (131072 + tmp)) + 0x80) >> 8;
    return tmp2;
}
// count leading zeros of a 16-bit value (0 -> 16)
static inline unsigned gte_clz16(uint16_t v) {
    unsigned n = 0;
    for (int b = 15; b >= 0; --b) { if (v & (1u << b)) break; ++n; }
    return n;
}
static int32_t gte_divide(uint16_t H, uint16_t SZ3, uint32_t& FLAG) {
    if (!s_gte_div_table_init) gte_init_div_table();
    // Hardware: overflow flag + saturate when 2*SZ3 <= H (includes SZ3 == 0).
    if ((uint32_t)SZ3 * 2 <= (uint32_t)H) {
        FLAG |= FLAG_DIV_OVF;
        return 0x1FFFF;
    }
    unsigned shift_bias = gte_clz16(SZ3);
    uint32_t dividend = (uint32_t)H   << shift_bias;
    uint32_t divisor  = (uint32_t)SZ3 << shift_bias;
    uint32_t result = (uint32_t)(((uint64_t)dividend *
                                  (uint32_t)gte_calc_recip((uint16_t)(divisor | 0x8000))
                                  + 32768) >> 16);
    if (result > 0x1FFFF) result = 0x1FFFF;   // 17-bit saturate (no flag; matches hw path)
    return (int32_t)result;
}

// lm (bit 10) selects the lower clamp bound of every IR write in the
// lighting/depth-cue ops: 0 → -0x8000, 1 → 0. sf (bit 19) selects the
// accumulator shift. Hardware takes BOTH from the instruction word; these
// helpers used to hardcode lm=1, which zeroed every negative component —
// Crash Bash lerps signed VERTICES through INTPL (lm=0), so its characters
// collapsed onto their anchor point on every tween frame (menu strobe bug).
static inline bool gte_instr_lm(uint32_t instr) { return (instr & (1u << 10)) != 0; }
static inline int  gte_instr_sf(uint32_t instr) { return (instr & (1u << 19)) ? 12 : 0; }

// Transform a vertex by Light matrix → IR1/IR2/IR3 (step 1 of lighting)
static void light_transform(GTEState* gte, int16_t* V, uint32_t instr) {
    const bool lm = gte_instr_lm(instr);
    int64_t mac1 = ((int64_t)gte->L[0][0] * V[0] +
                    (int64_t)gte->L[0][1] * V[1] +
                    (int64_t)gte->L[0][2] * V[2]) >> 12;
    int64_t mac2 = ((int64_t)gte->L[1][0] * V[0] +
                    (int64_t)gte->L[1][1] * V[1] +
                    (int64_t)gte->L[1][2] * V[2]) >> 12;
    int64_t mac3 = ((int64_t)gte->L[2][0] * V[0] +
                    (int64_t)gte->L[2][1] * V[1] +
                    (int64_t)gte->L[2][2] * V[2]) >> 12;
    gte->MAC1 = static_cast<int32_t>(mac1);
    gte->MAC2 = static_cast<int32_t>(mac2);
    gte->MAC3 = static_cast<int32_t>(mac3);
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, lm);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, lm);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, lm);
}

// Apply light color matrix + background color to IR → IR (step 2 of lighting)
static void light_color(GTEState* gte, uint32_t instr) {
    const bool lm = gte_instr_lm(instr);
    int64_t mac1 = (int64_t)gte->BK[0] * 4096 +
                   (int64_t)gte->LC[0][0] * gte->IR1 +
                   (int64_t)gte->LC[0][1] * gte->IR2 +
                   (int64_t)gte->LC[0][2] * gte->IR3;
    int64_t mac2 = (int64_t)gte->BK[1] * 4096 +
                   (int64_t)gte->LC[1][0] * gte->IR1 +
                   (int64_t)gte->LC[1][1] * gte->IR2 +
                   (int64_t)gte->LC[1][2] * gte->IR3;
    int64_t mac3 = (int64_t)gte->BK[2] * 4096 +
                   (int64_t)gte->LC[2][0] * gte->IR1 +
                   (int64_t)gte->LC[2][1] * gte->IR2 +
                   (int64_t)gte->LC[2][2] * gte->IR3;
    gte->MAC1 = static_cast<int32_t>(mac1 >> 12);
    gte->MAC2 = static_cast<int32_t>(mac2 >> 12);
    gte->MAC3 = static_cast<int32_t>(mac3 >> 12);
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, lm);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, lm);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, lm);
}

// Multiply IR by RGBC color and push to RGB FIFO (step 3 of color output)
static void color_output(GTEState* gte, uint32_t instr) {
    const bool lm = gte_instr_lm(instr);
    uint8_t r0 = (gte->RGBC >> 0)  & 0xFF;
    uint8_t g0 = (gte->RGBC >> 8)  & 0xFF;
    uint8_t b0 = (gte->RGBC >> 16) & 0xFF;
    int64_t mac1 = (int64_t)r0 * gte->IR1 * 16;
    int64_t mac2 = (int64_t)g0 * gte->IR2 * 16;
    int64_t mac3 = (int64_t)b0 * gte->IR3 * 16;
    gte->MAC1 = static_cast<int32_t>(mac1 >> 12);
    gte->MAC2 = static_cast<int32_t>(mac2 >> 12);
    gte->MAC3 = static_cast<int32_t>(mac3 >> 12);
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, lm);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, lm);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, lm);
    gte->push_rgb(
        gte->saturate_color(gte->MAC1 >> 4, 0),
        gte->saturate_color(gte->MAC2 >> 4, 1),
        gte->saturate_color(gte->MAC3 >> 4, 2));
}

// Depth cue / interpolate current IR toward the far color using IR0
// (common tail of DPCS/DPCT/DPCL/INTPL/NCDS/NCDT/CDP). Hardware:
//   base    = IR << 12
//   step    = lim(((FC << 12) - base) >> (sf*12))   ; ±0x8000 clamp, lm FORCED off
//   MAC     = (base + IR0 * step) >> (sf*12)
//   IR      = lim(MAC, lm)                          ; lm from the instruction
static void depth_cue_from_ir(GTEState* gte, uint32_t instr) {
    const int  shift = gte_instr_sf(instr);
    const bool lm    = gte_instr_lm(instr);
    int64_t base1 = (int64_t)gte->IR1 * 4096;
    int64_t base2 = (int64_t)gte->IR2 * 4096;
    int64_t base3 = (int64_t)gte->IR3 * 4096;
    int16_t step1 = gte->saturate_ir(
        (int32_t)((((int64_t)gte->FC[0] * 4096 - base1) >> shift)), 1, false);
    int16_t step2 = gte->saturate_ir(
        (int32_t)((((int64_t)gte->FC[1] * 4096 - base2) >> shift)), 2, false);
    int16_t step3 = gte->saturate_ir(
        (int32_t)((((int64_t)gte->FC[2] * 4096 - base3) >> shift)), 3, false);
    int64_t mac1 = (base1 + (int64_t)gte->IR0 * step1) >> shift;
    int64_t mac2 = (base2 + (int64_t)gte->IR0 * step2) >> shift;
    int64_t mac3 = (base3 + (int64_t)gte->IR0 * step3) >> shift;
    gte->check_mac_overflow(mac1, 1);
    gte->check_mac_overflow(mac2, 2);
    gte->check_mac_overflow(mac3, 3);
    gte->MAC1 = static_cast<int32_t>(mac1);
    gte->MAC2 = static_cast<int32_t>(mac2);
    gte->MAC3 = static_cast<int32_t>(mac3);
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, lm);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, lm);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, lm);
}

// ---------------------------------------------------------------------------
// Widescreen X-squash (verified-enhancement, default off = identity).
//
// For a display aspect wider than the native 4:3, screen-space X is scaled by
// (4*den)/(3*num) around the projection centre OFX — e.g. 3/4 for 16:9 — and
// the present path stretches the 4:3 frame to the wide aspect, netting a wider
// horizontal field of view (the DuckStation/Beetle "widescreen hack", but
// applied in our GTE library so every RTPS/RTPT caller — generated code,
// interpreter, overlay DLLs — sees it). Only the IR1*h/sz term is scaled, NOT
// OFX, so the squash is centred on the game's own projection centre and
// games' post-projection screen-bounds culls (which read SXY back from us)
// stay aligned with the visible frame.
// ---------------------------------------------------------------------------
static int32_t s_ws_xnum = 1, s_ws_xden = 1;
extern "C" int gpu_ws_present_native_43(void);  /* gpu.c — suppress on 4:3 frames */
extern "C" void psx_ws_note_gte_project(int nverts);  /* gpu.c — gte_game_mode stamp */
static int s_gte_replay_sandbox = 0;

/* Optional visual-only geometry precision cache. The PS1-visible SXY FIFO
 * remains integer and fully faithful; this side cache retains the discarded
 * 16.16 projection fraction so the high-resolution software mirror can match
 * a later GP0 polygon and place its vertices between native pixels. */
#define GEOM_CACHE_SIZE 8192u
struct GeomVertex {
    uint32_t packed;
    int32_t x16, y16;
    uint32_t generation;
};
static GeomVertex s_geom_cache[GEOM_CACHE_SIZE];
static uint32_t s_geom_generation = 1;
static int s_geom_enabled = 0;
static uint32_t s_geom_hits = 0;

/* Exact GTE projection provenance for perspective texture correction. The
 * recompiler/interpreters call gte_precision_store after SWC2 writes an SXY
 * register into guest RAM. GPU DMA later supplies that exact RAM address, so
 * depth lookup follows the packet rather than guessing from rounded X/Y. */
struct PreciseProjection {
    uint32_t packed;
    int32_t x16, y16;
    uint16_t z;
    uint8_t valid;
};
static PreciseProjection s_precise_sxy[4] = {};

#define PRECISION_STORE_SIZE 65536u
struct PrecisionStoreEntry {
    uint32_t addr;
    PreciseProjection projection;
    uint32_t generation;
};
static PrecisionStoreEntry s_precision_store[PRECISION_STORE_SIZE];
static uint32_t s_precision_generation = 1;
static int s_precision_tracking = 0;
static PreciseProjection s_speculative_saved_sxy[4];
static uint32_t s_speculative_depth = 0;
static int s_speculative_timeline_invalidated = 0;

static void gte_precision_generation_advance(void) {
    if (++s_precision_generation == 0) {
        std::memset(s_precision_store, 0, sizeof(s_precision_store));
        s_precision_generation = 1;
    }
}

static void gte_geom_generation_advance(void) {
    if (++s_geom_generation == 0) {
        std::memset(s_geom_cache, 0, sizeof(s_geom_cache));
        s_geom_generation = 1;
    }
}

extern "C" void gte_precision_timeline_invalidate(void) {
    for (int i = 0; i < 4; ++i) s_precise_sxy[i].valid = 0;
    /* Raw machine-state restore is authoritative even if host polling reached
     * it during a speculative validation pass. Defer the generation advance
     * until the outer transaction ends so old provenance cannot be restored. */
    if (s_speculative_depth != 0) {
        s_speculative_timeline_invalidated = 1;
        return;
    }
    gte_precision_generation_advance();
    gte_geom_generation_advance();
}

extern "C" void gte_precision_speculative_begin(void) {
    if (s_speculative_depth++ == 0) {
        std::memcpy(s_speculative_saved_sxy, s_precise_sxy,
                    sizeof(s_precise_sxy));
        s_speculative_timeline_invalidated = 0;
    }
}

extern "C" void gte_precision_speculative_end(void) {
    if (s_speculative_depth == 0) return;
    if (--s_speculative_depth == 0) {
        if (s_speculative_timeline_invalidated) {
            gte_precision_generation_advance();
            gte_geom_generation_advance();
            s_speculative_timeline_invalidated = 0;
        } else {
            std::memcpy(s_precise_sxy, s_speculative_saved_sxy,
                        sizeof(s_precise_sxy));
        }
    }
}

static inline uint32_t precision_hash(uint32_t addr) {
    addr >>= 2;
    addr ^= addr >> 11;
    return (addr * 2654435761u) & (PRECISION_STORE_SIZE - 1u);
}

static inline int precision_ram_address(uint32_t addr, uint32_t *physical) {
    uint32_t mapped = addr & 0x1FFFFFFFu;
    if (mapped >= 0x00800000u) return 0;
    *physical = mapped & 0x1FFFFCu;
    return 1;
}

extern "C" void gte_precision_tracking_set(int enabled) {
    s_precision_tracking = enabled ? 1 : 0;
    gte_precision_generation_advance();
}

extern "C" void gte_precision_store_word(uint32_t addr, uint8_t reg) {
    if (s_speculative_depth != 0 || s_gte_replay_sandbox || !s_precision_tracking ||
        reg < 12 || reg > 15) return;
    int index = reg == 15 ? 2 : (int)reg - 12;
    const PreciseProjection &projection = s_precise_sxy[index];
    if (!projection.valid) return;
    uint32_t physical;
    if (!precision_ram_address(addr, &physical)) return;
    PrecisionStoreEntry &entry = s_precision_store[precision_hash(physical)];
    entry.addr = physical;
    entry.projection = projection;
    entry.generation = s_precision_generation;
}

extern "C" void gte_precision_invalidate_word(uint32_t addr) {
    if (s_speculative_depth != 0 || s_gte_replay_sandbox || !s_precision_tracking) return;
    uint32_t physical;
    if (!precision_ram_address(addr, &physical)) return;
    PrecisionStoreEntry &entry = s_precision_store[precision_hash(physical)];
    if (entry.generation == s_precision_generation && entry.addr == physical)
        entry.generation = 0;
}

extern "C" int gte_precision_load_word(uint32_t addr, uint32_t packed,
                                        int32_t *x16, int32_t *y16,
                                        uint16_t *z) {
    if (s_speculative_depth != 0 || !s_precision_tracking) return 0;
    uint32_t physical;
    if (!precision_ram_address(addr, &physical)) return 0;
    const PrecisionStoreEntry &entry = s_precision_store[precision_hash(physical)];
    if (entry.generation != s_precision_generation || entry.addr != physical ||
        !entry.projection.valid || entry.projection.packed != packed)
        return 0;
    if (x16) *x16 = entry.projection.x16;
    if (y16) *y16 = entry.projection.y16;
    if (z) *z = entry.projection.z;
    return entry.projection.z != 0;
}

static inline uint32_t geom_hash(uint32_t packed) {
    packed ^= packed >> 16;
    return (packed * 2654435761u) & (GEOM_CACHE_SIZE - 1u);
}

extern "C" void gte_geometry_correction_set(int enabled) {
    s_geom_enabled = enabled ? 1 : 0;
    s_geom_hits = 0;
    gte_geom_generation_advance();
}

extern "C" int gte_geometry_correction_enabled(void) {
    return s_geom_enabled;
}

extern "C" uint32_t gte_geometry_correction_hits(void) {
    return s_geom_hits;
}

extern "C" int gte_geometry_correction_lookup(uint32_t packed,
                                                int32_t *x16, int32_t *y16) {
    if (s_speculative_depth != 0 || !s_geom_enabled) return 0;
    const GeomVertex &entry = s_geom_cache[geom_hash(packed)];
    if (entry.generation != s_geom_generation || entry.packed != packed) return 0;
    if (x16) *x16 = entry.x16;
    if (y16) *y16 = entry.y16;
    s_geom_hits++;
    return 1;
}

static inline void geom_note(uint32_t packed, int64_t x16, int64_t y16) {
    if (s_speculative_depth != 0 || s_gte_replay_sandbox || !s_geom_enabled) return;
    /* Saturated off-screen projections are unsuitable for subpixel recovery. */
    int32_t x = (int16_t)(packed & 0xFFFFu);
    int32_t y = (int16_t)(packed >> 16);
    if (x <= -0x400 || x >= 0x3FF || y <= -0x400 || y >= 0x3FF) return;
    GeomVertex &entry = s_geom_cache[geom_hash(packed)];
    entry.packed = packed;
    entry.x16 = (int32_t)x16;
    entry.y16 = (int32_t)y16;
    entry.generation = s_geom_generation;
}

// Per-draw suppression of the X-squash (8C far-backdrop). The far backdrop
// (ocean/cloud/distant mountain) is a parallax layer that is conceptually at
// infinity: squashing it toward centre leaves its geometry short of the
// revealed 16:9 edges (blue void). Drawing it UN-squashed lets its 4:3-extent
// fill the stretched frame (skybox treatment) while the near 3D world keeps the
// wider FOV. The recompiler brackets the backdrop driver (FUN_8004db3c) with
// gte_ws_set_suppress(1)/(0) ([widescreen] backdrop_unsquash_funcs).
// The driver (FUN_8004db3c) actually draws a MIX: the far backdrop (which we
// want un-squashed) AND nearer props (doors/mansion models) that must stay
// squashed or they drift relative to the rest of the world. So suppression is
// DEPTH-GATED: only vertices with SZ (projected Z) >= s_ws_far_threshold get
// un-squashed while suppress is active; nearer props keep the squash. Threshold
// tuned live via the ws_far_threshold TCP cmd; SZ stats below characterize the
// driver's depth range so the split is set from data, not a guess.
static int s_ws_suppress = 0;
static int32_t s_ws_far_threshold = 900;  /* SZ split: near props squashed, far backdrop un-squashed (8C, tunable via ws_far_threshold) */
extern "C" void gte_ws_set_suppress(int on) { s_ws_suppress = on ? 1 : 0; }
extern "C" void gte_ws_set_far_threshold(int t) { s_ws_far_threshold = t; }
extern "C" int  gte_ws_get_far_threshold(void) { return (int)s_ws_far_threshold; }
/* SZ stats observed while suppress is active (one frame window, reset on read). */
static int32_t s_ws_sz_min = 0x7FFFFFFF, s_ws_sz_max = 0;
static uint32_t s_ws_sz_n = 0, s_ws_sz_far = 0;
extern "C" void gte_ws_get_sz_stats(int* mn, int* mx, unsigned* n, unsigned* far_n) {
    if (mn) *mn = (s_ws_sz_n ? s_ws_sz_min : 0);
    if (mx) *mx = s_ws_sz_max;
    if (n)  *n  = s_ws_sz_n;
    if (far_n) *far_n = s_ws_sz_far;
    s_ws_sz_min = 0x7FFFFFFF; s_ws_sz_max = 0; s_ws_sz_n = 0; s_ws_sz_far = 0;
}

// ---- Native-wide sky-DOME expand ------------------------------------------
// In native-wide (mode 2) the GTE is fed identity (no squash) so the world
// fills the wider FOV via the cull widening. But a sky DOME is a FINITE mesh
// authored to fill 4:3 — its curved edge falls short of the wider frame corners
// (black corners). Fix: scale the FAR-depth vertices' projected X OUTWARD from
// the projection centre (OFX) by the frame-widening ratio (3*num)/(4*den) — the
// inverse of the squash — so the dome grows to cover the wider FOV. DEPTH-GATED
// (SZ >= s_ws_far_threshold) so only the farthest layer (the sky) expands while
// nearer world geometry stays put. On + ratio set from main.cpp when native-
// wide engages; threshold tuned live via ws_far_threshold. Off => identity.
static int      s_ws_dome_on  = 0;
static int32_t  s_ws_dome_num = 1, s_ws_dome_den = 1;  // expand = num/den (>1)
extern "C" void gte_ws_set_dome_expand(int on, int aspect_num, int aspect_den) {
    s_ws_dome_on = on ? 1 : 0;
    // widen ratio = wide_w / disp_w = (aspect/(4:3)) = (3*num)/(4*den).
    int32_t n = 3 * aspect_num, d = 4 * aspect_den;
    if (n <= 0 || d <= 0 || n <= d) { s_ws_dome_num = s_ws_dome_den = 1; return; }
    s_ws_dome_num = n; s_ws_dome_den = d;
}

// ---- Dome-locate probe -----------------------------------------------------
// Tally which guest function (g_debug_current_func_addr, set at dispatch)
// projects FAR (SZ >= threshold) vertices, so the sky-dome draw function can be
// identified as the top far-vertex emitter — the target for the per-function
// dome-expand bracket (the clean alternative to the scene-dependent depth gate).
extern "C" { extern uint32_t g_debug_current_func_addr; }
static uint32_t s_gte_caller_ra = 0;   /* guest ra at gte_execute = return into the GAME fn that issued the projection (not the libgte leaf) */
#define WS_DOME_CALL_SITES_MAX 32
static uint32_t s_ws_dome_call_sites[WS_DOME_CALL_SITES_MAX];
static int      s_ws_dome_call_sites_n = 0;
extern "C" void gte_ws_configure_dome_sites(const uint32_t* sites, int count) {
    if (count < 0) count = 0;
    if (count > WS_DOME_CALL_SITES_MAX) count = WS_DOME_CALL_SITES_MAX;
    s_ws_dome_call_sites_n = count;
    for (int i = 0; i < count; i++)
        s_ws_dome_call_sites[i] = sites[i] & 0x1FFFFFFFu;
}
static inline bool ws_dome_call_matches(void) {
    // JAL writes pc+8 to RA; config records the call instruction itself.
    const uint32_t call_pc = (s_gte_caller_ra - 8u) & 0x1FFFFFFFu;
    for (int i = 0; i < s_ws_dome_call_sites_n; i++)
        if (s_ws_dome_call_sites[i] == call_pc) return true;
    return false;
}
#define DOME_PROBE_SLOTS 48
static struct { uint32_t func; uint32_t count; int32_t max_sz; } s_dome_probe[DOME_PROBE_SLOTS];
static int     s_dome_probe_on  = 0;
static int32_t s_dome_probe_thr = 4000;
extern "C" void gte_dome_probe(int on, int thr) {
    s_dome_probe_on = on ? 1 : 0;
    if (thr > 0) s_dome_probe_thr = thr;
    if (on) for (int i = 0; i < DOME_PROBE_SLOTS; i++) { s_dome_probe[i].func = 0; s_dome_probe[i].count = 0; s_dome_probe[i].max_sz = 0; }
}
static inline void dome_probe_note(int32_t sz) {
    if (s_gte_replay_sandbox) return;
    if (!s_dome_probe_on || sz < s_dome_probe_thr) return;
    /* Tally the guest RA (the GAME fn that jal'd to libgte for this projection),
     * NOT g_debug_current_func_addr (which is the libgte leaf 0x80000F40). */
    uint32_t f = s_gte_caller_ra;
    int slot = -1, empty = -1;
    for (int i = 0; i < DOME_PROBE_SLOTS; i++) {
        if (s_dome_probe[i].count && s_dome_probe[i].func == f) { slot = i; break; }
        if (empty < 0 && s_dome_probe[i].count == 0) empty = i;
    }
    if (slot < 0) slot = empty;
    if (slot < 0) return;
    s_dome_probe[slot].func = f;
    s_dome_probe[slot].count++;
    if (sz > s_dome_probe[slot].max_sz) s_dome_probe[slot].max_sz = sz;
}
extern "C" int gte_dome_probe_dump(uint32_t* funcs, uint32_t* counts, int32_t* maxsz, int cap) {
    int n = 0;
    for (int i = 0; i < DOME_PROBE_SLOTS && n < cap; i++)
        if (s_dome_probe[i].count) { funcs[n] = s_dome_probe[i].func; counts[n] = s_dome_probe[i].count; maxsz[n] = s_dome_probe[i].max_sz; n++; }
    return n;
}

// ---------------------------------------------------------------------------
// GTE projection ring (ALWAYS-ON) — records each RTPS/RTPT's inputs (vertex,
// rotation matrix, translation, projection config) and outputs (screen XY / Z /
// FLAG). A post-hoc probe queries this to find flattened/degenerate character
// projections and decide whether the fault is in the INPUTS supplied by the
// recompiled game code (bad matrix/vertex) or the GTE math itself.
// ---------------------------------------------------------------------------
// NOTE: this block is already inside namespace PSXRecomp::GTE (opened line 8),
// so types are referred to unqualified.
extern "C" { extern uint64_t s_frame_count; }
struct GteRtpRec {
    uint32_t seq, frame, caller_ra, cmd;
    int16_t  V0[3], V1[3], V2[3];
    int16_t  RT[9];
    int32_t  TR[3];
    uint16_t H;  int32_t OFX, OFY;
    int32_t  SXY0, SXY1, SXY2;
    uint16_t SZ1, SZ2, SZ3;
    uint32_t FLAG;
};
#define GTE_RTP_RING_CAP (1u << 18)   /* 256K raw entries (~a few seconds) */
static GteRtpRec* s_gte_rtp_ring = nullptr;
static uint64_t   s_gte_rtp_seq  = 0;

/* Per-frame aggregate stats (retained ring over recent distinct frames). Lets a
 * probe see the alternating flat/normal render pattern without shipping 84k
 * projections/sec over JSON. nsat = projections whose newest output vertex
 * saturated off-screen; nflat = RTP whose last-3 output verts are collinear
 * (area 0) = a flattened triangle. */
#define GTE_FSTAT_CAP 512
struct GteFStat { uint32_t frame, nproj, nsat, nflat, nintpl, nintpl_tiny; };
static GteFStat s_gte_fstat[GTE_FSTAT_CAP];
static uint32_t s_gte_fstat_head = 0;   /* index of most-recent frame slot */
static int      s_gte_fstat_valid = 0;

/* Degenerate-latch: full records of flat/saturated projections, retained (small
 * ring) so intermittent flat frames are caught regardless of raw-ring roll. */
#define GTE_LATCH_CAP (1u << 13)   /* 8192 */
static GteRtpRec* s_gte_latch = nullptr;
static uint64_t   s_gte_latch_seq = 0;

static inline int gte_sxx(int32_t p){ int v=p&0xFFFF; return v>=0x8000? v-0x10000:v; }
static inline int gte_syy(int32_t p){ int v=(p>>16)&0xFFFF; return v>=0x8000? v-0x10000:v; }

static void gte_rtp_record(const GTEState* g, uint32_t cmd) {
    if (s_gte_replay_sandbox) return;
    if (!s_gte_rtp_ring) {
        s_gte_rtp_ring = (GteRtpRec*)calloc(GTE_RTP_RING_CAP, sizeof(GteRtpRec));
        if (!s_gte_rtp_ring) return;
    }
    GteRtpRec* e = &s_gte_rtp_ring[s_gte_rtp_seq % GTE_RTP_RING_CAP];
    e->seq = (uint32_t)s_gte_rtp_seq;
    e->frame = (uint32_t)s_frame_count;
    e->caller_ra = s_gte_caller_ra;
    e->cmd = cmd;
    for (int i = 0; i < 3; i++) { e->V0[i]=g->V0[i]; e->V1[i]=g->V1[i]; e->V2[i]=g->V2[i]; }
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) e->RT[r*3+c]=g->RT[r][c];
    for (int i = 0; i < 3; i++) e->TR[i]=g->TR[i];
    e->H=g->H; e->OFX=g->OFX; e->OFY=g->OFY;
    e->SXY0=g->SXY[0]; e->SXY1=g->SXY[1]; e->SXY2=g->SXY[2];
    e->SZ1=g->SZ[1]; e->SZ2=g->SZ[2]; e->SZ3=g->SZ[3];
    e->FLAG=g->FLAG;
    s_gte_rtp_seq++;

    /* classify */
    int x0=gte_sxx(e->SXY0),y0=gte_syy(e->SXY0);
    int x1=gte_sxx(e->SXY1),y1=gte_syy(e->SXY1);
    int x2=gte_sxx(e->SXY2),y2=gte_syy(e->SXY2);
    int sat = (x2<=-1023||x2>=1023||y2<=-1023||y2>=1023) ? 1 : 0;
    long area2 = (long)(x1-x0)*(y2-y0) - (long)(x2-x0)*(y1-y0);
    int flat = (area2==0) ? 1 : 0;   /* three output verts exactly collinear */

    /* per-frame stats */
    uint32_t f=(uint32_t)s_frame_count;
    if (!s_gte_fstat_valid || s_gte_fstat[s_gte_fstat_head].frame != f) {
        s_gte_fstat_head = s_gte_fstat_valid ? (s_gte_fstat_head+1)%GTE_FSTAT_CAP : 0;
        s_gte_fstat_valid = 1;
        s_gte_fstat[s_gte_fstat_head].frame=f;
        s_gte_fstat[s_gte_fstat_head].nproj=0;
        s_gte_fstat[s_gte_fstat_head].nsat=0;
        s_gte_fstat[s_gte_fstat_head].nflat=0;
        s_gte_fstat[s_gte_fstat_head].nintpl=0;
        s_gte_fstat[s_gte_fstat_head].nintpl_tiny=0;
    }
    s_gte_fstat[s_gte_fstat_head].nproj++;
    s_gte_fstat[s_gte_fstat_head].nsat += sat;
    s_gte_fstat[s_gte_fstat_head].nflat += flat;

    /* latch degenerate (saturated) projections — the "flat/absent" character */
    if (sat) {
        if (!s_gte_latch) {
            s_gte_latch = (GteRtpRec*)calloc(GTE_LATCH_CAP, sizeof(GteRtpRec));
            if (!s_gte_latch) return;
        }
        s_gte_latch[s_gte_latch_seq % GTE_LATCH_CAP] = *e;
        s_gte_latch_seq++;
    }
}

extern "C" uint64_t gte_rtp_ring_total(void) { return s_gte_rtp_seq; }
extern "C" uint64_t gte_latch_total(void) { return s_gte_latch_seq; }

// ---------------------------------------------------------------------------
// GTE INTPL ring (ALWAYS-ON) — records each INTPL (0x11) vertex-lerp: inputs
// (IR0 blend, IR1-3 pose-A vertex, FC pose-B vertex) snapshotted BEFORE the op
// and outputs (MAC1-3 / IR1-3 / FLAG) after. Crash Bash tweens character
// vertex animation through INTPL; this ring decides whether collapsed tween
// output comes from already-tiny inputs (upstream CPU/game-state divergence)
// or from the INTPL math itself.
// ---------------------------------------------------------------------------
struct GteIntplRec {
    uint32_t seq, frame, caller_ra;
    int16_t  ir0, in1, in2, in3;
    int32_t  fc[3];
    int32_t  mac[3];
    int16_t  out1, out2, out3;
    uint32_t flag;
};
#define GTE_INTPL_RING_CAP (1u << 17)   /* 128K entries (~4s at menu rates) */
static GteIntplRec* s_gte_intpl_ring = nullptr;
static uint64_t     s_gte_intpl_seq  = 0;

static void gte_intpl_record(const GTEState* g,
                             const int16_t pre_ir[4], const int32_t pre_fc[3]) {
    if (s_gte_replay_sandbox) return;
    if (!s_gte_intpl_ring) {
        s_gte_intpl_ring = (GteIntplRec*)calloc(GTE_INTPL_RING_CAP, sizeof(GteIntplRec));
        if (!s_gte_intpl_ring) return;
    }
    GteIntplRec* e = &s_gte_intpl_ring[s_gte_intpl_seq % GTE_INTPL_RING_CAP];
    e->seq = (uint32_t)s_gte_intpl_seq;
    e->frame = (uint32_t)s_frame_count;
    e->caller_ra = s_gte_caller_ra;
    e->ir0 = pre_ir[0]; e->in1 = pre_ir[1]; e->in2 = pre_ir[2]; e->in3 = pre_ir[3];
    for (int i = 0; i < 3; i++) e->fc[i] = pre_fc[i];
    e->mac[0]=g->MAC1; e->mac[1]=g->MAC2; e->mac[2]=g->MAC3;
    e->out1=g->IR1; e->out2=g->IR2; e->out3=g->IR3;
    e->flag=g->FLAG;
    s_gte_intpl_seq++;

    /* per-frame stats (shares the fstat ring; nintpl_tiny = all outputs small
     * while at least one input was not — the collapse signature) */
    uint32_t f=(uint32_t)s_frame_count;
    if (!s_gte_fstat_valid || s_gte_fstat[s_gte_fstat_head].frame != f) {
        s_gte_fstat_head = s_gte_fstat_valid ? (s_gte_fstat_head+1)%GTE_FSTAT_CAP : 0;
        s_gte_fstat_valid = 1;
        s_gte_fstat[s_gte_fstat_head].frame=f;
        s_gte_fstat[s_gte_fstat_head].nproj=0;
        s_gte_fstat[s_gte_fstat_head].nsat=0;
        s_gte_fstat[s_gte_fstat_head].nflat=0;
        s_gte_fstat[s_gte_fstat_head].nintpl=0;
        s_gte_fstat[s_gte_fstat_head].nintpl_tiny=0;
    }
    s_gte_fstat[s_gte_fstat_head].nintpl++;
    {
        int out_tiny = (e->out1>-48 && e->out1<48 && e->out2>-48 && e->out2<48
                        && e->out3>-48 && e->out3<48);
        int in_big = (e->in1<=-48 || e->in1>=48 || e->in2<=-48 || e->in2>=48
                      || e->in3<=-48 || e->in3>=48
                      || e->fc[0]<=-48 || e->fc[0]>=48
                      || e->fc[1]<=-48 || e->fc[1]>=48
                      || e->fc[2]<=-48 || e->fc[2]>=48);
        if (out_tiny && in_big) s_gte_fstat[s_gte_fstat_head].nintpl_tiny++;
    }
}

extern "C" uint64_t gte_intpl_ring_total(void) { return s_gte_intpl_seq; }

/* Dump INTPL ring entries as JSON array body. offset skips the first N
 * MATCHING entries (after frame filtering), so a probe can page through a
 * whole frame regardless of where it sits in the ring. */
extern "C" int gte_intpl_ring_dump_json(char* out, int outsz, int max_count,
                                        int newest_first, long frame_filter,
                                        int offset) {
    if (!s_gte_intpl_ring || outsz < 64) { if (out && outsz) out[0]=0; return 0; }
    uint64_t total = s_gte_intpl_seq;
    if (total == 0) { out[0]=0; return 0; }
    uint64_t oldest = total > GTE_INTPL_RING_CAP ? total - GTE_INTPL_RING_CAP : 0;
    int pos = 0, emitted = 0, matched = 0;
    for (uint64_t k = 0; k < (total - oldest) && emitted < max_count; k++) {
        uint64_t seq = newest_first ? (total - 1 - k) : (oldest + k);
        if (seq < oldest || seq >= total) break;
        const GteIntplRec* e = &s_gte_intpl_ring[seq % GTE_INTPL_RING_CAP];
        if (frame_filter >= 0 && e->frame != (uint32_t)frame_filter) continue;
        if (matched++ < offset) continue;
        if (pos > outsz - 400) break;
        pos += snprintf(out+pos, outsz-pos,
            "%s{\"seq\":%u,\"frame\":%u,\"ra\":\"0x%08X\","
            "\"ir0\":%d,\"in\":[%d,%d,%d],\"fc\":[%d,%d,%d],"
            "\"mac\":[%d,%d,%d],\"out\":[%d,%d,%d],\"flag\":\"0x%08X\"}",
            emitted?",":"", e->seq, e->frame, e->caller_ra,
            e->ir0, e->in1, e->in2, e->in3, e->fc[0], e->fc[1], e->fc[2],
            e->mac[0], e->mac[1], e->mac[2], e->out1, e->out2, e->out3,
            e->flag);
        emitted++;
    }
    out[pos] = 0;
    return emitted;
}

/* Emit per-frame stats (newest first) as JSON array body. */
extern "C" int gte_fstat_dump_json(char* out, int outsz, int max_frames) {
    if (!s_gte_fstat_valid) { if(out&&outsz) out[0]=0; return 0; }
    int pos=0, emitted=0;
    for (int k=0; k<GTE_FSTAT_CAP && emitted<max_frames; k++) {
        int idx=(int)((s_gte_fstat_head + GTE_FSTAT_CAP - k)%GTE_FSTAT_CAP);
        GteFStat* s=&s_gte_fstat[idx];
        if (s->nproj==0 && !(k==0)) continue;
        if (pos>outsz-96) break;
        pos+=snprintf(out+pos,outsz-pos,
                      "%s{\"frame\":%u,\"nproj\":%u,\"nsat\":%u,\"nflat\":%u,"
                      "\"nintpl\":%u,\"nintpl_tiny\":%u}",
                      emitted?",":"", s->frame,s->nproj,s->nsat,s->nflat,
                      s->nintpl,s->nintpl_tiny);
        emitted++;
    }
    out[pos]=0; return emitted;
}

/* Dump latched degenerate projections (newest first). */
extern "C" int gte_latch_dump_json(char* out, int outsz, int max_count) {
    if (!s_gte_latch || s_gte_latch_seq==0){ if(out&&outsz) out[0]=0; return 0; }
    uint64_t total=s_gte_latch_seq;
    uint64_t oldest = total>GTE_LATCH_CAP ? total-GTE_LATCH_CAP : 0;
    int pos=0, emitted=0;
    for (uint64_t k=0; k<(total-oldest) && emitted<max_count; k++) {
        uint64_t seq=total-1-k;
        if (seq<oldest) break;
        const GteRtpRec* e=&s_gte_latch[seq % GTE_LATCH_CAP];
        if (pos>outsz-700) break;
        pos+=snprintf(out+pos,outsz-pos,
            "%s{\"frame\":%u,\"ra\":\"0x%08X\",\"cmd\":\"0x%08X\","
            "\"RT\":[%d,%d,%d,%d,%d,%d,%d,%d,%d],\"TR\":[%d,%d,%d],\"H\":%u,"
            "\"V0\":[%d,%d,%d],\"V1\":[%d,%d,%d],\"V2\":[%d,%d,%d],"
            "\"S0\":[%d,%d],\"S1\":[%d,%d],\"S2\":[%d,%d],\"SZ\":[%u,%u,%u],\"FLAG\":\"0x%08X\"}",
            emitted?",":"", e->frame, e->caller_ra, e->cmd,
            e->RT[0],e->RT[1],e->RT[2],e->RT[3],e->RT[4],e->RT[5],e->RT[6],e->RT[7],e->RT[8],
            e->TR[0],e->TR[1],e->TR[2],(unsigned)e->H,
            e->V0[0],e->V0[1],e->V0[2], e->V1[0],e->V1[1],e->V1[2], e->V2[0],e->V2[1],e->V2[2],
            gte_sxx(e->SXY0),gte_syy(e->SXY0),gte_sxx(e->SXY1),gte_syy(e->SXY1),
            gte_sxx(e->SXY2),gte_syy(e->SXY2),(unsigned)e->SZ1,(unsigned)e->SZ2,(unsigned)e->SZ3,e->FLAG);
        emitted++;
    }
    out[pos]=0; return emitted;
}

/* Format up to max_count recent entries as a JSON array body (no outer braces).
 * If frame_filter >= 0, only entries with that frame are emitted. Returns count. */
extern "C" int gte_rtp_ring_dump_json(char* out, int outsz, int max_count,
                                      int newest_first, long frame_filter) {
    if (!s_gte_rtp_ring || outsz < 64) { if (out && outsz) out[0]=0; return 0; }
    uint64_t total = s_gte_rtp_seq;
    if (total == 0) { out[0]=0; return 0; }
    uint64_t oldest = total > GTE_RTP_RING_CAP ? total - GTE_RTP_RING_CAP : 0;
    int pos = 0, emitted = 0;
    for (uint64_t k = 0; k < (total - oldest) && emitted < max_count; k++) {
        uint64_t seq = newest_first ? (total - 1 - k) : (oldest + k);
        if (seq < oldest || seq >= total) break;
        const GteRtpRec* e = &s_gte_rtp_ring[seq % GTE_RTP_RING_CAP];
        if (frame_filter >= 0 && e->frame != (uint32_t)frame_filter) continue;
        auto sxx = [](int32_t p){ int v=p&0xFFFF; return v>=0x8000? v-0x10000:v; };
        auto syy = [](int32_t p){ int v=(p>>16)&0xFFFF; return v>=0x8000? v-0x10000:v; };
        if (pos > outsz - 700) break;
        pos += snprintf(out+pos, outsz-pos,
            "%s{\"seq\":%u,\"frame\":%u,\"ra\":\"0x%08X\",\"cmd\":\"0x%08X\","
            "\"V0\":[%d,%d,%d],\"V1\":[%d,%d,%d],\"V2\":[%d,%d,%d],"
            "\"RT\":[%d,%d,%d,%d,%d,%d,%d,%d,%d],\"TR\":[%d,%d,%d],"
            "\"H\":%u,\"OFX\":%d,\"OFY\":%d,"
            "\"S0\":[%d,%d],\"S1\":[%d,%d],\"S2\":[%d,%d],"
            "\"SZ\":[%u,%u,%u],\"FLAG\":\"0x%08X\"}",
            emitted?",":"", e->seq, e->frame, e->caller_ra, e->cmd,
            e->V0[0],e->V0[1],e->V0[2], e->V1[0],e->V1[1],e->V1[2], e->V2[0],e->V2[1],e->V2[2],
            e->RT[0],e->RT[1],e->RT[2],e->RT[3],e->RT[4],e->RT[5],e->RT[6],e->RT[7],e->RT[8],
            e->TR[0],e->TR[1],e->TR[2], (unsigned)e->H, e->OFX, e->OFY,
            sxx(e->SXY0),syy(e->SXY0), sxx(e->SXY1),syy(e->SXY1), sxx(e->SXY2),syy(e->SXY2),
            (unsigned)e->SZ1,(unsigned)e->SZ2,(unsigned)e->SZ3, e->FLAG);
        emitted++;
    }
    out[pos] = 0;
    return emitted;
}

extern "C" void gte_set_display_aspect(int num, int den) {
    if (num <= 0 || den <= 0) { s_ws_xnum = s_ws_xden = 1; return; }
    // squash = (4/3) / (num/den) = (4*den) / (3*num); identity for 4:3.
    int32_t n = 4 * den, d = 3 * num;
    int32_t a = n, b = d;
    while (b) { int32_t t = a % b; a = b; b = t; }   // gcd
    s_ws_xnum = n / a;
    s_ws_xden = d / a;
}

// ---------------------------------------------------------------------------
// RTPS — Perspective Transformation (internal, operates on given vertex V)
// ---------------------------------------------------------------------------
void gte_rtps_internal(GTEState* gte, int16_t* V, bool setMac0) {
    // Step 1: Matrix multiplication + translation
    int64_t mac1 = (int64_t)gte->TR[0] * 4096 +
                   (int64_t)gte->RT[0][0] * V[0] +
                   (int64_t)gte->RT[0][1] * V[1] +
                   (int64_t)gte->RT[0][2] * V[2];
    int64_t mac2 = (int64_t)gte->TR[1] * 4096 +
                   (int64_t)gte->RT[1][0] * V[0] +
                   (int64_t)gte->RT[1][1] * V[1] +
                   (int64_t)gte->RT[1][2] * V[2];
    int64_t mac3 = (int64_t)gte->TR[2] * 4096 +
                   (int64_t)gte->RT[2][0] * V[0] +
                   (int64_t)gte->RT[2][1] * V[1] +
                   (int64_t)gte->RT[2][2] * V[2];

    gte->check_mac_overflow(mac1 >> 12, 1);
    gte->check_mac_overflow(mac2 >> 12, 2);
    gte->check_mac_overflow(mac3 >> 12, 3);
    gte->MAC1 = static_cast<int32_t>(mac1 >> 12);
    gte->MAC2 = static_cast<int32_t>(mac2 >> 12);
    gte->MAC3 = static_cast<int32_t>(mac3 >> 12);

    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, false);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, false);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, false);

    // Step 2: Push SZ FIFO
    gte->push_sz(gte->MAC3);

    // Step 3: Perspective division
    int32_t h_div_sz = gte_divide(gte->H, gte->SZ[3], gte->FLAG);

    // Step 4: Project to screen coordinates. Squash X only when configured AND
    // this frame is being stretched — never on a 4:3-presented frame (FMV /
    // full-2D screen), so content and present stay locked.
    int64_t xterm = (int64_t)gte->IR1 * h_div_sz;
    bool do_squash = (s_ws_xnum != s_ws_xden) && !gpu_ws_present_native_43();
    const bool dome_call = ws_dome_call_matches();
    // Curved backdrops are authored to cover the original 4:3 projection.
    // In classic widescreen, leave that projection intact and let the normal
    // final-frame stretch cover the wide output instead of shrinking it first.
    if (dome_call) do_squash = false;
    if (do_squash && s_ws_suppress) {
        /* Depth-gated: un-squash only FAR geometry (the backdrop); keep near
         * props squashed so they stay aligned. Record SZ stats for tuning. */
        int32_t sz = gte->SZ[3];
        if (!s_gte_replay_sandbox) {
            if (sz < s_ws_sz_min) s_ws_sz_min = sz;
            if (sz > s_ws_sz_max) s_ws_sz_max = sz;
            s_ws_sz_n++;
        }
        if (sz >= s_ws_far_threshold) {
            do_squash = false;
            if (!s_gte_replay_sandbox) s_ws_sz_far++;
        }
    }
    if (do_squash)
        xterm = xterm * s_ws_xnum / s_ws_xden;
    // Native-wide dome expansion remains a diagnostic-only depth probe.
    else if (s_ws_dome_on && s_ws_dome_num != s_ws_dome_den &&
             !gpu_ws_present_native_43()) {
        int32_t sz = gte->SZ[3];
        if (!s_gte_replay_sandbox) {
            if (sz < s_ws_sz_min) s_ws_sz_min = sz;
            if (sz > s_ws_sz_max) s_ws_sz_max = sz;
            s_ws_sz_n++;
        }
        if (sz >= s_ws_far_threshold) {
            xterm = xterm * s_ws_dome_num / s_ws_dome_den;
            if (!s_gte_replay_sandbox) s_ws_sz_far++;
        }
    }
    dome_probe_note(gte->SZ[3]);   /* locate the dome draw fn (far-vertex tally) */
    int64_t sx16 = gte->OFX + xterm;
    int64_t sy16 = gte->OFY + (int64_t)gte->IR2 * h_div_sz;
    int64_t sx = sx16 >> 16;
    int64_t sy = sy16 >> 16;
    gte->push_sxy(sx, sy);
    if (!s_gte_replay_sandbox) {
        s_precise_sxy[0] = s_precise_sxy[1];
        s_precise_sxy[1] = s_precise_sxy[2];
        s_precise_sxy[2].packed = (uint32_t)gte->SXY[2];
        s_precise_sxy[2].x16 = (int32_t)sx16;
        s_precise_sxy[2].y16 = (int32_t)sy16;
        s_precise_sxy[2].z = gte->SZ[3];
        s_precise_sxy[2].valid = gte->SZ[3] != 0;
        s_precise_sxy[3] = s_precise_sxy[2];
    }
    geom_note((uint32_t)gte->SXY[2], sx16, sy16);

    // Step 5: Depth cueing (MAC0/IR0) — only for last vertex of RTPT or RTPS
    if (setMac0) {
        int64_t mac0 = (int64_t)gte->DQA * h_div_sz + gte->DQB;
        gte->check_mac0_overflow(mac0);
        gte->MAC0 = static_cast<int32_t>(mac0);
        gte->IR0 = gte->saturate_ir0(static_cast<int32_t>(mac0 >> 12));
    }
}

// RTPS (0x01) — single vertex, always sets MAC0
void gte_rtps(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    gte_rtps_internal(gte, gte->V0, true);
    gte->set_error_flag();
}

// RTPT (0x30) — triple vertex, only last sets MAC0
void gte_rtpt(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    gte_rtps_internal(gte, gte->V0, false);
    gte_rtps_internal(gte, gte->V1, false);
    gte_rtps_internal(gte, gte->V2, true);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// NCLIP (0x06) — Normal Clipping (2D cross product for backface culling)
// ---------------------------------------------------------------------------
void gte_nclip(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    int16_t sx0 = static_cast<int16_t>(gte->SXY[0] & 0xFFFF);
    int16_t sy0 = static_cast<int16_t>(gte->SXY[0] >> 16);
    int16_t sx1 = static_cast<int16_t>(gte->SXY[1] & 0xFFFF);
    int16_t sy1 = static_cast<int16_t>(gte->SXY[1] >> 16);
    int16_t sx2 = static_cast<int16_t>(gte->SXY[2] & 0xFFFF);
    int16_t sy2 = static_cast<int16_t>(gte->SXY[2] >> 16);
    int64_t mac0 = (int64_t)sx0 * (sy1 - sy2) +
                   (int64_t)sx1 * (sy2 - sy0) +
                   (int64_t)sx2 * (sy0 - sy1);
    gte->check_mac0_overflow(mac0);
    gte->MAC0 = static_cast<int32_t>(mac0);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// AVSZ3 (0x2D) — Average Z (3 points)
// ---------------------------------------------------------------------------
void gte_avsz3(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    int64_t mac0 = (int64_t)gte->ZSF3 * (gte->SZ[1] + gte->SZ[2] + gte->SZ[3]);
    gte->check_mac0_overflow(mac0);
    gte->MAC0 = static_cast<int32_t>(mac0 >> 12);
    gte->OTZ = gte->saturate_sz(gte->MAC0);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// AVSZ4 (0x2E) — Average Z (4 points)
// ---------------------------------------------------------------------------
void gte_avsz4(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    int64_t mac0 = (int64_t)gte->ZSF4 * (gte->SZ[0] + gte->SZ[1] + gte->SZ[2] + gte->SZ[3]);
    gte->check_mac0_overflow(mac0);
    gte->MAC0 = static_cast<int32_t>(mac0 >> 12);
    gte->OTZ = gte->saturate_sz(gte->MAC0);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// NCCS (0x1B) — Normal Color Color Single
// ---------------------------------------------------------------------------
void gte_nccs_internal(GTEState* gte, int16_t* V, uint32_t instr) {
    light_transform(gte, V, instr);
    light_color(gte, instr);
    color_output(gte, instr);
}

void gte_nccs(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    gte_nccs_internal(gte, gte->V0, instr);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// NCCT (0x3F) — Normal Color Color Triple
// ---------------------------------------------------------------------------
void gte_ncct(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    gte_nccs_internal(gte, gte->V0, instr);
    gte_nccs_internal(gte, gte->V1, instr);
    gte_nccs_internal(gte, gte->V2, instr);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// NCDS (0x13) — Normal Color Depth Cue Single
// ---------------------------------------------------------------------------
void gte_ncds_internal(GTEState* gte, int16_t* V, uint32_t instr) {
    light_transform(gte, V, instr);
    light_color(gte, instr);
    depth_cue_from_ir(gte, instr);
    color_output(gte, instr);
}

void gte_ncds(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    gte_ncds_internal(gte, gte->V0, instr);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// NCDT (0x16) — Normal Color Depth Cue Triple
// ---------------------------------------------------------------------------
void gte_ncdt(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    gte_ncds_internal(gte, gte->V0, instr);
    gte_ncds_internal(gte, gte->V1, instr);
    gte_ncds_internal(gte, gte->V2, instr);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// NCS (0x1E) — Normal Color Single (no vertex color multiply)
// ---------------------------------------------------------------------------
void gte_ncs_internal(GTEState* gte, int16_t* V, uint32_t instr) {
    light_transform(gte, V, instr);
    light_color(gte, instr);
    // Output directly (no RGBC multiply)
    gte->push_rgb(
        gte->saturate_color(gte->MAC1 >> 4, 0),
        gte->saturate_color(gte->MAC2 >> 4, 1),
        gte->saturate_color(gte->MAC3 >> 4, 2));
}

void gte_ncs(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    gte_ncs_internal(gte, gte->V0, instr);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// NCT (0x20) — Normal Color Triple
// ---------------------------------------------------------------------------
void gte_nct(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    gte_ncs_internal(gte, gte->V0, instr);
    gte_ncs_internal(gte, gte->V1, instr);
    gte_ncs_internal(gte, gte->V2, instr);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// DPCS (0x10) — Depth Cueing Single (from RGBC color)
// ---------------------------------------------------------------------------
void gte_dpcs(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    uint8_t r = (gte->RGBC >> 0)  & 0xFF;
    uint8_t g = (gte->RGBC >> 8)  & 0xFF;
    uint8_t b = (gte->RGBC >> 16) & 0xFF;
    // MAC = RGBC << 4
    gte->IR1 = gte->saturate_ir(r << 4, 1, false);
    gte->IR2 = gte->saturate_ir(g << 4, 2, false);
    gte->IR3 = gte->saturate_ir(b << 4, 3, false);
    // Interpolate toward far color using IR0
    depth_cue_from_ir(gte, instr);
    // Output
    gte->push_rgb(
        gte->saturate_color(gte->MAC1 >> 4, 0),
        gte->saturate_color(gte->MAC2 >> 4, 1),
        gte->saturate_color(gte->MAC3 >> 4, 2));
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// DPCT (0x2A) — Depth Cueing Triple (from RGB FIFO entries)
// ---------------------------------------------------------------------------
void gte_dpct(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    for (int i = 0; i < 3; i++) {
        uint8_t r = (gte->RGB[0] >> 0)  & 0xFF;
        uint8_t g = (gte->RGB[0] >> 8)  & 0xFF;
        uint8_t b = (gte->RGB[0] >> 16) & 0xFF;
        gte->IR1 = gte->saturate_ir(r << 4, 1, false);
        gte->IR2 = gte->saturate_ir(g << 4, 2, false);
        gte->IR3 = gte->saturate_ir(b << 4, 3, false);
        depth_cue_from_ir(gte, instr);
        gte->push_rgb(
            gte->saturate_color(gte->MAC1 >> 4, 0),
            gte->saturate_color(gte->MAC2 >> 4, 1),
            gte->saturate_color(gte->MAC3 >> 4, 2));
    }
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// DPCL (0x29) — Depth Cueing Light (from IR, uses existing lighting result)
// ---------------------------------------------------------------------------
void gte_dpcl(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    // IR already contains lighting result, multiply by RGBC
    uint8_t r = (gte->RGBC >> 0)  & 0xFF;
    uint8_t g = (gte->RGBC >> 8)  & 0xFF;
    uint8_t b = (gte->RGBC >> 16) & 0xFF;
    gte->MAC1 = (r * gte->IR1) >> 8;
    gte->MAC2 = (g * gte->IR2) >> 8;
    gte->MAC3 = (b * gte->IR3) >> 8;
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, gte_instr_lm(instr));
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, gte_instr_lm(instr));
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, gte_instr_lm(instr));
    // Depth cue toward far color
    depth_cue_from_ir(gte, instr);
    gte->push_rgb(
        gte->saturate_color(gte->MAC1 >> 4, 0),
        gte->saturate_color(gte->MAC2 >> 4, 1),
        gte->saturate_color(gte->MAC3 >> 4, 2));
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// INTPL (0x11) — Interpolation (IR toward far color using IR0)
// ---------------------------------------------------------------------------
void gte_intpl(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    depth_cue_from_ir(gte, instr);
    gte->push_rgb(
        gte->saturate_color(gte->MAC1 >> 4, 0),
        gte->saturate_color(gte->MAC2 >> 4, 1),
        gte->saturate_color(gte->MAC3 >> 4, 2));
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// CDP (0x14) — Color Depth Cue (light color + depth cue, no normal transform)
// ---------------------------------------------------------------------------
void gte_cdp(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    // IR1/IR2/IR3 already set (from previous NCS or similar)
    light_color(gte, instr);
    depth_cue_from_ir(gte, instr);
    color_output(gte, instr);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// CC (0x1C) — Color Color (light color + vertex color, no depth cue)
// ---------------------------------------------------------------------------
void gte_cc(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    light_color(gte, instr);
    color_output(gte, instr);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// MVMVA (0x12) — Matrix Vector Multiply Add
// Highly configurable: matrix, vector, and translation selected by bits
// ---------------------------------------------------------------------------
void gte_mvmva(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    uint32_t mx = (instr >> 17) & 3;  // Matrix: 0=RT, 1=Light, 2=LightColor, 3=reserved
    uint32_t vv = (instr >> 15) & 3;  // Vector: 0=V0, 1=V1, 2=V2, 3=IR
    uint32_t tv = (instr >> 13) & 3;  // Translation: 0=TR, 1=BK, 2=FC/bugged, 3=none
    int sf = (instr >> 19) & 1;        // Shift: 0=no shift, 1=shift right 12

    // Select matrix
    int16_t M[3][3];
    switch (mx) {
        case 0: std::memcpy(M, gte->RT, sizeof(M)); break;
        case 1: std::memcpy(M, gte->L, sizeof(M)); break;
        case 2: std::memcpy(M, gte->LC, sizeof(M)); break;
        default: std::memset(M, 0, sizeof(M)); break; // Garbage on real HW
    }

    // Select vector
    int16_t V[3];
    switch (vv) {
        case 0: V[0] = gte->V0[0]; V[1] = gte->V0[1]; V[2] = gte->V0[2]; break;
        case 1: V[0] = gte->V1[0]; V[1] = gte->V1[1]; V[2] = gte->V1[2]; break;
        case 2: V[0] = gte->V2[0]; V[1] = gte->V2[1]; V[2] = gte->V2[2]; break;
        case 3: V[0] = gte->IR1;   V[1] = gte->IR2;   V[2] = gte->IR3;   break;
    }

    // Select translation vector
    int64_t T[3];
    switch (tv) {
        case 0: T[0] = (int64_t)gte->TR[0] * 4096; T[1] = (int64_t)gte->TR[1] * 4096; T[2] = (int64_t)gte->TR[2] * 4096; break;
        case 1: T[0] = (int64_t)gte->BK[0] * 4096; T[1] = (int64_t)gte->BK[1] * 4096; T[2] = (int64_t)gte->BK[2] * 4096; break;
        case 2: T[0] = (int64_t)gte->FC[0] * 4096; T[1] = (int64_t)gte->FC[1] * 4096; T[2] = (int64_t)gte->FC[2] * 4096; break;
        case 3: T[0] = 0; T[1] = 0; T[2] = 0; break;
    }

    // Multiply
    int64_t mac1 = T[0] + (int64_t)M[0][0] * V[0] + (int64_t)M[0][1] * V[1] + (int64_t)M[0][2] * V[2];
    int64_t mac2 = T[1] + (int64_t)M[1][0] * V[0] + (int64_t)M[1][1] * V[1] + (int64_t)M[1][2] * V[2];
    int64_t mac3 = T[2] + (int64_t)M[2][0] * V[0] + (int64_t)M[2][1] * V[1] + (int64_t)M[2][2] * V[2];

    if (sf) {
        mac1 >>= 12; mac2 >>= 12; mac3 >>= 12;
    }

    gte->check_mac_overflow(mac1, 1);
    gte->check_mac_overflow(mac2, 2);
    gte->check_mac_overflow(mac3, 3);

    gte->MAC1 = static_cast<int32_t>(mac1);
    gte->MAC2 = static_cast<int32_t>(mac2);
    gte->MAC3 = static_cast<int32_t>(mac3);

    bool lm = (instr >> 10) & 1;
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, lm);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, lm);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, lm);

    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// OP (0x0C) — Outer Product of two vectors (cross product)
// ---------------------------------------------------------------------------
void gte_op(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    int sf = (instr >> 19) & 1;
    // D1=RT[0][0], D2=RT[1][1], D3=RT[2][2] (rotation matrix diagonal)
    int16_t d1 = gte->RT[0][0];
    int16_t d2 = gte->RT[1][1];
    int16_t d3 = gte->RT[2][2];

    int64_t mac1 = (int64_t)d2 * gte->IR3 - (int64_t)d3 * gte->IR2;
    int64_t mac2 = (int64_t)d3 * gte->IR1 - (int64_t)d1 * gte->IR3;
    int64_t mac3 = (int64_t)d1 * gte->IR2 - (int64_t)d2 * gte->IR1;

    if (sf) { mac1 >>= 12; mac2 >>= 12; mac3 >>= 12; }

    gte->MAC1 = static_cast<int32_t>(mac1);
    gte->MAC2 = static_cast<int32_t>(mac2);
    gte->MAC3 = static_cast<int32_t>(mac3);

    bool lm = (instr >> 10) & 1;
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, lm);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, lm);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, lm);

    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// SQR (0x28) — Square of IR vector
// ---------------------------------------------------------------------------
void gte_sqr(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    int sf = (instr >> 19) & 1;

    int64_t mac1 = (int64_t)gte->IR1 * gte->IR1;
    int64_t mac2 = (int64_t)gte->IR2 * gte->IR2;
    int64_t mac3 = (int64_t)gte->IR3 * gte->IR3;

    if (sf) { mac1 >>= 12; mac2 >>= 12; mac3 >>= 12; }

    gte->MAC1 = static_cast<int32_t>(mac1);
    gte->MAC2 = static_cast<int32_t>(mac2);
    gte->MAC3 = static_cast<int32_t>(mac3);

    bool lm = (instr >> 10) & 1;
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, lm);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, lm);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, lm);

    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// GPF (0x3D) — General Purpose Interpolation (IR0 * IR)
// ---------------------------------------------------------------------------
void gte_gpf(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    int sf = (instr >> 19) & 1;

    int64_t mac1 = (int64_t)gte->IR0 * gte->IR1;
    int64_t mac2 = (int64_t)gte->IR0 * gte->IR2;
    int64_t mac3 = (int64_t)gte->IR0 * gte->IR3;

    if (sf) { mac1 >>= 12; mac2 >>= 12; mac3 >>= 12; }

    gte->MAC1 = static_cast<int32_t>(mac1);
    gte->MAC2 = static_cast<int32_t>(mac2);
    gte->MAC3 = static_cast<int32_t>(mac3);

    bool lm = (instr >> 10) & 1;
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, lm);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, lm);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, lm);

    gte->push_rgb(
        gte->saturate_color(gte->MAC1 >> 4, 0),
        gte->saturate_color(gte->MAC2 >> 4, 1),
        gte->saturate_color(gte->MAC3 >> 4, 2));

    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// GPL (0x3E) — General Purpose Interpolation (MAC + IR0 * IR)
// ---------------------------------------------------------------------------
void gte_gpl(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    int sf = (instr >> 19) & 1;

    int64_t mac1 = (sf ? (int64_t)gte->MAC1 * 4096 : (int64_t)gte->MAC1) + (int64_t)gte->IR0 * gte->IR1;
    int64_t mac2 = (sf ? (int64_t)gte->MAC2 * 4096 : (int64_t)gte->MAC2) + (int64_t)gte->IR0 * gte->IR2;
    int64_t mac3 = (sf ? (int64_t)gte->MAC3 * 4096 : (int64_t)gte->MAC3) + (int64_t)gte->IR0 * gte->IR3;

    if (sf) { mac1 >>= 12; mac2 >>= 12; mac3 >>= 12; }

    gte->MAC1 = static_cast<int32_t>(mac1);
    gte->MAC2 = static_cast<int32_t>(mac2);
    gte->MAC3 = static_cast<int32_t>(mac3);

    bool lm = (instr >> 10) & 1;
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, lm);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, lm);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, lm);

    gte->push_rgb(
        gte->saturate_color(gte->MAC1 >> 4, 0),
        gte->saturate_color(gte->MAC2 >> 4, 1),
        gte->saturate_color(gte->MAC3 >> 4, 2));

    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// MTC2 — Move To Coprocessor 2 (write GTE data register)
// ---------------------------------------------------------------------------
void gte_mtc2(GTEState* gte, uint8_t reg, uint32_t value) {
    switch (reg) {
        case 0:  gte->V0[0] = value & 0xFFFF; gte->V0[1] = value >> 16; break;
        case 1:  gte->V0[2] = value & 0xFFFF; break;
        case 2:  gte->V1[0] = value & 0xFFFF; gte->V1[1] = value >> 16; break;
        case 3:  gte->V1[2] = value & 0xFFFF; break;
        case 4:  gte->V2[0] = value & 0xFFFF; gte->V2[1] = value >> 16; break;
        case 5:  gte->V2[2] = value & 0xFFFF; break;
        case 6:  gte->RGBC = value; break;
        case 7:  gte->OTZ = value & 0xFFFF; break;
        case 8:  gte->IR0 = static_cast<int16_t>(value & 0xFFFF); break;
        case 9:  gte->IR1 = static_cast<int16_t>(value & 0xFFFF); break;
        case 10: gte->IR2 = static_cast<int16_t>(value & 0xFFFF); break;
        case 11: gte->IR3 = static_cast<int16_t>(value & 0xFFFF); break;
        case 12: gte->SXY[0] = value; break;
        case 13: gte->SXY[1] = value; break;
        case 14: gte->SXY[2] = value; gte->SXY[3] = value; break;
        case 15: // SXYP — push to FIFO
            gte->SXY[0] = gte->SXY[1]; gte->SXY[1] = gte->SXY[2];
            gte->SXY[2] = value; gte->SXY[3] = value; break;
        case 16: gte->SZ[0] = value & 0xFFFF; break;
        case 17: gte->SZ[1] = value & 0xFFFF; break;
        case 18: gte->SZ[2] = value & 0xFFFF; break;
        case 19: gte->SZ[3] = value & 0xFFFF; break;
        case 20: gte->RGB[0] = value; break;
        case 21: gte->RGB[1] = value; break;
        case 22: gte->RGB[2] = value; break;
        case 23: break; // RES1 reserved
        case 24: gte->MAC0 = value; break;
        case 25: gte->MAC1 = value; break;
        case 26: gte->MAC2 = value; break;
        case 27: gte->MAC3 = value; break;
        case 28: // IRGB — packed 5-bit RGB → IR1/IR2/IR3
            gte->IR1 = (value & 0x1F) << 7;
            gte->IR2 = ((value >> 5) & 0x1F) << 7;
            gte->IR3 = ((value >> 10) & 0x1F) << 7;
            break;
        case 29: break; // ORGB read-only
        case 30: // LZCS — set source and compute LZCR
            gte->LZCS = static_cast<int32_t>(value);
            {
                uint32_t v = value;
                if ((int32_t)value < 0) v = ~v;
                int cnt = 0;
                if (v == 0) { cnt = 32; }
                else { while (!(v & 0x80000000u)) { v <<= 1; cnt++; } }
                gte->LZCR = cnt;
            }
            break;
        case 31: break; // LZCR read-only
        default: break;
    }
}

// ---------------------------------------------------------------------------
// MFC2 — Move From Coprocessor 2 (read GTE data register)
// ---------------------------------------------------------------------------
uint32_t gte_mfc2(GTEState* gte, uint8_t reg) {
    switch (reg) {
        case 0:  return (static_cast<uint16_t>(gte->V0[1]) << 16) | static_cast<uint16_t>(gte->V0[0]);
        case 1:  return static_cast<uint16_t>(gte->V0[2]);
        case 2:  return (static_cast<uint16_t>(gte->V1[1]) << 16) | static_cast<uint16_t>(gte->V1[0]);
        case 3:  return static_cast<uint16_t>(gte->V1[2]);
        case 4:  return (static_cast<uint16_t>(gte->V2[1]) << 16) | static_cast<uint16_t>(gte->V2[0]);
        case 5:  return static_cast<uint16_t>(gte->V2[2]);
        case 6:  return gte->RGBC;
        case 7:  return gte->OTZ;
        case 8:  return static_cast<int32_t>(gte->IR0);  // sign-extend
        case 9:  return static_cast<int32_t>(gte->IR1);
        case 10: return static_cast<int32_t>(gte->IR2);
        case 11: return static_cast<int32_t>(gte->IR3);
        case 12: return gte->SXY[0];
        case 13: return gte->SXY[1];
        case 14: return gte->SXY[2];
        case 15: return gte->SXY[3];
        case 16: return gte->SZ[0];
        case 17: return gte->SZ[1];
        case 18: return gte->SZ[2];
        case 19: return gte->SZ[3];
        case 20: return gte->RGB[0];
        case 21: return gte->RGB[1];
        case 22: return gte->RGB[2];
        case 23: return 0; // RES1
        case 24: return gte->MAC0;
        case 25: return gte->MAC1;
        case 26: return gte->MAC2;
        case 27: return gte->MAC3;
        case 28: case 29: { // IRGB/ORGB
            uint32_t r = std::clamp((int)(gte->IR1 >> 7), 0, 0x1F);
            uint32_t g = std::clamp((int)(gte->IR2 >> 7), 0, 0x1F);
            uint32_t b = std::clamp((int)(gte->IR3 >> 7), 0, 0x1F);
            return (b << 10) | (g << 5) | r;
        }
        case 30: return gte->LZCS;
        case 31: return gte->LZCR;
        default: return 0;
    }
}

// ---------------------------------------------------------------------------
// CTC2 — Move To Coprocessor 2 Control (write GTE control register)
// ---------------------------------------------------------------------------
void gte_ctc2(GTEState* gte, uint8_t reg, uint32_t value) {
    switch (reg) {
        case 0:  gte->RT[0][0] = value & 0xFFFF; gte->RT[0][1] = value >> 16; break;
        case 1:  gte->RT[0][2] = value & 0xFFFF; gte->RT[1][0] = value >> 16; break;
        case 2:  gte->RT[1][1] = value & 0xFFFF; gte->RT[1][2] = value >> 16; break;
        case 3:  gte->RT[2][0] = value & 0xFFFF; gte->RT[2][1] = value >> 16; break;
        case 4:  gte->RT[2][2] = value & 0xFFFF; break;
        case 5:  gte->TR[0] = value; break;
        case 6:  gte->TR[1] = value; break;
        case 7:  gte->TR[2] = value; break;
        case 8:  gte->L[0][0] = value & 0xFFFF; gte->L[0][1] = value >> 16; break;
        case 9:  gte->L[0][2] = value & 0xFFFF; gte->L[1][0] = value >> 16; break;
        case 10: gte->L[1][1] = value & 0xFFFF; gte->L[1][2] = value >> 16; break;
        case 11: gte->L[2][0] = value & 0xFFFF; gte->L[2][1] = value >> 16; break;
        case 12: gte->L[2][2] = value & 0xFFFF; break;
        case 13: gte->BK[0] = value; break;
        case 14: gte->BK[1] = value; break;
        case 15: gte->BK[2] = value; break;
        case 16: gte->LC[0][0] = value & 0xFFFF; gte->LC[0][1] = value >> 16; break;
        case 17: gte->LC[0][2] = value & 0xFFFF; gte->LC[1][0] = value >> 16; break;
        case 18: gte->LC[1][1] = value & 0xFFFF; gte->LC[1][2] = value >> 16; break;
        case 19: gte->LC[2][0] = value & 0xFFFF; gte->LC[2][1] = value >> 16; break;
        case 20: gte->LC[2][2] = value & 0xFFFF; break;
        case 21: gte->FC[0] = value; break;
        case 22: gte->FC[1] = value; break;
        case 23: gte->FC[2] = value; break;
        case 24: gte->OFX = value; break;
        case 25: gte->OFY = value; break;
        case 26: gte->H = value & 0xFFFF; break;
        case 27: gte->DQA = static_cast<int16_t>(value & 0xFFFF); break;
        case 28: gte->DQB = value; break;
        case 29: gte->ZSF3 = static_cast<int16_t>(value & 0xFFFF); break;
        case 30: gte->ZSF4 = static_cast<int16_t>(value & 0xFFFF); break;
        case 31: gte->FLAG = value & 0x7FFFF000u; break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// CFC2 — Move From Coprocessor 2 Control (read GTE control register)
// ---------------------------------------------------------------------------
uint32_t gte_cfc2(GTEState* gte, uint8_t reg) {
    switch (reg) {
        case 0:  return (static_cast<uint16_t>(gte->RT[0][1]) << 16) | static_cast<uint16_t>(gte->RT[0][0]);
        case 1:  return (static_cast<uint16_t>(gte->RT[1][0]) << 16) | static_cast<uint16_t>(gte->RT[0][2]);
        case 2:  return (static_cast<uint16_t>(gte->RT[1][2]) << 16) | static_cast<uint16_t>(gte->RT[1][1]);
        case 3:  return (static_cast<uint16_t>(gte->RT[2][1]) << 16) | static_cast<uint16_t>(gte->RT[2][0]);
        case 4:  return static_cast<uint16_t>(gte->RT[2][2]);
        case 5:  return gte->TR[0];
        case 6:  return gte->TR[1];
        case 7:  return gte->TR[2];
        case 8:  return (static_cast<uint16_t>(gte->L[0][1]) << 16) | static_cast<uint16_t>(gte->L[0][0]);
        case 9:  return (static_cast<uint16_t>(gte->L[1][0]) << 16) | static_cast<uint16_t>(gte->L[0][2]);
        case 10: return (static_cast<uint16_t>(gte->L[1][2]) << 16) | static_cast<uint16_t>(gte->L[1][1]);
        case 11: return (static_cast<uint16_t>(gte->L[2][1]) << 16) | static_cast<uint16_t>(gte->L[2][0]);
        case 12: return static_cast<uint16_t>(gte->L[2][2]);
        case 13: return gte->BK[0];
        case 14: return gte->BK[1];
        case 15: return gte->BK[2];
        case 16: return (static_cast<uint16_t>(gte->LC[0][1]) << 16) | static_cast<uint16_t>(gte->LC[0][0]);
        case 17: return (static_cast<uint16_t>(gte->LC[1][0]) << 16) | static_cast<uint16_t>(gte->LC[0][2]);
        case 18: return (static_cast<uint16_t>(gte->LC[1][2]) << 16) | static_cast<uint16_t>(gte->LC[1][1]);
        case 19: return (static_cast<uint16_t>(gte->LC[2][1]) << 16) | static_cast<uint16_t>(gte->LC[2][0]);
        case 20: return static_cast<uint16_t>(gte->LC[2][2]);
        case 21: return gte->FC[0];
        case 22: return gte->FC[1];
        case 23: return gte->FC[2];
        case 24: return gte->OFX;
        case 25: return gte->OFY;
        case 26: return gte->H;
        case 27: return static_cast<int32_t>(gte->DQA); // sign-extend
        case 28: return gte->DQB;
        case 29: return static_cast<int32_t>(gte->ZSF3);
        case 30: return static_cast<int32_t>(gte->ZSF4);
        case 31: return gte->FLAG;
        default: return 0;
    }
}

} // namespace GTE
} // namespace PSXRecomp

// ---------------------------------------------------------------------------
// gte_execute — C-linkage bridge called from generated code
// ---------------------------------------------------------------------------
static uint64_t s_gte_exec_count = 0;
extern "C" uint64_t gte_get_exec_count(void) { return s_gte_exec_count; }
static uint32_t s_gte_replay_saved_caller_ra = 0;
extern "C" int gte_replay_side_effects_begin(void) {
    using namespace PSXRecomp::GTE;
    if (s_gte_replay_sandbox) return 0;
    s_gte_replay_saved_caller_ra = s_gte_caller_ra;
    s_gte_replay_sandbox = 1;
    return 1;
}
extern "C" void gte_replay_side_effects_end(void) {
    using namespace PSXRecomp::GTE;
    if (!s_gte_replay_sandbox) return;
    s_gte_caller_ra = s_gte_replay_saved_caller_ra;
    s_gte_replay_sandbox = 0;
}

static uint32_t gte_cpu_lzcr(uint32_t value);

static inline int16_t gte_unpack_s16(uint32_t value) {
    return static_cast<int16_t>(value & 0xFFFFu);
}

static inline uint32_t gte_pack_s16_pair(int16_t lo, int16_t hi) {
    return static_cast<uint32_t>(static_cast<uint16_t>(lo)) |
           (static_cast<uint32_t>(static_cast<uint16_t>(hi)) << 16);
}

/* Exact full marshaling for GTE commands. Keep this explicit: GTEState has
 * mixed-width fields and padding, while CPUState exposes architectural 32-bit
 * register words. Whole-struct copies would be layout/endian dependent. */
static void gte_import_cpu_state(PSXRecomp::GTE::GTEState* gte,
                                 const CPUState* cpu) {
    const uint32_t* d = cpu->gte_data;
    const uint32_t* c = cpu->gte_ctrl;

    gte->V0[0] = gte_unpack_s16(d[0]);
    gte->V0[1] = gte_unpack_s16(d[0] >> 16);
    gte->V0[2] = gte_unpack_s16(d[1]);
    gte->V1[0] = gte_unpack_s16(d[2]);
    gte->V1[1] = gte_unpack_s16(d[2] >> 16);
    gte->V1[2] = gte_unpack_s16(d[3]);
    gte->V2[0] = gte_unpack_s16(d[4]);
    gte->V2[1] = gte_unpack_s16(d[4] >> 16);
    gte->V2[2] = gte_unpack_s16(d[5]);
    gte->RGBC = d[6];
    gte->OTZ = static_cast<uint16_t>(d[7]);
    gte->IR0 = gte_unpack_s16(d[8]);
    gte->IR1 = gte_unpack_s16(d[9]);
    gte->IR2 = gte_unpack_s16(d[10]);
    gte->IR3 = gte_unpack_s16(d[11]);
    gte->SXY[0] = static_cast<int32_t>(d[12]);
    gte->SXY[1] = static_cast<int32_t>(d[13]);
    gte->SXY[2] = static_cast<int32_t>(d[14]);
    gte->SXY[3] = static_cast<int32_t>(d[14]);
    for (int i = 0; i < 4; ++i) gte->SZ[i] = static_cast<uint16_t>(d[16 + i]);
    for (int i = 0; i < 3; ++i) gte->RGB[i] = d[20 + i];
    gte->MAC0 = static_cast<int32_t>(d[24]);
    gte->MAC1 = static_cast<int32_t>(d[25]);
    gte->MAC2 = static_cast<int32_t>(d[26]);
    gte->MAC3 = static_cast<int32_t>(d[27]);
    /* Skip IRGB (28): importing it would quantize and overwrite IR1..3. */
    gte->LZCS = static_cast<int32_t>(d[30]);
    gte->LZCR = static_cast<int32_t>(gte_cpu_lzcr(d[30]));

    gte->RT[0][0] = gte_unpack_s16(c[0]);
    gte->RT[0][1] = gte_unpack_s16(c[0] >> 16);
    gte->RT[0][2] = gte_unpack_s16(c[1]);
    gte->RT[1][0] = gte_unpack_s16(c[1] >> 16);
    gte->RT[1][1] = gte_unpack_s16(c[2]);
    gte->RT[1][2] = gte_unpack_s16(c[2] >> 16);
    gte->RT[2][0] = gte_unpack_s16(c[3]);
    gte->RT[2][1] = gte_unpack_s16(c[3] >> 16);
    gte->RT[2][2] = gte_unpack_s16(c[4]);
    for (int i = 0; i < 3; ++i) gte->TR[i] = static_cast<int32_t>(c[5 + i]);

    gte->L[0][0] = gte_unpack_s16(c[8]);
    gte->L[0][1] = gte_unpack_s16(c[8] >> 16);
    gte->L[0][2] = gte_unpack_s16(c[9]);
    gte->L[1][0] = gte_unpack_s16(c[9] >> 16);
    gte->L[1][1] = gte_unpack_s16(c[10]);
    gte->L[1][2] = gte_unpack_s16(c[10] >> 16);
    gte->L[2][0] = gte_unpack_s16(c[11]);
    gte->L[2][1] = gte_unpack_s16(c[11] >> 16);
    gte->L[2][2] = gte_unpack_s16(c[12]);
    for (int i = 0; i < 3; ++i) gte->BK[i] = static_cast<int32_t>(c[13 + i]);

    gte->LC[0][0] = gte_unpack_s16(c[16]);
    gte->LC[0][1] = gte_unpack_s16(c[16] >> 16);
    gte->LC[0][2] = gte_unpack_s16(c[17]);
    gte->LC[1][0] = gte_unpack_s16(c[17] >> 16);
    gte->LC[1][1] = gte_unpack_s16(c[18]);
    gte->LC[1][2] = gte_unpack_s16(c[18] >> 16);
    gte->LC[2][0] = gte_unpack_s16(c[19]);
    gte->LC[2][1] = gte_unpack_s16(c[19] >> 16);
    gte->LC[2][2] = gte_unpack_s16(c[20]);
    for (int i = 0; i < 3; ++i) gte->FC[i] = static_cast<int32_t>(c[21 + i]);
    gte->OFX = static_cast<int32_t>(c[24]);
    gte->OFY = static_cast<int32_t>(c[25]);
    gte->H = static_cast<uint16_t>(c[26]);
    gte->DQA = gte_unpack_s16(c[27]);
    gte->DQB = static_cast<int32_t>(c[28]);
    gte->ZSF3 = gte_unpack_s16(c[29]);
    gte->ZSF4 = gte_unpack_s16(c[30]);
    gte->FLAG = c[31] & 0x7FFFF000u;
}

static uint32_t gte_state_pack_irgb(const PSXRecomp::GTE::GTEState* gte) {
    const uint32_t r = static_cast<uint32_t>(std::clamp(
        static_cast<int>(gte->IR1 >> 7), 0, 0x1F));
    const uint32_t g = static_cast<uint32_t>(std::clamp(
        static_cast<int>(gte->IR2 >> 7), 0, 0x1F));
    const uint32_t b = static_cast<uint32_t>(std::clamp(
        static_cast<int>(gte->IR3 >> 7), 0, 0x1F));
    return (b << 10) | (g << 5) | r;
}

static void gte_export_cpu_state(CPUState* cpu,
                                 const PSXRecomp::GTE::GTEState* gte) {
    uint32_t* d = cpu->gte_data;
    uint32_t* c = cpu->gte_ctrl;

    d[0] = gte_pack_s16_pair(gte->V0[0], gte->V0[1]);
    d[1] = static_cast<uint16_t>(gte->V0[2]);
    d[2] = gte_pack_s16_pair(gte->V1[0], gte->V1[1]);
    d[3] = static_cast<uint16_t>(gte->V1[2]);
    d[4] = gte_pack_s16_pair(gte->V2[0], gte->V2[1]);
    d[5] = static_cast<uint16_t>(gte->V2[2]);
    d[6] = gte->RGBC;
    d[7] = gte->OTZ;
    d[8] = static_cast<uint32_t>(static_cast<int32_t>(gte->IR0));
    d[9] = static_cast<uint32_t>(static_cast<int32_t>(gte->IR1));
    d[10] = static_cast<uint32_t>(static_cast<int32_t>(gte->IR2));
    d[11] = static_cast<uint32_t>(static_cast<int32_t>(gte->IR3));
    d[12] = static_cast<uint32_t>(gte->SXY[0]);
    d[13] = static_cast<uint32_t>(gte->SXY[1]);
    d[14] = static_cast<uint32_t>(gte->SXY[2]);
    d[15] = static_cast<uint32_t>(gte->SXY[3]);
    for (int i = 0; i < 4; ++i) d[16 + i] = gte->SZ[i];
    for (int i = 0; i < 3; ++i) d[20 + i] = gte->RGB[i];
    d[23] = 0;
    d[24] = static_cast<uint32_t>(gte->MAC0);
    d[25] = static_cast<uint32_t>(gte->MAC1);
    d[26] = static_cast<uint32_t>(gte->MAC2);
    d[27] = static_cast<uint32_t>(gte->MAC3);
    d[28] = d[29] = gte_state_pack_irgb(gte);
    d[30] = static_cast<uint32_t>(gte->LZCS);
    d[31] = static_cast<uint32_t>(gte->LZCR);

    c[0] = gte_pack_s16_pair(gte->RT[0][0], gte->RT[0][1]);
    c[1] = gte_pack_s16_pair(gte->RT[0][2], gte->RT[1][0]);
    c[2] = gte_pack_s16_pair(gte->RT[1][1], gte->RT[1][2]);
    c[3] = gte_pack_s16_pair(gte->RT[2][0], gte->RT[2][1]);
    c[4] = static_cast<uint16_t>(gte->RT[2][2]);
    for (int i = 0; i < 3; ++i) c[5 + i] = static_cast<uint32_t>(gte->TR[i]);
    c[8] = gte_pack_s16_pair(gte->L[0][0], gte->L[0][1]);
    c[9] = gte_pack_s16_pair(gte->L[0][2], gte->L[1][0]);
    c[10] = gte_pack_s16_pair(gte->L[1][1], gte->L[1][2]);
    c[11] = gte_pack_s16_pair(gte->L[2][0], gte->L[2][1]);
    c[12] = static_cast<uint16_t>(gte->L[2][2]);
    for (int i = 0; i < 3; ++i) c[13 + i] = static_cast<uint32_t>(gte->BK[i]);
    c[16] = gte_pack_s16_pair(gte->LC[0][0], gte->LC[0][1]);
    c[17] = gte_pack_s16_pair(gte->LC[0][2], gte->LC[1][0]);
    c[18] = gte_pack_s16_pair(gte->LC[1][1], gte->LC[1][2]);
    c[19] = gte_pack_s16_pair(gte->LC[2][0], gte->LC[2][1]);
    c[20] = static_cast<uint16_t>(gte->LC[2][2]);
    for (int i = 0; i < 3; ++i) c[21 + i] = static_cast<uint32_t>(gte->FC[i]);
    c[24] = static_cast<uint32_t>(gte->OFX);
    c[25] = static_cast<uint32_t>(gte->OFY);
    c[26] = gte->H;
    c[27] = static_cast<uint32_t>(static_cast<int32_t>(gte->DQA));
    c[28] = static_cast<uint32_t>(gte->DQB);
    c[29] = static_cast<uint32_t>(static_cast<int32_t>(gte->ZSF3));
    c[30] = static_cast<uint32_t>(static_cast<int32_t>(gte->ZSF4));
    c[31] = gte->FLAG;
}

static void gte_run_command(PSXRecomp::GTE::GTEState* gte, uint32_t cmd) {
    using namespace PSXRecomp::GTE;
    switch (cmd & 0x3Fu) {
        case 0x01: gte_rtps(gte, cmd); break;
        case 0x06: gte_nclip(gte, cmd); break;
        case 0x0C: gte_op(gte, cmd); break;
        case 0x10: gte_dpcs(gte, cmd); break;
        case 0x11: gte_intpl(gte, cmd); break;
        case 0x12: gte_mvmva(gte, cmd); break;
        case 0x13: gte_ncds(gte, cmd); break;
        case 0x14: gte_cdp(gte, cmd); break;
        case 0x16: gte_ncdt(gte, cmd); break;
        case 0x1B: gte_nccs(gte, cmd); break;
        case 0x1C: gte_cc(gte, cmd); break;
        case 0x1E: gte_ncs(gte, cmd); break;
        case 0x20: gte_nct(gte, cmd); break;
        case 0x28: gte_sqr(gte, cmd); break;
        case 0x29: gte_dpcl(gte, cmd); break;
        case 0x2A: gte_dpct(gte, cmd); break;
        case 0x2D: gte_avsz3(gte, cmd); break;
        case 0x2E: gte_avsz4(gte, cmd); break;
        case 0x30: gte_rtpt(gte, cmd); break;
        case 0x3D: gte_gpf(gte, cmd); break;
        case 0x3E: gte_gpl(gte, cmd); break;
        case 0x3F: gte_ncct(gte, cmd); break;
        default: std::exit(1);
    }
}

extern "C" void gte_execute(CPUState* cpu, uint32_t cmd) {
    using namespace PSXRecomp::GTE;
#ifndef PSX_NO_DEBUG_TOOLS
    if (!s_gte_replay_sandbox) s_gte_exec_count++;
    s_gte_caller_ra = cpu->gpr[31];   /* dome-locate probe: game fn that issued this projection */
#endif

    GTEState gte;
    gte_import_cpu_state(&gte, cpu);

    uint8_t func = cmd & 0x3F;
    /* GTE-activity gameplay detector ([widescreen] gte_game_mode): note every
     * perspective projection so gpu.c can stamp real 3D frames as gameplay
     * (no-op unless the game opts in — early-out on the config flag). */
    if (!s_gte_replay_sandbox && (func == 0x01 || func == 0x30))
        psx_ws_note_gte_project(func == 0x30 ? 3 : 1);
#ifndef PSX_NO_DEBUG_TOOLS
    /* INTPL ring: snapshot inputs before the op mutates IR (outputs recorded
     * after the switch). Debug tooling only; production does no probe copies. */
    int16_t intpl_pre_ir[4] = {0,0,0,0};
    int32_t intpl_pre_fc[3] = {0,0,0};
    if (func == 0x11) {
        intpl_pre_ir[0]=gte.IR0; intpl_pre_ir[1]=gte.IR1;
        intpl_pre_ir[2]=gte.IR2; intpl_pre_ir[3]=gte.IR3;
        intpl_pre_fc[0]=gte.FC[0]; intpl_pre_fc[1]=gte.FC[1]; intpl_pre_fc[2]=gte.FC[2];
    }
#endif
    gte_run_command(&gte, cmd);

#ifndef PSX_NO_DEBUG_TOOLS
    if (func == 0x01 || func == 0x30) gte_rtp_record(&gte, cmd);
    if (func == 0x11) gte_intpl_record(&gte, intpl_pre_ir, intpl_pre_fc);
#endif

    gte_export_cpu_state(cpu, &gte);

#ifdef PSX_ENABLE_BLOCK_CYCLES
    /* Faithful GTE command completion-stall: arm the per-command deadline
     * (serializing back-to-back ops). Any later COP2 register access stalls to
     * it. Single shared site for BOTH backends (compiled + dirty interp both
     * route GTE commands through gte_execute). */
    psx_gte_set(cpu, psx_gte_cmd_latency(cmd));
#endif
}

#ifdef PSX_GTE_REGISTER_TEST
/* Preserved pre-optimization bridge: test oracle for full command marshaling. */
extern "C" void gte_test_execute_reference(CPUState* cpu, uint32_t cmd) {
    using namespace PSXRecomp::GTE;
    GTEState gte;
    for (uint8_t reg = 0; reg < 32; ++reg) {
        if (reg == 15 || reg == 28) continue;
        gte_mtc2(&gte, reg, cpu->gte_data[reg]);
    }
    for (uint8_t reg = 0; reg < 32; ++reg)
        gte_ctc2(&gte, reg, cpu->gte_ctrl[reg]);
    switch (cmd & 0x3Fu) {
        case 0x01: gte_rtps(&gte, cmd); break;
        case 0x06: gte_nclip(&gte, cmd); break;
        case 0x0C: gte_op(&gte, cmd); break;
        case 0x10: gte_dpcs(&gte, cmd); break;
        case 0x11: gte_intpl(&gte, cmd); break;
        case 0x12: gte_mvmva(&gte, cmd); break;
        case 0x13: gte_ncds(&gte, cmd); break;
        case 0x14: gte_cdp(&gte, cmd); break;
        case 0x16: gte_ncdt(&gte, cmd); break;
        case 0x1B: gte_nccs(&gte, cmd); break;
        case 0x1C: gte_cc(&gte, cmd); break;
        case 0x1E: gte_ncs(&gte, cmd); break;
        case 0x20: gte_nct(&gte, cmd); break;
        case 0x28: gte_sqr(&gte, cmd); break;
        case 0x29: gte_dpcl(&gte, cmd); break;
        case 0x2A: gte_dpct(&gte, cmd); break;
        case 0x2D: gte_avsz3(&gte, cmd); break;
        case 0x2E: gte_avsz4(&gte, cmd); break;
        case 0x30: gte_rtpt(&gte, cmd); break;
        case 0x3D: gte_gpf(&gte, cmd); break;
        case 0x3E: gte_gpl(&gte, cmd); break;
        case 0x3F: gte_ncct(&gte, cmd); break;
        default: std::exit(1);
    }
    for (uint8_t reg = 0; reg < 32; ++reg)
        cpu->gte_data[reg] = gte_mfc2(&gte, reg);
    for (uint8_t reg = 0; reg < 32; ++reg)
        cpu->gte_ctrl[reg] = gte_cfc2(&gte, reg);
}
#endif

/* C-callable wrappers for GTE register transfers.
 *
 * CPUState's arrays are the canonical GTE backing store between commands.
 * The old wrappers rebuilt a full GTEState by replaying 62 register writes
 * for every read, and rebuilt plus exported 64 registers for every write.
 * Transfers are common in vertex loops, so that exact-but-accidental work was
 * a severe interpreter-only tax.  These switches implement the same guest
 * register semantics in O(1), without changing the command implementation. */

static uint32_t gte_cpu_lzcr(uint32_t value) {
    uint32_t bits = (value & 0x80000000u) ? ~value : value;
    if (bits == 0) return 32;
    uint32_t count = 0;
    while ((bits & 0x80000000u) == 0) {
        bits <<= 1;
        ++count;
    }
    return count;
}

static uint32_t gte_cpu_irgb_component(uint32_t value) {
    const int32_t ir = static_cast<int16_t>(value & 0xFFFFu);
    if (ir <= 0) return 0;
    const uint32_t scaled = static_cast<uint32_t>(ir) >> 7;
    return scaled > 0x1Fu ? 0x1Fu : scaled;
}

static uint32_t gte_cpu_pack_irgb(const CPUState* cpu) {
    const uint32_t r = gte_cpu_irgb_component(cpu->gte_data[9]);
    const uint32_t g = gte_cpu_irgb_component(cpu->gte_data[10]);
    const uint32_t b = gte_cpu_irgb_component(cpu->gte_data[11]);
    return (b << 10) | (g << 5) | r;
}

static uint32_t gte_sign_extend_16(uint32_t value) {
    return static_cast<uint32_t>(static_cast<int32_t>(
        static_cast<int16_t>(value & 0xFFFFu)));
}

/* Normalize only the guest-visible backing words. This deliberately does not
 * invalidate host precision caches: it is also the compatibility boundary for
 * committed AOT C emitted before all masked/aliased writes used helpers. */
static void gte_cpu_canonicalize_backing(CPUState* cpu) {
    static const uint8_t data_u16[] = {1, 3, 5, 7, 16, 17, 18, 19};
    static const uint8_t data_s16[] = {8, 9, 10, 11};
    static const uint8_t ctrl_u16[] = {4, 12, 20, 26};
    static const uint8_t ctrl_s16[] = {27, 29, 30};

    for (uint8_t reg : data_u16) cpu->gte_data[reg] &= 0xFFFFu;
    for (uint8_t reg : data_s16)
        cpu->gte_data[reg] = gte_sign_extend_16(cpu->gte_data[reg]);
    cpu->gte_data[15] = cpu->gte_data[14];
    cpu->gte_data[23] = 0;
    cpu->gte_data[28] = cpu->gte_data[29] = gte_cpu_pack_irgb(cpu);
    cpu->gte_data[31] = gte_cpu_lzcr(cpu->gte_data[30]);

    for (uint8_t reg : ctrl_u16) cpu->gte_ctrl[reg] &= 0xFFFFu;
    for (uint8_t reg : ctrl_s16)
        cpu->gte_ctrl[reg] = gte_sign_extend_16(cpu->gte_ctrl[reg]);
}

extern "C" uint32_t gte_read_data(CPUState* cpu, uint8_t reg) {
    if (reg >= 32) return 0;
    switch (reg) {
        case 1: case 3: case 5: case 7:
        case 16: case 17: case 18: case 19:
            return cpu->gte_data[reg] & 0xFFFFu;
        case 8: case 9: case 10: case 11:
            return gte_sign_extend_16(cpu->gte_data[reg]);
        case 15:
            return cpu->gte_data[14];
        case 23:
            return 0;
        case 28: case 29:
            return gte_cpu_pack_irgb(cpu);
        case 31:
            return gte_cpu_lzcr(cpu->gte_data[30]);
        default:
            return cpu->gte_data[reg];
    }
}

extern "C" uint32_t gte_read_ctrl(CPUState* cpu, uint8_t reg) {
    if (reg >= 32) return 0;
    switch (reg) {
        case 4: case 12: case 20: case 26:
            return cpu->gte_ctrl[reg] & 0xFFFFu;
        case 27: case 29: case 30:
            return gte_sign_extend_16(cpu->gte_ctrl[reg]);
        default:
            return cpu->gte_ctrl[reg];
    }
}

extern "C" void gte_write_data(CPUState* cpu, uint8_t reg, uint32_t val) {
    if (reg >= 32) return;
    gte_cpu_canonicalize_backing(cpu);
    switch (reg) {
        case 1: case 3: case 5: case 7:
        case 16: case 17: case 18: case 19:
            cpu->gte_data[reg] = val & 0xFFFFu;
            break;
        case 8: case 9: case 10: case 11:
            cpu->gte_data[reg] = gte_sign_extend_16(val);
            if (reg >= 9) {
                const uint32_t packed = gte_cpu_pack_irgb(cpu);
                cpu->gte_data[28] = packed;
                cpu->gte_data[29] = packed;
            }
            break;
        case 12: case 13:
            cpu->gte_data[reg] = val;
            if (!PSXRecomp::GTE::s_gte_replay_sandbox)
                for (int i = 0; i < 4; ++i) PSXRecomp::GTE::s_precise_sxy[i].valid = 0;
            break;
        case 14:
            cpu->gte_data[14] = val;
            cpu->gte_data[15] = val;
            if (!PSXRecomp::GTE::s_gte_replay_sandbox)
                for (int i = 0; i < 4; ++i) PSXRecomp::GTE::s_precise_sxy[i].valid = 0;
            break;
        case 15:
            cpu->gte_data[12] = cpu->gte_data[13];
            cpu->gte_data[13] = cpu->gte_data[14];
            cpu->gte_data[14] = val;
            cpu->gte_data[15] = val;
            if (!PSXRecomp::GTE::s_gte_replay_sandbox)
                for (int i = 0; i < 4; ++i) PSXRecomp::GTE::s_precise_sxy[i].valid = 0;
            break;
        case 23:
            cpu->gte_data[23] = 0;
            break;
        case 28: {
            cpu->gte_data[9] = (val & 0x1Fu) << 7;
            cpu->gte_data[10] = ((val >> 5) & 0x1Fu) << 7;
            cpu->gte_data[11] = ((val >> 10) & 0x1Fu) << 7;
            const uint32_t packed = val & 0x7FFFu;
            cpu->gte_data[28] = packed;
            cpu->gte_data[29] = packed;
            break;
        }
        case 29: {
            const uint32_t packed = gte_cpu_pack_irgb(cpu);
            cpu->gte_data[28] = packed;
            cpu->gte_data[29] = packed;
            break;
        }
        case 30:
            cpu->gte_data[30] = val;
            cpu->gte_data[31] = gte_cpu_lzcr(val);
            break;
        case 31:
            cpu->gte_data[31] = gte_cpu_lzcr(cpu->gte_data[30]);
            break;
        default:
            cpu->gte_data[reg] = val;
            break;
    }
}

extern "C" void gte_write_ctrl(CPUState* cpu, uint8_t reg, uint32_t val) {
    if (reg >= 32) return;
    gte_cpu_canonicalize_backing(cpu);
    switch (reg) {
        case 4: case 12: case 20: case 26:
            cpu->gte_ctrl[reg] = val & 0xFFFFu;
            break;
        case 27: case 29: case 30:
            cpu->gte_ctrl[reg] = gte_sign_extend_16(val);
            break;
        case 31:
            cpu->gte_ctrl[31] = val & 0x7FFFF000u;
            break;
        default:
            cpu->gte_ctrl[reg] = val;
            break;
    }
}

extern "C" void gte_canonicalize_cpu_state(CPUState* cpu) {
    gte_cpu_canonicalize_backing(cpu);
    /* ctrl[31] is emulator state, not a guest CTC2 write. Preserve the
     * command-computed summary bit 31 exactly. */
    /* Raw restore replaced RAM and SXY0..2. Drop every host-only provenance
     * cache from the previous timeline, including address-keyed packet depth
     * and geometry correction entries. */
    gte_precision_timeline_invalidate();
}

#ifdef PSX_GTE_REGISTER_TEST
extern "C" void gte_test_set_precise_valid_mask(uint32_t mask) {
    for (uint32_t i = 0; i < 4; ++i)
        PSXRecomp::GTE::s_precise_sxy[i].valid = (mask >> i) & 1u;
}

extern "C" uint32_t gte_test_get_precise_valid_mask(void) {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < 4; ++i)
        mask |= (PSXRecomp::GTE::s_precise_sxy[i].valid ? 1u : 0u) << i;
    return mask;
}

extern "C" void gte_test_set_timeline_generations(uint32_t precision,
                                                    uint32_t geometry) {
    PSXRecomp::GTE::s_precision_generation = precision;
    PSXRecomp::GTE::s_geom_generation = geometry;
}

extern "C" uint32_t gte_test_get_precision_generation(void) {
    return PSXRecomp::GTE::s_precision_generation;
}

extern "C" uint32_t gte_test_get_geometry_generation(void) {
    return PSXRecomp::GTE::s_geom_generation;
}

extern "C" void gte_test_seed_precise_projection(uint32_t index,
                                                    uint32_t packed,
                                                    int32_t x16,
                                                    int32_t y16,
                                                    uint16_t z) {
    if (index >= 4) return;
    auto &p = PSXRecomp::GTE::s_precise_sxy[index];
    p.packed = packed;
    p.x16 = x16;
    p.y16 = y16;
    p.z = z;
    p.valid = 1;
}

extern "C" void gte_test_get_precise_projection(uint32_t index,
                                                  uint32_t* packed,
                                                  int32_t* x16,
                                                  int32_t* y16,
                                                  uint16_t* z,
                                                  uint8_t* valid) {
    if (index >= 4) return;
    const auto &p = PSXRecomp::GTE::s_precise_sxy[index];
    if (packed) *packed = p.packed;
    if (x16) *x16 = p.x16;
    if (y16) *y16 = p.y16;
    if (z) *z = p.z;
    if (valid) *valid = p.valid;
}

extern "C" void gte_test_seed_geometry(uint32_t packed, int32_t x16,
                                         int32_t y16) {
    auto &entry = PSXRecomp::GTE::s_geom_cache[
        PSXRecomp::GTE::geom_hash(packed)];
    entry.packed = packed;
    entry.x16 = x16;
    entry.y16 = y16;
    entry.generation = PSXRecomp::GTE::s_geom_generation;
}
#endif
