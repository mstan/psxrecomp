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
#ifndef PSXRECOMP_V4_DIRTY_RAM_INTERP_H
#define PSXRECOMP_V4_DIRTY_RAM_INTERP_H

#include <stdint.h>
#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 1 if `addr` lies in a dirty kernel-RAM page and the interpreter
 * ran a basic block at that PC.  Returns 0 if `addr` is clean (caller must
 * fall back to the static dispatch table).  On return with 1, cpu->pc is
 * either 0 (block ended on jr $ra style return) or the target of a tail
 * jump that the dispatch trampoline should re-enter. */
int dirty_ram_dispatch(CPUState* cpu, uint32_t addr);

/* Test whether a given physical kernel-RAM address is in a page that was
 * written-to since boot.  Defined in memory.c. */
int      dirty_ram_is_dirty(uint32_t phys);
uint32_t dirty_ram_get_bitmap(void);
uint32_t dirty_ram_get_bitmap_word(uint32_t word_index);
uint32_t dirty_ram_get_bitmap_word_count(void);
void     dirty_ram_mark_executable_range(uint32_t phys, uint32_t len);

/* Counters for visibility / TCP debug.  Increment in interpreter; expose
 * via debug_server.c if helpful. */
extern uint64_t g_dirty_ram_blocks_run;     /* basic blocks interpreted */
extern uint64_t g_dirty_ram_insns_run;      /* instructions interpreted */
extern uint64_t g_dirty_ram_aborts;         /* unsupported-opcode aborts */
extern uint64_t g_dirty_ram_unsupported_midblock;
extern uint32_t g_dirty_ram_last_unsupported_pc;
extern uint32_t g_dirty_ram_last_unsupported_insn;
extern const char *g_dirty_ram_last_unsupported_reason;

/* Per-entry-PC counters.  Aggregate counters above hide which install
 * stubs actually fire — a single noisy spurious-dispatch site can mask
 * a legitimate handler that never runs.  This open-addressed table keys
 * on the entry PC of each interpreted block. */
#define DIRTY_RAM_PC_TABLE_SIZE 64
typedef struct {
    uint32_t pc;        /* entry PC, 0 = empty slot */
    uint64_t hits;      /* number of times dispatched here */
    uint64_t insns;     /* total instructions executed across hits */
} DirtyRamPcEntry;
extern DirtyRamPcEntry g_dirty_ram_pc_table[DIRTY_RAM_PC_TABLE_SIZE];

/* Block-entry ring buffer. Records every dispatch into dirty RAM with the
 * caller's RA at entry, plus a0/a1 for call-arg context — answers
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
#define DIRTY_RAM_BLOCK_LOG_CAP (1u << 22) /* 4M entries (~128 MB).
 * At ~580K dispatches/s during boot, this retains ~7s of history; at
 * ~10K/s during modal idle, ~400s. Sized for retroactive press-window
 * analysis without the prior 16K ring's 28-ms eviction problem. */
typedef struct {
    uint64_t seq;       /* monotonic, unique per entry */
    uint32_t target;    /* entry PC (RAM address) */
    uint32_t ra;        /* cpu->gpr[31] at dispatch — caller's return target */
    uint32_t a0;        /* cpu->gpr[4] at dispatch */
    uint32_t a1;        /* cpu->gpr[5] at dispatch */
    uint32_t frame;     /* s_frame_count at the time of dispatch */
    uint32_t pad;       /* align to 32 bytes */
} DirtyRamBlockLogEntry;
extern DirtyRamBlockLogEntry g_dirty_ram_block_log[DIRTY_RAM_BLOCK_LOG_CAP];
extern uint64_t              g_dirty_ram_block_log_seq;

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_V4_DIRTY_RAM_INTERP_H */
