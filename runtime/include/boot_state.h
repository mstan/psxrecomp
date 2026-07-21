#ifndef PSX_BOOT_STATE_H
#define PSX_BOOT_STATE_H

#include <stdint.h>
#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Boot snapshot (a.k.a. fast_boot) — a COMPLETE post-BIOS save-state.
 *
 * Model: first launch (and the first launch after ANY app/recompiler update)
 * runs the real recompiled BIOS normally, logos and all. At the moment the BIOS
 * dispatches into the game's PS-EXE entry_pc, we capture a complete hardware
 * snapshot. Every subsequent launch (same build) restores that snapshot and
 * presents the game's first frame — instant boot, no BIOS, no logos.
 *
 * This is NOT HLE: it persists the REAL hardware state produced by a real BIOS
 * run, then replays it. Nothing about BIOS behaviour is synthesized.
 *
 * Rebuild-proof: the file carries an integrity key (below) that includes the
 * codegen hash + ABI tag + codegen version. A user update changes those, so a
 * stale snapshot can NEVER silently load into a new build — it is rejected and
 * the next boot is a normal boot that recaptures. Graceful, automatic.
 *
 * Completeness is mandatory (v4 no-stub rule): a partial capture that leaves a
 * subsystem at reset while CPU/RAM assume it was configured is a latent stub.
 * Every mutable hardware subsystem gets a section here, and the capture is
 * proven complete by diffing a restored session's frames against a normal-boot
 * session's frames (see the "bootsnap" debug command). Host-side / recompiler-
 * derived state (dirty-RAM bitmap, overlay tables, debug rings) is NOT
 * serialized — it is re-derived from restored guest RAM on load.
 */

#define BOOT_STATE_MAGIC   0x50535842u  /* "PSXB" */
/* v1 = incomplete RAM-only; v2 = full machine but host-struct memcpy (padding);
 * v3 = little-endian field wire (portable Win/Linux/macOS ARM). */
#define BOOT_STATE_VERSION 3u

/*
 * On-disk header (v3): nine little-endian uint32 fields at offset 0 (36 bytes),
 * followed by the section stream. ALL key fields must match the running build
 * or the snapshot is rejected. Do not fwrite() this struct — use pst_wire.
 */
typedef struct {
    uint32_t magic;          /* BOOT_STATE_MAGIC                                  */
    uint32_t version;        /* BOOT_STATE_VERSION                                */
    /* ---- integrity key (every field must match to accept) ---- */
    uint32_t bios_checksum;  /* sum of all uint32 words in the BIOS ROM           */
    uint32_t entry_pc;       /* game PS-EXE entry PC                              */
    uint32_t codegen_hash;   /* PSX_OVERLAY_CODEGEN_HASH (auto-gen by cmake)      */
    int32_t  abi_tag;        /* PSX_OVERLAY_ABI_TAG (abi version | flavor<<16)    */
    uint32_t codegen_ver;    /* PSX_OVERLAY_CODEGEN_VER                           */
    /* ---- layout ---- */
    uint32_t section_count;  /* number of sections that follow                    */
    uint32_t reserved;       /* 0                                                 */
} BootStateHeader;

#define BOOT_STATE_HEADER_WIRE_BYTES 36u

/*
 * Section stream (v3): section_count records, each laid out as
 *     uint32_t tag;        LE (one of BS_SEC_*)
 *     uint32_t pad;        LE 0
 *     uint64_t len;        LE payload byte count
 *     uint8_t  payload[len];   (module payloads are LE field wires too)
 * An unknown tag, a length mismatch, or a missing required section on load is a
 * hard reject (incomplete restore is never allowed) -> normal boot + recapture.
 */
enum {
    BS_SEC_CPU    = 0x01,  /* CPUState: gpr/pc/hi/lo/cop0/gte_data/gte_ctrl       */
    BS_SEC_RAM    = 0x02,  /* 2 MB main RAM                                       */
    BS_SEC_SPAD   = 0x03,  /* 1 KB scratchpad                                     */
    BS_SEC_IRQ    = 0x04,  /* i_stat / i_mask                                     */
    BS_SEC_TIMER  = 0x05,  /* 3 root counters (counter/mode/target/irq/frac)      */
    BS_SEC_CLOCK  = 0x06,  /* psx_cycle_count                                     */
    BS_SEC_GPU    = 0x07,  /* GPU regs: display/draw-area/offset/mask/texpage/xfer*/
    BS_SEC_VRAM   = 0x08,  /* 1 MB VRAM (1024x512x16)                             */
    BS_SEC_SPU    = 0x09,  /* SPU regs + 24 voice decode/ADSR state + latches     */
    BS_SEC_SPURAM = 0x0A,  /* 512 KB SPU RAM                                      */
    BS_SEC_CDROM  = 0x0B,  /* CD-ROM controller FSM (regs/FIFOs/seek/read/pending)*/
    BS_SEC_DMA    = 0x0C,  /* DMA channels[7] + dpcr/dicr + async-transfer state  */
    BS_SEC_SIO    = 0x0D,  /* SIO regs + pad-config FSM + memcard FSM             */
    BS_SEC_DIRTY  = 0x0E,  /* dirty-RAM page bitmap (guest-written code pages)    */
};

/* Save a COMPLETE snapshot at game handoff. Returns 1 on success. */
int  boot_state_save(const CPUState* cpu, uint32_t bios_checksum,
                     uint32_t entry_pc, const char* path);

/* Load + validate (integrity key) + restore the full machine. On any mismatch
 * or incompleteness returns 0 (caller then boots normally and recaptures). */
int  boot_state_load(const char* path, uint32_t bios_checksum,
                     uint32_t entry_pc, CPUState* cpu);

/* Register a deferred capture: when boot_state_trigger_capture() fires (from
 * fntrace at game-start), serialize to path. One-shot. */
void boot_state_set_capture(const char* path, uint32_t bios_checksum,
                             uint32_t entry_pc);

/* Called from fntrace when the game entry PC first dispatches. */
void boot_state_trigger_capture(const CPUState* cpu);

#ifdef __cplusplus
}
#endif

#endif /* PSX_BOOT_STATE_H */
