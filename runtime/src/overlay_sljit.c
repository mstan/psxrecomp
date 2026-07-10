/* overlay_sljit.c — Tier-2 self-contained in-process JIT backend (sljit).
 * See overlay_sljit.h for the tier model and the precision-over-recall SAFETY
 * CONTRACT. Provides: backend-selection policy, sljit availability + a real
 * codegen smoke test, and the MIPS->sljit emitter (overlay_sljit_try_compile)
 * that parallels dirty_ram_interp.c — leaf+multi-block functions with branches,
 * mult/div, jal/jalr calls, jr-tables, GTE/COP2, unaligned mem, and block-local
 * GPR register allocation. Unsupported instructions/shapes abort the WHOLE
 * fragment (return fn=NULL → caller runs the interpreter), so the emitter can
 * decline but never mis-compile. The validated gcc path is untouched. */

#include "overlay_sljit.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "sljitLir.h"
#include "gpu.h"
#include "ws_backdrop_detect.h"  /* shared backdrop-window detector (auto_backdrop) */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

/* ---- counters ---------------------------------------------------------- */
/* RECURSION_BUG.md §25 — 1 when the build is continuation-passing (set by a
 * constructor in the generated CPS dispatch). Under CPS this JIT must emit the
 * tail-transfer contract: jr $ra publishes cpu->pc, and call-containing
 * fragments decline to the (CPS-safe, depth-1) interpreter instead of the legacy
 * nested psx_sljit_call. Defined in overlay_loader.c. */
extern int g_psx_cps_mode;

static int            s_selftest_ok = -1; /* -1 = not run */
static uint64_t       s_compiles = 0;
static uint64_t       s_declines = 0;
static uint64_t       s_bytes    = 0;
static char           s_last_msg[256];

static void sljit_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(s_last_msg, sizeof(s_last_msg), fmt, ap);
    va_end(ap);
}

/* Backend-selection policy moved to overlay_backend.{c,h} (it is compiler-
 * neutral — gcc/tcc/sljit/auto — and does not belong in the sljit emitter). */

int overlay_sljit_available(void) {
    /* Debug A/B knob: PSX_NO_SLJIT=1 makes sljit report unavailable, so the
     * overlay path falls through to the dirty-RAM interpreter (no JIT shards).
     * Paired with [runtime] overlay_cache=false (no gcc DLLs / autocompile) this
     * forces INTERP-ONLY overlay execution — isolates the static<->interp
     * boundary/dispatch from JIT codegen when bisecting a regression. */
    static int disabled = -1;
    if (disabled < 0) {
        const char *e = getenv("PSX_NO_SLJIT");
        disabled = (e && *e && *e != '0') ? 1 : 0;
    }
    return disabled ? 0 : 1;
}

/* ---- smoke test: JIT a trivial leaf and run it ------------------------- */
/* Produces machine code for `sljit_sw f(sljit_sw a) { return a + 1234; }`,
 * runs it, and checks the result. Proves the codegen + executable allocator
 * work in this build/host. */
typedef sljit_sw (SLJIT_FUNC *SmokeFn)(sljit_sw);

int overlay_sljit_selftest(void) {
    if (s_selftest_ok >= 0) return s_selftest_ok;

    struct sljit_compiler *C = sljit_create_compiler(NULL);
    if (!C) { s_selftest_ok = 0; sljit_log("selftest: create_compiler failed"); return 0; }

    /* one arg (W) -> arrives in saved reg S0; one scratch, one saved, no locals */
    sljit_emit_enter(C, 0, SLJIT_ARGS1(W, W), 1, 1, 0);
    sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0, SLJIT_S0, 0, SLJIT_IMM, 1234);
    sljit_emit_return(C, SLJIT_MOV, SLJIT_R0, 0);

    void *code = sljit_generate_code(C, 0, NULL);
    sljit_uw code_size = sljit_get_generated_code_size(C);
    sljit_free_compiler(C);

    if (!code) { s_selftest_ok = 0; sljit_log("selftest: generate_code failed"); return 0; }

    SmokeFn fn = (SmokeFn)code;
    sljit_sw got = fn(1000);
    sljit_free_code(code, NULL);

    s_selftest_ok = (got == 2234) ? 1 : 0;
    sljit_log("selftest: f(1000)=%ld expected=2234 ok=%d code_size=%lu",
              (long)got, s_selftest_ok, (unsigned long)code_size);
    return s_selftest_ok;
}

/* ======================================================================== *
 *  MIPS -> sljit emitter (SLJIT.md §7 step 4, first slice)
 *
 *  Parallels dirty_ram_interp.c's exec_one — same decode, but emits host code
 *  instead of stepping. PARITY RULES:
 *    - GPRs are MEMORY-BACKED in the CPUState struct (gpr[n] at [S0 + 4n]); each
 *      instruction loads operands, computes, stores back. gpr[0] is never
 *      written (hardwired 0); reads of gpr[0] read the struct's 0. No host
 *      register allocation across instructions (a later pass).
 *    - 32-bit ops everywhere (SLJIT_*32) so wraparound matches the R3000A.
 *    - memory access routes through the cpu read/write callbacks (icall) exactly
 *      like the interpreter, so MMIO / page-watch / dirty-tracking behave alike.
 *  FIRST SLICE shape: single-block LEAF functions only, terminator `jr $ra`.
 *  ANY other control transfer / unsupported opcode aborts the WHOLE fragment
 *  (out->fn = NULL) and the caller runs the interpreter (precision over recall).
 * ======================================================================== */

/* cpu pointer lives in S0 after emit_enter(SLJIT_ARGS1V(P), ...). S1 is the
 * control-transfer temp (branch predicate / jalr|jr-rX target), live only across
 * a transfer's delay slot — never used by the GPR cache (which starts at S2). */
#define R_CPU    SLJIT_S0
#define R_CTRL   SLJIT_S1
#define GPR_OFF(n)  ((sljit_sw)(offsetof(CPUState, gpr) + 4u * (n)))

/* MIPS field decoders (mirror dirty_ram_interp.c). */
static inline uint32_t f_op   (uint32_t i) { return (i >> 26) & 0x3Fu; }
static inline uint32_t f_rs   (uint32_t i) { return (i >> 21) & 0x1Fu; }
static inline uint32_t f_rt   (uint32_t i) { return (i >> 16) & 0x1Fu; }
static inline uint32_t f_rd   (uint32_t i) { return (i >> 11) & 0x1Fu; }
static inline uint32_t f_sh   (uint32_t i) { return (i >>  6) & 0x1Fu; }
static inline uint32_t f_fn   (uint32_t i) { return  i        & 0x3Fu; }
static inline uint32_t f_imm  (uint32_t i) { return  i        & 0xFFFFu; }
static inline uint32_t f_tgt26(uint32_t i) { return  i        & 0x03FFFFFFu; }
static inline sljit_sw f_simm (uint32_t i) { return (sljit_sw)(int32_t)(int16_t)f_imm(i); }

/* ===================== block-local GPR register cache =====================
 * (SLJIT.md "register allocation" — the dominant perf win.) The memory-backed
 * model loaded each operand from cpu->gpr[] and stored each result back, so a
 * value produced by one instruction and consumed by the next made a useless
 * round-trip through memory. Instead we cache MIPS GPRs in host SAVED registers
 * (S2..S{N-1}) across a straight-line basic block. Saved registers are callee-
 * saved, so they survive the read_/write_ icalls a plain load/store emits — the
 * cache is only disturbed at basic-block boundaries and around helpers/calls
 * that touch cpu->gpr[] in memory:
 *   - gpr_flush  stores dirty slots to memory (keeps them, now clean): before
 *                every control transfer, and before a helper/call that READS gpr.
 *   - gpr_reset  drops all mappings (no store): at every branch-target label (a
 *                join — all edges then read from memory) AFTER a preceding flush,
 *                and after any helper/call that WROTE gpr (memory now supersedes
 *                the cache). Unconditional transfers reset too (following code is
 *                dead or label-guarded).
 *   - gpr[0] is never cached (reads => IMM 0, writes => discarded).
 * Correctness is independent of perf: a cache bug surfaces only as a same-state
 * differential divergence (sljit shards run live ONLY after passing it), never
 * as a silently-wrong shipped shard (the precision-over-recall contract). */
#define GPR_SLOTS_MAX (SLJIT_NUMBER_OF_SAVED_REGISTERS - 2)  /* reserve S0, S1 */
typedef struct { int reg; int dirty; int pinned; uint32_t lru; } GprSlot;
static GprSlot  s_gpr[GPR_SLOTS_MAX];
static int      s_gpr_slots;     /* active slots this compile (<= GPR_SLOTS_MAX)*/
static uint32_t s_gpr_lru;
/* Set per-fragment (PASS 1) when this fragment carries the GTE render-funnel
 * screen-extent reject signature (sltiu 0x140/0x141 + sltiu 0xE0/0xF1). When
 * set, SLTIU 0x140/0x141 is routed through psx_ws_cull_sltiu so a sljit-JIT'd
 * overlay widens identically to the gcc cache + interp (widescreen FOV). */
