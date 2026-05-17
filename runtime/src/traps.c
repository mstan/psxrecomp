/* traps.c — Phase 2 trap handlers.
 *
 * All traps abort with a diagnostic message.
 * Phase 2 expects MMIO abort before any trap fires.
 */

#include "cpu_state.h"
#include "debug_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* Forward declarations from interrupts.c */
int psx_get_in_exception(void);
void psx_exception_longjmp(void);

static void trap_crash(const char* msg) {
    FILE* cf = fopen("psx_crash.txt", "w");
    if (cf) { fprintf(cf, "%s\n", msg); fclose(cf); }
    /* Also dump the structured crash trace with rings + cpu state. */
    extern void psx_crash_trace_dump(const char *reason, void *seh_info);
    psx_crash_trace_dump("trap_crash", NULL);
}

typedef struct ThreadWaitFrame {
    uint32_t tcb;
    jmp_buf jmp;
} ThreadWaitFrame;

static ThreadWaitFrame s_thread_wait_stack[16];
static int s_thread_wait_depth;

static int psx_is_valid_tcb(CPUState* cpu, uint32_t tcb)
{
    uint32_t base = cpu->read_word(0x00000110u);
    uint32_t size = cpu->read_word(0x00000114u);
    if (base == 0 || size == 0) return 0;
    uint32_t offset = tcb - base;
    return offset < size && (offset % 0xC0u) == 0;
}

static uint32_t psx_current_tcb_ptr(CPUState* cpu)
{
    uint32_t tcbh = cpu->read_word(0x00000108u);
    return tcbh ? cpu->read_word(tcbh) : 0;
}

static void psx_set_current_tcb(CPUState* cpu, uint32_t tcb)
{
    uint32_t tcbh = cpu->read_word(0x00000108u);
    if (tcbh) cpu->write_word(tcbh, tcb);
}

/* Save/restore PC ring — diagnoses thread-resume drift.
 *
 * Each thread switch logs the (op, tcb, pc, sp, ra) tuple at save and
 * restore. Read back via debug_server "thread_ctx_ring" to see whether
 * a thread's saved resume_pc matches the one we later restore. If a
 * save/restore pair for the same TCB shows different resume_pc with no
 * intervening progress, the recompiled code never advanced and we have
 * a thread-state corruption signal. */
typedef struct ThreadCtxRingEntry {
    uint32_t seq;
    uint32_t frame;
    uint8_t  op;       /* 0=save 1=restore */
    uint8_t  pad0[3];
    uint32_t tcb;
    uint32_t resume_pc;/* save: passed-in resume_pc; restore: read from TCB+128 */
    uint32_t gpr_29;   /* sp */
    uint32_t gpr_31;   /* ra */
    uint32_t cop0_sr;
    uint32_t cop0_epc;
} ThreadCtxRingEntry;
#define THREAD_CTX_RING_CAP 256u
ThreadCtxRingEntry g_thread_ctx_ring[THREAD_CTX_RING_CAP];
uint64_t g_thread_ctx_ring_seq = 0;

extern uint64_t s_frame_count;

static void thread_ctx_ring_log(CPUState* cpu, uint32_t tcb,
                                uint32_t resume_pc, uint8_t op)
{
    ThreadCtxRingEntry* e = &g_thread_ctx_ring[g_thread_ctx_ring_seq & (THREAD_CTX_RING_CAP - 1u)];
    e->seq        = (uint32_t)g_thread_ctx_ring_seq;
    e->frame      = (uint32_t)s_frame_count;
    e->op         = op;
    e->tcb        = tcb;
    e->resume_pc  = resume_pc;
    e->gpr_29     = cpu->gpr[29];
    e->gpr_31     = cpu->gpr[31];
    e->cop0_sr    = cpu->cop0[12];
    e->cop0_epc   = cpu->cop0[14];
    g_thread_ctx_ring_seq++;
}

