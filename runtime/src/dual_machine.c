/*
 * dual_machine.c -- Stage B dual-console driver. See dual_machine.h.
 *
 * Fiber-based switch core. Each machine owns a complete native stack tree
 * (root fiber + the TCB fibers traps.c creates for BIOS threads), so a
 * suspended machine's CPS frames survive intact and resuming is simply
 * psx_fiber_switch back into its poll frame -- no psx_scheduler_resume_at,
 * no null-pc $ra bridging (the failure mode of the snapshot-jump variant:
 * every call-return above a resume point needed a guessed bridge, which
 * converged on a stuck dispatch loop).
 *
 * What swaps at a switch:
 *   - guest state: the full boot_state blob (RAM/VRAM/devices/CPU regs into
 *     the SHARED CPUState);
 *   - scheduler/exception host statics: the PsxSchedMachineCtx bundle
 *     (traps.c) -- jmp_bufs stay valid because each targets its own
 *     machine's preserved fiber stacks; the TCB->fiber table swaps so two
 *     machines' identical guest TCB addresses cannot collide;
 *   - the SIO1 device instance (sio1_dual_install).
 * What deliberately does NOT swap: the crossover wire (host-owned channel
 * between the machines; BS_SEC_SIO1 apply is suppressed).
 *
 * Switches only happen where psx_interrupts_switch_safe() -- outside
 * exception dispatch / device service -- so every host static NOT in the
 * bundle is zero on both machines at the boundary.
 */
#include "dual_machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "boot_state.h"
#include "cpu_state.h"
#include "interrupts.h"
#include "psx_cycles.h"
#include "psx_fiber.h"
#include "psx_link.h"
#include "psx_rewind.h"
#include "psx_scheduler.h"
#include "fntrace.h"
#include "gpu_gl_renderer.h"
#include "psx_ram.h"
#include "spu.h"
#include "savestate.h"
#include "sio1.h"

int g_psx_dual_active = 0;

/* Coarse slice while the link is idle (guest cycles). Half an NTSC frame:
 * keeps handshake-line latency under ~8 ms while paying only ~4 swaps per
 * frame-pair. PSX_DUAL_SLICE overrides. */
#define DUAL_SLICE_COARSE_DEFAULT 282240u
/* Floor for the fine (link-armed) slice so a not-yet-programmed BAUD of 0
 * (char_cycles ~7) cannot thrash the switcher. */
#define DUAL_SLICE_FINE_FLOOR 1024u
/* Machine 1's root fiber runs the whole scheduler + dispatch tree. */
#define DUAL_ROOT_FIBER_STACK (16u * 1024u * 1024u)

/* traps.c machine-context bundle (opaque). */
extern size_t psx_sched_machine_ctx_size(void);
extern void   psx_sched_machine_swap_out(void *out);
extern void   psx_sched_machine_swap_in(const void *in);
extern void   psx_sched_machine_ctx_init(void *ctx);
extern psx_fiber_t psx_sched_root_fiber(void);
/* interrupts.c: quiescence test for the host statics not in the bundle. */
extern int psx_interrupts_switch_safe(void);
/* main.cpp: re-push current host pad state (or neutral, when the route
 * excludes this machine) into sio.c after a switch load. */
extern void psx_dual_repush_host_pads(int allow);

static int       s_requested;
static int       s_active;
static int       s_live;                /* machine currently executing */
static int       s_local;               /* machine that presents A/V */
static uint8_t  *s_blob[2];             /* parked machine's guest state */
static size_t    s_blob_len[2];
static void     *s_ctx[2];              /* parked machine's sched context */
static psx_fiber_t s_fiber[2];          /* fiber each machine suspends on */
static uint64_t  s_cycles[2];
static Sio1Device      *s_dev[2];
static PsxLinkEndpoint *s_ep[2];
static struct CPUState *s_cpu;          /* the shared CPUState object */
static uint64_t  s_swaps;
static int       s_fastswap;   /* RAM handed over by bank pointer, not blob */
/* Machine 1's CPU registers. Machine 0 keeps main.cpp's stack CPUState (every
 * frame of its fiber tree was created holding that pointer); machine 1's tree
 * grows entirely from dual_machine1_entry, so rooting it at its OWN struct
 * makes its whole call tree consistent -- and then the CPU section can leave
 * the switch blob: each machine's registers simply persist in its own struct
 * while the other runs. Under threads (Stage D) this is also the struct each
 * thread owns outright. Only meaningful while s_fastswap. */
