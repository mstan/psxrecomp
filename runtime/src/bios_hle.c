/* bios_hle.c — opt-in High-Level Emulation tier for PSX BIOS kernel services.
 *
 * See bios_hle.h for the design (LLE default + oracle, per-game opt-in,
 * unimplemented services fall through to the recompiled BIOS, boot shell-skip,
 * always-on observability ring).
 *
 * v1 service coverage — the B0 event family, ground-truthed instruction by
 * instruction against the SCPH1001 kernel (ROM 0xBFC11644..0xBFC11A84, kernel
 * RAM 0x1B44..0x1F8C; cross-checked with docs/psx_bios_disasm.txt B0 table @
 * 0x874 and PSX-SPX "Kernel: Events"):
 *
 *   B0:0x07 DeliverEvent(class, spec)
 *   B0:0x08 OpenEvent(class, spec, mode, func)
 *   B0:0x09 CloseEvent(event)
 *   B0:0x0B TestEvent(event)
 *   B0:0x0C EnableEvent(event)
 *   B0:0x0D DisableEvent(event)
 *
 * EvCB table: base pointer at kernel [0x120], size in bytes at [0x124], one
 * 0x1C-byte entry per event: +0x00 class, +0x04 status, +0x08 spec, +0x0C
 * mode, +0x10 func. Status values: 0 free, 0x1000 disabled, 0x2000 enabled,
 * 0x4000 ready/delivered. Mode values: 0x1000 = call callback on delivery,
 *0x2000 = mark ready (polled via TestEvent/WaitEvent).
 *
 * Everything else (WaitEvent — it blocks; threads; pads; the card/CD stack;
 * all of A0/C0) falls through to LLE in v1, exactly like gbarecomp's
 * machine-state SWIs. The recompiled BIOS stays fully load-bearing for them.
 *
 * TIMING NOTE: cycle costs charged here are approximations of the kernel
 * routine's dynamic instruction count (vector stub + table handler + body).
 * LLE accrues the exact count by executing; cycle-exact HLE is a documented
 * limitation of the opt-in tier. LLE remains the timing oracle.
 */

#include "bios_hle.h"

#include "cpu_state.h"
#include "psx_cycles.h"

#include <stddef.h>

/* fntrace.c — game-entry handoff latch (drives the boot turbo window). */
extern int fntrace_is_game_started(void);

/* ── hook slot (referenced by the generated dispatch trampoline) ─────────── */

int (*g_psx_bios_hle_hook)(struct CPUState* cpu, uint32_t phys) = NULL;

/* ── mode state ──────────────────────────────────────────────────────────── */

static int s_call_hle_on     = 0;   /* service kernel calls in HLE */
static int s_boot_skip_on    = 0;   /* one-shot shell intercept */
static int s_shell_skipped   = 0;   /* boot skip already fired */

/* SCPH1001 physical addresses. The shell is unpacked to RAM 0x30000 by
 * LoadRunShell (ROM 0xBFC06FF0: memcpy(0x80030000, 0xBFC18000, 0x67FF0)) and
 * entered via an indirect call, which always routes through the dispatcher. */
#define PSX_SHELL_ENTRY_PHYS 0x00030000u

/* Kernel EvCB table anchors (docs/psx_bios_disasm.txt; kseg1 like the kernel
 * itself uses so reads/writes are plain RAM traffic). */
#define KADDR_EVCB_BASE 0xA0000120u
#define KADDR_EVCB_SIZE 0xA0000124u

/* EvCB field offsets / values (SCPH1001 kernel + PSX-SPX). */
#define EVCB_STRIDE   0x1Cu
#define EV_CLASS      0x00u
#define EV_STATUS     0x04u
#define EV_SPEC       0x08u
#define EV_MODE       0x0Cu
#define EV_FUNC       0x10u
#define EVST_FREE     0x0000u
#define EVST_DISABLED 0x1000u
#define EVST_ENABLED  0x2000u
#define EVST_READY    0x4000u
#define EVMD_CALLBACK 0x1000u
#define EVMD_MARK     0x2000u

/* DeliverEvent's callback return site in kernel RAM (the instruction after
 * the kernel loop's `jalr v0` at 0x1718) — the exact $ra an LLE delivery
 * gives the callback, reused here so the dispatch return contract matches. */
#define KADDR_DELIVER_RET 0x80001720u