static void psx_save_context_to_tcb(CPUState* cpu, uint32_t tcb, uint32_t resume_pc)
{
    uint32_t save = tcb + 8u;
    uint32_t sr = cpu->cop0[12];
    for (int i = 1; i < 32; i++) {
        if (i == 26) continue; /* k0 is the BIOS restore jump register. */
        cpu->write_word(save + (uint32_t)i * 4u, cpu->gpr[i]);
    }
    cpu->write_word(save + 128u, resume_pc);
    cpu->write_word(save + 132u, cpu->hi);
    cpu->write_word(save + 136u, cpu->lo);
    cpu->write_word(save + 140u, (sr & ~0x3Fu) | ((sr & 0x0Fu) << 2));
    cpu->write_word(save + 144u, cpu->cop0[13]);
    thread_ctx_ring_log(cpu, tcb, resume_pc, 0);
    debug_server_log_thread_event(1, cpu, tcb, tcb, resume_pc);
}

static uint32_t psx_restore_context_from_tcb(CPUState* cpu, uint32_t tcb)
{
    uint32_t save = tcb + 8u;
    for (int i = 1; i < 32; i++) {
        if (i == 26) continue;
        cpu->gpr[i] = cpu->read_word(save + (uint32_t)i * 4u);
    }
    cpu->hi = cpu->read_word(save + 132u);
    cpu->lo = cpu->read_word(save + 136u);
    {
        uint32_t saved_sr = cpu->read_word(save + 140u);
        cpu->cop0[12] = (saved_sr & 0xFFFFFFC0u) | ((saved_sr >> 2) & 0x0Fu);
    }
    cpu->cop0[13] = cpu->read_word(save + 144u);
    cpu->gpr[26] = cpu->read_word(save + 128u);
    thread_ctx_ring_log(cpu, tcb, cpu->gpr[26], 1);
    debug_server_log_thread_event(2, cpu, psx_current_tcb_ptr(cpu), tcb, cpu->gpr[26]);
    return cpu->gpr[26];
}

#ifdef _WIN32

typedef struct HostThreadFiber {
    uint32_t tcb;
    LPVOID fiber;
    LPVOID return_fiber;
    uint32_t return_tcb;
    CPUState* cpu;
    int used;
    int owned;
    int closed;
} HostThreadFiber;

static HostThreadFiber s_host_threads[32];
static LPVOID s_main_fiber;

static uint32_t psx_tcb_state(CPUState* cpu, uint32_t tcb)
{
    return psx_is_valid_tcb(cpu, tcb) ? cpu->read_word(tcb) : 0;
}

static LPVOID psx_current_host_fiber(void)
{
    if (!s_main_fiber) {
        s_main_fiber = ConvertThreadToFiber(NULL);
        if (!s_main_fiber) {
            trap_crash("ConvertThreadToFiber failed");
            exit(1);
        }
    }
    return GetCurrentFiber();
}

static HostThreadFiber* psx_find_host_thread(uint32_t tcb)
{
    for (size_t i = 0; i < sizeof(s_host_threads) / sizeof(s_host_threads[0]); i++) {
        if (s_host_threads[i].used && s_host_threads[i].tcb == tcb) {
            return &s_host_threads[i];
        }
    }
    return NULL;
}

static HostThreadFiber* psx_alloc_host_thread(void)
{
    for (size_t i = 0; i < sizeof(s_host_threads) / sizeof(s_host_threads[0]); i++) {
        if (!s_host_threads[i].used) {
            memset(&s_host_threads[i], 0, sizeof(s_host_threads[i]));
            s_host_threads[i].used = 1;
            return &s_host_threads[i];
        }
    }
    trap_crash("BIOS thread fiber table full");
    exit(1);
}

static HostThreadFiber* psx_bind_current_host_thread(CPUState* cpu, uint32_t tcb)
{
    LPVOID fiber = psx_current_host_fiber();
    HostThreadFiber* slot = psx_find_host_thread(tcb);
    if (!slot) {
        slot = psx_alloc_host_thread();
    }
    slot->tcb = tcb;
    slot->fiber = fiber;
    slot->cpu = cpu;
    slot->used = 1;
    slot->owned = (fiber != s_main_fiber);
    slot->closed = 0;
    return slot;
}

