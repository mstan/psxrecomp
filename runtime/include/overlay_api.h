#ifndef OVERLAY_API_H
#define OVERLAY_API_H

/* Shared ABI between psx-runtime.exe and overlay DLLs.
 *
 * After LoadLibrary, the runtime calls overlay_init() with this struct.
 * The DLL stores the pointers and routes all cross-module calls through them.
 */

#include "cpu_state.h"
#include <stdint.h>

/* ABI version exported by every overlay DLL as `overlay_abi()`.  The loader
 * rejects (and deletes, so autocompile regenerates) any DLL whose version
 * doesn't match — including all pre-versioning DLLs, which lack the export.
 *
 * v2: dispatch call-contract (Bug D family) — DLL call sites carry (ra, sp)
 *     contract checks and share the runtime's bail state via the appended
 *     callback pointers below.
 * v3: widescreen backdrop screenX squash ([widescreen.backdrop] x_sites) —
 *     overlay backdrop handlers now emit psx_ws_backdrop_x() at their screenX
 *     store. This is an EMIT-content change (not a struct-ABI change), but the
 *     version is the cache discriminator: bumping it forces the loader to
 *     reject pre-backdrop DLLs so autocompile regenerates them with the squash.
 *     (The deferred follow-up is to auto-derive this tag from a hash of the
 *     widescreen config + emit headers; until then it's a manual bump.)
 * v4: widescreen backdrop column PRELOAD ([widescreen.cull] auto_backdrop) —
 *     overlay backdrop generators now emit psx_ws_backdrop_value() at the
 *     window START/END finalize, and the callback struct grows a
 *     ws_backdrop_value pointer (appended last). Bumping rejects pre-preload
 *     DLLs (which lack both the emit and a host that supplies the callback). */
/*   v5: psx_syscall callback return type void->int (CPS, RECURSION_BUG.md §25);
 *       overlays compiled under CPS emit `if (psx_syscall(...)) return;`. */
/*   v7: advance_cycles callback added; overlays now built with
 *       PSX_ENABLE_BLOCK_CYCLES and charge the shared host cycle/timer timeline.
 *       Bumping rejects stale DLLs that charged no cycles (Tomba2 Timer1 fork). */
/*   v8: check_interrupts_at callback added so generated native overlay block
 *       checks can provide the real guest resume PC for async RFE recovery.
 *   v9: faithful-timing callbacks (cyc_load, icache_fetch, muldiv, mult_latency,
 *       gte, slice_block) so overlay code built with PSX_ENABLE_BLOCK_CYCLES links
 *       against the runtime's real timing impls (the cycle-accuracy rearchitecture).
 *   v10: GTE special-register accessors (gte_read_data/gte_read_ctrl/
 *        gte_write_data/gte_write_ctrl) — the emitter emits direct calls for
 *        flag/IR/derived GTE regs; GTE-heavy overlays failed to LINK without
 *        shims (undefined gte_read_ctrl), silently blocking new coverage. */
/* v11 batches per-instruction cycle charges inside overlay DLLs and flushes at
 * block/device/store boundaries, removing a cross-DLL callback per instruction
 * without changing the shared guest-cycle timeline. */
#define PSX_OVERLAY_ABI_VERSION 11

/* Codegen flavor of the recompiled output the overlays + runtime were built
 * against. Overlays are keyed in the cache by guest-bytes CRC, which is
 * flavor-BLIND — the same guest overlay yields the same filename whether it was
 * compiled by the base recompiler or a variant (e.g. widescreen, which emits
 * extra runtime symbols + GTE adjustments). Without a flavor tag a widescreen
 * DLL and a base DLL for the same overlay collide. The flavor is folded into
 * overlay_abi() (high 16 bits) so the loader's existing ABI gate rejects (and
 * regenerates) any DLL whose flavor differs from the running build's, keeping
 * caches separate even in a shared directory.
 *
 * Base/master MUST stay 0 so the tag == PSX_OVERLAY_ABI_VERSION (backward
 * compatible with all existing base-flavor DLLs). A variant build overrides it
 * (e.g. -DPSX_OVERLAY_FLAVOR=1 for widescreen) for BOTH the runtime and
 * compile_overlays.py (its --flavor arg). */
#ifndef PSX_OVERLAY_FLAVOR
#define PSX_OVERLAY_FLAVOR 0
#endif

/* Combined tag exported by overlay_abi() and checked by the loader. */
#define PSX_OVERLAY_ABI_TAG \
    ((int)((PSX_OVERLAY_ABI_VERSION & 0xFFFF) | ((PSX_OVERLAY_FLAVOR & 0xFFFF) << 16)))

