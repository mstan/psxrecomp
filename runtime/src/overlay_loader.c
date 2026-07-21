#include "overlay_loader.h"
#include "overlay_api.h"
#include "code_provider.h"
#include "overlay_backend.h"
#include "crc32.h"
#include "dirty_ram_interp.h"
#include "interrupts.h"
#include "debug_server.h"
#include "psx_cycles.h"
#include "overlay_posix.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <unistd.h>
#endif

#ifdef _WIN32
#  define OVERLAY_SHARED_EXT ".dll"
#  define OVERLAY_SHARED_EXT_LEN 4
#else
#  define OVERLAY_SHARED_EXT ".so"
#  define OVERLAY_SHARED_EXT_LEN 3
#endif

/* ============================================================================
 * Inc3 — Per-entry validity + multi-candidate dispatch (design doc §8)
 *
 * Validity is tracked per *compiled entry*, not per dirty region. Each entry
 * carries its tight code byte-ranges (from the recompiler's per-function walk,
 * emitted as a {base}_{crc}.ranges manifest beside the DLL), a content hash of
 * those ranges, and a page-generation snapshot. A write to a watched page bumps
 * that page's generation (memory.c); dispatch lazily re-hashes an entry only
 * when the generation over its ranges changed. This:
 *   - kills the false-invalidation churn of the old [fn_lo,fn_hi) region flag
 *     (data interleaved between functions no longer nukes the whole region), and
 *   - makes reload-on-return gradual and automatic — each entry flips back to
 *     native the moment its own code bytes reappear (hash matches), with no
 *     region-wide atomic threshold.
 *
 * PC -> candidate list: different overlays can reuse the same RAM address
 * (Tomba loads village and overworld both at 0x800E7xxx). A single-entry table
 * would let the later DLL clobber the earlier candidate, making reload-on-return
 * impossible. So each PC maps to a chain of candidates; dispatch picks the one
 * whose code hash matches live RAM.
 *
 * §8.3 RESOLVED: jump tables compile to `switch (live register)` with a
 * call_by_address default, so table contents are an optimization, not a
 * correctness dependency. The dependency set is the entry's code bytes only.
 * ==========================================================================*/

typedef void (*OverlayFn)(CPUState *);
typedef void (*OverlayFlushFn)(void);

#define MAX_CODE_RANGES 16   /* code ranges per function (coalesced; usually 1) */

enum { ENTRY_VALID = 0, ENTRY_INVALID = 1, ENTRY_BLACKLIST = 2 };

typedef struct {
    uint32_t  addr;                      /* phys entry address                 */
    OverlayFn fn;
    uint32_t  range_lo[MAX_CODE_RANGES]; /* phys code-range starts             */
    uint32_t  range_len[MAX_CODE_RANGES];
    int       nranges;
    uint32_t  crc_code;                  /* hash of code ranges at registration*/
    uint32_t  val_gen;                   /* pagegen sum when last (in)validated*/
    int       state;                     /* ENTRY_VALID/INVALID/BLACKLIST      */
    int       dll;                       /* source DLL index                   */
    int       next;                      /* next candidate at same addr, -1 end*/
    uint32_t  diff_passes;               /* clean same-state diffs (verify budget)*/
    int       device_touch;              /* 1 = touches MMIO; never run its shard,
                                          * always interp (shadow diff can't safely
                                          * double-execute device I/O). */
} Candidate;

/* Same-state differential verify budget: diff a candidate this many times with
 * 0 divergence, then trust it (stop diffing — run it normally). Bounds the
 * differential's cost to ~(distinct functions × budget) instead of every
 * dispatch forever, making a validation playtest playable; a DIVERGING
 * candidate never reaches the budget, so it stays diff-gated (interp result
 * kept) and never executes native live. */
#define OVERLAY_DIFF_BUDGET 32u

#define CAND_CAP   16384
static Candidate s_cand[CAND_CAP];
static int       s_cand_n = 0;

/* CPS continuation lookup. A full candidate-table scan at every tail transfer
 * scales catastrophically once a warmed cache contains hundreds of variant
 * DLLs. Index candidates by the 4 KiB RAM pages touched by their code ranges;
 * a continuation then examines only candidates that could contain its PC. */
#define RANGE_PAGE_COUNT (2u * 1024u * 1024u / 4096u)
#define RANGE_LINK_CAP   (CAND_CAP * 8)
typedef struct { int cand, next; } RangeLink;
static int       s_range_page_head[RANGE_PAGE_COUNT];
static int       s_range_page_tail[RANGE_PAGE_COUNT];
static RangeLink s_range_links[RANGE_LINK_CAP];
static int       s_range_link_n = 0;
static int       s_range_index_overflow = 0;

/* Cache the selected owner of hot CPS continuation PCs. The cached candidate
 * is still generation/CRC validated on every lookup; this only avoids walking
 * hundreds of historical, currently-invalid owners in a reused region. */
#define RANGE_PC_CACHE_CAP 16384u
#define RANGE_PC_CACHE_MASK (RANGE_PC_CACHE_CAP - 1u)
typedef struct { uint32_t phys; int cand; } RangePcCache;
static RangePcCache s_range_pc_cache[RANGE_PC_CACHE_CAP];

static void range_index_add_candidate(int ci) {
    Candidate *c = &s_cand[ci];
    uint8_t seen[RANGE_PAGE_COUNT / 8] = {0};
    for (int r = 0; r < c->nranges; r++) {
        uint32_t lo = c->range_lo[r];
        uint32_t hi = lo + c->range_len[r] - 1u;
        uint32_t p0 = lo >> 12, p1 = hi >> 12;
        if (p0 >= RANGE_PAGE_COUNT) continue;
        if (p1 >= RANGE_PAGE_COUNT) p1 = RANGE_PAGE_COUNT - 1u;
        for (uint32_t p = p0; p <= p1; p++) {
            uint8_t bit = (uint8_t)(1u << (p & 7u));
            if (seen[p >> 3] & bit) continue;
            seen[p >> 3] |= bit;
            if (s_range_link_n >= RANGE_LINK_CAP) {
                /* Never leave lookup correctness dependent on a partial index.
                 * overlay_find_by_range() falls back to the original ordered
                 * candidate scan if a future title exceeds this sizing guess. */
                s_range_index_overflow = 1;
                return;
            }
            int li = s_range_link_n++;
            s_range_links[li].cand = ci;
            s_range_links[li].next = -1;
            if (s_range_page_head[p] < 0) s_range_page_head[p] = li;
            else s_range_links[s_range_page_tail[p]].next = li;
            s_range_page_tail[p] = li;
        }
    }
}

/* RECURSION_BUG.md §25 — 1 when the build is continuation-passing (set by a
 * constructor in the generated CPS dispatch). The overlay dispatch reads it to
 * emit/route the CPS tail-transfer contract. Defined here (before
 * overlay_loader_dispatch) so both can see it. 0 = legacy unit-model. */
int g_psx_cps_mode = 0;

/* Open-addressed index: phys entry addr -> head candidate index (-1 sentinel
 * stored as chain terminator on each Candidate). addr 0 = empty slot. */
#define IDX_CAP  32768u
#define IDX_MASK (IDX_CAP - 1u)
typedef struct { uint32_t addr; int head; } IdxSlot;
static IdxSlot s_idx[IDX_CAP];

static int idx_head(uint32_t phys) {
    uint32_t h = (phys * 2654435761u) & IDX_MASK;
    for (uint32_t i = 0; i < IDX_CAP; i++) {
        uint32_t k = (h + i) & IDX_MASK;
        if (s_idx[k].addr == 0)    return -1;
        if (s_idx[k].addr == phys) return s_idx[k].head;
    }
    return -1;
}
static void idx_set_head(uint32_t phys, int head) {
    uint32_t h = (phys * 2654435761u) & IDX_MASK;
    for (uint32_t i = 0; i < IDX_CAP; i++) {
        uint32_t k = (h + i) & IDX_MASK;
        if (s_idx[k].addr == 0 || s_idx[k].addr == phys) {
            s_idx[k].addr = phys;
            s_idx[k].head = head;
            return;
        }
    }
}

/* Active native-entry stack (self-modification detection, §8.5). */
static int s_active_stack[64];
static int s_active_depth = 0;

/* ---- Observability (recomp-debug: measure, don't eyeball) -------------- */
/* Always-on ring of native overlay calls, so the FIRST divergence / last
 * function executed before a corruption can be read back from the window of
 * interest — never "arm a trace then hope". s_native_inprogress holds the entry
 * currently executing (nonzero at dump => a native fn was entered and never
 * returned: a freeze INSIDE native code, the strongest single suspect). */
#define NRING_CAP 16384
typedef struct { uint32_t addr; uint32_t crc; uint32_t frame; uint64_t seq; int returned; } NRingEnt;
static NRingEnt s_nring[NRING_CAP];
static uint32_t s_nring_pos = 0;
static uint64_t s_nring_seq = 0;
static uint32_t s_native_inprogress = 0;   /* addr of fn currently in native, 0 = none */
/* Sampler accessor (phase_profile hot-function histogram): the innermost native
 * candidate's registered entry. Read off-thread; a torn read is harmless. */
uint32_t overlay_loader_native_inprogress(void) { return s_native_inprogress; }
static uint64_t s_native_calls_total = 0;
extern uint64_t s_frame_count;

/* Runtime A/B: when 0, candidates are still hashed/validated and recorded, but
 * NEVER executed native — execution falls to the interpreter. Lets us prove
 * whether native EXECUTION is the cause without a rebuild or losing candidate
 * visibility. Default on. */
static int s_native_exec = 1;
static uint64_t s_would_run_native = 0;   /* matched but skipped (exec off)   */

void overlay_loader_set_native_exec(int on) { s_native_exec = on ? 1 : 0; }
int  overlay_loader_get_native_exec(void)   { return s_native_exec; }
/* Fail-closed native entry guard (root-cause fix for the Tomba/Tomba2 native-
 * overlay "blue screen" wedge). A native overlay function's generated CPS entry-
 * switch calls psx_native_bad_entry when it is entered at a PC that is NOT one of
 * its legal entries (true prologue or a known continuation) -- e.g. when range
 * ownership wrongly resolved a foreign interior PC to this function. Instead of
 * the old `default: break` (which fell through and ran the function from its
 * TOP, corrupting shared CPU/RAM state -- the wedge), the function records the
 * illegal entry and returns WITHOUT executing; overlay_loader_dispatch then
 * routes the requested PC to the sanctioned dirty-RAM interpreter (the bytes at
 * that PC run correctly). NOT a stub/HLE -- the code still runs, via interp. */
int      g_native_bad_entry = 0;       /* set by psx_native_bad_entry, consumed by dispatch */
static uint32_t s_bad_entry_owner = 0; /* the function unit that rejected the PC */
static uint32_t s_bad_entry_pc = 0;    /* the illegal entry PC */
static uint64_t s_bad_entry_count = 0;
void psx_native_bad_entry(CPUState *cpu, uint32_t owner, uint32_t pc) {
    (void)cpu;
    g_native_bad_entry = 1;
    s_bad_entry_owner = owner;
    s_bad_entry_pc = pc;
    s_bad_entry_count++;
}
void overlay_loader_bad_entry_stats(uint32_t *owner, uint32_t *pc, uint64_t *count) {
    if (owner) *owner = s_bad_entry_owner;
    if (pc)    *pc    = s_bad_entry_pc;
    if (count) *count = s_bad_entry_count;
}

/* Per-function native-disable (bisection). Functions whose ENTRY phys address is
 * listed here are forced through the sanctioned dirty-RAM interpreter instead of
 * their native shard — exactly as if s_native_exec were off, but for that one
 * function only. This is a DIAGNOSTIC localization knob (not skip/stub/HLE): the
 * function still runs, just via the interpreter. Used to binary-search which
 * compiled overlay function's native execution causes a native-vs-interp
 * divergence. Keyed by phys (addr & 0x1FFFFFFF) so KSEG bits don't matter. */
#define NATIVE_BLOCK_CAP 64
static uint32_t s_native_block[NATIVE_BLOCK_CAP];
static int      s_native_block_n = 0;
static uint64_t s_native_block_hits = 0;
static int overlay_native_blocked(uint32_t addr) {
    if (s_native_block_n == 0) return 0;
    uint32_t p = addr & 0x1FFFFFFFu;
    for (int i = 0; i < s_native_block_n; i++)
        if (s_native_block[i] == p) { s_native_block_hits++; return 1; }
    return 0;
}
int overlay_loader_native_block_add(uint32_t addr) {
    uint32_t p = addr & 0x1FFFFFFFu;
    for (int i = 0; i < s_native_block_n; i++) if (s_native_block[i] == p) return s_native_block_n;
    if (s_native_block_n >= NATIVE_BLOCK_CAP) return -1;
    s_native_block[s_native_block_n++] = p;
    return s_native_block_n;
}
void overlay_loader_native_block_clear(void) { s_native_block_n = 0; s_native_block_hits = 0; }
int  overlay_loader_native_block_list(uint32_t *out, int cap) {
    int n = s_native_block_n < cap ? s_native_block_n : cap;
    for (int i = 0; i < n; i++) out[i] = s_native_block[i];
    return s_native_block_n;
}
uint64_t overlay_loader_native_block_hits(void) { return s_native_block_hits; }

/* CPS interior-continuation dispatch probe (diagnostic, always-on when armed).
 * Records, for a watched interior PC, what the CPS continuation re-entry path
 * (overlay_find_by_range + validate) decides: chosen candidate entry, range
 * count, crc match, and outcome. Used to confirm the wrong-candidate / escape
 * hypothesis for the Tomba 2 FMV freeze (pc=0x50B30). */
static uint32_t s_cps_probe_pc = 0;        /* phys, 0 = disarmed */
static uint64_t s_cps_probe_count = 0;
static uint32_t s_cps_probe_found = 0;     /* chosen c->addr (0 if none) */
static int      s_cps_probe_ci = -2;       /* overlay_find_by_range result */
static int      s_cps_probe_nrange = -1;
static int      s_cps_probe_matched = -1;  /* crc match */
static int      s_cps_probe_outcome = -1;  /* 0=find<0,1=crc-miss->interp,2=native,3=device,4=blocked */
static int      s_cps_probe_ncand_inrange = 0; /* how many candidates' range contains the PC */
void overlay_loader_cps_probe_set(uint32_t pc) {
    s_cps_probe_pc = pc & 0x1FFFFFFFu; s_cps_probe_count = 0;
    s_cps_probe_found = 0; s_cps_probe_ci = -2; s_cps_probe_nrange = -1;
    s_cps_probe_matched = -1; s_cps_probe_outcome = -1; s_cps_probe_ncand_inrange = 0;
}
void overlay_loader_cps_probe_get(uint32_t *pc, uint64_t *cnt, uint32_t *found,
                                  int *ci, int *nrange, int *matched, int *outcome,
                                  int *ncand) {
    if (pc) *pc = s_cps_probe_pc; if (cnt) *cnt = s_cps_probe_count;
    if (found) *found = s_cps_probe_found; if (ci) *ci = s_cps_probe_ci;
    if (nrange) *nrange = s_cps_probe_nrange; if (matched) *matched = s_cps_probe_matched;
    if (outcome) *outcome = s_cps_probe_outcome; if (ncand) *ncand = s_cps_probe_ncand_inrange;
}
/* count how many live candidates have a range containing phys (multi-variant check) */
static int overlay_count_by_range(uint32_t phys) {
    int n = 0;
    for (int i = 0; i < s_cand_n; i++) {
        const Candidate *c = &s_cand[i];
        if (c->state == ENTRY_BLACKLIST) continue;
        for (int r = 0; r < c->nranges; r++)
            if (phys >= c->range_lo[r] && phys < c->range_lo[r] + c->range_len[r]) { n++; break; }
    }
    return n;
}
/* Address of the native overlay function currently executing (0 if none).
 * Used by the event-timeline ring to tag an event's execution mode. */
uint32_t overlay_loader_get_inprogress(void) { return s_native_inprogress; }

/* Same-state differential — defined fully below; declared here for dispatch. */
static int  s_diff_mode = 0;
static int  s_in_shadow = 0;
/* Candidate whose shadow NATIVE pass is currently executing (NULL outside it).
 * Set ONLY around run_shadow_diff's native pass — never during the interp pass,
 * which must stay pure interp. Lets the CPS continuation re-entry path below
 * run THIS candidate's own interiors natively inside its shadow run (so the
 * diff exercises the whole function, not just the first CPS segment), while
 * every other dispatch inside the shadow stays on the interpreter. */
static const void *s_shadow_cand = NULL;
/* Longjmp-escape hardening. run_shadow_diff saves the pre-shadow values of
 * s_native_exec / s_suppress_irq here (in addition to its locals) so that
 * overlay_loader_shadow_escape_fixup() can restore them if an exception
 * longjmp ever unwinds through a live shadow frame (should be impossible —
 * the dispatch gate refuses to start a shadow while in an exception dispatch —
 * but an escape must fail LOUD and self-heal, not silently kill the
 * instrument). s_shadow_epoch is the exception-setjmp epoch at shadow start:
 * a longjmp targeting a frame with epoch <= s_shadow_epoch escapes the shadow;
 * a larger epoch is an exception armed INSIDE the shadow (contained). */
static int      s_shadow_saved_native_exec = 0;
static int      s_shadow_saved_supp       = 0;
static uint64_t s_shadow_epoch            = 0;
static uint32_t s_shadow_escapes          = 0;
static uint32_t s_shadow_escapes_native   = 0;
/* (sljit removed 2026-07-15: the deprecated in-process JIT tier and its
 * live-execution mode are gone; overlay gaps fall to the interpreter.) */
static void run_shadow_diff(CPUState *cpu, Candidate *c, uint32_t addr);

/* ---- Counters (surfaced via overlay_loader_status) --------------------- */
static int      s_ndlls          = 0;   /* DLLs LoadLibrary'd                 */
static uint64_t s_load_total_us  = 0;
static uint64_t s_load_max_us    = 0;
static uint64_t s_load_last_us   = 0;
static int      s_valid_count    = 0;   /* candidates currently VALID         */
static uint64_t s_disp_native    = 0;
static uint64_t s_disp_interp    = 0;
static uint64_t s_stale_blocked  = 0;   /* dispatches skipped (candidate !valid)*/
static uint32_t s_invalidations  = 0;   /* VALID -> INVALID transitions       */
static uint32_t s_revalidations  = 0;   /* INVALID -> VALID (reload-on-return) */
static uint32_t s_rehashes       = 0;   /* code-range hashes computed         */
static uint32_t s_rehash_miss    = 0;   /* hashes that didn't match crc_code  */
static uint64_t s_gen_fastpath   = 0;   /* dispatches that skipped the crc32   */
                                        /* via the unchanged page-generation   */
                                        /* fast path (overlay-cache v2 P2)      */
static uint64_t s_diffgate_interp = 0;  /* CPS interior re-entries sent to the  */
                                        /* interp because their candidate is    */
                                        /* still inside the diff verify budget  */
static uint32_t s_last_crc       = 0;
static uint32_t s_no_manifest    = 0;   /* exports skipped (no manifest range)*/
static uint32_t s_selfmod        = 0;   /* entries blacklisted (self-mod)     */
static uint32_t s_last_write_pc   = 0;
static uint32_t s_last_write_addr = 0;
static uint32_t s_last_write_size = 0;

/* ABI v11 cycle batching. Each DLL owns a small pending-cycle accumulator.
 * While its code is active, memory stores ask this export to commit cycles
 * before the guest-visible write; block/device boundaries flush through the
 * DLL glue directly. */
