/* cosim_state.h — full-architectural-state canonical hash for the first-divergence
 * co-simulation oracle. See COSIM_ORACLE.md.
 *
 * The single correctness rule: this hash must cover EVERY piece of state that can
 * influence future guest execution, and NOTHING that is host-only (pointers, padding,
 * jmpbufs, fiber/malloc addresses). A missed execution-relevant field is a blind spot
 * (false "no divergence"); an included host-only field is a false positive. The
 * validation gates in COSIM_ORACLE.md (compiled-vs-compiled == 0, injected-divergence
 * halts at the right field) exist to prove this list is exactly right.
 */
#pragma once
#include <stdint.h>
#include "cpu_state.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Per-subsystem sub-hashes so a mismatch localizes to a subsystem before a full
 * field/byte diff. All 64-bit FNV-1a over canonical little-endian serialization. */
typedef struct {
    uint64_t cpu;      /* GPRs/HI/LO/PC/COP0/GTE + micro-state (muldiv/gte/load-delay) */
    uint64_t irqctl;   /* i_stat/i_mask + interrupts.c timing statics + VBLANK schedule */
    uint64_t ram;      /* 2MB main RAM (incremental page-hash) */
    uint64_t scratch;  /* 1KB scratchpad */
    uint64_t vram;     /* 1MB VRAM (incremental page-hash) */
    uint64_t gpu;      /* GPU regs + FIFO + draw budget + deadlines (boot_state blob) */
    uint64_t spu;      /* SPU regs + voices + SPU RAM */
    uint64_t cdrom;    /* CDROM command/FIFO/phase/next-event */
    uint64_t dma;      /* DMA channel regs + in-flight/deadlines */
    uint64_t sio;      /* SIO/pad/memcard transaction state */
    uint64_t sio1;     /* SIO1 serial link regs + FIFO + shifter + wire queue */
    uint64_t timers;   /* RootCounters counter/mode/target/frac/last-update */
    uint64_t clock;    /* psx_cycle_count */
    uint64_t dirty;    /* dirty-RAM page bitmap (affects dispatch/interp routing) */
} CosimSubHashes;

/* Compute the full canonical state hash of the LIVE machine (debug_cpu_ptr + globals).
 * Fills `sub` (may be NULL) and returns the folded top hash. Cheap per call: RAM/VRAM
 * use incremental page hashes maintained by cosim_note_ram_write/cosim_note_vram_write;
 * everything else is small. */
uint64_t cosim_state_hash(CosimSubHashes *sub);

/* Incremental page-hash maintenance. Call from the RAM/VRAM write chokepoints in the
 * cosim build: mark the touched page dirty (cheap); the page hash is recomputed lazily
 * inside cosim_state_hash(). phys is the byte offset into RAM (0..2MB) / VRAM (0..1MB). */
void cosim_note_ram_write(uint32_t phys, uint32_t nbytes);
void cosim_note_vram_write(uint32_t byte_off, uint32_t nbytes);

/* Reset all incremental state (call once at machine init / after a full state load). */
void cosim_state_reset(void);

/* ---- engine (cosim.c) ---- */
void cosim_init(void);            /* start the TCP oracle server (call once at startup) */
void cosim_block(uint32_t pc);    /* block-leader pc stash (emitted + interp), reporting */
uint32_t cosim_last_block(void);  /* last block-leader pc, for diagnostic attribution */
void cosim_tick(void);            /* cycle-keyed checkpoint (called from psx_advance_cycles) */
void cosim_instr(uint32_t pc);    /* retire pending cycle checkpoints at an instruction boundary */
uint32_t cosim_cycles_to_next_checkpoint(void);

/* Gate-4 fault injection: after this is armed, the next cosim_state_hash perturbs one
 * byte of RAM (addr) or one CPU GPR (reg 0..31, hi=32, lo=33) by XOR val, so the
 * oracle MUST halt at exactly that field. Returns via the same hash path. */
void cosim_inject_ram(uint32_t phys, uint8_t xor_val);
void cosim_inject_reg(int reg_index, uint32_t xor_val);

#ifdef __cplusplus
}
#endif
