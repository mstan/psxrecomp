/* memory.c — Phase 2 PS1 memory system.
 *
 * Physical address routing:
 *   0x00000000..0x001FFFFF — 2 MB RAM
 *   0x1F800000..0x1F8003FF — 1 KB scratchpad
 *   0x1F801000..0x1F803FFF — MMIO (fatal abort)
 *   0x1FC00000..0x1FC7FFFF — 512 KB BIOS ROM (read-only)
 *   Everything else         — fatal abort (unmapped)
 */

#include "cpu_state.h"
#include "cdrom.h"
#include "crash_trace.h"
#include "dma.h"
#include "fntrace.h"
#include "gpu.h"
#include "mdec.h"
#include "sio.h"
#include "spu.h"
#include "timers.h"
#include "lockstep.h"
#include "psx_cycles.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE        (2 * 1024 * 1024)
#define SCRATCHPAD_SIZE 1024
#define BIOS_ROM_SIZE   (512 * 1024)

static uint8_t ram[RAM_SIZE];
static uint8_t scratchpad[SCRATCHPAD_SIZE];
static uint8_t bios_rom[BIOS_ROM_SIZE];

/* Physical address translation for guest accesses. The 2 MB main RAM is
 * mirrored 4x across the first 8 MB of each segment (mem-ctrl RAM_SIZE
 * register, default 0x0B88) and games rely on it — Kula World's crt0
 * parks $sp in the 4th mirror (0x807FFFF8). Fold the mirrors here so the
 * RAM bounds checks below see canonical offsets; without this, mirror
 * writes fell into the open-bus no-op and mirror reads returned 0 (the
 * guest's stack silently vanished and $ra came back as 0). */
static inline uint32_t psx_phys_addr(uint32_t addr) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < 0x00800000u) phys &= (uint32_t)(RAM_SIZE - 1);
    return phys;
}

/* Expose RAM pointer for oracle comparison (find_first_divergence). */
uint8_t *memory_get_ram_ptr(void) { return ram; }
uint8_t *memory_get_scratchpad_ptr(void) { return scratchpad; }

void memory_clear_low_boot_scratch(void) {
    memset(ram, 0, 0x10u);
}

/* ---- Dirty-page tracking for install-at-runtime code (CLAUDE.md Rule 18) ----
 *
 * The PS1 BIOS writes 4-instruction dispatch stubs into kernel RAM at runtime
 * (e.g. RAM 0xCF0 for the SIO data-byte handler).  A static recompiler can't
 * see those instructions because they don't exist at compile time.  We track
 * which RAM pages have been written-to since boot and route any psx_dispatch
 * landing in such a page through a small MIPS interpreter (dirty_ram_interp.c).
 *
 * Granularity: 4 KB pages.  Ordinary CPU writes only mark the kernel-code
 * region (RAM 0..0xFFFF) where BIOS install stubs live.  CD-ROM DMA can also
 * mark arbitrary RAM ranges as executable candidates, which covers game
 * overlays loaded from disc without treating every data write as code.
 *
 * Future option (Option B, see docs/dynamic_handler_install.md): when a page
 * goes dirty, JIT-compile its bytes via StrictTranslator instead of running
 * an interpreter.  Pros: one source of MIPS semantics, hot install stubs run
 * as native compiled C.  Cons: gcc-at-runtime build dep, ~200 ms compile latency
 * stall on first dispatch, file I/O on hot path, cache-invalidation complexity,
 * Windows MinGW + dlopen friction.  Revisit only if install stubs become a
 * measurable hot path; today they're cold-path glue (~4k instructions per
 * directory-load is sub-microsecond to interpret). */
#define DIRTY_RAM_KERNEL_TRACK_BYTES 0x10000u
#define DIRTY_RAM_PAGE_SHIFT    12          /* 4 KB pages */
#define DIRTY_RAM_PAGE_COUNT    (RAM_SIZE >> DIRTY_RAM_PAGE_SHIFT)
#define DIRTY_RAM_BITMAP_WORDS  ((DIRTY_RAM_PAGE_COUNT + 31u) / 32u)
static uint32_t dirty_ram_bitmap[DIRTY_RAM_BITMAP_WORDS];

/* Monotonic generation for RAM-resident CODE changes (kernel install-stub
 * writes + DMA/EXE loads that mark executable ranges). Consumers that cache
 * per-PC classifications of RAM instructions (the interp's widescreen
 * cull/backdrop site caches) compare against this and re-derive after any
 * code change — a cached kind must never survive an overlay reload. */
uint32_t g_dirty_ram_code_gen = 1;

static inline void dirty_ram_mark_page(uint32_t phys) {
    if (phys >= RAM_SIZE) return;
    uint32_t page = phys >> DIRTY_RAM_PAGE_SHIFT;
    uint32_t bit = 1u << (page & 31u);
    /* Generation bumps on the clean->dirty TRANSITION only: kernel-window data
     * writes land here on every guest store, and bumping per write would
     * invalidate the per-PC site caches continuously. Overlay (re)loads always
     * bump via dirty_ram_mark_executable_range. */
    if (!(dirty_ram_bitmap[page >> 5] & bit)) g_dirty_ram_code_gen++;
    dirty_ram_bitmap[page >> 5] |= bit;
}

/* ---- Kernel-image bless -------------------------------------------------
 * Kernel Part 2 (RAM [0x500,0x8500)) is the BIOS's boot-time copy of ROM
 * [0x1FC10000,0x1FC18000); the statically-recompiled kernel functions in the
 * generated dispatch table were compiled FROM those ROM bytes. Those pages
 * are permanently dirty (TCB saves, event tables, install stubs share the
 * window), which diverted EVERY kernel dispatch to the interpreter — the
 * dominant cost of event-heavy scenes (a Tomba village frame samples ~97%
 * in kernel PCs) and CD-load drains.
 *
 * A dispatch key may run its static native function IFF the live RAM bytes
 * of everything that function can execute (its body extent, emitter-supplied
 * via psx_bios_kernel_bodies[]) still byte-match the ROM source. Per-entry,
 * lazily verified, cached; a guest write landing INSIDE an entry's body
 * drops that entry back to unverified (next dispatch re-compares). Entries
 * whose bodies were runtime-patched (the BIOS installs pad/SIO handlers into
 * ROM-zero gaps of the pad driver) simply never verify and stay on the
 * faithful interpreter path — their behavior is unchanged by design. This is
 * the runtime half of the relocation-manifest contract
 * (docs/RELOCATION_MANIFEST_FORMAT.md: "runtime verifies against live RAM
 * before dispatching the AOT function"). */
#define KBLESS_RAM_LO  0x500u
#define KBLESS_RAM_HI  0x8500u
#define KBLESS_ROM_OFF 0x10000u   /* bios_rom[] offset of RAM 0x500 */

typedef struct {
    uint32_t key;
    uint32_t body_lo;
    uint32_t body_hi;
} PsxKernelBody;
extern const PsxKernelBody psx_bios_kernel_bodies[];
extern const uint32_t      psx_bios_kernel_body_count;

#define KBLESS_UNKNOWN  0u
#define KBLESS_CLEAN    1u
#define KBLESS_MISMATCH 2u
#define KBLESS_MAX_ENTRIES 4096u
static uint8_t  kbless_state[KBLESS_MAX_ENTRIES];   /* parallel to bodies[] */
static int      kbless_enabled = -1;   /* env PSX_KERNEL_BLESS=0 disables */
/* Always-on counters (TCP kernel_bless). */
static uint64_t kbless_native_hits   = 0;
static uint64_t kbless_verifies      = 0;
static uint64_t kbless_mismatches    = 0;
static uint64_t kbless_invalidations = 0;

static int kbless_on(void) {
    if (kbless_enabled < 0) {
        const char* e = getenv("PSX_KERNEL_BLESS");
        kbless_enabled = (e && e[0] == '0') ? 0 : 1;
        if (psx_bios_kernel_body_count > KBLESS_MAX_ENTRIES) kbless_enabled = 0;
    }
    return kbless_enabled;
}

/* Binary search the (key-sorted) body table. -1 if absent. */
static int kbless_find(uint32_t phys) {
    uint32_t lo = 0, hi = psx_bios_kernel_body_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        uint32_t k = psx_bios_kernel_bodies[mid].key;
        if (k == phys) return (int)mid;
        if (k < phys) lo = mid + 1; else hi = mid;
    }
    return -1;
}

/* Dispatch-time query: may `phys` run its static native function?
 * Verifies lazily; every failure mode falls back to the interpreter. */
int psx_kernel_bless_dispatchable(uint32_t phys) {
    if (phys - KBLESS_RAM_LO >= (KBLESS_RAM_HI - KBLESS_RAM_LO)) return 0;
    if (!kbless_on()) return 0;
    int i = kbless_find(phys);
    if (i < 0) return 0;
    uint8_t st = kbless_state[i];
    if (st == KBLESS_CLEAN)    { kbless_native_hits++; return 1; }
    if (st == KBLESS_MISMATCH) return 0;
    const PsxKernelBody* b = &psx_bios_kernel_bodies[i];
    kbless_verifies++;
    if (memcmp(ram + b->body_lo,
               bios_rom + KBLESS_ROM_OFF + (b->body_lo - KBLESS_RAM_LO),
               b->body_hi - b->body_lo) == 0) {
        kbless_state[i] = KBLESS_CLEAN;
        kbless_native_hits++;
        return 1;
    }
    kbless_state[i] = KBLESS_MISMATCH;
    kbless_mismatches++;
    return 0;
}

/* A write landed in the kernel window: any entry whose body contains the
 * written byte must re-verify before its next native dispatch. Kernel DATA
 * writes (TCB saves, event tables — the frequent case) lie outside every
 * body and fall through the loop without invalidating anything; genuine
 * code-window writes (install-time, patches) are rare. */
static void kbless_note_write(uint32_t phys) {
    if (phys - KBLESS_RAM_LO >= (KBLESS_RAM_HI - KBLESS_RAM_LO)) return;
    if (kbless_enabled == 0) return;
    uint32_t n = psx_bios_kernel_body_count;
    if (n > KBLESS_MAX_ENTRIES) return;
    for (uint32_t i = 0; i < n; i++) {
        /* No early exit: a continuation key can sort ABOVE the written
         * address while its parent body contains it (bodies overlap). n is
         * a few hundred; this path fires only on kernel-window writes. */
        const PsxKernelBody* b = &psx_bios_kernel_bodies[i];
        if (phys >= b->body_lo && phys < b->body_hi &&
            kbless_state[i] != KBLESS_UNKNOWN) {
            kbless_state[i] = KBLESS_UNKNOWN;
            kbless_invalidations++;
        }
    }
}

/* Range write (DMA / EXE load / savestate restore) overlapping the window:
 * bulk, rare events — reset every entry rather than per-byte scanning. */
void psx_kernel_bless_note_range(uint32_t phys, uint32_t len) {
    if (len == 0) return;
    uint32_t end = phys + len;
    if (end < phys) end = 0xFFFFFFFFu;
    if (end <= KBLESS_RAM_LO || phys >= KBLESS_RAM_HI) return;
    uint32_t n = psx_bios_kernel_body_count;
    if (n > KBLESS_MAX_ENTRIES) return;
    for (uint32_t i = 0; i < n; i++) {
        if (kbless_state[i] != KBLESS_UNKNOWN) {
            kbless_state[i] = KBLESS_UNKNOWN;
            kbless_invalidations++;
        }
    }
}