static OverlayFlushFn s_dll_flush[4096];
OverlayFlushFn g_overlay_flush_pending_cycles = NULL;

static OverlayFlushFn overlay_flush_enter(const Candidate *c) {
    OverlayFlushFn prev = g_overlay_flush_pending_cycles;
    if (prev) prev();
    g_overlay_flush_pending_cycles =
        (c->dll >= 0 && c->dll < (int)(sizeof(s_dll_flush) / sizeof(s_dll_flush[0])))
        ? s_dll_flush[c->dll] : NULL;
    return prev;
}

static void overlay_flush_leave(OverlayFlushFn prev) {
    if (g_overlay_flush_pending_cycles) g_overlay_flush_pending_cycles();
    g_overlay_flush_pending_cycles = prev;
}

extern uint8_t *memory_get_ram_ptr(void);
extern uint32_t overlay_watch_pagegen_sum(uint32_t phys, uint32_t len);

/* ---- Per-candidate hash / generation over its code ranges -------------- */

static uint32_t cand_crc(const Candidate *c) {
    const uint8_t *ram = memory_get_ram_ptr();
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < c->nranges; i++)
        crc = crc32_update(crc, ram + c->range_lo[i], c->range_len[i]);
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t cand_gensum(const Candidate *c) {
    uint32_t s = 0;
    for (int i = 0; i < c->nranges; i++)
        s += overlay_watch_pagegen_sum(c->range_lo[i], c->range_len[i]);
    return s;
}

#ifdef PSX_HAS_OVERLAY_DISPATCH
/* Static-overlay validation uses the same exact-code-range contract as the
 * dynamic DLL loader. Generated code passes immutable {phys_lo, len} pairs and
 * the CRC of the bytes it was compiled from. A page-generation cache keeps the
 * hot path O(number of ranges), without re-hashing unchanged code each call. */
#define STATIC_MATCH_CACHE_CAP 4096u
typedef struct {
    const uint32_t *ranges;
    uint32_t count;
    uint32_t expected_crc;
    uint32_t gen_sum;
    int      matches;
} StaticMatchCache;

static StaticMatchCache s_static_match_cache[STATIC_MATCH_CACHE_CAP];
static uint64_t s_static_match_rehashes = 0;
static uint64_t s_static_match_crc_misses = 0;
static uint64_t s_static_match_gen_fastpath = 0;

int psx_overlay_static_code_matches(const uint32_t *lo_len_pairs,
                                    uint32_t count,
                                    uint32_t expected_crc) {
    const uint8_t *ram = memory_get_ram_ptr();
    if (!ram || !lo_len_pairs || count == 0u || count > 4096u) {
        s_static_match_crc_misses++;
        return 0;
    }

    uint32_t gen_sum = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t lo = lo_len_pairs[i * 2u] & 0x1FFFFFFFu;
        uint32_t len = lo_len_pairs[i * 2u + 1u];
        if (len == 0u || lo >= 2u * 1024u * 1024u ||
            len > 2u * 1024u * 1024u - lo) {
            s_static_match_crc_misses++;
            return 0;
        }
        gen_sum += overlay_watch_pagegen_sum(lo, len);
    }

    uintptr_t raw = (uintptr_t)lo_len_pairs;
    uint32_t slot = (uint32_t)(((raw >> 4) ^ (raw >> 19) ^ expected_crc ^
                                (count * 0x9E3779B9u)) &
                               (STATIC_MATCH_CACHE_CAP - 1u));
    StaticMatchCache *entry = NULL;
    for (uint32_t probe = 0; probe < STATIC_MATCH_CACHE_CAP; probe++) {
        StaticMatchCache *candidate =
            &s_static_match_cache[(slot + probe) & (STATIC_MATCH_CACHE_CAP - 1u)];
        if (!candidate->ranges ||
            (candidate->ranges == lo_len_pairs &&
             candidate->count == count &&
             candidate->expected_crc == expected_crc)) {
            entry = candidate;
            break;
        }
    }

    if (entry && entry->ranges && entry->gen_sum == gen_sum) {
        s_static_match_gen_fastpath++;
        return entry->matches;
    }

    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t lo = lo_len_pairs[i * 2u] & 0x1FFFFFFFu;
        uint32_t len = lo_len_pairs[i * 2u + 1u];
        crc = crc32_update(crc, ram + lo, len);
    }
    crc ^= 0xFFFFFFFFu;
    int matches = (crc == expected_crc) ? 1 : 0;
    s_static_match_rehashes++;
    if (!matches) s_static_match_crc_misses++;

    if (entry) {
        entry->ranges = lo_len_pairs;
        entry->count = count;
        entry->expected_crc = expected_crc;
        entry->gen_sum = gen_sum;
        entry->matches = matches;
    }
    return matches;
}

void overlay_loader_static_match_stats(uint64_t *rehashes,
                                       uint64_t *crc_misses,
                                       uint64_t *gen_fastpath) {
    if (rehashes) *rehashes = s_static_match_rehashes;
    if (crc_misses) *crc_misses = s_static_match_crc_misses;
    if (gen_fastpath) *gen_fastpath = s_static_match_gen_fastpath;
}
#endif

/* ---- Per-DLL code-range manifest --------------------------------------- */
/* Minimal line format emitted by tools/compile_overlays.py beside each DLL:
 *   F <entry_hex>            start a function (entry = virtual export addr)
 *   R <lo_hex> <len_hex>     a code byte-range (virtual addr, byte length)
 * The recompiler's per-function instruction walk yields exactly the executed
 * code bytes — interleaved jump tables / constant pools are excluded, which is
 * what makes the hash stable across reloads. */
typedef struct {
    uint32_t entry;
    uint32_t crc;       /* expected hash of the compiled-from code bytes       */
    int      has_crc;   /* 1 if the manifest supplied the authoritative hash   */
    uint32_t lo[MAX_CODE_RANGES];
    uint32_t len[MAX_CODE_RANGES];
    int      n;
} ManFn;

static ManFn *parse_manifest(const char *path, int *out_n) {
    *out_n = 0;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    int cap = 1024, n = 0;
    ManFn *arr = (ManFn *)malloc(sizeof(ManFn) * cap);
    if (!arr) { fclose(f); return NULL; }
    char line[160];
    ManFn *cur = NULL;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == 'F') {
            uint32_t e = 0, crc = 0;
            int got = sscanf(line + 1, "%x %x", &e, &crc);
            if (got >= 1) {
                if (n >= cap) {
                    cap *= 2;
                    ManFn *na = (ManFn *)realloc(arr, sizeof(ManFn) * cap);
                    if (!na) break;
                    arr = na;
                }
                cur = &arr[n++];
                cur->entry   = e;
                cur->crc     = crc;
                cur->has_crc = (got >= 2) ? 1 : 0;
                cur->n = 0;
            }
        } else if (line[0] == 'R' && cur) {
            uint32_t lo, len;
            if (sscanf(line + 1, "%x %x", &lo, &len) == 2 &&
                cur->n < MAX_CODE_RANGES && len > 0) {
                cur->lo[cur->n]  = lo;
                cur->len[cur->n] = len;
                cur->n++;
            }
        }
    }
    fclose(f);
    *out_n = n;
    return arr;
}

static ManFn *man_find(ManFn *arr, int n, uint32_t entry) {
    for (int i = 0; i < n; i++)
        if (arr[i].entry == entry) return &arr[i];
    return NULL;
}

/* ---- Candidate registration -------------------------------------------- */

static void loader_log(const char *fmt, ...);   /* defined below */
static void cand_register(uint32_t phys, OverlayFn fn, const ManFn *m, int dll) {
    if (s_cand_n >= CAND_CAP) return;
    int idx = s_cand_n++;
    Candidate *c = &s_cand[idx];
    c->addr    = phys;
    c->fn      = fn;
    c->dll     = dll;
    c->state   = ENTRY_VALID;
    c->nranges = 0;
    for (int i = 0; i < m->n && c->nranges < MAX_CODE_RANGES; i++) {
        c->range_lo[c->nranges]  = m->lo[i] & 0x1FFFFFFFu;
        c->range_len[c->nranges] = m->len[i];
        c->nranges++;
    }
    /* Watch the code pages so future writes bump their generation. */
    extern void overlay_watch_set_range(uint32_t phys, uint32_t len);
    for (int i = 0; i < c->nranges; i++)
        overlay_watch_set_range(c->range_lo[i], c->range_len[i]);

    /* crc_code is the AUTHORITATIVE hash of the bytes the recompiler compiled
     * from (supplied by the manifest) — NOT a sample of live RAM at this instant
     * (registration is a single transient moment; the overlay may not be fully
     * present yet, and other overlays sharing the merged DLL are not present at
     * all). A candidate is callable iff live RAM matches this compiled-from hash,
     * which makes validity timing-independent and reload-on-return work. */
    if (m->has_crc) {
        c->crc_code = m->crc;
    } else {
        c->crc_code = cand_crc(c);   /* legacy manifest without hashes */
    }
    c->val_gen = cand_gensum(c);
    c->state   = (cand_crc(c) == c->crc_code) ? ENTRY_VALID : ENTRY_INVALID;
    c->next    = idx_head(phys);
    idx_set_head(phys, idx);
    range_index_add_candidate(idx);
    if (c->state == ENTRY_VALID) s_valid_count++;
    /* (sljit removed 2026-07-15: the gcc-DLL-obsoletes-older-sljit-shard block
     * lived here. With no sljit shards ever registered (dll == -1), there is
     * nothing to obsolete; the normal content-keyed chain dispatch stands.) */
}

/* (sljit removed 2026-07-15: register_sljit_candidate, the JIT-on-miss memo,
 * the async compile-worker glue, the master tier gate, and try_sljit_region
 * lived here. Overlay misses now fall to the interpreter.) */

/* ---- Global state ------------------------------------------------------ */

static char s_cache_dir[512];
static char s_game_id[64];
static int  s_active = 0;

/* Rule 3: no stderr logging. Most recent loader event recorded here and
 * surfaced through overlay_loader_status. */
static char s_last_msg[256] = {0};

static void loader_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_msg, sizeof(s_last_msg), fmt, ap);
    va_end(ap);
}

const char *overlay_loader_last_msg(void) { return s_last_msg; }

/* ---- Cache index: region_start -> dll path ----------------------------- */

/* 256 -> 4096 (2026-07-03): Tomba2's cache crossed 256 DLLs and the index
 * TRUNCATED SILENTLY — every entry past the cap was invisible to the loader
 * (region ran interpreted forever, no diagnostic). Found by the ABI-sweep
 * negative test. scan_one_cache_dir now shouts if even 4096 is hit. */
#define CACHE_IDX_CAP 4096
typedef struct {
    uint32_t region_start;
    uint64_t mtime;
    int func_count;
    int indexed_func_count;
    char path[768];
} CacheEntry;
static CacheEntry s_cache_idx[CACHE_IDX_CAP];
static int        s_cache_idx_count = 0;

#ifdef _WIN32
/* Image-only, bounded mapping for small recovery/fragment DLLs. Windows can
 * spend ~100 ms in its loader/security path on the first LoadLibrary, enough to
 * drain audio even though guest execution is otherwise at full speed. Map the
 * dynamic-text-base fragments on a worker before streamed code first uses them.
 * The emulation thread still performs overlay_init, export registration, and
 * live-byte validation.
 *
 * This deliberately is NOT a boot-time preload: Tomba's mature vault contains
 * hundreds of historical fragments and retaining all of them consumed ~1.8 GB.
 * Only one cache region's small fragments are mapped. */
typedef struct {
    volatile LONG state; /* 0 idle, 1 mapping, 2 ready, 3 cancelled, -1 failed */
    HMODULE handle;
} ImageWarm;
typedef struct {
    int cache_idx;
    char path[768];
} ImageWarmJob;
typedef struct {
    int count;
    ImageWarmJob jobs[1];
} ImageWarmBatch;
static ImageWarm s_image_warm[CACHE_IDX_CAP];
volatile LONG g_overlay_image_warm_loaded = 0;
volatile LONG g_overlay_image_warm_pending = 0;
static int s_image_warm_enabled = 1;
static ULONGLONG s_image_warm_not_before_ms = 0;

static DWORD WINAPI overlay_image_warm_main(LPVOID unused) {
    ImageWarmBatch *batch = (ImageWarmBatch *)unused;
    ULONGLONG now = GetTickCount64();
    if (now < s_image_warm_not_before_ms)
        Sleep((DWORD)(s_image_warm_not_before_ms - now));
    for (int i = 0; i < batch->count; i++) {
        ImageWarmJob *job = &batch->jobs[i];
        int ci = job->cache_idx;
        if (InterlockedCompareExchange(&s_image_warm[ci].state, 0, 0) != 1) {
            InterlockedDecrement(&g_overlay_image_warm_pending);
            continue;
        }
        HMODULE h = LoadLibraryA(job->path);
        s_image_warm[ci].handle = h;
        if (InterlockedCompareExchange(&s_image_warm[ci].state,
                                       h ? 2 : -1, 1) == 1) {
            if (h) InterlockedIncrement(&g_overlay_image_warm_loaded);
        } else {
            /* The emulation thread needed this image before the batch reached
             * it and cancelled the speculative reference. */
            s_image_warm[ci].handle = NULL;
            if (h) FreeLibrary(h);
            InterlockedExchange(&s_image_warm[ci].state, 0);
        }
        InterlockedDecrement(&g_overlay_image_warm_pending);
    }
    free(batch);
    return 0;
}

static void overlay_image_warm_init(void) {
    const char *e = getenv("PSX_OVERLAY_IMAGE_WARM");
    if (e && (e[0] == '0' || e[0] == 'n' || e[0] == 'N'))
        s_image_warm_enabled = 0;
    /* Leave BIOS/game initialization alone, but finish before the first
     * streamed replacement of the dynamic text base. */
    s_image_warm_not_before_ms = GetTickCount64() + 5000u;
}

static void overlay_image_warm_queue(const int *indices, int count) {
    if (!s_image_warm_enabled || count <= 0) return;
    size_t bytes = sizeof(ImageWarmBatch) +
                   (size_t)(count - 1) * sizeof(ImageWarmJob);
    ImageWarmBatch *batch = (ImageWarmBatch *)malloc(bytes);
    if (!batch) return;
    batch->count = 0;
    for (int i = 0; i < count; i++) {
        int ci = indices[i];
        if (ci < 0 || ci >= s_cache_idx_count) continue;
        if (InterlockedCompareExchange(&s_image_warm[ci].state, 1, 0) != 0)
            continue;
        ImageWarmJob *job = &batch->jobs[batch->count++];
        job->cache_idx = ci;
        snprintf(job->path, sizeof(job->path), "%s", s_cache_idx[ci].path);
        InterlockedIncrement(&g_overlay_image_warm_pending);
    }
    if (batch->count == 0) { free(batch); return; }
    HANDLE h = CreateThread(NULL, 0, overlay_image_warm_main, batch, 0, NULL);
    if (!h) {
        for (int i = 0; i < batch->count; i++) {
            InterlockedDecrement(&g_overlay_image_warm_pending);
            InterlockedExchange(&s_image_warm[batch->jobs[i].cache_idx].state, 0);
        }
        free(batch);
        return;
    }
    SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(h);
}

static void overlay_image_warm_cancel(int ci) {
    if (ci < 0 || ci >= CACHE_IDX_CAP) return;
    InterlockedCompareExchange(&s_image_warm[ci].state, 3, 1);
}

static void overlay_image_warm_release(int ci) {
    if (ci < 0 || ci >= CACHE_IDX_CAP) return;
    if (InterlockedCompareExchange(&s_image_warm[ci].state, 0, 0) != 2) return;
    HMODULE h = s_image_warm[ci].handle;
    s_image_warm[ci].handle = NULL;
    InterlockedExchange(&s_image_warm[ci].state, 0);
    if (h) FreeLibrary(h); /* load_overlay_dll retained its own reference */
}
#else
long g_overlay_image_warm_loaded = 0;
long g_overlay_image_warm_pending = 0;
static void overlay_image_warm_init(void) {}
static void overlay_image_warm_queue(const int *indices, int count)
    { (void)indices; (void)count; }
static void overlay_image_warm_cancel(int ci) { (void)ci; }
static void overlay_image_warm_release(int ci) { (void)ci; }
#endif

static void overlay_image_warm_seed_boot_text(void);

/* Manifest-only cache index. Parsing the small .ranges sidecars up front lets
 * a dispatch hash the live code bytes and LoadLibrary only the exact matching
 * variant. This is the scalable alternative to trial-loading hundreds of DLLs
 * that share one reused RAM region. */
#define LAZY_MAN_CAP (CAND_CAP * 2)
#define LAZY_ENTRY_CAP 32768u
#define LAZY_ENTRY_MASK (LAZY_ENTRY_CAP - 1u)
#define LAZY_RANGE_LINK_CAP (LAZY_MAN_CAP * 8)
typedef struct {
    int cache_idx;
    ManFn fn;
    uint32_t val_gen;
    uint8_t state;                 /* 0xFF unknown, else ENTRY_VALID/INVALID */
    int next_entry;
    int next_bundle;
} LazyMan;
static LazyMan s_lazy_man[LAZY_MAN_CAP];
static int     s_lazy_man_n = 0;
static int     s_lazy_man_overflow = 0;
static int     s_lazy_entry_head[LAZY_ENTRY_CAP];
static int     s_lazy_entry_tail[LAZY_ENTRY_CAP];
static int     s_lazy_bundle_head[CACHE_IDX_CAP];
static int     s_lazy_page_head[RANGE_PAGE_COUNT];
static int     s_lazy_page_tail[RANGE_PAGE_COUNT];
static RangeLink s_lazy_range_links[LAZY_RANGE_LINK_CAP];
static int       s_lazy_range_link_n = 0;

