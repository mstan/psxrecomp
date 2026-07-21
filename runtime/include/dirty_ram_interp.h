/* dirty_ram_interp.h — interpret-on-dispatch for install-at-runtime RAM.
 *
 * See CLAUDE.md Rule 18 and docs/dynamic_handler_install.md for the full
 * rationale.  The PS1 BIOS dynamically writes 4-instruction dispatch stubs
 * into kernel RAM (notably RAM 0xCF0 for the SIO data-byte handler).  A
 * static recompiler can't see those bytes at compile time, so a small MIPS
 * interpreter here runs them at dispatch time on the same CPUState.
 *
 * Scope: this is NOT a fallback for code the recompiler failed to translate.
 * It runs only against PCs in pages that have been written-to since boot.
 * Static-recompiled code continues to handle ROM-resident code and game
 * RAM.  See docs/dynamic_handler_install.md for the inline note about a
 * potential future migration to runtime JIT (Option B).
 */
#ifndef PSXRECOMP_DIRTY_RAM_INTERP_H
#define PSXRECOMP_DIRTY_RAM_INTERP_H

#include <stdint.h>
#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 1 if `addr` lies in a dirty kernel-RAM page and the interpreter
 * ran a basic block at that PC.  Returns 0 if `addr` is clean (caller must
 * fall back to the static dispatch table).  On return with 1, cpu->pc is
 * either 0 (block ended on jr $ra style return) or the target of a tail
 * jump that the dispatch trampoline should re-enter.
 *
 * `stop_addr` is the dispatch loop's return contract (psx_dispatch_call's
 * return_addr; 0 for plain tail dispatches).  The interpreter's overlay
 * local-flow chaining MUST NOT run past it: when a transfer (or fall-
 * through) reaches stop_addr, the interpreter exits with cpu->pc ==
 * stop_addr so the dispatch loop returns into the suspended native
 * caller.  Skipping this check lets the interp execute the native
 * caller's tail itself; the suspended native host frame then resumes and
 * double-executes that tail against a moved guest stack, restoring
 * garbage callee-saved registers (the pig-throw blue-screen/MMIO-fatal
 * corruption, 2026-06-10). */
int dirty_ram_dispatch(CPUState* cpu, uint32_t addr, uint32_t stop_addr);

/* Overlay-cache windows — the address ranges eligible for capture, offline
 * recompilation, and per-entry-validated native execution (Rule 18 code that
 * does not exist in any compile-time image):
 *
 *   Kernel RAM   [0x00000, 0x10000): the BIOS part-2 image relocated to RAM
 *     plus install-at-runtime stubs (e.g. the SIO data-byte stub at 0xCF0).
 *     Dirty-tracked per CPU store (dirty_ram_mark_kernel_write).
 *   Overlay region [OVERLAY_REGION_FLOOR, RAM_SIZE): game overlays loaded by
 *     CD DMA (dirty_ram_mark_executable_range).
 *
 * Main-EXE text [0x10000, OVERLAY_REGION_FLOOR): CLEAN pages are statically
 * recompiled and never captured. But games that discard their boot/init code
 * load gameplay overlays OVER this region (Tomba 2: hot gameplay code at
 * 0x1DA88/0x24548/0x3116C, well below its 0x38800 floor). Post-baseline
 * (dirty_ram_clear_image_baseline) a DIRTY page here is by definition live
 * overlay code — the compiled image is stale for it — so dirty boot-text
 * pages ARE a capture/candidate window (2026-07-06; before this they could
 * only interpret, which dominated wall time at ~300ns/insn).
 *
 * The interpreter's LOCAL-FLOW gates (is_local_dirty_target /
 * phys_is_overlay_flow_region) use DIRTY_RAM_KERNEL_WINDOW_END alone: only
 * the kernel window stays per-block (it runs in exception context where the
 * verified dispatch cadence is delicate). Native coverage for the kernel
 * window comes from the overlay loader, not from interp chaining. */
#define DIRTY_RAM_KERNEL_WINDOW_END 0x00010000u