static int      s_frag_ws_cull;
extern int psx_ws_cull_sltiu(uint32_t sx, uint32_t imm);  /* gpu.c — shared widen */
extern int psx_ws_cull_slti(uint32_t sx, uint32_t imm);   /* gpu.c — signed right edge */
extern int32_t psx_ws_player_x_bound(int32_t vanilla);    /* gpu.c — gameplay X clamp */
extern int psx_ws_cull_bltz(uint32_t v);                  /* gpu.c — signed left edge */
extern int psx_ws_is_cull_w_imm(uint32_t imm);            /* gpu.c — per-game W-imm set */
extern int psx_ws_auto_cull_on(void);                     /* gpu.c — per-game opt-in gate */
extern int psx_ws_cull_bltz_at(const uint32_t *words, int n, int idx);  /* gpu.c */
extern int psx_ws_func_has_screen_cull(const uint32_t *words, int n);   /* gpu.c */
#define SLJIT_MAX_FRAG_INSNS 2048u
#define SLJIT_MAX_FRAG_CTRL  512u   /* branches/jumps per fragment cap */
/* Per-fragment (PASS 1) map of classified LEFT-edge funnel bltz words
 * (ws_cull_detect.h idioms 2/3): emit_cond_to_S1 routes those through
 * psx_ws_cull_bltz so the reject widens with the revealed margin. */
static uint8_t  s_ws_bltz[SLJIT_MAX_FRAG_INSNS];
/* Per-fragment (PASS 1) backdrop-preload site map ([widescreen.cull]
 * auto_backdrop): s_bd_kind[i] = WS_BD_START_ZERO/END_WIDEN for the window
 * START/END finalize at fragment word i, else WS_BD_NONE. emit_one routes those
 * move/addiu through psx_ws_backdrop_value so a JIT'd backdrop generator
 * preloads the whole tile row identically to the gcc cache + interp. */
static uint8_t  s_bd_kind[SLJIT_MAX_FRAG_INSNS];
static int16_t  s_bd_cols[SLJIT_MAX_FRAG_INSNS];   /* window width (cols) per site */
extern uint32_t psx_ws_backdrop_value(uint32_t orig, int is_end, int window_cols);  /* gpu.c — shared widen */

static inline sljit_s32 slot_reg(int k) { return SLJIT_S(k + 2); } /* S2.. */

static void gpr_reset(void) {
    for (int k = 0; k < s_gpr_slots; k++) { s_gpr[k].reg = -1; s_gpr[k].dirty = 0; s_gpr[k].pinned = 0; }
}
static void gpr_flush(struct sljit_compiler *C) {
    for (int k = 0; k < s_gpr_slots; k++)
        if (s_gpr[k].reg >= 0 && s_gpr[k].dirty) {
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU),
                           GPR_OFF((uint32_t)s_gpr[k].reg), slot_reg(k), 0);
            s_gpr[k].dirty = 0;
        }
}
static int gpr_find(int n) {
    for (int k = 0; k < s_gpr_slots; k++) if (s_gpr[k].reg == n) return k;
    return -1;
}
/* Pick a slot for MIPS reg n (n != 0): reuse n's slot, else an empty slot, else
 * evict the LRU unpinned slot (flushing it if dirty). At most 3 slots are pinned
 * per instruction and s_gpr_slots >= 4 whenever any reg is used, so a victim
 * always exists. */
static int gpr_alloc(struct sljit_compiler *C, int n) {
    int k = gpr_find(n);
    if (k >= 0) return k;
    int empty = -1, victim = -1; uint32_t best = 0xFFFFFFFFu;
    for (int j = 0; j < s_gpr_slots; j++) {
        if (s_gpr[j].reg < 0) { empty = j; break; }
        if (!s_gpr[j].pinned && s_gpr[j].lru < best) { best = s_gpr[j].lru; victim = j; }
    }
    k = (empty >= 0) ? empty : victim;
    if (k < 0) k = 0;   /* unreachable given the pin invariant; defensive */
    if (s_gpr[k].reg >= 0 && s_gpr[k].dirty)
        sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU),
                       GPR_OFF((uint32_t)s_gpr[k].reg), slot_reg(k), 0);
    s_gpr[k].reg = n; s_gpr[k].dirty = 0; s_gpr[k].pinned = 0;
    return k;
}
/* Source operand for reading MIPS reg n. $0 => IMM 0; else the slot host reg
 * (loading it from memory on a cache miss). Pins the slot for this instruction. */
static void gpr_src(struct sljit_compiler *C, uint32_t n, sljit_s32 *r, sljit_sw *w) {
    *w = 0;
    if (n == 0) { *r = SLJIT_IMM; return; }
    int k = gpr_find((int)n);
    if (k < 0) {
        k = gpr_alloc(C, (int)n);
        sljit_emit_op1(C, SLJIT_MOV32, slot_reg(k), 0, SLJIT_MEM1(R_CPU), GPR_OFF(n));
    }
    s_gpr[k].pinned = 1; s_gpr[k].lru = ++s_gpr_lru;
    *r = slot_reg(k);
}
/* Destination host reg for writing MIPS reg n. $0 => scratch sink (discarded);
 * else the (dirty-marked) slot reg. Won't evict pinned (source) slots. */
static sljit_s32 gpr_dst(struct sljit_compiler *C, uint32_t n) {
    if (n == 0) return SLJIT_R0;
    int k = gpr_alloc(C, (int)n);
    s_gpr[k].dirty = 1; s_gpr[k].lru = ++s_gpr_lru;
    return slot_reg(k);
}
static void gpr_unpin(void) { for (int k = 0; k < s_gpr_slots; k++) s_gpr[k].pinned = 0; }

/* Force an operand to be a register (materialize an IMM into `scratch`). Only
 * needed where sljit would see BOTH operands as immediates (the rare $0,$0
 * shape) or an icall/DIVMOD requires a named scratch. */
static void as_reg(struct sljit_compiler *C, sljit_s32 *r, sljit_sw *w, sljit_s32 scratch) {
    if (*r == SLJIT_IMM) {
        sljit_emit_op1(C, SLJIT_MOV32, scratch, 0, SLJIT_IMM, *w);
        *r = scratch; *w = 0;
    }
}

/* rd = rs <op2> rt (register-register ALU). */
static void emit_alu_rr(struct sljit_compiler *C, sljit_s32 op,
                        uint32_t rd, uint32_t rs, uint32_t rt) {
    sljit_s32 a, b, d; sljit_sw aw, bw;
    gpr_src(C, rs, &a, &aw);
    gpr_src(C, rt, &b, &bw);
    if (a == SLJIT_IMM && b == SLJIT_IMM) as_reg(C, &a, &aw, SLJIT_R0);
    d = gpr_dst(C, rd);
    sljit_emit_op2(C, op, d, 0, a, aw, b, bw);
    gpr_unpin();
}

/* rd = rt <shift> sh (immediate shift). */
static void emit_shift_imm(struct sljit_compiler *C, sljit_s32 op,
                           uint32_t rd, uint32_t rt, uint32_t sh) {
    sljit_s32 a, d; sljit_sw aw;
    gpr_src(C, rt, &a, &aw);
    if (a == SLJIT_IMM) as_reg(C, &a, &aw, SLJIT_R0);
    d = gpr_dst(C, rd);
    sljit_emit_op2(C, op, d, 0, a, aw, SLJIT_IMM, (sljit_sw)sh);
    gpr_unpin();
}

/* rd = rt <shift> (rs & 31) (variable shift). */
static void emit_shift_var(struct sljit_compiler *C, sljit_s32 op,
                           uint32_t rd, uint32_t rt, uint32_t rs) {
    sljit_s32 a, c, d; sljit_sw aw, cw;
    gpr_src(C, rs, &c, &cw);
    if (c == SLJIT_IMM) {
        /* count known at compile time (rs == $0 => 0): fold to an immediate shift */
        gpr_src(C, rt, &a, &aw);
        if (a == SLJIT_IMM) as_reg(C, &a, &aw, SLJIT_R0);
        d = gpr_dst(C, rd);
        sljit_emit_op2(C, op, d, 0, a, aw, SLJIT_IMM, (sljit_sw)(cw & 31));
    } else {
        sljit_emit_op2(C, SLJIT_AND32, SLJIT_R1, 0, c, 0, SLJIT_IMM, 31);
        gpr_src(C, rt, &a, &aw);
        if (a == SLJIT_IMM) as_reg(C, &a, &aw, SLJIT_R0);
        d = gpr_dst(C, rd);
        sljit_emit_op2(C, op, d, 0, a, aw, SLJIT_R1, 0);
    }
    gpr_unpin();
}

/* dst = (rs <cond> b) ? 1 : 0, via flags. b is gpr[rt] (has_rt) or `imm`. The
 * dest slot is allocated BEFORE the flag-setting compare so a spill store can't
 * clobber the flags between op2u and op_flags. */
