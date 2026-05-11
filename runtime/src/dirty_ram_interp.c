/* dirty_ram_interp.c — small MIPS interpreter for install-at-runtime RAM.
 *
 * See CLAUDE.md Rule 18, docs/dynamic_handler_install.md, and the inline
 * note in memory.c (search for "Option B") for the architectural rationale.
 *
 * Scope: only fires when psx_dispatch lands on a PC whose page has been
 * written-to since boot.  Runs one basic block (terminator: jr/jalr/j/jal
 * or branch) and returns; dispatch trampoline re-enters for the next block.
 *
 * Strict policy: any opcode not implemented here aborts fatally.  This
 * surfaces unknown install patterns immediately so we expand the support
 * set deliberately, never silently.
 *
 * Future option (Option B, see docs/dynamic_handler_install.md): JIT-compile
 * dirty pages via the existing StrictTranslator instead of interpreting.
 * Pros: single source of MIPS semantics shared with the build-time path,
 * native-speed install stubs, generalizes to game JIT cases.  Cons: gcc-at-
 * runtime build dep, ~200 ms compile latency stall on first dispatch, file
 * I/O on hot path, cache-invalidation complexity, Windows MinGW + dlopen
 * friction.  Today install stubs are cold-path glue (~4k instructions per
 * directory-load); interpretation is sub-microsecond and the right fit.
 * Revisit if measurement shows install-stub instructions becoming a
 * meaningful fraction of total runtime work.
 */

#include "dirty_ram_interp.h"
#include "cpu_state.h"
#include "debug_server.h"
#include "interrupts.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

uint64_t g_dirty_ram_blocks_run = 0;
uint64_t g_dirty_ram_insns_run  = 0;
uint64_t g_dirty_ram_aborts     = 0;

/* Mid-block unsupported-opcode counters. Bumped instead of fprintf-spamming
 * stderr (CLAUDE.md §3). Read via dirty_ram_get_unsupported(). The "last_*"
 * fields capture the most recent occurrence so a TCP query can see what
 * opcode is missing without needing log scraping. */
uint64_t g_dirty_ram_unsupported_midblock = 0;
uint32_t g_dirty_ram_last_unsupported_pc  = 0;
uint32_t g_dirty_ram_last_unsupported_insn = 0;
const char *g_dirty_ram_last_unsupported_reason = NULL;

DirtyRamPcEntry g_dirty_ram_pc_table[DIRTY_RAM_PC_TABLE_SIZE] = {0};

DirtyRamBlockLogEntry g_dirty_ram_block_log[DIRTY_RAM_BLOCK_LOG_CAP] = {0};
uint64_t              g_dirty_ram_block_log_seq = 0;

/* Current frame counter, defined in debug_server.c. */
extern uint64_t s_frame_count;

/* Linear-probed insert/lookup keyed on entry PC.  Table is small (64) and
 * the working set of install-stub PCs is tiny (handful), so this stays
 * O(1) in practice.  Returns NULL if the table is full — caller treats
 * that as "stop tracking" rather than failing. */
static DirtyRamPcEntry *pc_table_get_or_insert(uint32_t pc) {
    uint32_t h = (pc * 2654435761u) & (DIRTY_RAM_PC_TABLE_SIZE - 1);
    for (uint32_t i = 0; i < DIRTY_RAM_PC_TABLE_SIZE; i++) {
        uint32_t idx = (h + i) & (DIRTY_RAM_PC_TABLE_SIZE - 1);
        DirtyRamPcEntry *e = &g_dirty_ram_pc_table[idx];
        if (e->pc == pc) return e;
        if (e->pc == 0) { e->pc = pc; return e; }
    }
    return NULL;
}

/* From debug_server.c — keep our outer-frame attribution coherent. */
extern uint32_t g_debug_current_func_addr;
extern uint32_t g_debug_last_store_pc;

#ifdef PSX_HAS_GAME_DISPATCH
extern int psx_dispatch_game_compiled(CPUState* cpu, uint32_t addr);
#endif