static void rebuild_lazy_manifest_index(void) {
    s_lazy_man_n = 0;
    s_lazy_man_overflow = 0;
    s_lazy_range_link_n = 0;
    for (uint32_t i = 0; i < LAZY_ENTRY_CAP; i++) {
        s_lazy_entry_head[i] = -1;
        s_lazy_entry_tail[i] = -1;
    }
    for (int i = 0; i < CACHE_IDX_CAP; i++) s_lazy_bundle_head[i] = -1;
    for (uint32_t i = 0; i < RANGE_PAGE_COUNT; i++) {
        s_lazy_page_head[i] = -1;
        s_lazy_page_tail[i] = -1;
    }
    for (int ci = 0; ci < s_cache_idx_count; ci++) {
        char path[800];
        snprintf(path, sizeof(path), "%s", s_cache_idx[ci].path);
        size_t n = strlen(path);
        if (n < OVERLAY_SHARED_EXT_LEN ||
            strcmp(path + n - OVERLAY_SHARED_EXT_LEN, OVERLAY_SHARED_EXT) != 0)
            continue;
        snprintf(path + n - OVERLAY_SHARED_EXT_LEN,
                 sizeof(path) - (n - OVERLAY_SHARED_EXT_LEN), ".ranges");
        int man_n = 0;
        ManFn *man = parse_manifest(path, &man_n);
        if (!man) continue;
        s_cache_idx[ci].func_count = man_n;
        s_cache_idx[ci].indexed_func_count = 0;
        for (int mi = 0; mi < man_n; mi++) {
            if (!man[mi].has_crc || man[mi].n <= 0) continue;
            uint32_t entry = man[mi].entry & 0x1FFFFFFFu;
            uint32_t bucket = (entry * 2654435761u) & LAZY_ENTRY_MASK;

            if (s_lazy_man_n >= LAZY_MAN_CAP) {
                s_lazy_man_overflow = 1;
                break;
            }
            int li = s_lazy_man_n++;
            LazyMan *lm = &s_lazy_man[li];
            lm->cache_idx = ci;
            lm->fn = man[mi];
            lm->val_gen = 0;
            lm->state = 0xFFu;
            lm->next_bundle = s_lazy_bundle_head[ci];
            s_lazy_bundle_head[ci] = li;
            s_cache_idx[ci].indexed_func_count++;
            /* Exact-entry chains historically insert at the head, so later
             * cache entries win. Keep that order distinct from range ownership
             * below, which is oldest-first. */
            lm->next_entry = s_lazy_entry_head[bucket];
            s_lazy_entry_head[bucket] = li;
            if (s_lazy_entry_tail[bucket] < 0) s_lazy_entry_tail[bucket] = li;

            uint8_t seen[RANGE_PAGE_COUNT / 8] = {0};
            for (int r = 0; r < lm->fn.n; r++) {
                uint32_t lo = lm->fn.lo[r] & 0x1FFFFFFFu;
                uint32_t hi = lo + lm->fn.len[r] - 1u;
                uint32_t p0 = lo >> 12, p1 = hi >> 12;
                if (p0 >= RANGE_PAGE_COUNT) continue;
                if (p1 >= RANGE_PAGE_COUNT) p1 = RANGE_PAGE_COUNT - 1u;
                for (uint32_t p = p0; p <= p1; p++) {
                    uint8_t bit = (uint8_t)(1u << (p & 7u));
                    if (seen[p >> 3] & bit) continue;
                    seen[p >> 3] |= bit;
                    if (s_lazy_range_link_n >= LAZY_RANGE_LINK_CAP) {
                        s_lazy_man_overflow = 1;
                        break;
                    }
                    int ri = s_lazy_range_link_n++;
                    s_lazy_range_links[ri].cand = li;
                    s_lazy_range_links[ri].next = -1;
                    if (s_lazy_page_head[p] < 0) s_lazy_page_head[p] = ri;
                    else s_lazy_range_links[s_lazy_page_tail[p]].next = ri;
                    s_lazy_page_tail[p] = ri;
                }
                if (s_lazy_man_overflow) break;
            }
            if (s_lazy_man_overflow) break;
        }
        free(man);
        if (s_lazy_man_overflow) break;
    }
}

static int cache_idx_has_path(const char *path) {
    for (int i = 0; i < s_cache_idx_count; i++)
        if (strcmp(s_cache_idx[i].path, path) == 0) return 1;
    return 0;
}

/* Dedup by FILENAME (<addr8>_<crc8>.dll = region+crc). Used so a tcc/ shard for a
 * region already covered by a gcc/ shard is skipped: scan_cache_dir scans gcc/
 * FIRST, so gcc always wins the tie (tier order gcc > tcc). Also subsumes the
 * per-path rescan-idempotence check (same dir => same filename). */
static int cache_idx_has_basename(const char *fname) {
    for (int i = 0; i < s_cache_idx_count; i++) {
        const char *b = strrchr(s_cache_idx[i].path, '/');
        b = b ? b + 1 : s_cache_idx[i].path;
        if (strcmp(b, fname) == 0) return 1;
    }
    return 0;
}

/* Canonical cache arch-abi tag (caches are namespaced per backend AND per
 * target so a Windows-x64 gcc DLL and, later, a same-OS arm64 build for the
 * same fragment never comingle). compile_overlays.py
 * computes the IDENTICAL string from platform.system()/machine(); keep the two
 * mappings in lockstep ("<os>-<arch>": win|linux|macos + x64|arm64|x86). */
#if defined(_WIN32)
#  define PSX_OL_OS "win"
#elif defined(__APPLE__)
#  define PSX_OL_OS "macos"
#else
#  define PSX_OL_OS "linux"
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
#  define PSX_OL_ARCH "arm64"
#elif defined(__x86_64__) || defined(_M_X64)
#  define PSX_OL_ARCH "x64"
#elif defined(__i386__) || defined(_M_IX86)
#  define PSX_OL_ARCH "x86"
#else
#  define PSX_OL_ARCH "unknown"
#endif
#define PSX_OVERLAY_ARCH_ABI PSX_OL_OS "-" PSX_OL_ARCH

const char *overlay_loader_arch_abi(void) { return PSX_OVERLAY_ARCH_ABI; }

#ifndef _WIN32
static int add_posix_cache_file(const PsxOverlayCacheFile *file, void *opaque) {
    (void)opaque;
    if (s_cache_idx_count >= CACHE_IDX_CAP) {
        loader_log("*** CACHE INDEX FULL (%d): further DLLs near %s are being "
                   "IGNORED — their regions will run interpreted. Raise "
                   "CACHE_IDX_CAP.", CACHE_IDX_CAP, file->path);
        return 1;
    }
    if (cache_idx_has_basename(file->name)) return 0;
    CacheEntry *e = &s_cache_idx[s_cache_idx_count++];
    e->region_start = file->region_start;
    e->mtime = file->mtime;
    e->func_count = 0;
    e->indexed_func_count = 0;
    snprintf(e->path, sizeof(e->path), "%s", file->path);
    return 0;
}
#endif

/* Scan one directory for <addr8>_<crc8>.dll cache entries into the index.
 * `dir` is a full directory path. Idempotent (skips already-indexed paths). */
static void scan_one_cache_dir(const char *dir) {
#ifdef _WIN32
    char pattern[768];
    snprintf(pattern, sizeof(pattern), "%s/*_*.dll", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        uint32_t addr = 0, crc = 0;
        if (!psx_overlay_cache_name_parse(fd.cFileName, &addr, &crc)) continue;
        (void)crc;
        if (s_cache_idx_count >= CACHE_IDX_CAP) {
            /* Never-again (the silent-256 truncation): overflowing the index
             * means real native coverage is being IGNORED. Shout once. */
            loader_log("*** CACHE INDEX FULL (%d): further DLLs in %s are being "
                       "IGNORED — their regions will run interpreted. Raise "
                       "CACHE_IDX_CAP.", CACHE_IDX_CAP, dir);
            break;
        }
        char full[768];
        snprintf(full, sizeof(full), "%s/%s", dir, fd.cFileName);
        /* skip if this region+crc is already indexed (rescan idempotence, AND a
         * higher-priority namespace already covers it — gcc/ scanned before tcc/). */
        if (cache_idx_has_basename(fd.cFileName)) continue;
        CacheEntry *e = &s_cache_idx[s_cache_idx_count++];
        e->region_start = addr;
        e->mtime = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) |
                   (uint64_t)fd.ftLastWriteTime.dwLowDateTime;
        e->func_count = 0;
        e->indexed_func_count = 0;
        snprintf(e->path, sizeof(e->path), "%s", full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    psx_overlay_posix_scan_cache_dir(dir, add_posix_cache_file, NULL);
#endif
}

/* Scan the namespaced gcc cache: gcc/<arch-abi>/cg<codegen-ver>/. The codegen
 * version segment means a build with new emitter output reads a FRESH directory
 * and never picks up a stale DLL (old versions coexist on disk, no migration).
 * (Pre-1.0: no legacy fallback — older flat / unversioned caches are simply
 * ignored and regenerated.) */
/* Hardening (never-again for the silent cg-tag read≠write drift): when the loader
 * finds ZERO shards under its OWN cg-tag, check whether a SIBLING cg<...> folder in
 * the same tier holds shards. If so, the autocompile wrote to a different codegen
 * hash than this build reads — every overlay will silently fall to the interpreter
 * ("why is it slow"). This is USUALLY a mismatched overlay_autocompile_cmd
 * --recompiler / --runtime-include (e.g. a cross-build: runtime from one framework
 * checkout, autocompile pointed at another). Shout it once, loudly, with both tags,
 * so it can never again be diagnosed as generic slowness. */
static void warn_on_cgtag_mismatch(const char *tier) {
#ifdef _WIN32
    char base[768], pattern[900];
    snprintf(base, sizeof base, "%s/%s/%s/%s",
             s_cache_dir, s_game_id, tier, PSX_OVERLAY_ARCH_ABI);
    char expect[64];
    snprintf(expect, sizeof expect, "cg%d_%08x",
             PSX_OVERLAY_CODEGEN_VER, (unsigned)PSX_OVERLAY_CODEGEN_HASH);
    snprintf(pattern, sizeof pattern, "%s/cg*", base);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (strcmp(fd.cFileName, expect) == 0) continue;     /* our own tag */
        char dllpat[900]; WIN32_FIND_DATAA fd2;
        snprintf(dllpat, sizeof dllpat, "%s/%s/*_*.dll", base, fd.cFileName);
        HANDLE h2 = FindFirstFileA(dllpat, &fd2);
        if (h2 != INVALID_HANDLE_VALUE) {          /* sibling tag HAS shards */
            FindClose(h2);
            loader_log("*** OVERLAY CACHE HASH MISMATCH: this build reads %s/%s but "
                       "shards exist under %s/%s. The autocompile is writing to a "
                       "DIFFERENT codegen hash than this runtime reads -> ALL overlays "
                       "run INTERPRETED (slow). Fix overlay_autocompile_cmd's "
                       "--recompiler/--runtime-include to match THIS build's framework.",
                       tier, expect, tier, fd.cFileName);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    char base[768], expect[64], found[256];
    snprintf(base, sizeof base, "%s/%s/%s/%s",
             s_cache_dir, s_game_id, tier, PSX_OVERLAY_ARCH_ABI);
    snprintf(expect, sizeof expect, "cg%d_%08x",
             PSX_OVERLAY_CODEGEN_VER, (unsigned)PSX_OVERLAY_CODEGEN_HASH);
    if (psx_overlay_posix_find_other_cache_tag(base, expect, found,
                                               sizeof(found))) {
        loader_log("*** OVERLAY CACHE HASH MISMATCH: this build reads %s/%s but "
                   "shards exist under %s/%s. The autocompile is writing to a "
                   "DIFFERENT codegen hash than this runtime reads -> ALL overlays "
                   "run INTERPRETED (slow). Fix overlay_autocompile_cmd's "
                   "--recompiler/--runtime-include to match THIS build's framework.",
                   tier, expect, tier, found);
    }
#endif
}

static void cache_idx_remove_path(const char *path) {
    for (int i = 0; i < s_cache_idx_count; i++) {
        if (strcmp(s_cache_idx[i].path, path) == 0) {
            s_cache_idx[i] = s_cache_idx[--s_cache_idx_count];
            return;
        }
    }
}

static void remove_posix_dll_and_manifest(const char *dll_path) {
#ifndef _WIN32
    remove(dll_path);
    char ranges[800];
    int n = snprintf(ranges, sizeof(ranges), "%s", dll_path);
    if (n >= OVERLAY_SHARED_EXT_LEN &&
        (size_t)n + 7 - OVERLAY_SHARED_EXT_LEN < sizeof(ranges) &&
        strcmp(ranges + n - OVERLAY_SHARED_EXT_LEN, OVERLAY_SHARED_EXT) == 0) {
        memcpy(ranges + n - OVERLAY_SHARED_EXT_LEN, ".ranges", 8);
        remove(ranges);
    }
#else
    (void)dll_path;
#endif
}

#ifndef _WIN32
typedef struct PosixAbiSweep {
    int purged;
    int kept;
} PosixAbiSweep;

static int posix_abi_sweep_file(const PsxOverlayCacheFile *file, void *opaque) {
    PosixAbiSweep *sweep = (PosixAbiSweep *)opaque;
    char error[256] = {0};
    void *handle = psx_overlay_posix_library_open(file->path, error, sizeof(error));
    int abi = 0;
    if (handle) {
        typedef int (*AbiFn)(void);
        AbiFn abi_fn = (AbiFn)psx_overlay_posix_library_symbol(handle, "overlay_abi");
        abi = abi_fn ? abi_fn() : 0;
        psx_overlay_posix_library_close(handle);
    }
    if (handle && abi == PSX_OVERLAY_ABI_TAG) {
        sweep->kept++;
        return 0;
    }

    if (handle)
        loader_log("ABI preflight rejecting %s: dll=0x%X runtime=0x%X",
                   file->path, abi, PSX_OVERLAY_ABI_TAG);
    else
        loader_log("ABI preflight rejecting unloadable %s: %s",
                   file->path, error);
    remove_posix_dll_and_manifest(file->path);
    cache_idx_remove_path(file->path);
    sweep->purged++;
    return 0;
}
#endif

/* ---- ABI pre-flight sweep (batch purge) ----------------------------------
 * A contract-ABI bump (e.g. v9 -> v10) invalidates EVERY cached DLL at once.
 * The lazy path handled that per-dispatch: try_load_region -> LoadLibrary ->
 * ABI reject -> DeleteFile -> retry next dispatch — thousands of file ops on
 * the EMULATION thread (which owns the window pump). On a large cache this
 * presented as a "(Not Responding)" black window for minutes (MMX6, 136-DLL
 * cache, the v10 migration, 2026-07-03 regression gate).
 *
 * Instead, sweep ONCE at scan time: load each indexed DLL, check overlay_abi,
 * delete mismatches (DLL + .ranges) and drop them from the index. A marker
 * file (.abi_<tag>.ok) per cache dir records a completed sweep, so healthy
 * caches skip the sweep entirely on later boots — steady-state cost is one
 * stat per dir. Autocompile only ever writes current-ABI DLLs, so the marker
 * stays truthful; the per-load ABI gate in load_overlay_dll remains as
 * defense in depth. */
static void abi_preflight_sweep(const char *dir) {
#ifdef _WIN32
    char marker[900];
    snprintf(marker, sizeof marker, "%s/.abi_%08x.ok", dir, (unsigned)PSX_OVERLAY_ABI_TAG);
    if (GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES) return;  /* swept */

    /* Enumerate the DIRECTORY, not the index: the index is capacity-bounded
     * and dedup-filtered, so sweeping it alone can leave stale files behind
     * while the marker claims a complete sweep (found by the negative test —
     * the planted v6 DLL survived behind the old 256-entry index cap). */
    int purged = 0, kept = 0;
    char pattern[900];
    snprintf(pattern, sizeof pattern, "%s/*_*.dll", dir);
    WIN32_FIND_DATAA fd;
    HANDLE hf = FindFirstFileA(pattern, &fd);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            char full[900];
            snprintf(full, sizeof full, "%s/%s", dir, fd.cFileName);
            HMODULE h = LoadLibraryA(full);
            if (h) {
                typedef int (*AbiFn)(void);
                AbiFn abi_fn = (AbiFn)GetProcAddress(h, "overlay_abi");
                int abi = abi_fn ? abi_fn() : 0;
                FreeLibrary(h);
                if (abi == PSX_OVERLAY_ABI_TAG) { kept++; continue; }
            }
            /* Unloadable or wrong ABI: purge DLL + its .ranges + index entry. */
            DeleteFileA(full);
            char ranges[912];
            size_t n = strlen(full);
            if (n > 4 && n + 4 < sizeof ranges) {
                memcpy(ranges, full, n - 4);
                memcpy(ranges + n - 4, ".ranges", 8);
                DeleteFileA(ranges);
            }
            purged++;
            for (int i = 0; i < s_cache_idx_count; i++) {
                if (strcmp(s_cache_idx[i].path, full) == 0) {
                    s_cache_idx[i] = s_cache_idx[--s_cache_idx_count];
                    break;
                }
            }
        } while (FindNextFileA(hf, &fd));
        FindClose(hf);
    }
    if (purged)
        loader_log("abi preflight: purged %d stale DLL(s), kept %d in %s",
                   purged, kept, dir);
    /* Mark the sweep complete (even if nothing purged) so later boots skip it. */
    HANDLE m = CreateFileA(marker, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (m != INVALID_HANDLE_VALUE) CloseHandle(m);
#else
    char marker[900];
    snprintf(marker, sizeof marker, "%s/.abi_%08x.ok", dir,
             (unsigned)PSX_OVERLAY_ABI_TAG);
    if (access(marker, F_OK) == 0) return;

    PosixAbiSweep sweep = {0};
    psx_overlay_posix_scan_cache_dir(dir, posix_abi_sweep_file, &sweep);
    if (sweep.purged)
        loader_log("abi preflight: purged %d stale DLL(s), kept %d in %s",
                   sweep.purged, sweep.kept, dir);
    FILE *m = fopen(marker, "wb");
    if (m) fclose(m);
#endif
}

static void scan_cache_dir(void) {
    char dir[768];
    /* Tier order: scan gcc/ FIRST (highest native priority — the dev/production
     * shards), THEN tcc/ (the toolchain-free fallback). scan_one_cache_dir dedups
     * by filename (region+crc), so a tcc shard for a region a gcc shard already
     * covers is skipped — gcc always wins the tie. */
    snprintf(dir, sizeof(dir), "%s/%s/gcc/%s/cg%d_%08x",
             s_cache_dir, s_game_id, PSX_OVERLAY_ARCH_ABI, PSX_OVERLAY_CODEGEN_VER,
             (unsigned)PSX_OVERLAY_CODEGEN_HASH);
    scan_one_cache_dir(dir);
    abi_preflight_sweep(dir);
    snprintf(dir, sizeof(dir), "%s/%s/tcc/%s/cg%d_%08x",
             s_cache_dir, s_game_id, PSX_OVERLAY_ARCH_ABI, PSX_OVERLAY_CODEGEN_VER,
             (unsigned)PSX_OVERLAY_CODEGEN_HASH);
    scan_one_cache_dir(dir);
    abi_preflight_sweep(dir);

    rebuild_lazy_manifest_index();

    /* Never-again guard: if we loaded NOTHING but wrong-hash shards exist, shout. */
    if (s_cache_idx_count == 0) {
        warn_on_cgtag_mismatch("gcc");
        warn_on_cgtag_mismatch("tcc");
    }

    /* Opt-in startup/rescan inventory from PR #13. Current master deliberately
     * keeps routine loader events off stderr, so retain that policy unless the
     * operator explicitly requests the diagnostic. */
    const char *inventory = getenv("PSX_OVERLAY_CACHE_INVENTORY");
    if (inventory && *inventory && *inventory != '0') {
        fprintf(stderr, "overlay cache scan: %d shared library file(s) indexed "
                        "[arch=%s]\n", s_cache_idx_count, PSX_OVERLAY_ARCH_ABI);
        for (int i = 0; i < s_cache_idx_count; i++) {
            const char *base = strrchr(s_cache_idx[i].path, '/');
            base = base ? base + 1 : s_cache_idx[i].path;
            fprintf(stderr, "  [%d] %s (region=0x%08X, functions=%d)\n",
                    i, base, s_cache_idx[i].region_start,
                    s_cache_idx[i].indexed_func_count);
        }
        fflush(stderr);
    }
}

/* (sljit removed 2026-07-15: the persisted sljit shard cache — blob header,
 * persist/publish writers, on-disk reload scan, and their debug accessors —
 * lived here. No shards are produced or reloaded anymore.) */

/* True if the cache holds a DLL for this region compiled from an image with
 * this CRC (filename <addr8>_<crc8>.dll). Autocapture's "unseen" test. */
int overlay_loader_has_cached_crc(uint32_t region_start, uint32_t crc) {
    for (int i = 0; i < s_cache_idx_count; i++) {
        if (s_cache_idx[i].region_start != region_start) continue;
        const char *fn = strrchr(s_cache_idx[i].path, '/');
        fn = fn ? fn + 1 : s_cache_idx[i].path;
        if (strlen(fn) == 17u + OVERLAY_SHARED_EXT_LEN &&
            strcmp(fn + 17, OVERLAY_SHARED_EXT) == 0 &&
            (uint32_t)strtoul(fn + 9, NULL, 16) == crc)
            return 1;
    }
    return 0;
}

/* ---- Runtime callbacks wired into overlay DLLs via overlay_init() ------ */

extern void psx_dispatch_call(CPUState *cpu, uint32_t addr, uint32_t ra);
extern void psx_check_interrupts(CPUState *cpu);
extern void psx_check_interrupts_at(CPUState *cpu, uint32_t resume_pc);
extern int psx_interrupt_delivery_needed(const CPUState *cpu);
extern void gte_execute(CPUState *cpu, uint32_t cmd);
extern int psx_syscall(CPUState *cpu, uint32_t code);
extern void psx_unknown_dispatch(CPUState *cpu, uint32_t addr, uint32_t phys);
extern void debug_server_log_call_entry_fn(uint32_t func_addr);

static OverlayCallbacks s_callbacks;

/* Timing-hypothesis probe: native overlay code calls psx_check_interrupts at
 * EVERY block (up to ~100x/function), whereas the dirty-RAM interpreter checks
 * only ~every 4096 instructions + at function exit. That cadence gap can deliver
 * an interrupt at a different point in native vs interp -> divergence with no
 * mistranslation. When s_suppress_irq is set we drop native's per-block checks
 * (cadence ~ interp). If the blue screen vanishes, the cause is interrupt timing.
 * Two modes: full suppress, or rate-limit (call the real check every Nth time).
 *
 * NOTE: this probe rests on the cross-game finding that overlay code never
 * installs its own IRQ/DMA/callback handlers (PsyQ convention — all timing-
 * critical handlers live in resident static code; holds for every sampled
 * title). That makes interrupt-check *cadence* the only native-vs-interp
 * difference. If a future title violates the convention (an overlay installs a
 * handler), this probe is no longer sufficient and a discriminator is needed. */
static int      s_suppress_irq = 0;
static uint32_t s_irq_ratelimit = 0;   /* 0 = full suppress; N = every Nth call */
static uint32_t s_irq_callcount = 0;
static uint64_t s_irq_suppressed = 0;
static uint32_t s_irq_budget_cycles = 0; /* 0 = off; otherwise guest cycles/check */
static uint64_t s_irq_last_check_cycle = UINT64_MAX;
static int      s_irq_suppress_cdrom_only = 0;
static int      s_irq_post_dispatch_pump = 0;
static int      s_irq_defer_cdrom = 0;

extern uint32_t i_stat;
extern uint32_t i_mask;

/* Nested call-unit depth (Ape memcard native<->interp resume-desync fix).
 *
 * When the dirty interpreter (or a shard) issues a guest jal/jalr to a callee
 * run as a UNIT — an overlay-native shard (overlay_loader_call_native) OR a
 * non-local dirty/kernel routine (dispatch_nonlocal_call -> psx_dispatch_call) —
 * that callee must be ATOMIC w.r.t. the cooperative thread switch, exactly like
 * statically-compiled code (psx_dispatch_impl checks interrupts ONLY at the
 * outermost dispatch return; a nested callee never interrupts before its caller
 * runs the post-call continuation). Both the overlay CI wrappers AND the dirty
 * per-transfer IRQ pumps, however, checked interrupts at EVERY block/transfer
 * regardless of nesting; an IRQ + cooperative ChangeThread landing inside such a
 * nested unit suspended the interrupted thread with an INCONSISTENT snapshot
 * (resume PC at the caller's post-call point, sp still mid-callee) -> a leaked
 * stack frame that compounded across cooperative cycles into a smeared jumptable
 * index (the Ape "Checking MEMORY CARD" softlock/fatal). While this depth is >0
 * both backends defer the IRQ check; the callee runs to completion, then the
 * enclosing top-level dirty pump / outermost dispatch return delivers the IRQ at
 * a consistent (pc, sp) boundary. The TOP-LEVEL flow (depth 0) still pumps IRQs,
 * so the consumer's poll loop and long overlay game-loops keep their
 * responsiveness. Incremented only when the A/B toggle is on (below). */
int g_call_unit_depth = 0;

/* A/B toggle for the nested-unit IRQ deferral (PSX_OVERLAY_UNIT_DEFER, default
 * ON). Gates the depth INCREMENTS (so =0 leaves g_call_unit_depth at 0 and
 * nothing defers — the pre-fix behavior), letting us attribute a behavior change
 * to this fix without a rebuild. */
int overlay_unit_defer_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("PSX_OVERLAY_UNIT_DEFER");
        cached = (e && (e[0] == '0' || e[0] == 'n' || e[0] == 'N')) ? 0 : 1;
    }
    return cached;
}