void psx_kernel_bless_stats(uint64_t out[6]) {
    uint32_t n = psx_bios_kernel_body_count;
    uint32_t clean = 0, mism = 0;
    if (n > KBLESS_MAX_ENTRIES) n = KBLESS_MAX_ENTRIES;
    for (uint32_t i = 0; i < n; i++) {
        if (kbless_state[i] == KBLESS_CLEAN)    clean++;
        if (kbless_state[i] == KBLESS_MISMATCH) mism++;
    }
    out[0] = psx_bios_kernel_body_count;
    out[1] = clean;
    out[2] = mism;
    out[3] = kbless_native_hits;
    out[4] = kbless_verifies;
    out[5] = kbless_invalidations;
}

static inline void dirty_ram_mark_kernel_write(uint32_t phys) {
    if (phys >= DIRTY_RAM_KERNEL_TRACK_BYTES) return;
    dirty_ram_mark_page(phys);
    kbless_note_write(phys);
}

int dirty_ram_is_dirty(uint32_t phys);

/* Registered boot-EXE reference image and its modification state.  Declared
 * before dirty_ram_clear_image_baseline because that handoff clears the writes
 * performed by the BIOS loader itself. */
static uint8_t *text_ref_image = NULL;
static uint32_t text_ref_lo = 0, text_ref_hi = 0;
static uint32_t text_modified_bitmap[DIRTY_RAM_BITMAP_WORDS];
static uint32_t text_diverged_bitmap[DIRTY_RAM_BITMAP_WORDS];
static uint64_t g_text_native_blocked = 0;
static uint32_t g_text_diverged_pages = 0;

/* Establish the clean compiled-image baseline for the game-EXE text region.
 * Called ONCE when the game entry is first reached (fntrace game-start): by then
 * the BIOS has fully loaded the boot EXE into [0x10000, FLOOR) — which IS the
 * compiled image — but no gameplay overlay has run yet. The EXE load (CD DMA via
 * dirty_ram_mark_executable_range) marks the whole text dirty as a FALSE POSITIVE
 * (RAM == compiled image); clearing it here means dirty_ram_is_dirty() afterwards
 * is true ONLY for pages a later overlay actually overwrote. The dispatch can then
 * trust a clean text page to run its compiled function and divert only truly-
 * overlaid pages to the interpreter (Tomba 2 loads a loader overlay over
 * 0x8001Dxxx). The kernel window [0,0x10000) (BIOS install stubs) and the overlay
 * region [FLOOR, RAM) are left untouched. */
extern uint32_t g_overlay_region_floor;
void dirty_ram_clear_image_baseline(void) {
    uint32_t floor = g_overlay_region_floor;
    if (floor <= DIRTY_RAM_KERNEL_TRACK_BYTES) return;
    if (floor > RAM_SIZE) floor = RAM_SIZE;
    uint32_t first_page = DIRTY_RAM_KERNEL_TRACK_BYTES >> DIRTY_RAM_PAGE_SHIFT;
    uint32_t last_page  = (floor - 1u) >> DIRTY_RAM_PAGE_SHIFT;
    for (uint32_t page = first_page; page <= last_page; page++) {
        dirty_ram_bitmap[page >> 5] &= ~(1u << (page & 31u));
        /* The guarded writes that loaded the original EXE are also baseline,
         * not runtime modifications.  Forget them here so pristine code does
         * not pay a byte-compare on every dispatch. */
        text_modified_bitmap[page >> 5] &= ~(1u << (page & 31u));
        text_diverged_bitmap[page >> 5] &= ~(1u << (page & 31u));
    }
}

/* Text-image divergence guard.
 *
 * dirty_ram_is_dirty() is intentionally coarse: it says a page was touched by
 * runtime code loading. For deciding whether the original static game dispatch
 * is still safe, a dirty page is not enough information: data in a code page can
 * be written without changing the instructions at a target, while packed games
 * can also rewrite their own text with completely different instructions.
 *
 * main.cpp registers the boot EXE bytes as the reference image. Writes inside
 * that range mark pages whose bytes differ from the reference. At dispatch time,
 * a modified page is still allowed to use the static native entry if the code
 * bytes at the target match the original image; otherwise it is sticky-diverged
 * and must execute from live RAM through the dirty interpreter. */
/* Mutable: the runtime owns this heap buffer (main.cpp mallocs the EXE image and
 * never frees it). Intentional data patches by the translation layer are blessed
 * into it via dirty_ram_text_bless so they are not mistaken for self-modifying
 * code — see dirty_ram_text_native_ok / dirty_ram_text_bless. */
void dirty_ram_register_text_image(uint32_t phys_lo, const uint8_t *bytes,
                                   uint32_t len) {
    if (!bytes || len == 0 || phys_lo >= RAM_SIZE) return;
    if (len > RAM_SIZE - phys_lo) len = RAM_SIZE - phys_lo;
    text_ref_image = (uint8_t *)bytes;  /* runtime-owned mutable heap buffer */
    text_ref_lo = phys_lo;
    text_ref_hi = phys_lo + len;
    memset(text_modified_bitmap, 0, sizeof(text_modified_bitmap));
    memset(text_diverged_bitmap, 0, sizeof(text_diverged_bitmap));
    g_text_native_blocked = 0;
    g_text_diverged_pages = 0;
}

int dirty_ram_text_image_registered(void) { return text_ref_image != NULL; }

static inline void text_guard_note_write(uint32_t phys, uint32_t val, int size) {
    if (!text_ref_image) return;
    if (phys < text_ref_lo || phys + (uint32_t)size > text_ref_hi) return;
    const uint8_t *ref = text_ref_image + (phys - text_ref_lo);
    uint8_t buf[4] = { (uint8_t)val, (uint8_t)(val >> 8),
                       (uint8_t)(val >> 16), (uint8_t)(val >> 24) };
    if (memcmp(ref, buf, (size_t)size) != 0) {
        uint32_t page = phys >> DIRTY_RAM_PAGE_SHIFT;
        text_modified_bitmap[page >> 5] |= (1u << (page & 31u));
    }
}

int dirty_ram_text_native_ok(uint32_t phys) {
    if (!text_ref_image || phys < text_ref_lo || phys >= text_ref_hi)
        return !dirty_ram_is_dirty(phys);

    uint32_t page = phys >> DIRTY_RAM_PAGE_SHIFT;
    uint32_t bit = 1u << (page & 31u);
    if (text_diverged_bitmap[page >> 5] & bit) {
        g_text_native_blocked++;
        return 0;
    }
    /* Fast path: a page never touched by a guarded write AND not runtime-dirty
     * still holds the pristine compiled image — native is safe with no compare. */
    if (!(text_modified_bitmap[page >> 5] & bit) && !dirty_ram_is_dirty(phys))
        return 1;

    /* Flagged (a guarded write that differed, an overlay-dirty page, or an
     * intentional runtime data patch): the compiled native code is valid IFF the
     * bytes at the entry still match the reference image. Decide by the ACTUAL
     * bytes, not the page-dirty heuristic — so a data-only change in a page that
     * also holds code (e.g. a translation string table blessed into the ref via
     * dirty_ram_text_bless) does not needlessly block a still-valid function and
     * route it to psx_unknown_dispatch. A genuine code overwrite still diverges. */
    uint32_t n = 256;
    if (n > text_ref_hi - phys) n = text_ref_hi - phys;
    if (memcmp(ram + phys, text_ref_image + (phys - text_ref_lo), n) == 0)
        return 1;

    text_diverged_bitmap[page >> 5] |= bit;
    g_text_diverged_pages++;
    g_text_native_blocked++;
    return 0;
}

/* Validate an exact generated-code interval against the original EXE image.
 * Unlike dirty_ram_text_native_ok(), this deliberately ignores mutable bytes
 * elsewhere on the same page.  The generated game dispatcher calls it for all
 * CFG instruction ranges owned by a function before entering native code. */
int dirty_ram_text_range_native_ok(uint32_t phys_lo, uint32_t len) {
    if (len == 0) return 1;
    if (!text_ref_image || phys_lo < text_ref_lo ||
        phys_lo >= text_ref_hi || len > text_ref_hi - phys_lo)
        return 0;

    uint32_t phys_hi = phys_lo + len;
    uint32_t first_page = phys_lo >> DIRTY_RAM_PAGE_SHIFT;
    uint32_t last_page = (phys_hi - 1u) >> DIRTY_RAM_PAGE_SHIFT;
    for (uint32_t page = first_page; page <= last_page; ++page) {
        uint32_t bit = 1u << (page & 31u);
        if (!(text_modified_bitmap[page >> 5] & bit) &&
            !dirty_ram_is_dirty(page << DIRTY_RAM_PAGE_SHIFT))
            continue;

        uint32_t lo = phys_lo;
        uint32_t page_lo = page << DIRTY_RAM_PAGE_SHIFT;
        uint32_t page_hi = page_lo + (1u << DIRTY_RAM_PAGE_SHIFT);
        if (lo < page_lo) lo = page_lo;
        uint32_t hi = phys_hi < page_hi ? phys_hi : page_hi;
        if (memcmp(ram + lo, text_ref_image + (lo - text_ref_lo), hi - lo) != 0) {
            g_text_native_blocked++;
            return 0;
        }
    }
    return 1;
}

/* Bless an INTENTIONAL data patch into the reference image so the text-divergence
 * guard does not mistake it for self-modifying code. The runtime's own
 * translation layer (text_xlate) patches string/glyph tables that share 4 KB
 * pages — and the guard's 256-byte compare window — with real compiled functions.
 * Without this, patching a data table diverges the page and blocks native
 * dispatch of every function in it (a real function fatally routes to
 * psx_unknown_dispatch). By updating text_ref_image to match the patched bytes,
 * the compare passes for the intentional change while a genuine game write of
 * DIFFERENT bytes still diverges and is still caught. Clears the sticky diverged
 * bit for affected pages so the guard re-evaluates against the updated reference.
 * Byte-for-byte identical writes are a no-op. */
void dirty_ram_text_bless(uint32_t phys, const uint8_t *bytes, uint32_t len) {
    if (!text_ref_image || !bytes || len == 0) return;
    if (phys >= text_ref_hi || phys + len <= text_ref_lo) return;   /* out of range */
    uint32_t lo = phys < text_ref_lo ? text_ref_lo : phys;
    uint32_t hi = phys + len > text_ref_hi ? text_ref_hi : phys + len;
    if (hi <= lo) return;
    uint8_t *ref = text_ref_image + (lo - text_ref_lo);
    const uint8_t *src = bytes + (lo - phys);
    if (memcmp(ref, src, hi - lo) == 0) return;                     /* already in sync */
    memcpy(ref, src, hi - lo);
    /* Re-open the affected pages: clear the sticky diverged bit so the next
     * dispatch re-runs the compare against the now-updated reference. */
    uint32_t first_page = lo >> DIRTY_RAM_PAGE_SHIFT;
    uint32_t last_page  = (hi - 1u) >> DIRTY_RAM_PAGE_SHIFT;
    for (uint32_t page = first_page; page <= last_page; page++)
        text_diverged_bitmap[page >> 5] &= ~(1u << (page & 31u));
}

uint64_t dirty_ram_text_native_blocked(void) { return g_text_native_blocked; }
uint32_t dirty_ram_text_diverged_pages(void) { return g_text_diverged_pages; }