static CPUState  s_cpu_bank1;


/* PSX_DUAL_CPU_BANK=0 roots machine 1 at the SHARED struct (pre-banking
 * behaviour, CPU section back in the blob) -- bisect gate. */
static int cpu_bank_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("PSX_DUAL_CPU_BANK");
        v = !(e && e[0] == '0');
    }
    return v;
}

static CPUState *dual_cpu(int m) {
    return (m == 1 && s_fastswap && cpu_bank_enabled()) ? &s_cpu_bank1 : s_cpu;
}
static uint32_t  s_last_pc[2];  /* PC each machine last yielded at */
static uint32_t  s_slice_coarse = DUAL_SLICE_COARSE_DEFAULT;
static uint32_t  s_bios_checksum;
static uint32_t  s_entry_pc;
static uint64_t  s_defer_notes;
static int       s_machine1_started;
static int       s_input_route;         /* 0 both, 1 A only, 2 B only */

void psx_dual_set_input_route(int route) {
    s_input_route = (route >= 0 && route <= 2) ? route : 0;
}
int psx_dual_get_input_route(void) { return s_input_route; }
int psx_dual_input_allowed(void) {
    if (!s_active || s_input_route == 0) return 1;
    return (s_input_route == 1) ? (s_live == 0) : (s_live == 1);
}

void psx_dual_machine_request(int local_machine) {
    s_requested = 1;
    s_local = local_machine ? 1 : 0;
    g_psx_dual_active = 1;
}

int psx_dual_present_gate(void) {
    return !s_active || s_live == s_local;
}

/* Move A/V ownership to a console at runtime (F6 focus). This is the ONE
 * display-ownership concept -- `s_local` is what psx_dual_present_gate() has
 * always tested, and sdl_vblank_present() already returns early for the
 * non-local machine, so nothing else needs its own mute. */
void psx_dual_set_local_machine(int machine) {
    s_local = machine ? 1 : 0;
}
int psx_dual_get_local_machine(void) { return s_local; }

/* Veto a config-file request before activation (PSX_DUAL_CONSOLE=0). */
void psx_dual_machine_cancel(void) {
    if (s_active) return;   /* too late -- already running two machines */
    s_requested = 0;
}

int psx_dual_machine_live(void) { return s_active ? s_live : -1; }
uint64_t psx_dual_machine_swaps(void) { return s_swaps; }
void psx_dual_machine_cycles(uint64_t out[2]) {
    out[0] = s_cycles[0];
    out[1] = s_cycles[1];
}

static void dual_disable(const char *why) {
    fprintf(stderr, "dual: %s -- dual mode OFF\n", why);
    s_requested = 0;
    s_active = 0;
    g_psx_dual_active = 0;
}

/* Is either console using the serial port? This gates the FINE slice, so a
 * false negative is fatal to the link: the machines keep trading 282240-cycle
 * quanta (8.3 ms of guest time) while the driver waits on a handshake whose
 * character time is 704 cycles (21 us), and every reply lands hundreds of
 * character-times late -> ESTABLISH LINK times out.
 *
 * TXEN|RXEN is NOT the right trigger, though it is tempting: WipEout opens the
 * port during boot (ctrl=0x0305) and leaves it open, so arming on that pins the
 * fine slice on forever -- measured 10.6k swaps/s and ~100 ms/s of blob cost,
 * dropping the menus to single-digit fps -- while buying nothing, because with
 * no byte in flight there is nothing to be timely about. `active` (a shift in
 * flight or chars queued) fires the instant either side actually transmits,
 * which is exactly when the slice needs to tighten. */
static int link_armed(void) {
    for (int m = 0; m < 2; m++) {
        uint16_t ctrl;
        if (!s_dev[m]) continue;
        if (sio1_device_active(s_dev[m])) return 1;
        ctrl = sio1_device_peek_ctrl(s_dev[m]);
        if (ctrl & SIO1_CTRL_DTR) return 1;
    }
    return 0;
}