void overlay_loader_set_irq_suppress(int mode, uint32_t ratelimit) {
    s_suppress_irq  = mode ? 1 : 0;
    s_irq_ratelimit = ratelimit;
    /* Reset counters on every (re)arm so an A/B run reports a clean, isolated
     * suppressed-count for that arming rather than a cumulative total. */
    s_irq_callcount  = 0;
    s_irq_suppressed = 0;
}
void overlay_loader_get_irq_suppress(int *mode, uint32_t *rl, uint64_t *supp) {
    if (mode) *mode = s_suppress_irq;
    if (rl)   *rl   = s_irq_ratelimit;
    if (supp) *supp = s_irq_suppressed;
}

static int overlay_irq_suppressed_now(void) {
    if (s_suppress_irq) {
        if (s_irq_ratelimit == 0) { s_irq_suppressed++; return 1; }
        if ((++s_irq_callcount % s_irq_ratelimit) != 0) { s_irq_suppressed++; return 1; }
    }
    if (s_irq_suppress_cdrom_only) {
        uint32_t pending = i_stat & i_mask;
        if (pending == (1u << IRQ_CDROM)) {
            s_irq_suppressed++;
            return 1;
        }
    }
    if (s_irq_budget_cycles != 0) {
        uint64_t now = psx_get_cycle_count();
        if (s_irq_last_check_cycle != UINT64_MAX &&
            now - s_irq_last_check_cycle < (uint64_t)s_irq_budget_cycles) {
            s_irq_suppressed++;
            return 1;
        }
        s_irq_last_check_cycle = now;
    }
    return 0;
}

static void overlay_ci_wrapper(CPUState *cpu) {
    /* Defer while inside a nested call unit — a callee must not interrupt
     * mid-call (static-call atomicity). See g_call_unit_depth. */
    if (g_call_unit_depth > 0) return;
    if (overlay_irq_suppressed_now()) return;
    /* psx_advance_cycles() has already raised every device edge due at this
     * block. Avoid entering the full scheduler/diagnostic path when COP0 could
     * not take the IRQ anyway. FMV polling loops can execute this edge millions
     * of times while an INTC bit is pending but IEc is deliberately clear. */
    if ((i_stat & i_mask) == 0) return;
    if ((cpu->cop0[12] & ((1u << 10) | 1u)) != ((1u << 10) | 1u)) return;
    if (!psx_interrupt_delivery_needed(cpu)) return;
    if (s_irq_defer_cdrom && (i_stat & (1u << IRQ_CDROM))) {
        uint32_t saved_cd = i_stat & (1u << IRQ_CDROM);
        i_stat &= ~(1u << IRQ_CDROM);
        psx_check_interrupts(cpu);
        i_stat |= saved_cd;
        return;
    }
    psx_check_interrupts(cpu);
}

static int overlay_idle_note_is_internal_or_return(const CPUState *cpu,
                                                   uint32_t resume_pc) {
    uint32_t phys = resume_pc & 0x1FFFFFFFu;
    if ((resume_pc & 0x1FFFFFFFu) == (cpu->gpr[31] & 0x1FFFFFFFu)) return 1;
    if (s_active_depth <= 0) return 0;
    Candidate *c = &s_cand[s_active_stack[s_active_depth - 1]];
    for (int r = 0; r < c->nranges; r++) {
        uint32_t lo = c->range_lo[r];
        if (phys >= lo && phys < lo + c->range_len[r]) return 1;
    }
    return 0;
}

static void overlay_ci_at_wrapper(CPUState *cpu, uint32_t resume_pc) {
    /* Defer while inside a nested call unit (see g_call_unit_depth): suspending
     * here would save resume_pc at the callee's block leader while the enclosing
     * dirty caller expects an atomic unit — the resume-desync bug. */
    if (g_call_unit_depth > 0) return;
    if (overlay_irq_suppressed_now()) return;
    if ((i_stat & i_mask) == 0) return;
    if ((cpu->cop0[12] & ((1u << 10) | 1u)) != ((1u << 10) | 1u)) return;
    if (!psx_interrupt_delivery_needed(cpu)) return;
    extern int g_idle_note_suppress;
    int suppress_idle_note = overlay_idle_note_is_internal_or_return(cpu, resume_pc);
    if (suppress_idle_note) g_idle_note_suppress++;
    if (s_irq_defer_cdrom && (i_stat & (1u << IRQ_CDROM))) {
        uint32_t saved_cd = i_stat & (1u << IRQ_CDROM);
        i_stat &= ~(1u << IRQ_CDROM);
        psx_check_interrupts_at(cpu, resume_pc);
        i_stat |= saved_cd;
        if (suppress_idle_note) g_idle_note_suppress--;
        return;
    }
    psx_check_interrupts_at(cpu, resume_pc);
    if (suppress_idle_note) g_idle_note_suppress--;
}

static int overlay_irq_budget_blocks_now(void) {
    if (s_irq_budget_cycles == 0) return 0;
    uint64_t now = psx_get_cycle_count();
    if (s_irq_last_check_cycle != UINT64_MAX &&
        now - s_irq_last_check_cycle < (uint64_t)s_irq_budget_cycles) {
        s_irq_suppressed++;
        return 1;
    }
    s_irq_last_check_cycle = now;
    return 0;
}

static void overlay_post_dispatch_irq_pump(CPUState *cpu) {
    if (!s_irq_post_dispatch_pump) return;
    /* Never deliver an IRQ inside a shadow run: the handler would ack device
     * state (I_STAT) that survives the sandbox while the guest state it ran on
     * is rolled back — a lost interrupt. Shadow runs are IRQ-suppressed by
     * design (s_suppress_irq in run_shadow_diff); this pump must match. */
    if (s_in_shadow) return;
    if (overlay_irq_budget_blocks_now()) return;
    if (cpu->pc != 0u) psx_check_interrupts_at(cpu, cpu->pc);
    else psx_check_interrupts(cpu);
}

/* Probe wrapper (mmx6_card_load_regression_state): record shard-side calls to
 * xprobe-watched targets (before + after) so overlay-DLL call-outs are visible
 * in the xprobe `watched` dump the way interp JAL/JALR sites already are. */
extern void dirty_ram_xprobe_call_note(CPUState *cpu, uint32_t target, uint32_t ra, uint8_t phase);
extern int g_exec_phase;   /* wall-time sampler phase (dirty_ram_interp.c) */
static void overlay_dispatch_call_probed(CPUState *cpu, uint32_t addr, uint32_t ra) {
    dirty_ram_xprobe_call_note(cpu, addr, ra, 0);
    int prev_phase = g_exec_phase;
    g_exec_phase = 3;   /* compiled route; dirty/native callees re-tag inside */
    psx_dispatch_call(cpu, addr, ra);
    g_exec_phase = prev_phase;
    dirty_ram_xprobe_call_note(cpu, addr, ra, 1);
}

static void init_callbacks(void) {
    extern void psx_restore_state_escape(void);
    extern void psx_rfe_mark_escape(void);
    s_callbacks.dispatch_call        = overlay_dispatch_call_probed;
    s_callbacks.check_interrupts     = overlay_ci_wrapper;
    s_callbacks.check_interrupts_at  = overlay_ci_at_wrapper;
    { extern void psx_advance_cycles(uint32_t cycles);
      s_callbacks.advance_cycles     = psx_advance_cycles; }
    s_callbacks.gte_execute          = gte_execute;
    s_callbacks.psx_syscall          = psx_syscall;
    s_callbacks.psx_native_bad_entry = psx_native_bad_entry;
    s_callbacks.psx_unknown_dispatch = psx_unknown_dispatch;
#ifdef PSX_NO_DEBUG_TOOLS
    /* Generated DLLs NULL-check this callback. Avoid a production call/return
     * at every guest function entry when the callee would immediately no-op. */
    s_callbacks.log_call_entry       = NULL;
#else
    s_callbacks.log_call_entry       = debug_server_log_call_entry_fn;
#endif
    s_callbacks.psx_restore_state_escape = psx_restore_state_escape;
    /* Return-from-exception mark (ABI v12): overlay `rfe` ops forward here. */
    s_callbacks.rfe_mark_escape          = psx_rfe_mark_escape;
    /* Call-contract state (ABI v2): DLL code shares the runtime's bail
     * flag and counters through these pointers. */
    s_callbacks.call_bail_flag = &g_psx_call_bail;
    s_callbacks.bail_first     = &g_psx_bail_first;
    s_callbacks.bail_resolved  = &g_psx_bail_resolved;
    /* Widescreen hooks (ABI v3): overlay-emitted psx_ws_* calls forward to
     * the runtime's live widescreen state (gpu.c). */
    {
        extern int  psx_ws_backdrop_x(int x);
        extern int  psx_ws_x_margin(void);
        extern void psx_ws_sprite_tag(CPUState *cpu);
        extern uint32_t psx_ws_backdrop_value(uint32_t orig, int is_end, int window_cols);  /* ABI v4 */
        s_callbacks.ws_backdrop_x    = psx_ws_backdrop_x;
        s_callbacks.ws_x_margin      = psx_ws_x_margin;
        s_callbacks.ws_sprite_tag    = psx_ws_sprite_tag;
        s_callbacks.ws_backdrop_value = psx_ws_backdrop_value;
    }
    /* Faithful-timing functions (ABI v9): overlay code built with
     * PSX_ENABLE_BLOCK_CYCLES emits these; forward to the runtime's real impls so
     * native overlays charge cycles on the SAME timeline as the interp/BIOS. */
    {
        extern uint32_t psx_cyc_load_word(CPUState*, uint32_t, uint32_t, uint32_t);
        extern uint16_t psx_cyc_load_half(CPUState*, uint32_t, uint32_t, uint32_t);
        extern uint8_t  psx_cyc_load_byte(CPUState*, uint32_t, uint32_t, uint32_t);
        extern uint32_t psx_cyc_lwc2_read(CPUState*, uint32_t);
        extern void     psx_icache_fetch_fn(CPUState*, uint32_t);
        extern void     psx_muldiv_set(CPUState*, uint32_t);
        extern void     psx_muldiv_stall(CPUState*);
        extern uint32_t psx_mult_latency_s(uint32_t);
        extern uint32_t psx_mult_latency_u(uint32_t);
        extern void     psx_gte_stall(CPUState*);
        extern void     psx_gte_read(CPUState*, uint32_t);
        extern int      psx_slice_block_impl(CPUState*, uint32_t, uint32_t, int);
        s_callbacks.cyc_load_word  = psx_cyc_load_word;
        s_callbacks.cyc_load_half  = psx_cyc_load_half;
        s_callbacks.cyc_load_byte  = psx_cyc_load_byte;
        s_callbacks.cyc_lwc2_read  = psx_cyc_lwc2_read;
        s_callbacks.icache_fetch   = psx_icache_fetch_fn;
        s_callbacks.muldiv_set     = psx_muldiv_set;
        s_callbacks.muldiv_stall   = psx_muldiv_stall;
        s_callbacks.mult_latency_s = psx_mult_latency_s;
        s_callbacks.mult_latency_u = psx_mult_latency_u;
        s_callbacks.gte_stall      = psx_gte_stall;
        s_callbacks.gte_read       = psx_gte_read;
        s_callbacks.slice_block    = psx_slice_block_impl;
        /* ABI v10: GTE special-register accessors — the emitter emits direct
         * calls for flag/IR/derived GTE regs (mfc2/cfc2/mtc2/ctc2); a GTE-heavy
         * overlay DLL cannot link without these forwarded. */
        {
            extern uint32_t gte_read_data(CPUState*, uint8_t);
            extern uint32_t gte_read_ctrl(CPUState*, uint8_t);
            extern void     gte_write_data(CPUState*, uint8_t, uint32_t);
            extern void     gte_write_ctrl(CPUState*, uint8_t, uint32_t);
            s_callbacks.gte_read_data  = gte_read_data;
            s_callbacks.gte_read_ctrl  = gte_read_ctrl;
            s_callbacks.gte_write_data = gte_write_data;
            s_callbacks.gte_write_ctrl = gte_write_ctrl;
        }
        /* ABI v12: rfe escape marker — emitted at every `rfe`, including in
         * overlay-resident exception-context code; mutates the runtime's
         * shared RFE-pending state so it must forward. */
        {
            extern void psx_rfe_mark_escape(void);
            s_callbacks.rfe_mark_escape = psx_rfe_mark_escape;
        }
        /* ABI v13: typed native-wide signed player-X bound — the emitter
         * rewrites configured signed_x_bound LUI sites (Einhander player
         * bounds) into psx_ws_player_x_bound() calls, including in
         * overlay-resident code; the clamp reads the host's live widescreen
         * state (gpu.c) so the DLL must forward. */
        {
            extern int32_t psx_ws_player_x_bound(int32_t vanilla);
            s_callbacks.ws_player_x_bound = psx_ws_player_x_bound;
        }
    }
}

/* ---- DLL loading and export enumeration -------------------------------- */