static VOID CALLBACK psx_thread_fiber_entry(LPVOID param)
{
    HostThreadFiber* slot = (HostThreadFiber*)param;
    CPUState* cpu = slot->cpu;

    debug_server_log_thread_event(10, cpu, psx_current_tcb_ptr(cpu), slot->tcb, 0);
    psx_set_current_tcb(cpu, slot->tcb);
    uint32_t target_pc = psx_restore_context_from_tcb(cpu, slot->tcb);
    if (target_pc != 0) {
        psx_dispatch(cpu, target_pc);
    }

    debug_server_log_thread_event(11, cpu, psx_current_tcb_ptr(cpu), slot->tcb, target_pc);
    slot->closed = 1;
    if (psx_tcb_state(cpu, slot->tcb) == 0x4000u) {
        cpu->write_word(slot->tcb, 0x1000u);
    }

    if (slot->return_fiber) {
        if (psx_is_valid_tcb(cpu, slot->return_tcb)) {
            psx_set_current_tcb(cpu, slot->return_tcb);
            (void)psx_restore_context_from_tcb(cpu, slot->return_tcb);
        }
        debug_server_log_thread_event(12, cpu, slot->tcb, slot->return_tcb, 0);
        SwitchToFiber(slot->return_fiber);
    }

    trap_crash("BIOS thread fiber returned with no scheduler target");
    exit(1);
}

static HostThreadFiber* psx_get_or_create_host_thread(CPUState* cpu, uint32_t tcb)
{
    HostThreadFiber* slot = psx_find_host_thread(tcb);
    if (psx_tcb_state(cpu, tcb) != 0x4000u) {
        return NULL;
    }

    if (slot && slot->fiber && !slot->closed) {
        return slot;
    }

    if (!slot) {
        slot = psx_alloc_host_thread();
    } else if (slot->fiber && slot->owned && slot->fiber != GetCurrentFiber()) {
        DeleteFiber(slot->fiber);
        slot->fiber = NULL;
    }

    slot->tcb = tcb;
    slot->cpu = cpu;
    slot->return_fiber = NULL;
    slot->return_tcb = 0;
    slot->owned = 1;
    slot->closed = 0;
    slot->fiber = CreateFiber(1024 * 1024, psx_thread_fiber_entry, slot);
    if (!slot->fiber) {
        trap_crash("CreateFiber failed for BIOS thread");
        exit(1);
    }
    return slot;
}

static int psx_change_thread_fiber(CPUState* cpu, uint32_t target_tcb)
{
    uint32_t current_tcb = psx_current_tcb_ptr(cpu);
    debug_server_log_thread_event(3, cpu, current_tcb, target_tcb, 0);
    if (!psx_is_valid_tcb(cpu, current_tcb) || !psx_is_valid_tcb(cpu, target_tcb)) {
        debug_server_log_thread_event(4, cpu, current_tcb, target_tcb, 0);
        return 0;
    }
    if (current_tcb == target_tcb) {
        debug_server_log_thread_event(5, cpu, current_tcb, target_tcb, cpu->gpr[31]);
        cpu->pc = 0;
        return 1;
    }

    HostThreadFiber* current = psx_bind_current_host_thread(cpu, current_tcb);
    if (psx_tcb_state(cpu, current_tcb) == 0x4000u) {
        psx_save_context_to_tcb(cpu, current_tcb, cpu->gpr[31]);
    } else {
        current->closed = 1;
        debug_server_log_thread_event(6, cpu, current_tcb, target_tcb, 0);
    }

    HostThreadFiber* target = psx_get_or_create_host_thread(cpu, target_tcb);
    if (!target) {
        debug_server_log_thread_event(7, cpu, current_tcb, target_tcb, 0);
        return 0;
    }

    target->return_fiber = current->fiber;
    target->return_tcb = current_tcb;

    psx_set_current_tcb(cpu, target_tcb);
    (void)psx_restore_context_from_tcb(cpu, target_tcb);
    debug_server_log_thread_event(8, cpu, current_tcb, target_tcb, 0);
    SwitchToFiber(target->fiber);

    /* If a non-owner fiber requested an exception longjmp while we were
     * suspended, it deferred it by SwitchToFiber'ing back to us (the
     * owner). Honor it now from the correct stack. */
    extern void* g_exception_owner_fiber;
    extern int   g_pending_exception_longjmp;
    extern jmp_buf exception_jmpbuf;
    if (g_pending_exception_longjmp && GetCurrentFiber() == g_exception_owner_fiber) {
        int code = g_pending_exception_longjmp;
        g_pending_exception_longjmp = 0;
        longjmp(exception_jmpbuf, code);
    }

    debug_server_log_thread_event(9, cpu, target_tcb, current_tcb, 0);
    cpu->pc = 0;
    return 1;
}

#endif

