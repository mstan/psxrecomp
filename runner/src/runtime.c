#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "psx_runtime.h"
#include "cdrom_stub.h"
#include "spu.h"
#include "automation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <signal.h>

#include "diag_log.h"
#include "game_extras.h"

/* ---------------------------------------------------------------------------
 * PS1 memory regions
 *
 * Main RAM:   0x00000000–0x001FFFFF physical (2MB)
 *             Accessed via KSEG0 (0x80000000–0x801FFFFF) or
 *             KSEG1 (0xA0000000–0xA01FFFFF) or KUSEG (0x00000000–0x001FFFFF)
 *
 * Scratchpad: 0x1F800000–0x1F8003FF physical (1KB fast RAM)
 *             Must be kept SEPARATE from main RAM (NOT the same bytes).
 *
 * I/O ports:  0x1F801000–0x1F801FFF — GPU, CDROM, timers, etc.
 *             Stubbed: reads return 0, writes are ignored.
 *
 * Using addr & 0x1FFFFF for everything aliases scratchpad to main RAM[0],
 * corrupting both. This mapping prevents that.
 * --------------------------------------------------------------------------- */
static uint8_t g_ram[2 * 1024 * 1024];   /* 2MB main RAM */
static uint8_t g_scratch[1024];           /* 1KB scratchpad */

/* Global CPU pointer for watchpoint diagnostics */
CPUState* g_diag_cpu = NULL;

/* Current pad1 button state (active-high), exposed for debug_server.c */
uint16_t g_pad1_state = 0;

/* Scripting VM per-frame opcode counter (reset each VM entry, used to force yield) */
uint32_t g_vm_ops_this_frame = 0;

/* Current CDROM seek position (LBA), set by CdlSeekL intercept */
static uint32_t g_cdrom_lba = 0;

/* Dispatch miss tracking */
#define MAX_DISPATCH_MISS_UNIQUE 256
static uint32_t s_dispatch_miss_count = 0;
static uint32_t s_dispatch_miss_addrs[MAX_DISPATCH_MISS_UNIQUE];
static int      s_dispatch_miss_unique_count = 0;

uint32_t psx_get_dispatch_miss_count(void) {
    return s_dispatch_miss_count;
}

/* ---------------------------------------------------------------------------
 * PS1 BIOS interrupt handler chains
 *
 * SysEnqIntRP(prio, queue_ptr) inserts a handler into chain[prio].
 * SysDeqIntRP(prio, queue_ptr) removes it.
 * fire_interrupt_chain(cpu, prio) walks chain[prio] and calls each handler.
 *
 * Queue struct layout (at game-supplied PS1 address):
 *   +0  next_ptr   [uint32]  — filled by SysEnqIntRP
 *   +4  handler1   [uint32]  — function pointer (called on interrupt)
 *   +8  handler2   [uint32]  — second function pointer (optional)
 *   +C  reserved   [uint32]
 *
 * Chain heads are stored in a C global (game never reads them directly).
 * Priority 2 = CD-ROM (IRQ2), fired after each CdRead batch completes.
 * ---------------------------------------------------------------------------*/
static uint32_t g_int_chains[4] = {0};

/* ---------------------------------------------------------------------------
 * Memory card file I/O — backs B(0x32-0x36) BIOS calls with flat files on disk.
 * Save files are stored in C:/temp/memcard/ named by slot and game path.
 * --------------------------------------------------------------------------- */
/* Real PS1 BIOS allows at most 2 simultaneously open memory card files.
 * open() returns 0 or 1 on success, -1 if both slots are full.
 * Repeated O_CREAT to the SAME file path returns the SAME fd (BIOS slot reuse).
 * O_WRONLY to the same file allocates a second fd (separate write handle). */
#define MEMCARD_MAX_FD 2
typedef struct { FILE* fp; char path[256]; } mc_fd_t;
static mc_fd_t s_mc_fds[MEMCARD_MAX_FD];  /* open file handles; path="" if free */
static int     s_nextfile_remain = 0;    /* remaining blocks after firstfile; set by FUN_B4CC intercept */
static char    s_last_mc_name[21] = {0}; /* last O_CREAT filename (PS1 name part, e.g. BASCUS-94236TOMBA-00) */

static void mc_ensure_dir(void) {
    CreateDirectoryA("C:/temp/memcard", NULL);  /* no-op if exists */
}

/* Parse PS1 path "bu[speed][slot]:\FILENAME" → slot (0/1) and filename.
 * Returns 0 on success, -1 on bad format. */
static int mc_parse_path(const char* path, int* slot_out, char* name_out, int name_max) {
    if (!path || path[0] != 'b' || path[1] != 'u') return -1;
    *slot_out = (path[3] == '1') ? 1 : 0;
    /* skip past ':' and optional backslash */
    const char* p = strchr(path + 4, ':');
    if (p) p++;
    else    p = path + 4;
    if (*p == '\\' || *p == '/') p++;
    int i = 0;
    while (*p && i < name_max - 1) { name_out[i++] = *p++; }
    name_out[i] = '\0';
    return (i > 0) ? 0 : -1;
}

/* ---------------------------------------------------------------------------
 * Frame-gated diagnostic logging — fires only during DIAG_FRAME_START..END
 * --------------------------------------------------------------------------- */
extern uint32_t g_ps1_frame;

/* Per-frame addPrim counter — reset by DrawOTag override, incremented by addPrim override */
uint32_t g_addprim_count = 0;

/* Button-triggered INTERP-CALL trace: set to g_ps1_frame+200 when Circle/Square pressed.
 * While g_ps1_frame < g_attack_trace_end_frame, all overlay→compiled calls are logged. */
uint32_t g_attack_trace_end_frame = 0;


/* ---------------------------------------------------------------------------
 * Long-frame watchdog: detect when a frame takes >2s (stall / infinite loop).
 * psx_present_frame (in main_runner.cpp) resets the frame-start timestamp
 * each frame.  read_word / read_half check it periodically.
 * --------------------------------------------------------------------------- */
static LARGE_INTEGER g_wd_freq  = {0};
static LARGE_INTEGER g_wd_start = {0};
static uint32_t      g_wd_frame = 0;      /* frame number at last reset */
static uint64_t      g_wd_reads = 0;      /* total reads since frame start */
static int           g_wd_fired = 0;      /* already printed for this frame? */

void psx_watchdog_reset(void) {
    if (g_wd_freq.QuadPart == 0) QueryPerformanceFrequency(&g_wd_freq);
    QueryPerformanceCounter(&g_wd_start);
    g_wd_frame = g_ps1_frame;
    g_wd_reads = 0;
    g_wd_fired = 0;

    /* BCA7 per-frame watchpoint — catches compiled code writes.
     * Also logs script VM state (ca04, ec30, script PC) every 200 frames after bca7=1. */
    {
        static uint8_t  s_prev_bca7 = 0xFF;
        static uint32_t s_bca7_set_frame = 0;
        uint8_t cur = g_ram[0x9BCA7];
        if (cur != s_prev_bca7) {
            uint32_t ca04 = 0; memcpy(&ca04, &g_ram[0x9CA04], 4);
            uint8_t  ec30 = g_ram[0x9EC30];
            uint32_t ctx_ptr = 0; memcpy(&ctx_ptr, &g_ram[0x9E458], 4);
            uint16_t script_pc = 0;
            uint32_t reg0 = 0;
            if (ctx_ptr >= 0x80000000u) {
                uint32_t ctx = ctx_ptr - 0x80000000u;
                if (ctx + 0x1194 < sizeof(g_ram)) {
                    memcpy(&script_pc, &g_ram[ctx + 0x8a], 2);
                    memcpy(&reg0, &g_ram[ctx + 0x1190], 4);
                }
            }
            /* [BCA7-CHANGE] — re-enable when debugging zone script state:
            printf("[BCA7-CHANGE] f%u: %u->%u ca04=0x%08X ec30=%u ctx=0x%08X pc=%u reg0=0x%X\n",
                   g_ps1_frame, s_prev_bca7, cur, ca04, ec30, ctx_ptr, script_pc, reg0);
            fflush(stdout); */
            if (cur == 1) s_bca7_set_frame = g_ps1_frame;
            s_prev_bca7 = cur;
        }
        /* Per-frame PC change tracker while bca7==1 */
        {
            static uint16_t s_prev_script_pc = 0xFFFF;
            if (cur == 1 && s_bca7_set_frame > 0) {
                uint16_t script_pc2 = 0;
                memcpy(&script_pc2, &g_ram[0x9EC32], 2);  /* ctx+0x8a = 0x9EBA8+0x8a */
                if (script_pc2 != s_prev_script_pc) {
                    /* [PC-CHANGE] — re-enable when debugging zone script PC:
                    printf("[PC-CHANGE] f%u: %u->%u ec30=%u\n",
                           g_ps1_frame, s_prev_script_pc, script_pc2, g_ram[0x9EC30]);
                    fflush(stdout); */
                    s_prev_script_pc = script_pc2;
                }
            } else if (cur == 0) {
                s_prev_script_pc = 0xFFFF;
            }
        }
        /* Periodic dump while bca7 is stuck at 1.
         * Script context ptr is at g_ram[0x9E458] (value = 0x8009EBA8 = g_ram[0x9EBA8]).
         * PC = ctx+0x8A, reg0 = ctx+0x1190. */
        /* [SCRIPT-STATE] periodic dump while bca7 stuck — use LOG_PER_SEC to re-enable:
        if (cur == 1 && s_bca7_set_frame > 0) {
            uint32_t ca04 = 0; memcpy(&ca04, &g_ram[0x9CA04], 4);
            uint8_t  ec30 = g_ram[0x9EC30];
            uint8_t  bcca = g_ram[0x9BCCA];
            uint8_t  bca2 = g_ram[0x9BCA2];
            uint8_t  bcc8 = g_ram[0x9BCC8];
            uint32_t a539c = 0; memcpy(&a539c, &g_ram[0xA539C], 4);
            uint32_t ctx_ptr = 0; memcpy(&ctx_ptr, &g_ram[0x9E458], 4);
            uint16_t script_pc = 0; uint32_t reg0 = 0;
            if (ctx_ptr >= 0x80000000u) { uint32_t ctx = ctx_ptr - 0x80000000u;
                if (ctx + 0x1194 < sizeof(g_ram)) { memcpy(&script_pc, &g_ram[ctx + 0x8a], 2); memcpy(&reg0, &g_ram[ctx + 0x1190], 4); } }
            uint32_t script_base_ptr = 0; memcpy(&script_base_ptr, &g_ram[0x9C974], 4);
            uint8_t opcode = 0xFF;
            if (script_base_ptr >= 0x80000000u) { uint32_t sbase = script_base_ptr - 0x80000000u;
                if (sbase + script_pc < sizeof(g_ram)) opcode = g_ram[sbase + script_pc]; }
            LOG_PER_SEC("SCRIPT-STATE", "ca04=0x%08X ec30=%u bcca=%u bca2=%u bcc8=%u 539c=0x%02X ctx=0x%08X pc=%u reg0=0x%X sbase=0x%08X op=0x%02X",
                   ca04, ec30, bcca, bca2, bcc8, a539c, ctx_ptr, script_pc, reg0, script_base_ptr, opcode);
        }
        */
    }
}

/* Called from read_word / read_half.  Every 100K reads, check wall clock. */
static void watchdog_check(uint32_t addr, int width) {
    ++g_wd_reads;
    if ((g_wd_reads & 0xFFFFF) != 0) return;  /* check every ~1M reads */
    if (g_wd_freq.QuadPart == 0) return;       /* not initialized yet */
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed = (double)(now.QuadPart - g_wd_start.QuadPart) / (double)g_wd_freq.QuadPart;
    if (elapsed > 2.0) {
        uint32_t ra = g_diag_cpu ? g_diag_cpu->ra : 0;
        uint32_t sp = g_diag_cpu ? g_diag_cpu->sp : 0;
        uint32_t phys = addr & 0x1FFFFFFF;
        uint32_t val = 0;
        if (phys < 0x200000) {
            if (width == 32) memcpy(&val, &g_ram[phys], 4);
            else { uint16_t h; memcpy(&h, &g_ram[phys], 2); val = h; }
        } else if (phys >= 0x1F800000 && phys < 0x1F800400) {
            uint32_t off = phys & 0x3FF;
            if (width == 32) memcpy(&val, &g_scratch[off], 4);
            else { uint16_t h; memcpy(&h, &g_scratch[off], 2); val = h; }
        }
        /* [WATCHDOG] — re-enable when debugging spin loops:
        if (!g_wd_fired || (g_wd_reads & 0x3FFFFF) == 0) {
            printf("[WATCHDOG] f%u  %.1fs  addr=0x%08X val=0x%08X w=%d  ra=0x%08X sp=0x%08X  reads=%llu\n",
                   g_wd_frame, elapsed, addr, val, width,
                   ra, sp, (unsigned long long)g_wd_reads);
            fflush(stdout);
        } */
        g_wd_fired = 1;
    }
}

#define DIAG_FRAME_START 4370
#define DIAG_FRAME_END   4385
/* DIAG window disabled — was freezing game at frame ~4382 during normal play.
 * Re-enable by restoring: (g_ps1_frame >= DIAG_FRAME_START && g_ps1_frame <= DIAG_FRAME_END) */
#define DIAG_ENABLED() 0

/* Entity loop (FUN_8001dfd4) s0/s1 save — protects caller's registers */
static uint32_t g_entity_saved_s0, g_entity_saved_s1;

/* Per-frame DrawOTag stats */
static struct {
    uint32_t ot_entries;
    uint32_t ot_nonempty;
    uint32_t total_words;
    uint32_t poly_cmds;    /* 0x20-0x3F: shaded/textured polygons */
    uint32_t line_cmds;    /* 0x40-0x5F: lines */
    uint32_t rect_cmds;    /* 0x60-0x7F: rectangles/sprites */
    uint32_t fill_cmds;    /* 0x02: fill rect */
    uint32_t env_cmds;     /* 0xE1-0xE6: drawing environment */
    uint32_t misc_cmds;    /* everything else */
} g_dt_stats;

/* Per-frame GP0 environment commands (E3/E4/E5) */
static struct {
    uint32_t last_e3;  /* DrawAreaTopLeft */
    uint32_t last_e4;  /* DrawAreaBottomRight */
    uint32_t last_e5;  /* DrawOffset */
    int seen_e3, seen_e4, seen_e5;
} g_env_stats;

/* Per-frame GP1 display commands */
static struct {
    uint32_t last_gp1_05;  /* DisplayAreaStart */
    uint32_t last_gp1_06;  /* HorizDisplayRange */
    uint32_t last_gp1_07;  /* VertDisplayRange */
    uint32_t last_gp1_08;  /* DisplayMode */
    int seen_05, seen_06, seen_07, seen_08;
} g_disp_stats;

/* GP1 display tracking — called from main_runner.cpp */
void diag_track_gp1(uint32_t cmd) {
    uint8_t op = (uint8_t)(cmd >> 24);
    switch (op) {
        case 0x05:
            g_disp_stats.last_gp1_05 = cmd;
            g_disp_stats.seen_05 = 1;
            if (DIAG_ENABLED()) {
                uint32_t x = cmd & 0x3FF;
                uint32_t y = (cmd >> 10) & 0x1FF;
                printf("[GP1-DIAG] f%u GP1(05h) DisplayAreaStart x=%u y=%u (raw=0x%08X)\n",
                       g_ps1_frame, x, y, cmd);
                fflush(stdout);
            }
            break;
        case 0x06:
            g_disp_stats.last_gp1_06 = cmd;
            g_disp_stats.seen_06 = 1;
            break;
        case 0x07:
            g_disp_stats.last_gp1_07 = cmd;
            g_disp_stats.seen_07 = 1;
            break;
        case 0x08:
            g_disp_stats.last_gp1_08 = cmd;
            g_disp_stats.seen_08 = 1;
            if (DIAG_ENABLED()) {
                printf("[GP1-DIAG] f%u GP1(08h) DisplayMode raw=0x%08X\n",
                       g_ps1_frame, cmd);
                fflush(stdout);
            }
            break;
    }
}

/* Classify a GP0 command byte into a stat bucket */
static void diag_classify_gp0(uint8_t cmd_byte) {
    if (cmd_byte == 0x02)                          g_dt_stats.fill_cmds++;
    else if (cmd_byte >= 0x20 && cmd_byte <= 0x3F) g_dt_stats.poly_cmds++;
    else if (cmd_byte >= 0x40 && cmd_byte <= 0x5F) g_dt_stats.line_cmds++;
    else if (cmd_byte >= 0x60 && cmd_byte <= 0x7F) g_dt_stats.rect_cmds++;
    else if (cmd_byte >= 0xE1 && cmd_byte <= 0xE6) g_dt_stats.env_cmds++;
    else                                           g_dt_stats.misc_cmds++;
}

/* ---------------------------------------------------------------------------
 * MMIO shadow registers — interrupt, DMA, and timer emulation
 * --------------------------------------------------------------------------- */
static uint32_t g_i_stat = 0;          /* 0x1F801070: Interrupt Status */
static uint32_t g_i_mask = 0;          /* 0x1F801074: Interrupt Mask */
static uint32_t g_dpcr   = 0x07654321; /* 0x1F8010F0: DMA Primary Control (default enables) */
static uint32_t g_dicr   = 0;          /* 0x1F8010F4: DMA Interrupt Control */

/* Timer counters (free-running, incremented per read to simulate progression) */
static uint32_t g_timer_count[3]  = {0, 0, 0};  /* current value */
static uint32_t g_timer_mode[3]   = {0, 0, 0};  /* mode register */
static uint32_t g_timer_target[3] = {0, 0, 0};  /* target value */

/* ---------------------------------------------------------------------------
 * MMIO Tracing — logs every hardware register read/write to mmio_trace.log
 * Enable with psx_mmio_trace_enable(1) or --mmio-trace CLI flag.
 * --------------------------------------------------------------------------- */
static int   g_mmio_trace_enabled = 0;
static FILE* g_mmio_trace_file    = NULL;

void psx_mmio_trace_enable(int enable) {
    g_mmio_trace_enabled = enable;
    if (enable && !g_mmio_trace_file) {
        g_mmio_trace_file = fopen("mmio_trace.log", "w");
        if (g_mmio_trace_file) {
            setbuf(g_mmio_trace_file, NULL);  /* unbuffered — crash-safe */
            fprintf(g_mmio_trace_file, "OP,ADDR,VALUE,WIDTH,RA,SP\n");
            printf("[MMIO] Tracing enabled → mmio_trace.log\n");
            fflush(stdout);
        }
    }
}

static void mmio_trace(const char* op, uint32_t addr, uint32_t value,
                       int width) {
    if (!g_mmio_trace_enabled || !g_mmio_trace_file) return;
    uint32_t ra = g_diag_cpu ? g_diag_cpu->ra : 0;
    uint32_t sp = g_diag_cpu ? g_diag_cpu->sp : 0;
    fprintf(g_mmio_trace_file, "%s,0x%08X,0x%08X,%d,0x%08X,0x%08X\n",
            op, addr, value, width, ra, sp);
}

/* Crash handler — uses Windows SEH to catch access violations */
#ifdef _WIN32
#include <windows.h>
static LONG WINAPI crash_exception_handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    const char* name = "EXCEPTION";
    if (code == EXCEPTION_ACCESS_VIOLATION)       name = "ACCESS_VIOLATION";
    else if (code == EXCEPTION_STACK_OVERFLOW)     name = "STACK_OVERFLOW";
    else if (code == EXCEPTION_INT_DIVIDE_BY_ZERO) name = "DIV_BY_ZERO";

    /* Native crash address (x86-64 instruction pointer) */
    uintptr_t native_ip = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    uintptr_t fault_addr = 0;
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        fault_addr = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];

    printf("\n[CRASH] %s (code 0x%08lX) at native IP=0x%p\n", name, code, (void*)native_ip);
    if (fault_addr)
        printf("[CRASH] Faulting memory address: 0x%p\n", (void*)fault_addr);

    if (g_diag_cpu) {
        printf("[CRASH] MIPS state:\n");
        printf("[CRASH]  pc=0x%08X ra=0x%08X sp=0x%08X\n",
               g_diag_cpu->pc, g_diag_cpu->ra, g_diag_cpu->sp);
        printf("[CRASH]  v0=0x%08X v1=0x%08X\n",
               g_diag_cpu->v0, g_diag_cpu->v1);
        printf("[CRASH]  a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X\n",
               g_diag_cpu->a0, g_diag_cpu->a1, g_diag_cpu->a2, g_diag_cpu->a3);
        printf("[CRASH]  s0=0x%08X s1=0x%08X s2=0x%08X s3=0x%08X\n",
               g_diag_cpu->s0, g_diag_cpu->s1, g_diag_cpu->s2, g_diag_cpu->s3);
        printf("[CRASH]  s4=0x%08X s5=0x%08X s6=0x%08X s7=0x%08X\n",
               g_diag_cpu->s4, g_diag_cpu->s5, g_diag_cpu->s6, g_diag_cpu->s7);
        printf("[CRASH]  t0=0x%08X t1=0x%08X t2=0x%08X t3=0x%08X\n",
               g_diag_cpu->t0, g_diag_cpu->t1, g_diag_cpu->t2, g_diag_cpu->t3);
        printf("[CRASH]  hi=0x%08X lo=0x%08X gp=0x%08X fp=0x%08X\n",
               g_diag_cpu->hi, g_diag_cpu->lo, g_diag_cpu->gp, g_diag_cpu->fp);
    }
    fflush(stdout);

    if (g_mmio_trace_file) {
        fprintf(g_mmio_trace_file,
                "CRASH,%s,code=0x%08lX,native_ip=0x%p,fault_addr=0x%p,0x%08X,0x%08X\n",
                name, code, (void*)native_ip, (void*)fault_addr,
                g_diag_cpu ? g_diag_cpu->ra : 0,
                g_diag_cpu ? g_diag_cpu->sp : 0);
        if (g_diag_cpu) {
            fprintf(g_mmio_trace_file,
                    "CRASH_REGS,pc=0x%08X,v0=0x%08X,v1=0x%08X,a0=0x%08X,a1=0x%08X,a2=0x%08X,a3=0x%08X\n",
                    g_diag_cpu->pc, g_diag_cpu->v0, g_diag_cpu->v1,
                    g_diag_cpu->a0, g_diag_cpu->a1, g_diag_cpu->a2, g_diag_cpu->a3);
            fprintf(g_mmio_trace_file,
                    "CRASH_REGS,s0=0x%08X,s1=0x%08X,s2=0x%08X,s3=0x%08X,s4=0x%08X,s5=0x%08X,s6=0x%08X,s7=0x%08X\n",
                    g_diag_cpu->s0, g_diag_cpu->s1, g_diag_cpu->s2, g_diag_cpu->s3,
                    g_diag_cpu->s4, g_diag_cpu->s5, g_diag_cpu->s6, g_diag_cpu->s7);
            fprintf(g_mmio_trace_file,
                    "CRASH_REGS,t0=0x%08X,t1=0x%08X,t2=0x%08X,t3=0x%08X,gp=0x%08X,fp=0x%08X,hi=0x%08X,lo=0x%08X\n",
                    g_diag_cpu->t0, g_diag_cpu->t1, g_diag_cpu->t2, g_diag_cpu->t3,
                    g_diag_cpu->gp, g_diag_cpu->fp, g_diag_cpu->hi, g_diag_cpu->lo);
        }
        fclose(g_mmio_trace_file);
        g_mmio_trace_file = NULL;
    }
    _exit(1);
    return EXCEPTION_EXECUTE_HANDLER; /* unreachable */
}
#endif

void psx_install_crash_handler(void) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_exception_handler);
#else
    signal(SIGSEGV, crash_signal_handler);
    signal(SIGABRT, crash_signal_handler);
#endif
}

/* ---------------------------------------------------------------------------
 * Cooperative threading — Windows Fibers
 *
 * PS1 uses two cooperative threads sharing one CPU:
 *   Thread 1 (main):    calls ChangeThread(2) to yield to display thread
 *   Thread 2 (display): calls ChangeThread(1) to yield back to main thread
 *
 * We implement this with Windows Fibers so each thread has its own native
 * C call stack while sharing the same CPUState*.  MIPS GP registers are
 * saved/restored on every switch so each thread sees its own register file.
 * --------------------------------------------------------------------------- */

/* Number of MIPS GP registers at the start of CPUState (zero..lo = 35) */
#define MIPS_GP_REGS 35

static LPVOID g_fiber_main      = NULL;  /* main thread fiber */
static LPVOID g_fiber_display   = NULL;  /* display thread fiber */
static LPVOID g_fiber_loading   = NULL;  /* loading/CDROM thread fiber */
static LPVOID g_fiber_secondary = NULL;  /* secondary processing thread (TCB[1]) */

/* Saved MIPS GP register banks (one per thread) */
static uint32_t g_main_saved[MIPS_GP_REGS];
static uint32_t g_display_saved[MIPS_GP_REGS];
static uint32_t g_loading_saved[MIPS_GP_REGS];
static uint32_t g_secondary_saved[MIPS_GP_REGS];

/* Loading fiber setup from OpenThread */
static uint32_t g_loading_sp    = 0x801FF400u;  /* initial MIPS SP for loading thread */
static uint32_t g_loading_entry = 0;             /* entry function address */

/* Secondary fiber setup (TCB[1] — e.g. FUN_8001F1C0 game-state processor) */
static uint32_t g_secondary_sp    = 0;
static uint32_t g_secondary_entry = 0;

/* Display fiber entry — starts as FUN_800191E0, switches to FUN_80019844 after first load */
static uint32_t g_display_entry = 0;  /* set by game_get_display_entry() in runtime_init */

/* Display-ready flag: 1 when display fiber has yielded back to main and is
 * waiting to be dispatched again.  The game's main loop writes GPU setup data
 * to 0x801FD800 (overwriting TCB[0]) between scheduler calls.  We use this
 * flag to repair TCB[0] state/handle in the scheduler intercept before the
 * game's scheduler code reads them. */
static int g_display_ready = 0;

/* Display fiber restart flag: set when FUN_800172c4 closes the display thread and
 * stores a new entry at TCB[0]+12.  On real PS1, CloseThread destroys the fiber and
 * the scheduler re-opens it with the new entry.  In our fiber model, we detect this
 * in the yield handler and recreate the fiber on next ChangeThread(2) dispatch. */
static int s_display_needs_restart = 0;

/* Re-entrancy guard: func_80016940 (frame-flip) is called once from our VBlank
 * injection, but also re-entrantly from within its own body via func_8005F1C8.
 * The second call double-flips the toggle (net zero) and wipes OT primitives.
 * This flag blocks any re-entrant call. */
static int g_frame_flip_running = 0;

/* Dispatch to compiled game functions — defined in tomba_dispatch.c */
extern int psx_dispatch_compiled(CPUState* cpu, uint32_t addr);

/* Display thread entry function — defined in tomba_full.c */
extern void func_800191E0(CPUState* cpu);

/* Gameplay state handler — needs callee-save wrapper (see 0x8001a954 case) */
extern void func_8001A954(CPUState* cpu);

static VOID WINAPI fiber_display_func(PVOID param) {
    CPUState* cpu = (CPUState*)param;
    /* Set up display thread MIPS stack.
     * The game's OpenThread specifies sp=0x801FE400, but the TCB table sits at
     * 0x801FD800-0x801FD94F — only 0x600 bytes below.  The display state machine
     * (func_800191E0) builds deep call chains that consume >1KB of MIPS stack,
     * which overflows into the TCB area and corrupts thread handles.
     * Fix: start the MIPS SP much higher (0x801FFBF8) so the fiber has ~6KB of
     * headroom before reaching the TCBs.  The loading fiber (sp=0x801FF400) and
     * main fiber (sp≈0x801FFE00) share this region cooperatively (never concurrent),
     * so there is no actual aliasing conflict. */
    cpu->sp = 0x801FFBF8u;
    /* Run display thread — dispatches to current entry (800191E0 or 80019844) */
    psx_dispatch_compiled(cpu, g_display_entry);
    /* Should never return; if it does, park here */
    for (;;) { SwitchToFiber(g_fiber_main); }
}

static VOID WINAPI fiber_loading_func(PVOID param) {
    CPUState* cpu = (CPUState*)param;
    cpu->sp = g_loading_sp;
    /* Dispatch to the loading thread entry (compiled game function) */
    psx_dispatch_compiled(cpu, g_loading_entry);
    /* Should never return; if it does, park here */
    for (;;) { SwitchToFiber(g_fiber_main); }
}

static VOID WINAPI fiber_secondary_func(PVOID param) {
    CPUState* cpu = (CPUState*)param;
    cpu->sp = g_secondary_sp;
    printf("[SECONDARY] fiber started entry=0x%08X sp=0x%08X\n",
           g_secondary_entry, g_secondary_sp);
    fflush(stdout);
    psx_dispatch_compiled(cpu, g_secondary_entry);
    for (;;) { SwitchToFiber(g_fiber_main); }
}

/* Resolve a PS1 virtual address to a pointer into the right region.
 * Returns NULL for I/O ports (caller handles as stub). */
static uint8_t* addr_ptr(uint32_t addr) {
    uint32_t phys = addr & 0x1FFFFFFF;  /* strip KSEG bits */
    if (phys < 0x200000)                 return &g_ram[phys];
    if (phys >= 0x1F800000 && phys < 0x1F800400) return &g_scratch[phys & 0x3FF];
    return NULL;  /* I/O or unmapped */
}

/* ---------------------------------------------------------------------------
 * Memory access helpers
 * --------------------------------------------------------------------------- */
static uint32_t read_word(uint32_t addr) {
    watchdog_check(addr, 32);
    /* Heartbeat + spin detector */
    {
        static uint64_t s_rw_count = 0;
        static uint32_t s_last_addr = 0;
        static uint32_t s_repeat = 0;
        ++s_rw_count;
        /* Heartbeat: log every 500K reads so we can tell if the process is alive */
        if (g_mmio_trace_enabled && g_mmio_trace_file && (s_rw_count % 500000u) == 0) {
            uint32_t ra = g_diag_cpu ? g_diag_cpu->ra : 0;
            uint32_t sp = g_diag_cpu ? g_diag_cpu->sp : 0;
            fprintf(g_mmio_trace_file, "HEARTBEAT,0x%08X,0x%08X,%llu,0x%08X,0x%08X\n",
                    addr, 0, (unsigned long long)s_rw_count, ra, sp);
        }
        if (addr == s_last_addr) {
            ++s_repeat;
            if (s_repeat == 1000000u) {
                uint32_t ra = g_diag_cpu ? g_diag_cpu->ra : 0;
                uint32_t sp = g_diag_cpu ? g_diag_cpu->sp : 0;
                uint8_t* pp = addr_ptr(addr);
                uint32_t val = 0;
                if (pp) memcpy(&val, pp, 4);
                printf("[SPIN] read_word(0x%08X) called 1M times, val=0x%08X  ra=0x%08X sp=0x%08X total=%llu\n",
                       addr, val, ra, sp, (unsigned long long)s_rw_count);
                fflush(stdout);
                if (g_mmio_trace_enabled && g_mmio_trace_file) {
                    fprintf(g_mmio_trace_file, "SPIN,0x%08X,0x%08X,32,0x%08X,0x%08X\n",
                            addr, val, ra, sp);
                }
                s_repeat = 0;  /* reset for next million */
            }
        } else {
            s_last_addr = addr;
            s_repeat = 0;
        }
    }
    uint8_t* p = addr_ptr(addr);
    if (!p) {
        uint32_t phys = addr & 0x1FFFFFFF;
        uint32_t val = 0;
        /* GPU DATA register (GPUREAD) — return next word from the VRAM-to-CPU buffer.
         * Used by CPU poll reads between C0h VRAMToCPU command and DMA transfer.
         * Without this, reads return 0 and the game saves/restores wrong pixel data. */
        if (phys == 0x1F801810u) { extern uint32_t gpu_read_word(void); val = gpu_read_word(); }
        /* GPU STATUS register — report "ready for DMA and command" always.
         * Bit 26 = DMA ready, Bit 27 = VRAM-to-CPU ready, Bit 28 = GP0 cmd ready.
         * Bit 19 = ready to receive DMA block. */
        else if (phys == 0x1F801814u) val = 0x1C080000u;
        /* Interrupt Status / Mask registers */
        else if (phys == 0x1F801070u) val = g_i_stat;
        else if (phys == 0x1F801074u) val = g_i_mask;
        /* DMA Primary Control / Interrupt Control */
        else if (phys == 0x1F8010F0u) val = g_dpcr;
        else if (phys == 0x1F8010F4u) val = g_dicr;
        /* Timer registers — free-running counters that increment each read */
        else if (phys == 0x1F801100u) val = g_timer_count[0]++ & 0xFFFF;
        else if (phys == 0x1F801104u) val = g_timer_mode[0];
        else if (phys == 0x1F801108u) val = g_timer_target[0];
        else if (phys == 0x1F801110u) val = g_timer_count[1]++ & 0xFFFF;
        else if (phys == 0x1F801114u) val = g_timer_mode[1];
        else if (phys == 0x1F801118u) val = g_timer_target[1];
        else if (phys == 0x1F801120u) val = g_timer_count[2]++ & 0xFFFF;
        else if (phys == 0x1F801124u) val = g_timer_mode[2];
        else if (phys == 0x1F801128u) val = g_timer_target[2];
        /* DMA channel CHCR registers — return 0 (busy bit clear = transfer done) */
        /* val already 0 for CHCR and all unhandled addresses */
        mmio_trace("R", addr, val, 32);
        return val;
    }
    uint32_t v; memcpy(&v, p, 4); return v;
}
/* DMA channel 2 (GPU) register shadow — populated by write_word MMIO intercepts */
static uint32_t s_dma2_madr = 0;   /* 0x1F8010A0: source base address */
/* (g_draw_buf_full removed — was too aggressive, blocked all entity rendering) */
static uint32_t s_dma2_bcr  = 0;   /* 0x1F8010A4: block count / size   */

