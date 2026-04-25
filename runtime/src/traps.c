/* traps.c — Phase 2 trap handlers.
 *
 * All traps abort with a diagnostic message.
 * Phase 2 expects MMIO abort before any trap fires.
 */

#include "cpu_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declarations from interrupts.c */
int psx_get_in_exception(void);
void psx_exception_longjmp(void);

static void trap_crash(const char* msg) {
    FILE* cf = fopen("psx_crash.txt", "w");
    if (cf) { fprintf(cf, "%s\n", msg); fclose(cf); }
}

void psx_syscall(CPUState* cpu, uint32_t code) {
    /*
     * PS1 BIOS SYSCALL convention:
     *   $a0 = 1: EnterCriticalSection — disable interrupts, return old SR
     *   $a0 = 2: ExitCriticalSection  — enable interrupts, return old SR
     *   $a0 = 3: ReturnFromException  — restore full TCB state + RFE
     *
     * Syscalls 1 and 2 are always handled directly — they only touch IEc
     * in SR and don't need the full exception mechanism.
     *
     * Syscall 3 and unknown numbers route through the real BIOS exception
     * handler once it's installed, because ReturnFromException must restore
     * the full register state from the current thread's TCB (including the
     * saved SR which carries IM[2]).
     */
    uint32_t func = cpu->gpr[4]; /* $a0 = syscall function number */
    uint32_t sr = cpu->cop0[12];

    switch (func) {
        case 1: /* EnterCriticalSection: disable interrupts */
            cpu->cop0[12] = sr & ~1u; /* clear IEc (bit 0) */
            cpu->gpr[2] = sr & 1u; /* return old IEc */
            cpu->pc = 0;
            return;

        case 2: /* ExitCriticalSection: enable interrupts */
            cpu->cop0[12] = sr | 0x0401u; /* set IEc (bit 0) + IM[2] (bit 10) */
            cpu->gpr[2] = 0;
            cpu->pc = 0;
            return;

        case 3: { /* ReturnFromException: restore from TCB + RFE */
            uint32_t tcb_ptr_addr = cpu->read_word(0x00000108u);
            if (tcb_ptr_addr != 0) {
                uint32_t save_area = cpu->read_word(tcb_ptr_addr);
                if (save_area != 0) {
                    save_area += 8; /* handler adds 8 before saving */
                    uint32_t saved_epc = cpu->read_word(save_area + 128);
                    uint32_t saved_sr  = cpu->read_word(save_area + 140);
                    /* Restore ALL GPRs from TCB save area.
                     * Layout: offset 0 = $zero (skip), 4 = $at, ... 124 = $ra,
                     *         128 = EPC, 132 = HI, 136 = LO, 140 = SR. */
                    for (int i = 1; i < 32; i++) {
                        cpu->gpr[i] = cpu->read_word(save_area + i * 4);
                    }
                    cpu->hi = cpu->read_word(save_area + 132);
                    cpu->lo = cpu->read_word(save_area + 136);
                    /* RFE pop on saved SR (clears bits [5:0], shifts [5:2]→[3:0]). */
                    cpu->cop0[12] = (saved_sr & 0xFFFFFFC0u) | ((saved_sr >> 2) & 0x0Fu);
                    cpu->pc = saved_epc;
                    if (psx_get_in_exception()) {
                        psx_exception_longjmp(); /* unwind handler */
                    }
                    return;
                }
            }
            /* Fallback: simple RFE on current SR. */
            cpu->cop0[12] = (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu);
            cpu->pc = cpu->cop0[14];
            return;
        }

        default:
            break;
    }

    /* Unknown syscalls: route through the real BIOS exception handler. */
    uint32_t handler_word = cpu->read_word(0x80000080u);
    if (handler_word != 0) {
        /* Route through the real BIOS exception handler. */
        cpu->cop0[14] = cpu->pc;  /* EPC */
        cpu->cop0[13] = (cpu->cop0[13] & ~0x7Cu) | (0x08u << 2); /* Cause: Syscall */
        cpu->cop0[12] = (sr & ~0x3Fu) | ((sr & 0x0Fu) << 2); /* SR push */
        uint32_t vector = (sr & 0x00400000u) ? 0xBFC00180u : 0x80000080u;
        psx_dispatch(cpu, vector);
        return;
    }

    /* Early boot fallback for syscall 3. */
    if (func == 3) {
        cpu->cop0[12] = (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu);
        cpu->pc = cpu->cop0[14];
        return;
    }

    /* Unknown syscall number and no handler — fatal. */
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "SYSCALL %u with no handler @ PC=0x%08X",
                 func, cpu->pc);
        trap_crash(buf);
        fprintf(stderr, "%s\n", buf); fflush(stderr);
        exit(1);
    }
}