/* PSX_DUAL_SLICE_FINE=<cycles> raises the armed-link floor. The switch cost is
 * a full guest-state blob save+load (~19 MiB of memcpy at 8 MB RAM), so host
 * cost scales as 1/slice: this is the knob that proves whether the switch RATE
 * is what a slow dual-console run is paying for. */
static uint32_t fine_floor(void) {
    static uint32_t v = 0;
    if (!v) {
        const char *e = getenv("PSX_DUAL_SLICE_FINE");
        unsigned long n = (e && e[0]) ? strtoul(e, NULL, 0) : 0ul;
        v = (n >= 64ul && n <= 33868800ul) ? (uint32_t)n : DUAL_SLICE_FINE_FLOOR;
    }
    return v;
}

static uint32_t current_slice(void) {
    if (!link_armed())
        return s_slice_coarse;
    {
        uint32_t a = sio1_device_char_cycles(s_dev[0]);
        uint32_t b = sio1_device_char_cycles(s_dev[1]);
        uint32_t fine = a < b ? a : b;
        uint32_t floor_cyc = fine_floor();
        if (fine < floor_cyc) fine = floor_cyc;
        if (fine > s_slice_coarse) fine = s_slice_coarse;
        return fine;
    }
}

/* Save the live machine's guest state EXACTLY as-is (cpu->pc untouched --
 * fibers resume inline, nothing re-dispatches from the blob's pc except
 * machine 1's cold start, which uses a resolved pc; see try_activate). */
static double s_save_ms, s_load_ms;   /* accumulated, reported by PSX_DUAL_DIAG */

static double dual_mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static int save_live_blob(struct CPUState *cpu) {
    uint8_t *data = NULL;
    size_t len = 0;
    const double t0 = dual_mono_ms();
    if (!boot_state_save_buffer_raw(cpu, s_bios_checksum, s_entry_pc,
                                    &data, &len))
        return 0;
    free(s_blob[s_live]);
    s_blob[s_live] = data;
    s_blob_len[s_live] = len;
    s_save_ms += dual_mono_ms() - t0;
    return 1;
}

/* Machine 1's root fiber: cold-start its scheduler once the first switch
 * has loaded blob[1] + ctx[1]. If its guest ever exits, park by handing
 * control back to machine 0 forever. */
static void dual_machine1_entry(void *arg) {
    (void)arg;
    psx_scheduler_run(dual_cpu(1));
    fprintf(stderr, "dual: machine 1 guest exited -- parking\n");
    dual_disable("machine 1 exit");
    for (;;)
        psx_fiber_switch(s_fiber[0]);
}

/* First poll with a safe resume PC: capture the shared t0 state as machine
 * 1's start blob and arm the crossover. Machine 0 keeps executing. */
/* PSX_DUAL_FORK=game|boot -- where machine 1 is forked from. Default "game":
 * wait for the game EXE to be running, so the second console inherits a
 * fully-booted primary instead of replaying the BIOS itself. Forking at BIOS
 * reset (the old behaviour, =boot) is wrong twice over: the second console
 * plays the whole SCEA boot animation on its own, and the mod plan is applied
 * by a PROCESS-GLOBAL one-shot (mod_runtime.cpp `s.main_applied`), so only
 * whichever machine reaches the EXE entry first gets the patched game -- the
 * other silently boots stock. Forking after game start copies the already
 * patched, already booted state into machine 1 and both run the same build. */
static int fork_at_game_start(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("PSX_DUAL_FORK");
        /* DEFAULT = boot (BIOS reset). A game-start fork looks attractive --
         * machine 1 inherits the booted, mod-patched primary and skips the
         * SCEA animation -- but it leaves the secondary NOT EXECUTING: its
         * scheduler context is initialised clean ("no TCB fibers yet"), which
         * is only correct at BIOS reset, so psx_scheduler_run() has nothing to
         * run and machine 1 never enters the guest (proved by a frozen RAM
         * digest and aconstant 0x00000000 yield PC). Cloning machine 0's context is
         * not possible: it holds jmp_bufs into machine 0's live fiber stacks.
         * At a BIOS-reset fork machine 1 boots the disc itself, which is why
         * mod_runtime's main_applied had to become per-machine -- otherwise
         * only the first console to reach the EXE entry got the mod plan. */
        v = (e && e[0] == 'g') ? 1 : 0;
    }
    return v;
}