/* Forward decls from memory.c — used to read instruction bytes. */
extern uint8_t *memory_get_ram_ptr(void);

/* MIPS instruction field decoders. */
static inline uint32_t op_field    (uint32_t i) { return (i >> 26) & 0x3Fu; }
static inline uint32_t rs_field    (uint32_t i) { return (i >> 21) & 0x1Fu; }
static inline uint32_t rt_field    (uint32_t i) { return (i >> 16) & 0x1Fu; }
static inline uint32_t rd_field    (uint32_t i) { return (i >> 11) & 0x1Fu; }
static inline uint32_t shamt_field (uint32_t i) { return (i >>  6) & 0x1Fu; }
static inline uint32_t funct_field (uint32_t i) { return  i        & 0x3Fu; }
static inline uint32_t imm16_field (uint32_t i) { return  i        & 0xFFFFu; }
static inline int32_t  simm16_field(uint32_t i) { return (int32_t)(int16_t)imm16_field(i); }
static inline uint32_t target26    (uint32_t i) { return  i        & 0x03FFFFFFu; }

/* Read a 32-bit instruction word from kernel RAM at the given physical addr.
 * Caller has already verified the address is in dirty kernel RAM. */
static inline uint32_t fetch_word(uint32_t phys) {
    const uint8_t *ram = memory_get_ram_ptr();
    return  (uint32_t)ram[phys]
         | ((uint32_t)ram[phys + 1] <<  8)
         | ((uint32_t)ram[phys + 2] << 16)
         | ((uint32_t)ram[phys + 3] << 24);
}

static inline uint32_t interp_lwl(CPUState *cpu, uint32_t addr, uint32_t rt_value) {
    uint32_t word = cpu->read_word(addr & ~3u);
    switch (addr & 3u) {
        case 0: return (rt_value & 0x00FFFFFFu) | (word << 24);
        case 1: return (rt_value & 0x0000FFFFu) | (word << 16);
        case 2: return (rt_value & 0x000000FFu) | (word << 8);
        default: return word;
    }
}

static inline uint32_t interp_lwr(CPUState *cpu, uint32_t addr, uint32_t rt_value) {
    uint32_t word = cpu->read_word(addr & ~3u);
    switch (addr & 3u) {
        case 0: return word;
        case 1: return (rt_value & 0xFF000000u) | (word >> 8);
        case 2: return (rt_value & 0xFFFF0000u) | (word >> 16);
        default: return (rt_value & 0xFFFFFF00u) | (word >> 24);
    }
}

static inline void interp_swl(CPUState *cpu, uint32_t addr, uint32_t value) {
    uint32_t aligned = addr & ~3u;
    uint32_t word = cpu->read_word(aligned);
    switch (addr & 3u) {
        case 0: word = (word & 0xFFFFFF00u) | (value >> 24); break;
        case 1: word = (word & 0xFFFF0000u) | (value >> 16); break;
        case 2: word = (word & 0xFF000000u) | (value >> 8); break;
        default: word = value; break;
    }
    cpu->write_word(aligned, word);
}

static inline void interp_swr(CPUState *cpu, uint32_t addr, uint32_t value) {
    uint32_t aligned = addr & ~3u;
    uint32_t word = cpu->read_word(aligned);
    switch (addr & 3u) {
        case 0: word = value; break;
        case 1: word = (word & 0x000000FFu) | (value << 8); break;
        case 2: word = (word & 0x0000FFFFu) | (value << 16); break;
        default: word = (word & 0x00FFFFFFu) | (value << 24); break;
    }
    cpu->write_word(aligned, word);
}

