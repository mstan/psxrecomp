/* psx_bios_backend.c — routing between the runtime and the active BIOS.
 *
 * A build links every recompiled BIOS it ships (bundled OpenBIOS plus a retail
 * image). Each exports one descriptor; this file picks one, publishes it, and
 * owns the symbols that used to be defined by the generated dispatch back when
 * exactly one BIOS was linked.
 *
 * Nothing else had to change: psx_dispatch()/psx_dispatch_call() keep their
 * names here as forwarders, so the game's generated C and all existing runtime
 * call sites are untouched, and psx_bios_image keeps working as a global
 * because it is assigned on selection.
 *
 * Selection policy itself lives in main.cpp (it needs the player's choice and
 * the game config); this file only provides the mechanism.
 */

#include <string.h>

#include "bios_hle.h"
#include "cpu_state.h"
#include "psx_bios_backend.h"

/* ── State published to the rest of the runtime ─────────────────────────── */

const PsxBiosBackend *psx_bios_active = 0;

/* Assigned from the active backend. Zeroed until then, which reads as
 * "structurally unavailable" for every field — the documented meaning of 0 in
 * psx_bios_image.h — rather than as stale data from some other image. */
PsxBiosImageInfo psx_bios_image;

const PsxKernelBody *psx_bios_kernel_bodies     = 0;
uint32_t             psx_bios_kernel_body_count = 0;

/* Dispatch nesting depth. Shared dispatch state, not per-image: each generated
 * dispatch used to define its own copy, which is precisely why two of them
 * could not be linked together. */
int g_psx_dispatch_depth = 0;

/* ── Forwarders ─────────────────────────────────────────────────────────────
 *
 * The generated BIOS C defines <STEM>_psx_dispatch; these keep the unprefixed
 * names that ~66 runtime call sites and the game's generated C already use. */

void psx_dispatch(CPUState *cpu, uint32_t addr)
{
    if (!psx_bios_active || !psx_bios_active->dispatch) return;
    psx_bios_active->dispatch(cpu, addr);
}

void psx_dispatch_call(CPUState *cpu, uint32_t addr, uint32_t return_addr)
{
    if (!psx_bios_active || !psx_bios_active->dispatch_call) return;
    psx_bios_active->dispatch_call(cpu, addr, return_addr);
}

/* ── Selection ──────────────────────────────────────────────────────────── */

const PsxBiosBackend *psx_bios_find(const char *image_id)
{
    if (!image_id) return 0;
    for (uint32_t i = 0; i < psx_bios_registry_count; i++) {
        const PsxBiosBackend *b = psx_bios_registry[i];
        if (b && b->image && b->image->image_id &&
            strcmp(b->image->image_id, image_id) == 0)
            return b;
    }
    return 0;
}

const PsxBiosBackend *psx_bios_bundled(void)
{
    for (uint32_t i = 0; i < psx_bios_registry_count; i++) {
        const PsxBiosBackend *b = psx_bios_registry[i];
        if (b && b->image && b->image->image_bundled) return b;
    }
    return 0;
}

int psx_bios_activate(const PsxBiosBackend *backend)
{
    if (!backend || !backend->image) return 0;
    psx_bios_active             = backend;
    psx_bios_image              = *backend->image;
    psx_bios_kernel_bodies      = backend->kernel_bodies;
    psx_bios_kernel_body_count  = backend->kernel_body_count;
    /* Soft-return rematch can switch OPENBIOS ↔ SCPH without process exit.
     * Drop the prior image's call-HLE / boot-skip hook immediately so a
     * sticky SCPH DeliverEvent path cannot run against OpenBIOS ROM bytes
     * before session_reboot re-runs psx_bios_hle_plan + configure. */
    psx_bios_hle_configure(0, 0);
    return 1;
}

/* Is there any image the player could supply, i.e. a linked backend that is
 * NOT the bundled one? Drives whether the launcher shows its BIOS row: with
 * both a bundled and a retail image linked, a player must be able to opt into
 * the retail one (and clear back). */
int psx_bios_has_selectable(void)
{
    for (uint32_t i = 0; i < psx_bios_registry_count; i++) {
        const PsxBiosBackend *b = psx_bios_registry[i];
        if (b && b->image && !b->image->image_bundled) return 1;
    }
    return 0;
}
