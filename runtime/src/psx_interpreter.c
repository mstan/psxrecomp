/* psx_interpreter.c — Minimal R3000A interpreter for oracle builds.
 *
 * Implements the full MIPS I instruction set used by the PSX BIOS:
 *   - ALU, shifts, multiply/divide, branches, jumps
 *   - Loads/stores including unaligned (LWL/LWR/SWL/SWR)
 *   - COP0 (MFC0, MTC0, RFE)
 *   - COP2/GTE (delegated to gte.cpp)
 *   - SYSCALL, BREAK
 *
 * Uses the same CPUState and memory bus as the recompiled native build.
 * NOT compiled in the native target — stub_interpreter.c provides no-ops.
 */

#include "psx_interpreter.h"
#include "cpu_state.h"
#include "gpu.h"
#include "sio.h"
#include "timers.h"
#include "cdrom.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* I_STAT and I_MASK are owned by memory.c */
extern uint32_t i_stat;
extern uint32_t i_mask;

#define COP0_SR    12
#define COP0_CAUSE 13
#define COP0_EPC   14
#define IRQ_VBLANK 0

/* VBlank scheduling — matches interrupts.c */
#define VBLANK_INTERVAL 50000
static uint32_t s_vblank_count;

/* ---- Decode helpers ---- */
#define OP(i)    (((i) >> 26) & 0x3F)
#define RS(i)    (((i) >> 21) & 0x1F)
#define RT(i)    (((i) >> 16) & 0x1F)
#define RD(i)    (((i) >> 11) & 0x1F)
#define SA(i)    (((i) >>  6) & 0x1F)
#define FUNC(i)  ((i) & 0x3F)
#define IMM16(i) ((uint16_t)((i) & 0xFFFF))
#define SIMM(i)  ((int32_t)(int16_t)((i) & 0xFFFF))
#define JTARGET(i,pc) ((((pc) + 4) & 0xF0000000u) | (((i) & 0x03FFFFFFu) << 2))

/* ---- State ---- */
static int  s_hit_bp;
static int  s_trace_on;
static int  s_halted;

/* PC breakpoints */
#define MAX_BREAKPOINTS 64
static uint32_t s_bp[MAX_BREAKPOINTS];
static int      s_bp_count;

/* Trace ring */
#define TRACE_RING_SIZE (1u << 20)  /* 1M entries */
#define TRACE_RING_MASK (TRACE_RING_SIZE - 1)
static InterpTraceEntry s_trace_ring[TRACE_RING_SIZE];
static uint64_t s_trace_seq;
static uint32_t s_trace_write_idx;

/* Pending load delay slot (R3000A has a 1-instruction load delay). */
static uint32_t s_load_reg;
static uint32_t s_load_val;

/* ---- Init ---- */

void interp_init(CPUState* cpu) {
    s_hit_bp = 0;
    s_trace_on = 0;
    s_halted = 0;
    s_bp_count = 0;
    s_trace_seq = 0;
    s_trace_write_idx = 0;
    s_load_reg = 0;
    s_load_val = 0;
    s_vblank_count = 0;
    memset(s_trace_ring, 0, sizeof(s_trace_ring));
}

/* ---- Breakpoints ---- */

int interp_break_add(uint32_t pc) {
    if (s_bp_count >= MAX_BREAKPOINTS) return -1;
    s_bp[s_bp_count] = pc;
    return s_bp_count++;
}

int interp_break_remove(uint32_t pc) {
    for (int i = 0; i < s_bp_count; i++) {
        if (s_bp[i] == pc) {
            s_bp[i] = s_bp[--s_bp_count];
            return 1;
        }
    }
    return 0;
}

void interp_break_clear_all(void) { s_bp_count = 0; }
int  interp_hit_breakpoint(void)  { return s_hit_bp; }

/* ---- Trace ---- */

void interp_trace_enable(int on) { s_trace_on = on; }
uint64_t interp_trace_count(void) { return s_trace_seq; }