/* Soft-fail thread-local flag.  When the interpreter encounters an opcode
 * it doesn't implement, it sets this flag and returns instead of aborting,
 * letting the caller (psx_dispatch via dirty_ram_dispatch) fall back to
 * psx_unknown_dispatch — which has its own ad-hoc resolver for known
 * trampoline patterns (jr-based vector dispatch, etc.).
 *
 * This is a deliberate retreat from "always pick the most complete option"
 * for ONE narrow case: dispatch into pages that have been written-to but
 * don't actually contain valid stub code at the dispatched PC (e.g.
 * stale data, return-target addresses that point to non-code areas).  The
 * pre-existing psx_unknown_dispatch already handles those — we just need
 * to let it.  If a true install stub uses an opcode we don't have, this
 * will silently route it to psx_unknown_dispatch, which will likely return
 * a no-op cpu->pc=0.  When that happens, we'll see "card protocol stalls"
 * in measurement and add the missing opcode here. */
static int g_unsupported_seen = 0;
static uint32_t g_unsupported_pc = 0;
static uint32_t g_unsupported_insn = 0;
static const char *g_unsupported_reason = NULL;

static int abort_unsupported(uint32_t pc, uint32_t insn, const char *reason) {
    g_dirty_ram_aborts++;
    g_unsupported_seen   = 1;
    g_unsupported_pc     = pc;
    g_unsupported_insn   = insn;
    g_unsupported_reason = reason;
    return 1; /* signal "control transferred" so the caller stops */
}

/* Execute ONE instruction at *pc on the given CPU state.  Returns:
 *   0 = continue (advance pc by 4)
 *   1 = control transferred OR unsupported opcode (caller checks
 *       g_unsupported_seen to distinguish).
 * Branches encode their delay slot themselves before returning 1. */
static int exec_one(CPUState *cpu, uint32_t pc, uint32_t *next_pc_out);

/* Forward: helper for delay-slot execution on jumps/branches. */
static void exec_delay_slot(CPUState *cpu, uint32_t pc) {
    /* Delay-slot instruction at pc must NOT be a control transfer.
     * Recursively interpret as a single non-branching instruction. */
    uint32_t ds_phys = pc & 0x1FFFFFFFu;
    uint32_t insn = fetch_word(ds_phys);
    uint32_t opc = op_field(insn);
    uint32_t fnt = funct_field(insn);
    /* Reject branches/jumps in delay slots — undefined on R3000A and our
     * static recompiler explicitly handles this case differently (the
     * fall-through fix from 2026-04-21).  In install stubs, delay slots
     * are always nop or simple arithmetic. */
    if (opc == 0x02 /*j*/ || opc == 0x03 /*jal*/ ||
        opc == 0x04 /*beq*/ || opc == 0x05 /*bne*/ ||
        opc == 0x06 /*blez*/ || opc == 0x07 /*bgtz*/ ||
        opc == 0x01 /*regimm*/ ||
        (opc == 0x00 && (fnt == 0x08 /*jr*/ || fnt == 0x09 /*jalr*/))) {
        (void)abort_unsupported(pc, insn, "control-transfer in delay slot");
        return;
    }
    uint32_t dummy_next = 0;
    (void)exec_one(cpu, pc, &dummy_next);
    g_dirty_ram_insns_run++;
}