static void emit_slt(struct sljit_compiler *C, sljit_s32 setflag, sljit_s32 cc,
                     uint32_t dst, uint32_t rs, int has_rt, uint32_t rt, sljit_sw imm) {
    sljit_s32 a, b, d; sljit_sw aw, bw;
    gpr_src(C, rs, &a, &aw);
    if (has_rt) gpr_src(C, rt, &b, &bw); else { b = SLJIT_IMM; bw = imm; }
    if (a == SLJIT_IMM && b == SLJIT_IMM) as_reg(C, &a, &aw, SLJIT_R0);
    d = gpr_dst(C, dst);
    sljit_emit_op2u(C, setflag, a, aw, b, bw);
    sljit_emit_op_flags(C, SLJIT_MOV32, d, 0, cc);
    gpr_unpin();
}

/* Load via cpu->read_* (icall ARGS1(32,32)); addr = gpr[rs] + simm. `ext`
 * is the post-call sign/zero extension op (SLJIT_MOV32 for word). The icall
 * clobbers scratch regs only; the GPR cache (saved regs) survives. */
static void emit_load(struct sljit_compiler *C, size_t fnoff, sljit_s32 ext,
                      uint32_t rt, uint32_t rs, sljit_sw simm) {
    sljit_s32 a; sljit_sw aw;
    gpr_src(C, rs, &a, &aw);
    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, a, aw);          /* addr -> R0 */
    if (simm) sljit_emit_op2(C, SLJIT_ADD32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, simm);
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(R_CPU), (sljit_sw)fnoff);
    sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1(32, 32), SLJIT_R1, 0);
    if (ext != SLJIT_MOV32)
        sljit_emit_op1(C, ext, SLJIT_R0, 0, SLJIT_R0, 0);
    sljit_emit_op1(C, SLJIT_MOV32, gpr_dst(C, rt), 0, SLJIT_R0, 0);
    gpr_unpin();
}

/* Store via cpu->write_* (icall ARGS2V(32,32)); addr = gpr[rs]+simm, val=gpr[rt]. */
static void emit_store(struct sljit_compiler *C, size_t fnoff,
                       uint32_t rt, uint32_t rs, sljit_sw simm) {
    sljit_s32 a, b; sljit_sw aw, bw;
    gpr_src(C, rs, &a, &aw);
    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, a, aw);          /* addr -> R0 */
    if (simm) sljit_emit_op2(C, SLJIT_ADD32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, simm);
    gpr_src(C, rt, &b, &bw);
    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R1, 0, b, bw);          /* value -> R1 */
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0, SLJIT_MEM1(R_CPU), (sljit_sw)fnoff);
    sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS2V(32, 32), SLJIT_R2, 0);
    gpr_unpin();
}

/* Emit a call to a void helper(CPUState*, uint32_t insn) — used for op-classes
 * interpreted by a parity-exact runtime helper (GTE/COP2, unaligned mem) rather
 * than inlined. The helper READS and WRITES cpu->gpr[] in memory, so the cache
 * is flushed before and dropped after. cpu in R0, insn in R1, helper in R2. */
/* helper_idx is a SLJIT_HLP_* index into cpu->sljit_helpers (cpu-relative, NOT a
 * baked pointer) so the emitted LIR is position-independent — see Stage 1 in
 * SLJIT_PERSIST_CACHE.md. */
static void emit_helper2(struct sljit_compiler *C, int helper_idx, uint32_t insn) {
    gpr_flush(C);
    sljit_emit_op1(C, SLJIT_MOV,   SLJIT_R0, 0, R_CPU, 0);
    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)insn);
    sljit_emit_op1(C, SLJIT_MOV,   SLJIT_R2, 0, SLJIT_MEM1(R_CPU),
                   (sljit_sw)(offsetof(CPUState, sljit_helpers)
                              + (size_t)helper_idx * sizeof(void *)));
    sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS2V(P, 32), SLJIT_R2, 0);
    gpr_reset();
}

/* Populate the cpu-relative host-helper table the emitter calls through (Stage 1
 * of the persisted sljit cache). Order MUST match the SLJIT_HLP_* enum. Called
 * once at startup, after CPUState's memory fn-pointers are wired. */
void overlay_sljit_init_helpers(CPUState *cpu) {
    cpu->sljit_helpers[SLJIT_HLP_MEMX]         = (void *)psx_sljit_memx;
    cpu->sljit_helpers[SLJIT_HLP_COP2]         = (void *)psx_sljit_cop2;
    cpu->sljit_helpers[SLJIT_HLP_WS_CULL]      = (void *)psx_ws_cull_sltiu;
    cpu->sljit_helpers[SLJIT_HLP_CALL]         = (void *)psx_sljit_call;
    cpu->sljit_helpers[SLJIT_HLP_WS_CULL_SLTI] = (void *)psx_ws_cull_slti;
    cpu->sljit_helpers[SLJIT_HLP_WS_CULL_BLTZ] = (void *)psx_ws_cull_bltz;
    cpu->sljit_helpers[SLJIT_HLP_WS_PLAYER_X_BOUND] = (void *)psx_ws_player_x_bound;
}

static void emit_ws_player_x_bound(struct sljit_compiler *C, uint32_t rt, int32_t vanilla) {
    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)vanilla);
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0, SLJIT_MEM1(R_CPU),
                   (sljit_sw)(offsetof(CPUState, sljit_helpers)
                              + (size_t)SLJIT_HLP_WS_PLAYER_X_BOUND * sizeof(void *)));
    sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1(32, 32), SLJIT_R2, 0);
    sljit_emit_op1(C, SLJIT_MOV32, gpr_dst(C, rt), 0, SLJIT_R0, 0);
    gpr_unpin();
}

/* Emit `rt = <cull helper>(gpr[rs], imm)` for a flagged auto_screen_x cull
 * site (helper_idx = SLJIT_HLP_WS_CULL for the unsigned per-vertex idiom,
 * SLJIT_HLP_WS_CULL_SLTI for the signed min/max right edge), so a sljit-JIT'd
 * overlay widens the render-funnel screen-X reject the same way the gcc
 * backend does. The helper reads only its two args (+ the runtime widescreen
 * margin); it does NOT touch cpu->gpr[], so the GPR cache survives — same
 * icall discipline as emit_load (materialise the source into a scratch before
 * the call, write the result into rt after). */
static void emit_ws_cull(struct sljit_compiler *C, uint32_t rt, uint32_t rs, uint32_t imm,
                         int helper_idx) {
    sljit_s32 a; sljit_sw aw;
    gpr_src(C, rs, &a, &aw);
    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, a, aw);                 /* sx  -> R0 (arg0) */
    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)imm); /* imm -> R1 (arg1) */
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0, SLJIT_MEM1(R_CPU),           /* fn (cpu-relative) */
                   (sljit_sw)(offsetof(CPUState, sljit_helpers)
                              + (size_t)helper_idx * sizeof(void *)));
    sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS2(32, 32, 32), SLJIT_R2, 0); /* R0 = verdict */
    sljit_emit_op1(C, SLJIT_MOV32, gpr_dst(C, rt), 0, SLJIT_R0, 0);
    gpr_unpin();
}

enum { EMIT_OK = 0, EMIT_TERM = 1, EMIT_ABORT = 2 };

/* Emit ONE non-control instruction. Returns EMIT_OK, or EMIT_TERM iff `insn`
 * is `jr $ra` (the only terminator this slice supports — caller then emits the
 * delay slot + the shard return), or EMIT_ABORT for anything outside the slice
 * (any other control transfer, or an unsupported opcode). */