/* Codegen version — bumped whenever the recompiler emits DIFFERENT bytes for the
 * same guest overlay WITHOUT an ABI change (the load-time overlay_abi() gate only
 * catches ABI/flavor changes; a pure-emitter change produces a different DLL the
 * gate would happily accept). The cache is namespaced by this version
 * (gcc/<arch-abi>/cg<N>/), so a build with new codegen writes + reads a FRESH
 * directory and never reuses a stale DLL; old versions coexist on disk (no
 * auto-delete — a user who downgrades still finds their matching cache). Python
 * (compile_overlays.py) parses this same constant from this header, so the two
 * sides can never drift. BUMP THIS when you change code_generator.cpp emit in a
 * way that alters overlay output (e.g. the auto_screen_x widescreen widening).
 *   1: baseline + auto_screen_x render-funnel cull via psx_ws_cull_sltiu.
 *   2: + auto_backdrop far-backdrop column preload via psx_ws_backdrop_value.
 *   3: auto_backdrop switched to camera-tracked window WIDENING
 *      (psx_ws_backdrop_value gains a window_cols arg).
 *   4: continuation-passing (PSX_CPS, RECURSION_BUG.md §25) — overlay funcs
 *      tail-transfer + carry an entry-switch; the loader routes continuations.
 *      Fresh namespace so CPS DLLs never reuse a legacy (unit-model) cg3 DLL. */
#define PSX_OVERLAY_CODEGEN_VER 4

/* Auto codegen hash (generated by hash_codegen.cmake from the recompiler codegen
 * sources). Folded into the cache PATH next to cg<N> (gcc/<arch-abi>/cg<N>_<hash>/)
 * so ANY emitter change auto-invalidates the cache, even within the same manual
 * PSX_OVERLAY_CODEGEN_VER — closing the stale-but-cgN reuse that caused the v0.3.0
 * black screen. The build writes overlay_codegen_hash.h (gitignored) into this
 * dir; a fresh checkout without it falls back to 0 (== old single-cgN behaviour)
 * until the first build. compile_overlays.py reads the same value, so the loader
 * and the compiler always agree on the path. */
#if defined(__has_include)
#  if __has_include("overlay_codegen_hash.h")
#    include "overlay_codegen_hash.h"
#  endif
#endif
#ifndef PSX_OVERLAY_CODEGEN_HASH
#  define PSX_OVERLAY_CODEGEN_HASH 0u
#endif