static void try_activate(struct CPUState *cpu, uint32_t hint) {
    uint32_t pc = hint;
    if (fork_at_game_start() && !fntrace_is_game_started())
        return;
    if (!pc || (pc & 3u) || !psx_is_dispatchable(pc))
        pc = 0;
    if (!pc && cpu->pc && !(cpu->pc & 3u) && psx_is_dispatchable(cpu->pc))
        pc = cpu->pc;
    if (!pc)
        return;
    savestate_get_integrity(&s_bios_checksum, &s_entry_pc);
    {
        /* PSX_DUAL_INPUT_ROUTE=both|a|b — starting pad routing. A link handshake
         * is asymmetric, and two machines forked from one state with identical
         * input stay identical forever, so bring-up needs one console driven at
         * a time. F6 cycles this live (see dual_focus_apply in main.cpp). */
        const char *e = getenv("PSX_DUAL_INPUT_ROUTE");
        if (e && e[0]) {
            int r = (e[0] == 'a' || e[0] == 'A' || e[0] == '1') ? 1
                  : (e[0] == 'b' || e[0] == 'B' || e[0] == '2') ? 2 : 0;
            psx_dual_set_input_route(r);
            fprintf(stdout, "dual: input route = %s (PSX_DUAL_INPUT_ROUTE)\n",
                    r == 0 ? "both" : (r == 1 ? "A (primary)" : "B (secondary)"));
        }
    }
    {
        const char *e = getenv("PSX_DUAL_SLICE");
        if (e && e[0]) {
            unsigned long v = strtoul(e, NULL, 0);
            if (v >= DUAL_SLICE_FINE_FLOOR && v <= 33868800ul)
                s_slice_coarse = (uint32_t)v;
        }
    }

    s_cpu = cpu;
    s_live = 0;
    /* Fast swap: machine 1 gets its own 8 MiB DRAM bank, seeded once here with
     * the fork state, and RAM leaves the switch blob for good. Without this the
     * handoff copies RAM+VRAM+SPU (9.5 MiB, measured 2.7 ms) EVERY switch.
     * PSX_DUAL_FASTSWAP=0 keeps the old copy-through-blob path for bisecting. */
    {
        const char *e = getenv("PSX_DUAL_FASTSWAP");
        s_fastswap = !(e && e[0] == '0');
        if (s_fastswap) {
            extern uint8_t *memory_get_ram_ptr(void);
            uint32_t excl = 0;
            if (memory_ram_bank_create(1)) {
                memcpy(memory_ram_bank_ptr(1), memory_get_ram_ptr(),
                       memory_get_ram_bytes());
                excl |= 1u << BS_SEC_RAM;
            }
            if (spu_ram_bank_create(1)) {
                memcpy(spu_ram_bank_ptr(1), spu_get_ram_ptr(),
                       spu_get_ram_bytes());
                excl |= 1u << BS_SEC_SPURAM;
            }
            /* Forking after boot means machine 1 inherits what the primary has
             * already drawn, so bank 1 is seeded from bank 0 (GPU blit, no
             * readback). At a BIOS-reset fork there is nothing drawn yet and
             * the cleared bank is already correct. */
            if (gl_renderer_vram_bank_create(1)) {
                if (!fork_at_game_start() ||
                    gl_renderer_vram_bank_seed(1, gl_renderer_vram_bank_live()))
                    excl |= 1u << BS_SEC_VRAM;
            }
            if (!excl) {
                fprintf(stderr, "dual: no banks available -- copying state per switch\n");
                s_fastswap = 0;
            } else {
                /* CPU regs travel by struct, not by blob: machine 1 starts
                 * from the fork state with the resolved dispatchable pc. */
                s_cpu_bank1 = *cpu;
                s_cpu_bank1.pc = pc;
                if (cpu_bank_enabled())
                    excl |= 1u << BS_SEC_CPU;
                boot_state_set_section_exclude(excl);
                fprintf(stdout,
                        "dual: fast swap on (banked:%s%s%s%s)\n",
                        (excl & (1u << BS_SEC_RAM))    ? " ram"    : "",
                        (excl & (1u << BS_SEC_SPURAM)) ? " spuram" : "",
                        (excl & (1u << BS_SEC_VRAM))   ? " vram"   : "",
                        (excl & (1u << BS_SEC_CPU))    ? " cpu"    : "");
            }
        }
    }
    {
        /* Machine 1 cold-starts by dispatching the blob's pc, so this one
         * blob (and only this one) carries a resolved dispatchable pc. */
        uint8_t *data = NULL;
        size_t len = 0;
        CPUState snap = *cpu;
        snap.pc = pc;
        if (!boot_state_save_buffer_raw(&snap, s_bios_checksum, s_entry_pc,
                                        &data, &len)) {
            dual_disable("activation save failed");
            return;
        }
        s_blob[1] = data;
        s_blob_len[1] = len;
    }
    s_ctx[0] = malloc(psx_sched_machine_ctx_size());
    s_ctx[1] = malloc(psx_sched_machine_ctx_size());
    if (!s_ctx[0] || !s_ctx[1]) {
        dual_disable("ctx alloc failed");
        return;
    }
    psx_sched_machine_ctx_init(s_ctx[1]);  /* clean: no TCB fibers yet */

    s_fiber[0] = psx_sched_root_fiber();   /* converts the thread if needed */
    s_fiber[1] = psx_fiber_create(DUAL_ROOT_FIBER_STACK,
                                  dual_machine1_entry, NULL);
    if (!s_fiber[0] || !s_fiber[1]) {
        dual_disable("fiber create failed");
        return;
    }

    psx_link_crossover_create(&s_ep[0], &s_ep[1]);
    if (!s_ep[0] || !s_ep[1]) {
        dual_disable("crossover alloc failed");
        return;
    }
    s_dev[0] = sio1_get_device();
    s_dev[1] = sio1_device_create();
    if (!s_dev[0] || !s_dev[1]) {
        dual_disable("sio1 device missing");
        return;
    }
    sio1_device_attach(s_dev[0], s_ep[0]);
    sio1_device_attach(s_dev[1], s_ep[1]);
    /* Wire latency >= the armed lookahead is the barrier's causality guard: a
     * byte sent at T becomes due at T+latency, and the peer's clock can lead
     * the sender by at most the lookahead -- so with latency >= lookahead no
     * receiver is ever already PAST a byte's due cycle when it lands. On the
     * cooperative scheduler this only shifts delivery by <=242 us of guest
     * time (well under the driver's tolerance, proven at slice=8192); under
     * threads (Stage D) it is what lets both machines run a full quantum in
     * parallel without observing each other mid-quantum.
     * PSX_DUAL_WIRE_LATENCY=<cycles> overrides for A/B. */
    {
        uint32_t lat = fine_floor();
        const char *e = getenv("PSX_DUAL_WIRE_LATENCY");
        if (e && e[0]) {
            unsigned long v = strtoul(e, NULL, 0);
            if (v <= 33868800ul) lat = (uint32_t)v;
        }
        psx_link_set_latency_cycles(s_ep[0], lat);
        psx_link_set_latency_cycles(s_ep[1], lat);
        fprintf(stdout, "dual: wire latency = %u cycles (%.0f us guest)\n",
                lat, lat / 33.8688);
    }
    sio1_device_reset(s_dev[1], psx_get_cycle_count());
    /* Seed machine 1's SIO1 REGISTER FILE from the primary's. Machine 0 keeps
     * the live device (with whatever the boot already programmed into it);
     * machine 1's is freshly created, so without this it powers on blank while
     * its guest -- forked from a state where the port WAS configured -- believes
     * it is already open and never re-programs it. Observed exactly that:
     * A[ctrl=0305] vs B[ctrl=0000] for a whole session, no handshake possible.
     * Harmless when forking at BIOS reset (nothing programmed yet); required
     * once the fork moved to game start (PSX_DUAL_FORK=game).
     *
     * Registers ONLY: the snapshot's later sections carry the attached
     * endpoint, and the two devices hold OPPOSITE ends of the crossover --
     * copying those would splice A's wire end over B's. Section ends are
     * [0]=regs, [1]=fsm+endpoint, [2]=total. */
    {
        uint32_t na = sio1_device_snap_bytes(s_dev[0]);
        uint32_t nb = sio1_device_snap_bytes(s_dev[1]);
        uint32_t ea[3], eb[3];
        sio1_device_snap_section_ends(s_dev[0], ea);
        sio1_device_snap_section_ends(s_dev[1], eb);
        if (na == nb && ea[0] == eb[0] && ea[0] <= nb) {
            uint8_t *ba = (uint8_t *)malloc(na);
            uint8_t *bb = (uint8_t *)malloc(nb);
            uint64_t now = psx_get_cycle_count();
            if (ba && bb) {
                sio1_device_snap_write(s_dev[0], ba, now);
                sio1_device_snap_write(s_dev[1], bb, now);
                memcpy(bb, ba, ea[0]);          /* regs only */
                if (!sio1_device_snap_read(s_dev[1], bb, nb, now))
                    fprintf(stderr, "dual: sio1 reg seed rejected\n");
            }
            free(ba);
            free(bb);
        } else {
            fprintf(stderr,
                    "dual: sio1 reg seed skipped (layout %u/%u regs %u/%u)\n",
                    na, nb, ea[0], eb[0]);
        }
        fprintf(stdout, "dual: sio1 seeded A ctrl=%04X -> B ctrl=%04X\n",
                sio1_device_peek_ctrl(s_dev[0]), sio1_device_peek_ctrl(s_dev[1]));
    }
    sio1_dual_suppress_snapshot_apply(1);

    /* Rewind's snapshot ring would interleave two different machines. */
    psx_rewind_shutdown();
    /* Per-switch load_timing stderr would be ~100+ lines/s. */
    boot_state_set_quiet_load(1);

    s_cycles[0] = s_cycles[1] = psx_get_cycle_count();
    s_swaps = 0;
    s_machine1_started = 0;
    s_active = 1;
    fprintf(stderr,
            "dual: ACTIVE (fiber switch) -- two consoles from pc=0x%08X "
            "cyc=%llu (local=%d, coarse slice=%u, blob=%zu bytes)\n",
            (unsigned)pc, (unsigned long long)psx_get_cycle_count(),
            s_local, s_slice_coarse, s_blob_len[1]);
}