/* ── always-on ring ──────────────────────────────────────────────────────── */

static PsxHleCallEntry s_ring[PSX_HLE_RING_CAP];
static uint64_t        s_ring_seq = 0;

static void hle_record(uint32_t vector, uint32_t fn, const CPUState* cpu,
                       uint32_t v0, uint8_t route)
{
    /* Thread attribution: current TCB via the kernel's process block chain
     * (0x108 -> PCB, PCB[0] -> running TCB; SCPH1001 layout, stable across
     * kernel versions). Distinguishes which guest thread issued each call —
     * load-bearing for event-consumption races (MMX6 card check). */
    uint32_t pcb = cpu->read_word(0xA0000108u);
    uint32_t pcb_phys = pcb & 0x1FFFFFFFu;
    uint32_t tcb = (pcb != 0u && pcb_phys < 0x200000u)
                       ? cpu->read_word(0xA0000000u | pcb_phys) : 0u;
    extern int psx_get_in_exception(void);
    uint8_t in_exc = (uint8_t)(psx_get_in_exception() ? 1u : 0u);

    /* Collapse tight polling cycles: guest code that spins on a handful of
     * calls (e.g. MMX6's 4-way TestEvent card poll at ~175K calls/s) would
     * otherwise flush the whole ring in ~0.1s. If an identical call
     * (vector/fn/args/ra/result/route/thread) exists among the last 8
     * entries, count it there. The first and last occurrence cycles plus the
     * exact repeat count survive; any change in args, result, or issuing
     * thread appends a fresh entry, so every TRANSITION is ring-ordered. */
    uint64_t back = s_ring_seq < 8 ? s_ring_seq : 8;
    for (uint64_t k = 1; k <= back; k++) {
        PsxHleCallEntry* p = &s_ring[(s_ring_seq - k) & (PSX_HLE_RING_CAP - 1)];
        if (p->vector == vector && p->fn == fn && p->route == route &&
            p->a0 == cpu->gpr[4] && p->a1 == cpu->gpr[5] &&
            p->a2 == cpu->gpr[6] && p->a3 == cpu->gpr[7] &&
            p->ra == cpu->gpr[31] && p->v0 == v0 &&
            p->tcb == tcb && p->in_exc == in_exc) {
            p->repeat++;
            p->cycle_last = psx_get_cycle_count();
            return;
        }
    }
    PsxHleCallEntry* e = &s_ring[s_ring_seq & (PSX_HLE_RING_CAP - 1)];
    e->seq    = s_ring_seq;
    e->cycle  = psx_get_cycle_count();
    e->cycle_last = e->cycle;
    e->vector = vector;
    e->fn     = fn;
    e->a0     = cpu->gpr[4];
    e->a1     = cpu->gpr[5];
    e->a2     = cpu->gpr[6];
    e->a3     = cpu->gpr[7];
    e->ra     = cpu->gpr[31];
    e->v0     = v0;
    e->repeat = 1;
    e->tcb    = tcb;
    e->route  = route;
    e->in_exc = in_exc;
    s_ring_seq++;
}

uint64_t psx_hle_ring_seq(void) { return s_ring_seq; }

const PsxHleCallEntry* psx_hle_ring_entry(uint64_t seq_index)
{
    if (seq_index >= s_ring_seq) return NULL;
    if (s_ring_seq - seq_index > PSX_HLE_RING_CAP) return NULL; /* evicted */
    return &s_ring[seq_index & (PSX_HLE_RING_CAP - 1)];
}

/* ── B0 event services (SCPH1001 kernel semantics, on real guest EvCBs) ──── */

/* get_free_EvCB_slot (kernel 0x1D00): first status==0 slot index, else -1. */
static uint32_t ev_free_slot(const CPUState* cpu, uint32_t base, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (cpu->read_word(base + i * EVCB_STRIDE + EV_STATUS) == EVST_FREE)
            return i;
    }
    return 0xFFFFFFFFu;
}

/* OpenEvent (kernel 0x1D8C): claim a free EvCB, fill it, status=disabled,
 * return 0xF1000000|slot; no free slot => 0xFFFFFFFF. */
