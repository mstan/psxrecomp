#ifndef PSX_BOOT_STATE_H
#define PSX_BOOT_STATE_H

#include <stddef.h>
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
 * v3 = little-endian field wire (portable Win/Linux/macOS ARM);
 * v4 = v3 + optional zlib on large sections (section pad bit0 = compressed);
 * v5 = v4 + CD-ROM Sub-Q replacement state. */
#define BOOT_STATE_VERSION 5u
/* v5 intentionally breaks older savestates after the CD-ROM wire grew. */
#define BOOT_STATE_VERSION_MIN_READ 5u
/* Section pad bit0: payload is u32 LE uncompressed_len + zlib deflate bytes. */
#define BOOT_STATE_SEC_ZLIB 1u

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
 * Section stream (v3/v4): section_count records, each laid out as
 *     uint32_t tag;        LE (one of BS_SEC_*)
 *     uint32_t pad;        LE flags (v3: 0; v4: BOOT_STATE_SEC_ZLIB optional)
 *     uint64_t len;        LE payload byte count
 *     uint8_t  payload[len];   (module payloads are LE field wires too)
 * When BOOT_STATE_SEC_ZLIB is set, payload = u32 LE raw_len + zlib(raw).
 * An unknown tag, a length mismatch, or a missing required section on load is a
 * hard reject (incomplete restore is never allowed) -> normal boot + recapture.
 */
enum {
    BS_SEC_CPU    = 0x01,  /* CPUState: gpr/pc/hi/lo/cop0/gte_data/gte_ctrl       */
    BS_SEC_RAM    = 0x02,  /* live main RAM (2 MB retail or unique 8 MB)           */
    BS_SEC_SPAD   = 0x03,  /* 1 KB scratchpad                                     */
    BS_SEC_IRQ    = 0x04,  /* i_stat / i_mask / cycles_since_vblank (12B; 8B ok)  */
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
    BS_SEC_MDEC   = 0x0F,  /* MDEC command/FIFOs/quant/scale (FMV decode resume)  */
    BS_SEC_ICACHE = 0x10,  /* R3000A I-cache tag/valid words (1024 u32) — fetch
                              cost model. Host-persistent otherwise: a warm load
                              without it replays with the pre-load timeline's
                              cache, so fetch-miss cycles differ per peer/retry
                              and IRQ delivery lands a few wait-loop iterations
                              apart (MotK abort@940: fin cyc Δ8, v0 5c83/5c86
                              from identical baselines). Optional on load for
                              old blobs (left untouched when absent).          */
    BS_SEC_SIO1   = 0x11,  /* SIO1 serial link: regs + RX FIFO + shifter +
                              endpoint wire queue (timeline deltas). Optional
                              on load for pre-SIO1 blobs: absent section
                              leaves the device at power-on (sio1_init),
                              same rule as BS_SEC_ICACHE.                   */
    /* MERGE NOTE (PR6): the widescreen branch allocated 0x11/0x12 for the two
     * sections below while master had already shipped 0x11 = BS_SEC_SIO1 (all
     * master v5 states and every dual-console switch blob carry it), so the
     * branch's tags are renumbered +1 here. States written by the PR BRANCH
     * builds are the only casualties: their 0x11 payload parses as SIO1 and
     * is refused -- dev-only states, accepted loss. */
    BS_SEC_TEXPACK = 0x12, /* HD texture pack upload tracker (rects+hashes)      */
    BS_SEC_MODSET  = 0x13, /* resolved mod-plan fingerprint (load guard):
                            * loading a state into a session whose enabled mod
                            * set differs poisons the machine (null-PC spin was
                            * observed live) -- refuse BEFORE any apply instead.
                            * Written FIRST so the reject precedes mutation.    */
};

/* Save a COMPLETE snapshot at game handoff. Returns 1 on success. */
int  boot_state_save(const CPUState* cpu, uint32_t bios_checksum,
                     uint32_t entry_pc, const char* path);

/* Same as boot_state_save, but into a malloc'd buffer (caller frees *out_data).
 * Compresses large sections (disk + local rewind). */
int  boot_state_save_buffer(const CPUState* cpu, uint32_t bios_checksum,
                            uint32_t entry_pc, uint8_t** out_data,
                            size_t* out_len);

/* In-memory netplay ring snaps: same sections, no zlib. Load accepts either
 * form. Prefer over zlib when every tick must stay under a hard latency budget. */
/* One-shot VRAM source override for the next save (rewind async readback);
 * pass NULL to clear. Only affects the classic full-VRAM section. */
void boot_state_set_vram_override(const uint16_t *vram);
int  boot_state_save_buffer_raw(const CPUState* cpu, uint32_t bios_checksum,
                                uint32_t entry_pc, uint8_t** out_data,
                                size_t* out_len);

/* §96 telemetry: after the latest save, how many VRAM scanlines were dirty
 * and whether the incremental mirror path patched (vs full memcpy). */
uint32_t boot_state_last_vram_dirty_rows(void);
int      boot_state_last_vram_incremental(void);

/* Drop the §96 VRAM mirror (RB shutdown / before re-enable). */
void boot_state_vram_mirror_reset(void);

/* Load + validate (integrity key) + restore the full machine. On any mismatch
 * or incompleteness returns 0 (caller then boots normally and recaptures). */
int  boot_state_load(const char* path, uint32_t bios_checksum,
                     uint32_t entry_pc, CPUState* cpu);

/* Same as boot_state_load, but from an already-buffered .pst image (netplay). */
int  boot_state_load_buffer(const uint8_t* file, size_t file_len,
                            uint32_t bios_checksum, uint32_t entry_pc,
                            CPUState* cpu);

/* Suppress the per-load "savestate: load_timing" stderr line (dual-console
 * machine switching loads at ~100 Hz). Default off. */
void boot_state_set_quiet_load(int on);

/* Sections the caller owns by other means and wants left out of the blob
 * entirely — neither written on save nor required on load. Sticky; pass 0 to
 * clear. Dual-console switching excludes BS_SEC_RAM because it hands DRAM over
 * by swapping bank pointers (memory_ram_bank_activate) instead of copying it. */
void     boot_state_set_section_exclude(uint32_t mask);
uint32_t boot_state_section_exclude(void);

/* Header-only integrity check (no section inflate/apply). Returns 1 if this
 * build can load the image; 0 and fills reason (when non-NULL) on reject. */
int  boot_state_check_buffer(const uint8_t* file, size_t file_len,
                             uint32_t bios_checksum, uint32_t entry_pc,
                             char* reason, size_t reason_cap);

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