typedef struct {
    /* Core dispatch: routes call_by_address() and out-of-overlay jal */
    void (*dispatch_call)(CPUState *cpu, uint32_t addr, uint32_t ra);
    /* Interrupt check: called after every function return in overlay */
    void (*check_interrupts)(CPUState *cpu);
    /* Interrupt check with the guest PC that should resume if a game-installed
     * handler later RFEs to the sentinel outside the synchronous host window. */
    void (*check_interrupts_at)(CPUState *cpu, uint32_t resume_pc);
    /* Guest-cycle accounting (ABI v7): block-cycle charge. Overlay code built
     * with PSX_ENABLE_BLOCK_CYCLES must charge the SAME shared host cycle/timer
     * timeline as the dirty-RAM interpreter and the BIOS, or timer-sensitive
     * code reads different values per backend (Tomba2 logo Timer1 fork). */
    void (*advance_cycles)(uint32_t cycles);
    /* GTE coprocessor 2 execution */
    void (*gte_execute)(CPUState *cpu, uint32_t cmd);
    /* MIPS syscall (break/syscall instructions). Returns 1 if control
     * transfers (cpu->pc set), 0 for a directly-handled void syscall. See
     * cpu_state.h. The signature changed with CPS (RECURSION_BUG.md §25), so
     * PSX_OVERLAY_ABI_VERSION was bumped to reject stale overlay caches. */
    int  (*psx_syscall)(CPUState *cpu, uint32_t code);
    /* Unresolved dispatch target */
    void (*psx_unknown_dispatch)(CPUState *cpu, uint32_t addr, uint32_t phys);
    /* Debug instrumentation: called at every function entry (may be NULL) */
    void (*log_call_entry)(uint32_t func_addr);
    /* RestoreState/ReturnFromException longjmp escape (interrupts.c). The
     * recompiler emits this at longjmp-return sites in exception-context
     * kernel code (the install-slot / kernel-window class). Appended LAST:
     * overlay_init copies the struct by value, so older DLLs built against
     * the shorter struct simply never read this member. */
    void (*psx_restore_state_escape)(void);
    /* Call-contract state shared with the runtime (ABI v2; see the contract
     * model in cpu_state.h).  DLL code reads the bail flag and bumps the
     * counters through these pointers. */
    int      *call_bail_flag;
    uint64_t *bail_first;
    uint64_t *bail_resolved;
    /* Widescreen hooks (ABI v3). The recompiler can emit psx_ws_* calls into
     * OVERLAY code (backdrop screenX squash; and, for other games, sprite-tag
     * or cull sites that resolve into overlays). These compute against the
     * host's live widescreen state (gpu.c), so the DLL forwards to the runtime
     * through these. Appended LAST (struct grows back-compatibly); may be NULL
     * on a host that predates them — the DLL glue falls back to identity. */
    int  (*ws_backdrop_x)(int x);
    int  (*ws_x_margin)(void);
    void (*ws_sprite_tag)(CPUState *cpu);
    /* Widescreen backdrop column-preload value substitution (ABI v4). Overlay
     * backdrop generators call this at a detected window START/END bound; the
     * runtime returns `orig` unless native-wide is engaged, in which case it
     * widens the camera-tracked window by the 16:9 reveal (START -> orig-margin,
     * END -> orig+margin; margin scales with window_cols). Appended LAST (struct
     * grows back-compatibly); may be NULL on a host that predates it — the DLL
     * glue falls back to orig. */
    uint32_t (*ws_backdrop_value)(uint32_t orig, int is_end, int window_cols);
    /* Fail-closed native entry guard (ABI v6). The generated CPS entry-switch
     * calls this when the function is dispatched at a PC that is not one of its
     * legal entries (a foreign interior PC from a range-ownership mismatch); the
     * function returns without executing and the runtime routes the PC to the
     * sanctioned dirty-RAM interpreter. Appended LAST (struct grows back-
     * compatibly); may be NULL on a host that predates it. */
    void (*psx_native_bad_entry)(CPUState *cpu, uint32_t owner, uint32_t pc);
    /* Faithful-timing functions (ABI v9). The cycle-accuracy + due-cycle-scheduler
     * rearchitecture made the recompiler emit these into OVERLAY code too (block-cycle
     * mode). They touch shared runtime state (guest RAM, the global cycle counter, the
     * I-cache tag array, the mul/div & GTE completion deadlines, the precise-slice
     * interp), so the DLL MUST forward them to the runtime — a local copy would diverge
     * from the interp/BIOS timeline (the exact class the cosim guards). Appended LAST
     * (struct grows back-compatibly); the ABI bump to 9 rejects any pre-timing cache. */
    uint32_t (*cyc_load_word)(CPUState *cpu, uint32_t addr, uint32_t rt, uint32_t reg_mask);
    uint16_t (*cyc_load_half)(CPUState *cpu, uint32_t addr, uint32_t rt, uint32_t reg_mask);
    uint8_t  (*cyc_load_byte)(CPUState *cpu, uint32_t addr, uint32_t rt, uint32_t reg_mask);
    uint32_t (*cyc_lwc2_read)(CPUState *cpu, uint32_t addr);
    void     (*icache_fetch)(CPUState *cpu, uint32_t addr);
    void     (*muldiv_set)(CPUState *cpu, uint32_t latency);
    void     (*muldiv_stall)(CPUState *cpu);
    uint32_t (*mult_latency_s)(uint32_t rs);
    uint32_t (*mult_latency_u)(uint32_t rs);
    void     (*gte_stall)(CPUState *cpu);
    void     (*gte_read)(CPUState *cpu, uint32_t rt);
    int      (*slice_block)(CPUState *cpu, uint32_t block_addr, uint32_t bcyc, int side_effects);
    /* GTE special-register accessors (ABI v10). The emitter routes mfc2/cfc2/
     * mtc2/ctc2 on flag/IR/derived registers through these (code_generator.cpp
     * COP2 case) — a GTE-heavy overlay (Tomba2 lava/Trolley attract demos)
     * emits direct calls and its DLL link fails without shims (undefined
     * gte_read_ctrl — the class that silently blocked ALL new overlay coverage
     * after the v9 caches rotated). Appended last; ABI bump to 10 rejects any
     * pre-shim DLL. */
    uint32_t (*gte_read_data)(CPUState *cpu, uint8_t reg);
    uint32_t (*gte_read_ctrl)(CPUState *cpu, uint8_t reg);
    void     (*gte_write_data)(CPUState *cpu, uint8_t reg, uint32_t value);
    void     (*gte_write_ctrl)(CPUState *cpu, uint8_t reg, uint32_t value);
} OverlayCallbacks;

#ifdef __cplusplus
extern "C" {
#endif

/* Exported by every overlay DLL.  Call once after LoadLibrary. */
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
void overlay_init(const OverlayCallbacks *cbs);

#ifdef __cplusplus
}
#endif

#endif /* OVERLAY_API_H */