static void hle_open_event(CPUState* cpu, uint32_t base, uint32_t n)
{
    uint32_t slot = ev_free_slot(cpu, base, n);
    if (slot == 0xFFFFFFFFu) {
        cpu->gpr[2] = 0xFFFFFFFFu;
    } else {
        uint32_t ev = base + slot * EVCB_STRIDE;
        cpu->write_word(ev + EV_CLASS,  cpu->gpr[4]);
        cpu->write_word(ev + EV_SPEC,   cpu->gpr[5]);
        cpu->write_word(ev + EV_MODE,   cpu->gpr[6]);
        cpu->write_word(ev + EV_FUNC,   cpu->gpr[7]);
        cpu->write_word(ev + EV_STATUS, EVST_DISABLED);
        cpu->gpr[2] = 0xF1000000u | slot;
    }
    psx_advance_cycles(40u + 6u * (slot == 0xFFFFFFFFu ? n : slot + 1u));
}

/* CloseEvent (kernel 0x1E1C): status=free, return 1. */
static void hle_close_event(CPUState* cpu, uint32_t base)
{
    uint32_t ev = base + (cpu->gpr[4] & 0xFFFFu) * EVCB_STRIDE;
    cpu->write_word(ev + EV_STATUS, EVST_FREE);
    cpu->gpr[2] = 1u;
    psx_advance_cycles(36u);
}

/* TestEvent (kernel 0x1EC8): ready => re-arm to enabled and return 1, else 0. */
static void hle_test_event(CPUState* cpu, uint32_t base)
{
    uint32_t ev = base + (cpu->gpr[4] & 0xFFFFu) * EVCB_STRIDE;
    if (cpu->read_word(ev + EV_STATUS) == EVST_READY) {
        cpu->write_word(ev + EV_STATUS, EVST_ENABLED);
        cpu->gpr[2] = 1u;
    } else {
        cpu->gpr[2] = 0u;
    }
    psx_advance_cycles(40u);
}

/* EnableEvent (kernel 0x1F10) / DisableEvent (kernel 0x1F4C): a non-free
 * event's status is set to enabled/disabled; return 1 either way. NOTE the
 * kernel really does discard a pending READY on EnableEvent — faithful. */
static void hle_set_event_status(CPUState* cpu, uint32_t base, uint32_t status)
{
    uint32_t ev = base + (cpu->gpr[4] & 0xFFFFu) * EVCB_STRIDE;
    if (cpu->read_word(ev + EV_STATUS) != EVST_FREE)
        cpu->write_word(ev + EV_STATUS, status);
    cpu->gpr[2] = 1u;
    psx_advance_cycles(40u);
}

/* DeliverEvent (kernel 0x1B44): for every ENABLED EvCB matching class+spec,
 * mode MARK => status=READY; mode CALLBACK => call the handler exactly as the
 * kernel's `jalr v0` does (same $ra, args untouched). */
static void hle_deliver_event(CPUState* cpu, uint32_t base, uint32_t n)
{
    uint32_t cls  = cpu->gpr[4];
    uint32_t spec = cpu->gpr[5];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ev = base + i * EVCB_STRIDE;
        if (cpu->read_word(ev + EV_STATUS) != EVST_ENABLED) continue;
        if (cpu->read_word(ev + EV_CLASS) != cls)  continue;
        if (cpu->read_word(ev + EV_SPEC)  != spec) continue;
        uint32_t mode = cpu->read_word(ev + EV_MODE);
        if (mode == EVMD_MARK) {
            cpu->write_word(ev + EV_STATUS, EVST_READY);
        } else if (mode == EVMD_CALLBACK) {
            uint32_t func = cpu->read_word(ev + EV_FUNC);
            if (func != 0) {
                /* Nested guest call, same shape as any compiled call site.
                 * $ra is the kernel's real post-jalr address so the callback
                 * returns through the standard dispatch contract; the caller's
                 * $ra is restored before the hook returns. */
                uint32_t saved_ra = cpu->gpr[31];
                cpu->gpr[31] = KADDR_DELIVER_RET;
                extern int g_exec_phase;   /* wall-time sampler (dirty_ram_interp.c) */
                int prev_phase = g_exec_phase;
                g_exec_phase = 3;   /* compiled route; dirty/native callees re-tag inside */
                psx_dispatch_call(cpu, func, KADDR_DELIVER_RET);
                g_exec_phase = prev_phase;
                cpu->gpr[31] = saved_ra;
            }
        }
        /* mode==anything else: the kernel loop skips it — so do we. */
    }
    /* Return value: the kernel returns with v0 = last comparison scratch; no
     * caller may depend on it (libapi types DeliverEvent void). Set 0 for a
     * stable, observable value. */
    cpu->gpr[2] = 0u;
    psx_advance_cycles(36u + 12u * n);
}