/* The overlay-region floor is the END of THIS game's statically-recompiled
 * main-EXE text (phys). Everything at/above it is runtime-loaded overlay code,
 * eligible for in-interpreter local-flow chaining and overlay-cache capture.
 *
 * It is a RUNTIME value (g_overlay_region_floor), NOT a compile-time constant,
 * because it differs per game: Tomba 1's text ends at 0x98000 (0x10000+0x88000)
 * but Tomba 2's boot EXE is only 0x28800 (text ends at 0x38800). A hardcoded
 * 0x98000 misclassified Tomba 2's overlays (loaded at 0x85000+) as main-EXE
 * text, forcing them onto the slow block-by-block dispatch path AND through the
 * bail-prone non-local-call contract — the Whoopee-Camp splash freeze. main.cpp
 * sets g_overlay_region_floor = (load_address + text_size) & 0x1FFFFFFF at game
 * load; OVERLAY_REGION_FLOOR_DEFAULT applies for BIOS-only runs. */
#define OVERLAY_REGION_FLOOR_DEFAULT 0x00098000u
extern uint32_t g_overlay_region_floor;
#define OVERLAY_REGION_FLOOR (g_overlay_region_floor)

/* Test whether a given physical kernel-RAM address is in a page that was
 * written-to since boot.  Defined in memory.c. */
int      dirty_ram_is_dirty(uint32_t phys);

/* Kernel-image bless (memory.c). A dispatch key in the relocated kernel
 * window (RAM [0x500,0x8500), the BIOS's boot-time copy of ROM 0x1FC10000+)
 * may run its statically-recompiled native function IFF the live RAM bytes
 * of the code reachable from it (emitter-supplied body extent) byte-match
 * the ROM source. Lazily verified, cached per entry, invalidated precisely
 * on writes into the body. Runtime-patched bodies (pad/SIO install stubs)
 * never verify and keep interpreting — faithful either way. */
int      psx_kernel_bless_dispatchable(uint32_t phys);
void     psx_kernel_bless_note_range(uint32_t phys, uint32_t len);
void     psx_kernel_bless_stats(uint64_t out[6]);

/* Capture/candidate window membership. Kernel window and overlay region are
 * unconditional; main-EXE text [KERNEL_WINDOW_END, FLOOR) is included only
 * when the page is dirty — i.e. a gameplay overlay overwrote the boot text
 * after the game-start baseline, so the bytes there are runtime overlay code
 * (see the window model note above). Clean boot text never enters the window
 * (it runs compiled; capturing it would violate the static-first design). */
static inline int overlay_cache_window_contains(uint32_t phys) {
    return phys < DIRTY_RAM_KERNEL_WINDOW_END
        || phys >= OVERLAY_REGION_FLOOR
        || dirty_ram_is_dirty(phys);
}
uint32_t dirty_ram_get_bitmap(void);
uint32_t dirty_ram_get_bitmap_word(uint32_t word_index);
uint32_t dirty_ram_get_bitmap_word_count(void);
void     dirty_ram_mark_executable_range(uint32_t phys, uint32_t len);
void     dirty_ram_register_text_image(uint32_t phys_lo, const uint8_t *bytes,
                                       uint32_t len);
int      dirty_ram_text_native_ok(uint32_t phys);
int      dirty_ram_text_image_registered(void);
/* Bless an intentional runtime data patch (e.g. text_xlate string/glyph tables)
 * into the text reference image so it is not mistaken for self-modifying code. */
void     dirty_ram_text_bless(uint32_t phys, const uint8_t *bytes, uint32_t len);
uint64_t dirty_ram_text_native_blocked(void);
uint32_t dirty_ram_text_diverged_pages(void);

/* Counters for visibility / TCP debug.  Increment in interpreter; expose
 * via debug_server.c if helpful. */
extern uint64_t g_dirty_ram_blocks_run;     /* basic blocks interpreted */
extern uint64_t g_dirty_ram_insns_run;      /* instructions interpreted */
extern uint64_t g_dirty_window_dispatches;  /* interp dispatches inside a
                                             * capture window (autocapture
                                             * pressure signal, step 2.8) */