void dirty_ram_mark_executable_range(uint32_t phys, uint32_t len) {
    if (len == 0 || phys >= RAM_SIZE) return;
    psx_kernel_bless_note_range(phys, len);
    uint32_t end = phys + len - 1u;
    if (end >= RAM_SIZE || end < phys) end = RAM_SIZE - 1u;

    uint32_t first_page = phys >> DIRTY_RAM_PAGE_SHIFT;
    uint32_t last_page = end >> DIRTY_RAM_PAGE_SHIFT;
    for (uint32_t page = first_page; page <= last_page; page++) {
        dirty_ram_bitmap[page >> 5] |= (1u << (page & 31u));
    }
    g_dirty_ram_code_gen++;
}

/* Force-interp mode (tooling): PSX_FORCE_INTERP=1 makes ALL RAM above the kernel
 * window report dirty, so every dispatch into game/overlay text routes to the
 * dirty-RAM interpreter (the SAME path overlays take) instead of the compiled
 * image. Interp-path Δ-ruler enabler: lets the cyctest isolation loops be measured
 * native-INTERP vs Beetle. Consulted by every routing site that already calls
 * dirty_ram_is_dirty (top dispatch, dirty_ram_dispatch_inner gates, the
 * psx_dispatch_game_compiled gates) — no emitter/dispatch change needed. */
static int dirty_ram_force_interp(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("PSX_FORCE_INTERP"); s = (e && e[0] && e[0] != '0'); }
    return s;
}

/* DIAGNOSTIC ONLY (class-B reproduction, NOT a fix — Rule -1): PSX_SHELLWIN_INTERP=1
 * reports the BIOS shell-copy relocated RAM window [0x30000, 0x5AFFF] as dirty, so
 * the compiled dispatch (full_function_emitter.cpp:1380) routes those addresses
 * through the recovering dirty-RAM interp instead of normalize()->shell ROM. This
 * peels the class-A shell-window pc=0 wedge (func_1FC42090) so the *general*
 * class-B compiled exception-return pc=0 (in [0x5B000, 0x8F000)) can surface and be
 * captured. Must be reverted before any merge; it is a probe, not the fix. */
static int dirty_ram_shellwin_interp(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("PSX_SHELLWIN_INTERP"); s = (e && e[0] && e[0] != '0'); }
    return s;
}

int dirty_ram_is_dirty(uint32_t phys) {
    if (phys >= RAM_SIZE) return 0;
    if (dirty_ram_force_interp() && phys >= DIRTY_RAM_KERNEL_TRACK_BYTES) return 1;
    if (dirty_ram_shellwin_interp() && phys >= 0x00030000u && phys <= 0x0005AFFFu) return 1;
    uint32_t page = phys >> DIRTY_RAM_PAGE_SHIFT;
    return (dirty_ram_bitmap[page >> 5] >> (page & 31u)) & 1u;
}

uint32_t dirty_ram_get_bitmap(void) { return dirty_ram_bitmap[0]; }

uint32_t dirty_ram_get_bitmap_word(uint32_t word_index) {
    if (word_index >= DIRTY_RAM_BITMAP_WORDS) return 0;
    return dirty_ram_bitmap[word_index];
}

uint32_t dirty_ram_get_bitmap_word_count(void) {
    return DIRTY_RAM_BITMAP_WORDS;
}

void dirty_ram_set_bitmap_words(const uint32_t* words, uint32_t count) {
    if (count > DIRTY_RAM_BITMAP_WORDS) count = DIRTY_RAM_BITMAP_WORDS;
    for (uint32_t i = 0; i < count; i++)
        dirty_ram_bitmap[i] = words[i];
}

/* ---- Inc3: watched overlay pages + per-page generation counters ---------
 * Pages covered by a registered overlay function's code range. The store path
 * (the single, audited RAM-write chokepoint — all CPU + DMA stores funnel
 * here) tests the watch bitmap and, on a hit, bumps that page's generation
 * counter. It does NOT eagerly invalidate (Inc1-D did): validity is now decided
 * lazily, per compiled entry, at dispatch time (overlay_loader.c §8). The
 * generation counter lets the loader cheaply detect "did any page covering this
 * entry's code change since I last validated it?" without hashing on the store
 * path. Monotonic: gen only increases, so a sum over an entry's pages is a
 * perfect change detector (no aliasing).
 *
 * The bitmap is almost always empty, so the per-store cost on the common path
 * is a single bitmap lookup.
 */
static uint32_t overlay_watch_bitmap[DIRTY_RAM_BITMAP_WORDS];
static uint32_t overlay_page_gen[DIRTY_RAM_PAGE_COUNT];

void overlay_watch_set_range(uint32_t phys, uint32_t len) {
    if (len == 0 || phys >= RAM_SIZE) return;
    uint32_t end = phys + len - 1u;
    if (end >= RAM_SIZE || end < phys) end = RAM_SIZE - 1u;
    uint32_t fp = phys >> DIRTY_RAM_PAGE_SHIFT;
    uint32_t lp = end  >> DIRTY_RAM_PAGE_SHIFT;
    for (uint32_t pg = fp; pg <= lp; pg++)
        overlay_watch_bitmap[pg >> 5] |= (1u << (pg & 31u));
}

void overlay_watch_clear_range(uint32_t phys, uint32_t len) {
    if (len == 0 || phys >= RAM_SIZE) return;
    uint32_t end = phys + len - 1u;
    if (end >= RAM_SIZE || end < phys) end = RAM_SIZE - 1u;
    uint32_t fp = phys >> DIRTY_RAM_PAGE_SHIFT;
    uint32_t lp = end  >> DIRTY_RAM_PAGE_SHIFT;
    for (uint32_t pg = fp; pg <= lp; pg++)
        overlay_watch_bitmap[pg >> 5] &= ~(1u << (pg & 31u));
}

/* Sum of generation counters over the pages spanning [phys, phys+len). The
 * loader stores this at validation time and compares on dispatch; any change
 * means a watched page in the range was written. */
uint32_t overlay_watch_pagegen_sum(uint32_t phys, uint32_t len) {
    if (len == 0 || phys >= RAM_SIZE) return 0;
    uint32_t end = phys + len - 1u;
    if (end >= RAM_SIZE || end < phys) end = RAM_SIZE - 1u;
    uint32_t fp = phys >> DIRTY_RAM_PAGE_SHIFT;
    uint32_t lp = end  >> DIRTY_RAM_PAGE_SHIFT;
    uint32_t sum = 0;
    for (uint32_t pg = fp; pg <= lp; pg++) sum += overlay_page_gen[pg];
    return sum;
}

static inline void overlay_watch_note_write(uint32_t phys, uint32_t size) {
    uint32_t pg = phys >> DIRTY_RAM_PAGE_SHIFT;
    if (pg >= DIRTY_RAM_PAGE_COUNT) return;
    if ((overlay_watch_bitmap[pg >> 5] >> (pg & 31u)) & 1u) {
        overlay_page_gen[pg]++;
        /* Self-modification of a currently-executing native entry cannot be
         * recovered lazily (the next dispatch is too late) — the loader
         * blacklists that entry. Everything else is handled at dispatch. */
        extern void overlay_loader_active_write_check(uint32_t phys, uint32_t size);
        overlay_loader_active_write_check(phys, size);
    }
}

/* Memory control registers: 0x1F801000..0x1F80103F (16 words) + 0x1F801060 (RAM size).
 * Includes expansion base/size, COM_DELAY, SPU_DELAY, CDROM_DELAY etc. */
static uint32_t mem_ctrl[16];   /* indices 0..15 → addresses 0x1F801000..0x1F80103C */
static uint32_t ram_size_reg;   /* 0x1F801060 */

/* KSEG2 no-op access telemetry (see the guards in the accessors). */
uint64_t g_kseg2_ignored_reads;
uint64_t g_kseg2_ignored_writes;

/* Cache control register (KSEG2: 0xFFFE0130). */
static uint32_t cache_ctrl;

/* Pointer to cpu->cop0[12] (SR).  Set once at init.
 * Used by write functions to check the IsC (Isolate Cache) bit.
 * When IsC is set, RAM/scratchpad writes are silently dropped — the
 * real R3000A sends them to the data cache only. */
static const uint32_t *sr_ptr;

/* Interrupt controller — non-static so hardware subsystems can set I_STAT bits. */
uint32_t i_stat;  /* 0x1F801070 — interrupt status (AND-acknowledge semantics) */
uint32_t i_mask;  /* 0x1F801074 — interrupt enable mask */

/* Shadow-diff device-access detector. run_shadow_diff arms g_shadow_mmio_watch
 * around its single authoritative (interpreter) probe pass; ANY MMIO touch bumps
 * g_shadow_mmio_hits, so the harness can detect a device-touching function and
 * SKIP the validation (native) pass — device I/O must never be double-executed
 * (one spurious card/SIO/DMA write corrupts hardware state). Zero cost when not
 * watching (adds 0). */
int      g_shadow_mmio_watch = 0;
uint64_t g_shadow_mmio_hits  = 0;
/* Always-on MMIO access counter (reads AND writes of the device page). The
 * idle-loop cycle skip (psx_cycles.c) requires "no MMIO touched since the
 * last interrupt check" before fast-forwarding time: an MMIO read can return
 * time-varying or side-effecting values (timer counters, CD response FIFO),
 * so any touch disqualifies the window. One increment per access. */
uint64_t g_mmio_access_count = 0;
#define SHADOW_NOTE_MMIO()  do { g_mmio_access_count++; \
    g_shadow_mmio_hits += (uint64_t)g_shadow_mmio_watch; } while (0)

/* Always-on guest store counter, bumped at the top of every raw store
 * (RAM, scratchpad, MMIO, even discarded KSEG2/IsC stores — over-counting
 * is conservative). Second gate of the idle-loop cycle skip: a window with
 * ANY store is not a pure poll loop. CPU stores and in-flight DMA writes
 * both funnel through the psx_write_*_raw chokepoints. */
uint64_t g_guest_store_count = 0;

/* ---- Card protocol trace: tracks I_MASK bit 7 transitions ---- */
#define IMASK_TRACE_CAP 4096
typedef struct {
    uint32_t old_mask;
    uint32_t new_mask;
    uint32_t caller;     /* g_debug_current_func_addr */
    uint32_t store_pc;   /* g_debug_last_store_pc — exact PC of the SW/SH */
    uint8_t  width;      /* 8, 16, or 32 */
    uint8_t  bit7_set;   /* 1 if this write SET bit 7 */
    uint8_t  bit7_clear; /* 1 if this write CLEARED bit 7 */
    uint8_t  in_exc;
} ImaskTraceEntry;
static ImaskTraceEntry imask_trace[IMASK_TRACE_CAP];
static int imask_trace_idx = 0;
static int imask_trace_count = 0;
static int imask_bit7_set_count = 0;
static int imask_bit7_clear_count = 0;

static void imask_trace_record(uint32_t old_val, uint32_t new_val, uint8_t width) {
    extern uint32_t g_debug_current_func_addr;
    extern uint32_t g_debug_last_store_pc;
    extern int psx_get_in_exception(void);
    ImaskTraceEntry *e = &imask_trace[imask_trace_idx];
    e->old_mask   = old_val;
    e->new_mask   = new_val;
    e->caller     = g_debug_current_func_addr;
    e->store_pc   = g_debug_last_store_pc;
    e->width      = width;
    e->bit7_set   = (!(old_val & 0x80) && (new_val & 0x80)) ? 1 : 0;
    e->bit7_clear = ((old_val & 0x80) && !(new_val & 0x80)) ? 1 : 0;
    e->in_exc     = (uint8_t)psx_get_in_exception();
    if (e->bit7_set) imask_bit7_set_count++;
    if (e->bit7_clear) imask_bit7_clear_count++;
    imask_trace_idx = (imask_trace_idx + 1) % IMASK_TRACE_CAP;
    imask_trace_count++;
}