static int exec_one(CPUState *cpu, uint32_t pc, uint32_t *next_pc_out) {
    uint32_t phys = pc & 0x1FFFFFFFu;
    uint32_t insn = fetch_word(phys);
    uint32_t opc  = op_field(insn);
    uint32_t rs   = rs_field(insn);
    uint32_t rt   = rt_field(insn);
    uint32_t rd   = rd_field(insn);
    uint32_t sh   = shamt_field(insn);
    uint32_t fnt  = funct_field(insn);
    int32_t  simm = simm16_field(insn);
    uint32_t imm  = imm16_field(insn);

    *next_pc_out = pc + 4;

    /* Update last-store PC tracker so SIO PC tracer attribution stays
     * coherent through interpreted stubs. */
    g_debug_last_store_pc = pc;

    switch (opc) {
    case 0x00: /* SPECIAL */
        switch (fnt) {
        case 0x00: /* SLL rd, rt, sh (also nop when all fields are 0) */
            cpu->gpr[rd] = cpu->gpr[rt] << sh;
            cpu->gpr[0] = 0;
            return 0;
        case 0x02: /* SRL */
            cpu->gpr[rd] = cpu->gpr[rt] >> sh;
            cpu->gpr[0] = 0;
            return 0;
        case 0x03: /* SRA */
            cpu->gpr[rd] = (uint32_t)((int32_t)cpu->gpr[rt] >> sh);
            cpu->gpr[0] = 0;
            return 0;
        case 0x04: /* SLLV */
            cpu->gpr[rd] = cpu->gpr[rt] << (cpu->gpr[rs] & 31);
            cpu->gpr[0] = 0;
            return 0;
        case 0x06: /* SRLV */
            cpu->gpr[rd] = cpu->gpr[rt] >> (cpu->gpr[rs] & 31);
            cpu->gpr[0] = 0;
            return 0;
        case 0x07: /* SRAV */
            cpu->gpr[rd] = (uint32_t)((int32_t)cpu->gpr[rt] >> (cpu->gpr[rs] & 31));
            cpu->gpr[0] = 0;
            return 0;
        case 0x08: { /* JR rs */
            uint32_t target = cpu->gpr[rs];
            exec_delay_slot(cpu, pc + 4);
            cpu->pc = target;
            return 1;
        }
        case 0x09: { /* JALR rd, rs */
            uint32_t target = cpu->gpr[rs];
            uint32_t return_pc = pc + 8;
            cpu->gpr[rd ? rd : 31] = return_pc;
            cpu->gpr[0] = 0;
            exec_delay_slot(cpu, pc + 4);
#ifdef PSX_HAS_GAME_DISPATCH
            cpu->pc = 0;
            if (psx_dispatch_game_compiled(cpu, target)) {
                if (cpu->pc != 0) return 1;
                *next_pc_out = return_pc;
                return 0;
            }
#endif
            cpu->pc = target;
            return 1;
        }
        case 0x0C: /* SYSCALL */
            cpu->pc = pc;
            psx_syscall(cpu, (insn >> 6) & 0xFFFFFu);
            return (cpu->pc != 0);
        case 0x0D: /* BREAK */
            psx_break(cpu, (insn >> 6) & 0xFFFFFu, pc);
            return 1;
        case 0x0F: /* SYNC */
            return 0;
        case 0x10: /* MFHI */
            cpu->gpr[rd] = cpu->hi;
            cpu->gpr[0] = 0;
            return 0;
        case 0x11: /* MTHI */
            cpu->hi = cpu->gpr[rs];
            return 0;
        case 0x12: /* MFLO */
            cpu->gpr[rd] = cpu->lo;
            cpu->gpr[0] = 0;
            return 0;
        case 0x13: /* MTLO */
            cpu->lo = cpu->gpr[rs];
            return 0;
        case 0x18: { /* MULT */
            int64_t r = (int64_t)(int32_t)cpu->gpr[rs] * (int64_t)(int32_t)cpu->gpr[rt];
            cpu->lo = (uint32_t)r;
            cpu->hi = (uint32_t)((uint64_t)r >> 32);
            return 0;
        }
        case 0x19: { /* MULTU */
            uint64_t r = (uint64_t)cpu->gpr[rs] * (uint64_t)cpu->gpr[rt];
            cpu->lo = (uint32_t)r;
            cpu->hi = (uint32_t)(r >> 32);
            return 0;
        }
        case 0x1A: { /* DIV */
            int32_t a = (int32_t)cpu->gpr[rs];
            int32_t b = (int32_t)cpu->gpr[rt];
            if (b == 0) {
                cpu->lo = (a < 0) ? 1u : 0xFFFFFFFFu;
                cpu->hi = (uint32_t)a;
            } else if ((uint32_t)a == 0x80000000u && b == -1) {
                cpu->lo = 0x80000000u;
                cpu->hi = 0;
            } else {
                cpu->lo = (uint32_t)(a / b);
                cpu->hi = (uint32_t)(a % b);
            }
            return 0;
        }
        case 0x1B: /* DIVU */
            if (cpu->gpr[rt] == 0) {
                cpu->lo = 0xFFFFFFFFu;
                cpu->hi = cpu->gpr[rs];
            } else {
                cpu->lo = cpu->gpr[rs] / cpu->gpr[rt];
                cpu->hi = cpu->gpr[rs] % cpu->gpr[rt];
            }
            return 0;
        case 0x20: /* ADD - overflow traps are delegated if they occur. */
        case 0x21: /* ADDU rd, rs, rt */
            cpu->gpr[rd] = cpu->gpr[rs] + cpu->gpr[rt];
            cpu->gpr[0] = 0;
            return 0;
        case 0x22: /* SUB - overflow traps are delegated if they occur. */
        case 0x23: /* SUBU */
            cpu->gpr[rd] = cpu->gpr[rs] - cpu->gpr[rt];
            cpu->gpr[0] = 0;
            return 0;
        case 0x24: /* AND */
            cpu->gpr[rd] = cpu->gpr[rs] & cpu->gpr[rt];
            cpu->gpr[0] = 0;
            return 0;
        case 0x25: /* OR */
            cpu->gpr[rd] = cpu->gpr[rs] | cpu->gpr[rt];
            cpu->gpr[0] = 0;
            return 0;
        case 0x26: /* XOR */
            cpu->gpr[rd] = cpu->gpr[rs] ^ cpu->gpr[rt];
            cpu->gpr[0] = 0;
            return 0;
        case 0x27: /* NOR */
            cpu->gpr[rd] = ~(cpu->gpr[rs] | cpu->gpr[rt]);
            cpu->gpr[0] = 0;
            return 0;
        case 0x2A: /* SLT */
            cpu->gpr[rd] = ((int32_t)cpu->gpr[rs] < (int32_t)cpu->gpr[rt]) ? 1u : 0u;
            cpu->gpr[0] = 0;
            return 0;
        case 0x2B: /* SLTU */
            cpu->gpr[rd] = (cpu->gpr[rs] < cpu->gpr[rt]) ? 1u : 0u;
            cpu->gpr[0] = 0;
            return 0;
        default:
            return abort_unsupported(pc, insn, "SPECIAL funct");
        }
        break;

    case 0x02: { /* J target */
        uint32_t target = ((pc + 4) & 0xF0000000u) | (target26(insn) << 2);
        exec_delay_slot(cpu, pc + 4);
        cpu->pc = target;
        return 1;
    }
    case 0x03: { /* JAL target */
        uint32_t target = ((pc + 4) & 0xF0000000u) | (target26(insn) << 2);
        uint32_t return_pc = pc + 8;
        cpu->gpr[31] = return_pc;
        exec_delay_slot(cpu, pc + 4);
#ifdef PSX_HAS_GAME_DISPATCH
        cpu->pc = 0;
        if (psx_dispatch_game_compiled(cpu, target)) {
            if (cpu->pc != 0) return 1;
            *next_pc_out = return_pc;
            return 0;
        }
#endif
        cpu->pc = target;
        return 1;
    }
    case 0x04: { /* BEQ rs, rt, simm */
        int taken = (cpu->gpr[rs] == cpu->gpr[rt]);
        exec_delay_slot(cpu, pc + 4);
        cpu->pc = taken ? (pc + 4 + (simm << 2)) : (pc + 8);
        return 1;
    }
    case 0x05: { /* BNE */
        int taken = (cpu->gpr[rs] != cpu->gpr[rt]);
        exec_delay_slot(cpu, pc + 4);
        cpu->pc = taken ? (pc + 4 + (simm << 2)) : (pc + 8);
        return 1;
    }
    case 0x06: { /* BLEZ */
        int taken = ((int32_t)cpu->gpr[rs] <= 0);
        exec_delay_slot(cpu, pc + 4);
        cpu->pc = taken ? (pc + 4 + (simm << 2)) : (pc + 8);
        return 1;
    }
    case 0x07: { /* BGTZ */
        int taken = ((int32_t)cpu->gpr[rs] > 0);
        exec_delay_slot(cpu, pc + 4);
        cpu->pc = taken ? (pc + 4 + (simm << 2)) : (pc + 8);
        return 1;
    }
    case 0x01: { /* REGIMM: BLTZ/BGEZ/BLTZAL/BGEZAL by rt field */
        int taken;
        switch (rt) {
        case 0x00: /* BLTZ */    taken = ((int32_t)cpu->gpr[rs] <  0); break;
        case 0x01: /* BGEZ */    taken = ((int32_t)cpu->gpr[rs] >= 0); break;
        case 0x10: /* BLTZAL */  taken = ((int32_t)cpu->gpr[rs] <  0);
                                  cpu->gpr[31] = pc + 8; break;
        case 0x11: /* BGEZAL */  taken = ((int32_t)cpu->gpr[rs] >= 0);
                                  cpu->gpr[31] = pc + 8; break;
        default: return abort_unsupported(pc, insn, "REGIMM rt");
        }
        exec_delay_slot(cpu, pc + 4);
        cpu->pc = taken ? (pc + 4 + (simm << 2)) : (pc + 8);
        return 1;
    }
    case 0x08: /* ADDI rt, rs, simm — same as ADDIU, sans overflow trap (we don't model traps here) */
        cpu->gpr[rt] = cpu->gpr[rs] + (uint32_t)simm;
        cpu->gpr[0] = 0;
        return 0;
    case 0x09: /* ADDIU rt, rs, simm */
        cpu->gpr[rt] = cpu->gpr[rs] + (uint32_t)simm;
        cpu->gpr[0] = 0;
        return 0;
    case 0x0A: /* SLTI */
        cpu->gpr[rt] = ((int32_t)cpu->gpr[rs] < simm) ? 1u : 0u;
        cpu->gpr[0] = 0;
        return 0;
    case 0x0B: /* SLTIU */
        cpu->gpr[rt] = (cpu->gpr[rs] < (uint32_t)simm) ? 1u : 0u;
        cpu->gpr[0] = 0;
        return 0;
    case 0x0C: /* ANDI */
        cpu->gpr[rt] = cpu->gpr[rs] & imm;
        cpu->gpr[0] = 0;
        return 0;
    case 0x0D: /* ORI */
        cpu->gpr[rt] = cpu->gpr[rs] | imm;
        cpu->gpr[0] = 0;
        return 0;
    case 0x0E: /* XORI */
        cpu->gpr[rt] = cpu->gpr[rs] ^ imm;
        cpu->gpr[0] = 0;
        return 0;
    case 0x0F: /* LUI rt, imm */
        cpu->gpr[rt] = imm << 16;
        cpu->gpr[0] = 0;
        return 0;
    case 0x10: { /* COP0 */
        uint32_t cop_op = rs;
        if (cop_op == 0x00) { /* MFC0 */
            cpu->gpr[rt] = cpu->cop0[rd];
            cpu->gpr[0] = 0;
            return 0;
        }
        if (cop_op == 0x04) { /* MTC0 */
            cpu->cop0[rd] = cpu->gpr[rt];
            return 0;
        }
        if (cop_op == 0x10 && fnt == 0x10) { /* RFE */
            uint32_t sr = cpu->cop0[12];
            cpu->cop0[12] = (sr & 0xFFFFFFF0u) | ((sr >> 2) & 0x0Fu);
            return 0;
        }
        return abort_unsupported(pc, insn, "COP0 op");
    }
    case 0x12: { /* COP2 / GTE */
        uint32_t cop_op = rs;
        if (cop_op == 0x00) { /* MFC2 */
            cpu->gpr[rt] = gte_read_data(cpu, (uint8_t)rd);
            cpu->gpr[0] = 0;
            return 0;
        }
        if (cop_op == 0x02) { /* CFC2 */
            cpu->gpr[rt] = gte_read_ctrl(cpu, (uint8_t)rd);
            cpu->gpr[0] = 0;
            return 0;
        }
        if (cop_op == 0x04) { /* MTC2 */
            gte_write_data(cpu, (uint8_t)rd, cpu->gpr[rt]);
            return 0;
        }
        if (cop_op == 0x06) { /* CTC2 */
            gte_write_ctrl(cpu, (uint8_t)rd, cpu->gpr[rt]);
            return 0;
        }
        if (cop_op & 0x10) {
            gte_execute(cpu, insn & 0x1FFFFFFu);
            return 0;
        }
        return abort_unsupported(pc, insn, "COP2 op");
    }
    case 0x20: { /* LB rt, simm(rs) */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        cpu->gpr[rt] = (uint32_t)(int32_t)(int8_t)cpu->read_byte(addr);
        cpu->gpr[0] = 0;
        return 0;
    }
    case 0x21: { /* LH */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        cpu->gpr[rt] = (uint32_t)(int32_t)(int16_t)cpu->read_half(addr);
        cpu->gpr[0] = 0;
        return 0;
    }
    case 0x22: { /* LWL */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        cpu->gpr[rt] = interp_lwl(cpu, addr, cpu->gpr[rt]);
        cpu->gpr[0] = 0;
        return 0;
    }
    case 0x23: { /* LW */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        cpu->gpr[rt] = cpu->read_word(addr);
        cpu->gpr[0] = 0;
        return 0;
    }
    case 0x24: { /* LBU */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        cpu->gpr[rt] = (uint32_t)cpu->read_byte(addr);
        cpu->gpr[0] = 0;
        return 0;
    }
    case 0x25: { /* LHU */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        cpu->gpr[rt] = (uint32_t)cpu->read_half(addr);
        cpu->gpr[0] = 0;
        return 0;
    }
    case 0x26: { /* LWR */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        cpu->gpr[rt] = interp_lwr(cpu, addr, cpu->gpr[rt]);
        cpu->gpr[0] = 0;
        return 0;
    }
    case 0x28: { /* SB */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        cpu->write_byte(addr, (uint8_t)cpu->gpr[rt]);
        return 0;
    }
    case 0x29: { /* SH */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        cpu->write_half(addr, (uint16_t)cpu->gpr[rt]);
        return 0;
    }
    case 0x2A: { /* SWL */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        interp_swl(cpu, addr, cpu->gpr[rt]);
        return 0;
    }
    case 0x2B: { /* SW */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        cpu->write_word(addr, cpu->gpr[rt]);
        return 0;
    }
    case 0x2E: { /* SWR */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        interp_swr(cpu, addr, cpu->gpr[rt]);
        return 0;
    }
    case 0x32: { /* LWC2 */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        gte_write_data(cpu, (uint8_t)rt, cpu->read_word(addr));
        return 0;
    }
    case 0x3A: { /* SWC2 */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        cpu->write_word(addr, gte_read_data(cpu, (uint8_t)rt));
        return 0;
    }
    default:
        return abort_unsupported(pc, insn, "primary opcode");
    }
    return 0;
}

