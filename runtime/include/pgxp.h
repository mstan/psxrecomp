#ifndef PGXP_H
#define PGXP_H

/* PGXP value-propagation engine — runtime-internal API (pgxp.cpp).
 *
 * The engine shadows every 32-bit word of guest RAM/scratchpad, every GPR,
 * and every GTE data register with the sub-pixel projection it carries
 * (screen X/Y in 16.16 plus the projected SZ depth). The GTE fills shadows at
 * RTPS/RTPT; the psx_pgxp_* hooks (pgxp_hooks.h) move them along with the
 * data; the GPU asks for the precise position of each GP0 vertex word by the
 * packet's RAM address, validated against the actual word — never guessed
 * from the rounded position (the measured G1.4 dead end).
 *
 * Guest-visible state is NEVER touched: shadows are host-only, dropped on
 * savestate/rewind, and suppressed during speculative validation passes.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- lifecycle / configuration ------------------------------------------ */

/* Master arm. Allocates the RAM shadow lazily; fails closed (stays disabled)
 * if the allocation fails. Idempotent. */
void pgxp_set_enabled(int enabled);
int  pgxp_enabled(void);

/* Tier-2 propagation through CPU arithmetic (default off, like the reference
 * implementations' default). Off is SAFE — value validation already stops
 * stale shadows — it only bounds coverage. Live-tunable (TCP). */
void pgxp_set_cpu_mode(int enabled);
int  pgxp_cpu_mode(void);

/* ALU/MULDIV hook bodies run only when tier-2 cpu_mode OR this flag is set
 * (texture/geometry correction dataflow chains). NCLIP-only arms leave it 0:
 * the SXY chain is LOAD/STORE/COP2. Stale shadows from the gated period are
 * dropped by pv_validate at every consumer (the single safety invariant). */
void pgxp_set_full_hooks(int on);

/* Reject a precise position whose sub-pixel offset from the native integer
 * position exceeds this many pixels. < 0 disables the clamp. */
void  pgxp_set_tolerance(float pixels);
float pgxp_tolerance(void);

/* Drop all shadows (savestate load, raw RAM restore, timeline breaks).
 * O(1) via generation bump. Deferred while suppressed. */
void pgxp_invalidate_all(void);

/* A guest RAM/scratchpad word was written outside the shadow hook. This is
 * required even for byte-identical writes: equality proves the integer bits,
 * not that the old sub-pixel projection produced the new value. */
void pgxp_memory_write(uint32_t addr, uint32_t size);
extern int g_pgxp_memory_armed;

/* Fast raw-store gate. Projection shadows can occupy at most 8 MiB of guest
 * RAM plus the 1 KiB scratchpad. A page stamp matches only while that page has
 * at least one live fractional component in the current shadow generation, so
 * unrelated stores avoid the out-of-line invalidation call. */
#define PGXP_MEMORY_LIVE_PAGE_SHIFT 12u
#define PGXP_MEMORY_LIVE_RAM_PAGES  2048u
#define PGXP_MEMORY_LIVE_SCRATCH_PAGE PGXP_MEMORY_LIVE_RAM_PAGES
#define PGXP_MEMORY_LIVE_PAGE_COUNT (PGXP_MEMORY_LIVE_RAM_PAGES + 1u)
extern uint32_t g_pgxp_memory_live_generation;
extern uint32_t
    g_pgxp_memory_live_page_generation[PGXP_MEMORY_LIVE_PAGE_COUNT];
static inline int pgxp_memory_page_armed(uint32_t phys) {
    uint32_t page;
    if (phys < 0x00800000u) {
        page = phys >> PGXP_MEMORY_LIVE_PAGE_SHIFT;
    } else if (phys >= 0x1F800000u && phys <= 0x1F8003FFu) {
        page = PGXP_MEMORY_LIVE_SCRATCH_PAGE;
    } else {
        return 0;
    }
    return g_pgxp_memory_live_page_generation[page] ==
           g_pgxp_memory_live_generation;
}

/* Counted suppression bracket for speculative native-validation passes and
 * the GTE replay sandbox: hooks and producers no-op inside it. */
void pgxp_suppress_begin(void);
void pgxp_suppress_end(void);

/* --- GTE producer (gte.cpp) ---------------------------------------------- */

/* Called at the RTPS/RTPT projection with the pre-truncation 16.16 screen
 * coordinates, the projected depth (SZ3), and the packed SXY word the guest
 * sees. Shifts the shadow FIFO exactly like push_sxy (regs 12..15). */
void pgxp_gte_push_sxy(int32_t x16, int32_t y16, uint16_t sz3, uint32_t packed);

/* Current SXY FIFO shadow (index 0..3 selects GTE data regs 12..15).
 * Returns nonzero when the shadow is live and carries X/Y precision. */