/* VBLANK-ack telemetry (Tomba 2 exception-reentry-storm diagnosis): counts how
 * many times an I_STAT write clears the VBLANK bit (the handler's ack). Read in
 * the freeze heartbeat against g_vblank_raise/deliver counts. */
uint64_t g_vblank_ack_count = 0;

static void interrupt_write_stat_masked(uint32_t val, uint32_t mask) {
    uint32_t ack_mask = mask & 0x7FFu;
    uint32_t before = i_stat;
    i_stat = (i_stat & ~ack_mask) | (i_stat & val & ack_mask);
    if ((before & 1u) && !(i_stat & 1u)) g_vblank_ack_count++;  /* VBLANK bit 1->0 */
}

static void interrupt_write_mask_masked(uint32_t val, uint32_t mask, uint8_t width) {
    uint32_t old = i_mask;
    i_mask = ((i_mask & ~mask) | (val & mask)) & 0x7FFu;
    imask_trace_record(old, i_mask, width);
}

/* Getters for debug server */
int memory_get_imask_bit7_set_count(void) { return imask_bit7_set_count; }
int memory_get_imask_bit7_clear_count(void) { return imask_bit7_clear_count; }
const ImaskTraceEntry *memory_get_imask_trace(int *idx_out, int *count_out) {
    if (idx_out) *idx_out = imask_trace_idx;
    if (count_out) *count_out = imask_trace_count;
    return imask_trace;
}

/* Tier 1 write-trace hooks (implemented in debug_server.c). */
extern void debug_server_trace_write_check(uint32_t phys, uint32_t old_val,
                                           uint32_t new_val, uint8_t width);
extern void debug_server_trace_mmio_write(uint32_t addr, uint32_t val, uint8_t width);
extern void debug_server_trace_mmio_read(uint32_t addr, uint32_t val, uint8_t width);
/* Targeted main-RAM read watch (debug_server.c). Flag gates the hot read path. */
extern int  g_ram_read_watch_active;
extern void debug_server_trace_ram_read_watch(uint32_t phys, uint32_t val);
extern void debug_server_trace_entryint_write(uint32_t phys, uint32_t old_val,
                                              uint32_t new_val, uint8_t width);
extern CPUState *debug_cpu_ptr;
extern uint32_t g_debug_last_store_pc;

/* Parity last-writer provenance (parity_trace.c): note every main-RAM write so
 * the watch-word last-writer table tracks the exact producing store. No-op
 * unless the parity ring is armed. */
extern void parity_trace_note_write(uint32_t addr, uint32_t width, uint32_t writer_pc);

/* Effective writer PC for provenance: during a DMA transfer the last CPU-store
 * PC is stale/unrelated, so attribute DMA-sourced RAM writes to the PC that
 * kicked the DMA (dma.c). Matches the wtrace recorder's DMA attribution. */
extern int      g_dma_exec_depth;
extern uint32_t g_dma_initiator_pc;
static inline uint32_t effective_store_pc(void) {
    return (g_dma_exec_depth > 0 && g_dma_initiator_pc) ? g_dma_initiator_pc
                                                        : g_debug_last_store_pc;
}

/* Card-byte destination capture (Phase 3 audit). Always-on. */
extern int card_data_writes_check(uint32_t phys, uint32_t value, uint8_t width);

static inline uint32_t read_ram_word(uint32_t phys) {
    return  (uint32_t)ram[phys]
         | ((uint32_t)ram[phys + 1] << 8)
         | ((uint32_t)ram[phys + 2] << 16)
         | ((uint32_t)ram[phys + 3] << 24);
}
static inline uint16_t read_ram_half(uint32_t phys) {
    return (uint16_t)ram[phys] | ((uint16_t)ram[phys + 1] << 8);
}

/* SPU registers are now handled by spu.c */

void memory_set_sr_ptr(const uint32_t *p) { sr_ptr = p; }
uint32_t memory_get_sr(void) { return sr_ptr ? *sr_ptr : 0; }

static uint32_t s_bios_checksum = 0;
uint32_t memory_get_bios_checksum(void) { return s_bios_checksum; }

void memory_init(const char* bios_path) {
    memset(ram, 0, sizeof(ram));
    memset(scratchpad, 0, sizeof(scratchpad));

    FILE* f = fopen(bios_path, "rb");
    if (!f) {
        fprintf(stderr, "FATAL: cannot open BIOS file: %s\n", bios_path);
        exit(1);
    }
    size_t n = fread(bios_rom, 1, BIOS_ROM_SIZE, f);
    fclose(f);
    if (n != BIOS_ROM_SIZE) {
        fprintf(stderr, "FATAL: BIOS file %s is %zu bytes (expected %d)\n",
                bios_path, n, BIOS_ROM_SIZE);
        exit(1);
    }
    s_bios_checksum = 0;
    for (uint32_t i = 0; i < BIOS_ROM_SIZE / 4; i++)
        s_bios_checksum += ((const uint32_t*)bios_rom)[i];
}

/* Unmapped-register access INSIDE the I/O window (0x1F801000..0x1F803FFF):
 * real hardware open-buses these (reads return garbage, writes vanish; no
 * fault) and games genuinely hit them — Tomba2's late attract sweeps a wild
 * byte loop across the whole window (bzero/read over a 0xDF80xxxx pointer).
 * Beetle returns 0 / ignores. Match it: count + (already ring-traced by the
 * callers' mmio trace hooks) + open-bus. Genuinely unknown-DEVICE reads are
 * still observable via the always-on MMIO rings and these counters — probes
 * query the rings, per the ring-buffer doctrine. mmio_fatal is retired. */
uint64_t g_io_openbus_reads;
uint64_t g_io_openbus_writes;

static void mmio_fatal(uint32_t vaddr, uint32_t phys, const char* op) {
    static char reason[96];
    snprintf(reason, sizeof(reason), "MMIO %s @ 0x%08X (phys 0x%08X)", op, vaddr, phys);
    fprintf(stderr, "%s\n", reason);
    fflush(stderr);
    FILE* cf = fopen("psx_crash.txt", "w");
    if (cf) { fprintf(cf, "%s\n", reason); fclose(cf); }
    psx_fatal_halt(reason);
}

static void mmio_unimplemented(uint32_t addr, const char* op) {
    static char reason[96];
    snprintf(reason, sizeof(reason), "UNIMPLEMENTED MMIO %s @ 0x%08X", op, addr);
    fprintf(stderr, "%s\n", reason);
    fflush(stderr);
    /* Also write to a crash file for capture when stderr is lost. */
    FILE* cf = fopen("psx_crash.txt", "w");
    if (cf) { fprintf(cf, "%s\n", reason); fclose(cf); }
    psx_fatal_halt(reason);
}

static void unmapped_fatal(uint32_t vaddr, uint32_t phys, const char* op) {
    /* On real PS1 hardware, reads from unmapped addresses return open bus
     * (typically the last value on the data bus, or 0xFFFFFFFF).
     * The BIOS intentionally probes unmapped regions (RAM size detection,
     * expansion hardware detection). Fatal abort would prevent normal boot. */
    (void)vaddr; (void)phys; (void)op;
}

/* --- MMIO read/write helpers --- */

static uint32_t mmio_read32_impl(uint32_t addr) {
    SHADOW_NOTE_MMIO();
    /* Memory control: 0x1F801000..0x1F801020 */
    if (addr >= 0x1F801000u && addr <= 0x1F80103Cu) {
        return mem_ctrl[(addr - 0x1F801000u) >> 2];
    }
    /* SIO: 0x1F801040..0x1F80105F */
    if (addr >= 0x1F801040u && addr <= 0x1F80105Fu) {
        return sio_read(addr);
    }
    /* RAM size: 0x1F801060 */
    if (addr == 0x1F801060u) {
        return ram_size_reg;
    }
    /* Interrupts: 0x1F801070, 0x1F801074 */
    if (addr == 0x1F801070u) { sio_tick(0); return i_stat; }
    if (addr == 0x1F801074u) return i_mask;
    /* DMA: 0x1F801080..0x1F8010FF */
    if (addr >= 0x1F801080u && addr <= 0x1F8010FFu) {
        return dma_read(addr);
    }
    /* Timers: 0x1F801100..0x1F80112F */
    if (addr >= 0x1F801100u && addr <= 0x1F80112Fu) {
        return timers_read(addr);
    }
    /* CDROM: 0x1F801800..0x1F801803 */
    if (addr >= 0x1F801800u && addr <= 0x1F801803u) {
        return cdrom_read(addr);
    }
    /* GPU: 0x1F801810 (GPUREAD), 0x1F801814 (GPUSTAT) */
    if (addr == 0x1F801810u) return gpu_read_gpuread();
    if (addr == 0x1F801814u) return gpu_read_gpustat();
    /* MDEC: 0x1F801820, 0x1F801824 */
    if (addr == 0x1F801820u || addr == 0x1F801824u) return mdec_read(addr);
    /* SPU: 0x1F801C00..0x1F801FFF */
    if (addr >= 0x1F801C00u && addr <= 0x1F801FFFu) {
        return spu_read(addr);
    }
    /* Expansion 2 / POST: 0x1F802000..0x1F802FFF */
    if (addr >= 0x1F802000u && addr <= 0x1F802FFFu) {
        return 0;
    }
    { /* open-bus (Beetle parity) */ g_io_openbus_reads++;  return 0;; }
    return 0;
}

/* Thin wrapper: record the loaded value into the MMIO-READ trace ring AFTER the
 * single (side-effecting) read. Callers use mmio_read32; the body is _impl, so
 * the device read executes exactly once. */
static uint32_t mmio_read32(uint32_t addr) {
    psx_devices_mmio_sync();
    uint32_t v = mmio_read32_impl(addr);
    debug_server_trace_mmio_read(addr, v, 4);
    return v;
}