extern uint64_t g_dirty_ram_aborts;         /* unsupported-opcode aborts */
extern uint64_t g_dirty_ram_guard_yields;   /* long dirty loops yielded */
extern uint64_t g_dirty_ram_unsupported_midblock;
extern uint32_t g_dirty_ram_last_unsupported_entry;
extern uint32_t g_dirty_ram_last_unsupported_entry_ra;
extern uint32_t g_dirty_ram_last_unsupported_entry_sp;
extern uint32_t g_dirty_ram_last_unsupported_insns;
extern uint32_t g_dirty_ram_last_unsupported_pc;
extern uint32_t g_dirty_ram_last_unsupported_insn;
extern const char *g_dirty_ram_last_unsupported_reason;

/* Per-entry-PC counters.  Aggregate counters above hide which install
 * stubs actually fire — a single noisy spurious-dispatch site can mask
 * a legitimate handler that never runs.  This open-addressed table keys
 * on the entry PC of each interpreted block. */
/* 65536 -> 262144 (2026-07-03): Tomba2's Trolley attract demo interprets a
 * working set past 64K distinct PCs; a full table degraded every lookup to a
 * full-table scan (0.3 fps host burn in pc_table_get_or_insert_in) and
 * silently truncated overlay-capture seed lists (MAX_CAPTURE_PCS aliases
 * this). Probe length is bounded in pc_table_get_or_insert_in either way. */
#define DIRTY_RAM_PC_TABLE_SIZE 262144
typedef struct {
    uint32_t pc;         /* entry PC, 0 = empty slot */
    uint64_t hits;       /* number of times dispatched here */
    uint64_t insns;      /* total instructions executed across hits */
    uint64_t entry_hits; /* dispatches that arrived from NATIVE code (call /
                          * dispatch loop), not from the interpreter's own
                          * block-to-block chaining. `hits` conflates the two:
                          * below the local-flow floor every taken branch
                          * re-dispatches, so chain blocks dominate. entry_hits
                          * is the evidence stream for interior-alias seeds. */
} DirtyRamPcEntry;
extern DirtyRamPcEntry g_dirty_ram_pc_table[DIRTY_RAM_PC_TABLE_SIZE];
/* Every aligned main-RAM word is a possible instruction PC.  Execution
 * coverage only needs presence, not a hit count, so record it in a direct
 * bitmap instead of probing a large hash table for every retired instruction.
 * This covers all 524,288 RAM words (the old 262K-entry hash could saturate)
 * and is also the execution-verified seed source used by overlay_capture. */
#define DIRTY_RAM_EXEC_WORD_COUNT   ((2u * 1024u * 1024u) / 4u)
#define DIRTY_RAM_EXEC_BITMAP_WORDS ((DIRTY_RAM_EXEC_WORD_COUNT + 31u) / 32u)
extern uint32_t g_dirty_ram_exec_pc_bitmap[DIRTY_RAM_EXEC_BITMAP_WORDS];
/* One bit per 4 KiB page. RAM writes use this as a one-test stale-evidence
 * guard; they clear that page's capture bits rather than serializing from the
 * universal store hot path. */
#define DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS 16u
extern uint32_t g_dirty_ram_exec_page_bitmap[DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS];
/* Presence-only companion for interpreted block/dispatch entries. The richer
 * counter table remains for telemetry, while capture can snapshot/reset this
 * compact evidence independently at overlay-generation boundaries. */
extern uint32_t g_dirty_ram_dispatch_pc_bitmap[DIRTY_RAM_EXEC_BITMAP_WORDS];

/* Block-entry ring buffer. Records every dispatch into dirty RAM with the
 * caller's RA at entry, plus argument context — answers
 * "who tried to JALR into this RAM stub, with what args".
 * Always-on, eviction keeps memory bounded; callers query the window of
 * interest (CLAUDE.md global rule on ring buffers).
 *
 * Limitation: this captures dispatches into RAM-resident code only. ROM
 * recompiled-C → recompiled-C calls (direct C function calls in
 * generated dispatch table) are NOT captured here. For BIOS shell
 * investigation that's fine — shell code lives in RAM (0x800XXXXX).
 *
 * `ra` is cpu->gpr[31] at dispatch time. For normal JAL-style calls,
 * (ra - 8) gives the caller's PC. For J/JR/tail-calls it's only a
 * heuristic — treat ra as "ra_callsite_guess", not authoritative. */