static void switch_machines(struct CPUState *cpu) {
    int to = s_live ^ 1;
    /* With CPU banking the caller's pointer is only the LIVE machine's struct;
     * every save/load below must target the struct of the machine it concerns,
     * not whichever chain happened to poll. */
    cpu = dual_cpu(s_live);
    /* Where each machine was when it last yielded. A machine whose PC never
     * moves is not executing -- it is spinning or parked, which no amount of
     * link/SIO work can fix. */
    s_last_pc[s_live] = cpu->pc;
    if (!save_live_blob(cpu)) {
        fprintf(stderr, "dual: save failed at switch (machine %d) -- retry\n",
                s_live);
        return;
    }
    s_cycles[s_live] = psx_get_cycle_count();
    psx_sched_machine_swap_out(s_ctx[s_live]);
    if (s_fastswap) {
        const uint32_t excl = boot_state_section_exclude();
        int bank_ok = 1;
        if (excl & (1u << BS_SEC_RAM))    bank_ok &= memory_ram_bank_activate(to);
        if (excl & (1u << BS_SEC_SPURAM)) bank_ok &= spu_ram_bank_activate(to);
        if (excl & (1u << BS_SEC_VRAM))   bank_ok &= gl_renderer_vram_bank_activate(to);
        if (!bank_ok) {
            psx_sched_machine_swap_in(s_ctx[s_live]);
            fprintf(stderr, "dual: bank %d activate failed -- dual mode OFF\n", to);
            dual_disable("bank activate failure");
            return;
        }
    }
    {
        const double t_load = dual_mono_ms();
        int loaded = boot_state_load_buffer(s_blob[to], s_blob_len[to],
                                            s_bios_checksum, s_entry_pc,
                                            dual_cpu(to));
        s_load_ms += dual_mono_ms() - t_load;
        if (!loaded) {
        /* Roll the host statics back and keep running this machine. */
        psx_sched_machine_swap_in(s_ctx[s_live]);
        fprintf(stderr,
                "dual: FATAL load failure switching to machine %d -- "
                "dual mode OFF (staying on machine %d)\n", to, s_live);
        dual_disable("blob load failure");
        return;
        }
    }
    psx_sched_machine_swap_in(s_ctx[to]);
    sio1_dual_install(s_dev[to]);
    /* Interior-pointer registrations must follow the machine. main.cpp wires
     * the memory system's SR view and the IRQ layer's CAUSE view as raw
     * pointers INTO main's CPUState (&cpu.cop0[12]/[13]). With per-machine CPU
     * structs, leaving them aimed at machine 0 makes machine 1 evaluate
     * interrupt enables against a FROZEN SR and assert IP2 into the WRONG
     * CAUSE -- it never takes an interrupt, and its BIOS boot unwinds ~214k
     * cycles in, the moment the kernel starts depending on exceptions. */
    if (s_fastswap && cpu_bank_enabled()) {
        extern void memory_set_sr_ptr(const uint32_t *p);
        extern void psx_irq_set_cause_ptr(uint32_t *p);
        CPUState *nc = dual_cpu(to);
        memory_set_sr_ptr(&nc->cop0[12]);
        psx_irq_set_cause_ptr(&nc->cop0[13]);
    }
    psx_cycles_resync_after_restore(dual_cpu(to));
    interrupts_resync_after_restore();
    /* NO cdrom_accelerate_after_savestate (exact-state resume) and NO
     * psx_frontend_on_savestate_loaded (re-anchors pacing/FPS/audio --
     * correct once per user load, harmful at per-switch rate). */
    {
        int route = s_input_route;
        int allow = (route == 0) || (route == 1 && to == 0) ||
                    (route == 2 && to == 1);
        psx_dual_repush_host_pads(allow);
    }
    s_swaps++;
    /* PSX_DUAL_DIAG=1: switch rate + slice + blob size, once a second. The
     * three together say whether the run is switch-bound. */
    {
        static int diag = -1;
        static uint64_t last_ms, last_swaps;
        if (diag < 0) { const char *e = getenv("PSX_DUAL_DIAG");
                        diag = (e && e[0] && e[0] != '0') ? 1 : 0; }
        if (diag) {
            struct timespec ts;
            uint64_t now_ms;
            uint32_t ram_a = 0, ram_b = 0;
            uint32_t tx_a = 0, rx_a = 0, ov_a = 0, irq_a = 0;
            uint32_t tx_b = 0, rx_b = 0, ov_b = 0, irq_b = 0;
            unsigned ctrl_a = 0, ctrl_b = 0, stat_a = 0, stat_b = 0;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            now_ms = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
            if (s_dev[0]) {
                sio1_device_get_counters(s_dev[0], &tx_a, &rx_a, &ov_a, &irq_a);
                ctrl_a = sio1_device_peek_ctrl(s_dev[0]);
                stat_a = sio1_device_peek_stat(s_dev[0], psx_get_cycle_count());
            }
            if (s_dev[1]) {
                sio1_device_get_counters(s_dev[1], &tx_b, &rx_b, &ov_b, &irq_b);
                ctrl_b = sio1_device_peek_ctrl(s_dev[1]);
                stat_b = sio1_device_peek_stat(s_dev[1], psx_get_cycle_count());
            }
            (void)ov_a; (void)ov_b;
            /* Sampled RAM digest per machine. The two consoles are forked from
             * one state and (while input routes to BOTH) fed identical input,
             * so they should be deterministic twins -- if these two digests
             * ever differ, they have DIVERGED and any link handshake between
             * them is hopeless. Banks are host-addressable, so this costs one
             * strided pass and no machine switch. Sampled 1:256 words. */
            {
                const uint8_t *pa = memory_ram_bank_ptr(0);
                const uint8_t *pb = memory_ram_bank_ptr(1);
                uint32_t n = memory_get_ram_bytes();
                ram_a = ram_b = 2166136261u;
                if (pa && pb) {
                    uint32_t off;
                    for (off = 0; off + 4u <= n; off += 1024u) {
                        uint32_t wa, wb;
                        memcpy(&wa, pa + off, 4);
                        memcpy(&wb, pb + off, 4);
                        ram_a = (ram_a ^ wa) * 16777619u;
                        ram_b = (ram_b ^ wb) * 16777619u;
                    }
                }
            }
            if (!last_ms) last_ms = now_ms;
            if (now_ms - last_ms >= 1000u) {
                fprintf(stderr,
                        "[DUAL] swaps/s=%llu slice=%u armed=%d blob=%zu KiB "
                        "save=%.1f load=%.1f ms/s m0=%llu m1=%llu d=%+lld "
                        "pc A=%08X B=%08X ram A=%08X B=%08X %s "
                        "sio A[ctrl=%04X stat=%04X dsr=%d tx=%u rx=%u irq=%u] "
                        "B[ctrl=%04X stat=%04X dsr=%d tx=%u rx=%u irq=%u] total_swaps=%llu\n",
                        (unsigned long long)(s_swaps - last_swaps),
                        current_slice(), link_armed(),
                        (s_blob_len[0] + 1023u) / 1024u,
                        s_save_ms, s_load_ms,
                        /* Per-machine guest cycle counts: both must ADVANCE and
                         * stay within a slice of each other. A frozen m1 means
                         * the secondary never really cold-started; a diverging
                         * d= means the scheduler is starving one of them. */
                        (unsigned long long)s_cycles[0],
                        (unsigned long long)s_cycles[1],
                        (long long)((int64_t)s_cycles[0] - (int64_t)s_cycles[1]),
                        /* Per-machine SIO1 traffic. `armed` only tests DTR /
                         * device-active, and WipEout's libcomb driver enables
                         * TXEN|RXEN (ctrl=0x0305) but NEVER asserts DTR — so
                         * armed stays 0 even when the link is working. tx/rx
                         * are the real signal: rx climbing on BOTH sides means
                         * bytes are crossing the wire. */
                        s_last_pc[0], s_last_pc[1],
                        ram_a, ram_b, ram_a == ram_b ? "TWIN" : "DIVERGED",
                        ctrl_a, stat_a, (stat_a & SIO1_STAT_DSR) ? 1 : 0,
                        tx_a, rx_a, irq_a,
                        ctrl_b, stat_b, (stat_b & SIO1_STAT_DSR) ? 1 : 0,
                        tx_b, rx_b, irq_b,
                        (unsigned long long)s_swaps);
                last_ms = now_ms; last_swaps = s_swaps;
                s_save_ms = 0.0; s_load_ms = 0.0;
            }
        }
    }
    {
        int from = s_live;
        s_live = to;
        if (to == 1 && !s_machine1_started) {
            s_machine1_started = 1;
            psx_fiber_switch(s_fiber[1]);      /* cold start: entry runs */
        } else {
            psx_fiber_switch(s_fiber[to]);     /* resume inside its poll */
        }
        /* Control returned: THIS machine is live again, its guest state
         * reloaded by the peer's switch. Continue inline. */
        (void)from;
    }
}