#ifdef _WIN32
static int load_overlay_dll(const char *dll_path, ManFn *man, int man_n, int dll) {
    HMODULE h = LoadLibraryA(dll_path);
    if (!h) {
        loader_log("LoadLibrary(%s) failed: %lu", dll_path, GetLastError());
        return 0;
    }
    /* ABI gate: reject any DLL whose contract ABI doesn't match this
     * runtime (pre-versioning DLLs lack the export entirely).  Delete the
     * stale file so the autocompile path regenerates it with the current
     * emitter. */
    typedef int (*AbiFn)(void);
    AbiFn abi_fn = (AbiFn)GetProcAddress(h, "overlay_abi");
    int abi = abi_fn ? abi_fn() : 0;
    /* Tag = ABI version (low 16) | codegen flavor (high 16). Mismatch on either
     * (wrong ABI, or a different-flavor cache e.g. widescreen vs base) is
     * rejected + deleted so autocompile regenerates it for THIS build. */
    if (abi != PSX_OVERLAY_ABI_TAG) {
        loader_log("ABI/flavor mismatch in %s: dll=0x%X runtime=0x%X — rejecting "
                   "and deleting stale cache entry", dll_path, abi,
                   PSX_OVERLAY_ABI_TAG);
        FreeLibrary(h);
        DeleteFileA(dll_path);
        return 0;
    }
    typedef void (*InitFn)(const OverlayCallbacks *);
    InitFn init_fn = (InitFn)GetProcAddress(h, "overlay_init");
    if (!init_fn) {
        loader_log("no overlay_init in %s", dll_path);
        FreeLibrary(h);
        return 0;
    }
    init_fn(&s_callbacks);
    OverlayFlushFn flush_fn = (OverlayFlushFn)GetProcAddress(h, "overlay_flush_cycles");
    if (!flush_fn || dll < 0 || dll >= (int)(sizeof(s_dll_flush) / sizeof(s_dll_flush[0]))) {
        loader_log("no ABI-v11 cycle flush export in %s", dll_path);
        FreeLibrary(h);
        return 0;
    }
    s_dll_flush[dll] = flush_fn;

    BYTE *base = (BYTE *)h;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt  = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY *exp_dd =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!exp_dd->VirtualAddress) {
        loader_log("no export dir in %s", dll_path);
        FreeLibrary(h);
        return 0;
    }
    IMAGE_EXPORT_DIRECTORY *exp =
        (IMAGE_EXPORT_DIRECTORY *)(base + exp_dd->VirtualAddress);
    DWORD *names    = (DWORD *)(base + exp->AddressOfNames);
    WORD  *ordinals = (WORD  *)(base + exp->AddressOfNameOrdinals);
    DWORD *funcs    = (DWORD *)(base + exp->AddressOfFunctions);

    int registered = 0;
    for (DWORD i = 0; i < exp->NumberOfNames; i++) {
        const char *name = (const char *)(base + names[i]);
        if (strncmp(name, "func_", 5) != 0) continue;
        if (strlen(name) != 13) continue;
        uint32_t addr = (uint32_t)strtoul(name + 5, NULL, 16);
        if (addr == 0) continue;

        WORD ord = ordinals[i];
        OverlayFn fn = (OverlayFn)(base + funcs[ord]);

        /* Precision-first (§1): only register a candidate when the recompiler
         * gave us its code ranges. A function with no manifest entry is left to
         * the interpreter rather than registered with a guessed extent. */
        ManFn *m = man_find(man, man_n, addr);
        if (!m || m->n == 0) { s_no_manifest++; continue; }
        cand_register(addr & 0x1FFFFFFFu, fn, m, dll);
        registered++;
    }
    loader_log("loaded %s -> %d candidates (%u no-manifest)",
               dll_path, registered, s_no_manifest);
    return registered;
}
#else
static int load_overlay_dll(const char *dll_path, ManFn *man, int man_n, int dll) {
    char error[256] = {0};
    void *h = psx_overlay_posix_library_open(dll_path, error, sizeof(error));
    if (!h) { loader_log("dlopen(%s) failed: %s", dll_path, error); return 0; }
    /* ABI gate (see the _WIN32 branch). */
    typedef int (*AbiFn)(void);
    AbiFn abi_fn = (AbiFn)psx_overlay_posix_library_symbol(h, "overlay_abi");
    int abi = abi_fn ? abi_fn() : 0;
    if (abi != PSX_OVERLAY_ABI_TAG) {
        loader_log("ABI/flavor mismatch in %s: dll=0x%X runtime=0x%X — rejecting "
                   "and deleting stale cache entry", dll_path, abi,
                   PSX_OVERLAY_ABI_TAG);
        psx_overlay_posix_library_close(h);
        remove_posix_dll_and_manifest(dll_path);
        return 0;
    }
    typedef void (*InitFn)(const OverlayCallbacks *);
    InitFn init_fn = (InitFn)psx_overlay_posix_library_symbol(h, "overlay_init");
    if (!init_fn) {
        loader_log("no overlay_init in %s", dll_path);
        psx_overlay_posix_library_close(h);
        return 0;
    }
    init_fn(&s_callbacks);
    OverlayFlushFn flush_fn = (OverlayFlushFn)psx_overlay_posix_library_symbol(
        h, "overlay_flush_cycles");
    if (!flush_fn || dll < 0 || dll >= (int)(sizeof(s_dll_flush) / sizeof(s_dll_flush[0]))) {
        loader_log("no ABI-v11 cycle flush export in %s", dll_path);
        psx_overlay_posix_library_close(h);
        return 0;
    }
    s_dll_flush[dll] = flush_fn;

    /* ELF/Mach-O do not expose a portable export-table walker. The range
     * manifest is already the loader's authority, so resolve only its exact
     * callable entries. Missing symbols stay interpreted rather than acquiring
     * a guessed extent. */
    int registered = 0;
    for (int i = 0; i < man_n; i++) {
        ManFn *m = &man[i];
        if (m->entry == 0 || m->n == 0) continue;
        OverlayFn fn = (OverlayFn)psx_overlay_posix_library_entry(h, m->entry);
        if (!fn) continue;
        cand_register(m->entry & 0x1FFFFFFFu, fn, m, dll);
        registered++;
    }
    loader_log("loaded %s -> %d manifest candidates", dll_path, registered);
    if (registered == 0) {
        s_dll_flush[dll] = NULL;
        psx_overlay_posix_library_close(h);
    }
    /* A successful handle intentionally stays open for process lifetime: the
     * registered function and flush pointers refer into it. Rescans are
     * idempotent through s_loaded_paths and never acquire a second reference. */
    return registered;
}
#endif

/* ---- Public API -------------------------------------------------------- */

void overlay_loader_init(const char *cache_dir, const char *game_id) {
    for (uint32_t p = 0; p < RANGE_PAGE_COUNT; p++) {
        s_range_page_head[p] = -1;
        s_range_page_tail[p] = -1;
    }
    s_range_link_n = 0;
    s_range_index_overflow = 0;
    for (uint32_t i = 0; i < RANGE_PC_CACHE_CAP; i++)
        s_range_pc_cache[i].cand = -1;
    strncpy(s_cache_dir, cache_dir, sizeof(s_cache_dir) - 1);
    strncpy(s_game_id,   game_id,   sizeof(s_game_id)   - 1);
    /* data shards persist under the same unified cache root (data_shards.c) */
    { extern void ds_init(const char*, const char*); ds_init(cache_dir, game_id); }
    init_callbacks();
    scan_cache_dir();
    overlay_image_warm_init();
    overlay_image_warm_seed_boot_text();
    /* (sljit removed 2026-07-15: the persisted-shard reload + off-thread compile
     * worker were started here. Overlay misses fall to the interpreter.) */
    /* Pre-seed native-block bisection list from PSX_NATIVE_BLOCK (comma/space
     * separated hex addrs). Lets us route init-time overlay functions through the
     * sanctioned dirty-RAM interpreter from the FIRST boot instruction — the
     * runtime overlay_native_block cmd can only be set post-boot, too late for
     * once-only init functions. Same diagnostic knob, just seedable at launch. */
    {
        const char *nb = getenv("PSX_NATIVE_BLOCK");
        if (nb && *nb) {
            const char *p = nb;
            while (*p) {
                while (*p == ' ' || *p == ',' || *p == '\t') p++;
                if (!*p) break;
                uint32_t a = (uint32_t)strtoul(p, NULL, 0);
                if (a) overlay_loader_native_block_add(a);
                while (*p && *p != ' ' && *p != ',' && *p != '\t') p++;
            }
            loader_log("PSX_NATIVE_BLOCK seeded %d native-block entr%s from '%s'",
                       s_native_block_n, s_native_block_n == 1 ? "y" : "ies", nb);
        }
    }
    /* Boot-time full-interp override (diagnostic): PSX_OVERLAY_NATIVE_OFF=1 forces
     * native overlay execution off from the first instruction, so a pristine
     * interpreter reference can be captured without racing post-boot cmds. Same
     * effect as the overlay_native_off cmd, but seeded at launch. */
    {
        const char *no = getenv("PSX_OVERLAY_NATIVE_OFF");
        if (no && *no && *no != '0') {
            s_native_exec = 0;
            loader_log("PSX_OVERLAY_NATIVE_OFF set: native overlay exec OFF from boot");
        }
    }
    {
        const char *sup = getenv("PSX_OVERLAY_IRQ_SUPPRESS");
        const char *rl  = getenv("PSX_OVERLAY_IRQ_RATELIMIT");
        if (sup && *sup && *sup != '0') {
            overlay_loader_set_irq_suppress(1, 0);
            loader_log("PSX_OVERLAY_IRQ_SUPPRESS set: native overlay IRQ checks suppressed from boot");
        } else if (rl && *rl) {
            uint32_t n = (uint32_t)strtoul(rl, NULL, 0);
            if (n == 0) n = 1;
            overlay_loader_set_irq_suppress(1, n);
            loader_log("PSX_OVERLAY_IRQ_RATELIMIT set: native overlay IRQ checks every %u call(s)", n);
        }
    }
    {
        const char *budget = getenv("PSX_OVERLAY_IRQ_BUDGET");
        if (budget && *budget) {
            uint32_t n = (uint32_t)strtoul(budget, NULL, 0);
            s_irq_budget_cycles = n;
            s_irq_last_check_cycle = UINT64_MAX;
            loader_log("PSX_OVERLAY_IRQ_BUDGET set: native overlay IRQ checks every %u guest cycle(s)", n);
        }
    }
    {
        const char *no_cd = getenv("PSX_OVERLAY_IRQ_NO_CDROM");
        if (no_cd && *no_cd && *no_cd != '0') {
            s_irq_suppress_cdrom_only = 1;
            loader_log("PSX_OVERLAY_IRQ_NO_CDROM set: native overlay CDROM-only IRQ checks suppressed");
        }
    }
    {
        const char *defer_cd = getenv("PSX_OVERLAY_IRQ_DEFER_CDROM");
        if (defer_cd && *defer_cd && *defer_cd != '0') {
            s_irq_defer_cdrom = 1;
            loader_log("PSX_OVERLAY_IRQ_DEFER_CDROM set: native overlay CDROM IRQ delivery deferred");
        }
    }
    {
        const char *post = getenv("PSX_OVERLAY_IRQ_POST_PUMP");
        if (post && *post && *post != '0') {
            s_irq_post_dispatch_pump = 1;
            s_irq_last_check_cycle = UINT64_MAX;
            loader_log("PSX_OVERLAY_IRQ_POST_PUMP set: native overlay dispatch-return IRQ pump enabled");
        }
    }
    {
        const char *diff = getenv("PSX_OVERLAY_DIFF");
        if (diff && *diff && *diff != '0') {
            s_diff_mode = 1;
            loader_log("PSX_OVERLAY_DIFF set: native/interp shadow diff ON from boot");
        }
    }
    s_active = 1;
}

/* (sljit removed 2026-07-15: overlay_loader_apply_live_policy resolved the
 * sljit live-execution policy once the Tier-2 backend was known. With the tier
 * gone there is no live policy to apply; the interpreter is the overlay-miss
 * floor unconditionally.) */

void overlay_loader_check_cache(uint32_t load_addr, uint32_t size,
                                const uint8_t *bytes) {
    /* DLL loading is deferred to the first dispatch miss (try_load_region). */
    (void)load_addr; (void)size; (void)bytes;
}

/* ---- Lazy region cache check (first dispatch miss for a region) -------- */

/* 64 -> 256 (2026-07-06): the dirty boot-text window adds per-variant regions
 * below FLOOR; a full memo silently stops caching new region checks. */
#define MAX_CHECKED 256
static uint32_t s_checked[MAX_CHECKED];
static int      s_nchecked = 0;
static int      s_last_file_found = 0;

static int already_checked(uint32_t region_start) {
    for (int i = 0; i < s_nchecked; i++)
        if (s_checked[i] == region_start) return 1;
    return 0;
}
static void mark_checked(uint32_t region_start) {
    if (s_nchecked < MAX_CHECKED) s_checked[s_nchecked++] = region_start;
}

/* Re-scan the cache dir for DLLs compiled after init (step 2.8 autocompile)
 * and clear the checked-regions memo so the next dispatch into a window
 * region reconsiders the cache. Already-loaded DLLs stay loaded;
 * dll_already_loaded() makes the re-walk idempotent. */
void overlay_loader_rescan(void) {
    if (!s_active) return;
    scan_cache_dir();
    s_nchecked = 0;
}

/* Loaded-DLL set — the cache is ADDITIVE: a memory slot reused by several
 * overlays (Tomba's village and overworld both at 0x800E7xxx) has one cached
 * DLL per distinct overlay (keyed by content crc in the filename). We load
 * ALL of them; each contributes its functions as separate candidates, and
 * per-entry validity (the live-RAM hash) decides which candidate is callable
 * at any moment. Nothing is ever clobbered — discoveries accumulate. */
/* The warmed vault and runtime-discovered variants can exceed the old 512-DLL
 * ceiling; once dll_already_loaded() lost track, a rescan could double-register
 * candidates. Match the cache-index capacity used by the loader. */
#define MAX_LOADED_DLLS 4096
static char s_loaded_paths[MAX_LOADED_DLLS][768];
static int  s_nloaded_paths = 0;

static int dll_already_loaded(const char *path) {
    for (int i = 0; i < s_nloaded_paths; i++)
        if (strcmp(s_loaded_paths[i], path) == 0) return 1;
    return 0;
}

static int load_one_dll(const char *dll_path) {
#ifdef _WIN32
    LARGE_INTEGER q0, q1, qf;
    QueryPerformanceCounter(&q0);
#endif
    /* Sibling code-range manifest: {base}_{crc}.ranges next to the DLL. */
    char ranges_path[800];
    snprintf(ranges_path, sizeof(ranges_path), "%s", dll_path);
    size_t plen = strlen(ranges_path);
    if (plen >= OVERLAY_SHARED_EXT_LEN &&
        strcmp(ranges_path + plen - OVERLAY_SHARED_EXT_LEN, OVERLAY_SHARED_EXT) == 0)
        snprintf(ranges_path + plen - OVERLAY_SHARED_EXT_LEN,
                 sizeof(ranges_path) - (plen - OVERLAY_SHARED_EXT_LEN), ".ranges");

    int man_n = 0;
    ManFn *man = parse_manifest(ranges_path, &man_n);
    if (!man || man_n == 0) {
        loader_log("no/empty manifest %s — DLL left to interpreter", ranges_path);
        free(man);
        return 0;
    }
    int registered = load_overlay_dll(dll_path, man, man_n, s_ndlls);
#ifdef _WIN32
    QueryPerformanceCounter(&q1);
    QueryPerformanceFrequency(&qf);
    uint64_t elapsed_us = qf.QuadPart > 0
        ? (uint64_t)((q1.QuadPart - q0.QuadPart) * 1000000LL / qf.QuadPart) : 0;
    s_load_last_us = elapsed_us;
    s_load_total_us += elapsed_us;
    if (elapsed_us > s_load_max_us) s_load_max_us = elapsed_us;
#endif
    free(man);
    if (registered <= 0) return 0;

    if (s_nloaded_paths < MAX_LOADED_DLLS) {
        strncpy(s_loaded_paths[s_nloaded_paths], dll_path, 767);
        s_loaded_paths[s_nloaded_paths][767] = '\0';
        s_nloaded_paths++;
    }
    s_ndlls++;
    return registered;
}

static int lazy_man_contains(const ManFn *m, uint32_t phys) {
    if ((m->entry & 0x1FFFFFFFu) == phys) return 1;
    for (int r = 0; r < m->n; r++) {
        uint32_t lo = m->lo[r] & 0x1FFFFFFFu;
        if (phys >= lo && phys < lo + m->len[r]) return 1;
    }
    return 0;
}