static int psx_change_thread_setjmp(CPUState* cpu, uint32_t target_tcb)
{
    uint32_t current_tcb = psx_current_tcb_ptr(cpu);
    if (!psx_is_valid_tcb(cpu, current_tcb) || !psx_is_valid_tcb(cpu, target_tcb)) {
        return 0;
    }
    if (current_tcb == target_tcb) {
        cpu->pc = 0;
        return 1;
    }

    psx_save_context_to_tcb(cpu, current_tcb, cpu->gpr[31]);

    if (s_thread_wait_depth > 0 &&
        s_thread_wait_stack[s_thread_wait_depth - 1].tcb == target_tcb) {
        ThreadWaitFrame *frame = &s_thread_wait_stack[s_thread_wait_depth - 1];
        s_thread_wait_depth--;
        psx_set_current_tcb(cpu, target_tcb);
        (void)psx_restore_context_from_tcb(cpu, target_tcb);
        cpu->pc = 0;
        longjmp(frame->jmp, 1);
    }

    if (s_thread_wait_depth >= (int)(sizeof(s_thread_wait_stack) / sizeof(s_thread_wait_stack[0]))) {
        trap_crash("ChangeThread wait stack overflow");
        exit(1);
    }

    int frame_index = s_thread_wait_depth++;
    ThreadWaitFrame *frame = &s_thread_wait_stack[frame_index];
    frame->tcb = current_tcb;

    if (setjmp(frame->jmp) != 0) {
        cpu->pc = 0;
        return 1;
    }

    psx_set_current_tcb(cpu, target_tcb);
    uint32_t target_pc = psx_restore_context_from_tcb(cpu, target_tcb);
    if (target_pc != 0) {
        psx_dispatch(cpu, target_pc);
    }

    if (s_thread_wait_depth > frame_index &&
        s_thread_wait_stack[frame_index].tcb == current_tcb) {
        s_thread_wait_depth = frame_index;
    }
    if (psx_current_tcb_ptr(cpu) != current_tcb) {
        psx_set_current_tcb(cpu, current_tcb);
        (void)psx_restore_context_from_tcb(cpu, current_tcb);
    }
    cpu->pc = 0;
    return 1;
}

static int psx_change_thread(CPUState* cpu, uint32_t target_tcb)
{
#ifdef _WIN32
    return psx_change_thread_fiber(cpu, target_tcb);
#else
    return psx_change_thread_setjmp(cpu, target_tcb);
#endif
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

        case 3: { /* ChangeThread / ReturnFromException */
            uint32_t target_tcb = cpu->gpr[5];
            if (!psx_get_in_exception() && psx_change_thread(cpu, target_tcb)) {
                return;
            }

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
        /* Always-on ring buffer of dispatch misses. Queryable via the
         * `unknown_dispatch_log` debug command. Replaces the prior
         * file-based log per CLAUDE.md §3. */
        extern void psx_unknown_dispatch_record(uint32_t addr, uint32_t phys,
                                                 uint32_t ra, uint32_t a0,
                                                 uint32_t a1);
        psx_unknown_dispatch_record(addr, phys, cpu->gpr[31],
                                    cpu->gpr[4], cpu->gpr[5]);

        /* Fail-fast mode: silent no-op masks bugs by leaving stale
         * register state. When PSX_FAIL_FAST_UNKNOWN_DISPATCH=1, dump
         * the crash report and exit so the first miss surfaces full
         * context instead of accumulating 60 silent stale-v0 returns. */
        static int s_fail_fast = -1;
        if (s_fail_fast < 0) {
            const char *e = getenv("PSX_FAIL_FAST_UNKNOWN_DISPATCH");
            s_fail_fast = (e && *e == '1') ? 1 : 0;
        }
        if (s_fail_fast) {
            extern void psx_crash_trace_dump(const char *reason, void *seh_info);
            psx_crash_trace_dump("fail_fast_unknown_dispatch", NULL);
            char msg[256];
            snprintf(msg, sizeof(msg),
                "FAIL-FAST unknown dispatch: addr=0x%08X phys=0x%08X ra=0x%08X "
                "a0=0x%08X a1=0x%08X — see psx_last_run_report.json\n",
                addr, phys, cpu->gpr[31], cpu->gpr[4], cpu->gpr[5]);
            trap_crash(msg);
            exit(1);
        }

        /* Return without executing — function is a no-op. */
        cpu->pc = 0;
    }
}