static void mmio_write32(uint32_t addr, uint32_t val) {
    psx_devices_mmio_sync();
    SHADOW_NOTE_MMIO();
    debug_server_trace_mmio_write(addr, val, 4);
    /* Memory control: 0x1F801000..0x1F801020 */
    if (addr >= 0x1F801000u && addr <= 0x1F80103Cu) {
        mem_ctrl[(addr - 0x1F801000u) >> 2] = val;
        return;
    }
    /* SIO: 0x1F801040..0x1F80105F */
    if (addr >= 0x1F801040u && addr <= 0x1F80105Fu) {
        sio_write(addr, val);
        return;
    }
    /* RAM size: 0x1F801060 */
    if (addr == 0x1F801060u) {
        ram_size_reg = val;
        return;
    }
    /* Interrupts: 0x1F801070, 0x1F801074 */
    if (addr == 0x1F801070u) { interrupt_write_stat_masked(val, 0xFFFFFFFFu); return; }
    if (addr == 0x1F801074u) { interrupt_write_mask_masked(val, 0xFFFFFFFFu, 32); return; }
    /* DMA: 0x1F801080..0x1F8010FF */
    if (addr >= 0x1F801080u && addr <= 0x1F8010FFu) {
        dma_write(addr, val);
        return;
    }
    /* Timers: 0x1F801100..0x1F80112F */
    if (addr >= 0x1F801100u && addr <= 0x1F80112Fu) {
        timers_write(addr, val);
        return;
    }
    /* CDROM: 0x1F801800..0x1F801803 */
    if (addr >= 0x1F801800u && addr <= 0x1F801803u) {
        cdrom_write(addr, val);
        return;
    }
    /* GPU GP0: 0x1F801810, GP1: 0x1F801814 */
    if (addr == 0x1F801810u) {
        uint32_t src = addr;
        if (g_debug_last_store_pc == 0xBFC38B1Cu && debug_cpu_ptr) {
            src = (debug_cpu_ptr->gpr[4] - 4u) & 0x1FFFFCu;
        }
        gpu_set_gp0_source(src);
        gpu_write_gp0(val);
        return;
    }
    if (addr == 0x1F801814u) { gpu_write_gp1(val); return; }
    /* MDEC: 0x1F801820, 0x1F801824 */
    if (addr == 0x1F801820u || addr == 0x1F801824u) { mdec_write(addr, val); return; }
    /* SPU: 0x1F801C00..0x1F801FFF */
    if (addr >= 0x1F801C00u && addr <= 0x1F801FFFu) {
        spu_write(addr, val);
        return;
    }
    /* Expansion 2 / POST: 0x1F802000..0x1F802FFF */
    if (addr >= 0x1F802000u && addr <= 0x1F802FFFu) {
        return; /* POST port — ignore */
    }
    { /* open-bus (Beetle parity) */ g_io_openbus_writes++; return;; }
}

static uint16_t mmio_read16_impl(uint32_t addr) {
    SHADOW_NOTE_MMIO();
    /* Memory control: 0x1F801000..0x1F80103C — halfword lane of the 32-bit
     * register (games touch EXP1/EXP2 config with sub-word accesses; Tomba2's
     * late attract byte-writes 0x1F801000). */
    if (addr >= 0x1F801000u && addr <= 0x1F80103Fu) {
        uint32_t v = mem_ctrl[(addr - 0x1F801000u) >> 2];
        return (uint16_t)(v >> (8u * (addr & 2u)));
    }
    if (addr >= 0x1F801060u && addr <= 0x1F801063u) {
        return (uint16_t)(ram_size_reg >> (8u * (addr & 2u)));
    }
    /* SIO: 0x1F801040..0x1F80105F */
    if (addr >= 0x1F801040u && addr <= 0x1F80105Fu) {
        return (uint16_t)sio_read(addr);
    }
    /* Interrupts */
    if (addr >= 0x1F801070u && addr <= 0x1F801072u) {
        sio_tick(0);
        uint32_t shift = (addr & 2u) ? 16u : 0u;
        return (uint16_t)(i_stat >> shift);
    }
    if (addr >= 0x1F801074u && addr <= 0x1F801076u) {
        uint32_t shift = (addr & 2u) ? 16u : 0u;
        return (uint16_t)(i_mask >> shift);
    }
    /* Timers: 0x1F801100..0x1F80112F */
    if (addr >= 0x1F801100u && addr <= 0x1F80112Fu) {
        return (uint16_t)timers_read(addr);
    }
    /* DMA: 0x1F801080..0x1F8010FF */
    if (addr >= 0x1F801080u && addr <= 0x1F8010FFu) {
        uint32_t val = dma_read(addr & ~3u);
        return (addr & 2) ? (uint16_t)(val >> 16) : (uint16_t)val;
    }
    /* MDEC: 0x1F801820..0x1F801827 */
    if (addr >= 0x1F801820u && addr <= 0x1F801827u) {
        uint32_t val = mdec_read(addr & ~3u);
        return (addr & 2) ? (uint16_t)(val >> 16) : (uint16_t)val;
    }
    /* SPU: 0x1F801C00..0x1F801FFF */
    if (addr >= 0x1F801C00u && addr <= 0x1F801FFFu) {
        return (uint16_t)spu_read(addr);
    }
    { /* open-bus (Beetle parity) */ g_io_openbus_reads++;  return 0;; }
    return 0;
}

static uint16_t mmio_read16(uint32_t addr) {
    psx_devices_mmio_sync();
    uint16_t v = mmio_read16_impl(addr);
    debug_server_trace_mmio_read(addr, (uint32_t)v, 2);
    return v;
}

static void mmio_write16(uint32_t addr, uint16_t val) {
    psx_devices_mmio_sync();
    SHADOW_NOTE_MMIO();
    debug_server_trace_mmio_write(addr, (uint32_t)val, 2);
    /* Memory control: 0x1F801000..0x1F80103C — halfword lane RMW. */
    if (addr >= 0x1F801000u && addr <= 0x1F80103Fu) {
        uint32_t idx = (addr - 0x1F801000u) >> 2;
        uint32_t shift = 8u * (addr & 2u);
        mem_ctrl[idx] = (mem_ctrl[idx] & ~(0xFFFFu << shift))
                      | ((uint32_t)val << shift);
        return;
    }
    /* RAM size register: 0x1F801060 — halfword lane RMW. */
    if (addr >= 0x1F801060u && addr <= 0x1F801063u) {
        uint32_t shift = 8u * (addr & 2u);
        ram_size_reg = (ram_size_reg & ~(0xFFFFu << shift))
                     | ((uint32_t)val << shift);
        return;
    }
    /* SIO: 0x1F801040..0x1F80105F */
    if (addr >= 0x1F801040u && addr <= 0x1F80105Fu) {
        sio_write(addr, val);
        return;
    }
    /* Interrupts */
    if (addr >= 0x1F801070u && addr <= 0x1F801072u) {
        uint32_t shift = (addr & 2u) ? 16u : 0u;
        interrupt_write_stat_masked((uint32_t)val << shift, 0xFFFFu << shift);
        return;
    }
    if (addr >= 0x1F801074u && addr <= 0x1F801076u) {
        uint32_t shift = (addr & 2u) ? 16u : 0u;
        interrupt_write_mask_masked((uint32_t)val << shift, 0xFFFFu << shift, 16);
        return;
    }
    /* Timers: 0x1F801100..0x1F80112F */
    if (addr >= 0x1F801100u && addr <= 0x1F80112Fu) {
        timers_write(addr, val);
        return;
    }
    /* DMA: 0x1F801080..0x1F8010FF */
    if (addr >= 0x1F801080u && addr <= 0x1F8010FFu) {
        uint32_t aligned = addr & ~3u;
        uint32_t shift = (addr & 2) ? 16u : 0u;
        uint32_t mask = 0xFFFFu << shift;
        dma_write_masked(aligned, (uint32_t)val << shift, mask);
        return;
    }
    /* MDEC: 0x1F801820..0x1F801827 */
    if (addr >= 0x1F801820u && addr <= 0x1F801827u) {
        uint32_t aligned = addr & ~3u;
        uint32_t cur = mdec_read(aligned);
        if (addr & 2)
            cur = (cur & 0x0000FFFFu) | ((uint32_t)val << 16);
        else
            cur = (cur & 0xFFFF0000u) | (uint32_t)val;
        mdec_write(aligned, cur);
        return;
    }
    /* SPU: 0x1F801C00..0x1F801FFF */
    if (addr >= 0x1F801C00u && addr <= 0x1F801FFFu) {
        spu_write(addr, val);
        return;
    }
    { /* open-bus (Beetle parity) */ g_io_openbus_writes++; return;; }
}

static uint8_t mmio_read8_impl(uint32_t addr) {
    SHADOW_NOTE_MMIO();
    /* Memory control: 0x1F801000..0x1F80103C — byte lane of the 32-bit reg. */
    if (addr >= 0x1F801000u && addr <= 0x1F80103Fu) {
        uint32_t v = mem_ctrl[(addr - 0x1F801000u) >> 2];
        return (uint8_t)(v >> (8u * (addr & 3u)));
    }
    if (addr >= 0x1F801060u && addr <= 0x1F801063u) {
        return (uint8_t)(ram_size_reg >> (8u * (addr & 3u)));
    }
    /* Interrupts: 0x1F801070..0x1F801077 (I_STAT, I_MASK) */
    if (addr >= 0x1F801070u && addr <= 0x1F801077u) {
        if (addr < 0x1F801074u) sio_tick(0);
        uint32_t val = (addr < 0x1F801074u) ? i_stat : i_mask;
        return (uint8_t)(val >> (8 * (addr & 3)));
    }
    /* SIO: 0x1F801040..0x1F80104F */
    if (addr >= 0x1F801040u && addr <= 0x1F80104Fu) {
        return (uint8_t)sio_read(addr & ~3u);
    }
    /* DMA: 0x1F801080..0x1F8010FF — byte reads return the corresponding
     * byte of the 32-bit register.  The BIOS shell reads DICR (0x1F8010F4)
     * as individual bytes during interrupt handling. */
    if (addr >= 0x1F801080u && addr <= 0x1F8010FFu) {
        uint32_t aligned = addr & ~3u;
        uint32_t val = dma_read(aligned);
        return (uint8_t)(val >> (8 * (addr & 3)));
    }
    /* MDEC: 0x1F801820..0x1F801827 */
    if (addr >= 0x1F801820u && addr <= 0x1F801827u) {
        uint32_t val = mdec_read(addr & ~3u);
        return (uint8_t)(val >> (8 * (addr & 3)));
    }
    /* CDROM: 0x1F801800..0x1F801803 */
    if (addr >= 0x1F801800u && addr <= 0x1F801803u) {
        return (uint8_t)cdrom_read(addr);
    }
    /* Expansion 2 / POST: 0x1F802000..0x1F802FFF */
    if (addr >= 0x1F802000u && addr <= 0x1F802FFFu) {
        return 0;
    }
    { /* open-bus (Beetle parity) */ g_io_openbus_reads++;  return 0;; }
    return 0;
}

static uint8_t mmio_read8(uint32_t addr) {
    psx_devices_mmio_sync();
    uint8_t v = mmio_read8_impl(addr);
    debug_server_trace_mmio_read(addr, (uint32_t)v, 1);
    return v;
}