#ifdef PSX_NO_DEBUG_TOOLS
/* Keep ABI-visible one-element sentinels so crash/debug stubs still link, while
 * Release does not reserve the diagnostic rings' ~227 MiB of address space. */
#define DIRTY_RAM_BLOCK_LOG_CAP 1u
#else
#define DIRTY_RAM_BLOCK_LOG_CAP (1u << 22) /* 4M entries (~224 MiB at 56 B each).
 * At ~580K dispatches/s during boot, this retains ~7s of history; at
 * ~10K/s during modal idle, ~400s. Sized for retroactive press-window
 * analysis without the prior 16K ring's 28-ms eviction problem. */
#endif
typedef struct {
    uint64_t seq;       /* monotonic, unique per entry */
    uint32_t target;    /* entry PC (RAM address) */
    uint32_t ra;        /* cpu->gpr[31] at dispatch — caller's return target */
    uint32_t a0;        /* cpu->gpr[4] at dispatch */
    uint32_t a1;        /* cpu->gpr[5] at dispatch */
    uint32_t a2;        /* cpu->gpr[6] at dispatch */
    uint32_t a3;        /* cpu->gpr[7] at dispatch */
    uint32_t t0;        /* cpu->gpr[8] at dispatch */
    uint32_t t1;        /* cpu->gpr[9] at dispatch */
    uint32_t t2;        /* cpu->gpr[10] at dispatch */
    uint32_t sp;        /* cpu->gpr[29] at dispatch */
    uint32_t frame;     /* s_frame_count at the time of dispatch */
} DirtyRamBlockLogEntry;
extern DirtyRamBlockLogEntry g_dirty_ram_block_log[DIRTY_RAM_BLOCK_LOG_CAP];
extern uint64_t              g_dirty_ram_block_log_seq;

#ifdef PSX_NO_DEBUG_TOOLS
#define DIRTY_RAM_FLOW_LOG_CAP 1u
#else
#define DIRTY_RAM_FLOW_LOG_CAP (1u << 16)
#endif
typedef struct {
    uint64_t seq;
    uint32_t pc;
    uint32_t target;
    uint32_t ra;
    uint32_t a0;
    uint32_t a1;
    uint32_t a2;
    uint32_t a3;
    uint32_t sp;
    uint32_t frame;
} DirtyRamFlowLogEntry;
extern DirtyRamFlowLogEntry g_dirty_ram_flow_log[DIRTY_RAM_FLOW_LOG_CAP];
extern uint64_t             g_dirty_ram_flow_log_seq;

#ifdef PSX_NO_DEBUG_TOOLS
#define DIRTY_RAM_INSN_LOG_CAP 1u
#else
#define DIRTY_RAM_INSN_LOG_CAP (1u << 16)
#endif
typedef struct {
    uint64_t seq;
    uint32_t pc;
    uint32_t insn;
    uint32_t next_pc;
    uint32_t target;
    uint32_t before_s0;
    uint32_t after_s0;
    uint32_t sp;
    uint32_t ra;
    uint32_t v0;
    uint32_t v1;
    uint32_t a0;
    uint32_t a1;
    uint32_t a2;
    uint32_t a3;
    uint32_t t0;
    uint32_t t1;
    uint32_t t2;
    uint32_t current_tcb;
    uint32_t task_ptr;
    uint32_t task_mode;
    uint32_t task_submode;
    uint32_t frame;
    uint8_t  transferred;
    uint8_t  pad[3];
} DirtyRamInsnLogEntry;
extern DirtyRamInsnLogEntry g_dirty_ram_insn_log[DIRTY_RAM_INSN_LOG_CAP];
extern uint64_t             g_dirty_ram_insn_log_seq;

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_DIRTY_RAM_INTERP_H */