static int emit_one(struct sljit_compiler *C, uint32_t pc, uint32_t insn, int bd_kind, int bd_cols) {
    uint32_t op = f_op(insn), rs = f_rs(insn), rt = f_rt(insn);
    uint32_t rd = f_rd(insn), sh = f_sh(insn), fn = f_fn(insn);
    uint32_t imm = f_imm(insn);
    sljit_sw simm = f_simm(insn);

    /* Widescreen far-backdrop column PRELOAD (auto_backdrop): this move/addiu is
     * a detected window START/END bound — route its value through
     * psx_ws_backdrop_value(orig, is_end, window_cols) so native-wide widens the
     * camera-tracked window by the 16:9 reveal (identity at 4:3). The helper
     * reads only its three args (+ runtime widescreen state), not cpu->gpr[], so
     * the GPR cache survives the icall — same discipline as emit_ws_cull. Three
     * args (R0/R1/R2) means the fn ptr goes in R3 to avoid clobbering arg2. */
    if (bd_kind != WS_BD_NONE) {
        int is_end = (bd_kind == WS_BD_END);
        sljit_s32 a; sljit_sw aw;
        if (op == 0x00u && (fn == 0x21u || fn == 0x25u)) {           /* move (addu/or rD,rS,$0) */
            uint32_t src = (rt == 0u) ? rs : rt;
            gpr_src(C, src, &a, &aw);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, a, aw);      /* orig -> R0 */
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)is_end);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, (sljit_sw)bd_cols);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0, SLJIT_IMM, (sljit_sw)(uintptr_t)psx_ws_backdrop_value);
            sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3(32, 32, 32, 32), SLJIT_R3, 0);
            sljit_emit_op1(C, SLJIT_MOV32, gpr_dst(C, rd), 0, SLJIT_R0, 0);
            gpr_unpin();
            return EMIT_OK;
        }
        if (op == 0x08u || op == 0x09u) {                           /* addiu/addi rT,rS,N */
            gpr_src(C, rs, &a, &aw);
            if (a == SLJIT_IMM)
                sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, aw + simm);   /* orig = N */
            else
                sljit_emit_op2(C, SLJIT_ADD32, SLJIT_R0, 0, a, aw, SLJIT_IMM, simm); /* orig = rS+N */
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)is_end);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, (sljit_sw)bd_cols);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0, SLJIT_IMM, (sljit_sw)(uintptr_t)psx_ws_backdrop_value);
            sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3(32, 32, 32, 32), SLJIT_R3, 0);
            sljit_emit_op1(C, SLJIT_MOV32, gpr_dst(C, rt), 0, SLJIT_R0, 0);
            gpr_unpin();
            return EMIT_OK;
        }
        /* Unexpected shape for a detected site: fall through to normal emit. */
    }

    switch (op) {
    case 0x00: /* SPECIAL */
        switch (fn) {
        case 0x00: emit_shift_imm(C, SLJIT_SHL32,  rd, rt, sh); return EMIT_OK; /* SLL (nop when 0) */
        case 0x02: emit_shift_imm(C, SLJIT_LSHR32, rd, rt, sh); return EMIT_OK; /* SRL */
        case 0x03: emit_shift_imm(C, SLJIT_ASHR32, rd, rt, sh); return EMIT_OK; /* SRA */
        case 0x04: emit_shift_var(C, SLJIT_SHL32,  rd, rt, rs); return EMIT_OK; /* SLLV */
        case 0x06: emit_shift_var(C, SLJIT_LSHR32, rd, rt, rs); return EMIT_OK; /* SRLV */
        case 0x07: emit_shift_var(C, SLJIT_ASHR32, rd, rt, rs); return EMIT_OK; /* SRAV */
        case 0x08: /* JR rs — terminator iff rs == $ra; else outside the slice */
            return (rs == 31) ? EMIT_TERM : EMIT_ABORT;
        case 0x0F: return EMIT_OK; /* SYNC = nop */
        case 0x20: case 0x21: emit_alu_rr(C, SLJIT_ADD32, rd, rs, rt); return EMIT_OK; /* ADD/ADDU */
        case 0x22: case 0x23: emit_alu_rr(C, SLJIT_SUB32, rd, rs, rt); return EMIT_OK; /* SUB/SUBU */
        case 0x24: emit_alu_rr(C, SLJIT_AND32, rd, rs, rt); return EMIT_OK; /* AND */
        case 0x25: emit_alu_rr(C, SLJIT_OR32,  rd, rs, rt); return EMIT_OK; /* OR */
        case 0x26: emit_alu_rr(C, SLJIT_XOR32, rd, rs, rt); return EMIT_OK; /* XOR */
        case 0x27: { /* NOR: ~(rs|rt) */
            sljit_s32 a, b, d; sljit_sw aw, bw;
            gpr_src(C, rs, &a, &aw); gpr_src(C, rt, &b, &bw);
            if (a == SLJIT_IMM && b == SLJIT_IMM) as_reg(C, &a, &aw, SLJIT_R0);
            d = gpr_dst(C, rd);
            sljit_emit_op2(C, SLJIT_OR32,  d, 0, a, aw, b, bw);
            sljit_emit_op2(C, SLJIT_XOR32, d, 0, d, 0, SLJIT_IMM, (sljit_sw)-1);
            gpr_unpin();
            return EMIT_OK;
        }
        case 0x2A: /* SLT (signed) */
            emit_slt(C, SLJIT_SUB32 | SLJIT_SET_SIG_LESS, SLJIT_SIG_LESS, rd, rs, 1, rt, 0);
            return EMIT_OK;
        case 0x2B: /* SLTU (unsigned) */
            emit_slt(C, SLJIT_SUB32 | SLJIT_SET_LESS, SLJIT_LESS, rd, rs, 1, rt, 0);
            return EMIT_OK;
        case 0x10: /* MFHI rd = hi */
            sljit_emit_op1(C, SLJIT_MOV32, gpr_dst(C, rd), 0, SLJIT_MEM1(R_CPU),
                           (sljit_sw)offsetof(CPUState, hi));
            return EMIT_OK;
        case 0x12: /* MFLO rd = lo */
            sljit_emit_op1(C, SLJIT_MOV32, gpr_dst(C, rd), 0, SLJIT_MEM1(R_CPU),
                           (sljit_sw)offsetof(CPUState, lo));
            return EMIT_OK;
        case 0x11: { /* MTHI hi = rs */
            sljit_s32 a; sljit_sw aw; gpr_src(C, rs, &a, &aw); as_reg(C, &a, &aw, SLJIT_R0);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU),
                           (sljit_sw)offsetof(CPUState, hi), a, 0);
            gpr_unpin();
            return EMIT_OK;
        }
        case 0x13: { /* MTLO lo = rs */
            sljit_s32 a; sljit_sw aw; gpr_src(C, rs, &a, &aw); as_reg(C, &a, &aw, SLJIT_R0);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU),
                           (sljit_sw)offsetof(CPUState, lo), a, 0);
            gpr_unpin();
            return EMIT_OK;
        }
        case 0x18: /* MULT (signed 32x32 -> 64): hi:lo = sext(rs) * sext(rt) */
        case 0x19: { /* MULTU (unsigned) */
            /* Extend both operands to a full host word so a single word MUL gives
             * the true 64-bit product; split into lo (low 32) and hi (>>32, the
             * raw upper 32 — interp does (uint64_t)r >> 32, a logical shift). */
            sljit_s32 a, b; sljit_sw aw, bw; sljit_s32 ext = (fn == 0x18) ? SLJIT_MOV_S32 : SLJIT_MOV_U32;
            gpr_src(C, rs, &a, &aw); sljit_emit_op1(C, ext, SLJIT_R0, 0, a, aw);
            gpr_src(C, rt, &b, &bw); sljit_emit_op1(C, ext, SLJIT_R1, 0, b, bw);
            gpr_unpin();
            sljit_emit_op2(C, SLJIT_MUL, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU),
                           (sljit_sw)offsetof(CPUState, lo), SLJIT_R0, 0);
            sljit_emit_op2(C, SLJIT_LSHR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 32);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU),
                           (sljit_sw)offsetof(CPUState, hi), SLJIT_R0, 0);
            return EMIT_OK;
        }
        case 0x1A: { /* DIV (signed). Host idiv traps on /0 and INT_MIN/-1, so
                      * branch-guard both to the interpreter's defined results
                      * BEFORE the DIVMOD ever runs. */
            sljit_s32 a, b; sljit_sw aw, bw;
            gpr_src(C, rs, &a, &aw); sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, a, aw); /* a */
            gpr_src(C, rt, &b, &bw); sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R1, 0, b, bw); /* b */
            gpr_unpin();
            struct sljit_jump *j_b0 =
                sljit_emit_cmp(C, SLJIT_EQUAL | SLJIT_32, SLJIT_R1, 0, SLJIT_IMM, 0);
            struct sljit_jump *j_n1 =
                sljit_emit_cmp(C, SLJIT_NOT_EQUAL | SLJIT_32, SLJIT_R0, 0,
                               SLJIT_IMM, (sljit_sw)0x80000000);
            struct sljit_jump *j_n2 =
                sljit_emit_cmp(C, SLJIT_NOT_EQUAL | SLJIT_32, SLJIT_R1, 0, SLJIT_IMM, -1);
            /* overflow a==INT_MIN, b==-1: lo=0x80000000, hi=0 */
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU), (sljit_sw)offsetof(CPUState, lo),
                           SLJIT_IMM, (sljit_sw)0x80000000);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU), (sljit_sw)offsetof(CPUState, hi),
                           SLJIT_IMM, 0);
            struct sljit_jump *j_da = sljit_emit_jump(C, SLJIT_JUMP);
            /* normal: lo=a/b, hi=a%b */
            struct sljit_label *Ln = sljit_emit_label(C);
            sljit_set_label(j_n1, Ln); sljit_set_label(j_n2, Ln);
            sljit_emit_op0(C, SLJIT_DIVMOD_S32);   /* R0=R0/R1, R1=R0%R1 */
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU), (sljit_sw)offsetof(CPUState, lo), SLJIT_R0, 0);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU), (sljit_sw)offsetof(CPUState, hi), SLJIT_R1, 0);
            struct sljit_jump *j_db = sljit_emit_jump(C, SLJIT_JUMP);
            /* b==0: lo=(a<0)?1:-1, hi=a (a still in R0) */
            struct sljit_label *Lb0 = sljit_emit_label(C);
            sljit_set_label(j_b0, Lb0);
            sljit_emit_op2u(C, SLJIT_SUB32 | SLJIT_SET_SIG_LESS, SLJIT_R0, 0, SLJIT_IMM, 0);
            sljit_emit_op_flags(C, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_SIG_LESS); /* (a<0)?1:0 */
            sljit_emit_op2(C, SLJIT_SHL32, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, 1);
            sljit_emit_op2(C, SLJIT_SUB32, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, 1); /* 1 or -1 */
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU), (sljit_sw)offsetof(CPUState, lo), SLJIT_R2, 0);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU), (sljit_sw)offsetof(CPUState, hi), SLJIT_R0, 0);
            struct sljit_label *Ld = sljit_emit_label(C);
            sljit_set_label(j_da, Ld); sljit_set_label(j_db, Ld);
            return EMIT_OK;
        }
        case 0x1B: { /* DIVU (unsigned). Guard /0 to interp's defined result. */
            sljit_s32 a, b; sljit_sw aw, bw;
            gpr_src(C, rs, &a, &aw); sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, a, aw); /* a */
            gpr_src(C, rt, &b, &bw); sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R1, 0, b, bw); /* b */
            gpr_unpin();
            struct sljit_jump *j_d0 =
                sljit_emit_cmp(C, SLJIT_EQUAL | SLJIT_32, SLJIT_R1, 0, SLJIT_IMM, 0);
            sljit_emit_op0(C, SLJIT_DIVMOD_U32);   /* R0=R0/R1, R1=R0%R1 */
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU), (sljit_sw)offsetof(CPUState, lo), SLJIT_R0, 0);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU), (sljit_sw)offsetof(CPUState, hi), SLJIT_R1, 0);
            struct sljit_jump *j_dn = sljit_emit_jump(C, SLJIT_JUMP);
            /* b==0: lo=0xFFFFFFFF, hi=a */
            struct sljit_label *Ld0 = sljit_emit_label(C);
            sljit_set_label(j_d0, Ld0);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU), (sljit_sw)offsetof(CPUState, lo),
                           SLJIT_IMM, (sljit_sw)0xFFFFFFFF);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU), (sljit_sw)offsetof(CPUState, hi), SLJIT_R0, 0);
            struct sljit_label *Ld = sljit_emit_label(C);
            sljit_set_label(j_dn, Ld);
            return EMIT_OK;
        }
        default: return EMIT_ABORT;
        }
    case 0x08: case 0x09: { /* ADDI/ADDIU rt = rs + simm */
        sljit_s32 a, d; sljit_sw aw;
        gpr_src(C, rs, &a, &aw);
        d = gpr_dst(C, rt);
        if (a == SLJIT_IMM)   /* rs == $0: rt = simm (the common li idiom) */
            sljit_emit_op1(C, SLJIT_MOV32, d, 0, SLJIT_IMM, aw + simm);
        else
            sljit_emit_op2(C, SLJIT_ADD32, d, 0, a, aw, SLJIT_IMM, simm);
        gpr_unpin();
        return EMIT_OK;
    }
    case 0x0A: /* SLTI (signed) */
        if (s_frag_ws_cull && psx_ws_is_cull_w_imm(imm))
            emit_ws_cull(C, rt, rs, imm, SLJIT_HLP_WS_CULL_SLTI);  /* signed funnel right edge */
        else
            emit_slt(C, SLJIT_SUB32 | SLJIT_SET_SIG_LESS, SLJIT_SIG_LESS, rt, rs, 0, 0, simm);
        return EMIT_OK;
    case 0x0B: /* SLTIU (unsigned, simm sign-extended then compared unsigned) */
        if (s_frag_ws_cull && psx_ws_is_cull_w_imm(imm))
            emit_ws_cull(C, rt, rs, imm, SLJIT_HLP_WS_CULL);   /* widescreen render-funnel cull widen (auto_screen_x) */
        else
            emit_slt(C, SLJIT_SUB32 | SLJIT_SET_LESS, SLJIT_LESS, rt, rs, 0, 0, simm);
        return EMIT_OK;
    case 0x0C: case 0x0D: case 0x0E: { /* ANDI / ORI / XORI (zero-extended imm) */
        sljit_s32 a, d; sljit_sw aw;
        sljit_s32 aop = (op == 0x0C) ? SLJIT_AND32 : (op == 0x0D) ? SLJIT_OR32 : SLJIT_XOR32;
        gpr_src(C, rs, &a, &aw);
        if (a == SLJIT_IMM) as_reg(C, &a, &aw, SLJIT_R0);   /* rs == $0 */
        d = gpr_dst(C, rt);
        sljit_emit_op2(C, aop, d, 0, a, aw, SLJIT_IMM, (sljit_sw)imm);
        gpr_unpin();
        return EMIT_OK;
    }
    case 0x0F: /* LUI rt = imm << 16 */
        if (psx_ws_is_signed_x_bound_site(pc, insn))
            emit_ws_player_x_bound(C, rt, (int32_t)(imm << 16));
        else
            sljit_emit_op1(C, SLJIT_MOV32, gpr_dst(C, rt), 0, SLJIT_IMM, (sljit_sw)(imm << 16));
        return EMIT_OK;
    case 0x20: emit_load(C, offsetof(CPUState, read_byte), SLJIT_MOV_S8,  rt, rs, simm); return EMIT_OK; /* LB */
    case 0x21: emit_load(C, offsetof(CPUState, read_half), SLJIT_MOV_S16, rt, rs, simm); return EMIT_OK; /* LH */
    case 0x23: emit_load(C, offsetof(CPUState, read_word), SLJIT_MOV32,   rt, rs, simm); return EMIT_OK; /* LW */
    case 0x24: emit_load(C, offsetof(CPUState, read_byte), SLJIT_MOV_U8,  rt, rs, simm); return EMIT_OK; /* LBU */
    case 0x25: emit_load(C, offsetof(CPUState, read_half), SLJIT_MOV_U16, rt, rs, simm); return EMIT_OK; /* LHU */
    case 0x28: emit_store(C, offsetof(CPUState, write_byte), rt, rs, simm); return EMIT_OK; /* SB */
    case 0x29: emit_store(C, offsetof(CPUState, write_half), rt, rs, simm); return EMIT_OK; /* SH */
    case 0x2B: emit_store(C, offsetof(CPUState, write_word), rt, rs, simm); return EMIT_OK; /* SW */
    case 0x22: case 0x26: case 0x2A: case 0x2E:   /* LWL/LWR/SWL/SWR */
        emit_helper2(C, SLJIT_HLP_MEMX, insn);
        return EMIT_OK;
    case 0x12:                                     /* COP2 (MFC2/CFC2/MTC2/CTC2/cmd) */
    case 0x32: case 0x3A:                          /* LWC2 / SWC2 */
        emit_helper2(C, SLJIT_HLP_COP2, insn);
        return EMIT_OK;
    default: return EMIT_ABORT;
    }
}