static void write_word(uint32_t addr, uint32_t value) {
    uint8_t* p = addr_ptr(addr);
    uint32_t phys = addr & 0x1FFFFFFFu;

    if (p) {
        /* FUN_8003ef50 (LZ decompressor) start: first thing it writes is scratchpad 0x70 (decompressed size).
         * At this moment scratchpad 0x288 still holds the table pointer → follow chain to find src. */
        /* [DECOMP-START] decompressor watchpoint — re-enable with LOG_FIRST_N to debug zone loads:
        if (phys == 0x1F800070u) {
            uint32_t tbl_ptr = 0, src_ptr = 0;
            memcpy(&tbl_ptr, &g_scratch[0x288], 4);
            if ((tbl_ptr >> 24) == 0x80u) { uint32_t off = (tbl_ptr & 0x1FFFFFu) + 4u;
                if (off + 4u <= sizeof(g_ram)) memcpy(&src_ptr, &g_ram[off], 4); }
            LOG_FIRST_N(10, "DECOMP-START", "decomp_size=0x%08X tbl=0x%08X src=0x%08X", value, tbl_ptr, src_ptr);
        }
        */
        /* Watchpoint: UI entry[3-9] corruption tracker (word writes), post-f1673 only */
        /* [UI-WW] UI entry corruption tracker — re-enable when debugging UI:
        if (phys >= 0x0A567Cu && phys < 0x0A5800u && g_ps1_frame > 1673u && DIAG_ENABLED()) {
            uint32_t ra = g_diag_cpu ? g_diag_cpu->ra : 0;
            static uint32_t _ui_ww_cnt = 0;
            if (++_ui_ww_cnt <= 500) {
                printf("[UI-WW] addr=0x%08X val=0x%08X f%u ra=0x%08X\n",
                       addr, value, g_ps1_frame, ra);
                fflush(stdout);
            }
        } */
        /* [KERN-WW] kernel-area write watchpoint — result: NO writes to 0x0000-0x7FFF
         * during save sequence.  BIOS MC buffer is not in low kernel RAM.
         * Re-enable: remove the comment-out below and set frame window as needed.
        if (phys < 0x8000u && g_ps1_frame >= 3400u && g_ps1_frame <= 3510u) {
            uint32_t ra = g_diag_cpu ? g_diag_cpu->ra : 0;
            static uint32_t _kern_cnt = 0;
            if (++_kern_cnt <= 2000)
                printf("[KERN-WW] addr=0x%08X val=0x%08X f%u ra=0x%08X\n",
                       addr, value, g_ps1_frame, ra);
        } */
        /* [E1C-WW] entity[0x1C] spawn watchpoint — commented out (re-enable with LOG_FIRST_N) */
        /* [POOL-WW] secondary pool word watchpoint — commented out (re-enable with LOG_FIRST_N) */
        /* [WP-WORD] TCB word watchpoint — commented out (re-enable with LOG_FIRST_N) */
        /* [WP-TOGGLE-W] OT toggle watchpoint — commented out (re-enable with LOG_ON_CHANGE) */
        /* Draw-buffer pointer guard: scratchpad[0x164] holds the current draw
         * buffer write cursor.  GTE depth overflow in FUN_8004a300 can advance
         * this pointer past the valid draw buffer (2×0xC000 bytes starting at
         * 0x800B3188) into TCB memory, causing direct sw/sh corruption.
         * Clamp to the max valid address so the game harmlessly overwrites the
         * tail of the draw buffer instead of trashing the thread control block. */
        if (phys == 0x1F800164u) {
            /* Draw-buffer pointer clamp: prevent the write cursor from
             * overflowing past the valid draw buffer (2×0xC000 bytes at
             * 0x800B3188).  Clamp the pointer so writes are harmless.
             * After 200 consecutive clamps, force s4=1 to terminate the
             * sprite loop (s4 is the loop counter in all sprite rendering
             * functions like FUN_8004a300).  200 is well above any
             * legitimate sprite count but catches corrupt entity data. */
            static uint32_t s_clamp_streak = 0;
            uint32_t phys_val = value & 0x1FFFFFFFu;
            const uint32_t DRAW_BUF_PHYS_MAX = 0x000CB187u;  /* end of toggle-1 buf */
            if (phys_val > DRAW_BUF_PHYS_MAX && phys_val < 0x00200000u) {
                value = (value & 0xE0000000u) | DRAW_BUF_PHYS_MAX;
                if (++s_clamp_streak > 200 && g_diag_cpu) {
                    g_diag_cpu->s4 = 1;
                }
            } else {
                s_clamp_streak = 0;
            }
        }
        /* [SCRPAD-EC] camera target watchpoint — re-enable with LOG_ON_CHANGE(value, "SCRPAD-EC", ...) */
        /* [CAM-E8] camera entity watchpoint — re-enable with LOG_ON_CHANGE(value, "CAM-E8", ...) */
        /* [CAM-WPW] camera scratchpad watchpoint — re-enable with LOG_ON_CHANGE(value, "CAM-WPW", ...) */
        /* [BCCA-WW] zone gate watchpoint — re-enable with LOG_ON_CHANGE(value, "BCCA-WW", ...) */
        memcpy(p, &value, 4);
        return;
    }

    /* ---- MMIO hardware intercepts (addr_ptr returned NULL) ---- */
    extern void gpu_submit_word(uint32_t w);
    mmio_trace("W", addr, value, 32);

    /* Interrupt Status — writing 0 to a bit acknowledges it */
    if (phys == 0x1F801070u) { g_i_stat &= value; return; }
    /* Interrupt Mask — writable */
    if (phys == 0x1F801074u) { g_i_mask = value; return; }
    /* DMA Primary Control */
    if (phys == 0x1F8010F0u) { g_dpcr = value; return; }
    /* DMA Interrupt Control */
    if (phys == 0x1F8010F4u) { g_dicr = value; return; }
    /* Timer registers */
    if (phys == 0x1F801100u) { g_timer_count[0] = value & 0xFFFF; return; }
    if (phys == 0x1F801104u) { g_timer_mode[0] = value; g_timer_count[0] = 0; return; }
    if (phys == 0x1F801108u) { g_timer_target[0] = value & 0xFFFF; return; }
    if (phys == 0x1F801110u) { g_timer_count[1] = value & 0xFFFF; return; }
    if (phys == 0x1F801114u) { g_timer_mode[1] = value; g_timer_count[1] = 0; return; }
    if (phys == 0x1F801118u) { g_timer_target[1] = value & 0xFFFF; return; }
    if (phys == 0x1F801120u) { g_timer_count[2] = value & 0xFFFF; return; }
    if (phys == 0x1F801124u) { g_timer_mode[2] = value; g_timer_count[2] = 0; return; }
    if (phys == 0x1F801128u) { g_timer_target[2] = value & 0xFFFF; return; }

    /* GPU GP0 data port — direct SW to 0x1F801810 */
    if (phys == 0x1F801810u) {
        gpu_submit_word(value);
        return;
    }

    /* GPU GP1 control port — direct SW to 0x1F801814 */
    if (phys == 0x1F801814u) {
        extern void gpu_write_gp1(uint32_t cmd);
        gpu_write_gp1(value);
        return;
    }

    /* DMA channel 2 (GPU) register shadow */
    if (phys == 0x1F8010A0u) { s_dma2_madr = value; return; }  /* MADR */
    if (phys == 0x1F8010A4u) { s_dma2_bcr  = value; return; }  /* BCR  */

    /* DMA channel 4 (SPU) register shadow */
    static uint32_t s_dma4_madr = 0;
    static uint32_t s_dma4_bcr  = 0;
    if (phys == 0x1F8010C0u) { s_dma4_madr = value; return; }  /* SPU DMA MADR */
    if (phys == 0x1F8010C4u) { s_dma4_bcr  = value; return; }  /* SPU DMA BCR  */
    if (phys == 0x1F8010C8u) {                                  /* SPU DMA CHCR */
        uint32_t dir   =  value        & 1u;     /* 1 = RAM→SPU */
        uint32_t start = (value >> 24) & 1u;
        if (start && dir == 1u) {
            uint32_t block_words = s_dma4_bcr & 0xFFFFu;
            uint32_t block_count = (s_dma4_bcr >> 16) & 0xFFFFu;
            uint32_t total_bytes = block_words * block_count * 4u;
            /* [DMA4-SPU] printf("[DMA4-SPU] CHCR=0x%08X madr=0x%08X bcr=0x%08X → %u bytes\n",
               value, s_dma4_madr, s_dma4_bcr, total_bytes); */
            spu_dma_write(s_dma4_madr, total_bytes, g_ram, sizeof(g_ram));
        }
        return;
    }

    /* SPU hardware registers (32-bit writes e.g. ADSR) */
    if (phys >= 0x1F801C00u && phys < 0x1F801E00u) {
        spu_write_word(phys, value);
        return;
    }

    /* DMA channel 2 CHCR — fire when START bit set in block mode, CPU→GPU */
    if (phys == 0x1F8010A8u) {
        uint32_t sync = (value >> 9) & 3u;
        uint32_t dir  =  value       & 1u;
        static uint32_t s_dma2_calls = 0;
        /* [DMA2-CHCR] — re-enable when debugging GPU DMA:
        if ((value & 0x01000000u) && (s_dma2_calls < 5 || DIAG_ENABLED())) {
            printf("[DMA2-CHCR] #%u: value=0x%08X sync=%u dir=%u madr=0x%08X bcr=0x%08X\n",
                   s_dma2_calls + 1, value, sync, dir, s_dma2_madr, s_dma2_bcr);
            fflush(stdout);
        } */
        if ((value & 0x01000000u) && sync == 1u && dir == 1u) {
            /* Block mode, RAM→GPU (dir=1 = to-device): forward every word to the GPU interpreter */
            uint32_t block_size  = s_dma2_bcr & 0xFFFFu;
            uint32_t block_count = (s_dma2_bcr >> 16) & 0xFFFFu;
            uint32_t total = block_size * block_count;
            uint32_t base  = s_dma2_madr & 0x1FFFFFu;  /* physical, word-aligned */
            ++s_dma2_calls;
            if (total > 1024u * 512u) total = 1024u * 512u;  /* sanity cap */
            for (uint32_t i = 0; i < total; i++) {
                uint32_t off = (base + i * 4u) & 0x1FFFFFu;
                if (off + 4u <= sizeof(g_ram)) {
                    uint32_t w; memcpy(&w, g_ram + off, 4);
                    gpu_submit_word(w);
                }
            }
            if (g_mmio_trace_enabled && g_mmio_trace_file) {
                fprintf(g_mmio_trace_file, "DMA2_DONE,0x%08X,0x%08X,%u,0x%08X,0x%08X\n",
                        s_dma2_madr, s_dma2_bcr, total,
                        g_diag_cpu ? g_diag_cpu->ra : 0,
                        g_diag_cpu ? g_diag_cpu->sp : 0);
            }
        } else if ((value & 0x01000000u) && sync == 2u && dir == 1u) {
            /* Linked-list mode, RAM→GPU (dir=1 = to-device): walk the OT chain from MADR */
            ++s_dma2_calls;
            /* Note: linked-list GPU DMA is already handled by the FUN_80060B70
             * override (DrawOTag intercept).  This path is a fallback in case
             * the game triggers the DMA hardware directly via MMIO. */
            uint32_t ptr = s_dma2_madr | 0x80000000u;
            for (int ll_limit = 0; ll_limit < 65536; ll_limit++) {
                uint8_t* ph = addr_ptr(ptr);
                if (!ph) break;
                uint32_t hdr; memcpy(&hdr, ph, 4);
                uint8_t cnt = (uint8_t)(hdr >> 24);
                for (uint8_t wi = 0; wi < cnt; wi++) {
                    uint8_t* pw = addr_ptr(ptr + 4u + wi * 4u);
                    if (pw) { uint32_t w; memcpy(&w, pw, 4); gpu_submit_word(w); }
                }
                uint32_t nxt = hdr & 0xFFFFFFu;
                if (nxt == 0xFFFFFFu || nxt == 0u) break;
                ptr = nxt | 0x80000000u;
            }
        } else if ((value & 0x01000000u) && sync == 1u && dir == 0u) {
            /* Block mode, GPU→RAM (dir=0 = from-device): drain GPUREAD buffer into g_ram.
             * The GPU's GPUREAD buffer was pre-filled by a prior GP0(C0h) VRAMToCPU command.
             * This is how the PS1 saves VRAM regions before opening overlaid menus. */
            extern uint32_t gpu_read_word(void);
            uint32_t block_size  = s_dma2_bcr & 0xFFFFu;
            uint32_t block_count = (s_dma2_bcr >> 16) & 0xFFFFu;
            uint32_t total = block_size * block_count;
            uint32_t base  = s_dma2_madr & 0x1FFFFFu;
            ++s_dma2_calls;
            if (total > 1024u * 512u) total = 1024u * 512u;  /* sanity cap */
            for (uint32_t i = 0; i < total; i++) {
                uint32_t off = (base + i * 4u) & 0x1FFFFFu;
                if (off + 4u <= sizeof(g_ram)) {
                    uint32_t w = gpu_read_word();
                    memcpy(g_ram + off, &w, 4);
                }
            }
        } else if (value & 0x01000000u) {
            /* Unhandled DMA2 mode — log and do nothing */
            ++s_dma2_calls;
            /* [DMA2-IGN] — re-enable when debugging DMA:
            printf("[DMA2-IGN] sync=%u dir=%u value=0x%08X\n", sync, dir, value);
            fflush(stdout); */
        }
        return;
    }
}
static uint16_t read_half(uint32_t addr) {
    watchdog_check(addr, 16);
    /* Spin detector for halfword reads */
    {
        static uint32_t s_last_rh = 0;
        static uint32_t s_rh_rep = 0;
        if (addr == s_last_rh) {
            if (++s_rh_rep == 1000000u) {
                uint32_t ra = g_diag_cpu ? g_diag_cpu->ra : 0;
                uint32_t sp = g_diag_cpu ? g_diag_cpu->sp : 0;
                uint8_t* pp = addr_ptr(addr);
                uint16_t val = 0;
                if (pp) memcpy(&val, pp, 2);
                printf("[SPIN-H] read_half(0x%08X) called 1M times, val=0x%04X  ra=0x%08X\n",
                       addr, (uint32_t)val, ra);
                fflush(stdout);
                if (g_mmio_trace_enabled && g_mmio_trace_file) {
                    fprintf(g_mmio_trace_file, "SPIN,0x%08X,0x%04X,16,0x%08X,0x%08X\n",
                            addr, (uint32_t)val, ra, sp);
                }
                s_rh_rep = 0;
            }
        } else { s_last_rh = addr; s_rh_rep = 0; }
    }
    uint8_t* p = addr_ptr(addr);
    if (!p) {
        uint32_t phys = addr & 0x1FFFFFFFu;
        uint16_t val = 0;
        if (phys >= 0x1F801C00u && phys < 0x1F801E00u)
            val = spu_read_half(addr);
        /* SIO0 registers — log first access to confirm game uses SIO0 for MC */
        /* [SIO0-R] — re-enable when debugging memory card SIO:
        else if (phys >= 0x1F801040u && phys <= 0x1F80105Eu) {
            static uint32_t s_sio_log_cnt = 0;
            if (s_sio_log_cnt < 20) {
                printf("[SIO0-R] phys=0x%08X f%u ra=0x%08X cnt=%u\n",
                       phys, g_ps1_frame,
                       g_diag_cpu ? g_diag_cpu->ra : 0, s_sio_log_cnt);
                if (++s_sio_log_cnt == 20) printf("[SIO0-R] (further SIO0 reads suppressed)\n");
            }
        } */
        else if (phys >= 0x1F801040u && phys <= 0x1F80105Eu) {
            /* SIO0 reads — silenced, see above */
        }
        /* Half-word reads of MMIO registers (lower 16 bits) */
        else if (phys == 0x1F801070u) val = g_i_stat & 0xFFFF;
        else if (phys == 0x1F801074u) val = g_i_mask & 0xFFFF;
        /* Timer half-word reads */
        else if (phys == 0x1F801100u) val = g_timer_count[0]++ & 0xFFFF;
        else if (phys == 0x1F801110u) val = g_timer_count[1]++ & 0xFFFF;
        else if (phys == 0x1F801120u) val = g_timer_count[2]++ & 0xFFFF;
        mmio_trace("R", addr, val, 16);
        return val;
    }
    uint16_t v; memcpy(&v, p, 2);
    /* VBlank simulation for the scratchpad VBlank counter at 0x1F8001E8.
     * On real PS1, the VBlank interrupt fires at 60Hz and increments this
     * counter.  In our recompiler there are no interrupts, so the main fiber
     * can spin on this address forever without ever seeing it increment.
     * Fix: if the game reads scr[1E8] while it is less than scr[1EA] (i.e.,
     * it is waiting for a VBlank), simulate a 60Hz tick using wall-clock time
     * so the spin loop exits within one frame period. */
    if (addr == 0x1F8001E8u) {
        uint16_t ea; memcpy(&ea, &g_scratch[0x1EA], 2);
        if (v < ea) {
            static clock_t s_last_vblank = 0;
            clock_t now = clock();
            if (s_last_vblank == 0) s_last_vblank = now;
            if (now - s_last_vblank >= CLOCKS_PER_SEC / 60) {
                s_last_vblank = now;
                v++;
                memcpy(&g_scratch[0x1E8], &v, 2);
                /* [VBlank] sim tick — re-enable with LOG_FIRST_N(5, "VBlank", "tick#%u", s_vb_count) */
            }
        }
    }
    return v;
}
static void write_half(uint32_t addr, uint16_t value) {
    uint8_t* p = addr_ptr(addr);
    if (p) {
        uint32_t phys = addr & 0x1FFFFFFFu;
        /* [KERN-WH] kernel-area half-word write watchpoint — result: none fired.
         * Re-enable: remove comment-out below.
        if (phys < 0x8000u && g_ps1_frame >= 3400u && g_ps1_frame <= 3510u) {
            uint32_t rah = g_diag_cpu ? g_diag_cpu->ra : 0;
            static uint32_t _kh_cnt = 0;
            if (++_kh_cnt <= 2000)
                printf("[KERN-WH] addr=0x%08X val=0x%04X f%u ra=0x%08X\n",
                       addr, (unsigned)value, g_ps1_frame, rah);
        } */
        /* [SUB/NEXT/GATE/1C6H/1CCH/1F8] scratchpad watchpoints — re-enable when debugging state:
        if (DIAG_ENABLED()) {
            uint32_t ra = g_diag_cpu ? g_diag_cpu->ra : 0;
            if (phys == 0x1FD84Au)   { printf("[SUB]  f%u val=0x%04X ra=0x%08X\n", g_ps1_frame, (uint32_t)value, ra); fflush(stdout); }
            if (phys == 0x1FD84Cu)   { printf("[SUB2] f%u val=0x%04X ra=0x%08X\n", g_ps1_frame, (uint32_t)value, ra); fflush(stdout); }
            if (phys == 0x1FD84Eu)   { printf("[SUB3] f%u val=0x%04X ra=0x%08X\n", g_ps1_frame, (uint32_t)value, ra); fflush(stdout); }
            if (phys == 0x1F8001DCu) { printf("[NEXT] f%u val=0x%04X ra=0x%08X\n", g_ps1_frame, (uint32_t)value, ra); fflush(stdout); }
            if (phys == 0x1F8001CEu) { printf("[GATE] f%u val=0x%04X ra=0x%08X\n", g_ps1_frame, (uint32_t)value, ra); fflush(stdout); }
            if (phys == 0x1F8001C6u) { printf("[1C6H] f%u val=0x%04X ra=0x%08X\n", g_ps1_frame, (uint32_t)value, ra); fflush(stdout); }
            if (phys == 0x1F8001CCu) { printf("[1CCH] f%u val=0x%04X ra=0x%08X\n", g_ps1_frame, (uint32_t)value, ra); fflush(stdout); }
            if (phys == 0x1F8001F8u) { printf("[1F8]  f%u val=0x%04X ra=0x%08X\n", g_ps1_frame, (uint32_t)value, ra); fflush(stdout); }
        } */
        /* [WP-TOGGLE-H] OT toggle — re-enable with LOG_ON_CHANGE((uint32_t)value, "WP-TOGGLE-H", ...) */
        /* [CAM-WP] camera position — re-enable with LOG_ON_CHANGE((uint32_t)value, "CAM-WP", ...) */
        /* [BCA2-WH] player control flag — re-enable with LOG_ON_CHANGE((uint32_t)value, "BCA2-WH", ...) */
        /* [BCCA-WH] zone gate half — re-enable with LOG_ON_CHANGE((uint32_t)value, "BCCA-WH", ...) */
        /* [TERR-WH] terrain pointer — re-enable with LOG_ON_CHANGE((uint32_t)value, "TERR-WH", ...) */
        memcpy(p, &value, 2);
    } else {
        mmio_trace("W", addr, value, 16);
        /* SPU hardware registers 0x1F801C00-0x1F801DFF */
        uint32_t phys = addr & 0x1FFFFFFFu;
        if (phys >= 0x1F801C00u && phys < 0x1F801E00u)
            spu_write_half(addr, value);
    }
}
static uint8_t read_byte(uint32_t addr) {
    uint8_t* p = addr_ptr(addr);
    if (!p) { mmio_trace("R", addr, 0, 8); return 0; }
    /* [E1C-RB] entity[0x1C] read watchpoint — re-enable with LOG_ON_CHANGE(*p, "E1C-RB", ...) */
    /* [E04-RB] entity[0x04] read watchpoint — re-enable with LOG_ON_CHANGE(*p, "E04-RB", ...) */
    return *p;
}
static void write_byte(uint32_t addr, uint8_t value) {
    uint8_t* p = addr_ptr(addr);
    if (p) {
        /* [KERN-WB] kernel-area byte write watchpoint — result: none fired.
         * Re-enable: remove comment-out below.
        uint32_t phys8 = addr & 0x1FFFFFFFu;
        if (phys8 < 0x8000u && g_ps1_frame >= 3400u && g_ps1_frame <= 3510u) {
            uint32_t ra8 = g_diag_cpu ? g_diag_cpu->ra : 0;
            static uint32_t _kb_cnt = 0;
            if (++_kb_cnt <= 2000)
                printf("[KERN-WB] addr=0x%08X val=0x%02X f%u ra=0x%08X\n",
                       addr, (unsigned)value, g_ps1_frame, ra8);
        } */
        *p = value; return;
    }
    mmio_trace("W", addr, value, 8);
    /* SIO0 TX register write — log to see if game uses SIO0 for MC */
    uint32_t phys8 = addr & 0x1FFFFFFFu;
    /* [SIO0-W] — re-enable when debugging memory card SIO:
    if (phys8 >= 0x1F801040u && phys8 <= 0x1F80105Eu) {
        static uint32_t s_sio_w_cnt = 0;
        if (s_sio_w_cnt < 20) {
            printf("[SIO0-W] phys=0x%08X val=0x%02X f%u ra=0x%08X cnt=%u\n",
                   phys8, (unsigned)value, g_ps1_frame,
                   g_diag_cpu ? g_diag_cpu->ra : 0, s_sio_w_cnt);
            if (++s_sio_w_cnt == 20) printf("[SIO0-W] (further SIO0 writes suppressed)\n");
        }
    } */
}
static uint32_t do_lwl(uint32_t addr, uint32_t rt) {
    uint32_t aligned = addr & ~3u;
    uint32_t word; memcpy(&word, &g_ram[aligned & 0x1FFFFF], 4);
    int shift = (addr & 3) * 8;
    uint32_t mask = 0xFFFFFFFFu << shift;
    return (rt & ~mask) | (word << shift);
}
static uint32_t do_lwr(uint32_t addr, uint32_t rt) {
    uint32_t aligned = addr & ~3u;
    uint32_t word; memcpy(&word, &g_ram[aligned & 0x1FFFFF], 4);
    int shift = ((3 - (addr & 3)) * 8);
    uint32_t mask = 0xFFFFFFFFu >> shift;
    return (rt & ~mask) | (word >> shift);
}
static void do_swl(uint32_t addr, uint32_t rt) {
    uint32_t aligned = addr & ~3u;
    uint32_t aphys = aligned & 0x1FFFFFFFu;
    /* [WATCHPOINT] do_swl — re-enable when debugging entity corruption:
    if (aphys == 0x1FD874u) {
        static uint32_t s_swl_wp = 0;
        if (++s_swl_wp <= 10) { printf("[WATCHPOINT] do_swl addr=0x%08X rt=0x%08X\n", addr, rt); fflush(stdout); }
    } */
    uint32_t word; memcpy(&word, &g_ram[aligned & 0x1FFFFF], 4);
    int shift = (addr & 3) * 8;
    uint32_t mask = 0xFFFFFFFFu >> (24 - shift);
    word = (word & ~mask) | (rt >> (24 - shift));
    memcpy(&g_ram[aligned & 0x1FFFFF], &word, 4);
}
static void do_swr(uint32_t addr, uint32_t rt) {
    uint32_t aligned = addr & ~3u;
    uint32_t aphys = aligned & 0x1FFFFFFFu;
    /* [WATCHPOINT] do_swr — re-enable when debugging entity corruption:
    if (aphys == 0x1FD874u) {
        printf("[WATCHPOINT] do_swr addr=0x%08X rt=0x%08X\n", addr, rt); fflush(stdout);
    } */
    uint32_t word; memcpy(&word, &g_ram[aligned & 0x1FFFFF], 4);
    int shift = (addr & 3) * 8;
    uint32_t mask = 0xFFFFFFFFu << shift;
    word = (word & ~mask) | (rt << shift);
    memcpy(&g_ram[aligned & 0x1FFFFF], &word, 4);
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
void psx_runtime_init(CPUState* cpu) {
    g_diag_cpu = cpu;
    memset(cpu, 0, sizeof(*cpu));
    memset(g_ram, 0, sizeof(g_ram));
    memset(g_scratch, 0, sizeof(g_scratch));
    /* Pad active-low idle state: all bits 1 = no buttons pressed.
     * FUN_80028D70 reads this and inverts → 0x0000 = no buttons active-high. */
    g_ram[0x9eb5a] = 0xFF;
    g_ram[0x9eb5b] = 0xFF;
    cpu->read_word  = read_word;
    cpu->write_word = write_word;
    cpu->read_half  = read_half;
    cpu->write_half = write_half;
    cpu->read_byte  = read_byte;
    cpu->write_byte = write_byte;
    cpu->lwl        = do_lwl;
    cpu->lwr        = do_lwr;
    cpu->swl        = do_swl;
    cpu->swr        = do_swr;

    g_display_entry = game_get_display_entry();
}

void psx_runtime_load(uint32_t addr, const uint8_t* data, uint32_t size) {
    uint32_t phys = addr & 0x1FFFFFFF;
    memcpy(&g_ram[phys], data, size);
}

/* ---------------------------------------------------------------------------
 * BIOS heap — simple bump allocator.
 * InitHeap(a0=base, a1=size) sets this up. malloc/calloc/realloc use it.
 * --------------------------------------------------------------------------- */
static uint32_t g_heap_base = 0;
static uint32_t g_heap_size = 0;
static uint32_t g_heap_ptr  = 0;  /* next free PS1 address */

/* ---------------------------------------------------------------------------
 * MIPS Overlay Interpreter
 * Executes MIPS R3000A code from g_ram for dynamically-loaded overlay regions
 * (addresses >= 0x80098000).  Called from call_by_address when the target is
 * not a compiled function.
 * --------------------------------------------------------------------------- */

/* Forward declaration — mips_interpret calls call_by_address for compiled fns */
static void mips_interpret(CPUState* cpu, uint32_t start_pc);

/* Returns 1 if addr is in the statically-compiled region */
static int is_compiled_addr(uint32_t addr) {
    return (addr >= 0x80010000u && addr < 0x80098000u);
}

/* Execute one MIPS instruction (inline, no branch handling).
 * Sets *branch_out and *target_out if the instruction is a branch/jump.
 * Returns 1 if branch/jump, 0 if normal. */
static int mips_exec_one(CPUState* cpu, uint32_t* R[32],
                          uint32_t pc, uint32_t instr,
                          int* is_link_out, int* is_jr31_out,
                          uint32_t* target_out) {
    uint32_t op    = instr >> 26;
    uint32_t rs    = (instr >> 21) & 0x1F;
    uint32_t rt    = (instr >> 16) & 0x1F;
    uint32_t rd    = (instr >> 11) & 0x1F;
    uint32_t shamt = (instr >> 6) & 0x1F;
    uint32_t funct = instr & 0x3F;
    int32_t  simm  = (int32_t)(int16_t)(instr & 0xFFFF);
    uint32_t uimm  = instr & 0xFFFF;

    *is_link_out  = 0;
    *is_jr31_out  = 0;
    *target_out   = 0;

    switch (op) {
    case 0x00: /* SPECIAL */
        switch (funct) {
        case 0x00: *R[rd] = *R[rt] << shamt; break;                     /* SLL  */
        case 0x02: *R[rd] = *R[rt] >> shamt; break;                     /* SRL  */
        case 0x03: *R[rd] = (uint32_t)((int32_t)*R[rt] >> shamt); break;/* SRA  */
        case 0x04: *R[rd] = *R[rt] << (*R[rs] & 31); break;             /* SLLV */
        case 0x06: *R[rd] = *R[rt] >> (*R[rs] & 31); break;             /* SRLV */
        case 0x07: *R[rd] = (uint32_t)((int32_t)*R[rt] >> (*R[rs] & 31)); break; /* SRAV */
        case 0x08: /* JR */
            *target_out  = *R[rs];
            *is_jr31_out = (rs == 31);
            return 1;
        case 0x09: /* JALR */
            *R[rd]       = pc + 8;
            *target_out  = *R[rs];
            *is_link_out = 1;
            return 1;
        case 0x0C: psx_syscall(cpu, (instr >> 6) & 0xFFFFF); break; /* SYSCALL */
        case 0x0D: break; /* BREAK — trap, no-op in recompiler */
        case 0x0F: break; /* SYNC */
        case 0x10: *R[rd] = cpu->hi; break;  /* MFHI */
        case 0x11: cpu->hi = *R[rs]; break;  /* MTHI */
        case 0x12: *R[rd] = cpu->lo; break;  /* MFLO */
        case 0x13: cpu->lo = *R[rs]; break;  /* MTLO */
        case 0x18: { /* MULT */
            int64_t r = (int64_t)(int32_t)*R[rs] * (int64_t)(int32_t)*R[rt];
            cpu->lo = (uint32_t)r; cpu->hi = (uint32_t)(r >> 32); break; }
        case 0x19: { /* MULTU */
            uint64_t r = (uint64_t)*R[rs] * (uint64_t)*R[rt];
            cpu->lo = (uint32_t)r; cpu->hi = (uint32_t)(r >> 32); break; }
        case 0x1A: /* DIV */
            if (*R[rt]) { cpu->lo = (uint32_t)((int32_t)*R[rs]/(int32_t)*R[rt]);
                          cpu->hi = (uint32_t)((int32_t)*R[rs]%(int32_t)*R[rt]); } break;
        case 0x1B: /* DIVU */
            if (*R[rt]) { cpu->lo = *R[rs] / *R[rt]; cpu->hi = *R[rs] % *R[rt]; } break;
        case 0x20: case 0x21: *R[rd] = *R[rs] + *R[rt]; break; /* ADD/ADDU */
        case 0x22: case 0x23: *R[rd] = *R[rs] - *R[rt]; break; /* SUB/SUBU */
        case 0x24: *R[rd] = *R[rs] & *R[rt]; break;  /* AND */
        case 0x25: *R[rd] = *R[rs] | *R[rt]; break;  /* OR  */
        case 0x26: *R[rd] = *R[rs] ^ *R[rt]; break;  /* XOR */
        case 0x27: *R[rd] = ~(*R[rs] | *R[rt]); break; /* NOR */
        case 0x2A: *R[rd] = (int32_t)*R[rs] < (int32_t)*R[rt] ? 1 : 0; break; /* SLT  */
        case 0x2B: *R[rd] = *R[rs] < *R[rt] ? 1 : 0; break;                   /* SLTU */
        default:
            printf("[INTERP] SPECIAL funct=0x%02X at 0x%08X\n", funct, pc);
            fflush(stdout);
        }
        return 0;

    case 0x01: /* REGIMM */
        switch (rt) {
        case 0x00: /* BLTZ */
            *target_out = ((int32_t)*R[rs] < 0) ? (pc + 4 + ((uint32_t)simm << 2)) : (pc + 8);
            return 1;
        case 0x01: /* BGEZ */
            *target_out = ((int32_t)*R[rs] >= 0) ? (pc + 4 + ((uint32_t)simm << 2)) : (pc + 8);
            return 1;
        case 0x10: /* BLTZAL */
            cpu->ra = pc + 8;
            *target_out  = ((int32_t)*R[rs] < 0) ? (pc + 4 + ((uint32_t)simm << 2)) : (pc + 8);
            *is_link_out = (*target_out != (pc + 8)); /* link only if taken */
            return 1;
        case 0x11: /* BGEZAL */
            cpu->ra = pc + 8;
            *target_out  = ((int32_t)*R[rs] >= 0) ? (pc + 4 + ((uint32_t)simm << 2)) : (pc + 8);
            *is_link_out = (*target_out != (pc + 8));
            return 1;
        default:
            printf("[INTERP] REGIMM rt=0x%02X at 0x%08X\n", rt, pc);
            fflush(stdout);
        }
        return 0;

    case 0x02: /* J */
        *target_out = (pc & 0xF0000000u) | ((instr & 0x3FFFFFFu) << 2);
        return 1;
    case 0x03: /* JAL */
        cpu->ra      = pc + 8;
        *target_out  = (pc & 0xF0000000u) | ((instr & 0x3FFFFFFu) << 2);
        *is_link_out = 1;
        return 1;
    case 0x04: /* BEQ */
        *target_out = (*R[rs] == *R[rt]) ? (pc + 4 + ((uint32_t)simm << 2)) : (pc + 8);
        return 1;
    case 0x05: /* BNE */
        *target_out = (*R[rs] != *R[rt]) ? (pc + 4 + ((uint32_t)simm << 2)) : (pc + 8);
        return 1;
    case 0x06: /* BLEZ */
        *target_out = ((int32_t)*R[rs] <= 0) ? (pc + 4 + ((uint32_t)simm << 2)) : (pc + 8);
        return 1;
    case 0x07: /* BGTZ */
        *target_out = ((int32_t)*R[rs] > 0) ? (pc + 4 + ((uint32_t)simm << 2)) : (pc + 8);
        return 1;
    case 0x08: case 0x09: /* ADDI/ADDIU */
        *R[rt] = *R[rs] + (uint32_t)simm; return 0;
    case 0x0A: /* SLTI */
        *R[rt] = ((int32_t)*R[rs] < simm) ? 1 : 0; return 0;
    case 0x0B: /* SLTIU */
        *R[rt] = (*R[rs] < (uint32_t)simm) ? 1 : 0; return 0;
    case 0x0C: *R[rt] = *R[rs] & uimm; return 0; /* ANDI */
    case 0x0D: *R[rt] = *R[rs] | uimm; return 0; /* ORI  */
    case 0x0E: *R[rt] = *R[rs] ^ uimm; return 0; /* XORI */
    case 0x0F: *R[rt] = uimm << 16;    return 0; /* LUI  */
    case 0x10: /* COP0 */
        if (rs >= 0x10 && (funct == 0x10)) {
            /* RFE — restore interrupt enable bits */
            uint32_t sr = cpu->cop0[12];
            cpu->cop0[12] = (sr & 0xFFFFFFF0u) | ((sr >> 2) & 0x0Fu);
        } else if ((rs & 0x1F) == 4) {
            cpu->cop0[rd] = *R[rt];       /* MTC0 */
        } else if ((rs & 0x1F) == 0 && rt) {
            *R[rt] = cpu->cop0[rd]; /* MFC0 */
        }
        return 0;
    case 0x12: /* COP2/GTE */
        if (rs == 0x00 && rt) { /* MFC2 — move from GTE data register */
            *R[rt] = gte_read_data(cpu, (uint8_t)rd);
        } else if (rs == 0x02 && rt) { /* CFC2 — move from GTE control register */
            *R[rt] = gte_read_ctrl(cpu, (uint8_t)rd);
        } else if (rs == 0x04) { /* MTC2 — move to GTE data register */
            gte_write_data(cpu, (uint8_t)rd, *R[rt]);
        } else if (rs == 0x06) { /* CTC2 — move to GTE control register */
            gte_write_ctrl(cpu, (uint8_t)rd, *R[rt]);
        } else if (rs & 0x10) { /* GTE command (bit 25 set) */
            gte_execute(cpu, instr & 0x1FFFFFF);
        }
        return 0;
    case 0x20: if (rt) *R[rt] = (uint32_t)(int8_t)cpu->read_byte(*R[rs]+(uint32_t)simm); return 0; /* LB  */
    case 0x21: if (rt) *R[rt] = (uint32_t)(int16_t)cpu->read_half(*R[rs]+(uint32_t)simm); return 0;/* LH  */
    case 0x22: if (rt) *R[rt] = cpu->lwl(*R[rs]+(uint32_t)simm, *R[rt]); return 0; /* LWL */
    case 0x23: if (rt) *R[rt] = cpu->read_word(*R[rs]+(uint32_t)simm); return 0;    /* LW  */
    case 0x24: if (rt) *R[rt] = (uint32_t)cpu->read_byte(*R[rs]+(uint32_t)simm); return 0; /* LBU */
    case 0x25: if (rt) *R[rt] = (uint32_t)cpu->read_half(*R[rs]+(uint32_t)simm); return 0; /* LHU */
    case 0x26: if (rt) *R[rt] = cpu->lwr(*R[rs]+(uint32_t)simm, *R[rt]); return 0; /* LWR */
    case 0x28: cpu->write_byte(*R[rs]+(uint32_t)simm, (uint8_t)*R[rt]);  return 0; /* SB  */
    case 0x29: cpu->write_half(*R[rs]+(uint32_t)simm, (uint16_t)*R[rt]); return 0; /* SH  */
    case 0x2A: cpu->swl(*R[rs]+(uint32_t)simm, *R[rt]); return 0; /* SWL */
    case 0x2B: cpu->write_word(*R[rs]+(uint32_t)simm, *R[rt]); return 0; /* SW  */
    case 0x2E: cpu->swr(*R[rs]+(uint32_t)simm, *R[rt]); return 0; /* SWR */
    case 0x32: /* LWC2 — load word to GTE data register */
        gte_write_data(cpu, (uint8_t)rt, cpu->read_word(*R[rs]+(uint32_t)simm));
        return 0;
    case 0x3A: /* SWC2 — store word from GTE data register */
        cpu->write_word(*R[rs]+(uint32_t)simm, gte_read_data(cpu, (uint8_t)rt));
        return 0;
    default:
        printf("[INTERP] opcode=0x%02X at 0x%08X instr=0x%08X\n", op, pc, instr);
        fflush(stdout);
        return 0;
    }
}

static void mips_interpret(CPUState* cpu, uint32_t start_pc) {
    /* Register pointer array — builds once per call depth */
    uint32_t zero_sink = 0;
    uint32_t* R[32];
    R[0]  = &zero_sink;  /* $zero — writes discarded */
    R[1]  = &cpu->at;    R[2]  = &cpu->v0;  R[3]  = &cpu->v1;
    R[4]  = &cpu->a0;    R[5]  = &cpu->a1;  R[6]  = &cpu->a2;  R[7]  = &cpu->a3;
    R[8]  = &cpu->t0;    R[9]  = &cpu->t1;  R[10] = &cpu->t2;  R[11] = &cpu->t3;
    R[12] = &cpu->t4;    R[13] = &cpu->t5;  R[14] = &cpu->t6;  R[15] = &cpu->t7;
    R[16] = &cpu->s0;    R[17] = &cpu->s1;  R[18] = &cpu->s2;  R[19] = &cpu->s3;
    R[20] = &cpu->s4;    R[21] = &cpu->s5;  R[22] = &cpu->s6;  R[23] = &cpu->s7;
    R[24] = &cpu->t8;    R[25] = &cpu->t9;  R[26] = &cpu->k0;  R[27] = &cpu->k1;
    R[28] = &cpu->gp;    R[29] = &cpu->sp;  R[30] = &cpu->fp;  R[31] = &cpu->ra;

    static int s_log = 0; ++s_log;
    /* [INTERP] enter — first 8: printf("[INTERP] enter 0x%08X ra=0x%08X\n", start_pc, cpu->ra); */

    /* Trace calls to 0x800022C4 (kernel RAM function called from Tomba tick) */
    /* [0x22C4] kernel RAM function trace — re-enable when debugging tick dispatch:
    if (start_pc == 0x800022C4u) {
        static uint32_t s_22c4 = 0;
        if (++s_22c4 <= 5) {
            uint32_t instr0 = cpu->read_word(0x800022C4u);
            uint32_t instr1 = cpu->read_word(0x800022C8u);
            printf("[0x22C4] #%u f%u first_instr=0x%08X second=0x%08X ra=0x%08X a0=0x%08X\n",
                   s_22c4, g_ps1_frame, instr0, instr1, cpu->ra, cpu->a0);
            fflush(stdout);
        }
    } */

    /* Tomba entity tick (type 0x18) — trace first 20 calls + every 100 + attack window */
    if (start_pc == 0x801139DCu) {
        static uint32_t s_tomba_tick = 0;
        ++s_tomba_tick;
        int in_atk_window = (g_ps1_frame < g_attack_trace_end_frame);
        /* [TOMBA-TICK] first 20 + every 100 + attack window — re-enable when investigating Tomba entity */
    }

    uint32_t pc = start_pc;
    int guard;

    for (guard = 0; guard < 10000; guard++) {
        cpu->zero = 0;
        uint32_t instr = cpu->read_word(pc);
        int  is_link = 0, is_jr31 = 0;
        uint32_t target = 0;

        int is_branch = mips_exec_one(cpu, R, pc, instr, &is_link, &is_jr31, &target);

        if (!is_branch) {
            pc += 4;
            continue;
        }

        /* Branch/jump: always execute delay slot first */
        {
            uint32_t di = cpu->read_word(pc + 4);
            int dl = 0, dj31 = 0; uint32_t dt = 0;
            mips_exec_one(cpu, R, pc + 4, di, &dl, &dj31, &dt);
            /* Ignore any branch in delay slot (undefined MIPS behaviour) */
        }

        /* Null / obviously-invalid target guard.
         * 0xA0/0xB0/0xC0 are BIOS dispatch entries — let them through even though
         * they are below 0x80000000 (handled explicitly after this block).
         * For JR/unconditional: return from this interpreted function.
         * For JALR (is_link): treat the null call as a no-op and continue from
         * pc+8 (the instruction after the delay slot).  Just returning would
         * leave the outer mips_interpret in a bad state (it already set *R[rd]
         * = pc+8 in the JALR case, so we want to continue from there). */
        if (target == 0 || (target < 0x80000000u && target != 0xA0u && target != 0xB0u && target != 0xC0u)) {
            if (is_link) {
                /* [NULL-JALR] — re-enable when debugging null function pointers:
                static uint32_t s_null_jalr = 0;
                if (++s_null_jalr <= 20) {
                    printf("[NULL-JALR] #%u f%u pc=0x%08X ra=0x%08X — null call skipped\n",
                           s_null_jalr, g_ps1_frame, pc, cpu->ra);
                    fflush(stdout);
                } */
                pc = pc + 8;  /* skip past JALR+delay-slot, treat call as no-op */
                continue;
            }
            return;
        }

        /* BIOS dispatch: jr/jalr to 0xA0/0xB0/0xC0 from interpreted BIOS stubs */
        if (target == 0xA0u || target == 0xB0u || target == 0xC0u) {
            call_by_address(cpu, target);
            if (is_link) { pc = pc + 8; continue; }
            return;
        }

        if (is_jr31) {
            /* JR $ra — true function return, back to compiled caller */
            return;
        }

        if (is_link) {
            /* JAL / JALR — cpu->ra already set to pc+8 inside mips_exec_one */
            uint32_t ret_pc = pc + 8;
            if (is_compiled_addr(target)) {
                if ((g_ps1_frame >= 3400u && g_ps1_frame < 4200u) ||
                    (g_attack_trace_end_frame > 0 && g_ps1_frame < g_attack_trace_end_frame)) {
                    /* Deduplicate: log each unique (from, to) pair only once per window.
                     * Without this, thousands of printf calls per frame kill performance. */
                    static struct { uint32_t from; uint32_t to; } s_seen[256];
                    static int      s_seen_n   = 0;
                    static uint32_t s_seen_key = 0;
                    if (s_seen_key != g_attack_trace_end_frame) {
                        s_seen_key = g_attack_trace_end_frame;
                        s_seen_n   = 0;
                    }
                    int dup = 0;
                    for (int _i = 0; _i < s_seen_n; _i++) {
                        if (s_seen[_i].from == pc && s_seen[_i].to == target) { dup = 1; break; }
                    }
                    if (!dup && s_seen_n < 256) {
                        s_seen[s_seen_n].from = pc;
                        s_seen[s_seen_n].to   = target;
                        s_seen_n++;
                        /* [INTERP-CALL] — re-enable when debugging overlay→compiled calls:
                        printf("[INTERP-CALL] f%u 0x%08X → compiled 0x%08X\n", g_ps1_frame, pc, target); */
                    }
                }
                call_by_address(cpu, target);
            } else {
                mips_interpret(cpu, target);
            }
            pc = ret_pc;
        } else {
            /* J / JR $tx — unconditional jump (or fall-through to pc+8) */
            if (target == pc + 8) {
                /* Branch not taken (BEQ/BNE etc.) — fall through after delay slot */
                pc = target;
            } else if (is_compiled_addr(target)) {
                /* Tail call to compiled function */
                if (g_attack_trace_end_frame > 0 && g_ps1_frame < g_attack_trace_end_frame) {
                    static struct { uint32_t from; uint32_t to; } s_jt_seen[64];
                    static int      s_jt_n   = 0;
                    static uint32_t s_jt_key = 0;
                    if (s_jt_key != g_attack_trace_end_frame) { s_jt_key = g_attack_trace_end_frame; s_jt_n = 0; }
                    int jt_dup = 0;
                    for (int _i = 0; _i < s_jt_n; _i++) {
                        if (s_jt_seen[_i].from == pc && s_jt_seen[_i].to == target) { jt_dup = 1; break; }
                    }
                    if (!jt_dup && s_jt_n < 64) {
                        s_jt_seen[s_jt_n].from = pc; s_jt_seen[s_jt_n].to = target; s_jt_n++;
                        /* [INTERP-TAIL] — re-enable when debugging overlay→compiled tail calls:
                        printf("[INTERP-TAIL] f%u 0x%08X → compiled 0x%08X\n", g_ps1_frame, pc, target); */
                    }
                }
                call_by_address(cpu, target);
                return;
            } else {
                /* Local jump within overlay space */
                pc = target;
            }
        }
    }

    /* [INTERP-GUARD] — re-enable when debugging interpreter loop limits:
    static uint32_t s_guard_hit = 0;
    if (++s_guard_hit <= 50) {
        printf("[INTERP-GUARD] #%u guard at PC=0x%08X entry=0x%08X f%u\n",
               s_guard_hit, pc, start_pc, g_ps1_frame);
        fflush(stdout);
    } */
}

/* ---------------------------------------------------------------------------
 * call_by_address — BIOS / dynamic jump handler.
 * Handles PS1 BIOS A(n)/B(n)/C(n) calls and unknown dynamic jumps.
 * See runtime.log for rationale on each implemented function.
 * --------------------------------------------------------------------------- */

/* Dispatch to compiled game functions (generated in tomba_dispatch.c).
 * Returns 1 if the address was a known compiled function, 0 otherwise. */
extern int psx_dispatch_compiled(CPUState* cpu, uint32_t addr);

/* Forward declaration — call_by_address is defined immediately below */
void call_by_address(CPUState* cpu, uint32_t addr);

/* Walk interrupt handler chain for the given priority and call each handler.
 * Equivalent to the PS1 BIOS dispatching an interrupt to registered handlers.
 * Guards against NULL handlers and circular chains (limit=16). */
static void fire_interrupt_chain(CPUState *cpu, uint32_t priority) {
    uint32_t entry = g_int_chains[priority & 3];
    int limit = 16;
    while (entry && --limit >= 0) {
        uint32_t ephys = entry & 0x1FFFFFu;
        if (ephys + 8 > sizeof(g_ram)) break;
        uint32_t handler = 0;
        memcpy(&handler, &g_ram[ephys + 4], 4);
        if (handler && handler != entry) {
            call_by_address(cpu, handler);
        }
        uint32_t next = 0;
        memcpy(&next, &g_ram[ephys], 4);
        if (next == entry) break;  /* guard against self-loop */
        entry = next;
    }
}

void call_by_address(CPUState* cpu, uint32_t addr) {
    /* Normalise KUSEG/KSEG1 addresses to KSEG0 before dispatch.
     * Some game code stores function pointers as KUSEG (no KSEG0 bit).
     * e.g. 0x00014900 → 0x80014900. */
    uint32_t kseg0 = (addr & 0x1FFFFFFFu) | 0x80000000u;

    /* Skip clearly invalid addresses — not BIOS entry (A0/B0/C0), not in
     * compiled range (0x80010000-0x80097FFF), and not in overlay range
     * (0x80098000-0x801FFFFF).  Overlay addresses are executed by the
     * MIPS interpreter at the bottom of this function. */
    if (addr != 0xA0u && addr != 0xB0u && addr != 0xC0u &&
        (kseg0 < 0x80010000u || kseg0 > 0x801FFFFFu)) {
        /* [OOR-CALL] — re-enable when debugging out-of-range calls:
        static uint32_t s_oor = 0;
        if (++s_oor <= 20) {
            printf("[OOR-CALL] #%u skip out-of-range 0x%08X  (ra=0x%08X)\n",
                   s_oor, addr, cpu->ra);
            fflush(stdout);
        } */
        return;
    }

    /* MIPS calling convention enforcement: callee must preserve s0-s7, sp.
     * Some generated functions have bugs where sub-subfunctions corrupt sp,
     * causing the function's own epilogue to read registers from wrong stack
     * locations.  Save/restore all callee-saved registers around dispatch. */
    uint32_t sp_before = cpu->sp;
    uint32_t s0_before = cpu->s0, s1_before = cpu->s1;
    uint32_t s2_before = cpu->s2, s3_before = cpu->s3;
    uint32_t s4_before = cpu->s4, s5_before = cpu->s5;
    uint32_t s6_before = cpu->s6, s7_before = cpu->s7;
    uint32_t fp_before = cpu->fp;

    if (kseg0 != addr && psx_dispatch_compiled(cpu, kseg0)) goto sp_check;

    /* Overlay callback aliases — overlay code sometimes calls main-binary addresses
     * that are off by a few bytes from the compiled function entry point.
     * Map to the correct compiled function. */
    if (addr == 0x800634C4u) { addr = 0x800634C0u; }  /* skip NOP at func entry */

    /* 2-instruction preamble pattern: lui v1,0x800A; lhu v1,-0x4338(v1) = load
     * halfword from 0x8009BCC8 into v1, then fall through to next function.
     * These are entity handler entry points that load a global before the body. */
    if (addr == 0x800338A8u) {
        cpu->v1 = cpu->read_half(0x8009BCC8u);
        if (psx_dispatch_compiled(cpu, 0x800338B0u)) goto sp_check;
        goto sp_check;
    }
    if (addr == 0x80033860u) {
        cpu->v1 = cpu->read_half(0x8009BCC8u);
        if (psx_dispatch_compiled(cpu, 0x80033868u)) goto sp_check;
        goto sp_check;
    }
    if (addr == 0x80033F50u) {
        cpu->v1 = cpu->read_half(0x8009BCC8u);
        if (psx_dispatch_compiled(cpu, 0x80033F58u)) goto sp_check;
        goto sp_check;
    }

    /* 0x8004DEE0: entity handler thunk — JAL func_8004DFA0 with a0=s0, then
     * jumps to switch continuation. We just call the handler and return. */
    if (addr == 0x8004DEE0u) {
        cpu->a0 = cpu->s0;
        if (psx_dispatch_compiled(cpu, 0x8004DFA0u)) goto sp_check;
        goto sp_check;
    }
    /* 0x8004DED0: entity handler thunk — JAL func_8004A300 with a0=s0 */
    if (addr == 0x8004DED0u) {
        cpu->a0 = cpu->s0;
        if (psx_dispatch_compiled(cpu, 0x8004A300u)) goto sp_check;
        goto sp_check;
    }
    /* 0x8004DEF0: entity handler thunk — JAL func_8004E244 with a0=s0 */
    if (addr == 0x8004DEF0u) {
        cpu->a0 = cpu->s0;
        if (psx_dispatch_compiled(cpu, 0x8004E244u)) goto sp_check;
        goto sp_check;
    }

    /* Check if address maps to a compiled function first */
    if (psx_dispatch_compiled(cpu, addr)) goto sp_check;

    uint32_t func = cpu->t1;  /* BIOS function number always in t1 */

    /* --- BIOS Function Table A (addr=0xA0) -------------------------------- */
    if (addr == 0xA0) {
        switch (func) {
            case 0x25: cpu->v0 = (uint8_t)cpu->a0 >= 'a' && (uint8_t)cpu->a0 <= 'z' ? (cpu->a0 - 32) : cpu->a0; return; /* toupper */
            case 0x26: cpu->v0 = (uint8_t)cpu->a0 >= 'A' && (uint8_t)cpu->a0 <= 'Z' ? (cpu->a0 + 32) : cpu->a0; return; /* tolower */
            case 0x2F: cpu->v0 = rand(); return;  /* rand */
            case 0x30: srand(cpu->a0); return;     /* srand */
            case 0x33: {  /* malloc(size) */
                uint32_t size = (cpu->a0 + 3u) & ~3u;  /* align to 4 bytes */
                if (g_heap_ptr + size <= g_heap_base + g_heap_size) {
                    cpu->v0 = g_heap_ptr;
                    g_heap_ptr += size;
                } else {
                    cpu->v0 = 0;  /* out of heap */
                }
                return;
            }
            case 0x34: return;  /* free(ptr) — no-op (bump allocator) */
            case 0x37: {  /* calloc(n, size) */
                uint32_t total = cpu->a0 * cpu->a1;
                uint32_t aligned = (total + 3u) & ~3u;
                if (g_heap_ptr + aligned <= g_heap_base + g_heap_size) {
                    cpu->v0 = g_heap_ptr;
                    memset(&g_ram[g_heap_ptr & 0x1FFFFFFF], 0, aligned);
                    g_heap_ptr += aligned;
                } else {
                    cpu->v0 = 0;
                }
                return;
            }
            case 0x38: {  /* realloc(ptr, size) — simple: allocate new, copy old */
                uint32_t size = (cpu->a1 + 3u) & ~3u;
                if (g_heap_ptr + size <= g_heap_base + g_heap_size) {
                    uint32_t new_ptr = g_heap_ptr;
                    g_heap_ptr += size;
                    if (cpu->a0) memcpy(&g_ram[new_ptr & 0x1FFFFFFF], &g_ram[cpu->a0 & 0x1FFFFFFF], size);
                    cpu->v0 = new_ptr;
                } else {
                    cpu->v0 = 0;
                }
                return;
            }
            case 0x39: {  /* InitHeap(addr, size) */
                g_heap_base = cpu->a0;
                g_heap_size = cpu->a1;
                g_heap_ptr  = cpu->a0;
                return;
            }
            case 0x3C: return;  /* FlushCache — no-op in recompiler */
            case 0x44: return;  /* FlushCache (alternate) — no-op */
            case 0x49: {  /* GPU_cw(cmd) — submit single GPU command word */
                extern void gpu_submit_word(uint32_t w);
                gpu_submit_word(cpu->a0);
                return;
            }
            case 0x70: return;  /* _bu_init — memory card init, no-op */
            case 0x72: return;  /* _96_init — CD-ROM init, no-op */
            case 0x3F: {  /* printf(fmt, ...) — best-effort: just print fmt string */
                if (cpu->a0) {
                    const char* s = (const char*)&g_ram[cpu->a0 & 0x1FFFFFFF];
                    printf("[BIOS printf] %s", s);
                    fflush(stdout);
                }
                return;
            }
            case 0xAB: {
                /* A(0xAB) = game-installed sound flush handler.
                 * During SPU init the game stores FUN_8006E660 at RAM[0x8009761C] and
                 * FUN_8006E660 at RAM[0x8009761C] via FUN_8006FA10/2C.
                 * FUN_8006E460 (the actual A(0xAB) body) reads those pointers and calls them.
                 * We replicate that: call optional callback at [0x80097620], then
                 * the primary flush at [0x8009761C]. */
                uint32_t fn_opt = 0, fn_main = 0;
                memcpy(&fn_opt,  &g_ram[0x97620], 4);
                memcpy(&fn_main, &g_ram[0x9761C],  4);
                if (fn_opt  && (fn_opt  >> 24) == 0x80) psx_dispatch_compiled(cpu, fn_opt);
                if (fn_main && (fn_main >> 24) == 0x80) psx_dispatch_compiled(cpu, fn_main);
                return;
            }
            case 0xAC: {
                /* A(0xAC) = second game-installed sound handler (e.g. voice-off flush).
                 * Pointer stored at RAM[0x80097628]. */
                uint32_t fn = 0;
                memcpy(&fn, &g_ram[0x97628], 4);
                if (fn && (fn >> 24) == 0x80) psx_dispatch_compiled(cpu, fn);
                return;
            }
            default:
                /* [BIOS A()] — re-enable when debugging unknown BIOS A calls:
                printf("[BIOS A(0x%02X)] a0=0x%08X a1=0x%08X\n", func, cpu->a0, cpu->a1);
                fflush(stdout); */
                return;
        }
    }

    /* --- BIOS Function Table B (addr=0xB0) -------------------------------- */
    if (addr == 0xB0) {
        /* [B0] — re-enable when debugging BIOS B calls:
        if (func != 0x0B && func != 0x10) {
            printf("[B0] f%u B(0x%02X) ra=0x%08X\n", g_ps1_frame, func, cpu->ra);
        } */
        switch (func) {
            case 0x08: /* OpenEvent(class,spec,mode,func) → event handle */
                /* Minimal stub: return a non-zero handle so callers don't treat it as error */
                cpu->v0 = 0xFF000000u | (cpu->a0 & 0xFFFFu);
                return;
            case 0x09: cpu->v0 = 1; return;  /* CloseEvent — success */
            case 0x0A: cpu->v0 = 1; return;  /* WaitEvent — return immediately (events always ready) */
            case 0x0B: cpu->v0 = 1; return;  /* TestEvent — return 1 (event fired) */
            case 0x0C: cpu->v0 = 1; return;  /* EnableEvent — success */
            case 0x0D: cpu->v0 = 1; return;  /* DisableEvent — success */
            case 0x0F: cpu->v0 = 1; return;  /* CloseThread — no-op in fiber model */
            case 0x0E: {  /* OpenThread(entry, sp, stksz) → thread handle */
                if (cpu->a0 == 0x800191E0u) {
                    /* Original display entry — fiber already running, no-op */
                    cpu->v0 = 2;
                } else if (cpu->a0 == 0x80021340u) {
                    /* Loading thread entry */
                    if (!g_fiber_loading) {
                        /* First creation */
                        g_loading_entry = cpu->a0;
                        g_loading_sp    = cpu->a1;
                        if (!g_fiber_main)
                            g_fiber_main = ConvertThreadToFiber(NULL);
                        g_fiber_loading = CreateFiber(512 * 1024, fiber_loading_func, (PVOID)cpu);
                    } else {
                        /* Restart — game starting a new loading batch */
                        /* [OpenThread] RESTART — re-enable when debugging fiber lifecycle:
                        printf("[OpenThread] RESTART loading fiber entry=0x%08X sp=0x%08X\n",
                               cpu->a0, cpu->a1);
                        fflush(stdout); */
                        DeleteFiber(g_fiber_loading);
                        g_fiber_loading = NULL;
                        g_loading_entry = cpu->a0;
                        if (cpu->a1 >= 0x80080000u && cpu->a1 <= 0x801FF000u) {
                            g_loading_sp = cpu->a1;
                        } else {
                            /* [OpenThread] WARNING — re-enable when debugging fiber lifecycle:
                            printf("[OpenThread] WARNING: invalid sp=0x%08X, reusing 0x%08X\n",
                                   cpu->a1, g_loading_sp);
                            fflush(stdout); */
                        }
                        memset(g_loading_saved, 0, sizeof(g_loading_saved));
                        g_fiber_loading = CreateFiber(512 * 1024, fiber_loading_func, (PVOID)cpu);
                    }
                    cpu->v0 = 3;
                } else if (GetCurrentFiber() == g_fiber_display ||
                           GetCurrentFiber() == g_fiber_loading) {
                    /* Called from within display/loading fiber — this is a SECONDARY THREAD
                     * (e.g. FUN_80017154(1, FUN_8001F1C0) from the game-logic state machine).
                     * We must NOT delete g_fiber_display here — it's the currently running fiber
                     * and DeleteFiber(self) terminates the thread immediately.
                     * Create a separate secondary fiber; return handle=4. */
                    printf("[OpenThread] SECONDARY THREAD entry=0x%08X sp=0x%08X\n",
                           cpu->a0, cpu->a1);
                    fflush(stdout);
                    g_secondary_entry = cpu->a0;
                    g_secondary_sp = (cpu->a1 >= 0x80080000u && cpu->a1 <= 0x801FF000u)
                                     ? cpu->a1 : 0x801FD000u;
                    memset(g_secondary_saved, 0, sizeof(g_secondary_saved));
                    if (g_fiber_secondary) {
                        DeleteFiber(g_fiber_secondary);
                        g_fiber_secondary = NULL;
                    }
                    if (!g_fiber_main)
                        g_fiber_main = ConvertThreadToFiber(NULL);
                    g_fiber_secondary = CreateFiber(512 * 1024, fiber_secondary_func, (PVOID)cpu);
                    cpu->v0 = 4;  /* handle for secondary thread */
                } else {
                    /* New display entry from scheduler/main fiber (e.g., 0x80019844).
                     * FUN_800172c4 sets TCB[0].state=3 + new entry, the scheduler calls here.
                     * Safe to delete the display fiber — we are in the main fiber. */
                    printf("[OpenThread] DISPLAY SWITCH entry=0x%08X sp=0x%08X\n",
                           cpu->a0, cpu->a1);
                    fflush(stdout);
                    g_display_entry = cpu->a0;
                    if (g_fiber_display) {
                        DeleteFiber(g_fiber_display);
                        g_fiber_display = NULL;
                    }
                    memset(g_display_saved, 0, sizeof(g_display_saved));
                    if (!g_fiber_main)
                        g_fiber_main = ConvertThreadToFiber(NULL);
                    g_fiber_display = CreateFiber(512 * 1024, fiber_display_func, (PVOID)cpu);
                    cpu->v0 = 2;  /* keep handle=2 for display */
                }
                printf("[OpenThread] entry=0x%08X sp=0x%08X → handle=%u\n",
                       cpu->a0, cpu->a1, cpu->v0);
                fflush(stdout);
                return;
            }
            case 0x10: {  /* ChangeThread(handle) — PS1 cooperative threading */
                if (cpu->a0 == 2u) {
                    /* Thread 1 (main) → Thread 2 (display) */
                    g_display_ready = 0;  /* clear flag: display is now running */
                    memcpy(g_main_saved, cpu, MIPS_GP_REGS * sizeof(uint32_t));
                    if (!g_fiber_main)
                        g_fiber_main = ConvertThreadToFiber(NULL);
                    if (s_display_needs_restart && g_fiber_display) {
                        /* FUN_800172c4 closed the display thread and stored a new entry.
                         * Delete old fiber, create fresh one with updated g_display_entry. */
                        /* [FIBER RESTART] printf("[FIBER RESTART] recreating display fiber...\n"); */
                        DeleteFiber(g_fiber_display);
                        g_fiber_display = CreateFiber(512 * 1024, fiber_display_func, (PVOID)cpu);
                        s_display_needs_restart = 0;
                    } else if (!g_fiber_display) {
                        /* First switch: create fiber; it sets display SP itself */
                        g_fiber_display = CreateFiber(512 * 1024, fiber_display_func, (PVOID)cpu);
                    } else {
                        /* Subsequent switches: restore display thread MIPS regs */
                        memcpy(cpu, g_display_saved, MIPS_GP_REGS * sizeof(uint32_t));
                        if (DIAG_ENABLED()) {
                            printf("[CT2-RESTORE] f%u display restore: s0=0x%08X s1=0x%08X s2=0x%08X ra=0x%08X sp=0x%08X\n",
                                   g_ps1_frame, cpu->s0, cpu->s1, cpu->s2, cpu->ra, cpu->sp);
                            fflush(stdout);
                        }
                    }
                    SwitchToFiber(g_fiber_display);
                    /* Main resumes here after display or loading yields back */
                    memcpy(cpu, g_main_saved, MIPS_GP_REGS * sizeof(uint32_t));
                } else if (cpu->a0 == 3u && g_fiber_loading) {
                    /* Yield to loading thread (main or display → loading).
                     * Same pattern as ChangeThread(2) but for the loading fiber. */
                    { static uint32_t s_ct3 = 0; ++s_ct3;
                      /* [CT3] if (s_ct3 <= 10 || s_ct3 % 500 == 0)
                         printf("[CT3] dispatch loading #%u f%u qw=%u qr=%u DAT_ce=%u\n", ...); */
                    }
                    LPVOID cur3 = GetCurrentFiber();
                    if (cur3 == g_fiber_display) {
                        memcpy(g_display_saved, cpu, MIPS_GP_REGS * sizeof(uint32_t));
                    } else {
                        memcpy(g_main_saved, cpu, MIPS_GP_REGS * sizeof(uint32_t));
                    }
                    memcpy(cpu, g_loading_saved, MIPS_GP_REGS * sizeof(uint32_t));
                    SwitchToFiber(g_fiber_loading);
                    /* Loading yields back here */
                    if (cur3 == g_fiber_display) {
                        memcpy(cpu, g_display_saved, MIPS_GP_REGS * sizeof(uint32_t));
                    } else {
                        memcpy(cpu, g_main_saved, MIPS_GP_REGS * sizeof(uint32_t));
                    }
                } else if (cpu->a0 == 4u && g_fiber_secondary) {
                    /* Yield to secondary thread (handle=4).
                     * The scheduler dispatches this when TCB[1].state==2 and TCB[1].handle==4. */
                    LPVOID cur4 = GetCurrentFiber();
                    if (cur4 == g_fiber_display) {
                        memcpy(g_display_saved, cpu, MIPS_GP_REGS * sizeof(uint32_t));
                    } else {
                        memcpy(g_main_saved, cpu, MIPS_GP_REGS * sizeof(uint32_t));
                    }
                    memcpy(cpu, g_secondary_saved, MIPS_GP_REGS * sizeof(uint32_t));
                    SwitchToFiber(g_fiber_secondary);
                    /* Secondary yields back here */
                    if (cur4 == g_fiber_display) {
                        memcpy(cpu, g_display_saved, MIPS_GP_REGS * sizeof(uint32_t));
                    } else {
                        memcpy(cpu, g_main_saved, MIPS_GP_REGS * sizeof(uint32_t));
                    }
                } else if (cpu->a0 == 0u) {
                    /* ChangeThread(0) = PS1 "main thread" handle.
                     * The scheduler calls this when a TCB has handle=0 (e.g. from a
                     * failed/restarted OpenThread).  On real PS1 this would context-switch
                     * to the initial CPU context (which is our main fiber — already running).
                     * We can't SwitchToFiber to ourselves, so just tick the VBlank counter
                     * so the main loop's spin-on-scr[1E8] can exit. */
                    {
                        uint16_t cnt;
                        memcpy(&cnt, &g_scratch[0x1E8], 2); cnt++; memcpy(&g_scratch[0x1E8], &cnt, 2);
                        memcpy(&cnt, &g_scratch[0x1F6], 2); cnt++; memcpy(&g_scratch[0x1F6], &cnt, 2);
                    }
                } else if (cpu->a0 == 1u || cpu->a0 == 0xff000000u) {
                    /* Yield back to main from display or loading thread.
                     * Both ChangeThread(1) and ChangeThread(0xff000000) map here.
                     * FUN_800171d4 uses 0xff000000 as the "back to main" handle. */
                    static uint32_t s_yield_count = 0;
                    static uint32_t s_yield_sp_prev = 0;
                    static uint32_t s_yield_ra_prev = 0;
                    ++s_yield_count;
                    {
                        LPVOID cur_chk = GetCurrentFiber();
                        if (cur_chk == g_fiber_display) {
                            if (s_yield_sp_prev != 0 && cpu->sp != s_yield_sp_prev) {
                                printf("[SP-DRIFT] yield #%u  sp 0x%08X→0x%08X (delta=%d)  ra=0x%08X\n",
                                       s_yield_count, s_yield_sp_prev, cpu->sp,
                                       (int32_t)(cpu->sp - s_yield_sp_prev), cpu->ra);
                                fflush(stdout);
                            } else if (s_yield_count <= 10 || (s_yield_count % 200) == 0) {
                                /* [SP-OK] printf("[SP-OK] yield #%u sp=0x%08X ra=0x%08X\n",
                                   s_yield_count, cpu->sp, cpu->ra); */
                            }
                            s_yield_sp_prev = cpu->sp;
                            s_yield_ra_prev = cpu->ra;
                        }
                    }
                    /* [HEARTBEAT] if (s_yield_count <= 5 || (s_yield_count % 600) == 0)
                       printf("[HEARTBEAT] ChangeThread(main) call #%u\n", s_yield_count); */

                    /* Vblank simulation: scan the TCB table and reset any state=1 (yielded)
                     * entries back to state=2 (ready).  On real PS1 the vblank interrupt
                     * handler does this reset.  We can't rely on scratchpad[0x1D4] because
                     * loading may have been started via SYSCALL(2) rather than FUN_80017024,
                     * leaving 0x1D4 pointing at the wrong TCB.
                     * TCB table: 3 entries at 0x801FD800/0x801FD870/0x801FD8E0, stride 0x70. */
                    {
                        static const uint32_t TCB_BASE   = 0x801FD800u;
                        static const uint32_t TCB_STRIDE = 0x70u;
                        static const int      TCB_COUNT  = 3;
                        for (int i = 0; i < TCB_COUNT; ++i) {
                            uint32_t taddr = TCB_BASE + (uint32_t)i * TCB_STRIDE;
                            uint32_t off   = taddr - 0x80000000u;
                            uint16_t state;
                            memcpy(&state, &g_ram[off], 2);
                            /* Reset state 1 (yielded) or 4 (running/dispatched) → 2 (ready).
                             * The real PS1 scheduler sets state=4 when dispatching a thread.
                             * The VBlank ISR then resets state back to 2.  Since we have no
                             * ISR, we do the reset here when any fiber yields to main. */
                            if (state == 3 && i == 0) {
                                /* State 3 = "needs OpenThread" — FUN_800172c4 set this
                                 * after CloseThread + storing a new entry at TCB+12.
                                 * On real PS1, the scheduler would call OpenThread to
                                 * restart the fiber.  We mark for restart and reset. */
                                uint32_t new_entry;
                                memcpy(&new_entry, &g_ram[off + 12], 4);
                                /* [FIBER RESTART] printf("[FIBER RESTART] TCB[0] state=3...\n"); */
                                g_display_entry = new_entry;
                                s_display_needs_restart = 1;
                                /* Set state=2 (ready) and handle=2 (display) */
                                g_ram[off]     = 2;
                                g_ram[off + 1] = 0;
                                uint32_t disp_handle = 2;
                                memcpy(&g_ram[off + 4], &disp_handle, 4);
                            }
                            else if (state == 1 || state == 4) {
                                g_ram[off]     = 2;        /* state=2 = ready   */
                                g_ram[off + 1] = 0;
                                /* [DIAG] if (s_yield_count <= 5) printf("[DIAG] yield #%u TCB[%d]=0x%08X...\n"); */
                            }
                        }
                    }

                    /* Repair corrupted TCB handles / zombie states.
                     * TCB layout (confirmed by scheduler decompile):
                     *   TCB+0  (uint16): state  (2=ready, 3=needs OpenThread, 4=running)
                     *   TCB+4  (uint32): handle (thread handle for ChangeThread)
                     *   TCB+8  (uint32): sp     (MIPS stack pointer)
                     *   TCB+12 (uint32): entry  (entry function address)
                     *   TCB+16 (uint32): stksz
                     * Known mapping (confirmed from game output):
                     *   TCB[0] at 0x801FD800 → display fiber  entry=0x800191E0 handle=2
                     *   TCB[2] at 0x801FD8E0 → loading fiber  entry=0x80021340 handle=3
                     *   TCB[1] at 0x801FD870 → unused slot (state should never be 2)
                     * If TCB[1].state becomes 2 (corrupted by stack overflow) the scheduler
                     * would dispatch a garbage handle.  We neutralise it by clearing state. */
                    {
                        static const uint32_t TCB_BASE2   = 0x801FD800u;
                        static const uint32_t TCB_STRIDE2 = 0x70u;
                        static const int      TCB_COUNT2  = 3;
                        for (int i = 0; i < TCB_COUNT2; ++i) {
                            uint32_t taddr = TCB_BASE2 + (uint32_t)i * TCB_STRIDE2;
                            uint32_t off   = taddr - 0x80000000u;
                            uint16_t state;
                            memcpy(&state, &g_ram[off], 2);
                            if (state == 2 || state == 4) {
                                uint32_t entry, handle;
                                memcpy(&entry,  &g_ram[off + 12], 4);
                                memcpy(&handle, &g_ram[off +  4], 4);
                                uint32_t correct = 0;
                                if      (entry == 0x800191E0u) correct = 2;  /* display (boot) */
                                else if (entry == 0x80019844u) correct = 2;  /* display (main loop) */
                                else if (entry == 0x80021340u) correct = 3;  /* loading */
                                else if (entry == 0x8001F1C0u) correct = 4;  /* secondary (TCB[1]) */
                                if (correct != 0 && handle != correct) {
                                    /* [TCB REPAIR] printf("[TCB REPAIR] TCB[%d] handle 0x%08X → %u (entry=0x%08X)\n",
                                       i, handle, correct, entry); */
                                    memcpy(&g_ram[off + 4], &correct, 4);
                                } else if (correct == 0 && state == 2 &&
                                           handle != 2u && handle != 3u && handle != 4u) {
                                    /* Unrecognised TCB in ready state with no valid handle —
                                     * corrupted by stack overflow.  Only kill garbage entries. */
                                    /* [TCB KILL] printf("[TCB KILL] TCB[%d] unknown entry=0x%08X...\n", i, entry, handle); */
                                    g_ram[off] = 0;
                                    g_ram[off + 1] = 0;
                                }
                            }
                        }
                    }

                    /* Simulate VBlank: increment scr[1E8] (frame counter) and scr[1F6].
                     * On real PS1, a VBlank interrupt fires at 60Hz and calls the game's
                     * registered callback (func_80017374) which does this increment.
                     * Since we have no interrupts, we simulate it here: every time a fiber
                     * yields back to main == one simulated VBlank occurred. */
                    {
                        uint16_t cnt;
                        memcpy(&cnt, &g_scratch[0x1E8], 2); cnt++; memcpy(&g_scratch[0x1E8], &cnt, 2);
                        memcpy(&cnt, &g_scratch[0x1F6], 2); cnt++; memcpy(&g_scratch[0x1F6], &cnt, 2);
                    }

                    LPVOID cur = GetCurrentFiber();
                    if (cur == g_fiber_loading) {
                        memcpy(g_loading_saved, cpu, MIPS_GP_REGS * sizeof(uint32_t));
                    } else if (cur == g_fiber_secondary) {
                        memcpy(g_secondary_saved, cpu, MIPS_GP_REGS * sizeof(uint32_t));
                    } else if (cur == g_fiber_display) {
                        if (DIAG_ENABLED()) {
                            /* Read FUN_800171d4's saved RA from its stack frame (sp+16) */
                            uint32_t caller_ra = 0;
                            uint32_t sp_phys = cpu->sp & 0x1FFFFFFF;
                            if (sp_phys + 20 < 0x200000)
                                memcpy(&caller_ra, &g_ram[sp_phys + 16], 4);
                            /* Also read one more frame up: caller's caller RA */
                            uint32_t caller2_ra = 0;
                            uint32_t parent_sp = cpu->sp + 24;  /* FUN_800171d4 frame size = 24 */
                            uint32_t psp_phys = parent_sp & 0x1FFFFFFF;
                            /* Read saved RA in parent frame — try offset 24 (typical) */
                            if (psp_phys + 28 < 0x200000)
                                memcpy(&caller2_ra, &g_ram[psp_phys + 24], 4);
                            printf("[CT1-SAVE] f%u display save: s0=0x%08X s1=0x%08X s2=0x%08X ra=0x%08X sp=0x%08X caller_ra=0x%08X caller2_ra=0x%08X\n",
                                   g_ps1_frame, cpu->s0, cpu->s1, cpu->s2, cpu->ra, cpu->sp, caller_ra, caller2_ra);
                            fflush(stdout);
                        }
                        memcpy(g_display_saved, cpu, MIPS_GP_REGS * sizeof(uint32_t));
                        g_frame_flip_running = 0;
                        if (s_display_needs_restart) {
                            /* Display fiber is being destroyed (FUN_800172c4 called CloseThread).
                             * Don't present — no frame was drawn.  Just mark ready so main
                             * fiber will recreate us on next ChangeThread(2). */
                            /* [FIBER RESTART] printf("[FIBER RESTART] display fiber yielded for restart...\n"); */
                        } else {
                            /* Display yielded after calling func_80016940 (frame-flip) itself.
                             * The game calls func_80016940 via a function pointer (indirect JALR)
                             * before yielding — that call does DrawOTag (queues GPU packets),
                             * ClearOTagR (clears new OT), and flips the toggle.  By the time we
                             * arrive here, all GPU packets for this frame are already queued.
                             * Present them now, then reset the re-entrancy guard for next frame. */
                            {
                                extern void psx_present_frame(void);
                                static uint32_t s_present = 0;
                                ++s_present;
                                /* [PRESENT] if (s_present <= 5 || (s_present % 300) == 0)
                                   printf("[PRESENT] frame #%u\n", s_present); */
                                psx_present_frame();
                                /* Frame-gated diagnostic summary */
                                if (DIAG_ENABLED()) {
                                    uint32_t d05x = g_disp_stats.last_gp1_05 & 0x3FF;
                                    uint32_t d05y = (g_disp_stats.last_gp1_05 >> 10) & 0x1FF;
                                    uint32_t e3x = g_env_stats.last_e3 & 0x3FF;
                                    uint32_t e3y = (g_env_stats.last_e3 >> 10) & 0x1FF;
                                    uint32_t e4x = g_env_stats.last_e4 & 0x3FF;
                                    uint32_t e4y = (g_env_stats.last_e4 >> 10) & 0x1FF;
                                    int32_t  e5x = (int32_t)(g_env_stats.last_e5 & 0x7FF);
                                    int32_t  e5y = (int32_t)((g_env_stats.last_e5 >> 11) & 0x7FF);
                                    if (e5x & 0x400) e5x |= (int32_t)0xFFFFF800;
                                    if (e5y & 0x400) e5y |= (int32_t)0xFFFFF800;
                                    printf("[FRAME-DIAG] f%u Display:(%u,%u) DrawArea:(%u,%u)-(%u,%u) Offset:(%d,%d) "
                                           "OT: entries=%u nonempty=%u words=%u  "
                                           "fill=%u poly=%u line=%u rect=%u env=%u misc=%u  "
                                           "ff_running=%d\n",
                                           g_ps1_frame, d05x, d05y, e3x, e3y, e4x, e4y, e5x, e5y,
                                           g_dt_stats.ot_entries, g_dt_stats.ot_nonempty,
                                           g_dt_stats.total_words,
                                           g_dt_stats.fill_cmds, g_dt_stats.poly_cmds,
                                           g_dt_stats.line_cmds, g_dt_stats.rect_cmds,
                                           g_dt_stats.env_cmds, g_dt_stats.misc_cmds,
                                           g_frame_flip_running);
                                    fflush(stdout);
                                    /* Reset env/disp stats for next frame */
                                    memset(&g_env_stats, 0, sizeof(g_env_stats));
                                    memset(&g_disp_stats, 0, sizeof(g_disp_stats));
                                }
                                /* Sound tick: call A(0xAB) equivalent every VBlank.
                                 * On real PS1 the VBlank ISR calls FUN_800746A8 which eventually
                                 * calls A(0xAB) → FUN_8006E460 → FUN_8006E660 (KON/KOFF flush).
                                 * FUN_800746A8 is not compiled, so we replicate what A(0xAB) does:
                                 * call the function pointers installed in RAM by the game. */
                                {
                                    uint32_t fn_opt = 0, fn_main = 0;
                                    memcpy(&fn_opt,  &g_ram[0x97620], 4);
                                    memcpy(&fn_main, &g_ram[0x9761C],  4);
                                    if (fn_opt  && (fn_opt  >> 24) == 0x80) psx_dispatch_compiled(cpu, fn_opt);
                                    if (fn_main && (fn_main >> 24) == 0x80) psx_dispatch_compiled(cpu, fn_main);
                                }
                            }
                        }
                        g_display_ready = 1;
                    }
                    if (g_fiber_main) SwitchToFiber(g_fiber_main);
                    /* Fiber resumes here; cpu regs already restored by whoever switched to us */
                }
                /* Any unrecognised handle = no-op */
                cpu->v0 = 0;
                return;
            }
            /* Memory card file I/O — B(0x32) open, B(0x33) lseek, B(0x34) read,
             * B(0x35) write, B(0x36) close.  Backed by flat files in C:/temp/memcard/. */
            case 0x32: {  /* open(path, flags) → fd or -1 */
                uint32_t pa = cpu->a0 & 0x1FFFFFFFu;
                uint32_t flags = cpu->a1;
                const char* path = (pa < sizeof(g_ram)) ? (const char*)&g_ram[pa] : "";
                /* [OPEN-DUMP] one-shot overlay dump — data collected, no longer needed.
                 * Result: a2=0x0C00 (save data size), ra=0x800E7BDC, overlay at 0x800E7BD0.
                 * Re-enable: remove comment-out below.
                if ((flags & 0x200u) && g_ps1_frame >= 3490u && g_ps1_frame <= 3510u) {
                    static int s_dump_done = 0;
                    if (!s_dump_done) {
                        s_dump_done = 1;
                        printf("[OPEN-DUMP] f%u a0=0x%08X(\"%s\") a1=0x%08X a2=0x%08X a3=0x%08X ra=0x%08X\n",
                               g_ps1_frame, cpu->a0, path, cpu->a1, cpu->a2, cpu->a3, cpu->ra);
                        uint32_t dump_start = (cpu->ra - 0x30u) & 0x1FFFFFFFu;
                        uint32_t dump_end   = (cpu->ra + 0x60u) & 0x1FFFFFFFu;
                        printf("[OPEN-DUMP] overlay code 0x%08X - 0x%08X:\n", dump_start + 0x80000000u, dump_end + 0x80000000u);
                        for (uint32_t i = dump_start; i < dump_end && i < sizeof(g_ram); i += 4) {
                            uint32_t w = g_ram[i] | (g_ram[i+1]<<8) | (g_ram[i+2]<<16) | (g_ram[i+3]<<24);
                            printf("  0x%08X: %08X\n", i + 0x80000000u, w);
                        }
                        fflush(stdout);
                    }
                } */
                int slot = 0;  char name[64] = {0};
                if (mc_parse_path(path, &slot, name, sizeof(name)) < 0) {
                    printf("[MEMCARD open] bad path a0=0x%08X flags=0x%05X\n", cpu->a0, flags);
                    cpu->v0 = (uint32_t)-1; return;
                }
                mc_ensure_dir();
                char filepath[256];
                snprintf(filepath, sizeof(filepath), "C:/temp/memcard/slot%d_%s.bin", slot, name);
                /* flags: bit 9 (0x200) = O_CREAT, bit 1 (0x2) = write, bit 0 (0x1) = read
                 * PS1 O_CREAT = create if not exists, open if exists (no truncation).
                 * Real PS1 BIOS: repeated O_CREAT to the same path returns same fd (slot reuse). */
                int create = (flags & 0x200u) != 0;
                int write  = (flags & 0x002u) != 0;
                /* On O_CREAT: if path already open, return existing fd (PS1 BIOS slot reuse) */
                if (create) {
                    for (int i = 0; i < MEMCARD_MAX_FD; i++) {
                        if (s_mc_fds[i].fp && strcmp(s_mc_fds[i].path, filepath) == 0) {
                            cpu->v0 = (uint32_t)i;
                            printf("[MEMCARD open] fd=%d reuse %s flags=0x%05X ra=0x%08X\n", i, filepath, flags, cpu->ra);
                            return;
                        }
                    }
                }
                /* Find a free FD slot — PS1 BIOS uses 0 and 1 (no stdin/stdout reservation) */
                int fd = -1;
                for (int i = 0; i < MEMCARD_MAX_FD; i++) {
                    if (!s_mc_fds[i].fp) { fd = i; break; }
                }
                if (fd < 0) { cpu->v0 = (uint32_t)-1; return; }
                if (create) {
                    /* Try to open existing file first (r+b), fall back to create (w+b) */
                    s_mc_fds[fd].fp = fopen(filepath, "r+b");
                    if (!s_mc_fds[fd].fp) {
                        /* New file: create and pre-fill 8 KB with zeros so read-back verify returns data */
                        static const char zeros[0x2000];
                        s_mc_fds[fd].fp = fopen(filepath, "w+b");
                        if (s_mc_fds[fd].fp) {
                            fwrite(zeros, 1, sizeof(zeros), s_mc_fds[fd].fp);
                            rewind(s_mc_fds[fd].fp);
                        }
                        /* If this is a "-00" block, also create companion "-01" and "-02" files */
                        int nlen = (int)strlen(name);
                        if (nlen >= 3 && name[nlen-3] == '-' && name[nlen-2] == '0' && name[nlen-1] == '0') {
                            for (int blk = 1; blk <= 2; blk++) {
                                char comp_path[256];
                                snprintf(comp_path, sizeof(comp_path), "C:/temp/memcard/slot%d_%s.bin", slot, name);
                                int clen = (int)strlen(comp_path);
                                comp_path[clen - 5] = '0' + blk;  /* -00.bin → -01.bin / -02.bin */
                                FILE *cfp = fopen(comp_path, "r+b");
                                if (!cfp) {
                                    cfp = fopen(comp_path, "w+b");
                                    if (cfp) {
                                        fwrite(zeros, 1, sizeof(zeros), cfp);
                                        printf("[MEMCARD create] companion %s\n", comp_path);
                                        fclose(cfp);
                                    }
                                } else {
                                    fclose(cfp);
                                }
                            }
                        }
                    }
                } else if (write) {
                    s_mc_fds[fd].fp = fopen(filepath, "r+b");
                    if (!s_mc_fds[fd].fp) s_mc_fds[fd].fp = fopen(filepath, "w+b");
                } else {
                    s_mc_fds[fd].fp = fopen(filepath, "rb");
                }
                if (!s_mc_fds[fd].fp) {
                    printf("[MEMCARD open] FAIL %s flags=0x%05X\n", filepath, flags);
                    cpu->v0 = (uint32_t)-1; return;
                }
                strncpy(s_mc_fds[fd].path, filepath, sizeof(s_mc_fds[fd].path) - 1);
                /* Track the PS1 name for firstfile/nextfile direntry population */
                if (create) strncpy(s_last_mc_name, name, sizeof(s_last_mc_name) - 1);
                cpu->v0 = (uint32_t)fd;
                printf("[MEMCARD open] fd=%d %s flags=0x%05X ra=0x%08X\n", fd, filepath, flags, cpu->ra);
                return;
            }
            case 0x33: {  /* lseek(fd, offset, whence) → new position */
                int fd = (int)cpu->a0;
                int32_t offset = (int32_t)cpu->a1;
                int whence = (int)cpu->a2;
                printf("[MEMCARD lseek] fd=%d offset=%d whence=%d ra=0x%08X\n", fd, offset, whence, cpu->ra);
                if (fd < 0 || fd >= MEMCARD_MAX_FD || !s_mc_fds[fd].fp) { cpu->v0 = (uint32_t)-1; return; }
                int w = (whence == 0) ? SEEK_SET : (whence == 1) ? SEEK_CUR : SEEK_END;
                fseek(s_mc_fds[fd].fp, offset, w);
                cpu->v0 = (uint32_t)ftell(s_mc_fds[fd].fp); return;
            }
            case 0x34: {  /* read(fd, buf, len) → bytes read */
                int fd = (int)cpu->a0;
                uint32_t buf = cpu->a1 & 0x1FFFFFFFu;
                uint32_t len = cpu->a2;
                printf("[MEMCARD read] fd=%d buf=0x%08X len=%u ra=0x%08X\n", fd, cpu->a1, len, cpu->ra);
                if (fd < 0 || fd >= MEMCARD_MAX_FD || !s_mc_fds[fd].fp) { cpu->v0 = (uint32_t)-1; return; }
                if (buf + len > sizeof(g_ram)) { cpu->v0 = (uint32_t)-1; return; }
                size_t n = fread(&g_ram[buf], 1, len, s_mc_fds[fd].fp);
                cpu->v0 = (uint32_t)n; return;
            }
            case 0x35: {  /* write(fd, buf, len) → bytes written */
                int fd = (int)cpu->a0;
                uint32_t buf = cpu->a1 & 0x1FFFFFFFu;
                uint32_t len = cpu->a2;
                /* stdout (fd=1) writes — only if fd=1 is NOT an open memcard file */
                if (fd == 1 && (fd >= MEMCARD_MAX_FD || !s_mc_fds[fd].fp)) {
                    if (buf < sizeof(g_ram) && len > 0) fwrite(&g_ram[buf], 1, len, stdout);
                    cpu->v0 = len; return;
                }
                printf("[MEMCARD write] fd=%d buf=0x%08X len=%u ra=0x%08X\n", fd, cpu->a1, len, cpu->ra);
                if (fd < 0 || fd >= MEMCARD_MAX_FD || !s_mc_fds[fd].fp) { cpu->v0 = (uint32_t)-1; return; }
                if (buf + len > sizeof(g_ram)) { cpu->v0 = (uint32_t)-1; return; }
                size_t n = fwrite(&g_ram[buf], 1, len, s_mc_fds[fd].fp);
                fflush(s_mc_fds[fd].fp);
                cpu->v0 = (uint32_t)n; return;
            }
            case 0x36: {  /* close(fd) → 0 */
                int fd = (int)cpu->a0;
                printf("[MEMCARD close] fd=%d ra=0x%08X\n", fd, cpu->ra);
                if (fd >= 0 && fd < MEMCARD_MAX_FD && s_mc_fds[fd].fp) {
                    fclose(s_mc_fds[fd].fp);
                    s_mc_fds[fd].fp = NULL;
                    s_mc_fds[fd].path[0] = '\0';
                }
                cpu->v0 = 0; return;
            }
            case 0x3D: putchar(cpu->a0 & 0xFF); fflush(stdout); return;  /* putchar */
            case 0x3F: {  /* puts */
                if (cpu->a0) puts((const char*)&g_ram[cpu->a0 & 0x1FFFFFFF]);
                return;
            }
            case 0x07: return;  /* DeliverEvent — no-op (no real event system) */
            case 0x17: return;  /* ReturnFromException — no-op in recompiler */
            case 0x18: return;  /* SetDefaultExitFromException — no-op */
            case 0x19: return;  /* SetCustomExitFromException — no-op */
            case 0x42: printf("[BIOS B(0x42)] firstfile a0=0x%08X a1=0x%08X ra=0x%08X\n", cpu->a0, cpu->a1, cpu->ra); fflush(stdout); cpu->v0 = 0; return; /* firstfile — NULL = no files found */
            case 0x43: /* nextfile — simulate 3-block chain: return a0 for remain>0, then 0.
                         * Update attr and name for blocks 2/3 (remain=2→0xA1, remain=1→0xA2). */
                if (cpu->a0 != 0 && s_nextfile_remain > 0) {
                    --s_nextfile_remain;
                    cpu->v0 = cpu->a0;
                    if (cpu->a0 >= 0x80000000u) {
                        uint32_t _s = cpu->a0 - 0x80000000u;
                        uint32_t attr = (s_nextfile_remain > 0) ? 0xA1u : 0xA2u;
                        *(uint32_t*)&g_ram[_s + 0x14] = attr;
                        *(uint32_t*)&g_ram[_s + 0x1C] = 0u;
                        /* Update name suffix: block2="-01", block3="-02" */
                        if (s_last_mc_name[0]) {
                            strncpy((char*)&g_ram[_s], s_last_mc_name, 20);
                            int nlen = (int)strlen(s_last_mc_name);
                            if (nlen >= 2) {
                                int blk = (s_nextfile_remain > 0) ? 1 : 2;
                                g_ram[_s + nlen - 2] = '0' + (blk / 10);
                                g_ram[_s + nlen - 1] = '0' + (blk % 10);
                            }
                        }
                    }
                } else {
                    s_nextfile_remain = 0;
                    cpu->v0 = 0;
                }
                printf("[BIOS B(0x43)] nextfile a0=0x%08X -> v0=0x%08X remain=%d\n",
                       cpu->a0, cpu->v0, s_nextfile_remain);
                return;
            case 0x47: return;  /* AddCDRomDevice — no-op */
            case 0x4E: printf("[BIOS B(0x4E)] card_write a0=0x%08X a1=0x%08X a2=0x%08X ra=0x%08X\n", cpu->a0, cpu->a1, cpu->a2, cpu->ra); cpu->v0 = (uint32_t)-1; return; /* card_write — not yet implemented */
            case 0x4A: cpu->v0 = 1; return;  /* InitCard — stub success */
            case 0x4B: cpu->v0 = 1; return;  /* StartCard — stub success */
            case 0x4C: cpu->v0 = 1; return;  /* StopCard — stub success */
            case 0x56: /* GetC0Table — return pointer to dummy table in RAM */
                cpu->v0 = 0x80000100u; /* point to zero-filled area near bottom of RAM */
                return;
            case 0x57: { /* GetB0Table — return pointer to B-function jump table in RAM */
                /* Populate the table with our BIOS wrapper addresses so the game can
                 * call B(N) via table[N] rather than only via JAL to the stub.
                 * Physical base: 0x200, logical: 0x80000200. Each entry is 4 bytes. */
                static int s_b0_init = 0;
                if (!s_b0_init) {
                    s_b0_init = 1;
                    static const struct { uint8_t fn; uint32_t addr; } b0_entries[] = {
                        { 0x07, 0x8005B3ACu }, { 0x08, 0x8005B3BCu },
                        { 0x09, 0x8005B3CCu }, { 0x0A, 0x8005B3DCu }, /* wait/test events near this area */
                        { 0x0B, 0x8005B3CCu }, /* TestEvent */
                        { 0x0C, 0x8005B3DCu }, /* EnableEvent */
                        { 0x0D, 0x8005B3ECu },
                        { 0x0E, 0x8005B3FCu }, { 0x0F, 0x8005B3FCu }, { 0x10, 0x8005B40Cu },
                        { 0x32, 0x8005B43Cu }, /* open */
                        { 0x33, 0x8005B44Cu }, /* lseek */
                        { 0x34, 0x8005B45Cu }, /* read */
                        { 0x35, 0x8005B46Cu }, /* write */
                        { 0x36, 0x8005B47Cu }, /* close */
                        { 0x41, 0x8005B48Cu }, /* sys_read */
                        { 0x42, 0x8005B76Cu }, /* firstfile */
                        { 0x43, 0x8005B49Cu }, /* nextfile */
                        { 0x45, 0x8005B4ACu }, /* delete */
                        { 0x4A, 0x8005CD98u }, /* InitCard */
                        { 0x4B, 0x8005CDA8u }, /* StartCard */
                        { 0x4C, 0x8005CDB8u }, /* StopCard */
                        { 0x4E, 0x8005CCACu }, /* card_write */
                        { 0x5B, 0x8005CD88u }, /* ChangeClearPad */
                    };
                    for (int i = 0; i < (int)(sizeof(b0_entries)/sizeof(b0_entries[0])); i++) {
                        uint32_t off = 0x200u + (uint32_t)b0_entries[i].fn * 4u;
                        if (off + 4 <= sizeof(g_ram))
                            memcpy(&g_ram[off], &b0_entries[i].addr, 4);
                    }
                    printf("[BIOS] B0 table populated at 0x80000200\n");
                    fflush(stdout);
                }
                cpu->v0 = 0x80000200u;
                return;
            }
            case 0x5B: return;  /* ChangeClearPad — no-op */
            default:
                printf("[BIOS B(0x%02X)] a0=0x%08X a1=0x%08X\n", func, cpu->a0, cpu->a1);
                fflush(stdout);
                return;
        }
    }

    /* --- BIOS Function Table C (addr=0xC0) -------------------------------- */
    if (addr == 0xC0) {
        switch (func) {
            case 0x02: {  /* SysEnqIntRP(priority, queue_ptr) — insert handler into chain */
                uint32_t prio  = cpu->a0 & 3;
                uint32_t qptr  = cpu->a1;
                uint32_t qphys = qptr & 0x1FFFFFu;
                if (qphys + 4 <= sizeof(g_ram)) {
                    /* queue[0] = old head (next pointer) */
                    memcpy(&g_ram[qphys], &g_int_chains[prio], 4);
                    g_int_chains[prio] = qptr;
                }
                cpu->v0 = qptr;
                return;
            }
            case 0x03: {  /* SysDeqIntRP(priority, queue_ptr) — remove handler from chain */
                uint32_t prio   = cpu->a0 & 3;
                uint32_t target = cpu->a1 & 0x1FFFFFFFu;
                /* Check if target is the head */
                if ((g_int_chains[prio] & 0x1FFFFFFFu) == target) {
                    uint32_t tphys = g_int_chains[prio] & 0x1FFFFFu;
                    uint32_t nxt = 0;
                    if (tphys + 4 <= sizeof(g_ram)) memcpy(&nxt, &g_ram[tphys], 4);
                    g_int_chains[prio] = nxt;
                } else {
                    /* Walk chain to find predecessor */
                    uint32_t cur = g_int_chains[prio];
                    int limit = 16;
                    while (cur && --limit >= 0) {
                        uint32_t cphys = cur & 0x1FFFFFu;
                        if (cphys + 4 > sizeof(g_ram)) break;
                        uint32_t nxt = 0;
                        memcpy(&nxt, &g_ram[cphys], 4);
                        if ((nxt & 0x1FFFFFFFu) == target) {
                            /* cur->next = target->next */
                            uint32_t tphys = nxt & 0x1FFFFFu;
                            uint32_t tnxt = 0;
                            if (tphys + 4 <= sizeof(g_ram)) memcpy(&tnxt, &g_ram[tphys], 4);
                            memcpy(&g_ram[cphys], &tnxt, 4);
                            break;
                        }
                        if (nxt == cur) break;
                        cur = nxt;
                    }
                }
                cpu->v0 = cpu->a1;
                return;
            }
            case 0x0A: return;  /* ChangeClearRCnt — no-op (timer/RCnt setup) */
            default:
                printf("[BIOS C(0x%02X)] a0=0x%08X a1=0x%08X\n", func, cpu->a0, cpu->a1);
                fflush(stdout);
                return;
        }
    }

    /* --- Mid-function / secondary entry points --- */

    /* block_80061350 — GPU queue drain entry, inside FUN_80061338.
     * Reached via dynamic call_by_address from the GPU packet queue dispatcher
     * (FUN_80060EF8), which stores 0x80061350 as the per-packet command handler.
     * In asm: "j 0x80061370 / nop" — skips FUN_80061480 (DMA start) and enters
     * the drain loop to wait for GPU ready.
     * With our stubs GPU STAT = 0x1C000000 (bit26=1 DMA ready, bit24=0 not busy)
     * the drain loop exits immediately.  Return v0=0 (no error). */
    if (addr == 0x80061350u) {
        static int s_61350 = 0;
        if (++s_61350 <= 3) { printf("[GPU-DRAIN] 0x80061350 intercepted #%d\n", s_61350); fflush(stdout); }
        cpu->v0 = 0;
        return;
    }

    /* --- Overlay interpreter — execute MIPS code from g_ram --- */
    /* FUN_8005CC54 — Sound-system VBlank tick wrapper.
     * This 4-instruction function calls A(0xAB), which the game installed as
     * FUN_8006E460 during SsInit.  FUN_8006E460 calls PTR_FUN_8009761c which
     * is hardcoded to FUN_8006E660 (sound flush: writes accumulated KON/KOFF
     * bits to the SPU hardware registers and runs per-voice envelope updates).
     *
     * 0x8005CC54 is in the game binary but was not recognised as a function
     * entry by the recompiler, so it is absent from tomba_dispatch.c.
     * It is called every frame from the overlay VBlank handler (ra=0x800E75B8).
     * We bypass the A-table indirection and call FUN_8006E660 directly. */
    if (addr == 0x8005CC54u) {
        static uint32_t s_cc54 = 0; ++s_cc54;
        /*if (s_cc54 <= 3) { printf("[SND-TICK] 0x8005CC54 #%u → calling func_8006E660\n", s_cc54); }*/
        psx_dispatch_compiled(cpu, 0x8006E660u);
        return;
    }

    /* FUN_80018248 — process one entry from entity pending list.
     * In binary but absent from dispatch table (recompiler missed function entry).
     * Called from overlay code when spawning projectiles/thrown pigs.
     *
     * scratchpad[0x236] = signed count of pending entries
     * scratchpad[0x204] = pointer to current position in list (array of entity ptrs)
     * scratchpad[0x1c8] = flag: bit 0 controls field ordering in each entry
     *
     * For each call: decrements count, advances pointer by 4, dereferences to get
     * entity_ptr, then writes entity_ptr+0x10 and entity_ptr+0x18 to entity[0x40]
     * and entity[0x44] respectively (or swapped if flag&1). Returns 0 always.
     *
     * Without this, spawned entities (projectiles) never get their [0x40]/[0x44]
     * fields initialized, their tick functions crash/no-op, and they vanish. */
    /* FUN_8004080c — boar grab countdown. First call: entity[5]==0 → sets bca7=1, timer=300.
     * Subsequent calls: entity[5]==1, decrements timer; when timer==1 → clears bca7=0.
     * Log to track whether it's being called and how many times. */
    if (addr == 0x8004080Cu) {
        uint32_t ent = cpu->a0;
        uint8_t step = (ent >= 0x80000000u && (ent-0x80000000u)+6 <= sizeof(g_ram))
                       ? g_ram[(ent-0x80000000u)+5] : 0xFF;
        int16_t timer = 0;
        if (ent >= 0x80000000u && (ent-0x80000000u)+0x24 <= sizeof(g_ram))
            memcpy(&timer, &g_ram[(ent-0x80000000u)+0x22], 2);
        static uint32_t s_8004080c = 0; ++s_8004080c;
        if (s_8004080c <= 5 || s_8004080c % 50 == 0 || step == 0)
            printf("[GRAB-CTR] #%u f%u ent=0x%08X step=%u timer=%d bca7=%u\n",
                   s_8004080c, g_ps1_frame, ent, step, timer, g_ram[0x9BCA7]);
        /* fall through to compiled dispatch */
    }

    if (addr == 0x80018248u) {
        int16_t count;
        memcpy(&count, &g_scratch[0x236], 2);
        static uint32_t s_f80018248_log = 0;
        if (++s_f80018248_log <= 5)
            printf("[FUN80018248] f%u count=%d ra=0x%08X\n", g_ps1_frame, count, cpu->ra);
        if (count <= 0) { cpu->v0 = 0; return; }
        count--;
        memcpy(&g_scratch[0x236], &count, 2);

        uint32_t ptr;
        memcpy(&ptr, &g_scratch[0x204], 4);
        uint32_t next_ptr = ptr + 4;
        memcpy(&g_scratch[0x204], &next_ptr, 4);

        uint32_t entity_ptr = 0;
        uint32_t pphys = ptr & 0x1FFFFFu;
        if (pphys + 4 <= sizeof(g_ram))
            memcpy(&entity_ptr, &g_ram[pphys], 4);
        uint32_t ephys = entity_ptr & 0x1FFFFFu;

        uint16_t flag = 0;
        memcpy(&flag, &g_scratch[0x1c8], 2);

        uint32_t lo = entity_ptr + 0x10;
        uint32_t hi = entity_ptr + 0x18;
        if (ephys + 0x48 <= sizeof(g_ram)) {
            if (flag & 1) {
                memcpy(&g_ram[ephys + 0x44], &lo, 4);
                memcpy(&g_ram[ephys + 0x40], &hi, 4);
            } else {
                memcpy(&g_ram[ephys + 0x40], &lo, 4);
                memcpy(&g_ram[ephys + 0x44], &hi, 4);
            }
        }
        /* Return entity_ptr (v0 = a0 from MIPS disassembly — callers use v0 for
         * further entity initialization after this call). Returning 0 previously
         * caused callers to initialize the mace/pig entity at address 0, corrupting
         * interrupt vectors and making the projectile loop run for ~100 frames. */
        cpu->v0 = entity_ptr;
        /* Log entity state so we can see what the spawned entity looks like */
        if (entity_ptr != 0 && ephys + 0x50 <= sizeof(g_ram)) {
            static uint32_t s_spawn_log = 0;
            if (++s_spawn_log <= 5) {
                uint8_t  e0   = g_ram[ephys + 0x00];  /* active flag */
                uint8_t  e2   = g_ram[ephys + 0x02];  /* type */
                uint8_t  e1c  = g_ram[ephys + 0x1C];  /* mode */
                uint32_t e40; memcpy(&e40, &g_ram[ephys + 0x40], 4);
                uint32_t e44; memcpy(&e44, &g_ram[ephys + 0x44], 4);
                uint32_t e4c; memcpy(&e4c, &g_ram[ephys + 0x4C], 4); /* overlay addr? */
                printf("[SPAWN-ENT] #%u f%u entity=0x%08X e[0]=%02X e[2]=%02X e[1C]=%02X e[40]=0x%08X e[44]=0x%08X e[4C]=0x%08X ra=0x%08X\n",
                       s_spawn_log, g_ps1_frame, entity_ptr,
                       e0, e2, e1c, e40, e44, e4c,
                       cpu->ra);
                fflush(stdout);
            }
        }
        return;
    }

    /* Addresses >= 0x80098000 are dynamically loaded overlays (via CdRead).
     * If the compiled dispatcher doesn't know the address, try interpreting it. */
    {
        uint32_t norm = (addr & 0x1FFFFFFFu) | 0x80000000u;
        if (norm >= 0x80098000u && norm <= 0x801FFFFFu) {
            static uint32_t s_interp_enter = 0;
            ++s_interp_enter;
            /* [INTERP] enter — first 50 + every 500: printf("[INTERP] enter 0x%08X ra=0x%08X\n", norm, cpu->ra); */

            /* --- Camera handler trace: FUN_8002d784 --- */
            if (norm == 0x8002D784u && g_ps1_frame >= 1670) {
                static uint32_t s_cam_d784 = 0;
                /* [CAM-D784] if (++s_cam_d784 <= 30 || s_cam_d784 % 200 == 1)
                   printf("[CAM-D784] #%u f%u sub=%u cam_x=0x%08X ptr34=0x%08X *ptr34=0x%08X\n", ...); */
                ++s_cam_d784;
            }

            /* --- Camera overlay trace + guard: 0x801168F4 ---
             * Camera overlay crashes when DAT_8009BCA2 (bca2) == 0 because zone
             * function pointers in scratchpad haven't been initialized yet.
             * On real PS1 the zone init runs before camera (slow CD).  In our
             * recompiler CD is instant so they can race.  Guard: skip the camera
             * overlay entirely when bca2==0 (zone not ready). */
            int is_cam_overlay = (norm == 0x801168F4u && g_ps1_frame >= 1670);
            if (is_cam_overlay) {
                uint8_t bca2_guard = g_ram[0x9bca2];
                if (bca2_guard == 0) {
                    static uint32_t s_cam_skip = 0;
                    if (++s_cam_skip <= 5) {
                        printf("[CAM-OVL-SKIP] #%u f%u bca2=0, skipping overlay to avoid null-ptr crash\n",
                               s_cam_skip, g_ps1_frame);
                        fflush(stdout);
                    }
                    return;  /* skip mips_interpret for camera overlay when bca2=0 */
                }
                static uint32_t s_cam_trace = 0;
                if (++s_cam_trace <= 5) {
                    uint8_t sub_state = g_ram[(cpu->a0 & 0x1FFFFFu) + 5];
                    printf("[CAM-OVL] #%u f%u ent=0x%08X sub=%u bca2=%u\n",
                           s_cam_trace, g_ps1_frame, cpu->a0, sub_state, bca2_guard);
                    fflush(stdout);
                }
            }

            /* --- Entity dispatcher overlay fix ---
             * The compiled entity dispatcher (func_8005A074) calls overlay entity
             * functions with ra=0x8005A0CC.  At that call site:
             *   s0 = entity pool base   (0x800A55C8)
             *   s1 = function table base (0x8007F6F4)
             *   a0 = this entity pointer
             *
             * Overlay entity handlers (e.g. 0x80130900) were compiled against the
             * original MIPS ABI where s1 is NOT the function table — it holds
             * whatever the overlay wants (typically the entity pointer or a scratch
             * register).  The overlay at 0x80130900 does:
             *   SH $v1, 32($s1)   — store to entity+32
             *   SB $v0,  7($s1)   — store to entity+7
             * With s1 = function-table base those writes corrupt the type-1 entry.
             *
             * Fix: when entering any overlay called from the entity dispatcher, set
             * s1 = a0 (the entity pointer) so the writes land inside entity data.
             * call_by_address already saves s1_before and restores it afterwards,
             * so the compiled dispatcher's s1 is unaffected.
             */
            if (cpu->ra == 0x8005A0CCu) {
                cpu->s1 = cpu->a0;  /* redirect overlay's [s1+N] writes to entity */
            }

            /* --- Options screen CD streaming fix ---
             * The options screen overlay (0x800E75CC) polls FUN_80068aa8 which reads
             * the streaming queue status byte at 0x8009B3E9 (slot0 + 0x49).
             * On real PS1 this becomes 2 after the CD-ROM fires its completion interrupt
             * (registered via SysEnqIntRP C(02)).  Our runtime has no CD-ROM hardware
             * so the interrupt never fires and status stays 0 forever.
             *
             * Fix: when the options screen poll function is about to run and the
             * streaming system is enabled (DAT_80097548 != 0) but status is still 0,
             * inject status=2 to simulate "CD drive ready".  This is safe because
             * nothing else will change this byte (no CD interrupts, no state machine). */
            if (norm == 0x800E75CCu && g_ram[0x97548] != 0 && g_ram[0x9B3E9] == 0) {
                g_ram[0x9B3E9] = 2;
                printf("[CD-STREAM-READY] f%u injected streaming status=2 (slot0+0x49=2)\n", g_ps1_frame);
                fflush(stdout);
            }

            mips_interpret(cpu, norm);


            return;
        }
    }

    /* In-binary function — try compiled dispatch first (runs C function including
     * psx_override_dispatch), then fall back to mips_interpret for functions the
     * recompiler didn't discover (e.g. runtime-patched handler tables). */
    if (kseg0 >= 0x80010000u && kseg0 < 0x80098000u) {
        static uint32_t s_interp_main = 0;
        if (psx_dispatch_compiled(cpu, kseg0)) goto sp_check;
        ++s_interp_main;
        /* [INTERP-MAIN] first 3: printf("[INTERP-MAIN] #%u f%u 0x%08X ...\n"); */
        mips_interpret(cpu, kseg0);
        return;
    }

    /* --- Unknown call --- */
    {
        ++s_dispatch_miss_count;
        /* Suppress log after first 50 to avoid flooding; print summary every 1000 */
        if (s_dispatch_miss_count <= 50 || s_dispatch_miss_count % 1000 == 0) {
            printf("[UNKNOWN CALL] #%u 0x%08X  (t1=0x%08X  a0=0x%08X  ra=0x%08X  sp=0x%08X)\n",
                   s_dispatch_miss_count, addr, cpu->t1, cpu->a0, cpu->ra, cpu->sp);
            fflush(stdout);
        }

        /* Track unique dispatch misses and log to file */
        {
            int found = -1;
            for (int i = 0; i < s_dispatch_miss_unique_count; i++) {
                if (s_dispatch_miss_addrs[i] == addr) { found = i; break; }
            }
            if (found < 0 && s_dispatch_miss_unique_count < MAX_DISPATCH_MISS_UNIQUE) {
                found = s_dispatch_miss_unique_count++;
                s_dispatch_miss_addrs[found] = addr;
                /* Log new unique miss to file */
                FILE *mf = fopen("dispatch_misses.log", "a");
                if (mf) {
                    fprintf(mf, "0x%08X  ra=0x%08X  frame=%u\n", addr, cpu->ra, g_ps1_frame);
                    fclose(mf);
                }
            }
        }
    }
    return;

sp_check:
    /* Restore callee-saved registers if any were corrupted */
    if (cpu->sp != sp_before || cpu->s0 != s0_before || cpu->s1 != s1_before) {
        static uint32_t s_callee_fix_count = 0;
        if (++s_callee_fix_count <= 20 || DIAG_ENABLED()) {
            printf("[CALLEE-FIX] #%u f%u call_by_address(0x%08X) sp:%08X->%08X s0:%08X->%08X s1:%08X->%08X — restoring\n",
                   s_callee_fix_count, g_ps1_frame, addr,
                   sp_before, cpu->sp, s0_before, cpu->s0, s1_before, cpu->s1);
            fflush(stdout);
        }
    }
    cpu->sp = sp_before;
    cpu->s0 = s0_before; cpu->s1 = s1_before;
    cpu->s2 = s2_before; cpu->s3 = s3_before;
    cpu->s4 = s4_before; cpu->s5 = s5_before;
    cpu->s6 = s6_before; cpu->s7 = s7_before;
    cpu->fp = fp_before;
}

/* === Terrain entity persistence state ===
 * The terrain VM processes entity tiles every 4 frames. On the 3 skip frames,
 * those entities are absent from the render list, causing flickering.
 * Fix: cache the pushed entity list and re-inject missing-but-recently-visible
 * entities at render time (FUN_80046264 intercept). */
static uint32_t s_tp_cache[50];          /* entity ptrs from last good push frame */
static uint8_t  s_tp_cache_e1[50];       /* entity[1] at snapshot time */
static uint32_t s_tp_cache_count = 0;
static uint32_t s_tp_cache_frame = 0;
static uint32_t s_tp_cur[50];            /* entity ptrs pushed this frame */
static uint32_t s_tp_cur_count = 0;
static uint32_t s_tp_cur_frame = 0;      /* frame s_tp_cur was last reset for */

/* === Case 5 entity persistence (FUN_80018d04 → list 0x228/0x248) ===
 * Signs, mushrooms, foreground objects use case 5 and suffer the same
 * every-N-frames skip. Same persistence cache approach as case 3 above. */
static uint32_t s_sp_cache[50];
static uint8_t  s_sp_cache_e1[50];       /* entity[1] at snapshot time */
static uint32_t s_sp_cache_count = 0;
static uint32_t s_sp_cache_frame = 0;
static uint32_t s_sp_cur[50];
static uint32_t s_sp_cur_count = 0;
static uint32_t s_sp_cur_frame = 0;

/* === Case 4 entity persistence (C list: ptr=scratch 0x224, count=scratch 0x24C) ===
 * FUN_8004d0c0 → FUN_80045efc. FUN_80045efc checks entity[1] and
 * if (entity[0xA] & 0x10) != 0 && entity[0xA0] == 0 → early return.
 * Must restore entity[0xA0] alongside the ptr. */
static uint32_t s_c4p_cache[50];
static uint32_t s_c4p_cache_e1[50];   /* entity[1] from active frame */
static uint32_t s_c4p_cache_ea0[50];  /* entity[0xA0] (4 bytes) from active frame */
static uint32_t s_c4p_cache_count = 0;
static uint32_t s_c4p_cache_frame = 0;

/* === Case 2 entity persistence (B2 list: ptr=scratch 0x21C, count=scratch 0x246) ===
 * Signs, NPCs, foreground objects — drops from 14→3 on skip frames.
 * Snapshot full list on active frames; re-inject missing on skip frames.
 * FUN_8004ad8c checks entity[1] != 0 before rendering — must also restore that byte. */
static uint32_t s_b2p_cache[50];
static uint8_t  s_b2p_cache_e1[50];  /* entity[1] byte saved from active frame */
static uint32_t s_b2p_cache_count = 0;
static uint32_t s_b2p_cache_frame = 0;

/* Override gate — always pass-through by default.
 * Do NOT add psx_register_override() here. If a function needs different
 * behavior, fix code_generator.cpp to emit correct code. */
int psx_override_dispatch(CPUState* cpu, uint32_t addr) {
    /* ---- Entity spawner diagnostics ---- */
    if (addr == 0x80018C40u) {
        /* FUN_80018c40: entity push */
        static uint32_t s_spawn_calls = 0;
        ++s_spawn_calls;
        if (DIAG_ENABLED()) {
            uint16_t ent_count = *(uint16_t*)(g_scratch + 0x24A);
            printf("[ENTITY-PUSH] #%u f%u a0=0x%08X count=%u stack=0x%08X ra=0x%08X\n",
                   s_spawn_calls, g_ps1_frame, cpu->a0, ent_count,
                   *(uint32_t*)(g_scratch + 0x220), cpu->ra);
            fflush(stdout);
        }
        /* Track pushed entities per frame for terrain persistence */
        if (s_tp_cur_frame != g_ps1_frame) {
            s_tp_cur_count = 0;
            s_tp_cur_frame = g_ps1_frame;
        }
        if (s_tp_cur_count < 50) {
            s_tp_cur[s_tp_cur_count++] = cpu->a0;
        }
    }
    if (addr == 0x80018D04u) {
        /* FUN_80018d04: case-5 entity push (signs, mushrooms, foreground objects)
         * List ptr=0x228, count=0x248. Track for persistence re-injection. */
        if (s_sp_cur_frame != g_ps1_frame) {
            s_sp_cur_count = 0;
            s_sp_cur_frame = g_ps1_frame;
        }
        if (s_sp_cur_count < 50) {
            s_sp_cur[s_sp_cur_count++] = cpu->a0;
        }
    }
    if (addr == 0x8005A074u) {
        /* FUN_8005a074: UI dispatcher — log first call */
        static uint32_t _uidisp_first = 0;
        if (_uidisp_first == 0) {
            _uidisp_first = g_ps1_frame;
            printf("[UI-DISP] first call at f%u\n", g_ps1_frame);
            fflush(stdout);
        }
    }
    if (addr == 0x8005A184u) {
        /* FUN_8005a184: type=0 UI handler — log calls with state */
        static uint32_t _a184cnt = 0; ++_a184cnt;
        if (DIAG_ENABLED()) {
            uint32_t eptr = cpu->a0;
            uint32_t phys = eptr & 0x1FFFFFFFu;
            uint8_t st = (phys < sizeof(g_ram)) ? g_ram[phys + 4] : 0xFF;
            printf("[UI-A184] f%u entry=0x%08X state=%d\n",
                   g_ps1_frame, eptr, (int)st);
            fflush(stdout);
        }
    }
    if (addr == 0x8005A508u) {
        /* FUN_8005a508: dialogue speech bubble setup — log calls + scratchpad pointer chain */
        static uint32_t _a508cnt = 0;
        if (++_a508cnt <= 50) {
            uint16_t bcc8;
            uint8_t  bcca;
            memcpy(&bcc8, g_ram + 0x09BCC8, 2);
            bcca = g_ram[0x09BCCA];
            /* Read scratchpad pointer: *(int*)(0x8007EEE0 + (bcca&7)*4) */
            uint32_t puVar5_idx = bcc8;  /* (&PTR_DAT_8007f0c8)[bcc8] */
            uint32_t puVar5 = 0;
            if ((puVar5_idx * 4 + 0x0007F0C8 + 4) <= sizeof(g_ram))
                memcpy(&puVar5, g_ram + 0x0007F0C8 + puVar5_idx * 4, 4);
            /* puVar5 is now a PS1 address; read the element at (bcca&7)*4 */
            uint32_t spptr_addr = puVar5 + (bcca & 7) * 4;
            uint32_t spptr_phys = spptr_addr & 0x1FFFFFFFu;
            uint32_t spptr_val = 0;
            if (spptr_phys + 4 <= sizeof(g_ram))
                memcpy(&spptr_val, g_ram + spptr_phys, 4);
            /* spptr_val is the PS1 address (e.g. scratchpad 0x1F800358); read what it points to */
            uint32_t scrpad_off = spptr_val & 0x3FFu;  /* scratchpad is 1K */
            uint32_t dialogue_ptr = 0;
            if ((spptr_val & 0xFF800000u) == 0x1F800000u && scrpad_off + 4 <= 1024)
                memcpy(&dialogue_ptr, g_scratch + scrpad_off, 4);
            printf("[UI-A508] f%u bcc8=0x%04X bcca=0x%02X puVar5=0x%08X spptr_val=0x%08X dialogue_ptr=0x%08X\n",
                   g_ps1_frame, (unsigned)bcc8, (unsigned)bcca,
                   (unsigned)puVar5, (unsigned)spptr_val, (unsigned)dialogue_ptr);
            fflush(stdout);
        }
    }
    if (addr == 0x8005AA98u) {
        /* FUN_8005aa98: dialogue entity allocator — log first few with position data */
        static uint32_t _aa98cnt = 0;
        if (++_aa98cnt <= 20) {
            /* [UI-AA98] first 20 — re-enable: add the printf back */
        }
    }
    if (addr == 0x80018354u) {
        /* FUN_80018354: dialogue sprite allocator — log count before allocation */
        static uint32_t _18354cnt = 0;
        if (++_18354cnt <= 20) {
            /* [UI-ALLOC-A] first 20 — re-enable: add the printf back */
        }
    }
    if (addr == 0x8005AF70u) {
        /* func_8005AF70: text box UI handler — log calls with entry state */
        static uint32_t _afcnt = 0; ++_afcnt;
        if (DIAG_ENABLED()) {
            uint32_t eptr = cpu->a0;
            uint32_t phys = eptr & 0x1FFFFFFFu;
            uint8_t state = (phys < sizeof(g_ram)) ? g_ram[phys + 4] : 0xFF;
            uint8_t active = (phys < sizeof(g_ram)) ? g_ram[phys + 0] : 0xFF;
            uint8_t type   = (phys < sizeof(g_ram)) ? g_ram[phys + 2] : 0xFF;
            printf("[UI-AF70] f%u entry=0x%08X active=%d type=%d state=%d\n",
                   g_ps1_frame, eptr, (int)active, (int)type, (int)state);
            fflush(stdout);
        }
    }
    if (addr == 0x80059F7Cu) {
        /* FUN_80059f7c: UI slot spawner — print free-list count before/after */
        if (DIAG_ENABLED()) {
            uint16_t free_count;
            memcpy(&free_count, g_scratch + 0x23E, 2);
            printf("[UI-SPAWN] f%u FUN_80059f7c entry: free_slots=%u\n",
                   g_ps1_frame, (unsigned)free_count);
            fflush(stdout);
        }
    }
    if (addr == 0x80018694u) {
        /* FUN_80018694: UI slot allocator — print free count on each call */
        if (DIAG_ENABLED()) {
            uint16_t free_count;
            memcpy(&free_count, g_scratch + 0x23E, 2);
            printf("[UI-ALLOC] f%u FUN_80018694 called: free_slots=%u ra=0x%08X\n",
                   g_ps1_frame, (unsigned)free_count, cpu->ra);
            fflush(stdout);
        }
    }
    if (addr == 0x80022E44u) {
        /* ---- Comprehensive entity visibility diagnostic for FUN_80022e44 ----
         * This function checks if an entity is visible and pushes it to the
         * appropriate render list based on layer (param_1[0x1c] & 0x7f).
         * Layer 1 -> List A (scratch 0x218/0x244)
         * Layer 3 -> List B (scratch 0x220/0x24A)
         * Trace-only: does NOT skip execution. */

        /* Per-frame accumulators (reset each frame) */
        static uint32_t s_prev_frame = 0;
        static uint32_t s_calls_this_frame = 0;
        static uint32_t s_active = 0;
        static uint32_t s_pass_x = 0;
        static uint32_t s_pass_y = 0;
        static uint32_t s_layer_counts[16] = {0}; /* layer distribution for visible entities */
        static uint32_t s_layer_counts_all[16] = {0}; /* layer distribution for ALL active entities */
        static uint32_t s_summary_count = 0;
        static int s_one_time_done = 0;

        /* Detect frame boundary and print previous frame's summary */
        if (g_ps1_frame != s_prev_frame) {
            if (s_prev_frame != 0 && DIAG_ENABLED()) {
                /* Read render list counts */
                uint16_t listA_count = 0, listB_count = 0;
                memcpy(&listA_count, &g_scratch[0x244], 2);
                memcpy(&listB_count, &g_scratch[0x24A], 2);
                int16_t cam_x = 0, cam_y = 0;
                memcpy(&cam_x, &g_scratch[0x176], 2);
                memcpy(&cam_y, &g_scratch[0x186], 2);

                printf("[ENTITY-VIS] f%u calls=%u active=%u passX=%u passY=%u "
                       "listA=%u listB=%u cam=(%d,%d)\n",
                       s_prev_frame, s_calls_this_frame, s_active,
                       s_pass_x, s_pass_y, listA_count, listB_count,
                       (int)cam_x, (int)cam_y);
                /* Print layer distribution for VISIBLE entities */
                printf("  visible layers:");
                for (int i = 0; i < 16; i++) {
                    if (s_layer_counts[i]) printf(" L%d=%u", i, s_layer_counts[i]);
                }
                printf("\n");
                /* Print layer distribution for ALL active entities */
                printf("  active layers: ");
                for (int i = 0; i < 16; i++) {
                    if (s_layer_counts_all[i]) printf(" L%d=%u", i, s_layer_counts_all[i]);
                }
                printf("\n");
                fflush(stdout);
                s_summary_count++;
            }
            /* Reset accumulators */
            s_calls_this_frame = 0;
            s_active = 0;
            s_pass_x = 0;
            s_pass_y = 0;
            memset(s_layer_counts, 0, sizeof(s_layer_counts));
            memset(s_layer_counts_all, 0, sizeof(s_layer_counts_all));
            s_prev_frame = g_ps1_frame;
        }

        s_calls_this_frame++;

        uint32_t a0_phys = cpu->a0 & 0x1FFFFFFFu;
        if (a0_phys < 0x200000u) {
            uint8_t alive = g_ram[a0_phys];
            if (alive != 0) {
                s_active++;
                uint8_t layer = g_ram[a0_phys + 0x1C] & 0x7F;
                if (layer < 16) s_layer_counts_all[layer]++;

                /* Replicate the visibility check to see where entities fail */
                uint32_t world_ptr = 0;
                memcpy(&world_ptr, &g_ram[a0_phys + 0x40], 4);
                uint32_t wp = world_ptr & 0x1FFFFFFFu;
                int16_t ent_x = 0, ent_y = 0;
                int16_t cam_x = 0, cam_y = 0;
                if (wp + 4 < 0x200000u) {
                    memcpy(&ent_x, &g_ram[wp + 2], 2);
                }
                memcpy(&ent_y, &g_ram[a0_phys + 0x16], 2);
                memcpy(&cam_x, &g_scratch[0x176], 2);
                memcpy(&cam_y, &g_scratch[0x186], 2);

                /* X check: (ent_x - cam_x + 0x40) as uint16 < 0x1c1 */
                uint16_t x_test = (uint16_t)((int16_t)(ent_x - cam_x) + 0x40);
                int x_pass = (x_test < 0x1C1);
                /* Y check: (cam_y - ent_y + 0x40) as uint16 < 0x171 */
                uint16_t y_test = (uint16_t)((int16_t)(cam_y - ent_y) + 0x40);
                int y_pass = (y_test < 0x171);

                if (x_pass) s_pass_x++;
                if (x_pass && y_pass) {
                    s_pass_y++;
                    if (layer < 16) s_layer_counts[layer]++;
                }

                /* ONE-TIME: dump first 20 entities with full detail on first
                 * gameplay frame where we have active entities */
                if (!s_one_time_done && s_active <= 30 && g_ps1_frame > 100) {
                    printf("[ENT-DETAIL] f%u #%u a0=0x%08X layer=%u alive=%u "
                           "ent=(%d,%d) cam=(%d,%d) xtest=%u(%s) ytest=%u(%s) "
                           "world_ptr=0x%08X\n",
                           g_ps1_frame, s_active, cpu->a0, layer, alive,
                           (int)ent_x, (int)ent_y, (int)cam_x, (int)cam_y,
                           x_test, x_pass ? "PASS" : "FAIL",
                           y_test, y_pass ? "PASS" : "FAIL",
                           world_ptr);
                    fflush(stdout);
                    if (s_active == 30 || s_calls_this_frame > 200) {
                        s_one_time_done = 1;
                        printf("[ENT-DETAIL] === one-time dump complete ===\n");
                        fflush(stdout);
                    }
                }

                /* Log layer=1 entities that PASS visibility (these should go
                 * to List A but might be missing) */
                if (x_pass && y_pass && layer == 1) {
                    static uint32_t s_l1_vis = 0;
                    if (++s_l1_vis <= 20) {
                        uint16_t listA_count = 0;
                        memcpy(&listA_count, &g_scratch[0x244], 2);
                        uint32_t listA_stack = 0;
                        memcpy(&listA_stack, &g_scratch[0x218], 4);
                        printf("[LAYER1-VIS] #%u f%u a0=0x%08X ent=(%d,%d) "
                               "listA_count=%u listA_stack=0x%08X\n",
                               s_l1_vis, g_ps1_frame, cpu->a0,
                               (int)ent_x, (int)ent_y,
                               listA_count, listA_stack);
                        fflush(stdout);
                    }
                }
            }
        }
        /* Trace-only: return 0 so the real function executes */
    }



    /* ---- Addprim counter per-frame (log at render entry/exit via 0x80046264) ---- */
    if (addr == 0x80046264u) {

        /* === Entity list counts at render entry ===
         * Read all 5 push-list counts from scratchpad to track what's queued.
         * Case 2 (B2: ptr=0x21C, count=0x246) = signs, NPCs, "flickering" entities.
         * Case 3 (B:  ptr=0x220, count=0x24A) = tracked as listB in ENTITY-VIS.
         * Case 5 (D:  ptr=0x228, count=0x248) = terrain tiles (ENTITY-PUSH tracks these). */
        {
            uint16_t c1, c2, c3, c4, c5;
            memcpy(&c1, &g_scratch[0x244], 2);
            memcpy(&c2, &g_scratch[0x246], 2);
            memcpy(&c3, &g_scratch[0x24A], 2);
            memcpy(&c4, &g_scratch[0x24C], 2);
            memcpy(&c5, &g_scratch[0x248], 2);
            static uint16_t s_prev_c1=0, s_prev_c2=0, s_prev_c3=0, s_prev_c4=0, s_prev_c5=0;
            static uint32_t s_list_tick = 0;
            /* Log on change or every 120 frames */
            ++s_list_tick;
            /* [LIST-COUNTS] on-change + every-120 — commented out (re-enable for visibility debugging) */
        }

        /* === UI entry array diagnostic (one-shot at frame 1700+) === */
        {
            static uint32_t s_ui_dump_frame = 0;
            if (s_ui_dump_frame == 0 && g_ps1_frame >= 1700) {
                s_ui_dump_frame = g_ps1_frame;
                printf("[UI-ENTRIES] f%u dump:\n", g_ps1_frame);
                for (int _ui = 0; _ui < 10; _ui++) {
                    const uint8_t *e = &g_ram[0x0A55C8 + _ui * 0x3C];
                    if (e[0]) {
                        printf("[UI-ENTRIES]   [%d] active=%d type=%d sub=%d state=%d\n",
                               _ui, (int)e[0], (int)e[2], (int)e[3], (int)e[4]);
                    }
                }
                /* Also dump DAT_8009ABCDD (dialogue box state) */
                printf("[UI-ENTRIES]   ABCDD=0x%02X ABCDE=0x%02X bca0=0x%02X\n",
                       g_ram[0x09BCDD], g_ram[0x09BCDE], g_ram[0x09BCA0]);
                fflush(stdout);
            }
        }

        /* === Case 2 entity persistence: re-inject cached entities on skip frames ===
         * B2 list (ptr=scratch 0x21C, count=scratch 0x246) drops from ~14→3 on skip
         * frames. Cache the full list on active frames; re-inject missing entities on
         * skip frames to match PS1 VRAM persistence. */
        {
            uint32_t b2ptr; memcpy(&b2ptr, &g_scratch[0x21C], 4);
            uint16_t c2; memcpy(&c2, &g_scratch[0x246], 2);
            if (c2 > 5u) {
                /* Active frame: snapshot entity ptrs */
                s_b2p_cache_count = 0;
                s_b2p_cache_frame = g_ps1_frame;
                for (uint16_t i = 0; i < c2 && s_b2p_cache_count < 50u; i++) {
                    uint32_t phys_ptr = (b2ptr + i*4u) & 0x1FFFFFFFu;
                    if (phys_ptr < 0x200000u) {
                        uint32_t ep; memcpy(&ep, &g_ram[phys_ptr], 4);
                        uint32_t phys_ep = ep & 0x1FFFFFFFu;
                        s_b2p_cache_e1[s_b2p_cache_count] =
                            (phys_ep + 1 < 0x200000u) ? g_ram[phys_ep + 1] : 1u;
                        s_b2p_cache[s_b2p_cache_count++] = ep;
                    }
                }
                static uint32_t s_b2_snap_log = 0;
                if (++s_b2_snap_log <= 5) {
                    printf("[B2-SNAP] f%u b2ptr=0x%08X c2=%u snapped=%u\n",
                           g_ps1_frame, b2ptr, c2, s_b2p_cache_count);
                    fflush(stdout);
                }
            } else if (s_b2p_cache_count > 0 && g_ps1_frame > s_b2p_cache_frame &&
                       g_ps1_frame - s_b2p_cache_frame <= 8u) {
                /* Skip frame with recent cache: re-inject missing entities */
                static uint32_t s_b2_inj_log = 0;
                uint32_t b2ptr_before = b2ptr; uint16_t c2_before = c2;
                for (uint32_t i = 0; i < s_b2p_cache_count; i++) {
                    uint32_t ent = s_b2p_cache[i];
                    uint32_t phys = ent & 0x1FFFFFFFu;
                    if (phys >= 0x200000u) continue;
                    int found = 0;
                    for (uint16_t j = 0; j < c2; j++) {
                        uint32_t sp = (b2ptr + j*4u) & 0x1FFFFFFFu;
                        if (sp < 0x200000u) {
                            uint32_t listed; memcpy(&listed, &g_ram[sp], 4);
                            if (listed == ent) { found = 1; break; }
                        }
                    }
                    if (!found && c2 < 50u) {
                        b2ptr -= 4u;
                        uint32_t sp = b2ptr & 0x1FFFFFFFu;
                        if (sp < 0x200000u) {
                            memcpy(&g_ram[sp], &ent, 4u);
                            c2++;
                            /* Restore entity[1] so FUN_8004ad8c's ptr[1]!=0 check passes */
                            uint32_t phys_ep = ent & 0x1FFFFFFFu;
                            if (phys_ep + 1 < 0x200000u && g_ram[phys_ep + 1] == 0) {
                                g_ram[phys_ep + 1] = s_b2p_cache_e1[i] ? s_b2p_cache_e1[i] : 1u;
                            }
                        }
                    }
                }
                memcpy(&g_scratch[0x21C], &b2ptr, 4);
                memcpy(&g_scratch[0x246], &c2, 2);
                if (++s_b2_inj_log <= 10) {
                    printf("[B2-INJ] f%u ptr 0x%08X→0x%08X c2 %u→%u cache=%u\n",
                           g_ps1_frame, b2ptr_before, b2ptr, c2_before, c2, s_b2p_cache_count);
                    /* Dump first injected entity's key fields */
                    if (s_b2p_cache_count > 0) {
                        uint32_t fe = s_b2p_cache[0];
                        uint32_t fp = fe & 0x1FFFFFFFu;
                        if (fp + 0x70 < 0x200000u) {
                            uint8_t e1 = g_ram[fp+1], eA = g_ram[fp+0xA];
                            int16_t x12, x16, x1a;
                            uint32_t p24, p3c;
                            memcpy(&x12, &g_ram[fp+0x12], 2);
                            memcpy(&x16, &g_ram[fp+0x16], 2);
                            memcpy(&x1a, &g_ram[fp+0x1A], 2);
                            memcpy(&p24, &g_ram[fp+0x24], 4);
                            memcpy(&p3c, &g_ram[fp+0x3c], 4);
                            printf("[B2-ENT0] ep=0x%08X e[1]=0x%02X e[A]=0x%02X "
                                   "x=%d z=%d y=%d p24=0x%08X p3c=0x%08X\n",
                                   fe, e1, eA, x12, x16, x1a, p24, p3c);
                            fflush(stdout);
                        }
                    }
                }
            } else if (s_b2p_cache_count == 0) {
                static uint32_t s_b2_nocache = 0;
                if (++s_b2_nocache <= 5) {
                    printf("[B2-NOCACHE] f%u c2=%u\n", g_ps1_frame, c2);
                    fflush(stdout);
                }
            }
        }

        /* === Case 4 entity persistence: re-inject cached entities on skip frames ===
         * C list (ptr=scratch 0x224, count=scratch 0x24C) drops from 7→0 on skip frames.
         * FUN_8004d0c0 iterates this list calling FUN_80045efc — no extra entity checks. */
        {
            uint32_t c4ptr; memcpy(&c4ptr, &g_scratch[0x224], 4);
            uint16_t c4; memcpy(&c4, &g_scratch[0x24C], 2);
            if (c4 > 0u) {
                /* Active frame: snapshot entity ptrs */
                s_c4p_cache_count = 0;
                s_c4p_cache_frame = g_ps1_frame;
                for (uint16_t i = 0; i < c4 && s_c4p_cache_count < 50u; i++) {
                    uint32_t phys_ptr = (c4ptr + i*4u) & 0x1FFFFFFFu;
                    if (phys_ptr < 0x200000u) {
                        uint32_t ep; memcpy(&ep, &g_ram[phys_ptr], 4);
                        uint32_t phys_ep = ep & 0x1FFFFFFFu;
                        s_c4p_cache_e1[s_c4p_cache_count] =
                            (phys_ep+1 < 0x200000u) ? g_ram[phys_ep+1] : 1u;
                        uint32_t ea0 = 0;
                        if (phys_ep+0xA0+4 <= 0x200000u)
                            memcpy(&ea0, &g_ram[phys_ep+0xA0], 4);
                        s_c4p_cache_ea0[s_c4p_cache_count] = ea0;
                        s_c4p_cache[s_c4p_cache_count++] = ep;
                    }
                }
            } else if (s_c4p_cache_count > 0 && g_ps1_frame > s_c4p_cache_frame &&
                       g_ps1_frame - s_c4p_cache_frame <= 8u) {
                /* Skip frame: inject all cached entities (c4=0, nothing already in list) */
                for (uint32_t i = 0; i < s_c4p_cache_count; i++) {
                    uint32_t ent = s_c4p_cache[i];
                    if ((ent & 0x1FFFFFFFu) >= 0x200000u) continue;
                    c4ptr -= 4u;
                    uint32_t sp = c4ptr & 0x1FFFFFFFu;
                    if (sp < 0x200000u) {
                        memcpy(&g_ram[sp], &ent, 4u);
                        c4++;
                        /* Restore entity[1] and entity[0xA0] so FUN_80045efc checks pass */
                        uint32_t phys_ep = ent & 0x1FFFFFFFu;
                        if (phys_ep+1 < 0x200000u && g_ram[phys_ep+1] == 0)
                            g_ram[phys_ep+1] = s_c4p_cache_e1[i] ? (uint8_t)s_c4p_cache_e1[i] : 1u;
                        if (phys_ep+0xA0+4 <= 0x200000u) {
                            uint32_t cur_ea0; memcpy(&cur_ea0, &g_ram[phys_ep+0xA0], 4);
                            if (cur_ea0 == 0 && s_c4p_cache_ea0[i] != 0)
                                memcpy(&g_ram[phys_ep+0xA0], &s_c4p_cache_ea0[i], 4);
                        }
                    }
                }
                memcpy(&g_scratch[0x224], &c4ptr, 4);
                memcpy(&g_scratch[0x24C], &c4, 2);
            }
        }

        /* === Terrain entity persistence: re-inject cached entities on skip frames ===
         * The terrain VM only pushes certain entity tiles every 4 frames. On skip
         * frames the render list is empty for those entities. We re-inject them
         * so they render every frame (matching PS1 VRAM persistence behavior). */

        /* If this frame had entity pushes, update the persistent cache */
        if (s_tp_cur_frame == g_ps1_frame && s_tp_cur_count > 0) {
            s_tp_cache_count = 0;
            for (uint32_t i = 0; i < s_tp_cur_count && i < 50u; i++) {
                uint32_t ep = s_tp_cur[i];
                uint32_t phys_ep = ep & 0x1FFFFFFFu;
                s_tp_cache[i] = ep;
                s_tp_cache_e1[i] = (phys_ep + 1 < 0x200000u) ? g_ram[phys_ep + 1] : 1u;
                s_tp_cache_count++;
            }
            s_tp_cache_frame = g_ps1_frame;
        }

        /* If cache is recent (< 8 frames old), inject any cached entity missing
         * from the current render list */
        if (s_tp_cache_count > 0 && g_ps1_frame > s_tp_cache_frame &&
            g_ps1_frame - s_tp_cache_frame <= 8) {
            uint32_t stack; memcpy(&stack, &g_scratch[0x220], 4);
            uint16_t count; memcpy(&count, &g_scratch[0x24A], 2);

            for (uint32_t i = 0; i < s_tp_cache_count; i++) {
                uint32_t ent = s_tp_cache[i];
                uint32_t phys = ent & 0x1FFFFFFFu;
                if (phys >= 0x200000u || g_ram[phys] == 0) continue; /* dead entity */

                /* Skip if already in current render list */
                int found = 0;
                for (uint32_t j = 0; j < count; j++) {
                    uint32_t sp = (stack + j * 4) & 0x1FFFFFFFu;
                    if (sp < 0x200000u) {
                        uint32_t listed; memcpy(&listed, &g_ram[sp], 4);
                        if (listed == ent) { found = 1; break; }
                    }
                }
                if (!found && count < 86) {
                    stack -= 4;
                    uint32_t sp = stack & 0x1FFFFFFFu;
                    if (sp < 0x200000u) {
                        memcpy(&g_ram[sp], &ent, 4);
                        count++;
                        /* Restore entity[1] so FUN_8004afac's entity[1]!=0 check passes */
                        if (phys + 1 < 0x200000u && g_ram[phys + 1] == 0)
                            g_ram[phys + 1] = s_tp_cache_e1[i] ? s_tp_cache_e1[i] : 1u;
                    }
                }
            }
            memcpy(&g_scratch[0x220], &stack, 4);
            memcpy(&g_scratch[0x24A], &count, 2);
        }

        /* === Case 5 entity persistence: signs, mushrooms, foreground objects === */
        if (s_sp_cur_frame == g_ps1_frame && s_sp_cur_count > 0) {
            s_sp_cache_count = 0;
            for (uint32_t i = 0; i < s_sp_cur_count && i < 50u; i++) {
                uint32_t ep = s_sp_cur[i];
                uint32_t phys_ep = ep & 0x1FFFFFFFu;
                s_sp_cache[i] = ep;
                s_sp_cache_e1[i] = (phys_ep + 1 < 0x200000u) ? g_ram[phys_ep + 1] : 1u;
                s_sp_cache_count++;
            }
            s_sp_cache_frame = g_ps1_frame;
        }
        if (s_sp_cache_count > 0 && g_ps1_frame > s_sp_cache_frame &&
            g_ps1_frame - s_sp_cache_frame <= 8) {
            uint32_t stack; memcpy(&stack, &g_scratch[0x228], 4);
            uint16_t count; memcpy(&count, &g_scratch[0x248], 2);
            for (uint32_t i = 0; i < s_sp_cache_count; i++) {
                uint32_t ent = s_sp_cache[i];
                uint32_t phys = ent & 0x1FFFFFFFu;
                if (phys >= 0x200000u || g_ram[phys] == 0) continue;
                int found = 0;
                for (uint32_t j = 0; j < count; j++) {
                    uint32_t sp = (stack + j * 4) & 0x1FFFFFFFu;
                    if (sp < 0x200000u) {
                        uint32_t listed; memcpy(&listed, &g_ram[sp], 4);
                        if (listed == ent) { found = 1; break; }
                    }
                }
                if (!found && count < 50) {
                    stack -= 4;
                    uint32_t sp = stack & 0x1FFFFFFFu;
                    if (sp < 0x200000u) {
                        memcpy(&g_ram[sp], &ent, 4);
                        count++;
                        /* Restore entity[1] so FUN_8004afac's entity[1]!=0 check passes */
                        if (phys + 1 < 0x200000u && g_ram[phys + 1] == 0)
                            g_ram[phys + 1] = s_sp_cache_e1[i] ? s_sp_cache_e1[i] : 1u;
                    }
                }
            }
            memcpy(&g_scratch[0x228], &stack, 4);
            memcpy(&g_scratch[0x248], &count, 2);
        }
    }

    /* ---- Split-function chain fixes ---- */
    /* The recompiler sometimes splits a single MIPS function into multiple C
     * functions when it doesn't detect fall-through.  These overrides execute
     * the preamble and then call the continuation so the full function runs. */
    /* 0x8005FFB8 and 0x80060084 overrides removed — recompiler fix (delay-slot
     * prologue detection) merged these into single functions with correct code. */

    /* ---- OT chain insert with bounds checking ---- */
    if (addr == 0x8005DFD8u) {
        /* FUN_8005dfd8(ot_entry, prim) — inserts prim into OT chain.
         * param_1 (a0) = OT entry address, param_2 (a1) = primitive address.
         * On real PS1, out-of-range OT depth silently corrupts random memory.
         * Here we protect critical regions (TCB, stack) from corruption. */
        g_addprim_count++;
        uint32_t ot_addr = cpu->a0 & 0x1FFFFFFFu;
        uint32_t prim_addr = cpu->a1 & 0x1FFFFFFFu;
        {
            static uint32_t s_ap_diag = 0;
            /* [ADDPRIM] first 30 — re-enable printf when investigating addPrim pointer corruption */
        }
        /* Sanity check: both pointers must be in PS1 RAM (0-2MB) */
        if (ot_addr < 0x200000u && prim_addr < 0x200000u) {
            /* Protect TCB area (0x1FD800-0x1FD94F) and stack (0x1FFC00+) */
            if (ot_addr >= 0x1FD800u && ot_addr < 0x200000u) {
                return 1;  /* OT depth overflow — skip */
            }
            /* Skip insert if primitive is at the clamped draw buffer edge
             * (garbage data there would form invalid OT chain links). */
            if (prim_addr >= 0x000CB000u && prim_addr < 0x000CC000u) {
                return 1;
            }
            uint32_t ot_val, prim_val;
            memcpy(&ot_val, &g_ram[ot_addr], 4);
            memcpy(&prim_val, &g_ram[prim_addr], 4);
            /* prim->tag = (prim->tag & 0xFF000000) | (ot->link & 0x00FFFFFF) */
            prim_val = (prim_val & 0xFF000000u) | (ot_val & 0x00FFFFFFu);
            memcpy(&g_ram[prim_addr], &prim_val, 4);
            /* ot->link = (ot->link & 0xFF000000) | (prim_addr_kseg0 & 0x00FFFFFF) */
            uint32_t prim_kseg0 = prim_addr | 0x80000000u;
            ot_val = (ot_val & 0xFF000000u) | (prim_kseg0 & 0x00FFFFFFu);
            memcpy(&g_ram[ot_addr], &ot_val, 4);
        } else {
            static uint32_t s_ap_rej = 0;
            if (++s_ap_rej <= 20) {
                printf("[ADDPRIM-REJECT] #%u f%u OOB: phys_ot=0x%08X phys_prim=0x%08X (raw: a0=0x%08X a1=0x%08X) ra=0x%08X\n",
                       s_ap_rej, g_ps1_frame, ot_addr, prim_addr, cpu->a0, cpu->a1, cpu->ra);
                fflush(stdout);
            }
        }
        return 1;
    }

    /* ---- FUN_8004f3dc: Sprite/primitive batch builder.
     * The do-while loop uses `s3 = lh *(base + offset)` as a count,
     * decrements per iteration, and loops while s3 != 0.
     * If entity data is uninitialized (overlay tick not dispatched yet),
     * the count can be 0 or negative, causing ~4 billion iterations.
     * Fix: validate count before entering the loop. ---- */
    if (addr == 0x8004F3DCu) {
        /* Compute what s3 would be: *(int16_t*)(*(param+0x18) + (a3 & 0xFFFF) * 4) */
        uint32_t entity = cpu->a0;
        uint32_t a3_idx = (cpu->a3 & 0xFFFF) * 4;
        uint32_t base_ptr = read_word(entity + 0x18);
        int16_t count = (int16_t)read_half(base_ptr + a3_idx);
        if (count <= 0) {
            static uint32_t s_skip = 0;
            if (++s_skip <= 5) {
                printf("[PRIM-SKIP] FUN_8004f3dc count=%d base=0x%08X idx=%u entity=0x%08X f%u\n",
                       (int)count, base_ptr, a3_idx / 4, entity, g_ps1_frame);
                fflush(stdout);
            }
            return 1;  /* skip — don't enter the buggy loop */
        }
        return 0;  /* count is valid, proceed normally */
    }

    /* ---- Init-chain trace (binary-search for hang) — capped at 5 calls each ---- */
    switch (addr) {
        case 0x8005EAB8u: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] func_8005EAB8 sp=0x%08X\n", cpu->sp); fflush(stdout); } break; }
        case 0x80016A18u: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] func_80016A18 sp=0x%08X\n", cpu->sp); fflush(stdout); } break; }
        case 0x8005E694u: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] func_8005E694\n"); fflush(stdout); } break; }
        case 0x8005E92Cu: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] func_8005E92C\n"); fflush(stdout); } break; }
        case 0x8006329Cu: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] func_8006329C\n"); fflush(stdout); } break; }
        case 0x80064664u: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] func_80064664 (CDROM init)\n"); fflush(stdout); } break; }
        case 0x8005CFE8u: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] func_8005CFE8\n"); fflush(stdout); } break; }
        case 0x80016AF4u: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] func_80016AF4\n"); fflush(stdout); } break; }
        case 0x80016A00u: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] func_80016A00\n"); fflush(stdout); } break; }
        case 0x800211A4u: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] func_800211A4\n"); fflush(stdout); } break; }
        case 0x8005DD6Cu: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] func_8005DD6C\n"); fflush(stdout); } break; }
        case 0x80019020u: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] func_80019020\n"); fflush(stdout); } break; }
        case 0x80028728u: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] func_80028728\n"); fflush(stdout); } break; }
        case 0x800163B0u: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] func_800163B0 (game main) sp=0x%08X\n", cpu->sp); fflush(stdout); } break; }
        case 0x800191E0u: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] func_800191E0 (display thread) sp=0x%08X\n", cpu->sp); fflush(stdout); } break; }
        case 0x80021340u: {
            /* [LOAD-ENTRY] printf("[LOAD-ENTRY] loading fiber ENTRY sp=0x%08X qw=%u qr=%u DAT_ce=%u f%u\n", ...); */
            break;
        }
        case 0x80064938u: break; /* [TRACE] func_80064938 (CDROM cmd wrapper) */
        case 0x800171D4u: break; /* yield — pass through */
        /* 0x8005B40C: ChangeThread wrapper — handled below in BIOS WRAPPER INTERCEPTS */
        case 0x80065470u: break; /* [TRACE] FUN_80065470 (CDROM sync wait) */
        case 0x80066084u: break; /* [TRACE] FUN_80066084 (CDROM low-level init) */
        case 0x80066a50u: break; /* [TRACE] FUN_80066a50 (CdRead) */
        case 0x80066b30u: break; /* [TRACE] FUN_80066b30 (CdReadSync) */
        case 0x80017208u: {
            /* [LOAD-DONE] printf("[LOAD-DONE] FUN_80017208 f%u qw=%u qr=%u...\n"); */
            break;
        }
        case 0x800222b8u: break; /* [TRACE] FUN_800222b8 (trigger loading batch) */
        case 0x80017154u: break; /* [TRACE] FUN_80017154 (start loading fiber) */
        case 0x80021bf4u: {
            /* [LOAD-TRIGGER2] printf("[LOAD-TRIGGER2] FUN_80021bf4 f%u qw=%u qr=%u\n", ...); */
            break;
        }
        case 0x800223e0u: break; /* FUN_800223e0 — per-frame update, pass through */
        case 0x8001a954u: { /* FUN_8001a954 — gameplay state handler.
             * Callee-save wrapper: func_8001A51C uses s0=2, s1=1 as switch
             * constants.  Deep call chains inside func_8001A954 (entity handlers
             * via func_8004117C etc.) corrupt s0/s1/sp via direct JAL calls that
             * bypass call_by_address's callee-save protection.
             * Re-entrancy guard: first call wraps with save/restore, recursive
             * call lets the real body execute. */
            static int s_inside_a954 = 0;
            if (!s_inside_a954) {
                s_inside_a954 = 1;
                uint32_t save_sp = cpu->sp, save_fp = cpu->fp;
                uint32_t save_s0 = cpu->s0, save_s1 = cpu->s1;
                uint32_t save_s2 = cpu->s2, save_s3 = cpu->s3;
                uint32_t save_s4 = cpu->s4, save_s5 = cpu->s5;
                uint32_t save_s6 = cpu->s6, save_s7 = cpu->s7;
                func_8001A954(cpu);
                if (cpu->s0 != save_s0 || cpu->s1 != save_s1 ||
                    cpu->sp != save_sp) {
                    static uint32_t s_fix = 0;
                    if (++s_fix <= 5 || s_fix % 1000 == 0) {
                        printf("[CALLEE-FIX-A954] #%u f%u sp:%08X->%08X s0:%08X->%08X s1:%08X->%08X\n",
                               s_fix, g_ps1_frame,
                               save_sp, cpu->sp, save_s0, cpu->s0,
                               save_s1, cpu->s1);
                        fflush(stdout);
                    }
                }
                cpu->sp = save_sp; cpu->fp = save_fp;
                cpu->s0 = save_s0; cpu->s1 = save_s1;
                cpu->s2 = save_s2; cpu->s3 = save_s3;
                cpu->s4 = save_s4; cpu->s5 = save_s5;
                cpu->s6 = save_s6; cpu->s7 = save_s7;
                s_inside_a954 = 0;
                return 1;
            }
            s_inside_a954 = 0;
            break;
        }
        case 0x8001a51cu: { /* FUN_8001a51c — game loop entry */
            static uint32_t s_a51c = 0; ++s_a51c;
            printf("[GAMELOOP] FUN_8001a51c entry #%u f%u sp=0x%08X ra=0x%08X s0=0x%08X s1=0x%08X\n",
                   s_a51c, g_ps1_frame, cpu->sp, cpu->ra, cpu->s0, cpu->s1);
            fflush(stdout);
            break;
        }
        case 0x8001a9f0u: { static uint32_t s_a9f0 = 0; ++s_a9f0; /* [TRACE] FUN_8001a9f0 — commented out */ break; }
        case 0x8001ac00u: { static uint32_t s_ac00 = 0; ++s_ac00; /* [TRACE] FUN_8001ac00 — commented out */ break; }
        case 0x8001b2b4u: { static uint32_t s_b2b4=0; ++s_b2b4; /* [TRACE] FUN_8001b2b4 — commented out */ break; }
        case 0x8001b5a8u: {
            static uint32_t s_b5a8=0; ++s_b5a8;
            /* [C6-CHANGE] / [TRACE] FUN_8001b5a8 — commented out
            static uint8_t s_prev_c6 = 0;
            uint8_t cur_c6 = g_scratch[0x1C6];
            if (cur_c6 != s_prev_c6) {
                uint16_t d_1fc; memcpy(&d_1fc, &g_scratch[0x1FC], 2);
                uint16_t d_c9d8; memcpy(&d_c9d8, &g_ram[0x9c9d8], 2);
                printf("[C6-CHANGE] f%u c6: %u→%u  1fc=0x%04X c9d8=0x%04X eb58=%u cd=%u\n",
                       g_ps1_frame, s_prev_c6, cur_c6, d_1fc, d_c9d8,
                       (uint32_t)g_ram[0x9eb58], (uint32_t)g_scratch[0x3CD]);
                fflush(stdout);
                s_prev_c6 = cur_c6;
            }
            if(s_b5a8<=5||s_b5a8%500==0) {
                printf("[TRACE] FUN_8001b5a8 #%u f%u c6=%u\n", s_b5a8, g_ps1_frame, cur_c6);
                fflush(stdout);
            }
            */
            break;
        }
        case 0x8001d6c0u: {
            static uint32_t s_d6c0=0; ++s_d6c0;
            /* [TRACE] FUN_8001d6c0 — commented out
            uint16_t d_1fc; memcpy(&d_1fc, &g_scratch[0x1FC], 2);
            if(s_d6c0<=10) {
                printf("[TRACE] FUN_8001d6c0 #%u f%u c6=%u 1fc=0x%04X eb58=%u\n",
                       s_d6c0, g_ps1_frame, (uint32_t)g_scratch[0x1C6], d_1fc, (uint32_t)g_ram[0x9eb58]);
                fflush(stdout);
            }
            */
            break;
        }
        case 0x80034524u: { static uint32_t s_4524=0; ++s_4524; /* [TRACE] FUN_80034524 — commented out */ break; }
        case 0x80029008u: { static uint32_t s_9008=0; ++s_9008; /* [TRACE] FUN_80029008 — commented out */ break; }
        case 0x8002907cu: { static uint32_t s_907c=0; ++s_907c; /* [TRACE] FUN_8002907c — commented out */ break; }
        case 0x80029c48u: { static uint32_t s_9c48b=0; ++s_9c48b; /* [TRACE] FUN_80029c48 — commented out */ break; }
        case 0x8002BB9Cu: {
            /* FUN_8002bb9c — player movement state machine.
             * param_1 (a0) = player struct. state at param_1+0x6c.
             * bca2 must be non-zero for movement to process.
             * DAT_800a5438 = terrain contact flags (0x02=grounded, 0x10=slope, 0x12=on_surface)
             * DAT_800a5436 = terrain type/mode
             * _DAT_1f8000e6 = horizontal velocity (scratchpad 0x0E6) */
            static uint32_t s_bb9c = 0; ++s_bb9c;
            if (s_bb9c <= 20) {  /* first 20 only — removed periodic every-200 to reduce log volume */
                uint32_t pstruct = cpu->a0;
                uint8_t state  = (pstruct >= 0x80000000u && (pstruct-0x80000000u)+0x70 <= sizeof(g_ram))
                                 ? g_ram[(pstruct-0x80000000u)+0x6c] : 0xFF;
                uint8_t facing = (pstruct >= 0x80000000u && (pstruct-0x80000000u)+0x70 <= sizeof(g_ram))
                                 ? g_ram[(pstruct-0x80000000u)+0x6e] : 0xFF;
                uint8_t bca2 = g_ram[0x9bca2];
                uint8_t bca7 = g_ram[0x9bca7];
                uint16_t c9d8; memcpy(&c9d8, &g_ram[0x9c9d8], 2);
                uint16_t fc;   memcpy(&fc,   &g_scratch[0x1FC], 2);
                uint32_t a5438; memcpy(&a5438, &g_ram[0x0a5438], 4);
                uint8_t  a5436 = g_ram[0x0a5436];
                int16_t  vel;  memcpy(&vel,  &g_scratch[0x0E6], 2);
                uint32_t bcc8; memcpy(&bcc8, &g_ram[0x9bcc8], 4);
                /* terrain data pointer at player+0x34 (used by overlay 0x80115AA8) */
                uint32_t terr_ptr = 0;
                if (pstruct >= 0x80000000u && (pstruct-0x80000000u)+0x38 <= sizeof(g_ram))
                    memcpy(&terr_ptr, &g_ram[(pstruct-0x80000000u)+0x34], 4);
                /* entity[0x30]=player X, entity[0x32]=entity state field */
                int16_t ent_x = 0, ent_e32 = 0;
                if (pstruct >= 0x80000000u && (pstruct-0x80000000u)+0x34 <= sizeof(g_ram)) {
                    memcpy(&ent_x,   &g_ram[(pstruct-0x80000000u)+0x30], 2);
                    memcpy(&ent_e32, &g_ram[(pstruct-0x80000000u)+0x32], 2);
                }
                printf("[PLAYER] #%u f%u pstruct=0x%08X state=%u facing=%u bca2=%u bca7=%u c9d8=0x%04X 1fc=0x%04X a5438=0x%08X a5436=%u vel=%d bcc8=0x%X terr=0x%08X x=%d e32=0x%04X\n",
                       s_bb9c, g_ps1_frame, pstruct, (uint32_t)state, (uint32_t)facing,
                       (uint32_t)bca2, (uint32_t)bca7, (uint32_t)c9d8, (uint32_t)fc,
                       a5438, (uint32_t)a5436, (int)vel, bcc8, terr_ptr, (int)ent_x, (uint32_t)(uint16_t)ent_e32);
                fflush(stdout);
            }
            break;
        }
        case 0x8002DA2Cu: {
            /* FUN_8002da2c — area dispatch (calls zone overlay for movement). */
            static uint32_t s_da2c = 0; ++s_da2c;
            /* [AREADISP] first 10 + every 300 — re-enable: if (s_da2c <= 10 || s_da2c % 300 == 0) printf(...); */
            break;
        }
        case 0x80046264u: {
            static uint32_t s_6264b=0; ++s_6264b;
            uint16_t ent_count; memcpy(&ent_count, &g_scratch[0x24A], 2);
            uint32_t ent_list; memcpy(&ent_list, &g_scratch[0x220], 4);
            uint16_t ent_count2; memcpy(&ent_count2, &g_scratch[0x252], 2);
            uint8_t terrain_flag = g_ram[0xA5399];
            uint8_t terrain_mode = g_ram[0xA539A];
            int16_t cam_x, cam_y;
            memcpy(&cam_x, &g_scratch[0x176], 2);
            memcpy(&cam_y, &g_scratch[0x186], 2);
            /* also read list-A count (0x244) to confirm it's always 0 */
            uint16_t ent_count_a; memcpy(&ent_count_a, &g_scratch[0x244], 2);
            /* Read all entity list counts */
            uint16_t lc, ld, le, lf, lg, lh;
            memcpy(&lc, &g_scratch[0x246], 2);
            memcpy(&ld, &g_scratch[0x24C], 2);
            memcpy(&le, &g_scratch[0x240], 2);
            memcpy(&lf, &g_scratch[0x242], 2);
            memcpy(&lg, &g_scratch[0x248], 2);
            memcpy(&lh, &g_scratch[0x24E], 2);
            int32_t gte_trx, gte_try, gte_trz;
            memcpy(&gte_trx, &g_scratch[0xD4], 4);
            memcpy(&gte_try, &g_scratch[0xD8], 4);
            memcpy(&gte_trz, &g_scratch[0xDC], 4);
            if(DIAG_ENABLED()) {
                printf("[RENDER] #%u f%u A=%u B=%u C=%u D=%u E=%u F=%u G=%u H=%u cam=(%d,%d) TRX=%d TRY=%d TRZ=%d terrain=%u mode=%u\n",
                       s_6264b, g_ps1_frame,
                       (uint32_t)ent_count_a, (uint32_t)ent_count,
                       (uint32_t)lc, (uint32_t)ld, (uint32_t)le, (uint32_t)lf,
                       (uint32_t)lg, (uint32_t)lh,
                       (int)cam_x, (int)cam_y, gte_trx, gte_try, gte_trz, terrain_flag, terrain_mode);
                fflush(stdout);
            }
            break;
        }
        case 0x8004637Cu: {
            static uint32_t s_637c=0; ++s_637c;
            uint8_t terrain_flag = g_ram[0xA5399];
            uint8_t terrain_mode = g_ram[0xA539A];
            /* Scan OT for non-empty entries BEFORE terrain renderer runs */
            uint32_t ot_base_addr = 0;
            memcpy(&ot_base_addr, &g_scratch[0x1E0], 4);
            uint32_t ot_phys = ot_base_addr & 0x1FFFFFu;
            uint32_t pre_nonempty = 0;
            if (ot_phys + 808*4 <= sizeof(g_ram)) {
                for (uint32_t oi = 0; oi < 808; oi++) {
                    uint32_t entry; memcpy(&entry, &g_ram[ot_phys + oi*4], 4);
                    if ((entry >> 24) > 0) pre_nonempty++;
                }
            }
            if(DIAG_ENABLED()) {
                printf("[TERRAIN-DISP] #%u f%u enable=%u mode=%u ot=0x%08X pre_nonempty=%u\n",
                       s_637c, g_ps1_frame, terrain_flag, terrain_mode, ot_base_addr, pre_nonempty);
                fflush(stdout);
            }
            break;
        }
        case 0x80046428u: {
            static uint32_t s_6428=0; ++s_6428;
            uint32_t pkt_ptr; memcpy(&pkt_ptr, &g_scratch[0x164], 4);
            uint32_t ot_base; memcpy(&ot_base, &g_scratch[0x1E0], 4);
            /* Read key terrain structure fields from a0 (= s2 inside func).
             * a0 points to the terrain struct at 0x800A5398.
             * +36 (0x24) = vertex data pointer
             * +60 (0x3C) = another data table pointer
             * +140 (0x8C) = angle/rotation value used for trig tables */
            uint32_t vert_ptr = 0, data_tbl = 0;
            uint32_t angle_val = 0;
            uint8_t* a0p = addr_ptr(cpu->a0);
            if (a0p) {
                memcpy(&vert_ptr, a0p + 36, 4);
                memcpy(&data_tbl, a0p + 60, 4);
                memcpy(&angle_val, a0p + 140, 4);
            }
            /* One-time dump of the terrain struct fields */
            static int s_struct_dumped = 0;
            if (!s_struct_dumped) {
                s_struct_dumped = 1;
                printf("[TERRAIN-STRUCT] Dumping terrain struct at 0x800A5398:\n");
                for (int i = 0; i < 20; i++) {
                    uint32_t val = *(uint32_t*)(g_ram + 0xA5398 + i*4);
                    printf("[TERRAIN-STRUCT]   +0x%02X = 0x%08X\n", i*4, val);
                }
                fflush(stdout);
            }
            /* One-time dump of the data table contents */
            static int s_dtbl_dumped = 0;
            if (!s_dtbl_dumped) {
                s_dtbl_dumped = 1;
                uint32_t dtbl_addr = *(uint32_t*)(g_ram + 0xA53B4); /* terrain_struct+0x1C = data table ptr */
                uint32_t dtbl_phys = dtbl_addr & 0x1FFFFF;
                printf("[TERRAIN-DTBL] data_table=0x%08X phys=0x%X\n", dtbl_addr, dtbl_phys);
                printf("[TERRAIN-DTBL] First 40 entries (idx, chunk_count, offset):\n");
                for (int i = 0; i < 40; i++) {
                    uint32_t entry_addr = dtbl_phys + i * 4;
                    if (entry_addr + 4 <= 0x200000) {
                        uint32_t val = *(uint32_t*)(g_ram + entry_addr);
                        printf("[TERRAIN-DTBL]   [%d] = 0x%08X (%u)\n", i, val, val);
                    }
                }
                fflush(stdout);
            }
            if(DIAG_ENABLED()) {
                /* Check if the data table pointer is in overlay territory and if data is loaded */
                uint32_t dtbl_phys = data_tbl & 0x1FFFFFu;
                uint8_t dtbl_sample[16] = {0};
                if (dtbl_phys + 16 <= sizeof(g_ram))
                    memcpy(dtbl_sample, g_ram + dtbl_phys, 16);
                /* Also read the vertex pointer to get chunk index */
                uint32_t vert_phys = vert_ptr & 0x1FFFFFu;
                int16_t chunk_idx = 0;
                if (vert_phys + 2 <= sizeof(g_ram))
                    memcpy(&chunk_idx, g_ram + vert_phys, 2);
                /* Also read scratchpad 0x74 (Z-depth from RTPS >> 2) and 0x70 (SXY from RTPS) */
                uint32_t sz_val = 0, sxy_val = 0;
                memcpy(&sz_val, &g_scratch[0x74], 4);
                memcpy(&sxy_val, &g_scratch[0x70], 4);
                /* Read param_1+0xf (terrain_z_offset byte) */
                uint8_t z_offset = 0;
                uint32_t a0p_0f = (cpu->a0 & 0x1FFFFFu) + 0xF;
                if (a0p_0f < sizeof(g_ram)) z_offset = g_ram[a0p_0f];
                /* OT range check: (z_offset + sz_val) * 4 + 0x10 must be < 0xCA0 from OT base */
                int32_t ot_idx = ((int8_t)z_offset + (int32_t)sz_val) * 4;
                /* Read chunk count from data table: puVar11 = dtbl + chunk_idx*4, local_70 = *puVar11 */
                uint16_t chunk_count = 0;
                uint32_t puVar11_addr = data_tbl + chunk_idx * 4;
                uint32_t puVar11_phys = puVar11_addr & 0x1FFFFFu;
                if (puVar11_phys + 2 <= sizeof(g_ram))
                    memcpy(&chunk_count, g_ram + puVar11_phys, 2);
                /* Read the data pointer (offset from puVar11[1]) */
                int16_t data_off = 0;
                if (puVar11_phys + 4 <= sizeof(g_ram))
                    memcpy(&data_off, g_ram + puVar11_phys + 2, 2);
                printf("[TERRAIN-R0] #%u f%u pkt=0x%08X ot=0x%08X SZ=%d SXY=0x%08X z_off=%d ot_idx=%d(max=3232) "
                       "idx=%d chunks=%u data_off=%d TR=[%08X,%08X,%08X]\n",
                       s_6428, g_ps1_frame, pkt_ptr, ot_base, sz_val, sxy_val, (int8_t)z_offset, ot_idx,
                       chunk_idx, chunk_count, data_off,
                       cpu->gte_ctrl[5], cpu->gte_ctrl[6], cpu->gte_ctrl[7]);
                fflush(stdout);
            }
            break;
        }
        case 0x80046CDCu: {
            /* Entity render loop — iterates entity lists from scratchpad and dispatches by type.
             * Reads list ptr from scratch[0x218], count from scratch[0x244].
             * Dispatches: type 0 -> FUN_80046ec0, type 2 -> FUN_80049134,
             *             type 0xd -> FUN_8004a6a0, type 0x11 -> FUN_8004d2a8 */
            static uint32_t s_6cdc = 0; ++s_6cdc;
            static uint32_t s_6cdc_last_frame = 0xFFFFFFFF;
            static uint32_t s_6cdc_per_frame = 0;
            if (g_ps1_frame != s_6cdc_last_frame) {
                /* New frame — print previous frame's summary if we had data */
                if (s_6cdc_last_frame != 0xFFFFFFFF && (s_6cdc <= 60 || s_6cdc % 200 == 0)) {
                    /* (printed below on first call of new frame) */
                }
                s_6cdc_per_frame = 0;
                s_6cdc_last_frame = g_ps1_frame;
            }
            s_6cdc_per_frame++;

            /* Read the entity list head pointer and count from scratchpad */
            uint32_t ent_list_ptr; memcpy(&ent_list_ptr, &g_scratch[0x218], 4);
            uint16_t ent_count_244; memcpy(&ent_count_244, &g_scratch[0x244], 2);
            uint8_t c6_val = g_scratch[0x1C6];
            uint16_t ent_24e; memcpy(&ent_24e, &g_scratch[0x24E], 2);
            uint32_t ent_25c; memcpy(&ent_25c, &g_scratch[0x25C], 4);

            /* Walk the entity list and count entries + type distribution */
            uint32_t walk_count = 0;
            uint32_t type_counts[256] = {0};
            uint32_t first_ent_addr = 0;
            uint32_t list_phys = ent_list_ptr & 0x1FFFFFu;
            if (c6_val == 0) {
                /* c6==0 path: uses scratch[0x244] as count, scratch[0x218] as list ptr array */
                uint32_t cnt = ent_count_244;
                uint32_t lp = list_phys;
                for (uint32_t i = 0; i < cnt && i < 300; i++) {
                    if (lp + 4 > sizeof(g_ram)) break;
                    uint32_t ent_addr; memcpy(&ent_addr, &g_ram[lp], 4);
                    if (i == 0) first_ent_addr = ent_addr;
                    uint32_t ep = ent_addr & 0x1FFFFFu;
                    if (ep + 11 <= sizeof(g_ram)) {
                        uint8_t active = g_ram[ep + 1];
                        uint8_t etype = g_ram[ep + 10];
                        if (active != 0) type_counts[etype]++;
                    }
                    walk_count++;
                    lp += 4;
                }
            } else {
                /* c6!=0 path: uses scratch[0x24E] as count, scratch[0x25C] as list ptr array */
                uint32_t cnt = ent_24e;
                uint32_t lp = ent_25c & 0x1FFFFFu;
                for (uint32_t i = 0; i < cnt && i < 300; i++) {
                    if (lp + 4 > sizeof(g_ram)) break;
                    uint32_t ent_addr; memcpy(&ent_addr, &g_ram[lp], 4);
                    if (i == 0) first_ent_addr = ent_addr;
                    uint32_t ep = ent_addr & 0x1FFFFFu;
                    if (ep + 11 <= sizeof(g_ram)) {
                        uint8_t active = g_ram[ep + 1];
                        uint8_t etype = g_ram[ep + 10];
                        if (active != 0) type_counts[etype]++;
                    }
                    walk_count++;
                    lp += 4;
                }
            }

            if (DIAG_ENABLED()) {
                printf("[ENT-LOOP] #%u f%u call_in_frame=%u c6=%u list=0x%08X count_244=%u "
                       "ent_24e=%u ent_25c=0x%08X walked=%u first=0x%08X "
                       "t0=%u t2=%u t13=%u t17=%u\n",
                       s_6cdc, g_ps1_frame, s_6cdc_per_frame, (uint32_t)c6_val,
                       ent_list_ptr, (uint32_t)ent_count_244,
                       (uint32_t)ent_24e, ent_25c, walk_count, first_ent_addr,
                       type_counts[0], type_counts[2], type_counts[0xd], type_counts[0x11]);
                fflush(stdout);
            }
            break;
        }
        case 0x80022E44u: {
            /* FUN_80022E44: entity visibility check + render-list sort.
             * Visibility: X = *(short*)(*(int*)(ent+0x40)+2) - scratch[0x176]
             *             Y = scratch[0x186] - *(short*)(ent+0x16)
             * a0 = entity pointer.
             */
            static uint32_t s_2e44_frame = 0xFFFFFFFF;
            static uint32_t s_2e44_calls = 0;
            static uint32_t s_2e44_fail_x = 0, s_2e44_fail_y = 0, s_2e44_pass = 0;
            static uint32_t s_2e44_total_frames = 0;
            /* Scratchpad dump once at gameplay start */
            static int s_scratch_dumped = 0;
            if (!s_scratch_dumped && g_ps1_frame >= 1674) {
                s_scratch_dumped = 1;
                printf("[VIS-SCRATCH] scratchpad 0x160-0x1A0:\n");
                for (int si = 0x160; si < 0x1A0; si += 4) {
                    uint32_t v; memcpy(&v, &g_scratch[si], 4);
                    printf("[VIS-SCRATCH]  +0x%03X = 0x%08X\n", si, v);
                }
                fflush(stdout);
            }
            if (g_ps1_frame != s_2e44_frame) {
                if (s_2e44_frame != 0xFFFFFFFF && s_2e44_total_frames <= 5) {
                    printf("[VIS] frame %u: calls=%u pass=%u fail_x=%u fail_y=%u\n",
                           s_2e44_frame, s_2e44_calls, s_2e44_pass, s_2e44_fail_x, s_2e44_fail_y);
                    fflush(stdout);
                }
                s_2e44_frame = g_ps1_frame;
                s_2e44_calls = 0; s_2e44_fail_x = 0; s_2e44_fail_y = 0; s_2e44_pass = 0;
                s_2e44_total_frames++;
            }
            s_2e44_calls++;
            /* Simulate the X and Y checks to see which fails */
            int16_t cam_x, cam_y;
            memcpy(&cam_x, &g_scratch[0x176], 2);
            memcpy(&cam_y, &g_scratch[0x186], 2);
            uint32_t ent_addr = cpu->a0;
            uint32_t ep = ent_addr & 0x1FFFFFu;
            /* X check: *(short*)(*(int*)(ent+0x40)+2) */
            int16_t ent_screen_x = 0;
            if (ep + 0x44 <= sizeof(g_ram)) {
                uint32_t ptr40; memcpy(&ptr40, &g_ram[ep + 0x40], 4);
                uint32_t pp = ptr40 & 0x1FFFFFu;
                if (pp + 4 <= sizeof(g_ram))
                    memcpy(&ent_screen_x, &g_ram[pp + 2], 2);
            }
            int16_t ent_y = 0;
            if (ep + 0x18 <= sizeof(g_ram))
                memcpy(&ent_y, &g_ram[ep + 0x16], 2);
            uint16_t x_check = (uint16_t)((int)(ent_screen_x - cam_x) + 0x40);
            uint16_t y_check = (uint16_t)((int)(cam_y - ent_y) + 0x40);
            int x_pass = x_check < 0x1c1;
            int y_pass = y_check < 0x171;
            if (!x_pass) s_2e44_fail_x++;
            else if (!y_pass) s_2e44_fail_y++;
            else s_2e44_pass++;
            break;
        }
        case 0x8004DD14u: {
            /* Renderer for entity list E (layer 7), reads count at 0x240, ptr at 0x22C */
            static uint32_t s_dd14 = 0; ++s_dd14;
            uint16_t e_count_in; memcpy(&e_count_in, &g_scratch[0x240], 2);
            if (DIAG_ENABLED()) {
                printf("[E-RENDER] #%u f%u list-E count at entry=%u\n", s_dd14, g_ps1_frame, (uint32_t)e_count_in);
                fflush(stdout);
            }
            break;
        }
        case 0x8004DD1Cu: {
            /* E list renderer body. c6 (scratchpad[0x1C6]) was read into v0 by func_8004DD14.
             * c6!=0: original goto-based loop (c6!=0 path) works fine — let it run.
             * c6==0: original uses jump table at 0x80013F18 whose entries are intra-function
             *         labels (0x8004DED0 etc.) that call_by_address cannot dispatch to,
             *         causing a silent return after the first entity. Override with correct loop. */
            if (cpu->v0 != 0) return 0;  /* c6!=0: let original run */

            /* c6==0: process live E list */
            uint16_t e_count; memcpy(&e_count, &g_scratch[0x240], 2);
            uint32_t e_ptr;   memcpy(&e_ptr,   &g_scratch[0x22C], 4);

            /* Save live state to saved-copy slots (as original does) */
            memcpy(&g_scratch[0x25A], &e_count, 2);
            memcpy(&g_scratch[0x270], &e_ptr,   4);

            while (e_count != 0) {
                /* Pop entity address from descending stack */
                uint32_t entity_addr;
                memcpy(&entity_addr, &g_ram[e_ptr & 0x1FFFFFu], 4);
                e_ptr  += 4;
                e_count -= 1;
                memcpy(&g_scratch[0x22C], &e_ptr,   4);
                memcpy(&g_scratch[0x240], &e_count, 2);

                uint8_t type = g_ram[(entity_addr & 0x1FFFFFu) + 10];
                cpu->a0 = entity_addr;

                if (type < 5) {
                    switch (type) {
                        case 0:
                            call_by_address(cpu, 0x8004A300u);
                            break;
                        case 1:
                            call_by_address(cpu, 0x8004DFA0u);
                            break;
                        case 2:
                            call_by_address(cpu, 0x8004E244u);
                            break;
                        case 3: {
                            uint8_t terrain_mode = g_ram[0xA539Au];
                            if (terrain_mode & 2)
                                call_by_address(cpu, 0x800EBA58u);
                            else
                                call_by_address(cpu, 0x800EA3A4u);
                            break;
                        }
                        case 4: {
                            /* Mirrors original: call 0x8011DA28 if bcc8==4, then call
                             * 0x80116308 if bcc8==12 (re-checked after first call) */
                            uint16_t bcc8; memcpy(&bcc8, &g_ram[0x9BCC8u], 2);
                            if (bcc8 == 4) {
                                call_by_address(cpu, 0x8011DA28u);
                                memcpy(&bcc8, &g_ram[0x9BCC8u], 2);
                            }
                            if (bcc8 == 12)
                                call_by_address(cpu, 0x80116308u);
                            break;
                        }
                        default: break;
                    }
                }

                /* Re-read loop variables — handlers may modify scratchpad */
                memcpy(&e_count, &g_scratch[0x240], 2);
                memcpy(&e_ptr,   &g_scratch[0x22C], 4);
            }
            return 1;
        }
        case 0x80046EC0u: {
            /* Type-0 entity renderer — general entities (sprites, platforms, etc.)
             * param_1 (a0) = entity pointer. Reads fields:
             *   +0x01 = active flag, +0x0A = type byte, +0x0D = sprite idx,
             *   +0x0F = z-depth, +0x12/+0x16 = XY, +0x1A = scale, +0x2E = flags, +0x8C = angle */
            static uint32_t s_6ec0 = 0; ++s_6ec0;
            uint32_t ent_addr = cpu->a0;
            uint32_t ep = ent_addr & 0x1FFFFFu;
            uint8_t etype = 0, active = 0, sprite_idx = 0, z_depth = 0;
            int16_t pos_x = 0, pos_y = 0, scale = 0;
            uint16_t flags = 0;
            uint32_t angle = 0;
            if (ep + 0x90 <= sizeof(g_ram)) {
                active = g_ram[ep + 1];
                etype = g_ram[ep + 10];
                sprite_idx = g_ram[ep + 0x0D];
                z_depth = g_ram[ep + 0x0F];
                memcpy(&pos_x, &g_ram[ep + 0x12], 2);
                memcpy(&pos_y, &g_ram[ep + 0x16], 2);
                memcpy(&scale, &g_ram[ep + 0x1A], 2);
                memcpy(&flags, &g_ram[ep + 0x2E], 2);
                memcpy(&angle, &g_ram[ep + 0x8C], 4);
            }
            /* Read OT base and pkt ptr for context */
            uint32_t ot_base; memcpy(&ot_base, &g_scratch[0x1E0], 4);
            uint32_t pkt_ptr; memcpy(&pkt_ptr, &g_scratch[0x164], 4);
            uint32_t sz_val; memcpy(&sz_val, &g_scratch[0x74], 4);

            if (s_6ec0 <= 80 || s_6ec0 % 500 == 0 || DIAG_ENABLED()) {
                printf("[ENT-TYPE0] #%u f%u ent=0x%08X active=%u type=%u sprite=%u z=%u "
                       "pos=(%d,%d) scale=%d flags=0x%04X angle=0x%X "
                       "ot=0x%08X pkt=0x%08X sz=%u\n",
                       s_6ec0, g_ps1_frame, ent_addr, (uint32_t)active, (uint32_t)etype,
                       (uint32_t)sprite_idx, (uint32_t)z_depth,
                       (int)pos_x, (int)pos_y, (int)scale, (uint32_t)flags,
                       angle, ot_base, pkt_ptr, sz_val);
                fflush(stdout);
            }
            break;
        }
        case 0x8003DCF0u: {
            /* FUN_8003dcf0 — cutscene script opcode 0x97 handler.
             * Clears bca7 when uVar1 (script param at _DAT_8009e458+0x1190) == 1.
             * _DAT_8009ca04 & 2 selects mode A vs B. */
            static uint32_t s_dcf0 = 0; ++s_dcf0;
            uint32_t e458; memcpy(&e458, &g_ram[0x9E458], 4);
            uint32_t uVar1 = 0, uVar2 = 0;
            if (e458 >= 0x80000000u && (e458 - 0x80000000u) + 0x11A0 <= sizeof(g_ram)) {
                uint32_t off = e458 - 0x80000000u;
                memcpy(&uVar1, &g_ram[off + 0x1190], 4);
                memcpy(&uVar2, &g_ram[off + 0x1194], 4);
            }
            uint8_t ca04 = g_ram[0x9CA04];
            uint8_t bca7 = g_ram[0x9BCA7];
            uint8_t bca2 = g_ram[0x9BCA2];
            if (s_dcf0 <= 30 || s_dcf0 % 100 == 0) {
                printf("[DCF0] #%u f%u e458=0x%08X uVar1=%u uVar2=%u ca04=0x%02X bca7=%u bca2=%u\n",
                       s_dcf0, g_ps1_frame, e458, uVar1, uVar2, ca04, bca7, bca2);
                fflush(stdout);
            }
            return 0;  /* let original run */
        }
        case 0x8004e3ecu: {
            static uint32_t s_e3ec=0; ++s_e3ec;
            uint8_t c618 = g_ram[0x9C618];
            uint8_t bca7 = g_ram[0x9BCA7];
            uint16_t b0778; memcpy(&b0778, &g_ram[0xB0778], 2);
            uint8_t b0770 = g_ram[0xB0770];
            uint8_t bcc8; memcpy(&bcc8, &g_ram[0x9BCC8], 1);
            if(DIAG_ENABLED()) {
                printf("[DRAW-GATE] #%u f%u c618=%u bca7=%u b0778=%u b0770=0x%02X bcc8=%u\n",
                       s_e3ec, g_ps1_frame, c618, bca7, b0778, b0770, bcc8);
                fflush(stdout);
            }
            break;
        }
        case 0x8004e468u: {
            static uint32_t s_e468=0; ++s_e468;
            if(DIAG_ENABLED()) {
                printf("[ENTITY-DRAW] #%u f%u a0=0x%08X\n", s_e468, g_ps1_frame, cpu->a0);
                fflush(stdout);
            }
            break;
        }
        case 0x8004e590u: { static uint32_t s=0; ++s; /* [DRAW-SUB] e590 */ break; }
        case 0x8004e714u: { static uint32_t s=0; ++s; /* [DRAW-SUB] e714 */ break; }
        case 0x8004e900u: { static uint32_t s=0; ++s; /* [DRAW-SUB] e900 */ break; }
        case 0x8004eb10u: { static uint32_t s=0; ++s; /* [DRAW-SUB] eb10 */ break; }
        case 0x8004ed80u: { static uint32_t s=0; ++s; /* [DRAW-SUB] ed80 */ break; }
        case 0x8004f24cu: { static uint32_t s=0; ++s; /* [DRAW-SUB] f24c */ break; }
        case 0x8001f6d4u: { static uint32_t s_f6d4b=0; if(++s_f6d4b<=3) { printf("[TRACE] FUN_8001f6d4 (post-draw) #%u\n", s_f6d4b); fflush(stdout); } break; }
        case 0x8003c9d4u: {
            static uint32_t s_c9d4=0; ++s_c9d4;
            /* Reset per-frame VM opcode counter each time the VM is entered */
            extern uint32_t g_vm_ops_this_frame;
            g_vm_ops_this_frame = 0;
            /* Trace ec30/fd78/fd7c to diagnose cutscene script stall */
            uint8_t ec30 = g_ram[0x9EC30];
            uint32_t fd78; memcpy(&fd78, &g_ram[0x9FD78], 4);
            uint32_t fd7c; memcpy(&fd7c, &g_ram[0x9FD7C], 4);
            uint16_t script_ip; memcpy(&script_ip, &g_ram[0x9EC32], 2);
            uint32_t ec2c; memcpy(&ec2c, &g_ram[0x9EC2C], 4);
            uint8_t cur_op = 0;
            if (ec2c >= 0x80000000u) {
                uint32_t base_off = (ec2c & 0x1FFFFF) + script_ip;
                if (base_off < sizeof(g_ram)) cur_op = g_ram[base_off];
            }
            uint16_t cur_bcc8; memcpy(&cur_bcc8, &g_ram[0x9BCC8], 2);
            uint8_t cur_bcca = g_ram[0x9BCCA];
            /* Trace entity 0x800ABE78 (script slot 0 / Tomba) key fields */
            {
                static int s_ent0_trc = 0;
                if (DIAG_ENABLED()) {
                    uint8_t e0b2 = g_ram[0xABE7A];   /* entity[+2] = type */
                    uint8_t e0b4 = g_ram[0xABE7C];   /* entity[+4] = state */
                    uint8_t e0b1c = g_ram[0xABE94];  /* entity[+0x1c] */
                    uint8_t e0f6a = g_ram[0xABEE2];  /* entity[+0x6A] */
                    printf("[ENT0] f%u type=0x%02X b4=%u b1c=0x%02X f6a=%u\n",
                           g_ps1_frame, e0b2&0x7F, e0b4, e0b1c, e0f6a);
                    fflush(stdout);
                    if (e0f6a == 1 && !s_ent0_trc) s_ent0_trc = 1; /* mark once f6a becomes 1 */
                }
            }
            if(DIAG_ENABLED()) {
                printf("[C9D4] #%u f%u ec30=%u fd78=%u fd7c=%u bca7=%u bcc8=%u bcca=%u ec2c=0x%08X IP=0x%04X op=0x%02X\n",
                       s_c9d4, g_ps1_frame, ec30, fd78, fd7c, g_ram[0x9BCA7], cur_bcc8, cur_bcca, ec2c, script_ip, cur_op);
                fflush(stdout);
            }
            break;
        }
        /* FUN_8003B860 — VM conditional branch handler.
         * Previously overridden due to split-function bug. Now fixed in recompiler
         * (delay-slot prologue detection in function_analysis.cpp). The generated
         * code correctly handles the full function (312 bytes, 18 blocks). */
        /* Scripting VM opcode handlers — trace + yield to avoid hang */
        case 0x8003c124u: {
            extern uint32_t g_vm_ops_this_frame;
            ++g_vm_ops_this_frame;
            /* Trace first 30 ops — only when diagnostics enabled */
            if (DIAG_ENABLED() && g_vm_ops_this_frame <= 30) {
                uint16_t ip; memcpy(&ip, &g_ram[0x9EC32], 2);
                uint8_t flag89 = 0;
                uint32_t ctx; memcpy(&ctx, &g_ram[0x9E458], 4);
                if (ctx >= 0x80000000u) flag89 = g_ram[(ctx & 0x1FFFFF) + 0x89];
                printf("[VM-TRACE] op#%u opcode=0x%02X IP=0x%04X flag89=%u\n",
                       g_vm_ops_this_frame, cpu->a0 & 0xFF, ip, flag89);
                fflush(stdout);
            }
            /* Force yield at 500 to prevent hang (but now we know the cause) */
            if (g_vm_ops_this_frame > 500u) {
                cpu->v0 = 0;
                return 1;
            }
            break;
        }
        case 0x8003e408u: {
            extern uint32_t g_vm_ops_this_frame;
            ++g_vm_ops_this_frame;
            /* Trace high opcodes — only when diagnostics enabled */
            if (DIAG_ENABLED() && g_vm_ops_this_frame <= 30) {
                uint16_t ip; memcpy(&ip, &g_ram[0x9EC32], 2);
                printf("[VM-TRACE] op#%u HIGH opcode a0=0x%08X IP=0x%04X\n",
                       g_vm_ops_this_frame, cpu->a0, ip);
                fflush(stdout);
            }
            if (g_vm_ops_this_frame > 500u) {
                cpu->v0 = 0;
                return 1;
            }
            break;
        }
        case 0x8001dfd4u: {
            static uint32_t s_dfd4=0; ++s_dfd4;
            extern uint32_t g_vm_ops_this_frame;
            /* [POST-VM] FUN_8001dfd4 — commented out
            if(s_dfd4<=20||s_dfd4%200==0) {
                printf("[POST-VM] #%u f%u vm_ops=%u\n", s_dfd4, g_ps1_frame, g_vm_ops_this_frame);
                fflush(stdout);
            }
            */
            break;
        }
        case 0x8003438cu: { static uint32_t s_438c=0; ++s_438c; /* [TRACE] FUN_8003438c — commented out */ break; }
        case 0x8005a074u: { static uint32_t s_a074=0; ++s_a074; /* [TRACE] FUN_8005a074 — commented out */ break; }
        case 0x80055ba0u: { static uint32_t s_5ba0=0; ++s_5ba0; /* [TRACE] FUN_80055ba0 — commented out */ break; }
        /* 0x80029c48: traced above with more detail */
        /* 0x8002da2cu: traced above with bcc8/bca detail */
        case 0x8002db3cu: { static uint32_t s_db3c=0; ++s_db3c; /* [TRACE] FUN_8002db3c — commented out */ break; }
        case 0x800246B0u: {
            static uint32_t s_246b0 = 0; ++s_246b0;
            /* [TRACE-246B0] — commented out
            if (s_246b0 <= 20) {
                uint16_t bcc8; memcpy(&bcc8, &g_ram[0x09BCC8], 2);
                uint8_t  bcca  = g_ram[0x09BCCA];
                uint16_t bcea; memcpy(&bcea, &g_ram[0x09BCEA], 2);
                uint16_t ic8;  memcpy(&ic8,  &g_scratch[0x1C8], 2);
                uint32_t ec_val; memcpy(&ec_val, &g_scratch[0xEC], 4);
                printf("[TRACE-246B0] #%u f%u bcc8=%u bcca=%u bcea=%u scr[1C8]=%u scr[EC]=0x%08X\n",
                       s_246b0, g_ps1_frame, bcc8, bcca, bcea, ic8, ec_val);
                fflush(stdout);
            }
            */
            break;
        }
        /* 0x80046264: traced above */
        case 0x80017614u: { static uint32_t s_7614=0; ++s_7614; /* [RENDER-TRACE] FUN_80017614 — commented out */ break; }
        /* 0x8001f6d4: traced above */
        /* Display thread key states */
        case 0x8001964cu: {
            static uint32_t s_964c = 0;
            ++s_964c;
            /* [RENDER-TRACE] FUN_8001964c — commented out
            if (s_964c <= 20 || s_964c % 100 == 0 || DIAG_ENABLED()) {
                uint32_t ot_base = 0, pkt_ptr = 0;
                memcpy(&ot_base, &g_scratch[0x1E0], 4);
                memcpy(&pkt_ptr, &g_scratch[0x164], 4);
                printf("[RENDER-TRACE] FUN_8001964c #%u f%u sp=0x%08X OT_base=0x%08X pkt_buf=0x%08X ot[1]=", s_964c, g_ps1_frame, cpu->sp, ot_base, pkt_ptr);
                if (ot_base >= 0x80010000u && ot_base < 0x80200000u) {
                    uint32_t ot1; memcpy(&ot1, &g_ram[(ot_base & 0x1FFFFFu) + 4], 4);
                    printf("0x%08X", ot1);
                } else printf("N/A");
                printf("\n"); fflush(stdout);
            }
            */
            break;
        }
        case 0x8005f1c8u: { /* PutDrawEnv — trace draw env buffer + first E-command */
            static uint32_t s_pde = 0;
            if (++s_pde <= 10) {
                /* a0 = DRAWENV struct pointer; command buffer at a0+0x1C (word offset 7) */
                uint32_t buf_addr = cpu->a0 + 0x1C;
                uint8_t* buf = addr_ptr(buf_addr);
                if (buf) {
                    uint32_t hdr, w1, w2, w3;
                    memcpy(&hdr, buf, 4); memcpy(&w1, buf+4, 4);
                    memcpy(&w2, buf+8, 4); memcpy(&w3, buf+12, 4);
                    printf("[PUT-DRAW-ENV] #%u f%u a0=0x%08X buf=0x%08X hdr=%08X w1=%08X w2=%08X w3=%08X\n",
                           s_pde, g_ps1_frame, cpu->a0, buf_addr, hdr, w1, w2, w3);
                } else {
                    printf("[PUT-DRAW-ENV] #%u f%u a0=0x%08X buf=0x%08X (NULL)\n",
                           s_pde, g_ps1_frame, cpu->a0, buf_addr);
                }
                fflush(stdout);
            }
            break;
        }
        case 0x8005fcd0u: { /* Build E-commands — trace output buffer and DRAWENV params */
            static uint32_t s_bec = 0;
            if (++s_bec <= 10) {
                /* a0 = output buffer (command words), a1 = DRAWENV params */
                printf("[BUILD-ENV] #%u f%u out=0x%08X params=0x%08X\n",
                       s_bec, g_ps1_frame, cpu->a0, cpu->a1);
                fflush(stdout);
            }
            break;
        }
        case 0x8001f158u: break; /* [TRACE] FUN_8001f158 (display state-1 → state-4) */
        case 0x800172c4u: break; /* [TRACE] FUN_800172c4 (display state-4 game callback) */
        case 0x800223a0u: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] FUN_800223a0 (display state-0 setup)\n"); fflush(stdout); } break; }
        case 0x80016ddcu: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] FUN_80016ddc (display state-10 init)\n"); fflush(stdout); } break; }
        case 0x80019844u: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] FUN_80019844 (game logic loop entry)\n"); fflush(stdout); } break; }
        /* FUN_8005D4D0 handled below in call_by_address proper */
        /* func_800211AC sub-functions */
        case 0x8006BA8Cu: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] func_8006BA8C sp=0x%08X\n", cpu->sp); fflush(stdout); } break; }
        case 0x8006F808u: { static uint32_t _c = 0; ++_c; /* [TRACE] func_8006F808 */ break; }
        case 0x8006F9E8u: { static uint32_t _c = 0; ++_c; /* [TRACE] func_8006F9E8 */ break; }
        case 0x80073AF0u: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] func_80073AF0\n"); fflush(stdout); } break; }
        case 0x800765ACu: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] func_800765AC\n"); fflush(stdout); } break; }
        case 0x80073B24u: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] func_80073B24\n"); fflush(stdout); } break; }
        case 0x8007694Cu: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] func_8007694C\n"); fflush(stdout); } break; }
        case 0x8006E420u: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] func_8006E420\n"); fflush(stdout); } break; }
        /* func_8006BA8C sub-sub-functions */
        case 0x800740D4u: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] FUN_800740D4 sp=0x%08X\n", cpu->sp); fflush(stdout); } break; }
        case 0x80074248u: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] FUN_80074248 (SPU reset) sp=0x%08X\n", cpu->sp); fflush(stdout); } break; }
        case 0x800741CCu: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] FUN_800741CC (SPU event setup)\n"); fflush(stdout); } break; }
        case 0x8006B9A4u: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] FUN_8006B9A4 (SPU voice init)\n"); fflush(stdout); } break; }
        case 0x80071058u: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] FUN_80071058 (SPU channel init)\n"); fflush(stdout); } break; }
        /* 0x8005B3AC/3DC/42C/41C: BIOS wrappers — handled below in BIOS WRAPPER INTERCEPTS */
        case 0x80074DF4u: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] FUN_80074DF4\n"); fflush(stdout); } break; }
        case 0x80074D94u: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] delay sp=0x%08X\n", cpu->sp); fflush(stdout); } break; }
        case 0x800744ECu: { static uint32_t _c = 0; if (++_c <= 5) { printf("[TRACE] FUN_800744EC (SPU DMA write)\n"); fflush(stdout); } break; }
        /* Sound system tracing — capped to avoid log spam during gameplay */
        case 0x8001FFE8u: {
            /* FUN_8001ffe8(sound_id) — enqueue a sound effect */
            static uint32_t s_snd = 0; ++s_snd;
            if (s_snd <= 20) {
                printf("[SND-Q] #%u  sound_id=0x%04X\n", s_snd, cpu->a0 & 0xFFFF);
                fflush(stdout);
            }
            break;
        }
        case 0x80073A2Cu: {
            /* FUN_80073a2c(bank, pitch) — bank status check before play */
            uint32_t bank = cpu->a0 & 0xFF;
            uint8_t  status = (bank < 0x20) ? g_ram[0x9E4A8 + bank] : 0xFF;
            static uint32_t s_bck = 0; ++s_bck;
            /* [SND-BANK] first 20 — re-enable: if (s_bck <= 20) printf(...); */
            break;
        }
        case 0x80073BD8u: {
            /* FUN_80073bd8(sample_idx) — mark SPU sample upload complete */
            static uint32_t s_srd = 0; ++s_srd;
            /* [SND-READY] first 20 — re-enable: if (s_srd <= 20) printf(...); */
            break;
        }
        case 0x80073CD8u: {
            /* FUN_80073cd8 — VAB upload, sets bank status 0→1→2 */
            static uint32_t s_vab = 0; ++s_vab;
            /* [VAB-UPLOAD] first 10 — re-enable: if (s_vab <= 10) printf(...); */
            break;
        }
        case 0x80070CA4u: {
            /* FUN_80070CA4 — SPU flush: reads _DAT_8009b690/92 (KON) then writes to SPU HW. */
            uint16_t kon_lo, kon_hi;
            memcpy(&kon_lo, &g_ram[0x9B690], 2);
            memcpy(&kon_hi, &g_ram[0x9B692], 2);
            static uint32_t s_flush = 0; ++s_flush;
            /* [FLUSH-KON] first 10 — re-enable: if (s_flush <= 10) printf(...); */
            break;
        }
        case 0x8007028Cu: {
            /* FUN_8007028c — try to key on voice.  Log entry args to diagnose guard failures. */
            static uint32_t s_k28 = 0; ++s_k28;
            /* [KON-TRY] first 20 — re-enable: if (s_k28 <= 20) printf(...); */
            break;
        }
        /* Main polling loop functions */
        case 0x80017024u: {
            /* Full C reimplementation — avoids MIPS prologue corrupting TCB[0].
             * Main fiber sp=0x801FD808; scheduler MIPS prologue addiu sp,-0x18 →
             * sp=0x801FD7F0, then stores ra/s0 at sp+0x10/sp+0x14
             * = 0x801FD800/0x801FD804 = TCB[0]+0/+4, clobbering state/handle.
             * This C version has no MIPS stack frame. Returns 1 (fully intercepted). */
            g_frame_flip_running = 0;
            static uint32_t s_fc = 0; ++s_fc;
            {
                uint16_t t0, t2;
                memcpy(&t0, &g_ram[0x1FD800], 2);
                memcpy(&t2, &g_ram[0x1FD8E0], 2);
                /* [SCHED-C] if (s_fc <= 5 || (s_fc % 2000) == 0)
                   printf("[SCHED-C] call #%u  TCB[0]=%u  TCB[2]=%u\n", s_fc, t0, t2); */
            }
            if (s_fc % 100 == 0 && DIAG_ENABLED()) {
                uint16_t ds, dss, dtimer, dctr, dsub2, dsub3;
                memcpy(&ds,     &g_ram[0x1FD848], 2);
                memcpy(&dss,    &g_ram[0x1FD84A], 2);
                memcpy(&dtimer, &g_ram[0x1FD858], 2);
                memcpy(&dctr,   &g_ram[0x1FD85A], 2);
                memcpy(&dsub2,  &g_ram[0x1FD84C], 2);
                memcpy(&dsub3,  &g_ram[0x1FD84E], 2);
                { uint16_t t2s; memcpy(&t2s, &g_ram[0x1FD8E0], 2);
                  uint32_t t2h; memcpy(&t2h, &g_ram[0x1FD8E4], 4);
                  uint32_t qw2=0, qr2=0;
                  memcpy(&qw2, &g_scratch[0x29C], 4);
                  memcpy(&qr2, &g_scratch[0x2A0], 4);
                  uint16_t d_1fc; memcpy(&d_1fc, &g_scratch[0x1FC], 2);
                  uint32_t bcc8; memcpy(&bcc8, &g_ram[0x9BCC8], 4);
                  uint32_t bcca_w; memcpy(&bcca_w, &g_ram[0x9BCCA], 4);
                  uint8_t a5398 = g_ram[0xA5398];
                  uint8_t a5399 = g_ram[0xA5399];
                  uint8_t a539a = g_ram[0xA539A];
                  uint16_t ent_cnt; memcpy(&ent_cnt, &g_scratch[0x24A], 2);
                  /* Count active entity slots (first byte non-zero, 200 slots at 0x800A5970, stride 0xD4) */
                  uint32_t active_ents = 0;
                  for (int ei = 0; ei < 200; ei++) {
                      if (g_ram[0xA5970 + ei * 0xD4] != 0) active_ents++;
                  }
                  printf("[DSTATE-C] #%u  state=%u sub=%u sub2=%u sub3=%u timer=%d ctr=%d  DAT_ce=%u DAT_cc=%u DAT_d3=%u c6=%u 1fc=0x%04X  bcc8=%u bcca=%u a5398=%u/%u/%u  entry=0x%08X  TCB2:s=%u h=%u qw=%u qr=%u fib=%d  ent_cnt=%u active=%u\n",
                       s_fc, (uint32_t)ds, (uint32_t)dss,
                       (uint32_t)dsub2, (uint32_t)dsub3,
                       (int16_t)dtimer, (int16_t)dctr, (uint32_t)g_scratch[0x1CE],
                       (uint32_t)g_scratch[0x1CC], (uint32_t)g_scratch[0x1D3],
                       (uint32_t)g_scratch[0x1C6], (uint32_t)d_1fc,
                       bcc8, bcca_w & 0xFFFF, (uint32_t)a5398, (uint32_t)a5399, (uint32_t)a539a,
                       g_display_entry, (uint32_t)t2s, t2h, qw2, qr2,
                       g_fiber_loading != NULL,
                       (uint32_t)ent_cnt, active_ents);
                }
                fflush(stdout);
            }
            /* Iterate TCBs: 3 entries at 0x801FD800/0x801FD870/0x801FD8E0, stride 0x70.
             * Set scratchpad[0x1D4] = current tptr before each dispatch so that
             * FUN_80017208's "*_DAT_1f8001d4 = 0" correctly targets the right TCB. */
            uint32_t tptr = 0x801FD800u;
            memcpy(&g_scratch[0x1D4], &tptr, 4);
            while (tptr <= 0x801FD94Fu) {
                uint32_t off = tptr - 0x80000000u;
                uint16_t state;
                memcpy(&state, &g_ram[off], 2);
                memcpy(&g_scratch[0x1D4], &tptr, 4);
                if (state == 2u) {
                    uint32_t handle;
                    memcpy(&handle, &g_ram[off + 4], 4);
                    uint16_t s4 = 4u;
                    memcpy(&g_ram[off], &s4, 2);
                    cpu->a0 = handle;
                    cpu->t1 = 0x10u;
                    call_by_address(cpu, 0xB0u);
                } else if (state == 3u) {
                    uint32_t entry, sp2, stksz;
                    memcpy(&entry,  &g_ram[off + 12], 4);
                    memcpy(&sp2,    &g_ram[off +  8], 4);
                    memcpy(&stksz,  &g_ram[off + 16], 4);
                    cpu->a0 = entry; cpu->a1 = sp2; cpu->a2 = stksz;
                    cpu->t1 = 0x0Eu;
                    call_by_address(cpu, 0xB0u);
                    uint32_t new_handle = cpu->v0;
                    memcpy(&g_ram[off + 4], &new_handle, 4);
                    uint16_t s4 = 4u;
                    memcpy(&g_ram[off], &s4, 2);
                    cpu->a0 = new_handle;
                    cpu->t1 = 0x10u;
                    call_by_address(cpu, 0xB0u);
                }
                tptr += 0x70u;
                memcpy(&g_scratch[0x1D4], &tptr, 4);
            }
            cpu->v0 = 0;
            return 1;
        }
        case 0x80016940u: { static uint32_t s_ff = 0; ++s_ff; /* [TRACE] FUN_80016940 (frame-flip) */ break; }
        case 0x80060C10u: { static uint32_t s_c10 = 0; ++s_c10; /* [GPU-Q] first 10 — re-enable printf when investigating GPU queue */ break; }
        case 0x80060624u: { static uint32_t s_624 = 0; if (++s_624 <= 10) { printf("[TRACE] FUN_80060624 (LoadImage) call #%u a0=0x%08X a1=0x%08X\n", s_624, cpu->a0, cpu->a1); fflush(stdout); } break; }
        case 0x80060EF0u: { static uint32_t s_ef0 = 0; if (++s_ef0 <= 5) { printf("[TRACE] FUN_80060EF0 (GPU dispatch) call #%u\n", s_ef0); fflush(stdout); } break; }
        case 0x80067E84u: break; /* [TRACE] FUN_80067E84 (IRQ disable B) */
        case 0x8005DFD8u: {
            /* addPrim — count per frame, let compiled code run */
            g_addprim_count++;
            break;
        }
        case 0x80060B70u: {
            /* DrawOTag — log per-frame addPrim count + OT pointer */
            static uint32_t s_b70 = 0;
            extern uint32_t g_addprim_count;
            ++s_b70;
            /* [DrawOTag-PRE] if (s_b70 <= 10) printf("[DrawOTag-PRE] #%u f%u a0=0x%08X addPrim=%u\n", ...); */
            g_addprim_count = 0; /* reset per DrawOTag call */
            break;
        }
        case 0x800173B0u: { static uint32_t s_sv = 0; if (++s_sv <= 3) { printf("[TRACE] func_800173B0 (SPU voice) call #%u\n", s_sv); fflush(stdout); } break; }
        case 0x8005EB54u: { static uint32_t s_ds1 = 0; if (++s_ds1 <= 5) { printf("[TRACE] func_8005EB54 a0=0x%08X\n", cpu->a0); fflush(stdout); } break; }
        case 0x8005EB5Cu: { static uint32_t s_ds2 = 0; if (++s_ds2 <= 5) { printf("[TRACE] func_8005EB5C a0=0x%08X\n", cpu->a0); fflush(stdout); } break; }
        case 0x80017374u: { static uint32_t s_17374 = 0; if (++s_17374 <= 5) { printf("[TRACE] func_80017374 (scr[1E8]++) call #%u\n", s_17374); fflush(stdout); } break; }
        case 0x80016FD8u: { static uint32_t s_fd8 = 0; if (++s_fd8 <= 5) { printf("[TRACE] func_80016FD8\n"); fflush(stdout); } break; }
        case 0x800170F8u: { static uint32_t s_f8 = 0; if (++s_f8 <= 5) { printf("[TRACE] func_800170F8\n"); fflush(stdout); } break; }
        case 0x80067C30u: { static uint32_t s_gc = 0; if (++s_gc <= 5) { printf("[TRACE] func_80067C30 call #%u\n", s_gc); fflush(stdout); } break; }
        case 0x8005F420u: { static uint32_t s_pde = 0; if (++s_pde <= 5) { printf("[TRACE] func_8005F420 (PutDispEnv) call #%u\n", s_pde); fflush(stdout); } break; }
        case 0x80067D78u: { static uint32_t s_gd = 0; if (++s_gd <= 3) { printf("[TRACE] func_80067D78 call #%u a0=0x%X a1=0x%X\n", s_gd, cpu->a0, cpu->a1); fflush(stdout); } break; }

        /* ================================================================
         * BIOS WRAPPER INTERCEPTS
         * ================================================================
         * Tomba's binary contains BIOS wrapper functions that load the
         * function number into t1, the vector (0xA0/0xB0/0xC0) into t2,
         * then do 'jr t2'. In recompiled code, 'jr t2' becomes a TODO
         * no-op. We intercept each wrapper here to make the actual BIOS
         * call via call_by_address().
         * ================================================================ */

        /* --- A-table wrappers (vector 0xA0) --- */
        case 0x8005B38Cu: /* A(0x44) FlushCache */
            cpu->t1 = 0x44; call_by_address(cpu, 0xA0); return 1;
        case 0x80061728u: /* A(0x49) GPU_cw */
            cpu->t1 = 0x49; call_by_address(cpu, 0xA0); return 1;
        case 0x8005B39Cu: /* A(0x70) _bu_init */
            cpu->t1 = 0x70; call_by_address(cpu, 0xA0); return 1;
        case 0x8006853Cu: /* A(0x72) _96_init */
            cpu->t1 = 0x72; call_by_address(cpu, 0xA0); return 1;
        /* 0x8005CC54: A(0xAB) sound flush — handled above by dedicated if-block */
        case 0x8005CC64u: /* A(0xAC) secondary sound handler */
            cpu->t1 = 0xAC; call_by_address(cpu, 0xA0); return 1;

        /* --- B-table wrappers (vector 0xB0) --- */
        case 0x8006476Cu: /* B(0x07) DeliverEvent */
            cpu->t1 = 0x07; call_by_address(cpu, 0xB0); return 1;
        case 0x8005B3ACu: /* B(0x08) OpenEvent */
            cpu->t1 = 0x08; call_by_address(cpu, 0xB0); return 1;
        case 0x8005B3BCu: /* B(0x09) CloseEvent */
            cpu->t1 = 0x09; call_by_address(cpu, 0xB0); return 1;
        case 0x8007659Cu: /* B(0x0A) WaitEvent */
            cpu->t1 = 0x0A; call_by_address(cpu, 0xB0); return 1;
        case 0x8005B3CCu: /* B(0x0B) TestEvent */
            cpu->t1 = 0x0B; call_by_address(cpu, 0xB0); return 1;
        case 0x8005B3DCu: /* B(0x0C) EnableEvent */
            cpu->t1 = 0x0C; call_by_address(cpu, 0xB0); return 1;
        case 0x80074E94u: /* B(0x0D) DisableEvent */
            cpu->t1 = 0x0D; call_by_address(cpu, 0xB0); return 1;
        case 0x8005B3ECu: /* B(0x0E) OpenThread */
            cpu->t1 = 0x0E; call_by_address(cpu, 0xB0); return 1;
        case 0x8005B3FCu: /* B(0x0F) CloseThread */
            cpu->t1 = 0x0F; call_by_address(cpu, 0xB0); return 1;
        case 0x8005B40Cu: /* B(0x10) ChangeThread */
            cpu->t1 = 0x10; call_by_address(cpu, 0xB0); return 1;
        case 0x80068554u: /* B(0x17) ReturnFromException */
            cpu->t1 = 0x17; call_by_address(cpu, 0xB0); return 1;
        case 0x80068564u: /* B(0x18) SetDefaultExitFromException */
            cpu->t1 = 0x18; call_by_address(cpu, 0xB0); return 1;
        case 0x80068574u: /* B(0x19) SetCustomExitFromException */
            cpu->t1 = 0x19; call_by_address(cpu, 0xB0); return 1;
        case 0x8005B43Cu: /* B(0x32) open */
            cpu->t1 = 0x32; call_by_address(cpu, 0xB0); return 1;
        case 0x8005B44Cu: /* B(0x33) lseek */
            cpu->t1 = 0x33; call_by_address(cpu, 0xB0); return 1;
        case 0x8005B45Cu: /* B(0x34) read */
            cpu->t1 = 0x34; call_by_address(cpu, 0xB0); return 1;
        case 0x8005B46Cu: /* B(0x35) write */
            cpu->t1 = 0x35; call_by_address(cpu, 0xB0); return 1;
        case 0x8005B47Cu: /* B(0x36) close */
            cpu->t1 = 0x36; call_by_address(cpu, 0xB0); return 1;
        case 0x8005B48Cu: /* B(0x41) sys_read */
            cpu->t1 = 0x41; call_by_address(cpu, 0xB0); return 1;
        case 0x8005B49Cu: /* B(0x43) nextfile */
            cpu->t1 = 0x43; call_by_address(cpu, 0xB0); return 1;
        case 0x8005B4ACu: /* B(0x45) delete */
            cpu->t1 = 0x45; call_by_address(cpu, 0xB0); return 1;
        case 0x8005B4CCu: /* FUN_8005B4CC: MC file-exists check — searches BIOS dir table at
                           * g_ram[0x150] which we never populate; return a1 (the info-struct ptr)
                           * as a non-zero "file found" result so game proceeds to O_WRONLY.
                           * Set s_nextfile_remain=2 so nextfile pretends 3 linked blocks exist.
                           * Fill in DIRENTRY struct with valid MC data so game logic passes. */
            cpu->v0 = cpu->a1 ? cpu->a1 : 1u;
            s_nextfile_remain = 2;
            if (cpu->a1 >= 0x80000000u) {
                uint32_t _s = cpu->a1 - 0x80000000u;
                /* Populate full DIRENTRY struct as firstfile() would:
                 * name[0x00-0x13]: 20-byte PS1 filename (from last O_CREAT)
                 * attr[0x14]: 0x51 = first linked block
                 * size[0x18]: 0x6000 = 3×8KB
                 * next[0x1C]: 0 (nextfile handles traversal)
                 * count[0x20]: 0 */
                memset(&g_ram[_s], 0, 0x24);
                strncpy((char*)&g_ram[_s], s_last_mc_name, 20);
                *(uint32_t*)&g_ram[_s + 0x14] = 0x51u;
                *(uint32_t*)&g_ram[_s + 0x18] = 0x6000u;
                *(uint32_t*)&g_ram[_s + 0x1C] = 0u;
                *(uint32_t*)&g_ram[_s + 0x20] = 0u;
                printf("[MEMCARD FUN_B4CC] a0='%s' -> name='%s' attr=0x51 size=0x6000\n",
                       (cpu->a0 >= 0x80000000u) ? (char*)&g_ram[cpu->a0 - 0x80000000u] : "?",
                       s_last_mc_name);
            } else {
                printf("[MEMCARD FUN_B4CC] a0=0x%08X a1=0x%08X -> v0=0x%08X\n", cpu->a0, cpu->a1, cpu->v0);
            }
            return 1;
        case 0x8005CCACu: /* B(0x4E) card_write */
            cpu->t1 = 0x4E; call_by_address(cpu, 0xB0); return 1;
        case 0x8005CCBCu: /* B(0x50) */
            cpu->t1 = 0x50; call_by_address(cpu, 0xB0); return 1;
        case 0x8005B76Cu: /* B(0x42) unknown (memory card related) */
            cpu->t1 = 0x42; call_by_address(cpu, 0xB0); return 1;
        case 0x8005CD98u: /* B(0x4A) InitCard */
            cpu->t1 = 0x4A; call_by_address(cpu, 0xB0); return 1;
        case 0x8005CDA8u: /* B(0x4B) StartCard */
            cpu->t1 = 0x4B; call_by_address(cpu, 0xB0); return 1;
        case 0x8005CDB8u: /* B(0x4C) StopCard */
            cpu->t1 = 0x4C; call_by_address(cpu, 0xB0); return 1;
        case 0x8005CD88u: /* B(0x5B) ChangeClearPad */
            cpu->t1 = 0x5B; call_by_address(cpu, 0xB0); return 1;

        /* --- C-table wrappers (vector 0xC0) --- */
        case 0x80069A8Cu: /* C(0x02) SysEnqIntRP */
            cpu->t1 = 0x02; call_by_address(cpu, 0xC0); return 1;
        case 0x80069A9Cu: /* C(0x03) SysDeqIntRP */
            cpu->t1 = 0x03; call_by_address(cpu, 0xC0); return 1;
        case 0x80067E14u: /* C(0x0A) ChangeClearRCnt */
            cpu->t1 = 0x0A; call_by_address(cpu, 0xC0); return 1;

        case 0x80028D70u: {
            /* FUN_80028D70 — joypad state reader.
             * Called via JAL (direct C call) from FUN_800223e0 every frame.
             * Reads active-low raw pad data from g_ram[0x9eb5a/5b],
             * inverts to active-high, returns in v0.
             * psx_set_pad1() writes ~buttons (active-low) there each frame. */
            uint16_t al = (uint16_t)(g_ram[0x9eb5a] | ((uint16_t)g_ram[0x9eb5b] << 8));
            cpu->v0 = (uint32_t)(uint16_t)(~al);
            { static uint32_t s_28d = 0; ++s_28d;
              /* [PAD-READ] first 5 + every 500 + DIAG — commented out (re-enable for pad debugging) */
              /* [PAD-ATCK] Circle/Square seen — commented out (re-enable for attack debugging) */
            }
            return 1;
        }
    }

    /* FUN_80060AE4 — GPU state cache writer (called by PutDrawEnv / PutDispEnv).
     * Ghidra decompile:
     *   *DAT_80090d70 = param_1;
     *   *(char *)((param_1 >> 0x18) + 0x8009b18c) = (char)param_1;
     * It stores GP0/GP1 commands in a RAM-side cache only — never writes to GPU
     * hardware port 0x1F801810.  That means E1-E6 drawing-area/offset/mode
     * commands never reach our OpenGLRenderer.  We intercept here and forward
     * GP0 environment commands (E1-E6) to the GPU interpreter so the renderer's
     * drawing area and offset are kept in sync.  Return 0 so MIPS also runs to
     * update the RAM cache as expected by the rest of the game. */
    if (addr == 0x80060AE4u) {
        extern void gpu_submit_word(uint32_t w);
        uint32_t cmd = cpu->a0;
        uint8_t  cmd_byte = (uint8_t)(cmd >> 24);
        static uint32_t s_e_log = 0;
        if (cmd_byte >= 0xE1u && cmd_byte <= 0xE6u) {
            if (++s_e_log <= 30) {
                printf("[GP0-ENV] FUN_80060AE4 E%X cmd=0x%08X\n", cmd_byte & 0xFu, cmd);
                fflush(stdout);
            }
            gpu_submit_word(cmd);
        }
        /* Track E3/E4/E5 for frame diagnostics */
        if (cmd_byte == 0xE3) {
            g_env_stats.last_e3 = cmd;
            g_env_stats.seen_e3 = 1;
            if (DIAG_ENABLED()) {
                uint32_t x = cmd & 0x3FF;
                uint32_t y = (cmd >> 10) & 0x1FF;
                printf("[GP0-ENV-DIAG] f%u E3 DrawAreaTL x=%u y=%u (raw=0x%08X)\n",
                       g_ps1_frame, x, y, cmd);
                fflush(stdout);
            }
        } else if (cmd_byte == 0xE4) {
            g_env_stats.last_e4 = cmd;
            g_env_stats.seen_e4 = 1;
            if (DIAG_ENABLED()) {
                uint32_t x = cmd & 0x3FF;
                uint32_t y = (cmd >> 10) & 0x1FF;
                printf("[GP0-ENV-DIAG] f%u E4 DrawAreaBR x=%u y=%u (raw=0x%08X)\n",
                       g_ps1_frame, x, y, cmd);
                fflush(stdout);
            }
        } else if (cmd_byte == 0xE5) {
            g_env_stats.last_e5 = cmd;
            g_env_stats.seen_e5 = 1;
            if (DIAG_ENABLED()) {
                int32_t x = (int32_t)(cmd & 0x7FF);
                if (x & 0x400) x |= (int32_t)0xFFFFF800;  /* sign-extend 11-bit */
                int32_t y = (int32_t)((cmd >> 11) & 0x7FF);
                if (y & 0x400) y |= (int32_t)0xFFFFF800;
                printf("[GP0-ENV-DIAG] f%u E5 DrawOffset x=%d y=%d (raw=0x%08X)\n",
                       g_ps1_frame, x, y, cmd);
                fflush(stdout);
            }
        }
        return 0; /* let MIPS run to update RAM cache */
    }

    /* FUN_8005D4D0 — MdecSync: "wait for MDEC to become not-busy".
     * We must also set _DAT_8009b044 (g_ram[0x9B044]) here because the
     * secondary fiber (FUN_8001F1C0) spins on that flag at 8001f428 after
     * FUN_8001EFE8 returns non-zero.  FUN_8005D4D0 is called via JALR
     * (call_by_address) from within the inner loop, so this fires reliably
     * and guarantees the guard check at 8001f414 succeeds. */
    if (addr == 0x8005D4D0u) {
        uint32_t done = 1u;
        memcpy(&g_ram[0x9B044u], &done, 4);
        cpu->v0 = 0;
        return 1;
    }

    /* FUN_80065470 — "wait for CDROM command to complete" (CdSync).
     * On real HW waits for IRQ2 to fire and set DAT_800962c8 != 0.
     * Our runtime never fires IRQs, so it would spin for up to 0x3C0000
     * iterations before timing out. Override: return 2 = command OK. */
    if (addr == 0x80065470u) {
        cpu->v0 = 2;
        return 1;
    }

    /* FUN_80066a50 — CdRead(sector_count, dest, mode): read sectors from disc.
     * a0 = number of 2048-byte sectors to read
     * a1 = destination PS1 RAM address
     * The LBA was set by the preceding CdlSeekL (stored in g_cdrom_lba).
     * Reads the sectors from the BIN file and copies into PS1 RAM. */
    if (addr == 0x80066a50u) {
        uint32_t sector_count = cpu->a0;
        uint32_t dest         = cpu->a1;
        static int s_cdread   = 0;
        ++s_cdread;
        /* [CdRead] printf("[CdRead] #%d LBA=%u count=%u dest=0x%08X\n", s_cdread, ...); */
        uint8_t sec_buf[2048];
        for (uint32_t i = 0; i < sector_count; i++) {
            if (!psx_cdrom_read_sector(g_cdrom_lba + i, sec_buf)) {
                printf("[CdRead] FAILED at LBA %u\n", g_cdrom_lba + i);
                fflush(stdout);
                cpu->v0 = 0;
                return 1;
            }
            psx_runtime_load(dest + i * 2048u, sec_buf, 2048u);
        }
        g_cdrom_lba += sector_count;
        cpu->v0 = 1;
        return 1;
    }

    /* FUN_80066b30 — CdReadSync(mode, result): wait for current read to finish.
     * With mode=1 (non-blocking), loops once then calls FUN_800648e8 and returns.
     * Returns 0 on success, -1 on error/timeout.  Case 4 advances on 0. */
    if (addr == 0x80066b30u) {
        cpu->v0 = 0;
        return 1;
    }

    /* FUN_800659BC — CDROM command dispatcher.
     * Intercepts all CD-ROM commands.  Returns 0 (success) for most.
     * CdlSeekL (cmd=2): reads the BCD MSF from the CdlLOC pointer in a1,
     * converts to an absolute LBA, and stores in g_cdrom_lba for CdRead. */
    if (addr == 0x800659BCu) {
        uint8_t cmd = (uint8_t)cpu->a0;
        if (cmd == 2u && cpu->a1 != 0u) {
            /* CdlSeekL — a1 = pointer to CdlLOC {minute, second, frame, ?} (BCD) */
            uint8_t* p = addr_ptr(cpu->a1);
            if (p) {
                uint8_t bm = p[0], bs = p[1], bf = p[2];
                uint32_t m = (uint32_t)((bm >> 4) * 10 + (bm & 0xFu));
                uint32_t s = (uint32_t)((bs >> 4) * 10 + (bs & 0xFu));
                uint32_t f = (uint32_t)((bf >> 4) * 10 + (bf & 0xFu));
                uint32_t lba = (m * 60u + s) * 75u + f;
                if (lba >= 150u) lba -= 150u;
                g_cdrom_lba = lba;
                /* [CdlSeekL] printf("[CdlSeekL] MSF=%02X:%02X:%02X → LBA %u\n", bm, bs, bf, lba); */
                extern void xa_audio_seek(uint32_t lba);
                xa_audio_seek(lba);
                /* Note: fmv_player is seeked lazily on first FUN_8001EFE8 call,
                 * using g_cdrom_lba, because that ensures the FMV seek (not a
                 * later title-music seek) is used to initialise video decoding. */
            }
        }
        cpu->v0 = 0;
        return 1;
    }

    /* --- BIOS kernel interrupt table stubs (PTR_PTR_800974A0 family) ----------
     *
     * The PS1 BIOS kernel interrupt dispatch table lives at the address stored
     * in PTR_800974A0 (= 0x80097480 in this binary).  Several entries in that
     * table are null (never written by a real BIOS kernel_init) so calling
     * through them gives a hard ACCESS_VIOLATION.
     *
     * These functions are interrupt-masking helpers.  In a static recompiler
     * (no interrupts, single-thread cooperative model) they are all no-ops.
     *
     *  FUN_80067E24 → calls *(*(PTR_800974A0 + 0x0C))() — DisableInterrupts A
     *  FUN_80067E54 → calls *(*(PTR_800974A0 + 0x08))() — EnableInterrupts
     *  FUN_80067E84 → calls *(*(PTR_800974A0 + 0x04))() — DisableInterrupts B (NULL!)
     */
    if (addr == 0x80067E24u || addr == 0x80067E54u || addr == 0x80067E84u) {
        cpu->v0 = 0;
        return 1;
    }

    /* FUN_80060B70 — actual GPU DMA submit (DrawOTag linked-list DMA trigger).
     * a0 = OT head address (pointer to the last entry in the ordering table).
     * Walk the OT linked list from head, submitting each GP0 word to the renderer.
     *
     * OT entry format (per 32-bit word at address ptr):
     *   bits [23:0]  = next_ptr (lower 24 bits of next OT entry; 0xFFFFFF = end)
     *   bits [31:24] = count    (number of GP0 data words immediately following)
     * The GP0 words are at ptr+4, ptr+8, ... ptr + count*4. */
    if (addr == 0x80060B70u) {
        extern void gpu_submit_word(uint32_t word);
        extern void gpu_abort_streaming(void);
        extern int g_in_drawtag;
        uint32_t ptr = cpu->a0;
        /* Normalize KUSEG → KSEG0 */
        if ((ptr & 0xFF000000u) == 0u && ptr != 0u) ptr |= 0x80000000u;
        static int s_drawtag = 0;
        int word_count = 0;
        g_in_drawtag = 1;

        /* Reset per-frame OT stats */
        memset(&g_dt_stats, 0, sizeof(g_dt_stats));

        /* Skip obviously-invalid OT heads: below game binary (0x80010000) means
         * uninitialized pointer or kernel/scratch area — not a real OT. */
        if (ptr < 0x80010000u) {
            ++s_drawtag;
            /* [DrawOTag] if (s_drawtag <= 5) printf("[DrawOTag] #%d: SKIP invalid head...\n"); */
            g_in_drawtag = 0;
            cpu->v0 = 0;
            return 1;
        }
        /* Diagnostic: dump draw env buffer contents on first few PutDrawEnv OT calls.
         * 0x8009D6E0 = Base A draw env buffer, 0x8009E3F0 = Base B draw env buffer */
        /* [ENV-DUMP] first 5 calls for ptr==draw-env buffer — re-enable when investigating env cmds */

        /* For OT head dumps: track first few calls to known head addresses */
        static uint32_t s_head_dump_a = 0, s_head_dump_b = 0;
        int do_chain_dump = 0;
        if ((cpu->a0 == 0x8009D6ACu || cpu->a0 == 0x8009E3BCu)) {
            uint32_t *cnt = (cpu->a0 == 0x8009D6ACu) ? &s_head_dump_a : &s_head_dump_b;
            if (++(*cnt) <= 3) do_chain_dump = 1;
        }
        /* [OT-CHAIN] first 3 per head — re-enable do_chain_dump block when investigating OT layout */
        uint32_t start_ptr = ptr;  /* save for rich-frame dump */
        int limit = 65536;  /* max OT entries — prevents infinite loops */
        uint32_t null_stop_addr = 0; int null_stop_entry = 0;
        int diag_prim_logged = 0;  /* count of non-empty entries logged for DIAG */
        while (limit-- > 0) {
            uint8_t* ph = addr_ptr(ptr);
            if (!ph) break;
            uint32_t header; memcpy(&header, ph, 4);
            uint32_t next24 = header & 0xFFFFFFu;
            uint8_t  count  = (uint8_t)(header >> 24);

            g_dt_stats.ot_entries++;
            if (count > 0) g_dt_stats.ot_nonempty++;

            for (uint8_t i = 1; i <= count; i++) {
                uint8_t* pw = addr_ptr(ptr + (uint32_t)i * 4u);
                if (pw) {
                    uint32_t w; memcpy(&w, pw, 4);
                    /* Classify first word of each primitive for stats */
                    if (i == 1) diag_classify_gp0((uint8_t)(w >> 24));
                    gpu_submit_word(w);
                    word_count++;
                    /* Track E3/E4/E5 for frame diagnostics (DrawOTag path) */
                    uint8_t wb = (uint8_t)(w >> 24);
                    if (wb == 0xE3) { g_env_stats.last_e3 = w; g_env_stats.seen_e3 = 1; }
                    else if (wb == 0xE4) { g_env_stats.last_e4 = w; g_env_stats.seen_e4 = 1; }
                    else if (wb == 0xE5) { g_env_stats.last_e5 = w; g_env_stats.seen_e5 = 1; }
                }
            }

            /* During diag window: log first 100 non-empty OT entries */
            if (DIAG_ENABLED() && count > 0 && diag_prim_logged < 100) {
                uint8_t* pw0 = addr_ptr(ptr + 4);
                uint32_t w0 = 0, w1 = 0, w2 = 0;
                if (pw0) memcpy(&w0, pw0, 4);
                if (count >= 3) {
                    uint8_t* pw1 = addr_ptr(ptr + 8);
                    uint8_t* pw2 = addr_ptr(ptr + 12);
                    if (pw1) memcpy(&w1, pw1, 4);
                    if (pw2) memcpy(&w2, pw2, 4);
                    /* Decode CLUT and tpage for textured prims */
                    uint16_t clut_x = ((w2 >> 16) & 0x3F) * 16;
                    uint16_t clut_y = (w2 >> 22) & 0x1FF;
                    printf("[DT-PRIM] f%u entry#%u @0x%08X cnt=%u cmd=0x%02X w0=0x%08X w1=0x%08X w2=0x%08X clut=(%u,%u)\n",
                           g_ps1_frame, g_dt_stats.ot_entries, ptr, count,
                           (unsigned)(w0 >> 24), w0, w1, w2, clut_x, clut_y);
                } else {
                    printf("[DT-PRIM] f%u entry#%u @0x%08X cnt=%u cmd=0x%02X w0=0x%08X\n",
                           g_ps1_frame, g_dt_stats.ot_entries, ptr, count,
                           (unsigned)(w0 >> 24), w0);
                }
                diag_prim_logged++;
            }

            /* After each OT entry: abort any CPUToVRAM streaming that was started.
             * On real PS1 hardware, CPUToVRAM commands in an OT chain receive their
             * pixel data from a SEPARATE DMA2 block-mode transfer, NOT from the next
             * OT entries. Without this, subsequent OT drawing commands get consumed
             * as fake pixel data, corrupting the entire frame. */
            gpu_abort_streaming();
            if (next24 == 0xFFFFFFu) break;  /* terminator */
            if (next24 == 0u) {               /* null link = end of list */
                null_stop_addr = ptr;
                null_stop_entry = 65536 - limit;
                break;
            }
            ptr = next24 | 0x80000000u;
        }
        g_dt_stats.total_words = (uint32_t)word_count;
        ++s_drawtag;
        /* [DrawOTag] if (s_drawtag <= 30 || (s_drawtag % 300) == 0)
           printf("[DrawOTag] #%d: a0=0x%08X %d GPU words entries=%u nonempty=%u f%u\n", ...); */

        /* Frame-gated diagnostic summary */
        if (DIAG_ENABLED()) {
            printf("[DT-DIAG] f%u a0=0x%08X entries=%u nonempty=%u words=%u  "
                   "fill=%u poly=%u line=%u rect=%u env=%u misc=%u\n",
                   g_ps1_frame, cpu->a0,
                   g_dt_stats.ot_entries, g_dt_stats.ot_nonempty, g_dt_stats.total_words,
                   g_dt_stats.fill_cmds, g_dt_stats.poly_cmds, g_dt_stats.line_cmds,
                   g_dt_stats.rect_cmds, g_dt_stats.env_cmds, g_dt_stats.misc_cmds);
            fflush(stdout);
        }

        /* One-time dump of GP0 primitive commands from a rich frame (>= 200 words).
         * Shows cmd byte and coords of each OT entry so we can verify rendering. */
        static int s_rich_dumped = 0;
        if (!s_rich_dumped && word_count >= 200) {
            s_rich_dumped = 1;
            printf("[GP0-DUMP] Rich frame #%d: %d words, OT head=0x%08X\n",
                   s_drawtag, word_count, cpu->a0);
            uint32_t dp = start_ptr;
            int ecount = 0;
            while (ecount < 60) {
                uint8_t* ph2 = addr_ptr(dp);
                if (!ph2) break;
                uint32_t hdr2; memcpy(&hdr2, ph2, 4);
                uint32_t nxt2 = hdr2 & 0xFFFFFFu;
                uint8_t  cnt2 = (uint8_t)(hdr2 >> 24);
                if (cnt2 > 0) {
                    /* Print first word (command) and second word (coords) of each primitive */
                    uint8_t* pw0 = addr_ptr(dp + 4);
                    uint32_t w0 = 0, w1 = 0;
                    if (pw0) memcpy(&w0, pw0, 4);
                    if (cnt2 >= 2) {
                        uint8_t* pw1 = addr_ptr(dp + 8);
                        if (pw1) memcpy(&w1, pw1, 4);
                    }
                    printf("  [%d] 0x%08X cnt=%u  cmd=0x%02X w0=0x%08X w1=0x%08X\n",
                           ecount, dp, cnt2, (unsigned)(w0 >> 24), w0, w1);
                }
                if (nxt2 == 0xFFFFFFu || nxt2 == 0u) break;
                dp = nxt2 | 0x80000000u;
                ecount++;
            }
            fflush(stdout);
        }
        g_in_drawtag = 0;
        cpu->v0 = 0;
        return 1;
    }

    /* FUN_800602E0 — GPU DMA OT fill (the real ClearOTagR engine).
     * On real PS1, this programs DMA channel 2 to fill the ordering table with
     * self-referential backward pointers (OT[i] → OT[i-1]).  Our runtime has no
     * GPU DMA, so we fill the chain in software here.
     * a0 = OT base address, a1 = count of entries.
     * Returns count (PS1 SDK convention). */
    if (addr == 0x800602E0u) {
        uint32_t ot_base = cpu->a0;
        uint32_t count   = cpu->a1;
        uint32_t phys    = ot_base & 0x1FFFFFFFu;
        if (phys < 0x200000u && count > 0u && count <= 8192u) {
            /* During diag window: scan OT for leftover non-empty entries before clearing */
            if (DIAG_ENABLED()) {
                uint32_t leftover = 0;
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t entry;
                    memcpy(&entry, &g_ram[phys + i * 4u], 4);
                    uint8_t cnt = (uint8_t)(entry >> 24);
                    if (cnt > 0) leftover++;
                }
                if (leftover > 0) {
                    printf("[COTR-DIAG] f%u WARNING: %u leftover primitives in OT "
                           "base=0x%08X count=%u (never drawn!)\n",
                           g_ps1_frame, leftover, ot_base, count);
                    fflush(stdout);
                } else {
                    printf("[COTR-DIAG] f%u OT clean, base=0x%08X count=%u\n",
                           g_ps1_frame, ot_base, count);
                    fflush(stdout);
                }
            }

            /* OT[0] = 0xFFFFFF (terminator — FUN_8005F05C overwrites with 0x90D58 after) */
            uint32_t term = 0x00FFFFFFu;
            memcpy(&g_ram[phys], &term, 4);
            /* OT[1..count-1]: each packed word = low-24-bits of address of OT[i-1] */
            for (uint32_t i = 1u; i < count; ++i) {
                uint32_t packed = (ot_base + (i - 1u) * 4u) & 0x00FFFFFFu;
                memcpy(&g_ram[phys + i * 4u], &packed, 4);
            }
            static uint32_t s_ctag = 0;
            ++s_ctag;
            /* [ClearOTagR] if (s_ctag <= 4) printf("[ClearOTagR] #%u base=0x%08X count=%u\n", ...); */
        }
        cpu->v0 = (int32_t)count;
        return 1;
    }

    /* FUN_80016940 — frame-flip: present GPU output and pump OS events.
     * Called each PS1 vblank after DrawOTag. Side-effect only: return 0 so the
     * compiled function still runs (handles display buffer swap bookkeeping). */
    if (addr == 0x80016940u) {
        if (g_frame_flip_running) {
            /* Re-entrant call from FUN_8005f1c8 — block it.
             * The display fiber calls func_80016940 once per frame via indirect JALR.
             * func_80016940 then calls FUN_8005f1c8, which tries to call func_80016940
             * again.  The re-entrant call would double-flip the toggle and re-submit
             * the OT.  Block it and return v0=0 so FUN_8005f1c8 sees "success". */
            static uint32_t s_reent = 0;
            if (++s_reent <= 5)
                printf("[FF-BLOCKED] re-entrant frame-flip #%u blocked\n", s_reent);
            fflush(stdout);
            cpu->v0 = 0;
            return 1;
        }
        /* First (legitimate) call this frame: set guard, execute MIPS normally.
         * psx_present_frame() is called from the display yield handler AFTER this
         * function has run DrawOTag and all GPU packets are in the queue. */
        g_frame_flip_running = 1;
        return 0;
    }

    /* FUN_80060194 — compute GPU SetTextureWindow command from 8-byte sprite descriptor.
     * The recompiler generated this as a 4-byte stub (only the delay-slot `sp -= 16` of
     * the opening `bne a0,zero,0x800601a4` instruction), so the real body at 0x800601a4
     * and the epilogue `sp += 16` are missing.  Each call leaks 16 bytes of MIPS stack,
     * and with 3 sprites per fade-in frame that's 48 bytes/frame — the observed drift.
     *
     * Real body (from disassembly 0x80060194–0x80060214):
     *   if (a0 == 0) { v0 = 0; return; }
     *   b0 = read_byte(a0+0);  b2 = read_byte(a0+2);
     *   h4 = read_half(a0+4) (signed);  h6 = read_half(a0+6) (signed);
     *   v0 = 0xe2000000
     *      | ((b0 >> 3) << 10)                          // bits 14:10
     *      | ((b2 >> 3) << 15)                          // bits 19:15
     *      | (((-h6) & 0xff) >> 3) << 5                 // bits  9:5
     *      | (((-h4) & 0xff) >> 3);                     // bits  4:0 */
    if (addr == 0x80060194u) {
        if (cpu->a0 == 0u) {
            cpu->v0 = 0;
        } else {
            uint32_t b0 = (uint32_t)cpu->read_byte(cpu->a0 + 0u);
            uint32_t b2 = (uint32_t)cpu->read_byte(cpu->a0 + 2u);
            int32_t  h4 = (int32_t)(int16_t)cpu->read_half(cpu->a0 + 4u);
            int32_t  h6 = (int32_t)(int16_t)cpu->read_half(cpu->a0 + 6u);
            uint32_t c1 = (b0 >> 3u) << 10u;
            uint32_t c2 = (b2 >> 3u) << 15u;
            uint32_t c3 = (uint32_t)((uint32_t)(-(uint32_t)h4) & 0xffu) >> 3u;
            uint32_t c4 = ((uint32_t)((uint32_t)(-(uint32_t)h6) & 0xffu) >> 3u) << 5u;
            cpu->v0 = 0xe2000000u | c2 | c1 | c4 | c3;
        }
        return 1;
    }

    /* FUN_8001EFE8 — CD DMA ring-buffer sector poll (FMV/CDXA streaming, secondary thread).
     * Real code: checks if DMA ring buffer has a new sector; decodes it via MDEC.
     *
     * FUN_8001F1C0 state machine (see Ghidra decompilation):
     *   state 0: seek + spin `do { FUN_8001efe8; } while(iVar2==0);`
     *   state 1→2 (LAB_8001f33c):
     *     spin `while(TCB+0x4e==0) FUN_8001efe8;`   → set TCB+0x4e=1 to exit
     *     MDEC kickoff (no-op in our stub)
     *     spin `do { iVar2=FUN_8001efe8; if(iVar2!=0) break; } while(DAT_8009b044!=1);`
     *     guard `if(DAT_8009b044==0) infinite loop;` → MUST set DAT_8009b044=1
     *   state 3: cleanup → FUN_80017208 (signals batch done to display thread)
     *   skip via DAT_1f8001d3==1 → sets TCB+0x48=3 → triggers state-3 cleanup
     *
     * Our implementation:
     *   - Initialise fmv_player on first call using g_cdrom_lba (set by CdlSeekL)
     *   - Call fmv_player_tick() which decodes one sector, uploads frame to VRAM
     *   - Always set TCB+0x4e=1 and DAT_8009b044=1 to unblock all spins
     *   - When FMV is done: set DAT_1f8001d3=1 (triggers skip path → state 3 → cleanup)
     *   - Return v0=1 always (sector ready)                                           */
    if (addr == 0x8001EFE8u) {
        static int s_fmv_init = 0;
        static int s_efe8_calls_since_done = 0;
        extern int  fmv_player_tick(void);
        extern void fmv_player_seek(uint32_t lba);
        extern int  fmv_player_is_active(void);

        /* Detect external skip: PS1 code set the skip flag (e.g. user pressed Enter).
         * This check runs BEFORE tick so we don't decode extra frames after skip.
         * Note: we never reach here with s_fmv_init==1 && flag==1 from our own
         * !still_playing path because we reset s_fmv_init to 0 at the same time. */
        if (s_fmv_init == 1 && g_scratch[0x1D3] == 1) {
            extern void xa_audio_seek(uint32_t lba);
            extern void fmv_player_stop(void);
            xa_audio_seek(0);    /* stop FMV audio on user skip */
            fmv_player_stop();   /* deactivate FMV player — unblocks GP1 + Present */
            s_fmv_init = 0;
            printf("[FUN_8001EFE8] User skip detected — stopping FMV\n");
            fflush(stdout);
        }

        /* Initialise FMV player once per FMV sequence, on first call */
        if (!s_fmv_init && g_scratch[0x1D3] == 0) {
            fmv_player_seek(g_cdrom_lba);
            s_fmv_init = 1;
            printf("[FUN_8001EFE8] FMV init at LBA %u\n", g_cdrom_lba);
            fflush(stdout);
        }

        /* Tick: read sector(s), decode frame if complete, upload to VRAM */
        if (s_fmv_init) {
            fmv_player_tick();

            /* Check if user wants to skip FMV (Cross=0x4000 or Start=0x0008).
             * The display fiber normally checks buttons and sets g_scratch[0x1D3],
             * but during FMV it never runs — the loading fiber monopolizes time
             * inside fmv_player_tick()'s 15fps spin-wait.  psx_present_frame()
             * inside tick already polled GLFW and wrote pad to g_ram[0x9eb5a/5b].
             * Read it back (active-low) and check for skip buttons. */
            {
                uint16_t al = (uint16_t)(g_ram[0x9eb5a] | ((uint16_t)g_ram[0x9eb5b] << 8));
                uint16_t buttons = ~al;
                if (buttons & 0x4008) {  /* Cross or Start */
                    extern void xa_audio_seek(uint32_t lba);
                    extern void fmv_player_stop(void);
                    g_scratch[0x1D3] = 1;   /* skip flag — FMV fiber sees this, transitions to state 3 */
                    xa_audio_seek(0);        /* stop FMV audio */
                    fmv_player_stop();       /* deactivate FMV player — unblocks GP1 + Present */
                    s_fmv_init = 0;
                    printf("[FUN_8001EFE8] FMV skip — pad raw=0x%04X buttons=0x%04X ps1_frame=%u\n",
                           al, buttons, g_ps1_frame);
                    fflush(stdout);
                }
            }
        }

        /* Sound trigger — mirrors FUN_8001efe8 logic:
         * when DAT_1f8001cd==0x15 (WhoopEeCamp intro) and 15+ ticks in, call FUN_80020af0(0)
         * to open the music sequence.  The original code has this condition inside
         * func_8001efe8 but our intercept returns before running it. */
        {
            static int s_snd_ticks   = 0;
            static int s_snd_done    = 0;
            if (s_fmv_init) {
                s_snd_ticks++;
                if (!s_snd_done && s_snd_ticks > 14 && g_scratch[0x1CD] == 0x15) {
                    uint32_t old_a0 = cpu->a0;
                    cpu->a0 = 0;
                    psx_dispatch_compiled(cpu, 0x80020AF0u);
                    cpu->a0 = old_a0;
                    s_snd_done = 1;
                    printf("[FUN_8001EFE8] Triggered FMV music (FUN_80020AF0(0))\n");
                    fflush(stdout);
                }
            } else {
                /* Reset for next FMV */
                s_snd_ticks = 0;
                s_snd_done  = 0;
            }
        }

        /* Check if FMV has ended naturally */
        if (s_fmv_init && !fmv_player_is_active()) {
            /* Signal skip: DAT_1f8001d3=1 → FMV state machine sets TCB+0x48=3 → cleanup */
            g_scratch[0x1D3] = 1;
            s_fmv_init = 0;  /* reset for potential next FMV */
            s_efe8_calls_since_done = 0;
            printf("[FUN_8001EFE8] FMV done — signalling state machine to exit (DAT_d3=%u)\n",
                   (uint32_t)g_scratch[0x1D3]);
            fflush(stdout);
        }

        /* Track calls after done to see if d3 flag persists */
        if (!s_fmv_init && s_efe8_calls_since_done < 10) {
            printf("[FUN_8001EFE8] post-done call #%d  DAT_d3=%u  DAT_cc=%u\n",
                   s_efe8_calls_since_done, (uint32_t)g_scratch[0x1D3],
                   (uint32_t)g_scratch[0x1CC]);
            fflush(stdout);
            s_efe8_calls_since_done++;
        }

        /* Always unblock both spin loops in FUN_8001F1C0 */
        uint32_t tcb;
        memcpy(&tcb, &g_scratch[0x1D4], 4);
        if (tcb >= 0x80000000u && tcb < 0x80200000u) {
            uint32_t off = tcb - 0x80000000u;
            uint16_t v1 = 1;
            memcpy(&g_ram[off + 0x4e], &v1, 2);  /* TCB+0x4e = 1 (VRAM transfer done) */
        }
        /* DAT_8009b044 = MDEC frame-done flag (prevents infinite loop guard) */
        uint16_t mdec_done = 1u;
        memcpy(&g_ram[0x9B044u], &mdec_done, 2);

        cpu->v0 = 1;
        return 1;
    }

    (void)cpu; (void)addr;
    return 0;
}