static uint32_t lazy_man_crc(const ManFn *m) {
    const uint8_t *ram = memory_get_ram_ptr();
    uint32_t crc = 0xFFFFFFFFu;
    for (int r = 0; r < m->n; r++)
        crc = crc32_update(crc, ram + (m->lo[r] & 0x1FFFFFFFu), m->len[r]);
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t lazy_man_gensum(const ManFn *m) {
    uint32_t sum = 0;
    for (int r = 0; r < m->n; r++)
        sum += overlay_watch_pagegen_sum(m->lo[r] & 0x1FFFFFFFu, m->len[r]);
    return sum;
}

static int lazy_man_matches(LazyMan *lm) {
    uint32_t gen = lazy_man_gensum(&lm->fn);
    if (lm->state == ENTRY_VALID && lm->val_gen == gen) return 1;
    if (lm->state == ENTRY_INVALID && lm->val_gen == gen) return 0;
    uint32_t live = lazy_man_crc(&lm->fn);
    lm->val_gen = gen;
    s_last_crc = live;
    lm->state = (live == lm->fn.crc) ? ENTRY_VALID : ENTRY_INVALID;
    return lm->state == ENTRY_VALID;
}

/* A CPS DLL can directly own internal tail targets, so one matching entry does
 * not prove the rest of that captured snapshot is current. Validate every
 * manifested function before preferring a broad bundle. This permits one
 * coherent snapshot to replace many synchronous incremental LoadLibrary calls
 * at a stage transition without substituting a stale CPS owner. */
static int lazy_bundle_matches(int ci) {
    if (ci < 0 || ci >= s_cache_idx_count) return 0;
    if (s_cache_idx[ci].func_count <= 0 ||
        s_cache_idx[ci].indexed_func_count != s_cache_idx[ci].func_count)
        return 0;
    int seen = 0;
    for (int li = s_lazy_bundle_head[ci]; li >= 0;
         li = s_lazy_man[li].next_bundle) {
        seen++;
        if (!lazy_man_matches(&s_lazy_man[li])) return 0;
    }
    return seen == s_cache_idx[ci].func_count;
}

/* A streamed variant is not byte-coherent until immediately before its first
 * call, too late to hide Windows' first image-map cost. Map every small bundle
 * for this ONE base after boot. Tomba's mature vault has 141 here versus 712 in
 * the rejected whole-cache preload. Mapping never registers a candidate; the
 * normal manifest/live-byte gate still decides whether it can execute. */
static void overlay_image_warm_seed_boot_text(void) {
    int indices[CACHE_IDX_CAP];
    int count = 0;
    for (int ci = 0; ci < s_cache_idx_count; ci++) {
        if (s_cache_idx[ci].region_start != DIRTY_RAM_KERNEL_WINDOW_END ||
            s_cache_idx[ci].func_count <= 0 ||
            s_cache_idx[ci].func_count > 8)
            continue;
        indices[count++] = ci;
    }
    overlay_image_warm_queue(indices, count);
}

static int lazy_is_loadable(int li, uint32_t region_start, uint32_t phys) {
    if (li < 0 || li >= s_lazy_man_n) return 0;
    LazyMan *lm = &s_lazy_man[li];
    int ci = lm->cache_idx;
    if (ci < 0 || ci >= s_cache_idx_count) return 0;
    return s_cache_idx[ci].region_start == region_start &&
        !dll_already_loaded(s_cache_idx[ci].path) &&
        lazy_man_contains(&lm->fn, phys) && lazy_man_matches(lm);
}

static int lazy_better_bundle(int li, int best) {
    (void)li;
    /* Candidate order is semantic for continuation-passing bundles: two DLLs
     * can contain a byte-identical entry while resolving its internal tail
     * targets against different region snapshots. Preserve the manifest index's
     * established order and publish only its first live match. Comparing bundle
     * size or timestamp can silently substitute an incompatible owner. */
    return best < 0;
}

static int lazy_load_selected(int li) {
    if (li < 0 || li >= s_lazy_man_n) return 0;
    int ci = s_lazy_man[li].cache_idx;
    if (ci < 0 || ci >= s_cache_idx_count ||
        dll_already_loaded(s_cache_idx[ci].path) ||
        s_cache_idx[ci].func_count <= 0) return 0;
    /* If proactive warming has not reached this fragment yet, prefer the
     * historical synchronous path over running a potentially hot function in
     * the interpreter. The worker drops its speculative reference safely. */
    overlay_image_warm_cancel(ci);
    s_last_file_found = 1;
    int loaded = load_one_dll(s_cache_idx[ci].path) > 0;
    overlay_image_warm_release(ci);
    return loaded;
}

static int try_load_region(uint32_t phys) {
    extern uint32_t dirty_ram_get_bitmap_word(uint32_t word_index);

    uint32_t page_sz = 4096u;

    /* Walk back over the contiguous dirty run to recover region_start — cache
     * DLLs are keyed by this start address in their filename.
     *
     * CRITICAL: the capture clamps each region to its WINDOW — kernel
     * [0, KERNEL_WINDOW_END), dirty boot-text [KERNEL_WINDOW_END, FLOOR), or
     * overlay [FLOOR, end) — so a DLL's region_start is the first dirty page
     * AT OR ABOVE the window floor. The walkback must apply the SAME clamp,
     * chosen by PAGE (the capture windows are page-granular and FLOOR need
     * not be page-aligned): an overlay-region walk stopping anywhere below
     * FLOOR's page, or a boot-text walk crossing into the kernel window,
     * yields a region_start that NO DLL filename matches — the overlay's
     * DLLs never load and its functions interpret forever. */
    uint32_t kern_pg = DIRTY_RAM_KERNEL_WINDOW_END / page_sz;
    uint32_t ovl_pg  = OVERLAY_REGION_FLOOR / page_sz;
    uint32_t pg = phys / page_sz;
    uint32_t floor_pg = (pg < kern_pg) ? 0u
                      : (pg < ovl_pg)  ? kern_pg
                      :                  ovl_pg;
    while (pg > floor_pg) {
        uint32_t pp = pg - 1;
        if (!((dirty_ram_get_bitmap_word(pp >> 5) >> (pp & 31u)) & 1u)) break;
        pg = pp;
    }
    uint32_t region_start = pg * page_sz;

    if (!already_checked(region_start)) mark_checked(region_start);

    /* Prefer the broadest fully coherent snapshot. If none has every function
     * live, retain the historical first matching-function fallback. */
    int best = -1;
    int fallback = -1;
    uint32_t bucket = (phys * 2654435761u) & LAZY_ENTRY_MASK;
    for (int li = s_lazy_entry_head[bucket]; li >= 0;
         li = s_lazy_man[li].next_entry) {
        if ((s_lazy_man[li].fn.entry & 0x1FFFFFFFu) != phys) continue;
        if (!lazy_is_loadable(li, region_start, phys)) continue;
        if (fallback < 0) fallback = li;
        int ci = s_lazy_man[li].cache_idx;
        if (lazy_bundle_matches(ci) &&
            (best < 0 || s_cache_idx[ci].func_count >
                         s_cache_idx[s_lazy_man[best].cache_idx].func_count))
            best = li;
    }

    uint32_t page = phys >> 12;
    /* An exact live manifest is sufficient and semantically stronger than an
     * enclosing range. Scanning every range link after finding one revalidated
     * hundreds of broad historical bundles at streamed-stage transitions. */
    if (fallback < 0 && page < RANGE_PAGE_COUNT) {
        for (int ri = s_lazy_page_head[page]; ri >= 0;
             ri = s_lazy_range_links[ri].next) {
            int li = s_lazy_range_links[ri].cand;
            if ((s_lazy_man[li].fn.entry & 0x1FFFFFFFu) == phys) continue;
            if (!lazy_is_loadable(li, region_start, phys)) continue;
            if (fallback < 0) fallback = li;
            int ci = s_lazy_man[li].cache_idx;
            if (lazy_bundle_matches(ci) &&
                (best < 0 || s_cache_idx[ci].func_count >
                             s_cache_idx[s_lazy_man[best].cache_idx].func_count))
                best = li;
        }
    }
    if (best >= 0 && lazy_load_selected(best)) return 1;
    return fallback >= 0 && lazy_load_selected(fallback);
}

/* O(1)-bucket discriminator between a real manifested function entry and an
 * interior CPS continuation. The latter must use its already-loaded range owner
 * first (the Whoopee 0x107624 fix); the former must be allowed to publish its
 * exact DLL even when a broader conservative owner contains the same PC. */
static int lazy_has_exact_entry(uint32_t phys) {
    /* Same overlay-off hazard as overlay_find_by_range: the lazy entry index
     * (s_lazy_entry_head) is -1-initialized only when overlay_cache is enabled;
     * with overlay off it is zero-initialized and the link traversal below
     * spins forever. Fail closed (no lazy entry) when the loader is inactive. */
    if (!s_active) return 0;
    uint32_t bucket = (phys * 2654435761u) & LAZY_ENTRY_MASK;
    for (int li = s_lazy_entry_head[bucket]; li >= 0;
         li = s_lazy_man[li].next_entry) {
        if ((s_lazy_man[li].fn.entry & 0x1FFFFFFFu) == phys)
            return 1;
    }
    return 0;
}

/* ---- Dispatch ---------------------------------------------------------- */

/* CPS (§25): find a non-blacklisted candidate whose code range CONTAINS phys —
 * i.e. phys is a continuation / return point INSIDE an overlay function, not its
 * registered entry. Under continuation-passing, an overlay function tail-
 * transfers after each block, so a callee returns to a mid-function address that
 * idx_head() (entry-keyed) misses; we re-enter the owning function with
 * cpu->pc = that address so its entry-switch routes to the right block. Returns
 * the candidate index, or -1. */
static int range_candidate_matches(int i, uint32_t phys) {
    Candidate *c = &s_cand[i];
    if (c->state == ENTRY_BLACKLIST) return 0;
    int contains = 0;
    for (int r = 0; r < c->nranges; r++) {
        uint32_t lo = c->range_lo[r];
        if (phys >= lo && phys < lo + c->range_len[r]) { contains = 1; break; }
    }
    if (!contains) return 0;

    /* A reused address can have several range-owning variants. Select the
     * one whose compiled code bytes match live RAM instead of returning the
     * first range hit and letting one stale variant mask every later match. */
    uint32_t gen = cand_gensum(c);
    if (c->state == ENTRY_VALID && gen == c->val_gen) {
        s_gen_fastpath++;
        return 1;
    }
    if (c->state == ENTRY_INVALID && gen == c->val_gen)
        return 0;                    /* known mismatch, no watched write */
    uint32_t live = cand_crc(c);
    s_rehashes++;
    s_last_crc = live;
    c->val_gen = gen;
    if (live == c->crc_code) {
        if (c->state != ENTRY_VALID) {
            c->state = ENTRY_VALID;
            s_valid_count++;
        }
        return 1;
    }
    s_rehash_miss++;
    if (c->state == ENTRY_VALID) {
        c->state = ENTRY_INVALID;
        s_invalidations++;
        if (s_valid_count > 0) s_valid_count--;
    } else {
        c->state = ENTRY_INVALID;
    }
    s_stale_blocked++;
    return 0;
}

static int overlay_find_by_range(uint32_t phys) {
    /* Definitive guard for the overlay-off case: the range page index
     * (s_range_page_head) is only initialized to -1 by overlay_loader_init,
     * which runs solely when overlay_cache is enabled. With overlay off it is
     * zero-initialized (0 = a valid index, not the -1 sentinel), so the link
     * traversal below never terminates. Fail closed (no candidate) when the
     * loader is inactive — there are no candidates to find anyway. */
    if (!s_active) return -1;
    uint32_t page = phys >> 12;
    if (page >= RANGE_PAGE_COUNT) return -1;

    RangePcCache *pc = &s_range_pc_cache[
        (phys * 2654435761u) & RANGE_PC_CACHE_MASK];
    if (pc->cand >= 0 && pc->phys == phys &&
        range_candidate_matches(pc->cand, phys))
        return pc->cand;

    if (s_range_index_overflow) {
        for (int i = 0; i < s_cand_n; i++)
            if (range_candidate_matches(i, phys)) {
                pc->phys = phys;
                pc->cand = i;
                return i;
            }
        return -1;
    }

    for (int li = s_range_page_head[page]; li >= 0; li = s_range_links[li].next) {
        int i = s_range_links[li].cand;
        if (range_candidate_matches(i, phys)) {
            pc->phys = phys;
            pc->cand = i;
            return i;
        }
    }
    return -1;
}

int overlay_loader_dispatch(CPUState *cpu, uint32_t addr) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    /* Overlay dispatch is a no-op when the overlay loader is inactive
     * (overlay_cache=false): there are no candidates to match, so this must
     * return 0 (dispatch to interp). This guard is also a HARD SAFETY:
     * overlay_loader_init() — the ONLY place the range index s_range_page_head[]
     * is initialized to -1 (empty) — runs only when overlay_cache is enabled
     * (main.cpp). With overlay off, that array stays zero-initialized (0 is a
     * VALID index, not the -1 sentinel), so the CPS continuation range lookup
     * below (gated on g_psx_cps_mode, independent of s_active) would traverse a
     * garbage index: s_range_page_head[page]=0 -> s_range_links[0].next=0 -> the
     * loop never terminates (infinite spin in range_candidate_matches). Every
     * overlay-off + CPS game hit this and wedged at boot before any game code ran
     * (found via Ape Escape, the only overlay-off title). Fail closed here. */
    if (!s_active) return 0;
    int lazy_loaded = 0;
retry_candidates:
    int head = idx_head(phys);
    int loaded_range_ci = -1;
    int lazy_exact = 0;
    int exact_needs_load = 0;
    if (head < 0 && s_active && overlay_cache_window_contains(phys)) {
        lazy_exact = head < 0 && lazy_has_exact_entry(phys);
        /* A CPS continuation is normally not a registered function ENTRY. Its
         * already-loaded range owner must be checked before lazy discovery:
         * try_load_region otherwise scans the 12K+ manifest index and may load
         * another duplicate bundle on every hot continuation edge. Whoopee's
         * 0x80107624 path paid that cost thousands of times per frame. Exact
         * manifested entries are different: a broader range owner must not mask
         * their valid cached DLL (Tomba FMV's 0x80106424/0x80106688 case). */
        if (head < 0 && g_psx_cps_mode)
            loaded_range_ci = overlay_find_by_range(phys);
        exact_needs_load = lazy_exact &&
            (loaded_range_ci < 0 || s_cand[loaded_range_ci].device_touch);
        if (head < 0 && (loaded_range_ci < 0 || exact_needs_load) &&
            !lazy_loaded && try_load_region(phys)) {
            lazy_loaded = 1;
            goto retry_candidates;
        }
        /* (sljit removed 2026-07-15: the JIT-on-miss gap-fill — off-thread
         * enqueue or synchronous compile — ran here. Misses fall to interp.) */
    }

    /* CPS (§25) continuation re-entry: phys is not an overlay ENTRY, but it may
     * be a return point inside an already-registered overlay function (the
     * caller's continuation after a tail-transferred call). Re-enter that
     * function natively with cpu->pc = addr so its entry-switch routes to the
     * continuation block — without this the continuation falls to the interp and
     * native coverage stops at the first call. gcc-DLL candidates are the trusted
     * tier (diff-validated at their entry; the continuation is the same DLL code),
     * so they run native directly here; device-touch funcs still go to interp. */
    if (head < 0 && g_psx_cps_mode) {
        int ci = loaded_range_ci >= 0 ? loaded_range_ci
                                      : overlay_find_by_range(phys);
        int _probe = (s_cps_probe_pc && phys == s_cps_probe_pc);
        if (_probe) {
            s_cps_probe_count++;
            s_cps_probe_ci = ci;
            s_cps_probe_ncand_inrange = overlay_count_by_range(phys);
            s_cps_probe_found = (ci >= 0) ? s_cand[ci].addr : 0;
            s_cps_probe_nrange = (ci >= 0) ? s_cand[ci].nranges : -1;
            s_cps_probe_matched = -1;
            s_cps_probe_outcome = (ci < 0) ? 0 : -1;
        }
        if (ci >= 0) {
            Candidate *c = &s_cand[ci];
            /* Generation-gated validation (overlay-cache v2 P2) — same contract
             * as the main chain: skip the crc32 when no watched code page changed
             * since this entry was last confirmed VALID. */
            uint32_t gen = cand_gensum(c);
            int matched;
            if (c->state == ENTRY_VALID && gen == c->val_gen) {
                matched = 1;
                s_gen_fastpath++;
            } else {
                uint32_t live = cand_crc(c);
                s_rehashes++;
                s_last_crc = live;
                c->val_gen = gen;
                matched = (live == c->crc_code);
            }
            if (_probe) s_cps_probe_matched = matched;
            if (matched) {
                if (c->state != ENTRY_VALID) { c->state = ENTRY_VALID; s_valid_count++; }
                if (c->device_touch)   { if (_probe) s_cps_probe_outcome = 3; s_disp_interp++; return 0; }
                /* Diff instrument — same contract as the entry chain's want_diff
                 * gate below. A continuation re-entry must NOT run native blind
                 * while its candidate is still inside the verify budget: CPS
                 * interiors dominate dispatch, so without this gate ~all native
                 * execution bypassed the diff harness (shadow_calls=1 over 51K
                 * frames while gen_fastpath took ~600M hits). Interiors are
                 * validated transitively: the entry-level shadow diff runs the
                 * WHOLE function natively (in_own_shadow below), so until the
                 * candidate passes its budget its interiors take the interpreter
                 * (the authority). Inside the candidate's OWN shadow native pass
                 * interiors DO run native — that IS the diff exercising the
                 * continuation blocks; the blocklist is not consulted there,
                 * matching the entry gate (which shadow-diffs blocked candidates
                 * too — the blocklist blocks LIVE native, not the sandbox). */
                int in_own_shadow = (s_in_shadow && (const void *)c == s_shadow_cand);
                if (!in_own_shadow) {
                    int want_diff = s_diff_mode;
                    if (want_diff && addr < 0x10000u) want_diff = 0;
                    if (want_diff && c->diff_passes < OVERLAY_DIFF_BUDGET) {
                        if (_probe) s_cps_probe_outcome = 5;
                        s_diffgate_interp++;
                        s_disp_interp++;
                        return 0;
                    }
                    if (!s_native_exec || overlay_native_blocked(c->addr) || overlay_native_blocked(addr))
                                           { if (_probe) s_cps_probe_outcome = 4; s_would_run_native++; s_disp_interp++; return 0; }
                }
                if (_probe) s_cps_probe_outcome = 2;
#ifndef PSX_NO_DEBUG_TOOLS
                uint32_t slot = s_nring_pos++ & (NRING_CAP - 1u);
                s_nring[slot].addr = addr;
                s_nring[slot].crc  = c->crc_code;
                s_nring[slot].frame = (uint32_t)s_frame_count;
                s_nring[slot].seq  = ++s_nring_seq;
                s_nring[slot].returned = 0;
                uint32_t prev_inprogress = s_native_inprogress;
                s_native_inprogress = c->addr;
#endif
                s_native_calls_total++;
                if (s_active_depth < (int)(sizeof(s_active_stack) / sizeof(s_active_stack[0])))
                    s_active_stack[s_active_depth++] = ci;
                s_disp_native++;
                cpu->pc = addr;          /* route the func's entry-switch to the block */
                {
                    int prev_phase = g_exec_phase;
                    OverlayFlushFn prev_flush = overlay_flush_enter(c);
                    g_exec_phase = 2;
                    c->fn(cpu);
                    overlay_flush_leave(prev_flush);
                    g_exec_phase = prev_phase;
                }
                overlay_post_dispatch_irq_pump(cpu);
                if (s_active_depth > 0) s_active_depth--;
#ifndef PSX_NO_DEBUG_TOOLS
                s_nring[slot].returned = 1;
                s_native_inprogress = prev_inprogress;
#endif
                if (g_native_bad_entry) {  /* foreign interior entry: fail closed to interp */
                    g_native_bad_entry = 0;
                    s_disp_native--; s_disp_interp++;
                    return 0;            /* cpu->pc was restored to the requested PC */
                }
                return 1;
            }
            if (_probe) s_cps_probe_outcome = 1;
            /* stale code bytes: fall through to the interpreter */
        }
    }

    for (int i = head; i >= 0; i = s_cand[i].next) {
        Candidate *c = &s_cand[i];
        if (c->state == ENTRY_BLACKLIST) continue;

        /* Generation-gated validation (overlay-cache v2 P2). The ONLY way this
         * entry's compiled code bytes can change is a write to one of its watched
         * code pages — and every CPU/DMA store funnels through the single store
         * chokepoint, which bumps overlay_page_gen for watched pages (memory.c).
         * So if the page-generation sum is unchanged since we last confirmed this
         * entry VALID, the bytes are PROVABLY unchanged and we run native without
         * re-hashing. We fall back to the full crc32 only when the generation
         * moved (or the entry isn't known-valid) — which also covers
         * reload-on-return: a write-away-then-back bumps the gen, forcing a
         * re-hash that re-confirms the byte-exact match before any native call.
         * This removes the per-dispatch crc32 of the whole function body from the
         * hot path for stable code (the warm-cache common case). Correctness is
         * identical to the old unconditional hash; only redundant hashing is cut. */
        uint32_t gen = cand_gensum(c);
        int matched;
        if (c->state == ENTRY_VALID && gen == c->val_gen) {
            matched = 1;                 /* no watched write since validation */
            s_gen_fastpath++;
        } else if (c->state == ENTRY_INVALID && gen == c->val_gen) {
            /* This exact byte identity already failed after the most recent
             * watched write. Re-hashing it on every dispatch makes reused
             * variant chains scale with cache history instead of live code. */
            continue;
        } else {
            uint32_t live = cand_crc(c);
            s_rehashes++;
            s_last_crc = live;
            c->val_gen = gen;
            matched = (live == c->crc_code);
        }
        if (matched) {
            if (c->state != ENTRY_VALID) {
                c->state = ENTRY_VALID;
                s_revalidations++;              /* reload-on-return */
                s_valid_count++;
            }
            /* Device-touching functions never run their shard: the shadow diff
             * can't safely double-execute MMIO/SIO/DMA to validate them, so they
             * always fall to the interpreter (the authoritative single path). */
            if (c->device_touch) { s_disp_interp++; return 0; }
            /* Same-state differential: run native+interp from identical state,
             * compare, keep the interp result. Takes precedence over the A/B
             * toggle. Verify-budget: once a candidate has passed cleanly enough
             * times it's trusted and falls through to normal execution, so the
             * diff cost stays bounded (a diverging candidate never reaches the
             * budget — it keeps being diff-gated and never runs native live).
             * gcc/DLL candidates are the trusted tier (validated at dev time) and
             * run native directly; they are diffed only in explicit dev diff mode
             * (PSX_OVERLAY_DIFF / overlay_diff cmd). */
            int want_diff = s_diff_mode;
            /* Kernel-window candidates (call gates 0xA0/0xB0/0xC0 and the RAM
             * kernel) are NOT shadow-diffable: the gates tail-jump via RAM
             * tables into kernel SERVICES whose behavior depends on scheduler/
             * event state and can block (TestEvent/WaitEvent) — the two passes
             * legitimately run different instruction paths and every call
             * "diverges" (observed: 2016/2017 calls, all 0xB0, flooding the
             * ring). The diff harness validates OVERLAY shard codegen against
             * the interp oracle; kernel gates stay on their normal route. */
            if (want_diff && addr < 0x10000u) want_diff = 0;
            if (want_diff && !s_in_shadow && c->diff_passes < OVERLAY_DIFF_BUDGET) {
                /* NEVER start a shadow inside an exception dispatch: the guest's
                 * ReturnFromException longjmps to the setjmp frame BELOW us
                 * (psx_check_interrupts), unwinding run_shadow_diff WITHOUT its
                 * epilogue — s_in_shadow/s_native_exec/s_suppress_irq stay stuck
                 * and the diff instrument is dead for the rest of the run
                 * (observed: shadow_calls=1 over 51K frames). Exceptions ENTERED
                 * during a shadow are contained — their setjmp is armed inside
                 * the shadow frame — so gating the START is structurally
                 * sufficient (every psx_exception_longjmp site is guarded by
                 * psx_get_in_exception()). The candidate still must not run
                 * native unvalidated: route to the interpreter (the authority);
                 * it gets diffed at its next non-exception dispatch. */
                extern int psx_get_in_exception(void);
                if (psx_get_in_exception()) {
                    s_diffgate_interp++;
                    s_disp_interp++;
                    return 0;
                }
                run_shadow_diff(cpu, c, addr);
                return 1;
            }

            /* A/B: prove whether native EXECUTION is the cause. When off, the
             * candidate matched (byte-exact) but we DON'T run native — interp
             * handles it. The per-function blocklist forces the same interp
             * routing for one function only (bisection localization). */
            if (!s_native_exec || overlay_native_blocked(c->addr))
                { s_would_run_native++; s_disp_interp++; return 0; }

            /* Record into the always-on ring BEFORE the call; mark in-progress
             * so a freeze inside this fn is visible at dump time. */
#ifndef PSX_NO_DEBUG_TOOLS
            uint32_t slot = s_nring_pos++ & (NRING_CAP - 1u);
            s_nring[slot].addr = c->addr;
            s_nring[slot].crc  = c->crc_code;
            s_nring[slot].frame = (uint32_t)s_frame_count;
            s_nring[slot].seq  = ++s_nring_seq;
            s_nring[slot].returned = 0;
            uint32_t prev_inprogress = s_native_inprogress;
            s_native_inprogress = c->addr;
#endif
            s_native_calls_total++;

            if (s_active_depth < (int)(sizeof(s_active_stack) / sizeof(s_active_stack[0])))
                s_active_stack[s_active_depth++] = i;
            s_disp_native++;
            /* Delimit this native execution in the interp insn ring (native code
             * emits no per-insn entries; markers keep the timeline alignable). */
#ifndef PSX_NO_DEBUG_TOOLS
            extern void dirty_ram_log_marker(uint32_t addr, uint32_t tag, int kind);
            uint32_t mtag = (uint32_t)s_nring[slot].seq;  /* stable across nesting */
            dirty_ram_log_marker(c->addr, mtag, 0);
#endif
            {
                int prev_phase = g_exec_phase;
                OverlayFlushFn prev_flush = overlay_flush_enter(c);
                g_exec_phase = 2;
                c->fn(cpu);
                overlay_flush_leave(prev_flush);
                g_exec_phase = prev_phase;
            }
            overlay_post_dispatch_irq_pump(cpu);
#ifndef PSX_NO_DEBUG_TOOLS
            dirty_ram_log_marker(c->addr, mtag, 1);
#endif
            if (s_active_depth > 0) s_active_depth--;

#ifndef PSX_NO_DEBUG_TOOLS
            s_nring[slot].returned = 1;
            s_native_inprogress = prev_inprogress;   /* restore (nested calls) */
#endif
            if (g_native_bad_entry) {  /* foreign interior entry: fail closed to interp */
                g_native_bad_entry = 0;
                s_disp_native--; s_disp_interp++;
                return 0;            /* cpu->pc was restored to the requested PC */
            }
            return 1;
        } else {
            s_rehash_miss++;
            if (c->state == ENTRY_VALID) {
                c->state = ENTRY_INVALID;
                s_invalidations++;
                if (s_valid_count > 0) s_valid_count--;
            } else {
                c->state = ENTRY_INVALID;
            }
            s_stale_blocked++;
        }
    }

    /* Publish at most one cached DLL and retry this same dispatch once. This
     * preserves additive variant coverage without turning one guest transition
     * into an unbounded synchronous LoadLibrary loop. */
    if (!lazy_loaded && s_active && overlay_cache_window_contains(phys) &&
        try_load_region(phys)) {
        lazy_loaded = 1;
        goto retry_candidates;
    }

    s_disp_interp++;
    return 0;
}