void psx_break(CPUState* cpu, uint32_t code, uint32_t pc) {
    char buf[128];
    snprintf(buf, sizeof(buf), "BREAK @ PC=0x%08X, code=0x%05X", pc, code);
    trap_crash(buf);
    fprintf(stderr, "%s\n", buf); fflush(stderr);
    exit(1);
}

void psx_arith_overflow(CPUState* cpu) {
    char buf[128];
    snprintf(buf, sizeof(buf), "OVERFLOW @ PC=0x%08X", cpu->pc);
    trap_crash(buf);
    fprintf(stderr, "%s\n", buf); fflush(stderr);
    exit(1);
}

void psx_unaligned_access(CPUState* cpu, uint32_t addr, uint32_t pc) {
    /* Survivable fault: log diagnostics and let the generated code's
     * `return;` skip the rest of the current function. */
    static int count = 0;
    if (count < 3) {
        char buf[1024];
        int n = snprintf(buf, sizeof(buf),
            "ADEL: addr=0x%08X PC=0x%08X ra=0x%08X v0=0x%08X a0=0x%08X t9=0x%08X (hit #%d)\n"
            "  Callback table at 0x800DFEE0:",
            addr, pc, cpu->gpr[31], cpu->gpr[2], cpu->gpr[4], cpu->gpr[25], count + 1);
        for (int i = 0; i < 11; i++) {
            uint32_t cb = cpu->read_word(0x800DFEE0u + (uint32_t)(i * 4));
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, " [%d]=0x%08X", i, cb);
        }
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
            "\n  Mask=0x%08X s0=0x%08X s1=0x%08X s2=0x%08X",
            cpu->read_word(0x800DFF0Cu),
            cpu->gpr[16], cpu->gpr[17], cpu->gpr[18]);
        trap_crash(buf);
        count++;
    }
}