/* ---- control-flow classification (no emission) ------------------------- */
enum { CTRL_NONE = 0, CTRL_RETURN, CTRL_BRANCH, CTRL_JUMP, CTRL_CALL,
       CTRL_TAILJUMP, CTRL_ABORT };

/* Classify a possible control instruction. For CTRL_BRANCH (conditional) and
 * CTRL_JUMP (unconditional J), *out_tbyte is the target as a SIGNED byte offset
 * relative to the fragment entry. `off` is the instruction's fragment-relative
 * byte offset; `entry_phys` is the fragment's phys entry (for J's absolute
 * target). This slice handles `jr $ra` (return), PC-relative conditional
 * branches, and the absolute J (when it stays inside the fragment). JAL/JALR/
 * jr-non-ra/link-branches are CTRL_ABORT (decline the whole fragment). */
static int classify_control(uint32_t insn, uint32_t off, uint32_t entry_phys,
                            int32_t *out_tbyte) {
    uint32_t op = f_op(insn), fn = f_fn(insn), rs = f_rs(insn), rt = f_rt(insn);
    if (op == 0x00) {
        if (fn == 0x08) return (rs == 31) ? CTRL_RETURN : CTRL_TAILJUMP; /* JR $ra / jr rX */
        if (fn == 0x09) return CTRL_CALL;                               /* JALR */
        return CTRL_NONE;
    }
    if (op == 0x03) return CTRL_CALL;                                 /* JAL */
    if (op == 0x02) {                            /* J target (absolute) */
        /* In KSEG the region's high bits cancel under the phys mask, so the
         * fragment-relative byte offset is just target_phys - entry_phys. */
        uint32_t target_phys = (f_tgt26(insn) << 2) & 0x1FFFFFFFu;
        *out_tbyte = (int32_t)target_phys - (int32_t)entry_phys;
        return CTRL_JUMP;
    }
    if (op >= 0x04 && op <= 0x07) {              /* BEQ/BNE/BLEZ/BGTZ */
        *out_tbyte = (int32_t)off + 4 + (int32_t)(f_simm(insn) << 2);
        return CTRL_BRANCH;
    }
    if (op == 0x01) {                            /* REGIMM */
        if (rt == 0x00 || rt == 0x01 ||          /* BLTZ / BGEZ */
            rt == 0x10 || rt == 0x11) {          /* BLTZAL / BGEZAL (link) */
            *out_tbyte = (int32_t)off + 4 + (int32_t)(f_simm(insn) << 2);
            return CTRL_BRANCH;
        }
        return CTRL_ABORT;                        /* other REGIMM */
    }
    return CTRL_NONE;
}

/* Compute a branch's taken/not-taken predicate (1/0) into R_CTRL (S1), reading
 * the source registers at the BRANCH instruction (before its delay slot runs).
 * Reads go through the GPR cache; the predicate lands in S1, which is never a
 * cache slot, so it survives the delay slot + the pre-jump flush. */