static void mmio_write8(uint32_t addr, uint8_t val) {
    psx_devices_mmio_sync();
    SHADOW_NOTE_MMIO();
    debug_server_trace_mmio_write(addr, (uint32_t)val, 1);
    /* Memory control: 0x1F801000..0x1F80103C — byte lane RMW. Tomba2's late
     * attract byte-writes the EXP1 base register at 0x1F801000; only the
     * 32-bit path covered the block and the byte path fatal'd. */
    if (addr >= 0x1F801000u && addr <= 0x1F80103Fu) {
        uint32_t idx = (addr - 0x1F801000u) >> 2;
        uint32_t shift = 8u * (addr & 3u);
        mem_ctrl[idx] = (mem_ctrl[idx] & ~(0xFFu << shift))
                      | ((uint32_t)val << shift);
        return;
    }
    /* RAM size register: 0x1F801060 — byte lane RMW (Tomba2 late attract). */
    if (addr >= 0x1F801060u && addr <= 0x1F801063u) {
        uint32_t shift = 8u * (addr & 3u);
        ram_size_reg = (ram_size_reg & ~(0xFFu << shift))
                     | ((uint32_t)val << shift);
        return;
    }
    /* Interrupts: partial stores affect only the addressed byte lane. */
    if (addr >= 0x1F801070u && addr <= 0x1F801073u) {
        uint32_t shift = 8u * (addr & 3u);
        interrupt_write_stat_masked((uint32_t)val << shift, 0xFFu << shift);
        return;
    }
    if (addr >= 0x1F801074u && addr <= 0x1F801077u) {
        uint32_t shift = 8u * (addr & 3u);
        interrupt_write_mask_masked((uint32_t)val << shift, 0xFFu << shift, 8);
        return;
    }
    /* SIO: 0x1F801040..0x1F80105F */
    if (addr >= 0x1F801040u && addr <= 0x1F80105Fu) {
        sio_write(addr & ~3u, (uint32_t)val);
        return;
    }
    /* DMA: 0x1F801080..0x1F8010FF — byte writes update the corresponding
     * byte of the 32-bit register.  Needed for DICR byte-level access. */
    if (addr >= 0x1F801080u && addr <= 0x1F8010FFu) {
        uint32_t aligned = addr & ~3u;
        uint32_t shift = 8 * (addr & 3);
        uint32_t mask = 0xFFu << shift;
        dma_write_masked(aligned, (uint32_t)val << shift, mask);
        return;
    }
    /* Timers: 0x1F801100..0x1F80112F — byte writes update the addressed byte
     * lane of the 32-bit register. Byte stores to timer registers are valid
     * hardware accesses; mmio_write16/32 already route here via timers_write, so
     * write8 must too (otherwise a guest `sb` to a timer fails loud). */
    if (addr >= 0x1F801100u && addr <= 0x1F80112Fu) {
        uint32_t aligned = addr & ~3u;
        uint32_t cur = timers_read(aligned);
        uint32_t shift = 8 * (addr & 3);
        cur = (cur & ~(0xFFu << shift)) | ((uint32_t)val << shift);
        timers_write(aligned, cur);
        return;
    }
    /* MDEC: 0x1F801820..0x1F801827 */
    if (addr >= 0x1F801820u && addr <= 0x1F801827u) {
        uint32_t aligned = addr & ~3u;
        uint32_t cur = mdec_read(aligned);
        uint32_t shift = 8 * (addr & 3);
        cur = (cur & ~(0xFFu << shift)) | ((uint32_t)val << shift);
        mdec_write(aligned, cur);
        return;
    }
    /* CDROM: 0x1F801800..0x1F801803 */
    if (addr >= 0x1F801800u && addr <= 0x1F801803u) {
        cdrom_write(addr, val);
        return;
    }
    /* Expansion 2 / POST: 0x1F802000..0x1F802FFF */
    if (addr >= 0x1F802000u && addr <= 0x1F802FFFu) {
        return;
    }
    { /* open-bus (Beetle parity) */ g_io_openbus_writes++; return;; }
}

/* --- Read functions --- */

/* lockstep: 1 while a guest-direct memory op's REAL body runs, so nested ops
 * (device-emulation reads triggered by an MMIO write, etc.) are not recorded —
 * the shadow replay verifies writes without performing them, so it never sees
 * those device-internal accesses. Interrupt-handler ops are excluded too
 * (psx_get_in_exception): the replay runs only the block, never the handler. */
static int s_ls_op_active = 0;
extern int g_ls_suppress_record;
extern int g_dma_exec_depth;
extern int psx_get_in_exception(void);
static uint32_t psx_read_word_raw(uint32_t addr);
uint32_t psx_read_word(uint32_t addr) {
    if (g_ls_mode == 2) return ls_read_hook(addr, 4, 0u);
    if (g_ls_mode != 1 || s_ls_op_active || g_ls_suppress_record || g_dma_exec_depth > 0) return psx_read_word_raw(addr);
    s_ls_op_active = 1;
    uint32_t v = psx_read_word_raw(addr);
    s_ls_op_active = 0;
    if (!psx_get_in_exception()) ls_read_hook(addr, 4, v);
    return v;
}
/* Physical address of a CPU/DMA main-RAM access. Fold KUSEG/KSEG0/KSEG1 first
 * (0x1FFFFFFF), then fold the 2nd-4th main-RAM mirrors: real hardware mirrors the
 * 2 MB DRAM across the WHOLE 0..0x7FFFFF physical window (Beetle libretro.cpp:874
 * `A < 0x00800000` routes to main RAM; psx-spx "2048K RAM ... mirrored 4x"). A game
 * may legitimately place its stack at the top of that window — Tsumu Light computes
 * sp = (ramtop-8)|0x80000000 with its ramtop constant 0x00800000, giving sp=0x807FFFF8
 * (top of the 4th mirror). Without this fold those accesses miss DRAM (`< RAM_SIZE`
 * fails) and hit open bus: stores drop, loads return 0, so the first saved return
 * address reads back 0 and `jr ra` derails to PC=0. Identity for phys < RAM_SIZE, so
 * normal-range RAM is byte-identical; MMIO/scratchpad/BIOS (>= 0x800000) untouched. */
static uint32_t psx_read_word_raw(uint32_t addr) {
    /* KSEG2 cache control — before physical translation. */
    if (addr == 0xFFFE0130u) return cache_ctrl;
    /* KSEG2 (0xC0000000+): only cache control (0xFFFE0130, above where
     * applicable) exists there. Real hardware maps NOTHING else — Beetle
     * (cpu.cpp addr_mask[6..7]=0xFFFFFFFF) leaves KSEG2 addresses unmasked
     * so they fall to unmapped space and the access is a no-op. Our flat
     * 0x1FFFFFFF masking routed KSEG2 garbage onto LIVE registers: Tomba2's
     * attract runs a BIOS bzero over a wild 0xDF80xxxx pointer, which zeroed
     * the SIO/memctrl I/O block byte-by-byte and then hit the unmapped-MMIO
     * fatal (frame 9705). Ignore writes, read as 0, count for telemetry. */
    if (addr >= 0xC0000000u) { g_kseg2_ignored_reads++; return 0; }

    uint32_t phys = psx_phys_addr(addr);

    if (phys < RAM_SIZE) {
        uint32_t v = (uint32_t)ram[phys]
             | ((uint32_t)ram[phys + 1] << 8)
             | ((uint32_t)ram[phys + 2] << 16)
             | ((uint32_t)ram[phys + 3] << 24);
        /* Targeted main-RAM read watch (debug). Flag is 0 in normal runs, so the
         * hot read path pays only a predictable branch. */
        if (g_ram_read_watch_active) debug_server_trace_ram_read_watch(phys, v);
        return v;
    }
    /* Expansion 1: 0x1F000000..0x1F7FFFFF — no device, open bus */
    if (phys >= 0x1F000000u && phys <= 0x1F7FFFFFu) {
        return 0xFFFFFFFFu;
    }
    if (phys >= 0x1F800000u && phys <= 0x1F8003FFu) {
        uint32_t off = phys - 0x1F800000u;
        return  (uint32_t)scratchpad[off]
             | ((uint32_t)scratchpad[off + 1] << 8)
             | ((uint32_t)scratchpad[off + 2] << 16)
             | ((uint32_t)scratchpad[off + 3] << 24);
    }
    if (phys >= 0x1F801000u && phys <= 0x1F803FFFu) {
        return mmio_read32(phys);
    }
    if (phys >= 0x1FC00000u && phys <= 0x1FC7FFFFu) {
        uint32_t off = phys - 0x1FC00000u;
        return  (uint32_t)bios_rom[off]
             | ((uint32_t)bios_rom[off + 1] << 8)
             | ((uint32_t)bios_rom[off + 2] << 16)
             | ((uint32_t)bios_rom[off + 3] << 24);
    }
    unmapped_fatal(addr, phys, "READ");
    return 0;
}

/* ---- VSync callback-pointer provenance probe (MMX6 boot wedge) -------------
 * The kernel VSync/RCnt callback-block pointer at phys 0x79D44 (KSEG0 0x80079D44)
 * is corrupted to 0x016F0110 at frame ~1188 by a write whose g_debug_last_store_pc
 * is STALE (so it is not a compiled/dirty-interp store — a DMA or runtime mem-op
 * with a wrong destination). This always-on ring records EVERY write to that word
 * at the unified RAM-write chokepoint, tagging CPU-vs-DMA via the dma.c exec flags,
 * so the real corrupting writer (and, if DMA, its channel + madr) is captured even
 * though the per-instruction store-PC tracker can't see it. Dump via `d44_ring`. */
#define D44_PHYS 0x00079D44u
#define D44_RING_CAP 32u
typedef struct {
    uint64_t seq;
    uint32_t val, old, store_pc;
    int32_t  dma_depth, dma_ch;
    uint32_t dma_madr, dma_bcr, frame;
} D44Entry;
D44Entry  g_d44_ring[D44_RING_CAP];
uint64_t  g_d44_seq = 0;
extern uint32_t g_debug_last_store_pc;
extern int      g_dma_exec_depth;     /* >0 while a DMA is moving data (dma.c) */
extern int      g_dma_cur_ch;         /* channel of the in-flight DMA, else -1  */
extern uint32_t g_dma_cur_madr;       /* current MADR of the in-flight DMA      */
extern uint32_t g_dma_cur_bcr;        /* BCR of the in-flight DMA               */
extern uint64_t s_frame_count;
static inline void d44_note(uint32_t phys, uint32_t old, uint32_t val) {
    if (phys != D44_PHYS) return;
    uint64_t i = g_d44_seq++;
    D44Entry *e = &g_d44_ring[i & (D44_RING_CAP - 1u)];
    e->seq = i; e->val = val; e->old = old; e->store_pc = g_debug_last_store_pc;
    e->dma_depth = g_dma_exec_depth; e->dma_ch = g_dma_cur_ch;
    e->dma_madr = g_dma_cur_madr; e->dma_bcr = g_dma_cur_bcr;
    e->frame = (uint32_t)s_frame_count;
}