/* B0 vector service. Returns 1 iff serviced in HLE. */
static int hle_service_b0(CPUState* cpu, uint32_t fn)
{
    /* Kernel executive not initialized yet (InitEvents hasn't run) => the
     * EvCB table doesn't exist; everything stays LLE. */
    uint32_t base = cpu->read_word(KADDR_EVCB_BASE);
    if (base == 0) return 0;
    uint32_t n = cpu->read_word(KADDR_EVCB_SIZE) / EVCB_STRIDE;
    if (n == 0 || n > 0x1000u) return 0; /* implausible table => LLE */

    switch (fn) {
    case 0x07u: hle_deliver_event(cpu, base, n);                  return 1;
    case 0x08u: hle_open_event(cpu, base, n);                     return 1;
    case 0x09u: hle_close_event(cpu, base);                       return 1;
    case 0x0Bu: hle_test_event(cpu, base);                        return 1;
    case 0x0Cu: hle_set_event_status(cpu, base, EVST_ENABLED);    return 1;
    case 0x0Du: hle_set_event_status(cpu, base, EVST_DISABLED);   return 1;
    default:    return 0; /* WaitEvent, threads, pads, card, ... => LLE */
    }
}

/* ── boot shell-skip ─────────────────────────────────────────────────────── */

static int hle_boot_shell_skip(CPUState* cpu)
{
    if (!s_boot_skip_on || s_shell_skipped || fntrace_is_game_started())
        return 0;
    s_shell_skipped = 1;
    /* Return straight to LoadRunShell's call site: BIOS Main() proceeds to
     * step 8 (SYSTEM.CNF + game EXE load) exactly as if the shell had exited
     * immediately with no disc-menu interaction. Kernel state at this point
     * was built entirely by the real recompiled kernel init. */
    cpu->gpr[2] = 0u;
    hle_record(PSX_SHELL_ENTRY_PHYS, 0u, cpu, 0u, PSX_HLE_ROUTE_BOOT);
    return 1;
}

int psx_bios_hle_boot_turbo_active(void)
{
    return s_boot_skip_on && !fntrace_is_game_started();
}

/* ── the hook ────────────────────────────────────────────────────────────── */

static int bios_hle_dispatch(struct CPUState* cpu, uint32_t phys)
{
    if (phys == 0xB0u) {
        uint32_t fn = cpu->gpr[9]; /* $t1 — the PSY-Q thunk's function number */
        int handled = s_call_hle_on ? hle_service_b0(cpu, fn) : 0;
        hle_record(0xB0u, fn, cpu, handled ? cpu->gpr[2] : 0u,
                   handled ? PSX_HLE_ROUTE_HLE : PSX_HLE_ROUTE_LLE);
        return handled;
    }
    if (phys == 0xA0u || phys == 0xC0u) {
        /* No A0/C0 services implemented in v1 — observe + fall through. */
        hle_record(phys, cpu->gpr[9], cpu, 0u, PSX_HLE_ROUTE_LLE);
        return 0;
    }
    if (phys == PSX_SHELL_ENTRY_PHYS)
        return hle_boot_shell_skip(cpu);
    return 0;
}

/* ── selection ───────────────────────────────────────────────────────────── */

void psx_bios_hle_configure(int call_hle, int boot_skip)
{
    s_call_hle_on  = call_hle ? 1 : 0;
    s_boot_skip_on = boot_skip ? 1 : 0;
    /* Rematch / session_reboot re-enters bring-up without process exit; the
     * shell-skip latch must arm again or peers run the interactive shell and
     * appear hung after netplay lockstep. */
    s_shell_skipped = 0;
    g_psx_bios_hle_hook =
        (s_call_hle_on || s_boot_skip_on) ? &bios_hle_dispatch : NULL;
}

int psx_bios_hle_enabled(void)           { return s_call_hle_on; }
int psx_bios_hle_boot_skip_enabled(void) { return s_boot_skip_on; }

const char* psx_bios_hle_backend_name(void)
{
    return s_call_hle_on ? "HLE (LLE fallback)" : "LLE (recompiled BIOS)";
}