const InterpTraceEntry* interp_trace_get(uint32_t ring_idx) {
    if (ring_idx >= TRACE_RING_SIZE) return NULL;
    uint32_t avail = (s_trace_seq < TRACE_RING_SIZE)
                   ? (uint32_t)s_trace_seq : TRACE_RING_SIZE;
    if (ring_idx >= avail) return NULL;
    uint32_t idx = (s_trace_write_idx - avail + ring_idx) & TRACE_RING_MASK;
    return &s_trace_ring[idx];
}

static void trace_record(CPUState* cpu, uint32_t insn) {
    InterpTraceEntry* e = &s_trace_ring[s_trace_write_idx];
    e->pc = cpu->pc;
    e->insn = insn;
    memcpy(e->gpr, cpu->gpr, sizeof(e->gpr));
    e->hi = cpu->hi;
    e->lo = cpu->lo;
    e->cop0_sr = cpu->cop0[12];
    e->cop0_cause = cpu->cop0[13];
    e->cop0_epc = cpu->cop0[14];
    e->seq = s_trace_seq++;
    s_trace_write_idx = (s_trace_write_idx + 1) & TRACE_RING_MASK;
}

/* ---- Register write (enforces $0 = 0) ---- */

static inline void set_reg(CPUState* cpu, uint32_t r, uint32_t v) {
    if (r != 0) cpu->gpr[r] = v;
}

/* ---- Apply pending load delay ---- */

static inline void apply_load_delay(CPUState* cpu) {
    if (s_load_reg != 0) {
        cpu->gpr[s_load_reg] = s_load_val;
        s_load_reg = 0;
    }
}

static inline void set_load_delay(uint32_t reg, uint32_t val) {
    s_load_reg = reg;
    s_load_val = val;
}

/* ---- Exception ---- */

static void raise_exception(CPUState* cpu, uint32_t exccode, int in_delay) {
    uint32_t sr = cpu->cop0[12];
    /* Push the SR exception stack (shift bits [5:0] left by 2). */
    cpu->cop0[12] = (sr & ~0x3Fu) | ((sr & 0x0Fu) << 2);
    /* Set Cause ExcCode. */
    cpu->cop0[13] = (cpu->cop0[13] & ~0x7Cu) | (exccode << 2);
    if (in_delay)
        cpu->cop0[13] |= (1u << 31);  /* BD bit */
    else
        cpu->cop0[13] &= ~(1u << 31);
    /* EPC */
    cpu->cop0[14] = in_delay ? cpu->pc - 4 : cpu->pc;
    /* Vector */
    cpu->pc = (sr & 0x00400000u) ? 0xBFC00180u : 0x80000080u;
}

/* ---- Execute one instruction ---- */