/* ---- Self-modification of an actively-executing entry (§8.5) ------------ */
/* Lazy re-hash on the NEXT dispatch is too late if a native function modifies
 * its own code and continues executing the modified bytes within the same
 * activation (native runs the originally-compiled semantics). We can't recover
 * the current activation, so we permanently demote that entry to interp. Called
 * from memory.c only when the written page is watched. */
void overlay_loader_active_write_check(uint32_t phys, uint32_t size) {
    extern uint32_t g_debug_last_store_pc;
    uint32_t p = phys & 0x1FFFFFFFu;
    for (int d = 0; d < s_active_depth; d++) {
        Candidate *c = &s_cand[s_active_stack[d]];
        for (int i = 0; i < c->nranges; i++) {
            uint32_t lo = c->range_lo[i];
            uint32_t hi = lo + c->range_len[i];
            if (p < hi && p + size > lo) {
                if (c->state != ENTRY_BLACKLIST) {
                    c->state = ENTRY_BLACKLIST;
                    s_selfmod++;
                    if (s_valid_count > 0) s_valid_count--;
                    s_last_write_pc   = g_debug_last_store_pc;
                    s_last_write_addr = phys;
                    s_last_write_size = size;
                    loader_log("blacklist self-mod entry 0x%08X (write 0x%08X)",
                               c->addr, phys);
                }
                break;
            }
        }
    }
}

/* ---- Status getters (signatures preserved for debug_server.c) ---------- */

void overlay_loader_get_counters(uint32_t *loads, uint32_t *invalidations,
                                 uint32_t *unregistered,
                                 uint64_t *disp_native, uint64_t *disp_interp,
                                 uint64_t *stale_blocked,
                                 uint32_t *last_write_pc,
                                 uint32_t *last_write_addr,
                                 uint32_t *last_write_size,
                                 int *regions, uint32_t *revalidations) {
    if (loads)           *loads           = (uint32_t)s_ndlls;
    if (invalidations)   *invalidations   = s_invalidations;
    if (unregistered)    *unregistered    = s_no_manifest;
    if (disp_native)     *disp_native     = s_disp_native;
    if (disp_interp)     *disp_interp     = s_disp_interp;
    if (stale_blocked)   *stale_blocked   = s_stale_blocked;
    if (last_write_pc)   *last_write_pc   = s_last_write_pc;
    if (last_write_addr) *last_write_addr = s_last_write_addr;
    if (last_write_size) *last_write_size = s_last_write_size;
    if (regions)         *regions         = s_ndlls;
    if (revalidations)   *revalidations   = s_revalidations;
}

void overlay_loader_get_load_timing(uint64_t *total_us, uint64_t *max_us,
                                    uint64_t *last_us) {
    if (total_us) *total_us = s_load_total_us;
    if (max_us) *max_us = s_load_max_us;
    if (last_us) *last_us = s_load_last_us;
}

/* Reload diagnostics. Repurposed for the per-entry model:
 *   r0_valid       -> candidates currently VALID
 *   r0_writes...   -> entries blacklisted (self-mod)
 *   r0_fn_lo       -> total candidates registered
 *   r0_fn_hi       -> DLLs loaded
 *   r0_crc_live    -> last computed code-range crc
 *   reval_attempts -> code-range hashes computed
 *   reval_crc_miss -> hashes that did not match
 *   last_reval_crc -> last computed crc                                    */
void overlay_loader_get_reload_debug(int *r0_valid, uint32_t *r0_writes,
                                     uint32_t *r0_fn_lo, uint32_t *r0_fn_hi,
                                     uint32_t *r0_crc_live,
                                     uint32_t *reval_attempts,
                                     uint32_t *reval_crc_miss,
                                     uint32_t *last_reval_crc) {
    if (r0_valid)       *r0_valid       = s_valid_count;
    if (r0_writes)      *r0_writes      = s_selfmod;
    if (r0_fn_lo)       *r0_fn_lo       = (uint32_t)s_cand_n;
    if (r0_fn_hi)       *r0_fn_hi       = (uint32_t)s_ndlls;
    if (r0_crc_live)    *r0_crc_live    = s_last_crc;
    if (reval_attempts) *reval_attempts = s_rehashes;
    if (reval_crc_miss) *reval_crc_miss = s_rehash_miss;
    if (last_reval_crc) *last_reval_crc = s_last_crc;
}

int overlay_loader_registered_count(void) { return s_valid_count; }

/* Dispatches that ran native via the unchanged-page-generation fast path,
 * i.e. skipped the per-dispatch code-range crc32 (overlay-cache v2 P2). High
 * values relative to reval_attempts mean the warm-cache hot path is cheap. */
uint64_t overlay_loader_gen_fastpath(void) { return s_gen_fastpath; }
int overlay_loader_range_link_count(void) { return s_range_link_n; }
int overlay_loader_range_index_overflow(void) { return s_range_index_overflow; }
int overlay_loader_lazy_manifest_count(void) { return s_lazy_man_n; }
int overlay_loader_lazy_manifest_overflow(void) { return s_lazy_man_overflow; }

/* (sljit removed 2026-07-15: overlay_loader_sljit_obsoleted and the
 * overlay_loader_sljit_probe one-shot JIT helper lived here.) */

/* ---- Native↔interp execution fingerprint differential (§5-E) ----------- */
/* For each CANDIDATE function execution we record the FULL register file at
 * entry and exit (plus the guest cycle), tagged native vs interp. Run once
 * native-OFF (all candidates interpreted = oracle) and once native-ON; diff by
 * sequence — the first entry whose in-state differs names the exact register
 * AND value where the two trajectories part ways, and the cycle field
 * quantifies native↔interp cycle-accounting skew over the aligned prefix.
 * Logging is purely additive (no control-flow change), driven from the single
 * dirty_ram_dispatch chokepoint. */
typedef struct {
    uint64_t seq;
    uint64_t cycle;          /* guest cycle at exit (log time)                 */
    uint32_t addr;
    uint32_t in_crc, out_crc;
    int      native;
    uint32_t in_regs[34];    /* r0..r31, hi, lo at entry                       */
    uint32_t out_regs[34];   /* r0..r31, hi, lo at exit                        */
} FpEnt;
#define FP_CAP (1u << 16)   /* ~19 MB with full reg files; ~65K executions     */
static FpEnt    s_fp[FP_CAP];
static uint64_t s_fp_seq = 0;

int overlay_loader_is_candidate(uint32_t phys) {
    if (!s_active) return 0;
    phys &= 0x1FFFFFFFu;
    return idx_head(phys) >= 0 || lazy_has_exact_entry(phys);
}

int overlay_fp_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("PSX_OVERLAY_FP_LOG");
        enabled = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return enabled;
}

/* ---- Same-state native↔interp differential (confident measurement) ------ */
/* At a matched dispatch: snapshot CPU+RAM, run native (discard), restore, run
 * interpreter (KEEP — game stays correct), compare under IDENTICAL input state.
 * Eliminates manual-nav desync. Interrupts suppressed during both shadow runs
 * so the comparison isolates COMPUTATION (and is longjmp-safe). A divergence
 * here = a real codegen bug (function + exact register/RAM). Zero divergence =
 * computation is correct and the fault is timing/interrupt-ordering. */
#define SHADOW_RAM_SIZE  (2u * 1024u * 1024u)
#define SHADOW_SPAD_SIZE 1024u
static uint8_t  s_ram0[SHADOW_RAM_SIZE];   /* pre-call main-RAM snapshot  */
static uint8_t  s_ramN[SHADOW_RAM_SIZE];   /* post-native main-RAM        */
static uint8_t  s_ramI[SHADOW_RAM_SIZE];   /* post-interp main-RAM (kept) */
static uint8_t  s_spad0[SHADOW_SPAD_SIZE]; /* pre-call scratchpad snapshot*/
static uint8_t  s_spadI[SHADOW_SPAD_SIZE]; /* post-interp scratchpad (kept)*/
static uint64_t s_shadow_skipped_dev = 0;  /* candidates skipped: touch MMIO */
/* s_diff_mode / s_in_shadow declared above (before dispatch). */

typedef struct {
    uint64_t seq; uint32_t addr;
    int      reg;          /* first differing gpr index, -1 none           */
    uint32_t reg_native, reg_interp;
    int      hi_diff, lo_diff;
    int64_t  ram_off;      /* first differing RAM byte, -1 none            */
    uint32_t ram_native, ram_interp;  /* the differing word               */
} ShadowDiv;
#define SDIV_CAP 512
static ShadowDiv s_sdiv[SDIV_CAP];
static int       s_sdiv_n = 0;
static uint64_t  s_shadow_calls = 0;
static uint64_t  s_shadow_divs  = 0;

/* One-shot full-state capture of the FIRST divergence: complete native and
 * interp register files so the diverging path can be localized. */
static int      s_detail_captured = 0;
static uint32_t s_detail_addr = 0;
static uint32_t s_detail_nat_gpr[32], s_detail_int_gpr[32];
static uint32_t s_detail_nat_hi, s_detail_nat_lo, s_detail_int_hi, s_detail_int_lo;

void overlay_loader_set_diff_mode(int on) { s_diff_mode = on ? 1 : 0; }

static void run_shadow_diff(CPUState *cpu, Candidate *c, uint32_t addr) {
    extern uint8_t *memory_get_ram_ptr(void);
    extern uint8_t *memory_get_scratchpad_ptr(void);
    extern int dirty_ram_dispatch(CPUState *cpu, uint32_t addr, uint32_t stop_addr);
    extern void psx_dispatch_call(CPUState *cpu, uint32_t addr, uint32_t return_addr);
    extern int      g_shadow_mmio_watch;   /* memory.c — device-access detector */
    extern uint64_t g_shadow_mmio_hits;
    uint8_t *ram  = memory_get_ram_ptr();
    uint8_t *spad = memory_get_scratchpad_ptr();

    extern uint64_t psx_exception_setjmp_epoch(void);
    s_in_shadow = 1;
    s_shadow_epoch = psx_exception_setjmp_epoch();
    int saved_supp = s_suppress_irq;
    s_suppress_irq = 1;                 /* isolate computation; longjmp-safe */
    s_shadow_saved_supp = saved_supp;   /* escape-fixup mirror (see decl) */
    /* Validate ONE function at a time: nested OVERLAY calls run via the
     * INTERPRETER on BOTH passes (s_native_exec=0). Otherwise the native pass
     * dispatches each callee as its own native shard while the interp pass runs
     * it interp, so the diff compares the whole call TREE and a callee that bails
     * or diverges is misattributed to this candidate (e.g. a contract bail that
     * skips THIS function's epilogue -> $sp off by the frame size). Per-function
     * isolation proves exactly this shard's codegen vs the interp oracle; whole-
     * tree soundness then follows by induction. */
    int sv = s_native_exec;
    s_native_exec = 0;
    s_shadow_saved_native_exec = sv;    /* escape-fixup mirror (see decl) */

    CPUState cpu0 = *cpu;
    memcpy(s_ram0,  ram,  SHADOW_RAM_SIZE);
    memcpy(s_spad0, spad, SHADOW_SPAD_SIZE);

    /* PASS 1 — INTERPRETER, the authoritative single execution, with the device
     * detector armed. Running interp FIRST (not native) guarantees device I/O
     * happens AT MOST ONCE and only via the trusted path: if this function (or a
     * callee) touches ANY MMIO we abandon the native pass entirely — device I/O
     * must never be double-executed (one spurious card/SIO/DMA write corrupts
     * hardware state and wedges the guest, e.g. the save-load crash). */
    uint64_t mmio0 = g_shadow_mmio_hits;
    g_shadow_mmio_watch++;
    dirty_ram_dispatch(cpu, addr, cpu->gpr[31]);
    g_shadow_mmio_watch--;
    s_shadow_calls++;

    if (g_shadow_mmio_hits != mmio0) {
        /* Device-touching: keep the interp result live (already in *cpu/ram/spad),
         * mark the candidate so it ALWAYS runs via the interpreter (never its
         * shard, never re-diffed). Device functions stay on the interpreter —
         * safe by construction, no double I/O. */
        c->device_touch = 1;
        s_shadow_skipped_dev++;
        g_psx_call_bail = 0;
        s_native_exec  = sv;
        s_suppress_irq = saved_supp;
        s_in_shadow    = 0;
        return;
    }

    /* Device-free: preserve the interp result, then run the NATIVE shard from the
     * same input and compare. No device I/O on either pass (proven clean above). */
    CPUState cpuI = *cpu;
    memcpy(s_ramI,  ram,  SHADOW_RAM_SIZE);
    memcpy(s_spadI, spad, SHADOW_SPAD_SIZE);

    *cpu = cpu0;
    memcpy(ram,  s_ram0,  SHADOW_RAM_SIZE);
    memcpy(spad, s_spad0, SHADOW_SPAD_SIZE);

    uint32_t stop_ra = cpu->gpr[31];   /* entry $ra = the function's return point */
    /* Arm the own-interior native route for the NATIVE pass only (see
     * s_shadow_cand decl): the candidate's CPS continuation re-entries run
     * native so the diff exercises every block of the function, not just the
     * first segment (nested CALLS still run interp on both passes —
     * s_native_exec=0 above). Never armed during the interp pass, which must
     * stay pure interp. */
    s_shadow_cand = c;
    {
        int prev_phase = g_exec_phase;
        OverlayFlushFn prev_flush = overlay_flush_enter(c);
        g_exec_phase = 2;
        c->fn(cpu);
        overlay_flush_leave(prev_flush);
        g_exec_phase = prev_phase;
    }
    /* CPS shards exit with cpu->pc set to the next tail target. Chain through
     * the normal dispatcher to the original caller return, with s_native_exec=0
     * above so nested overlay calls still run through the interpreter on both
     * passes. dirty_ram_dispatch alone cannot follow clean/static BIOS targets
     * and creates false shadow divergences for tail-transfer-heavy functions. */
    {
        int guard = 0;
        while (cpu->pc != 0 && !g_psx_call_bail && guard++ < 8192) {
            uint32_t tv = cpu->pc;
            if ((tv & 0x1FFFFFFFu) == (stop_ra & 0x1FFFFFFFu)) break;  /* returned */
            cpu->pc = 0;
            int prev_phase = g_exec_phase;
            g_exec_phase = 3;   /* compiled route; dirty/native callees re-tag inside */
            psx_dispatch_call(cpu, tv, stop_ra);
            g_exec_phase = prev_phase;
        }
    }
    s_shadow_cand = NULL;
    CPUState cpuN = *cpu;
    memcpy(s_ramN, ram, SHADOW_RAM_SIZE);

    /* Compare native (cpuN/s_ramN) vs interp (cpuI/s_ramI) under identical input. */
    int reg = -1;
    for (int r = 1; r < 32; r++) if (cpuN.gpr[r] != cpuI.gpr[r]) { reg = r; break; }
    int hidiff = (cpuN.hi != cpuI.hi), lodiff = (cpuN.lo != cpuI.lo);
    int64_t ramoff = -1;
    if (memcmp(s_ramN, s_ramI, SHADOW_RAM_SIZE) != 0) {
        for (uint32_t a = 0; a < SHADOW_RAM_SIZE; a++)
            if (s_ramN[a] != s_ramI[a]) { ramoff = (int64_t)a; break; }
    }
    if (reg < 0 && !hidiff && !lodiff && ramoff < 0) {
        /* Clean pass: credit the verify budget. */
        if (c->diff_passes < OVERLAY_DIFF_BUDGET) c->diff_passes++;
    } else {
        /* Divergence: reset the budget. Promotion to live requires N CONSECUTIVE
         * clean passes (the spec's "0 divergences over the budget"), so an
         * intermittently-wrong shard can never accumulate enough lucky passes to be
         * trusted — it stays diff-gated (interp result kept) and never runs live. */
        c->diff_passes = 0;
        s_shadow_divs++;
        if (!s_detail_captured) {
            s_detail_captured = 1;
            s_detail_addr = c->addr;
            for (int r = 0; r < 32; r++) {
                s_detail_nat_gpr[r] = cpuN.gpr[r];
                s_detail_int_gpr[r] = cpuI.gpr[r];
            }
            s_detail_nat_hi = cpuN.hi; s_detail_nat_lo = cpuN.lo;
            s_detail_int_hi = cpuI.hi; s_detail_int_lo = cpuI.lo;
        }
        /* Ring-flood guard: one persistently-diverging site must not evict
         * every other site's records (512x 0xB0 drowned the 0xF514 hunt).
         * Cap records per address; the divergence COUNTER still increments. */
        int addr_recs = 0;
        for (int i = 0; i < s_sdiv_n; i++)
            if (s_sdiv[i].addr == c->addr && ++addr_recs >= 16) break;
        if (s_sdiv_n < SDIV_CAP && addr_recs < 16) {
            ShadowDiv *d = &s_sdiv[s_sdiv_n++];
            d->seq = s_shadow_calls; d->addr = c->addr;
            d->reg = reg;
            d->reg_native = (reg >= 0) ? cpuN.gpr[reg] : 0;
            d->reg_interp = (reg >= 0) ? cpuI.gpr[reg] : 0;
            d->hi_diff = hidiff; d->lo_diff = lodiff;
            d->ram_off = ramoff;
            if (ramoff >= 0) {
                uint32_t a = (uint32_t)ramoff & ~3u;
                d->ram_native = *(uint32_t *)&s_ramN[a];
                d->ram_interp = *(uint32_t *)&s_ramI[a];
            }
        }
    }
    /* Restore the interp result as the authoritative live state (native discarded).
     * A bail raised by the shadow run must never leak into live execution (a
     * spurious in-progress unwind wedges the guest). */
    *cpu = cpuI;
    memcpy(ram,  s_ramI,  SHADOW_RAM_SIZE);
    memcpy(spad, s_spadI, SHADOW_SPAD_SIZE);
    g_psx_call_bail = 0;
    s_native_exec  = sv;
    s_suppress_irq = saved_supp;
    s_in_shadow    = 0;
}