/* SYSCALL — PS1 syscall instruction (immediate=0, actual code in a0):
 *   a0=1: EnterCritical — no-op (no real interrupts in our model)
 *   a0=2: ExitCritical  — no-op
 *
 * On real PS1, EnterCritical/ExitCritical just toggle the interrupt enable flag.
 * Thread dispatch is driven by the scheduler (FUN_80017024) via ChangeThread calls,
 * NOT by the interrupt mechanism.  Making these no-ops is correct:
 *
 *   FUN_80017154 (restart loading fiber) calls:
 *     EnterCritical → OpenThread → ExitCritical → return
 *   The caller (FUN_800222b8) then yields in a while loop until loading is done.
 *
 *   FUN_80017208 (loading batch done) calls:
 *     EnterCritical → CloseThread → ExitCritical → ChangeThread(0xFF000000)
 *
 *   FUN_80017024 (scheduler, state==3 path) calls:
 *     EnterCritical → OpenThread → store handle → ExitCritical → ChangeThread(handle)
 *
 * In all cases ExitCritical must return so the caller can continue. */
void psx_syscall(CPUState* cpu, uint32_t code) {
    (void)code;
    cpu->v0 = 0;
}

/* Joypad state injection — called from psx_present_frame() every frame.
 * FUN_80028D70 (compiled MIPS, not intercepted — JAL, not JALR) reads
 * _DAT_8009eb5a (g_ram[0x9eb5a]) as active-low pad data, then inverts it.
 * FUN_800223e0 calls FUN_80028D70 → stores result in _DAT_8009c9d8 →
 * computes _DAT_1f8001fc = held & ~prev_held (newly-pressed edge).
 * The game reads _DAT_1f8001fc for button events.
 *
 * We write ~buttons (active-low) to g_ram[0x9eb5a] so FUN_80028D70 returns
 * the correct active-high bitmask after inversion. */