static void emit_cond_to_S1(struct sljit_compiler *C, uint32_t insn, int ws_bltz) {
    uint32_t op = f_op(insn), rs = f_rs(insn), rt = f_rt(insn);
    sljit_s32 a, b; sljit_sw aw, bw;
    /* Classified LEFT-edge funnel bltz (auto_screen_x): predicate comes from
     * psx_ws_cull_bltz(gpr[rs]) — reject only past the revealed margin
     * (identity at 4:3). Helper reads only its arg, but the icall discipline
     * still flushes/resets the GPR cache via emit-helper conventions; here we
     * call through the cpu-relative table and land the verdict in S1. */
    if (ws_bltz && op == 0x01 && rt == 0x00) {
        gpr_src(C, rs, &a, &aw);
        sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, a, aw);
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0, SLJIT_MEM1(R_CPU),
                       (sljit_sw)(offsetof(CPUState, sljit_helpers)
                                  + (size_t)SLJIT_HLP_WS_CULL_BLTZ * sizeof(void *)));
        sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1(32, 32), SLJIT_R2, 0);
        sljit_emit_op1(C, SLJIT_MOV32, R_CTRL, 0, SLJIT_R0, 0);
        gpr_unpin();
        return;
    }
    switch (op) {
    case 0x04: /* BEQ rs==rt */
    case 0x05: /* BNE rs!=rt */
        gpr_src(C, rs, &a, &aw); gpr_src(C, rt, &b, &bw);
        if (a == SLJIT_IMM && b == SLJIT_IMM) as_reg(C, &a, &aw, SLJIT_R0);
        sljit_emit_op2u(C, SLJIT_SUB32 | SLJIT_SET_Z, a, aw, b, bw);
        sljit_emit_op_flags(C, SLJIT_MOV32, R_CTRL, 0,
                            (op == 0x04) ? SLJIT_EQUAL : SLJIT_NOT_EQUAL);
        break;
    case 0x06: /* BLEZ rs<=0 */
        gpr_src(C, rs, &a, &aw); as_reg(C, &a, &aw, SLJIT_R0);
        sljit_emit_op2u(C, SLJIT_SUB32 | SLJIT_SET_SIG_LESS_EQUAL, a, 0, SLJIT_IMM, 0);
        sljit_emit_op_flags(C, SLJIT_MOV32, R_CTRL, 0, SLJIT_SIG_LESS_EQUAL);
        break;
    case 0x07: /* BGTZ rs>0 */
        gpr_src(C, rs, &a, &aw); as_reg(C, &a, &aw, SLJIT_R0);
        sljit_emit_op2u(C, SLJIT_SUB32 | SLJIT_SET_SIG_GREATER, a, 0, SLJIT_IMM, 0);
        sljit_emit_op_flags(C, SLJIT_MOV32, R_CTRL, 0, SLJIT_SIG_GREATER);
        break;
    default: /* REGIMM: BLTZ/BLTZAL (rt 0x00/0x10) → rs<0 ; BGEZ/BGEZAL (0x01/0x11) → rs>=0 */
        gpr_src(C, rs, &a, &aw); as_reg(C, &a, &aw, SLJIT_R0);
        if (rt == 0x00 || rt == 0x10) {
            sljit_emit_op2u(C, SLJIT_SUB32 | SLJIT_SET_SIG_LESS, a, 0, SLJIT_IMM, 0);
            sljit_emit_op_flags(C, SLJIT_MOV32, R_CTRL, 0, SLJIT_SIG_LESS);
        } else {
            sljit_emit_op2u(C, SLJIT_SUB32 | SLJIT_SET_SIG_GREATER_EQUAL, a, 0, SLJIT_IMM, 0);
            sljit_emit_op_flags(C, SLJIT_MOV32, R_CTRL, 0, SLJIT_SIG_GREATER_EQUAL);
        }
        break;
    }
    gpr_unpin();
}

/* Read a little-endian word from the decode image at phys offset `off`. */
static inline uint32_t img_word(const uint8_t *b, uint32_t off) {
    return  (uint32_t)b[off]
         | ((uint32_t)b[off + 1] <<  8)
         | ((uint32_t)b[off + 2] << 16)
         | ((uint32_t)b[off + 3] << 24);
}

/* Bitmask of the distinct GPRs an instruction accesses THROUGH THE CACHE (i.e.
 * via gpr_src/gpr_dst), used to size the saved-register prologue so its push/pop
 * cost is proportional to a fragment's register pressure. MUST be a SUPERSET of
 * the cached regs each emit path touches (an omission would under-size the cache
 * and let gpr_alloc pick an unreserved saved reg). helper2 ops (COP2 / unaligned
 * mem) and J touch gpr only via memory in the helper, so they contribute 0. */
static uint32_t gpr_ref_mask(uint32_t insn) {
    uint32_t op = f_op(insn), fn = f_fn(insn);
    uint32_t m = 0;
    if (op == 0x00) {                       /* SPECIAL */
        if (fn == 0x08)                     /* JR rs (incl. jr $ra) */
            m = (1u << f_rs(insn));
        else if (fn == 0x09)                /* JALR rs, link = rd ? rd : 31 */
            m = (1u << f_rs(insn)) | (1u << (f_rd(insn) ? f_rd(insn) : 31u));
        else                                /* ALU / shift / mult / div / mf / mt */
            m = (1u << f_rs(insn)) | (1u << f_rt(insn)) | (1u << f_rd(insn));
    } else if (op == 0x01) {                /* REGIMM branch (+ link reg for *AL) */
        uint32_t rt = f_rt(insn);
        m = (1u << f_rs(insn));
        if (rt == 0x10u || rt == 0x11u) m |= (1u << 31);
    } else if (op == 0x03) {                /* JAL: link $ra */
        m = (1u << 31);
    } else if (op >= 0x04 && op <= 0x07) {  /* BEQ/BNE/BLEZ/BGTZ */
        m = (1u << f_rs(insn)) | (1u << f_rt(insn));
    } else if (op >= 0x08 && op <= 0x0E) {  /* ADDI..XORI, SLTI(U) */
        m = (1u << f_rs(insn)) | (1u << f_rt(insn));
    } else if (op == 0x0F) {                /* LUI */
        m = (1u << f_rt(insn));
    } else if ((op >= 0x20 && op <= 0x25) || op == 0x28 || op == 0x29 || op == 0x2B) {
        m = (1u << f_rs(insn)) | (1u << f_rt(insn));   /* loads / stores */
    }
    return m & ~1u;                         /* $0 is never cached */
}

