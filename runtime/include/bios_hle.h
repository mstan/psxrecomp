/* bios_hle.h — opt-in High-Level Emulation tier for PSX BIOS kernel services.
 *
 * CLAUDE.md §0 AMENDMENT 2026-07-02 (the gbarecomp model, see the sibling
 * gbarecomp project's src/runtime/bios_hle.h): the recompiled real
 * BIOS (LLE) is the DEFAULT, the reference implementation, and the
 * correctness/timing oracle. HLE is an *alternative backend*, selected
 * per-game via `[runtime] bios_hle = true` in game.toml (PSX_BIOS_HLE env
 * override). Every kernel service the HLE layer does not implement
 * transparently falls through to the recompiled BIOS, so HLE is never
 * load-bearing beyond what it covers and never becomes the oracle.
 *
 * Mechanism: psx_bios_hle_configure() installs (or leaves NULL) the dispatch
 * hook g_psx_bios_hle_hook consulted at the top of every psx_dispatch_impl
 * iteration (emitted by full_function_emitter.cpp), BEFORE any backend claims
 * the target. The hook sees the pre-normalize physical address; when the
 * target is a kernel service vector (0xA0/0xB0/0xC0, function number in $t1)
 * and the service is implemented, the handler computes its effect directly on
 * guest state (the REAL kernel structures in guest RAM — EvCB table etc., per
 * docs/psx_bios_disasm.txt + the SCPH1001 kernel disassembly), charges an
 * approximate cycle cost, and returns 1: the guest resumes at $ra with no
 * BIOS dispatch. NULL hook (default) = pure LLE, byte-identical dispatch.
 *
 * Boot HLE: with the tier on (and no keep-intro opt-out) the hook also
 * intercepts the first dispatch of the BIOS SHELL entry (RAM 0x30000 — the
 * boot animation; LoadRunShell's indirect `0x80030000()` call always routes
 * through the dispatcher) and returns immediately, so BIOS Main() proceeds
 * straight to step 8 (SYSTEM.CNF + game EXE load) with kernel state built
 * entirely by the REAL recompiled kernel init. The frontend runs unpaced
 * (turbo) until game handoff. This is THE boot-skip mechanism; it deprecates
 * the old fast_boot snapshot restore. LLE always plays the real intro.
 *
 * NO-STUBS STANDS: every handler here is a validated reimplementation of the
 * documented kernel mechanism operating on the real guest structures — never
 * a "return the answer" fake. Timing is approximate (documented limitation;
 * LLE remains the timing oracle).
 *
 * Observability (CLAUDE.md rule 3 + the global ring-buffer rule): every hook
 * decision is recorded in an always-on ring queryable over the TCP debug
 * server ("hle_dump"), never armed, never polled.
 */
#ifndef PSXRECOMP_BIOS_HLE_H
#define PSXRECOMP_BIOS_HLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/* The dispatch hook slot the generated trampoline consults (defined in
 * bios_hle.c, emitted-extern by full_function_emitter.cpp). NULL = pure LLE. */
extern int (*g_psx_bios_hle_hook)(struct CPUState* cpu, uint32_t phys);

/* Select the BIOS backend. Call at bring-up (and again on netplay rematch
 * session_reboot), after config + env resolution, before psx_scheduler_run.
 * Two independent axes:
 *   call_hle  — service implemented kernel calls in HLE ([runtime] bios_hle)
 *   boot_skip — one-shot shell intercept ((bios_hle && !keep_intro) OR the
 *               deprecated fast_boot alias, which skips boot only)
 * Clears the shell-skip latch so rematch can skip again. The hook is installed
 * when either axis is on; both off = NULL = pure LLE. */
void psx_bios_hle_configure(int call_hle, int boot_skip);

int         psx_bios_hle_enabled(void);
int         psx_bios_hle_boot_skip_enabled(void);
const char* psx_bios_hle_backend_name(void); /* startup banner */

/* True while the HLE boot-skip is driving the frontend unpaced: boot skip
 * enabled and the game entry PC has not yet been dispatched. main.cpp ORs
 * this into the turbo predicate so kernel init + EXE load run at host speed. */
int psx_bios_hle_boot_turbo_active(void);

/* ── Always-on HLE ring (queried by debug_server "hle_dump") ─────────────── */

enum {
    PSX_HLE_ROUTE_LLE  = 0,  /* vector call seen, fell through to recompiled BIOS */
    PSX_HLE_ROUTE_HLE  = 1,  /* serviced in HLE */
    PSX_HLE_ROUTE_BOOT = 2,  /* boot shell-skip fired */
};

typedef struct PsxHleCallEntry {
    uint64_t seq;
    uint64_t cycle;      /* psx_cycle_count at record time (first occurrence) */
    uint64_t cycle_last; /* psx_cycle_count of the most recent collapsed repeat */
    uint32_t vector;     /* 0xA0/0xB0/0xC0, or 0x30000 for the boot skip */
    uint32_t fn;         /* $t1 function number (0 for boot skip) */
    uint32_t a0, a1, a2, a3;
    uint32_t ra;
    uint32_t v0;         /* result for HLE-serviced calls (0 otherwise) */
    uint32_t repeat;     /* consecutive identical calls collapsed into this entry */
    uint32_t tcb;        /* current guest thread: [[0x108]] at call time (0 if unset) */
    uint8_t  route;      /* PSX_HLE_ROUTE_* */
    uint8_t  in_exc;     /* recorded inside guest exception context */
} PsxHleCallEntry;

#define PSX_HLE_RING_CAP 16384  /* power of two */

/* Snapshot accessors: seq is the total number of records ever written; entry
 * i (seq-CAP .. seq-1 valid window) returns the ring slot for that sequence. */
uint64_t               psx_hle_ring_seq(void);
const PsxHleCallEntry* psx_hle_ring_entry(uint64_t seq_index);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_BIOS_HLE_H */