int pgxp_get_gte_sxy(uint32_t index, int32_t *x16, int32_t *y16);
/* Validate-on-read variant: check!=0 additionally requires the shadow's packed
 * word to equal expect (the live guest SXY word). Sign consumers must use this
 * form so stale FIFO shadows fail closed to native integer behavior. */
int pgxp_get_gte_sxy_checked(uint32_t index, uint32_t expect, int check,
                             int32_t *x16, int32_t *y16);

/* Guest write to a GTE data register outside gte_execute (MTC2/CTC2 handled
 * by psx_pgxp_cop2; this is for direct gte_write_data paths): reg 15 performs
 * the SXYP FIFO push on the shadows, others just invalidate/overwrite. */
void pgxp_gte_reg_written(int reg, uint32_t value);

/* --- GPU consumer (gpu.c) ------------------------------------------------ */

enum {
    PGXP_SRC_NATIVE   = 0,   /* no shadow — caller uses the parsed integers  */
    PGXP_SRC_FALLBACK = 1,   /* position-cache fallback (never carries depth)*/
    PGXP_SRC_DATAFLOW = 2,   /* address-keyed shadow, value-validated        */
};

/* Precise position for one GP0 vertex word.
 *   addr        guest address the word was DMA'd from, or 0xFFFFFFFF if
 *               unknown (immediate GP0 writes) — skips the dataflow tier.
 *   packet_word the actual word (validation key).
 *   int_x/int_y the natively parsed 11-bit positions (pre-draw-offset).
 * On DATAFLOW/FALLBACK, x16 and y16 hold the sub-pixel position (16.16, same
 * coordinate space as the packet halves); *sz is the projected depth or 0.
 * Safeguards applied here: the integer part must match the native parse
 * (truncation agreement) and the tolerance clamp. */
int pgxp_get_precise_vertex(uint32_t addr, uint32_t packet_word,
                            int32_t int_x, int32_t int_y,
                            int32_t *x16, int32_t *y16, uint16_t *sz);

/* --- observability -------------------------------------------------------- */

typedef struct PGXPStats {
    uint64_t lookups;            /* pgxp_get_precise_vertex calls            */
    uint64_t dataflow_hit;
    uint64_t fallback_hit;
    uint64_t native;
    uint64_t value_mismatch;     /* shadow present but wrong word (stale)    */
    uint64_t trunc_reject;       /* integer part disagreed with native parse */
    uint64_t tolerance_reject;
    uint64_t w_valid;            /* lookups that also carried a usable depth */
    uint64_t produced;           /* RTPS/RTPT projections pushed into shadows */
    uint64_t swc2_stores;        /* GTE reg shadows copied to RAM shadows     */
    uint64_t invalidations;      /* generation bumps (each kills all shadows) */
    uint64_t memory_invalidations; /* raw word writes that killed provenance  */
    uint64_t mixed_depth_reject; /* complete XY rejected: projection IDs differ */
} PGXPStats;

void pgxp_get_stats(PGXPStats *out);

/* --- gte.cpp forwarding surface (v14 ABI compat) -------------------------- */

/* SWC2 site: copy the GTE register shadow (regs 12..15) to the RAM shadow. */
void pgxp_store_gte_reg(uint32_t addr, uint8_t reg);

/* Address-keyed precise-word lookup (perspective texturing path). Returns
 * nonzero when the tracked word matches `packed` AND carries a depth. */
enum {
    PGXP_SHADOW_NONE = 0,
    PGXP_SHADOW_STALE = 1,
    PGXP_SHADOW_VALUE_MISMATCH = 2,
    PGXP_SHADOW_POSITION_INCOMPLETE = 3,
    PGXP_SHADOW_DEPTH_DEAD = 4,
    PGXP_SHADOW_DEPTH_ZERO = 5,
    PGXP_SHADOW_ENGINE_OFF = 6,
    PGXP_SHADOW_DEPTH_READY = 7,
    PGXP_SHADOW_MIXED_PROJECTION = 8,
};
int pgxp_debug_shadow_class(uint32_t addr, uint32_t packed);
int pgxp_load_precise_word(uint32_t addr, uint32_t packed,
                           int32_t *x16, int32_t *y16, uint16_t *z);

/* --- test accessors (always compiled; trivial) ---------------------------- */

/* index 0..3 selects the SXY0..SXYP register shadow (GTE data regs 12..15). */
void pgxp_test_seed_gte_sxy(uint32_t index, uint32_t packed,
                            int32_t x16, int32_t y16, uint16_t z, int valid);
void pgxp_test_get_gte_sxy(uint32_t index, uint32_t *packed,
                           int32_t *x16, int32_t *y16, uint16_t *z,
                           uint8_t *valid);
uint32_t pgxp_test_generation(void);
uint32_t pgxp_test_suppress_depth(void);
int      pgxp_test_active(void);
void     pgxp_test_set_generation(uint32_t gen);

#ifdef __cplusplus
}
#endif

#endif /* PGXP_H */