/* ---- Bounded-skew barrier ------------------------------------------------
 *
 * THE synchronization contract between the two consoles, stated once so the
 * cooperative scheduler below and the threaded runner (Stage D) enforce the
 * same rule: a machine may not advance past
 *
 *     peer_clock + lookahead
 *
 * where lookahead = current_slice() (coarse while the wire is idle, the char
 * time while a byte is in flight). With that bound, a byte sent at cycle T is
 * observed at most `lookahead` cycles late on the peer's timeline -- never
 * lost, never reordered, because the crossover rings deliver by due-cycle and
 * the receiver's clock can lag the sender by at most the bound. Empirical
 * tolerance so far: the WipEout libcomb handshake completed and streamed
 * symmetric traffic at lookahead=8192 (242 us of guest time).
 *
 * On fibers "may not advance past" means "yield at the barrier" (the peer is
 * behind by construction, so switching IS the wait). Under threads the same
 * predicate becomes a blocking wait until the peer's clock catches up. */
int psx_dual_barrier_reached(uint64_t now) {
    return now >= s_cycles[s_live ^ 1] + current_slice();
}

void psx_dual_machine_poll(struct CPUState *cpu, uint32_t resume_pc) {
    uint64_t now;
    if (!s_requested)
        return;
    /* Only switch where the un-swapped host statics are quiescent. */
    if (!psx_interrupts_switch_safe())
        return;
    if (!s_active) {
        try_activate(cpu, resume_pc);
        return;
    }
    now = psx_get_cycle_count();
    s_cycles[s_live] = now;
    if (!psx_dual_barrier_reached(now))
        return;                          /* keep running this machine */
    /* Record where this machine suspends so the peer can switch back. */
    s_fiber[s_live] = psx_fiber_current();
    switch_machines(cpu);
    (void)s_defer_notes;
}