static void psx_write_word_raw(uint32_t addr, uint32_t val);
extern void gte_precision_invalidate_word(uint32_t addr);
void psx_write_word(uint32_t addr, uint32_t val) {
    if (g_ls_mode == 2) { ls_write_hook(addr, 4, val); return; }
    if (g_ls_mode != 1 || s_ls_op_active || g_ls_suppress_record || g_dma_exec_depth > 0) { psx_write_word_raw(addr, val); return; }
    if (!psx_get_in_exception()) ls_write_hook(addr, 4, val);
    s_ls_op_active = 1;
    psx_write_word_raw(addr, val);
    s_ls_op_active = 0;
}
static void psx_write_word_raw(uint32_t addr, uint32_t val) {
    g_guest_store_count++;
    gte_precision_invalidate_word(addr);
    /* KSEG2 cache control — before physical translation. */
    if (addr == 0xFFFE0130u) { cache_ctrl = val; return; }
    /* KSEG2 guard — see psx_read_word_raw. */
    if (addr >= 0xC0000000u) { g_kseg2_ignored_writes++; return; }

    /* IsC (Isolate Cache): when set, writes go to D-cache only.
     * We have no cache model, so silently discard RAM/scratchpad writes. */
    if (sr_ptr && (*sr_ptr & 0x10000u)) return;

    uint32_t phys = psx_phys_addr(addr);

    /* The generated BIOS mirrors its exception trampoline to 0x80000000 during
     * boot. On hardware this mirror copy is not visible in RAM; only the real
     * exception vector at 0x80000080 is. Tomba 2 later passes buffer=0 to the
     * BIOS card write routine, so stale mirror bytes at 0 corrupt the sector 63
     * management write and leave the load menu stuck checking the card. */
    if (fntrace_is_game_started() &&
        phys < 0x10u && g_debug_last_store_pc == 0xBFC10A00u) return;

    /* BIOS helpers use RAM address zero as a tiny delay-loop scratch between
     * device-register polls. Treat these two dummy stores as non-visible; real
     * hardware / Beetle preserve Tomba 2's sector-63 card-management payload
     * when it passes buffer=0 to _card_write. */
    if (fntrace_is_game_started() && phys == 0u) {
        switch (g_debug_last_store_pc) {
        case 0xBFC04E90u:
        case 0xBFC04EF0u:
        case 0xBFC05164u:
        case 0xBFC0D634u:
        case 0xBFC3EEB4u:
        case 0xBFC405E4u:
        case 0xBFC40788u:
        case 0xBFC41C50u:
        case 0x80012434u:
        case 0x800125ACu:
            return;
        default:
            break;
        }
    }

    if (phys < RAM_SIZE) {
        /* Tomba 2 load-game card check: the BIOS card-manager cleanup helper
         * at kernel RAM 0x1C5C scans MARK events and re-arms any READY event
         * matching F0000011/{4,8000,100,200,2000}. In the recomp timing path it
         * can run after the card completion DeliverEvent but before the game's
         * TestEvent poll, consuming the public card event and leaving the UI
         * stuck at "Checking MEMORY CARD...". Let the real TestEvent consume
         * public MARK card events instead; keep this narrowly keyed to the
         * helper's status store and the EvCB layout. */
        if (fntrace_is_game_started() &&
            g_debug_last_store_pc == 0xBFC117E4u &&
            val == 0x2000u &&
            phys >= 4u && (phys + 8u) < RAM_SIZE &&
            read_ram_word(phys) == 0x4000u &&
            read_ram_word(phys - 4u) == 0xF0000011u &&
            read_ram_word(phys + 8u) == 0x2000u) {
            uint32_t spec = read_ram_word(phys + 4u);
            if (spec == 0x00000004u || spec == 0x00008000u ||
                spec == 0x00000100u || spec == 0x00000200u ||
                spec == 0x00002000u) {
                return;
            }
        }
        if (phys == D44_PHYS) d44_note(phys, read_ram_word(phys), val);
        debug_server_trace_write_check(phys, read_ram_word(phys), val, 4);
        parity_trace_note_write(phys, 4, effective_store_pc());
        card_data_writes_check(phys, val, 4);
        dirty_ram_mark_kernel_write(phys);
        text_guard_note_write(phys, val, 4);
        overlay_watch_note_write(phys, 4);
#ifdef PSX_COSIM
        { extern void cosim_note_ram_write(uint32_t,uint32_t); cosim_note_ram_write(phys, 4); }
#endif
        ram[phys]     = (uint8_t)(val);
        ram[phys + 1] = (uint8_t)(val >> 8);
        ram[phys + 2] = (uint8_t)(val >> 16);
        ram[phys + 3] = (uint8_t)(val >> 24);
        return;
    }
    /* Expansion 1: 0x1F000000..0x1F7FFFFF — ignore writes */
    if (phys >= 0x1F000000u && phys <= 0x1F7FFFFFu) return;
    if (phys >= 0x1F800000u && phys <= 0x1F8003FFu) {
        uint32_t off = phys - 0x1F800000u;
        debug_server_trace_write_check(phys,
            (uint32_t)scratchpad[off]
          | ((uint32_t)scratchpad[off + 1] << 8)
          | ((uint32_t)scratchpad[off + 2] << 16)
          | ((uint32_t)scratchpad[off + 3] << 24),
            val, 4);
        scratchpad[off]     = (uint8_t)(val);
        scratchpad[off + 1] = (uint8_t)(val >> 8);
        scratchpad[off + 2] = (uint8_t)(val >> 16);
        scratchpad[off + 3] = (uint8_t)(val >> 24);
        return;
    }
    if (phys >= 0x1F801000u && phys <= 0x1F803FFFu) {
        mmio_write32(phys, val);
        return;
    }
    if (phys >= 0x1FC00000u && phys <= 0x1FC7FFFFu) {
        /* ROM: silently ignore writes */
        return;
    }
    unmapped_fatal(addr, phys, "WRITE");
}

static uint16_t psx_read_half_raw(uint32_t addr);
uint16_t psx_read_half(uint32_t addr) {
    if (g_ls_mode == 2) return (uint16_t)ls_read_hook(addr, 2, 0u);
    if (g_ls_mode != 1 || s_ls_op_active || g_ls_suppress_record || g_dma_exec_depth > 0) return psx_read_half_raw(addr);
    s_ls_op_active = 1;
    uint16_t v = psx_read_half_raw(addr);
    s_ls_op_active = 0;
    if (!psx_get_in_exception()) ls_read_hook(addr, 2, v);
    return v;
}
static uint16_t psx_read_half_raw(uint32_t addr) {
        /* KSEG2 guard — see psx_read_word_raw. */
    if (addr >= 0xC0000000u) { g_kseg2_ignored_reads++; return 0; }
    uint32_t phys = psx_phys_addr(addr);

    if (phys < RAM_SIZE) {
        return (uint16_t)ram[phys] | ((uint16_t)ram[phys + 1] << 8);
    }
    if (phys >= 0x1F000000u && phys <= 0x1F7FFFFFu) return 0xFFFFu;
    if (phys >= 0x1F800000u && phys <= 0x1F8003FFu) {
        uint32_t off = phys - 0x1F800000u;
        return (uint16_t)scratchpad[off] | ((uint16_t)scratchpad[off + 1] << 8);
    }
    if (phys >= 0x1F801000u && phys <= 0x1F803FFFu) {
        return mmio_read16(phys);
    }
    if (phys >= 0x1FC00000u && phys <= 0x1FC7FFFFu) {
        uint32_t off = phys - 0x1FC00000u;
        return (uint16_t)bios_rom[off] | ((uint16_t)bios_rom[off + 1] << 8);
    }
    unmapped_fatal(addr, phys, "READ");
    return 0;
}

static void psx_write_half_raw(uint32_t addr, uint16_t val);
void psx_write_half(uint32_t addr, uint16_t val) {
    if (g_ls_mode == 2) { ls_write_hook(addr, 2, val); return; }
    if (g_ls_mode != 1 || s_ls_op_active || g_ls_suppress_record || g_dma_exec_depth > 0) { psx_write_half_raw(addr, val); return; }
    if (!psx_get_in_exception()) ls_write_hook(addr, 2, val);
    s_ls_op_active = 1;
    psx_write_half_raw(addr, val);
    s_ls_op_active = 0;
}
static void psx_write_half_raw(uint32_t addr, uint16_t val) {
    g_guest_store_count++;
    gte_precision_invalidate_word(addr);
    if (sr_ptr && (*sr_ptr & 0x10000u)) return;

        /* KSEG2 guard — see psx_read_word_raw. */
    if (addr >= 0xC0000000u) { g_kseg2_ignored_writes++; return; }
    uint32_t phys = psx_phys_addr(addr);

    if (phys < RAM_SIZE) {
        debug_server_trace_write_check(phys, (uint32_t)read_ram_half(phys), (uint32_t)val, 2);
        parity_trace_note_write(phys, 2, effective_store_pc());
        card_data_writes_check(phys, (uint32_t)val, 2);
        dirty_ram_mark_kernel_write(phys);
        text_guard_note_write(phys, (uint32_t)val, 2);
        overlay_watch_note_write(phys, 2);
#ifdef PSX_COSIM
        { extern void cosim_note_ram_write(uint32_t,uint32_t); cosim_note_ram_write(phys, 2); }
#endif
        ram[phys]     = (uint8_t)(val);
        ram[phys + 1] = (uint8_t)(val >> 8);
        return;
    }
    if (phys >= 0x1F000000u && phys <= 0x1F7FFFFFu) return;
    if (phys >= 0x1F800000u && phys <= 0x1F8003FFu) {
        uint32_t off = phys - 0x1F800000u;
        debug_server_trace_write_check(phys,
            (uint32_t)scratchpad[off] | ((uint32_t)scratchpad[off + 1] << 8),
            (uint32_t)val, 2);
        scratchpad[off]     = (uint8_t)(val);
        scratchpad[off + 1] = (uint8_t)(val >> 8);
        return;
    }
    if (phys >= 0x1F801000u && phys <= 0x1F803FFFu) {
        mmio_write16(phys, val);
        return;
    }
    if (phys >= 0x1FC00000u && phys <= 0x1FC7FFFFu) {
        return; /* ROM: ignore */
    }
    unmapped_fatal(addr, phys, "WRITE");
}

static uint8_t psx_read_byte_raw(uint32_t addr);
uint8_t psx_read_byte(uint32_t addr) {
    if (g_ls_mode == 2) return (uint8_t)ls_read_hook(addr, 1, 0u);
    if (g_ls_mode != 1 || s_ls_op_active || g_ls_suppress_record || g_dma_exec_depth > 0) return psx_read_byte_raw(addr);
    s_ls_op_active = 1;
    uint8_t v = psx_read_byte_raw(addr);
    s_ls_op_active = 0;
    if (!psx_get_in_exception()) ls_read_hook(addr, 1, v);
    return v;
}
static uint8_t psx_read_byte_raw(uint32_t addr) {
        /* KSEG2 guard — see psx_read_word_raw. */
    if (addr >= 0xC0000000u) { g_kseg2_ignored_reads++; return 0; }
    uint32_t phys = psx_phys_addr(addr);

    if (phys < RAM_SIZE) {
        return ram[phys];
    }
    if (phys >= 0x1F000000u && phys <= 0x1F7FFFFFu) return 0xFFu;
    if (phys >= 0x1F800000u && phys <= 0x1F8003FFu) {
        return scratchpad[phys - 0x1F800000u];
    }
    if (phys >= 0x1F801000u && phys <= 0x1F803FFFu) {
        return mmio_read8(phys);
    }
    if (phys >= 0x1FC00000u && phys <= 0x1FC7FFFFu) {
        return bios_rom[phys - 0x1FC00000u];
    }
    unmapped_fatal(addr, phys, "READ");
    return 0;
}

/* ---- CPU guest-side data loads: faithful R3000A load-delay pipeline interlock ----
 * The R3000A has no usable D-cache (it is repurposed as the 1 KB scratchpad), so a
 * CPU data load from main DRAM stalls the pipeline. Beetle (cpu.cpp ReadMemory,
 * 364-451) models this as: a +2 "fudge" iff the predecessor committed no load, the
 * region wait (main RAM = +3, libretro.cpp:884), and a completion cost (+2 CPU /
 * +1 LWC2); the (region+completion) becomes a per-register LDAbsorb "give-back" that
 * following instructions consume instead of their own +1 base (pipeline write-back
 * overlap). The §1 base + GPR_DEPRES + DO_LDS that bracket this run in psx_cyc.h.
 *
 * These functions own the WHOLE per-instruction interlock for a CPU load (they call
 * psx_cyc_base/deps/lds), so the emitters/interp invoke them in place of the prior
 * cpu->read_* call and emit NO separate psx_cyc_step for the load. They return the
 * raw value via the UNCHARGED psx_read_* (cpu->read_* is now uncharged too — the
 * data-access cost is charged exactly once, here). Keyed on the runtime effective
 * physical address (KUSEG/KSEG0/KSEG1 alias the same DRAM).
 *
 * DMACycleSteal residual: Beetle adds the (dynamic) DMACycleSteal to EVERY read
 * (libretro.cpp:868-869). That is non-zero only while a DMA channel is actively
 * stealing the bus; modeling it needs the live steal count threaded out of the DMA
 * controller, and it can't be isolated by a static ruler. It remains an unmodeled
 * dynamic axis; the per-region device waits below are the static, validatable piece. */