void psx_unknown_dispatch(CPUState* cpu, uint32_t addr, uint32_t phys) {
    /* NULL dispatch: address 0 is never a valid function target.
     * Silently absorb — this can happen when a corrupt function pointer
     * or uninitialized callback slot is dispatched. */
    if (addr == 0) {
        cpu->pc = 0;
        return;
    }

    /* Detect ReturnFromException: when the recompiled B0:0x17 function
     * (or handler epilogue) has already restored all registers from the
     * TCB and done RFE, it sets cpu->pc = saved_epc = our sentinel
     * 0x80000048.  The dispatch loop tail-calls here.  If we're inside
     * the exception handler, longjmp back to psx_check_interrupts to
     * properly unwind the handler call tree. */
    if (addr == 0x80000048u && psx_get_in_exception()) {
        cpu->pc = 0;
        psx_exception_longjmp(); /* does not return */
    }

    /* Exception handler chain-walk continuation: 0xBFC10910 (phys 0x00000E10)
     * is the return address set by jalr $s1 in the exception handler chain
     * walker.  In the merged exception handler function, the continuation
     * code at BFC10910 follows the psx_dispatch call as a C fall-through.
     * When the chain handler returns via jr $ra (compiled as C `return;`),
     * the fall-through handles the continuation.  But if external code
     * dispatches to BFC10910 directly (e.g., the trampoline resolver or
     * the psx_dispatch tail-call loop picking up $ra), it's a no-op — the
     * continuation was already handled by the merged function. */
    if (phys == 0x00000E10u) {
        cpu->pc = 0;
        return;
    }

    /* Reject non-word-aligned targets — corrupt function pointer. Hard fail. */
    if (addr & 3) {
        char buf[512];
        snprintf(buf, sizeof(buf),
            "DISPATCH FATAL: misaligned target 0x%08X\n"
            "  aligned form: 0x%08X\n"
            "  physical:     0x%08X\n"
            "  cpu->pc:      0x%08X\n"
            "  $ra:          0x%08X\n"
            "  $t9:          0x%08X\n"
            "  $v0:          0x%08X\n"
            "  $a0:          0x%08X\n"
            "  $a1:          0x%08X\n"
            "  $a2:          0x%08X\n"
            "  $a3:          0x%08X\n"
            "  COP0_EPC:     0x%08X\n"
            "  COP0_SR:      0x%08X\n"
            "  COP0_Cause:   0x%08X\n",
            addr, addr & ~3u, phys,
            cpu->pc, cpu->gpr[31], cpu->gpr[25],
            cpu->gpr[2], cpu->gpr[4], cpu->gpr[5],
            cpu->gpr[6], cpu->gpr[7],
            cpu->cop0[14], cpu->cop0[12], cpu->cop0[13]);
        trap_crash(buf);
        fprintf(stderr, "%s", buf);
        fflush(stderr);
        exit(1);
    }

    /*
     * The BIOS writes small jump trampolines into RAM at runtime
     * (e.g., at 0xA0, 0xB0, 0xC0 for the A0/B0/C0 vectors).
     * Pattern: lui rN, hi / addiu rN, rN, lo / jr rN / nop
     * Since we can't execute RAM instructions, resolve the pattern
     * and re-dispatch to the computed target.
     */
    {
        uint32_t w0 = cpu->read_word(addr);
        uint32_t w1 = cpu->read_word(addr + 4);
        uint32_t w2 = cpu->read_word(addr + 8);
        uint32_t op0 = (w0 >> 26) & 0x3F;
        uint32_t op1 = (w1 >> 26) & 0x3F;

        /* Trampoline resolution: set cpu->pc so the dispatch loop re-dispatches.
         * This avoids growing the native stack for RAM trampoline chains. */

        /* Pattern 1: J target */
        if (op0 == 2) {
            uint32_t target = (addr & 0xF0000000u) | ((w0 & 0x03FFFFFFu) << 2);
            cpu->pc = target;
            return;
        }

        /* Pattern 2: JR rs (single instruction) */
        if (op0 == 0 && (w0 & 0x3F) == 0x08) {
            uint32_t rs = (w0 >> 21) & 0x1F;
            cpu->pc = cpu->gpr[rs];
            return;
        }

        /* Pattern 3: addiu rN, $zero, imm / jr rN (small address trampoline) */
        if (op0 == 0x09) { /* ADDIU */
            uint32_t rs0 = (w0 >> 21) & 0x1F;
            uint32_t rt0 = (w0 >> 16) & 0x1F;
            if (rs0 == 0) { /* addiu rN, $zero, imm = li rN, imm */
                int16_t imm = (int16_t)(w0 & 0xFFFF);
                uint32_t target = (uint32_t)(int32_t)imm;
                /* w1 should be jr rN */
                if ((w1 & 0xFC1FFFFF) == 0x00000008) { /* jr rs */
                    uint32_t jr_rs = (w1 >> 21) & 0x1F;
                    if (jr_rs == rt0) {
                        /* w2 is delay slot — execute it as load of $t1 */
                        uint32_t ds_op = (w2 >> 26) & 0x3F;
                        if (ds_op == 0x09) { /* ADDIU in delay slot */
                            uint32_t ds_rs = (w2 >> 21) & 0x1F;
                            uint32_t ds_rt = (w2 >> 16) & 0x1F;
                            int16_t ds_imm = (int16_t)(w2 & 0xFFFF);
                            if (ds_rs == 0) {
                                cpu->gpr[ds_rt] = (uint32_t)(int32_t)ds_imm;
                            }
                        }
                        cpu->pc = target;
                        return;
                    }
                }
            }
        }

        /* Pattern 4: lui rN, hi / addiu|ori rN, rN, lo / jr rN / nop
         * All three instructions must be present. Without the jr check,
         * any function prologue that loads a constant via lui+ori would
         * be misidentified as a trampoline (see 0xBFC3DF90 incident). */
        if (op0 == 0x0F) { /* LUI */
            uint32_t rt0 = (w0 >> 16) & 0x1F;
            uint32_t hi_val = (w0 & 0xFFFF) << 16;
            uint32_t computed = 0;
            int have_target = 0;

            if (op1 == 0x09) { /* ADDIU */
                uint32_t rs1 = (w1 >> 21) & 0x1F;
                uint32_t rt1 = (w1 >> 16) & 0x1F;
                if (rs1 == rt0 && rt1 == rt0) {
                    int16_t lo_val = (int16_t)(w1 & 0xFFFF);
                    computed = hi_val + (uint32_t)(int32_t)lo_val;
                    have_target = 1;
                }
            }
            if (!have_target && op1 == 0x0D) { /* ORI */
                uint32_t rs1 = (w1 >> 21) & 0x1F;
                uint32_t rt1 = (w1 >> 16) & 0x1F;
                if (rs1 == rt0 && rt1 == rt0) {
                    computed = hi_val | (w1 & 0xFFFF);
                    have_target = 1;
                }
            }
            /* Only resolve if w2 is jr rN targeting the same register. */
            if (have_target && (w2 & 0xFC1FFFFF) == 0x00000008) {
                uint32_t jr_rs = (w2 >> 21) & 0x1F;
                if (jr_rs == rt0) {
                    cpu->pc = computed;
                    return;
                }
            }

            /* Pattern 5: BIOS vector dispatch table.
             * lui  rN, hi             w0
             * addiu rN, rN, lo        w1  (base = hi|lo)
             * sll  rM, rM, 2          w2  (index <<= 2)
             * addu rN, rN, rM         w3  (ptr = base + index*4)
             * lw   rN, 0(rN)          w4  (func = *ptr)
             * jr   rN                 w5
             * This is the A0/B0/C0 dispatch pattern the BIOS writes at
             * 0x500+.  rM holds the function number (set by the A0/B0/C0
             * trampoline delay slot before we get here). */
            if (have_target) {
                uint32_t w3 = cpu->read_word(addr + 12);
                uint32_t w4 = cpu->read_word(addr + 16);
                uint32_t w5 = cpu->read_word(addr + 20);

                /* w2: sll rM, rM, 2  (opcode 0, func 0, sa 2) */
                if ((w2 & 0xFC0007FF) == 0x00000080) { /* SLL with sa=2 */
                    uint32_t idx_rt = (w2 >> 16) & 0x1F;
                    uint32_t idx_rd = (w2 >> 11) & 0x1F;
                    /* w3: add/addu rN, rN, rM */
                    if ((w3 & 0xFC0007FE) == 0x00000020) { /* ADD or ADDU */
                        uint32_t a_rs = (w3 >> 21) & 0x1F;
                        uint32_t a_rt = (w3 >> 16) & 0x1F;
                        uint32_t a_rd = (w3 >> 11) & 0x1F;
                        if (a_rs == rt0 && a_rt == idx_rd && a_rd == rt0) {
                            /* w4: lw rN, 0(rN) */
                            if ((w4 & 0xFFFF0000) == (0x8C000000u | ((uint32_t)rt0 << 21) | ((uint32_t)rt0 << 16))) {
                                /* w5: jr rN — or nop then jr rN (load delay slot) */
                                uint32_t jr_word = w5;
                                if (w5 == 0x00000000u) {
                                    jr_word = cpu->read_word(addr + 24);
                                }
                                if ((jr_word & 0xFC1FFFFF) == 0x00000008 && ((jr_word >> 21) & 0x1F) == rt0) {
                                    uint32_t index_val = cpu->gpr[idx_rt];
                                    uint32_t table_addr = computed + (index_val << 2);
                                    uint32_t func_ptr = cpu->read_word(table_addr);
                                    cpu->pc = func_ptr;
                                    return;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    {
        /* Log dispatch miss to file and continue (collect all misses). */
        static FILE* miss_log = NULL;
        static int miss_count = 0;
        if (!miss_log) miss_log = fopen("psx_dispatch_misses.txt", "w");
        if (miss_log) {
            fprintf(miss_log, "0x%08X phys=0x%08X ra=0x%08X t9=0x%08X pc=0x%08X\n",
                    addr, phys, cpu->gpr[31], cpu->gpr[25], cpu->pc);
            fflush(miss_log);
        }
        miss_count++;
        if (miss_count > 100000) {
            fprintf(stderr, "DISPATCH MISS limit reached (%d misses). See psx_dispatch_misses.txt\n", miss_count);
            fflush(stderr);
            if (miss_log) fclose(miss_log);
            exit(1);
        }
        /* Return without executing — function is a no-op. */
        cpu->pc = 0;
    }
}