/* Public entry point.  Caller (psx_dispatch) has translated `addr` to a
 * KSEG-stripped form already in some cases, so accept any address and
 * mask. Returns 1 if interpretation handled the basic block; 0 if the
 * caller should fall back (e.g. unsupported opcode at the entry, page
 * not dirty). */
int dirty_ram_dispatch(CPUState* cpu, uint32_t addr) {
    uint32_t phys = addr & 0x1FFFFFFFu;

    if (addr == 0x80000048u) {
        cpu->pc = 0;
        if (psx_get_in_exception()) {
            psx_exception_longjmp(); /* does not return */
        }
        return 1;
    }

#ifdef PSX_HAS_GAME_DISPATCH
    if (psx_dispatch_game_compiled(cpu, addr)) return 1;
#endif

    if (!dirty_ram_is_dirty(phys)) return 0;

    /* Reset soft-fail state at block entry. */
    g_unsupported_seen = 0;
    int allow_local_dirty_flow = (phys >= 0x00098000u);

    /* Per-PC entry counter (visible via dirty_ram_stats). */
    DirtyRamPcEntry *pc_entry = pc_table_get_or_insert(phys);
    if (pc_entry) pc_entry->hits++;

    /* Block-entry ring buffer — answers "who tried to JALR into this RAM
     * stub" by capturing cpu->gpr[31] (the caller's RA) at dispatch time.
     * Always-on; eviction keeps memory bounded. */
    {
        uint64_t s = g_dirty_ram_block_log_seq++;
        DirtyRamBlockLogEntry *e =
            &g_dirty_ram_block_log[s & (DIRTY_RAM_BLOCK_LOG_CAP - 1u)];
        e->seq    = s;
        e->target = addr;
        e->ra     = cpu->gpr[31];
        e->a0     = cpu->gpr[4];
        e->a1     = cpu->gpr[5];
        e->a2     = cpu->gpr[6];
        e->a3     = cpu->gpr[7];
        e->sp     = cpu->gpr[29];
        e->frame  = (uint32_t)s_frame_count;
    }

    if (debug_server_dirty_break_maybe_pause(addr, cpu)) {
        debug_server_wait_if_paused();
    }

    /* Run dirty code locally until it returns to compiled/non-dirty code.
     * Runtime-loaded overlays are larger than BIOS install stubs, so stopping
     * at every local branch burns the dispatch loop. */
    enum { MAX_INSNS_PER_DISPATCH = 1000000 };
    uint32_t pc = addr;
    int insns_executed = 0;
    for (int i = 0; i < MAX_INSNS_PER_DISPATCH; i++) {
        uint32_t next_pc = 0;
        int transferred = exec_one(cpu, pc, &next_pc);
        if (g_unsupported_seen) {
            if (insns_executed == 0) {
                /* Couldn't decode the first instruction.  Most likely
                 * dispatch landed in a dirty page that's not actually
                 * code (stale data, return-target into save area, etc.).
                 * Hand off to psx_unknown_dispatch which has its own
                 * pattern-matching trampoline resolver. */
                return 0;
            }
            /* Made some progress, then hit unknown.  Treat as a no-op
             * return like psx_unknown_dispatch does for unrecognized
             * targets — set cpu->pc=0 so the trampoline exits cleanly.
             * If this turns out to be load-bearing, measurement will
             * surface it as a card-protocol stall and we can add the
             * missing opcode.
             *
             * No fprintf — read the last-* globals via TCP if needed
             * (CLAUDE.md §3). Synchronous stderr at the rate this fires
             * starves the dispatch loop and the debug-server poll. */
            g_dirty_ram_unsupported_midblock++;
            g_dirty_ram_last_unsupported_pc     = g_unsupported_pc;
            g_dirty_ram_last_unsupported_insn   = g_unsupported_insn;
            g_dirty_ram_last_unsupported_reason = g_unsupported_reason;
            cpu->pc = 0;
            return 1;
        }
        g_dirty_ram_insns_run++;
        insns_executed++;
        if (transferred) {
            uint32_t target = cpu->pc;
            if (allow_local_dirty_flow &&
                target != 0 && dirty_ram_is_dirty(target & 0x1FFFFFFFu)) {
                pc = target;
                if ((insns_executed & 0xFFF) == 0) {
                    debug_server_poll();
                    debug_server_wait_if_paused();
                    psx_check_interrupts(cpu);
                }
                continue;
            }
            g_dirty_ram_blocks_run++;
            if (pc_entry) pc_entry->insns += (uint64_t)insns_executed;
            return 1;
        }
        pc = next_pc;
        /* Straight-line code that left the dirty page — hand back to
         * static dispatch by setting cpu->pc and returning. */
        uint32_t next_phys = pc & 0x1FFFFFFFu;
        if (!dirty_ram_is_dirty(next_phys)) {
            cpu->pc = pc;
            g_dirty_ram_blocks_run++;
            if (pc_entry) pc_entry->insns += (uint64_t)insns_executed;
            return 1;
        }
    }
    g_dirty_ram_aborts++;
    g_dirty_ram_last_unsupported_pc = pc;
    g_dirty_ram_last_unsupported_insn = fetch_word(pc & 0x1FFFFFFFu);
    g_dirty_ram_last_unsupported_reason = "instruction guard";
    abort();
    return 0;
}