void overlay_sljit_try_compile(uint32_t entry,
                               const uint8_t *bytes, uint32_t size,
                               uint32_t image_base_vram,
                               OverlaySljitResult *out) {
    out->fn = NULL; out->code_lo = 0; out->code_len = 0; out->insns = 0;
    out->serialized = NULL; out->serialized_size = 0;
    if (!bytes) { s_declines++; return; }

    uint32_t entry_phys = entry & 0x1FFFFFFFu;
    uint32_t base_phys  = image_base_vram & 0x1FFFFFFFu;
    if (entry_phys < base_phys) { s_declines++; return; }
    uint32_t off0 = entry_phys - base_phys;

    /* ---- PASS 1: find the terminator, collect branch targets + delay-slot
     * offsets. Linear scan in memory order (branches don't break the layout);
     * the fragment is [entry, jr $ra + its delay slot). ----------------------*/
    static uint8_t is_target[SLJIT_MAX_FRAG_INSNS]; /* index by word            */
    static uint8_t is_ds[SLJIT_MAX_FRAG_INSNS];     /* word is a delay slot      */
    /* (static: avoids ~20 KB of stack; the emitter is single-threaded — JIT is
     * synchronous on the emu thread.) */
    struct { uint32_t bw; int32_t tbyte; } brs[SLJIT_MAX_FRAG_CTRL];
    int nbr = 0;
    int ncalls = 0;
    uint32_t frag_words = 0;
    int found_term = 0;

    memset(is_target, 0, sizeof is_target);
    memset(is_ds, 0, sizeof is_ds);

    for (uint32_t i = 0; i < SLJIT_MAX_FRAG_INSNS; i++) {
        uint32_t off = off0 + i * 4u;
        if (off + 4u > size) break;                 /* off image w/o terminator */
        uint32_t insn = img_word(bytes, off);
        int32_t tbyte = 0;
        int ctrl = classify_control(insn, i * 4u, entry_phys, &tbyte);
        if (ctrl == CTRL_ABORT) { s_declines++; return; }
        if (ctrl == CTRL_RETURN) {
            if (i + 1u < SLJIT_MAX_FRAG_INSNS) is_ds[i + 1u] = 1;
            frag_words  = i + 2u;                    /* jr + delay slot */
            found_term  = 1;
            break;
        }
        if (ctrl == CTRL_BRANCH || ctrl == CTRL_JUMP) {
            if (i + 1u < SLJIT_MAX_FRAG_INSNS) is_ds[i + 1u] = 1;
            if (nbr >= (int)SLJIT_MAX_FRAG_CTRL) { s_declines++; return; }
            brs[nbr].bw = i; brs[nbr].tbyte = tbyte; nbr++;
        }
        if (ctrl == CTRL_CALL) {           /* jal/jalr: delay slot, no fragment target */
            if (i + 1u < SLJIT_MAX_FRAG_INSNS) is_ds[i + 1u] = 1;
            ncalls++;
        }
        if (ctrl == CTRL_TAILJUMP) {       /* jr rX (computed): interior transfer */
            if (i + 1u < SLJIT_MAX_FRAG_INSNS) is_ds[i + 1u] = 1;
        }
        /* CTRL_NONE: straight-line, continue. */
    }
    if (!found_term || frag_words == 0 || frag_words > SLJIT_MAX_FRAG_INSNS) {
        s_declines++; return;
    }

    /* Explicit address-listed cull hooks carry a live aspect term. Generated
     * overlay DLLs and the interpreter implement it directly; keep these rare
     * fragments out of the generic JIT so a vanilla immediate is never cached. */
    for (uint32_t i = 0; i < frag_words; i++) {
        uint32_t pc = entry + i * 4u;
        if (psx_ws_is_cull_bias_site(pc) || psx_ws_is_cull_slti_site(pc)) {
            s_declines++;
            return;
        }
    }
    if (off0 + frag_words * 4u > size) { s_declines++; return; }  /* delay slot off image */

    /* Validate branch targets: in-range, aligned, not landing on a delay slot. */
    for (int b = 0; b < nbr; b++) {
        int32_t tb = brs[b].tbyte;
        if (tb < 0 || (tb & 3) != 0) { s_declines++; return; }
        uint32_t tw = (uint32_t)tb / 4u;
        if (tw >= frag_words || is_ds[tw]) { s_declines++; return; }
        is_target[tw] = 1;
    }

    /* Size the GPR register cache to this fragment's register pressure so the
     * prologue only saves the saved-regs it will use (push/pop cost ∝ pressure;
     * see gpr_ref_mask). distinct >= the max regs in any single instruction, so
     * gpr_alloc always finds a slot. */
    uint32_t used_mask = 0;
    for (uint32_t i = 0; i < frag_words; i++)
        used_mask |= gpr_ref_mask(img_word(bytes, off0 + i * 4u));

    /* Widescreen cull + backdrop-preload detection ([widescreen.cull]
     * auto_screen_x / auto_backdrop): both run over the same fragment word
     * image via the shared detectors (ws_cull_detect.h / ws_backdrop_detect.h)
     * so the sljit JIT derives the SAME sites as the gcc emit + interp. Both
     * are gated on the per-game opt-in (a title that never opted in must never
     * have its code pattern-scanned and rewritten). A window whose context
     * falls outside this fragment is simply not detected here (the interp/gcc
     * paths cover the rest) — worst case a missed widen, never a mis-compile. */
    for (uint32_t i = 0; i < frag_words && i < SLJIT_MAX_FRAG_INSNS; i++) {
        s_bd_kind[i] = WS_BD_NONE; s_bd_cols[i] = 0; s_ws_bltz[i] = 0;
    }
    s_frag_ws_cull = 0;
    {
        static uint32_t bdwords[SLJIT_MAX_FRAG_INSNS];
        for (uint32_t i = 0; i < frag_words; i++) bdwords[i] = img_word(bytes, off0 + i * 4u);
        if (psx_ws_auto_cull_on()) {
            s_frag_ws_cull = psx_ws_func_has_screen_cull(bdwords, (int)frag_words);
            if (s_frag_ws_cull)
                for (uint32_t i = 0; i < frag_words && i < SLJIT_MAX_FRAG_INSNS; i++)
                    s_ws_bltz[i] = (uint8_t)psx_ws_cull_bltz_at(bdwords, (int)frag_words, (int)i);
        }
        WsBackdropSite bds[16];
        int nb = psx_ws_find_backdrop_windows(bdwords, (int)frag_words, entry, bds, 16);
        for (int b = 0; b < nb; b++) {
            uint32_t idx = (bds[b].pc - entry) / 4u;
            if (idx < frag_words) {
                s_bd_kind[idx] = (uint8_t)bds[b].kind;
                s_bd_cols[idx] = (int16_t)bds[b].window_cols;
            }
        }
    }

    int distinct = 0;
    for (uint32_t m = used_mask; m; m &= m - 1u) distinct++;
    s_gpr_slots = (distinct < GPR_SLOTS_MAX) ? distinct : GPR_SLOTS_MAX;
    s_gpr_lru = 0;

    /* ---- PASS 2: emit ----------------------------------------------------- */
    struct sljit_compiler *C = sljit_create_compiler(NULL);
    if (!C) { s_declines++; sljit_log("compile: create_compiler failed"); return; }
    /* void shard(CPUState* cpu): S0=cpu (R_CPU), S1=control-transfer temp
     * (R_CTRL: branch predicate / jalr|jr-rX target), S2..S{1+s_gpr_slots}=GPR
     * register cache. Scratches R0..R4 (operands, fn-ptr, addr/value, the 4 call
     * args + the call-helper fn-ptr). */
    sljit_emit_enter(C, 0, SLJIT_ARGS1V(P), 5, 2 + s_gpr_slots, 0);
    gpr_reset();

    static struct sljit_label *labels[SLJIT_MAX_FRAG_INSNS];
    struct { struct sljit_jump *j; uint32_t tw; } jmps[SLJIT_MAX_FRAG_CTRL];
    int njmp = 0;
    for (uint32_t i = 0; i < frag_words; i++) labels[i] = NULL;

    int aborted = 0;
    enum { PEND_NONE = 0, PEND_RET, PEND_BR, PEND_CALL, PEND_TAIL } pending = PEND_NONE;
    uint32_t pend_tw = 0;
    int pend_cond = 0;          /* PEND_BR: 1 = conditional (S1), 0 = unconditional */
    uint32_t pend_call_target = 0;  /* PEND_CALL: jal absolute target           */
    uint32_t pend_call_return = 0;  /* return address (continuation)            */
    int      pend_call_dynamic = 0; /* 1 = jalr (target in S1), 0 = jal (const) */
    int      pend_call_check = 0;   /* apply the (ra,sp) contract after the call*/

    for (uint32_t i = 0; i < frag_words; i++) {
        if (is_target[i]) {
            /* Branch-target = a join. Flush the fall-through edge's dirty regs to
             * memory (these stores precede the label, so a branch INTO it skips
             * them — every incoming edge already flushed), then reset the cache so
             * post-label code reads from memory (the one source all edges agree
             * on). */
            gpr_flush(C);
            gpr_reset();
            labels[i] = sljit_emit_label(C);
        }
        uint32_t insn = img_word(bytes, off0 + i * 4u);

        if (pending == PEND_NONE) {
            int32_t tbyte = 0;
            int ctrl = classify_control(insn, i * 4u, entry_phys, &tbyte);
            if (ctrl == CTRL_ABORT) { aborted = 1; break; }
            if (ctrl == CTRL_RETURN) { pending = PEND_RET; continue; }
            if (ctrl == CTRL_BRANCH || ctrl == CTRL_JUMP) {
                if (ctrl == CTRL_BRANCH) {
                    /* BLTZAL/BGEZAL link $ra = pc+8 (unconditionally, before the
                     * delay slot), then branch on the predicate. */
                    if (f_op(insn) == 0x01) {
                        uint32_t rtf = f_rt(insn);
                        if (rtf == 0x10 || rtf == 0x11)
                            sljit_emit_op1(C, SLJIT_MOV32, gpr_dst(C, 31), 0,
                                           SLJIT_IMM, (sljit_sw)(entry + i * 4u + 8u));
                    }
                    emit_cond_to_S1(C, insn,   /* predicate read BEFORE the delay slot */
                                    s_frag_ws_cull && i < SLJIT_MAX_FRAG_INSNS && s_ws_bltz[i]);
                }
                pending   = PEND_BR;
                pend_cond = (ctrl == CTRL_BRANCH);
                pend_tw   = (uint32_t)((int32_t)tbyte / 4);
                continue;
            }
            if (ctrl == CTRL_TAILJUMP) {       /* jr rX (computed): capture target */
                sljit_s32 a; sljit_sw aw;
                gpr_src(C, f_rs(insn), &a, &aw);   /* read BEFORE the delay slot */
                sljit_emit_op1(C, SLJIT_MOV32, R_CTRL, 0, a, aw);
                gpr_unpin();
                pending = PEND_TAIL;
                continue;
            }
            if (ctrl == CTRL_CALL) {
                /* Link write + (jalr) target capture happen BEFORE the delay slot
                 * (the delay slot may clobber rs); the dispatch happens AFTER it. */
                uint32_t cop = f_op(insn), crs = f_rs(insn), crd = f_rd(insn);
                uint32_t pc_virt = entry + i * 4u;
                pend_call_return = pc_virt + 8u;
                if (cop == 0x03) {                 /* JAL: link $ra, absolute target */
                    pend_call_dynamic = 0;
                    pend_call_target  = ((pc_virt + 4u) & 0xF0000000u) | (f_tgt26(insn) << 2);
                    pend_call_check   = 1;
                    sljit_emit_op1(C, SLJIT_MOV32, gpr_dst(C, 31), 0,
                                   SLJIT_IMM, (sljit_sw)pend_call_return);
                } else {                           /* JALR rd, rs: dynamic target */
                    uint32_t link = crd ? crd : 31u;
                    sljit_s32 a; sljit_sw aw;
                    pend_call_dynamic = 1;
                    pend_call_check   = (crd == 0 || crd == 31);
                    gpr_src(C, crs, &a, &aw);      /* capture target before delay slot */
                    sljit_emit_op1(C, SLJIT_MOV32, R_CTRL, 0, a, aw);
                    sljit_emit_op1(C, SLJIT_MOV32, gpr_dst(C, link), 0,
                                   SLJIT_IMM, (sljit_sw)pend_call_return);
                    gpr_unpin();
                }
                pending = PEND_CALL;
                continue;
            }
            if (emit_one(C, entry + i * 4u, insn, s_bd_kind[i], s_bd_cols[i]) != EMIT_OK) { aborted = 1; break; }
        } else {
            /* This instruction is the delay slot of the pending control insn;
             * it must be a plain, supported, non-control op (the constraint the
             * interpreter's exec_delay_slot enforces). It executes regardless of
             * branch outcome, so emit it, flush the cache to memory (the transfer
             * makes memory authoritative for the target/callee/return), THEN apply
             * the pending transfer. */
            int32_t dummy = 0;
            if (classify_control(insn, i * 4u, entry_phys, &dummy) != CTRL_NONE) { aborted = 1; break; }
            if (emit_one(C, entry + i * 4u, insn, s_bd_kind[i], s_bd_cols[i]) != EMIT_OK) { aborted = 1; break; }
            gpr_flush(C);
            if (pending == PEND_RET) {
                if (g_psx_cps_mode) {
                    /* CPS: publish $ra (jr $ra) so the flat trampoline dispatches
                     * the caller's continuation. gpr[] already flushed to memory
                     * by gpr_flush() above. */
                    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_MEM1(R_CPU),
                                   (sljit_sw)(offsetof(CPUState, gpr) + 31 * sizeof(uint32_t)));
                    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU),
                                   (sljit_sw)offsetof(CPUState, pc), SLJIT_R0, 0);
                }
                sljit_emit_return_void(C);
                gpr_reset();
            } else if (pending == PEND_TAIL) {
                /* jr rX: transfer to the computed target (cpu->pc = R_CTRL),
                 * return; the dispatch loop re-dispatches there (matches interp). */
                sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU),
                               (sljit_sw)offsetof(CPUState, pc), R_CTRL, 0);
                sljit_emit_return_void(C);
                gpr_reset();
            } else if (pending == PEND_BR) {
                struct sljit_jump *j = pend_cond
                    ? sljit_emit_cmp(C, SLJIT_NOT_EQUAL, R_CTRL, 0, SLJIT_IMM, 0)
                    : sljit_emit_jump(C, SLJIT_JUMP);
                if (njmp >= (int)SLJIT_MAX_FRAG_CTRL) { aborted = 1; break; }
                jmps[njmp].j = j; jmps[njmp].tw = pend_tw; njmp++;
                /* Conditional: the not-taken fall-through keeps the just-flushed
                 * (now-clean) cache. Unconditional J: nothing falls through, so
                 * the following code is dead or label-guarded — reset. */
                if (!pend_cond) gpr_reset();
            } else { /* PEND_CALL: psx_sljit_call(cpu, target, return_pc, check) */
                if (g_psx_cps_mode) {
                    /* CPS: a call's continuation (return_pc, inside this fragment)
                     * would need a re-entry entry-switch this JIT doesn't emit;
                     * and psx_sljit_call uses the legacy nested contract. Decline
                     * the whole fragment — it falls to the interpreter, which
                     * handles the call CPS-safely (depth-1). Leaf fragments (no
                     * calls) still JIT. */
                    aborted = 1; break;
                }
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, R_CPU, 0);           /* cpu */
                if (pend_call_dynamic)
                    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R1, 0, R_CTRL, 0);    /* jalr target */
                else
                    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)pend_call_target);
                sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, (sljit_sw)pend_call_return);
                sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R3, 0, SLJIT_IMM, (sljit_sw)pend_call_check);
                sljit_emit_op1(C, SLJIT_MOV,   SLJIT_R4, 0, SLJIT_MEM1(R_CPU),
                               (sljit_sw)(offsetof(CPUState, sljit_helpers)
                                          + (size_t)SLJIT_HLP_CALL * sizeof(void *)));
                sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS4(32, P, 32, 32, 32), SLJIT_R4, 0);
                /* helper returned nonzero ⇒ transfer/bail in progress: return now,
                 * propagating cpu->pc / g_psx_call_bail to the dispatch loop. */
                struct sljit_jump *cont =
                    sljit_emit_cmp(C, SLJIT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
                sljit_emit_return_void(C);
                sljit_set_label(cont, sljit_emit_label(C));
                gpr_reset();   /* callee wrote cpu->gpr[] in memory */
            }
            pending = PEND_NONE;
        }
    }
    if (!aborted && pending != PEND_NONE) aborted = 1;  /* control w/o delay slot */
    if (aborted) { sljit_free_compiler(C); s_declines++; return; }

    /* Bind each branch jump to its target label. */
    for (int k = 0; k < njmp; k++) {
        if (jmps[k].tw >= frag_words || !labels[jmps[k].tw]) {
            sljit_free_compiler(C); s_declines++; return;   /* defensive */
        }
        sljit_set_label(jmps[k].j, labels[jmps[k].tw]);
    }

    /* Serialize the (position-independent) LIR for the persisted cache BEFORE
     * generate_code: generate_code finalizes the compiler (sets compiler->error =
     * SLJIT_ERR_COMPILED), and sljit_serialize_compiler bails on any error, so it
     * MUST run first. It doesn't modify the LIR, so code generation continues
     * normally afterward. A serialize failure just means this shard isn't
     * persisted; in-session use is unaffected. */
    {
        /* options = 0 (NOT SLJIT_SERIALIZE_IGNORE_DEBUG): when this build has
         * sljit argument-checks/debug enabled, a buffer serialized WITHOUT debug
         * info cannot be deserialized. With checks off, no debug info exists and
         * 0 behaves identically — so 0 is correct for any build config. */
        sljit_uw ssz = 0;
        sljit_uw *sbuf = sljit_serialize_compiler(C, 0, &ssz);
        out->serialized      = (void *)sbuf;
        out->serialized_size = sbuf ? (unsigned long)ssz : 0;
    }
    void *code = sljit_generate_code(C, 0, NULL);
    sljit_uw csz = sljit_get_generated_code_size(C);
    sljit_free_compiler(C);
    if (!code) {
        if (out->serialized) {
            overlay_sljit_free_serialized(out->serialized);
            out->serialized = NULL; out->serialized_size = 0;
        }
        s_declines++; sljit_log("compile: generate_code failed @0x%08X", entry); return;
    }

    out->fn       = (OverlaySljitFn)code;
    out->code_lo  = entry_phys;
    out->code_len = frag_words * 4u;
    out->insns    = frag_words;
    s_compiles++;
    s_bytes += (uint64_t)csz;
    sljit_log("compile ok @0x%08X: %u insns (%d br, %d call) -> %lu bytes host",
              entry, frag_words, nbr, ncalls, (unsigned long)csz);
}