extern void psx_advance_cycles(uint32_t cycles);

/* Beetle MemRW device-region READ wait (libretro.cpp:859-1131), the device-dependent
 * part of a load's access cost (added to the timestamp before the +completion). `size`
 * is the access width in bytes (1/2/4) — the SPU and CDC waits are width-dependent.
 * Reads only; writes are posted (~free) in MemRW. phys is the masked physical address. */
static inline uint32_t psx_mmio_read_wait(uint32_t phys, uint32_t size) {
    /* Main RAM, mirrored across phys 0..0x7FFFFF (libretro.cpp:874 `A < 0x00800000`). */
    if (phys < 0x00800000u) return 3u;
    /* BIOS ROM (905) and Expansion 1 / PIO (1134): no extra device wait. */
    if (phys >= 0x1FC00000u && phys <= 0x1FC7FFFFu) return 0u;
    if (phys >= 0x1F000000u && phys <= 0x1F7FFFFFu) return 0u;
    /* Hardware MMIO window 0x1F801000..0x1F802FFF (921). */
    if (phys >= 0x1F801000u && phys <= 0x1F802FFFu) {
        /* Bisect gate (PSX_MMIO_WAIT=0): disable the device-region read waits
         * (the 9ae534d feature) to test whether they move the MMX6 cutscene
         * ordering. Read once. */
        static int s_mw = -1;
        if (s_mw < 0) { const char* e = getenv("PSX_MMIO_WAIT"); s_mw = (e && e[0] == '0') ? 0 : 1; }
        if (!s_mw) return 0u;
        if (phys >= 0x1F801C00u && phys <= 0x1F801FFFu)            /* SPU (929) */
            return (size == 4u) ? 36u : 16u;
        if (phys >= 0x1F801800u && phys <= 0x1F80180Fu)            /* CDC (979) */
            return 6u * size;
        if (phys >= 0x1F801810u && phys <= 0x1F801817u) return 1u; /* GPU (994) */
        if (phys >= 0x1F801820u && phys <= 0x1F801827u) return 1u; /* MDEC (1007) */
        if (phys >= 0x1F801000u && phys <= 0x1F801023u) return 1u; /* SysControl (1020) */
        if (phys >= 0x1F801040u && phys <= 0x1F80104Fu) return 1u; /* FrontIO/pad (1043) */
        if (phys >= 0x1F801050u && phys <= 0x1F80105Fu) return 1u; /* SIO (1055) */
        if (phys >= 0x1F801070u && phys <= 0x1F801077u) return 1u; /* IRQ (1094) */
        if (phys >= 0x1F801080u && phys <= 0x1F8010FFu) return 1u; /* DMA (1106) */
        if (phys >= 0x1F801100u && phys <= 0x1F80113Fu) return 1u; /* Timers (1119) */
        return 0u;   /* unmatched MMIO: Beetle adds no device wait */
    }
    return 0u;       /* unknown / open-bus region */
}

/* Beetle ReadMemory data-access timing (cpu.cpp:369-448), after §1/deps/DO_LDS.
 * compl_cost = 2 (CPU load) / 1 (LWC2); arm_rt = GPR to arm as pending load, or
 * 0x20 = none (LWC2, dest is a GTE reg). size = access width in bytes (1/2/4). */
static inline void psx_cyc_readmem(CPUState* cpu, uint32_t phys, uint32_t size,
                                   uint32_t compl_cost, uint32_t arm_rt) {
    /* ReadMemory start (369-370): clear the current give-back slot. */
    cpu->read_absorb[cpu->read_absorb_which] = 0u;
    cpu->read_absorb_which = 0u;
    /* Scratchpad (the D-cache): no wait, no give-back (414-422). */
    if (phys >= 0x1F800000u && phys <= 0x1F8003FFu) {
        cpu->ld_absorb = 0u;
        cpu->ld_which_t = (uint8_t)arm_rt;
        return;
    }
    /* fudge (424): +2 iff the predecessor committed no load (read_fudge==0x20). */
    psx_advance_cycles((uint32_t)((cpu->read_fudge >> 4) & 2u));
    uint32_t region = psx_mmio_read_wait(phys, size);  /* device-region wait */
    uint32_t cost = region + compl_cost;               /* LDAbsorb = region + completion */
    cpu->ld_absorb = cost;
    psx_advance_cycles(cost);
    cpu->ld_which_t = (uint8_t)arm_rt;
    /* PROOF GATE (PSX_POLL_PROOF=N, default 0/off): a FLAT, non-absorbed extra N
     * cycles per main-RAM data read — replicates the historical "+6 cyc/main-RAM
     * read" fix (mmx6_memcard_invalid_rootcause) that lengthened MMX6's card
     * busy-poll so it outlasts the VBlank-paced async card op. Unlike the
     * region+completion above (which arms ld_absorb and gets given back in a
     * tight loop), this is pure added cost. If setting this makes the MMX6 save
     * load, the card regression is confirmed as poll-vs-op timing. TEMPORARY —
     * the faithful fix models the uncached data-read cost properly. */
    if (phys < 0x00800000u) {
        static int s_pp = -1;
        if (s_pp < 0) { const char* e = getenv("PSX_POLL_PROOF"); s_pp = (e && e[0]) ? atoi(e) : 0; }
        if (s_pp > 0) psx_advance_cycles((uint32_t)s_pp);
    }
}

/* The interlock half of a load (§1+deps+(cancel)+DO_LDS+ReadMemory). Gated on
 * PSX_ENABLE_BLOCK_CYCLES so the Beetle-oracle build (cycles off) does a plain read. */
static inline void psx_cyc_load_timing(CPUState* cpu, uint32_t addr, uint32_t size,
                                       uint32_t rt, uint32_t reg_mask) {
#ifdef PSX_ENABLE_BLOCK_CYCLES
    /* Bisect gate (PSX_LOAD_DELAY=0): disable the R3000A load-delay interlock
     * timing (the d8c4a8e/fade560/d597797 feature) to test whether it moves the
     * MMX6 cutscene ordering. Read once; default on. */
    static int s_ld = -1;
    if (s_ld < 0) { const char* e = getenv("PSX_LOAD_DELAY"); s_ld = (e && e[0] == '0') ? 0 : 1; }
    if (!s_ld) { (void)addr; (void)size; (void)rt; (void)reg_mask; return; }
    psx_cyc_base(cpu);
    psx_cyc_deps(cpu, reg_mask);
    if (cpu->ld_which_t == rt) cpu->ld_which_t = 0u;   /* cancel pending load to same dest */
    psx_cyc_lds(cpu);
    psx_cyc_readmem(cpu, addr & 0x1FFFFFFFu, size, 2u, rt);
#else
    (void)cpu; (void)addr; (void)size; (void)rt; (void)reg_mask;
#endif
}

uint32_t psx_cyc_load_word(CPUState* cpu, uint32_t addr, uint32_t rt, uint32_t reg_mask) {
    psx_cyc_load_timing(cpu, addr, 4u, rt, reg_mask);
    return psx_read_word(addr);
}
uint16_t psx_cyc_load_half(CPUState* cpu, uint32_t addr, uint32_t rt, uint32_t reg_mask) {
    psx_cyc_load_timing(cpu, addr, 2u, rt, reg_mask);
    return psx_read_half(addr);
}
uint8_t psx_cyc_load_byte(CPUState* cpu, uint32_t addr, uint32_t rt, uint32_t reg_mask) {
    psx_cyc_load_timing(cpu, addr, 1u, rt, reg_mask);
    return psx_read_byte(addr);
}

/* LWC2 (GTE load): §1/DO_LDS done by psx_cyc_step(cpu,0); the GTE deadline stall by
 * psx_gte_stall — both emitted before this call. 32-bit access, completion +1, no
 * LDWhich arm. */
uint32_t psx_cyc_lwc2_read(CPUState* cpu, uint32_t addr) {
#ifdef PSX_ENABLE_BLOCK_CYCLES
    psx_cyc_readmem(cpu, addr & 0x1FFFFFFFu, 4u, 1u, 0x20u);
#else
    (void)cpu;
#endif
    return psx_read_word(addr);
}

/* Deprecated uncharged passthroughs (the +4 flat wait-state model is gone; load
 * timing now lives in psx_cyc_load_*). Kept so any stray reference stays valid. */
uint32_t psx_guest_read_word(uint32_t addr) { return psx_read_word(addr); }
uint16_t psx_guest_read_half(uint32_t addr) { return psx_read_half(addr); }
uint8_t  psx_guest_read_byte(uint32_t addr) { return psx_read_byte(addr); }

static void psx_write_byte_raw(uint32_t addr, uint8_t val);
void psx_write_byte(uint32_t addr, uint8_t val) {
    if (g_ls_mode == 2) { ls_write_hook(addr, 1, val); return; }
    if (g_ls_mode != 1 || s_ls_op_active || g_ls_suppress_record || g_dma_exec_depth > 0) { psx_write_byte_raw(addr, val); return; }
    if (!psx_get_in_exception()) ls_write_hook(addr, 1, val);
    s_ls_op_active = 1;
    psx_write_byte_raw(addr, val);
    s_ls_op_active = 0;
}
static void psx_write_byte_raw(uint32_t addr, uint8_t val) {
    g_guest_store_count++;
    gte_precision_invalidate_word(addr);
    if (sr_ptr && (*sr_ptr & 0x10000u)) return;

        /* KSEG2 guard — see psx_read_word_raw. */
    if (addr >= 0xC0000000u) { g_kseg2_ignored_writes++; return; }
    uint32_t phys = psx_phys_addr(addr);

    if (phys < RAM_SIZE) {
        debug_server_trace_write_check(phys, (uint32_t)ram[phys], (uint32_t)val, 1);
        parity_trace_note_write(phys, 1, effective_store_pc());
        card_data_writes_check(phys, (uint32_t)val, 1);
        dirty_ram_mark_kernel_write(phys);
        text_guard_note_write(phys, (uint32_t)val, 1);
        overlay_watch_note_write(phys, 1);
#ifdef PSX_COSIM
        { extern void cosim_note_ram_write(uint32_t,uint32_t); cosim_note_ram_write(phys, 1); }
#endif
        ram[phys] = val;
        return;
    }
    if (phys >= 0x1F000000u && phys <= 0x1F7FFFFFu) return;
    if (phys >= 0x1F800000u && phys <= 0x1F8003FFu) {
        debug_server_trace_write_check(phys, (uint32_t)scratchpad[phys - 0x1F800000u],
                                       (uint32_t)val, 1);
        scratchpad[phys - 0x1F800000u] = val;
        return;
    }
    if (phys >= 0x1F801000u && phys <= 0x1F803FFFu) {
        mmio_write8(phys, val);
        return;
    }
    if (phys >= 0x1FC00000u && phys <= 0x1FC7FFFFu) {
        return; /* ROM: ignore */
    }
    unmapped_fatal(addr, phys, "WRITE");
}
