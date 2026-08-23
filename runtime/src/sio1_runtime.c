/*
 * sio1_runtime.c -- SIO1 singleton wrapper for the live machine.
 *
 * Wires the pure Sio1Device instance (sio1.c) to the runtime: guest clock
 * (psx_get_cycle_count), IRQ delivery (psx_irq_raise(IRQ_SIO1)), backend
 * selection from [runtime.link] / env, and boot-state / digest glue.
 *
 * Kept separate from sio1.c so unit tests can link the device core with no
 * runtime stubs (the lesson from test_sio_card_protocol.c's stub wall).
 */
#include "sio1.h"
#include "psx_link_shm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "interrupts.h"
#include "psx_cycles.h"

volatile int g_sio1_active = 0;
/* PSX_SIO1_REGS=0 restores the legacy fold-into-SIO0 decode. Default on:
 * the register file exists on every PlayStation whether or not a cable is
 * plugged in (accuracy/axis4_sio1_serial.md). */
int g_sio1_regs_enabled = 1;

static Sio1Device *g_dev;
static PsxLinkEndpoint *g_owned_loopback;   /* destroyed on backend change */
static char g_backend[16] = "null";
static uint32_t g_latency_cycles = 0;

static void sio1_update_active(void) {
    g_sio1_active = sio1_device_active(g_dev);
}

static void sio1_irq_cb(void *user, uint32_t detail) {
    (void)user;
    psx_irq_raise(IRQ_SIO1, detail);
}

Sio1Device *sio1_get_device(void) { return g_dev; }

/* ===== dual-console hooks (dual_machine.c) ============================== */

static int g_snapshot_apply_suppressed;

void sio1_dual_install(Sio1Device *d) {
    if (d) g_dev = d;
    sio1_update_active();
}

void sio1_dual_suppress_snapshot_apply(int on) {
    g_snapshot_apply_suppressed = on ? 1 : 0;
}
const char *sio1_get_backend(void) { return g_backend; }

int sio1_set_backend(const char *name) {
    PsxLinkEndpoint *ep;
    if (!name) return 0;
    if (strcmp(name, "loopback") == 0) {
        ep = psx_link_loopback_create();
        if (!ep) return 0;
    } else if (strcmp(name, "shm") == 0) {
        /* Two-process link: each console is a full single-console runtime;
         * the cable is a shared-memory ring pair (psx_link_shm.h). Name and
         * role come from env so two instances on one machine can pair without
         * distinct game.tomls: PSX_LINK_SHM_NAME, PSX_LINK_SHM_ROLE=a|b. */
        const char *nm = getenv("PSX_LINK_SHM_NAME");
        const char *rl = getenv("PSX_LINK_SHM_ROLE");
        ep = psx_link_shm_open(nm, rl && rl[0] ? rl[0] : 0);
        if (!ep) return 0;
    } else if (strcmp(name, "null") == 0 || strcmp(name, "crossover") == 0) {
        /* "crossover" endpoints are attached by the dual-console driver via
         * sio1_device_attach; until that driver runs, behave as no-cable. */
        ep = psx_link_null();
    } else {
        return 0;
    }
    if (g_owned_loopback) {
        psx_link_loopback_destroy(g_owned_loopback);
        psx_link_shm_close(g_owned_loopback);
        g_owned_loopback = NULL;
    }
    if (ep->ops != psx_link_null()->ops)
        g_owned_loopback = ep;
    if (name != g_backend)
        snprintf(g_backend, sizeof g_backend, "%s", name);
    if (g_dev) {
        sio1_device_attach(g_dev, ep);
        psx_link_set_latency_cycles(ep, g_latency_cycles);
    }
    sio1_update_active();
    return 1;
}

void sio1_set_latency_cycles(uint32_t cycles) {
    g_latency_cycles = cycles;
    if (g_owned_loopback)
        psx_link_set_latency_cycles(g_owned_loopback, cycles);
}

void sio1_init(void) {
    if (!g_dev) {
        g_dev = sio1_device_create();
        if (!g_dev) {
            fprintf(stderr, "sio1: device alloc failed; SIO1 disabled\n");
            g_sio1_regs_enabled = 0;
            return;
        }
        sio1_device_set_irq(g_dev, sio1_irq_cb, NULL);
        sio1_device_attach(g_dev, psx_link_null());
        /* Config ([runtime.link]) may have run before init: re-apply the
         * recorded backend now that the device exists. Env overrides win. */
        sio1_set_backend(g_backend);
        {
            const char *e = getenv("PSX_SIO1_REGS");
            if (e && e[0] == '0') g_sio1_regs_enabled = 0;
        }
        {
            const char *e = getenv("PSX_SIO1_BACKEND");
            if (e && e[0]) sio1_set_backend(e);
        }
        {
            const char *e = getenv("PSX_SIO1_LATENCY");
            if (e && e[0]) sio1_set_latency_cycles((uint32_t)strtoul(e, NULL, 0));
        }
    }
    sio1_device_reset(g_dev, psx_get_cycle_count());
    sio1_update_active();
}

uint32_t sio1_read(uint32_t addr, uint32_t width) {
    uint32_t v;
    if (!g_dev) return 0;
    v = sio1_device_read(g_dev, addr, width, psx_get_cycle_count());
    sio1_update_active();
    return v;
}

void sio1_write(uint32_t addr, uint32_t width, uint32_t value) {
    if (!g_dev) return;
    sio1_device_write(g_dev, addr, width, value, psx_get_cycle_count());
    sio1_update_active();
}

void sio1_advance(uint32_t cycles) {
    if (!g_sio1_active || !g_dev) return;
    sio1_device_advance(g_dev, cycles, psx_get_cycle_count());
    sio1_update_active();
}

uint32_t sio1_cycles_to_irq(uint32_t i_mask) {
    if (!g_sio1_active || !g_dev) return 0xFFFFFFFFu;
    return sio1_device_cycles_to_irq(g_dev, i_mask);
}

/* ===== boot-state / digest glue (BS_SEC_SIO1) =========================== */

uint32_t sio1_snapshot_bytes(void) {
    return g_dev ? sio1_device_snap_bytes(g_dev) : 0;
}

void sio1_snapshot_write(uint8_t *p) {
    if (g_dev) sio1_device_snap_write(g_dev, p, psx_get_cycle_count());
}

int sio1_snapshot_read(const uint8_t *p, uint32_t len) {
    int ok;
    /* Dual-console: the device + crossover wire persist per machine in host
     * memory; applying the blob's (stale) copy would erase chars the peer
     * pushed since it was written. Accept and ignore the section. */
    if (g_snapshot_apply_suppressed) return 1;
    if (!g_dev) return len == 0;
    ok = sio1_device_snap_read(g_dev, p, len, psx_get_cycle_count());
    sio1_update_active();
    return ok;
}

void sio1_snapshot_section_ends(uint32_t out[3]) {
    if (g_dev) {
        sio1_device_snap_section_ends(g_dev, out);
    } else {
        out[0] = out[1] = out[2] = 0;
    }
}