/* Reload a serialized shard (persisted cache, Stage 2): deserialize the LIR and
 * regenerate host code for THIS process. The LIR is position-independent (Stage
 * 1 routes helper calls through the cpu-relative table), so the regenerated code
 * is correct at this process's base. Returns NULL on any failure (caller falls
 * back to JIT/interp). */
OverlaySljitFn overlay_sljit_deserialize(const void *blob, unsigned long blob_size) {
    if (!blob || blob_size == 0) return NULL;
    struct sljit_compiler *C =
        sljit_deserialize_compiler((sljit_uw *)blob, (sljit_uw)blob_size, 0, NULL);
    if (!C) { sljit_log("deserialize: failed"); return NULL; }
    void *code = sljit_generate_code(C, 0, NULL);
    sljit_uw csz = sljit_get_generated_code_size(C);
    sljit_free_compiler(C);
    if (!code) { sljit_log("deserialize: generate_code failed"); return NULL; }
    s_bytes += (uint64_t)csz;
    return (OverlaySljitFn)code;
}

/* Free a serialize buffer (sljit default allocator == malloc). */
void overlay_sljit_free_serialized(void *p) { if (p) free(p); }

void overlay_sljit_get_status(int *available, int *selftest_ok,
                              uint64_t *compiles, uint64_t *declines,
                              uint64_t *bytes_emitted) {
    if (available)     *available     = overlay_sljit_available();
    if (selftest_ok)   *selftest_ok   = s_selftest_ok;
    if (compiles)      *compiles      = s_compiles;
    if (declines)      *declines      = s_declines;
    if (bytes_emitted) *bytes_emitted = s_bytes;
}