/* Called by deferred_exception_longjmp() (interrupts.c) before it unwinds.
 * If the longjmp target frame predates a live shadow run (target_epoch <=
 * s_shadow_epoch), the unwind blows through run_shadow_diff and its epilogue
 * never runs — restore the shadow-scoped globals here and count the escape.
 * The dispatch gate (no shadow start while in an exception dispatch) makes
 * this structurally unreachable; if the counters ever move, that invariant is
 * broken and must be investigated. An escape during the NATIVE pass
 * (s_shadow_cand set) additionally means speculative native state leaked into
 * the live timeline — counted separately; it must stay 0. A longjmp to a
 * frame armed AFTER shadow start (target_epoch > s_shadow_epoch) is an
 * exception contained inside the shadow: it lands inside the shadow frame and
 * the shadow continues — the flags must NOT be touched. */
void overlay_loader_shadow_escape_fixup(uint64_t target_epoch) {
    /* A host unwind can skip a candidate call's normal epilogue. Commit any
     * cycles accumulated before the interrupt callback and clear the active
     * DLL store hook so later static/interpreted writes cannot address stale
     * execution context. */
    if (g_overlay_flush_pending_cycles) g_overlay_flush_pending_cycles();
    g_overlay_flush_pending_cycles = NULL;
    if (!s_in_shadow) return;
    if (target_epoch > s_shadow_epoch) return;   /* contained: leave armed */
    s_shadow_escapes++;
    if (s_shadow_cand != NULL) s_shadow_escapes_native++;
    s_in_shadow    = 0;
    s_shadow_cand  = NULL;
    s_native_exec  = s_shadow_saved_native_exec;
    s_suppress_irq = s_shadow_saved_supp;
}

int overlay_loader_dump_shadow_detail(char *out, int cap) {
    int n = 0;
    n += snprintf(out + n, cap - n,
        "{\"captured\":%d,\"addr\":\"0x%08X\",\"regs\":[", s_detail_captured, s_detail_addr);
    static const char *rn[32] = {"zero","at","v0","v1","a0","a1","a2","a3",
        "t0","t1","t2","t3","t4","t5","t6","t7","s0","s1","s2","s3","s4","s5",
        "s6","s7","t8","t9","k0","k1","gp","sp","fp","ra"};
    int first = 1;
    for (int r = 0; r < 32; r++) {
        if (s_detail_nat_gpr[r] == s_detail_int_gpr[r]) continue;
        n += snprintf(out + n, cap - n,
            "%s{\"r\":%d,\"name\":\"%s\",\"native\":\"0x%08X\",\"interp\":\"0x%08X\"}",
            first ? "" : ",", r, rn[r], s_detail_nat_gpr[r], s_detail_int_gpr[r]);
        first = 0;
    }
    n += snprintf(out + n, cap - n, "],\"hi\":{\"native\":\"0x%08X\",\"interp\":\"0x%08X\"},"
        "\"lo\":{\"native\":\"0x%08X\",\"interp\":\"0x%08X\"}}",
        s_detail_nat_hi, s_detail_int_hi, s_detail_nat_lo, s_detail_int_lo);
    return n;
}

int overlay_loader_dump_shadow(char *out, int cap) {
    int n = 0;
    n += snprintf(out + n, cap - n,
        "{\"diff_mode\":%d,\"shadow_calls\":%llu,\"divergences\":%llu,"
        "\"skipped_device\":%llu,\"interior_gated\":%llu,"
        "\"in_shadow\":%d,\"native_exec\":%d,"
        "\"escapes\":%u,\"escapes_native\":%u,\"records\":[",
        s_diff_mode, (unsigned long long)s_shadow_calls,
        (unsigned long long)s_shadow_divs,
        (unsigned long long)s_shadow_skipped_dev,
        (unsigned long long)s_diffgate_interp,
        s_in_shadow, s_native_exec,
        s_shadow_escapes, s_shadow_escapes_native);
    for (int i = 0; i < s_sdiv_n && n < cap - 200; i++) {
        ShadowDiv *d = &s_sdiv[i];
        n += snprintf(out + n, cap - n,
            "%s{\"seq\":%llu,\"addr\":\"0x%08X\",\"reg\":%d,"
            "\"reg_native\":\"0x%08X\",\"reg_interp\":\"0x%08X\","
            "\"hi\":%d,\"lo\":%d,\"ram_off\":%lld,"
            "\"ram_native\":\"0x%08X\",\"ram_interp\":\"0x%08X\"}",
            i ? "," : "", (unsigned long long)d->seq, d->addr, d->reg,
            d->reg_native, d->reg_interp, d->hi_diff, d->lo_diff,
            (long long)d->ram_off, d->ram_native, d->ram_interp);
    }
    n += snprintf(out + n, cap - n, "]}");
    return n;
}

/* Fingerprint over the general registers (r1..r31) + hi/lo. r0 excluded
 * (always 0). pc excluded (the return target is trivially equal at exit). */
uint32_t overlay_regs_crc(const CPUState *cpu) {
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, (const uint8_t *)&cpu->gpr[1], sizeof(uint32_t) * 31);
    crc = crc32_update(crc, (const uint8_t *)&cpu->hi, sizeof(uint32_t));
    crc = crc32_update(crc, (const uint8_t *)&cpu->lo, sizeof(uint32_t));
    return crc ^ 0xFFFFFFFFu;
}

/* Snapshot r0..r31 + hi/lo into a 34-word buffer (entry-state capture). */
void overlay_regs_snap(uint32_t out[34], const CPUState *cpu) {
    memcpy(out, cpu->gpr, sizeof(uint32_t) * 32);
    out[32] = cpu->hi;
    out[33] = cpu->lo;
}

/* CRC over words 1..33 (r0 excluded — always 0), matching overlay_regs_crc. */
static uint32_t regs34_crc(const uint32_t *r) {
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, (const uint8_t *)&r[1], sizeof(uint32_t) * 33);
    return crc ^ 0xFFFFFFFFu;
}

void overlay_fp_log(uint32_t addr, const uint32_t *in_regs,
                    const CPUState *cpu, int native) {
    extern uint64_t psx_get_cycle_count(void);
    uint64_t s = s_fp_seq++;
    FpEnt *e = &s_fp[s & (FP_CAP - 1u)];
    e->seq = s; e->cycle = psx_get_cycle_count();
    e->addr = addr & 0x1FFFFFFFu; e->native = native;
    memcpy(e->in_regs, in_regs, sizeof(e->in_regs));
    overlay_regs_snap(e->out_regs, cpu);
    e->in_crc  = regs34_crc(e->in_regs);
    e->out_crc = regs34_crc(e->out_regs);
}

/* Execute `addr` natively if a validated overlay candidate exists, keeping the
 * §5-E fingerprint record (same as the dirty_ram_dispatch chokepoint). Called
 * from the dirty-RAM interpreter's jal/jalr handlers so native overlay callees
 * get the SAME call contract as statically-compiled callees: execute as a
 * unit, then the interpreter resumes at the call's return address. Without
 * this, the call surfaces to the dispatch loop as a bare pc value; the native
 * callee's C-style return (pc==0) then unwinds the loop past the suspended
 * caller continuation — the caller's epilogue never runs and its stack frame
 * leaks (root cause of the dwarf->overworld native blue screen).
 * Returns 1 iff a native candidate ran. */
int overlay_loader_call_native(CPUState *cpu, uint32_t addr) {
    if (!s_active || !s_native_exec)
        return 0;  /* inactive/interp mode: keep the legacy inline path */
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (idx_head(phys) < 0 && !lazy_has_exact_entry(phys))
        return 0; /* neither a registered nor an exact cached entry */
    uint32_t in_regs[34];
    int fp = overlay_fp_enabled();
    if (fp) overlay_regs_snap(in_regs, cpu);
    /* Run the callee as an atomic UNIT: IRQ delivery is deferred inside it (both
     * the overlay CI wrappers and the dirty IRQ pumps check g_call_unit_depth) so
     * a cooperative ChangeThread cannot suspend the interrupted thread mid-callee
     * with an inconsistent (resume_pc, sp) snapshot. Restore (not just decrement)
     * so a bail/longjmp-out unwinds the depth correctly; the scheduler landing
     * also resets it to 0 as a backstop. Gated by the A/B toggle. */
    int prev_unit_depth = g_call_unit_depth;
    if (overlay_unit_defer_enabled()) g_call_unit_depth = prev_unit_depth + 1;
    int ran = overlay_loader_dispatch(cpu, addr);
    g_call_unit_depth = prev_unit_depth;
    if (!ran) return 0;
    if (fp) overlay_fp_log(addr, in_regs, cpu, 1);
    return 1;
}

/* (sljit removed 2026-07-15: the JIT-shard host helpers psx_sljit_call /
 * psx_sljit_cop2 / psx_sljit_memx lived here. They were called only by sljit-
 * emitted shards via the cpu->sljit_helpers table, which is no longer wired.) */

/* Write the whole fingerprint log to a file (no TCP size limit). Returns the
 * number of entries written, or -1 on open failure. */
int overlay_loader_write_fp_file(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    uint64_t total = s_fp_seq;
    uint64_t start = (total > FP_CAP) ? (total - FP_CAP) : 0;
    fputc('[', f);
    int first = 1, count = 0;
    for (uint64_t s = start; s < total; s++) {
        FpEnt *e = &s_fp[s & (FP_CAP - 1u)];
        fprintf(f,
            "%s{\"seq\":%llu,\"cycle\":%llu,\"addr\":\"0x%08X\",\"in\":\"0x%08X\","
            "\"out\":\"0x%08X\",\"native\":%d,\"in_regs\":[",
            first ? "" : ",\n", (unsigned long long)e->seq,
            (unsigned long long)e->cycle, e->addr,
            e->in_crc, e->out_crc, e->native);
        for (int r = 0; r < 34; r++)
            fprintf(f, "%s\"0x%08X\"", r ? "," : "", e->in_regs[r]);
        fputs("],\"out_regs\":[", f);
        for (int r = 0; r < 34; r++)
            fprintf(f, "%s\"0x%08X\"", r ? "," : "", e->out_regs[r]);
        fputs("]}", f);
        first = 0; count++;
    }
    fputs("]\n", f);
    fclose(f);
    return count;
}

/* Dump the native-call ring (most-recent first) + the in-progress entry. The
 * in_progress field being nonzero means a native function was entered and never
 * returned — a freeze INSIDE native code, pointing straight at the suspect. */
int overlay_loader_dump_native_ring(char *out, int cap) {
    int n = 0;
    n += snprintf(out + n, cap - n,
        "{\"native_exec\":%d,\"calls_total\":%llu,\"would_run\":%llu,"
        "\"in_progress\":\"0x%08X\",\"recent\":[",
        s_native_exec, (unsigned long long)s_native_calls_total,
        (unsigned long long)s_would_run_native, s_native_inprogress);
    /* Walk backward from the most recent entries kept in the diagnostic ring. */
    int shown = 0;
    for (uint32_t k = 0; k < NRING_CAP && n < cap - 140; k++) {
        uint32_t idx = (s_nring_pos - 1u - k) & (NRING_CAP - 1u);
        if (s_nring[idx].seq == 0) break;
        n += snprintf(out + n, cap - n,
            "%s{\"addr\":\"0x%08X\",\"crc\":\"0x%08X\",\"frame\":%u,\"seq\":%llu,\"returned\":%d}",
            shown ? "," : "", s_nring[idx].addr, s_nring[idx].crc, s_nring[idx].frame,
            (unsigned long long)s_nring[idx].seq, s_nring[idx].returned);
        shown++;
    }
    n += snprintf(out + n, cap - n, "]}");
    return n;
}

/* Diagnostic: dump every candidate with its stored vs live hash and generation
 * state, so reload behaviour can be inspected directly (Rule 3 — visibility via
 * the debug server, not logs). Writes a JSON array into `out`; returns bytes
 * written. */
int overlay_loader_dump_candidates(char *out, int cap) {
    int n = 0;
    n += snprintf(out + n, cap - n, "[");
    for (int i = 0; i < s_cand_n && n < cap - 160; i++) {
        Candidate *c = &s_cand[i];
        uint32_t live = cand_crc(c);
        uint32_t sum  = cand_gensum(c);
        n += snprintf(out + n, cap - n,
            "%s{\"addr\":\"0x%08X\",\"state\":%d,\"nranges\":%d,"
            "\"crc\":\"0x%08X\",\"live\":\"0x%08X\",\"match\":%d,"
            "\"val_gen\":%u,\"gen\":%u,\"dll\":%d,\"diff_passes\":%u}",
            i ? "," : "", c->addr, c->state, c->nranges,
            c->crc_code, live, (live == c->crc_code) ? 1 : 0,
            c->val_gen, sum, c->dll, c->diff_passes);
    }
    n += snprintf(out + n, cap - n, "]");
    return n;
}

/* Focused form for live miss diagnosis. The full candidate table can exceed the
 * debug command's response buffer once a game has accumulated many variants;
 * filtering by entry keeps every candidate at a reused PC visible. */
int overlay_loader_dump_candidates_at(uint32_t addr, char *out, int cap) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    int n = 0;
    int first = 1;
    n += snprintf(out + n, cap - n, "[");
    for (int i = 0; i < s_cand_n && n < cap - 180; i++) {
        Candidate *c = &s_cand[i];
        if (c->addr != phys) continue;
        uint32_t live = cand_crc(c);
        uint32_t sum  = cand_gensum(c);
        n += snprintf(out + n, cap - n,
            "%s{\"index\":%d,\"addr\":\"0x%08X\",\"state\":%d,\"nranges\":%d,"
            "\"crc\":\"0x%08X\",\"live\":\"0x%08X\",\"match\":%d,"
            "\"val_gen\":%u,\"gen\":%u,\"dll\":%d,\"diff_passes\":%u,"
            "\"device_touch\":%d}",
            first ? "" : ",", i, c->addr, c->state, c->nranges,
            c->crc_code, live, (live == c->crc_code) ? 1 : 0,
            c->val_gen, sum, c->dll, c->diff_passes, c->device_touch);
        first = 0;
    }
    n += snprintf(out + n, cap - n, "]");
    return n;
}

/* Focused view of manifest-only candidates that have not been loaded yet. */
int overlay_loader_dump_lazy_at(uint32_t addr, char *out, int cap) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    uint32_t pg = phys >> 12;
    uint32_t kern_pg = DIRTY_RAM_KERNEL_WINDOW_END >> 12;
    uint32_t ovl_pg = OVERLAY_REGION_FLOOR >> 12;
    uint32_t floor_pg = pg < kern_pg ? 0u : (pg < ovl_pg ? kern_pg : ovl_pg);
    extern uint32_t dirty_ram_get_bitmap_word(uint32_t word_index);
    while (pg > floor_pg) {
        uint32_t pp = pg - 1;
        if (!((dirty_ram_get_bitmap_word(pp >> 5) >> (pp & 31u)) & 1u)) break;
        pg = pp;
    }
    uint32_t recovered = pg << 12;
    uint32_t bucket = (phys * 2654435761u) & LAZY_ENTRY_MASK;
    int n = snprintf(out, cap, "{\"recovered_region\":\"0x%08X\",\"entries\":[",
                     recovered);
    int first = 1;
    for (int li = s_lazy_entry_head[bucket]; li >= 0 && n < cap - 320;
         li = s_lazy_man[li].next_entry) {
        LazyMan *lm = &s_lazy_man[li];
        if ((lm->fn.entry & 0x1FFFFFFFu) != phys) continue;
        int ci = lm->cache_idx;
        const char *base = strrchr(s_cache_idx[ci].path, '/');
        base = base ? base + 1 : s_cache_idx[ci].path;
        uint32_t live = lazy_man_crc(&lm->fn);
        n += snprintf(out + n, cap - n,
            "%s{\"li\":%d,\"ci\":%d,\"region\":\"0x%08X\","
            "\"file\":\"%s\",\"funcs\":%d,\"indexed\":%d,"
            "\"loaded\":%d,\"crc\":\"0x%08X\",\"live\":\"0x%08X\","
            "\"match\":%d,\"contains\":%d}",
            first ? "" : ",", li, ci, s_cache_idx[ci].region_start,
            base, s_cache_idx[ci].func_count, s_cache_idx[ci].indexed_func_count,
            dll_already_loaded(s_cache_idx[ci].path), lm->fn.crc, live,
            live == lm->fn.crc, lazy_man_contains(&lm->fn, phys));
        first = 0;
    }
    n += snprintf(out + n, cap - n, "]}");
    return n;
}

void overlay_loader_get_status(int *active, int *registered,
                               int *regions_checked,
                               char *cache_dir_out, int cache_dir_len,
                               char *game_id_out,   int game_id_len,
                               uint32_t *checked_out, int checked_max,
                               int *checked_written,
                               uint32_t *last_crc_out, int *last_file_found_out) {
    if (active)          *active          = s_active;
    if (registered)      *registered      = s_valid_count;
    if (regions_checked) *regions_checked = s_nchecked;
    if (cache_dir_out)   strncpy(cache_dir_out, s_cache_dir, (size_t)cache_dir_len - 1);
    if (game_id_out)     strncpy(game_id_out,   s_game_id,   (size_t)game_id_len   - 1);
    if (checked_out && checked_written) {
        int n = s_nchecked < checked_max ? s_nchecked : checked_max;
        for (int i = 0; i < n; i++) checked_out[i] = s_checked[i];
        *checked_written = n;
    }
    if (last_crc_out)        *last_crc_out        = s_last_crc;
    if (last_file_found_out) *last_file_found_out = s_last_file_found;
}