void psx_set_pad1(uint16_t buttons) {
    g_pad1_state = buttons;
    /* Arm INTERP-CALL trace window on Circle (0x2000) or Square (0x8000) press */
    static uint16_t s_prev_buttons = 0;
    uint16_t newly_pressed = buttons & ~s_prev_buttons;
    if (newly_pressed & (0x2000u | 0x8000u))
        g_attack_trace_end_frame = g_ps1_frame + 200;
    /* Flush trace output once when the window expires */
    static uint32_t s_prev_trace_end = 0;
    if (s_prev_trace_end != g_attack_trace_end_frame &&
        g_ps1_frame >= g_attack_trace_end_frame && g_attack_trace_end_frame > 0) {
        fflush(stdout);
        s_prev_trace_end = g_attack_trace_end_frame;
    }
    s_prev_buttons = buttons;

    uint16_t active_low = ~buttons;
    g_ram[0x9eb5a] = (uint8_t)(active_low & 0xFF);
    g_ram[0x9eb5b] = (uint8_t)(active_low >> 8);
    /* Clear pad transfer status byte (0x9EB58) — 0 = controller OK.
     * The game checks DAT_8009eb58 != 0 as "controller not connected"
     * and auto-opens the pause menu when non-zero. BIOS pad handlers
     * or game init may write 0xFF here; ensure it stays 0. */
    g_ram[0x9eb58] = 0;
}

uint8_t* psx_get_ram(void) { return g_ram; }
uint8_t* psx_get_scratch(void) { return g_scratch; }