static void exec_one(CPUState* cpu) {
    uint32_t pc = cpu->pc;

    /* Trap: halt if we jumped to address 0 (NULL function pointer). */
    if (pc == 0) {
        fprintf(stderr, "INTERP: NULL jump (PC=0x00000000), ra=0x%08X, t0=0x%08X, t1=0x%08X\n",
                cpu->gpr[31], cpu->gpr[8], cpu->gpr[9]);
        s_halted = 1;
        return;
    }

    uint32_t insn = cpu->read_word(pc);

    /* Trace before execution. */
    if (s_trace_on) trace_record(cpu, insn);

    /* Advance PC (branch/jump will override). */
    cpu->pc = pc + 4;

    /* Apply pending load from previous instruction. */
    apply_load_delay(cpu);

    uint32_t op = OP(insn);
    uint32_t rs_val = cpu->gpr[RS(insn)];
    uint32_t rt_val = cpu->gpr[RT(insn)];

    switch (op) {
    case 0x00: { /* SPECIAL */
        uint32_t func = FUNC(insn);
        uint32_t rd = RD(insn);
        uint32_t sa = SA(insn);
        switch (func) {
        case 0x00: set_reg(cpu, rd, rt_val << sa); break;           /* SLL */
        case 0x02: set_reg(cpu, rd, rt_val >> sa); break;           /* SRL */
        case 0x03: set_reg(cpu, rd, (uint32_t)((int32_t)rt_val >> sa)); break; /* SRA */
        case 0x04: set_reg(cpu, rd, rt_val << (rs_val & 31)); break; /* SLLV */
        case 0x06: set_reg(cpu, rd, rt_val >> (rs_val & 31)); break; /* SRLV */
        case 0x07: set_reg(cpu, rd, (uint32_t)((int32_t)rt_val >> (rs_val & 31))); break; /* SRAV */
        case 0x08: { /* JR */
            uint32_t target = rs_val;
            exec_one(cpu); /* delay slot */
            cpu->pc = target;
            return;
        }
        case 0x09: { /* JALR */
            uint32_t target = rs_val;
            set_reg(cpu, rd, pc + 8);
            exec_one(cpu); /* delay slot */
            cpu->pc = target;
            return;
        }
        case 0x0C: raise_exception(cpu, 8, 0); return;              /* SYSCALL */
        case 0x0D: raise_exception(cpu, 9, 0); return;              /* BREAK */
        case 0x10: set_reg(cpu, rd, cpu->hi); break;                /* MFHI */
        case 0x11: cpu->hi = rs_val; break;                         /* MTHI */
        case 0x12: set_reg(cpu, rd, cpu->lo); break;                /* MFLO */
        case 0x13: cpu->lo = rs_val; break;                         /* MTLO */
        case 0x18: { /* MULT */
            int64_t r = (int64_t)(int32_t)rs_val * (int64_t)(int32_t)rt_val;
            cpu->lo = (uint32_t)r; cpu->hi = (uint32_t)(r >> 32); break;
        }
        case 0x19: { /* MULTU */
            uint64_t r = (uint64_t)rs_val * (uint64_t)rt_val;
            cpu->lo = (uint32_t)r; cpu->hi = (uint32_t)(r >> 32); break;
        }
        case 0x1A: /* DIV */
            if (rt_val == 0) { cpu->lo = (rs_val & 0x80000000u) ? 1 : 0xFFFFFFFFu; cpu->hi = rs_val; }
            else if (rs_val == 0x80000000u && rt_val == 0xFFFFFFFFu) { cpu->lo = 0x80000000u; cpu->hi = 0; }
            else { cpu->lo = (uint32_t)((int32_t)rs_val / (int32_t)rt_val); cpu->hi = (uint32_t)((int32_t)rs_val % (int32_t)rt_val); }
            break;
        case 0x1B: /* DIVU */
            if (rt_val == 0) { cpu->lo = 0xFFFFFFFFu; cpu->hi = rs_val; }
            else { cpu->lo = rs_val / rt_val; cpu->hi = rs_val % rt_val; }
            break;
        case 0x20: { /* ADD (overflow trap) */
            int64_t r = (int64_t)(int32_t)rs_val + (int64_t)(int32_t)rt_val;
            if (r < (int64_t)INT32_MIN || r > (int64_t)INT32_MAX) { raise_exception(cpu, 12, 0); return; }
            set_reg(cpu, rd, (uint32_t)(int32_t)r); break;
        }
        case 0x21: set_reg(cpu, rd, rs_val + rt_val); break;       /* ADDU */
        case 0x22: { /* SUB (overflow trap) */
            int64_t r = (int64_t)(int32_t)rs_val - (int64_t)(int32_t)rt_val;
            if (r < (int64_t)INT32_MIN || r > (int64_t)INT32_MAX) { raise_exception(cpu, 12, 0); return; }
            set_reg(cpu, rd, (uint32_t)(int32_t)r); break;
        }
        case 0x23: set_reg(cpu, rd, rs_val - rt_val); break;       /* SUBU */
        case 0x24: set_reg(cpu, rd, rs_val & rt_val); break;       /* AND */
        case 0x25: set_reg(cpu, rd, rs_val | rt_val); break;       /* OR */
        case 0x26: set_reg(cpu, rd, rs_val ^ rt_val); break;       /* XOR */
        case 0x27: set_reg(cpu, rd, ~(rs_val | rt_val)); break;    /* NOR */
        case 0x2A: set_reg(cpu, rd, (int32_t)rs_val < (int32_t)rt_val ? 1 : 0); break; /* SLT */
        case 0x2B: set_reg(cpu, rd, rs_val < rt_val ? 1 : 0); break; /* SLTU */
        default:
            /* R3000A ignores undefined SPECIAL encodings (no trap). */
            break;
        }
        break;
    }

    case 0x01: { /* REGIMM: BLTZ, BGEZ, BLTZAL, BGEZAL */
        uint32_t rt = RT(insn);
        int link = (rt & 0x10) != 0;
        int cond = (rt & 0x01) ? ((int32_t)rs_val >= 0) : ((int32_t)rs_val < 0);
        if (link) set_reg(cpu, 31, pc + 8);
        if (cond) {
            uint32_t target = pc + 4 + (SIMM(insn) << 2);
            /* Execute delay slot, then branch. */
            exec_one(cpu);
            cpu->pc = target;
            return;
        }
        break;
    }

    case 0x02: { /* J */
        uint32_t target = JTARGET(insn, pc);
        exec_one(cpu); /* delay slot */
        cpu->pc = target;
        return;
    }
    case 0x03: { /* JAL */
        uint32_t target = JTARGET(insn, pc);
        set_reg(cpu, 31, pc + 8);
        exec_one(cpu); /* delay slot */
        cpu->pc = target;
        return;
    }

    case 0x04: { /* BEQ */
        if (rs_val == rt_val) {
            uint32_t target = pc + 4 + (SIMM(insn) << 2);
            exec_one(cpu); cpu->pc = target; return;
        }
        break;
    }
    case 0x05: { /* BNE */
        if (rs_val != rt_val) {
            uint32_t target = pc + 4 + (SIMM(insn) << 2);
            exec_one(cpu); cpu->pc = target; return;
        }
        break;
    }
    case 0x06: { /* BLEZ */
        if ((int32_t)rs_val <= 0) {
            uint32_t target = pc + 4 + (SIMM(insn) << 2);
            exec_one(cpu); cpu->pc = target; return;
        }
        break;
    }
    case 0x07: { /* BGTZ */
        if ((int32_t)rs_val > 0) {
            uint32_t target = pc + 4 + (SIMM(insn) << 2);
            exec_one(cpu); cpu->pc = target; return;
        }
        break;
    }

    case 0x08: { /* ADDI (overflow trap) */
        int64_t r = (int64_t)(int32_t)rs_val + (int64_t)SIMM(insn);
        if (r < (int64_t)INT32_MIN || r > (int64_t)INT32_MAX) { raise_exception(cpu, 12, 0); return; }
        set_reg(cpu, RT(insn), (uint32_t)(int32_t)r); break;
    }
    case 0x09: set_reg(cpu, RT(insn), rs_val + (uint32_t)SIMM(insn)); break; /* ADDIU */
    case 0x0A: set_reg(cpu, RT(insn), (int32_t)rs_val < SIMM(insn) ? 1 : 0); break; /* SLTI */
    case 0x0B: set_reg(cpu, RT(insn), rs_val < (uint32_t)SIMM(insn) ? 1 : 0); break; /* SLTIU */
    case 0x0C: set_reg(cpu, RT(insn), rs_val & IMM16(insn)); break; /* ANDI */
    case 0x0D: set_reg(cpu, RT(insn), rs_val | IMM16(insn)); break; /* ORI */
    case 0x0E: set_reg(cpu, RT(insn), rs_val ^ IMM16(insn)); break; /* XORI */
    case 0x0F: set_reg(cpu, RT(insn), (uint32_t)IMM16(insn) << 16); break; /* LUI */

    /* --- COP0 --- */
    case 0x10: {
        uint32_t cop_op = RS(insn);
        switch (cop_op) {
        case 0x00: /* MFC0 */
            set_load_delay(RT(insn), cpu->cop0[RD(insn)]);
            break;
        case 0x04: /* MTC0 */
            cpu->cop0[RD(insn)] = rt_val;
            break;
        case 0x10: /* RFE */
            if ((insn & 0x3F) == 0x10) {
                uint32_t sr = cpu->cop0[12];
                cpu->cop0[12] = (sr & 0xFFFFFFC0u) | ((sr >> 2) & 0x0Fu);
            }
            break;
        default:
            fprintf(stderr, "INTERP: unknown COP0 op 0x%02X at PC=0x%08X\n", cop_op, pc);
            break;
        }
        break;
    }

    /* --- COP2 (GTE) --- */
    case 0x12: {
        uint32_t cop_op = RS(insn);
        switch (cop_op) {
        case 0x00: /* MFC2 */
            set_load_delay(RT(insn), gte_read_data(cpu, RD(insn)));
            break;
        case 0x02: /* CFC2 */
            set_load_delay(RT(insn), gte_read_ctrl(cpu, RD(insn)));
            break;
        case 0x04: /* MTC2 */
            gte_write_data(cpu, RD(insn), rt_val);
            break;
        case 0x06: /* CTC2 */
            gte_write_ctrl(cpu, RD(insn), rt_val);
            break;
        default:
            if (cop_op & 0x10) {
                /* COP2 command */
                gte_execute(cpu, insn & 0x1FFFFFFu);
            } else {
                fprintf(stderr, "INTERP: unknown COP2 op 0x%02X at PC=0x%08X\n", cop_op, pc);
            }
            break;
        }
        break;
    }

    /* --- Loads --- */
    case 0x20: { /* LB */
        uint32_t addr = rs_val + (uint32_t)SIMM(insn);
        set_load_delay(RT(insn), (uint32_t)(int32_t)(int8_t)cpu->read_byte(addr));
        break;
    }
    case 0x21: { /* LH */
        uint32_t addr = rs_val + (uint32_t)SIMM(insn);
        set_load_delay(RT(insn), (uint32_t)(int32_t)(int16_t)cpu->read_half(addr));
        break;
    }
    case 0x22: { /* LWL */
        uint32_t addr = rs_val + (uint32_t)SIMM(insn);
        uint32_t aligned = addr & ~3u;
        uint32_t word = cpu->read_word(aligned);
        uint32_t cur = cpu->gpr[RT(insn)];
        switch (addr & 3) {
            case 0: cur = (cur & 0x00FFFFFFu) | (word << 24); break;
            case 1: cur = (cur & 0x0000FFFFu) | (word << 16); break;
            case 2: cur = (cur & 0x000000FFu) | (word <<  8); break;
            case 3: cur = word; break;
        }
        set_reg(cpu, RT(insn), cur);
        break;
    }
    case 0x23: { /* LW */
        uint32_t addr = rs_val + (uint32_t)SIMM(insn);
        set_load_delay(RT(insn), cpu->read_word(addr));
        break;
    }
    case 0x24: { /* LBU */
        uint32_t addr = rs_val + (uint32_t)SIMM(insn);
        set_load_delay(RT(insn), (uint32_t)cpu->read_byte(addr));
        break;
    }
    case 0x25: { /* LHU */
        uint32_t addr = rs_val + (uint32_t)SIMM(insn);
        set_load_delay(RT(insn), (uint32_t)cpu->read_half(addr));
        break;
    }
    case 0x26: { /* LWR */
        uint32_t addr = rs_val + (uint32_t)SIMM(insn);
        uint32_t aligned = addr & ~3u;
        uint32_t word = cpu->read_word(aligned);
        uint32_t cur = cpu->gpr[RT(insn)];
        switch (addr & 3) {
            case 0: cur = word; break;
            case 1: cur = (cur & 0xFF000000u) | (word >>  8); break;
            case 2: cur = (cur & 0xFFFF0000u) | (word >> 16); break;
            case 3: cur = (cur & 0xFFFFFF00u) | (word >> 24); break;
        }
        set_reg(cpu, RT(insn), cur);
        break;
    }

    /* --- Stores --- */
    case 0x28: { /* SB */
        uint32_t addr = rs_val + (uint32_t)SIMM(insn);
        cpu->write_byte(addr, (uint8_t)(rt_val & 0xFF));
        break;
    }
    case 0x29: { /* SH */
        uint32_t addr = rs_val + (uint32_t)SIMM(insn);
        cpu->write_half(addr, (uint16_t)(rt_val & 0xFFFF));
        break;
    }
    case 0x2A: { /* SWL */
        uint32_t addr = rs_val + (uint32_t)SIMM(insn);
        uint32_t aligned = addr & ~3u;
        uint32_t cur = cpu->read_word(aligned);
        switch (addr & 3) {
            case 0: cur = (cur & 0xFFFFFF00u) | (rt_val >> 24); break;
            case 1: cur = (cur & 0xFFFF0000u) | (rt_val >> 16); break;
            case 2: cur = (cur & 0xFF000000u) | (rt_val >>  8); break;
            case 3: cur = rt_val; break;
        }
        cpu->write_word(aligned, cur);
        break;
    }
    case 0x2B: { /* SW */
        uint32_t addr = rs_val + (uint32_t)SIMM(insn);
        cpu->write_word(addr, rt_val);
        break;
    }
    case 0x2E: { /* SWR */
        uint32_t addr = rs_val + (uint32_t)SIMM(insn);
        uint32_t aligned = addr & ~3u;
        uint32_t cur = cpu->read_word(aligned);
        switch (addr & 3) {
            case 0: cur = rt_val; break;
            case 1: cur = (cur & 0x000000FFu) | (rt_val <<  8); break;
            case 2: cur = (cur & 0x0000FFFFu) | (rt_val << 16); break;
            case 3: cur = (cur & 0x00FFFFFFu) | (rt_val << 24); break;
        }
        cpu->write_word(aligned, cur);
        break;
    }

    /* --- COP2 load/store (LWC2/SWC2) --- */
    case 0x32: { /* LWC2 */
        uint32_t addr = rs_val + (uint32_t)SIMM(insn);
        gte_write_data(cpu, RT(insn), cpu->read_word(addr));
        break;
    }
    case 0x3A: { /* SWC2 */
        uint32_t addr = rs_val + (uint32_t)SIMM(insn);
        cpu->write_word(addr, gte_read_data(cpu, RT(insn)));
        break;
    }

    default:
        /* R3000A ignores undefined opcodes (no trap). */
        break;
    }

    /* Enforce $0 = 0 after every instruction. */
    cpu->gpr[0] = 0;
}

/* ---- Interrupt injection ---- */

static void interp_check_interrupts(CPUState* cpu) {
    /* SIO tick. cycles=0 in legacy mode (Phase 1.0a signature change;
     * body ignores cycles, behaves identically to original). */
    sio_tick(0);

    /* VBlank / timer scheduling. */
    s_vblank_count++;
    if (s_vblank_count >= VBLANK_INTERVAL) {
        s_vblank_count = 0;
        i_stat |= (1u << IRQ_VBLANK);
        gpu_vblank_tick();
        timers_tick(33868);
        cdrom_tick();
    }

    /* Check if interrupt should fire. */
    if ((i_stat & i_mask) == 0) return;

    uint32_t sr = cpu->cop0[COP0_SR];
    if (!(sr & 0x01u)) return;       /* IEc disabled */
    if (!(sr & (1u << 10))) return;   /* IM2 not set */

    /* Fire interrupt exception.
     * Push SR stack, set Cause, set EPC, jump to vector. */
    cpu->cop0[COP0_CAUSE] = (cpu->cop0[COP0_CAUSE] & ~0x7Cu) | (0u << 2); /* ExcCode=0 (interrupt) */
    cpu->cop0[COP0_CAUSE] |= (1u << 10); /* IP2 */
    cpu->cop0[COP0_SR] = (sr & ~0x3Fu) | ((sr & 0x0Fu) << 2);
    cpu->cop0[COP0_EPC] = cpu->pc;

    /* Vector: BEV selects between RAM and ROM exception vectors. */
    if (sr & 0x00400000u)
        cpu->pc = 0xBFC00180u;
    else
        cpu->pc = 0x80000080u;
}

/* ---- Public step/run ---- */

uint32_t interp_step(CPUState* cpu, uint32_t count) {
    s_hit_bp = 0;
    uint32_t executed = 0;
    for (uint32_t i = 0; i < count; i++) {
        /* Check PC breakpoints before execution. */
        for (int b = 0; b < s_bp_count; b++) {
            if (cpu->pc == s_bp[b]) {
                s_hit_bp = 1;
                return executed;
            }
        }
        exec_one(cpu);
        executed++;
        if (s_halted) break;

        /* Check interrupts periodically (every 16 instructions). */
        if ((executed & 0xF) == 0) {
            interp_check_interrupts(cpu);
        }
    }
    return executed;
}

uint32_t interp_run(CPUState* cpu, uint32_t max_instructions) {
    return interp_step(cpu, max_instructions);
}
