#ifdef PSX_COSIM
/* cosim.c — first-divergence co-simulation engine + minimal TCP server.
 * See COSIM_ORACLE.md. Active ONLY in the clean `psx-cosim` target (PSX_COSIM); the
 * whole translation unit is empty otherwise, so it is safe in the shared source list.
 *
 * Model: the guest runs normally on the main thread. At every basic-block leader the
 * emitted/interp hook calls cosim_block(pc): it stamps the block into a ring, folds a
 * cumulative chain hash, and — when the coordinator has asked to stop at a target
 * sequence — PARKS the guest thread there so the coordinator can read a quiescent
 * machine over TCP, compare, and resume. Two processes (compiled + interp) driven in
 * lockstep by tools/cosim.py converge on the first divergent block, then drill.
 *
 * Determinism is a hard requirement (both runs must be bit-identical until the real
 * divergence). The clean build runs headless, single-threaded guest, software renderer,
 * no host-time throttle. The compiled-vs-compiled gate proves it.
 */
#include "cosim_state.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
#define COSIM_SLEEP(ms) Sleep(ms)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <pthread.h>
typedef int sock_t;
#define COSIM_SLEEP(ms) usleep((ms)*1000)
#endif

extern uint32_t i_stat, i_mask;
extern uint64_t psx_cycle_count;

/* EXPERIMENTAL input-driven cosim: held pad state applied every frame by the headless
 * sampler (main.cpp). 0xFFFF = all released (PSX pad active-low). Set via `setpad`.
 * If two instances desync under input, this path is proven nondeterministic. */
volatile int g_cosim_pad_hold[2] = { 0xFFFF, 0xFFFF };

#define RING_N 8192u   /* bounded reporting window (power of two) */
typedef struct { uint64_t cp; uint32_t pc; uint64_t hash; uint32_t istat, imask; uint64_t cycle; } Entry;
static Entry    g_ring[RING_N];
static uint64_t g_cp      = 0;          /* checkpoints crossed so far */
static uint64_t g_chain   = 1469598103934665603ULL; /* cumulative FNV over checkpoints */
static uint64_t g_stride  = 4096;       /* retired instructions per checkpoint */
static uint64_t g_next_cp = 0;          /* icount at which to take the next checkpoint */
static uint32_t g_last_leader_pc = 0;   /* set by cosim_block, reported at checkpoints */
static uint64_t g_icount  = 0;          /* retirement events (cosim_instr calls) */

/* lockstep control (written by TCP thread, read by guest thread).
 * The guest parks at EVERY checkpoint boundary (a deterministic guest cycle = multiple
 * of stride) and only advances when the coordinator grants budget via `step N`. It does
 * NOT free-run: the earlier "free-run until you notice an async stop cycle" design was
 * racy — two processes noticed the flag at different wall-times and parked at different
 * cycles (a HARNESS nondeterminism, not a guest one). Parking at fixed checkpoint
 * boundaries makes both processes stop at identical cycles by construction. */
static volatile int64_t  g_run_budget = 0;  /* checkpoints the guest may cross before parking */
static volatile int      g_parked     = 0;
static volatile int      g_go_token   = 0;  /* bump to wake a parked guest to re-check budget */

static inline uint64_t fold(uint64_t h, uint64_t v) {
    for (int i = 0; i < 8; i++) { h ^= (uint8_t)(v >> (i*8)); h *= 1099511628211ULL; }
    return h;
}

/* Block-leader hook (emitted under PSX_COSIM + called from the interp loop). Cheap:
 * just stashes the last leader PC for human-readable reporting — the actual compare is
 * the cycle-keyed checkpoint below, which aligns across both backends. */
void cosim_block(uint32_t pc) { g_last_leader_pc = pc; }
uint32_t cosim_last_block(void) { return g_last_leader_pc; }

uint32_t cosim_cycles_to_next_checkpoint(void) {
    return 0;   /* checkpoints are icount-keyed; no cycle sub-step cap needed */
}

/* Instruction-count-keyed checkpoint — taken inside cosim_instr, which both
 * backends call exactly once per retirement event (a branch and its delay slot
 * retire as ONE event in both the dirty-RAM interp and the emitted code). That
 * makes checkpoint k "the state after k*stride retirements" by construction, so
 * the two instances always hash the SAME architectural position. The guest-cycle
 * clock is part of the hash (cosim_state.c), so any cycle-accounting drift shows
 * up as a chain divergence AT an exact instruction instead of silently skewing
 * where the two instances park (the old cycle-keyed design parked at the first
 * instruction boundary after the stride cycle, which the two backends could
 * reach at different retirement positions — phantom divergences). */
static void cosim_record_checkpoint(uint64_t icount, uint32_t pc) {
    uint64_t h = cosim_state_hash(NULL);
    uint64_t cp = ++g_cp;
    g_chain = fold(fold(g_chain, icount), h);
    Entry *e = &g_ring[cp & (RING_N - 1u)];
    e->cp = cp; e->pc = pc; e->hash = h;
    e->istat = i_stat; e->imask = i_mask; e->cycle = psx_cycle_count;

    /* deterministic park: consume one checkpoint of budget, else block for `step`.
     * The guest ALWAYS stops here (a fixed icount boundary), never at a wall-time point. */
    if (g_run_budget > 0) { g_run_budget--; return; }
    g_parked = 1;
    while (g_run_budget <= 0) {
        int tok = g_go_token;
        while (g_go_token == tok && g_run_budget <= 0) COSIM_SLEEP(1);
    }
    g_parked = 0;
    g_run_budget--;
}

void cosim_tick(void) {
    /* Retained for the psx_advance_cycles call sites; checkpointing moved to
     * the icount key in cosim_instr (see cosim_record_checkpoint comment). */
}

void cosim_instr(uint32_t pc) {
    g_last_leader_pc = pc;
    g_icount++;
    if (g_cp == 0 && g_next_cp == 0) {
        /* Initial park at the very first retirement: the coordinator gains
         * control before the guest free-runs (unless PSX_COSIM_START_CYCLE
         * set a free-run target, which makes g_next_cp nonzero). */
        cosim_record_checkpoint(g_icount, pc);
        g_next_cp = g_icount + (g_stride ? g_stride : 1);
        return;
    }
    if (g_icount >= g_next_cp) {
        cosim_record_checkpoint(g_icount, pc);
        g_next_cp = g_icount + (g_stride ? g_stride : 1);
    }
}

/* ---------------- minimal TCP command server (own thread) ---------------- */
/* Line protocol, whitespace-separated. Replies one line.
 *   seq                       -> "seq <n> chain <hex> parked <0|1>"
 *   runto <n>                 -> set stop=n, resume; blocks until parked at n (or exit)
 *   chain                     -> "chain <hex> seq <n>"
 *   hash                      -> "hash <hex> pc <hex> istat <hex> imask <hex> cyc <n>"
 *   sub                       -> per-subsystem hashes of the current state
 *   window <n>                -> last n ring rows (bounded to RING_N)
 *   inject ram <phys> <xor>   -> gate-4 fault into RAM
 *   inject reg <idx> <xor>    -> gate-4 fault into a CPU reg (idx 0..31, 32=hi,33=lo)
 *   reset                     -> reset incremental hash state
 */
static void handle_line(sock_t s, char *line);

static int send_line(sock_t s, const char *buf) {
    size_t n = strlen(buf);
    size_t off = 0;
    while (off < n) {
        int sent = send(s, buf + off, (int)(n - off), 0);
        if (sent <= 0) return 0;
        off += (size_t)sent;
    }
    return 1;
}

static void server_loop(unsigned short port) {
    sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == (sock_t)-1) return;
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
    if (bind(ls, (struct sockaddr*)&a, sizeof a) != 0) return;
    listen(ls, 4);
    for (;;) {
        sock_t c = accept(ls, NULL, NULL);
        if (c == (sock_t)-1) { COSIM_SLEEP(5); continue; }
        char buf[512]; int used = 0;
        for (;;) {
            int r = recv(c, buf + used, (int)sizeof(buf) - 1 - used, 0);
            if (r <= 0) break;
            used += r; buf[used] = 0;
            char *nl;
            while ((nl = strchr(buf, '\n')) != NULL) {
                *nl = 0;
                char *cr = strchr(buf, '\r'); if (cr) *cr = 0;
                handle_line(c, buf);
                size_t rest = strlen(nl + 1);
                memmove(buf, nl + 1, rest + 1);
                used = (int)rest;
            }
            if (used >= (int)sizeof(buf) - 1) used = 0; /* overlong line: drop */
        }
#ifdef _WIN32
        closesocket(c);
#else
        close(c);
#endif
    }
}

static void handle_line(sock_t s, char *line) {
    char out[1024];
    char cmd[32] = {0};
    if (sscanf(line, "%31s", cmd) != 1) { send_line(s, "err empty\n"); return; }

    if (!strcmp(cmd, "status")) {
        snprintf(out, sizeof out, "cp %llu cycle %llu icnt %llu chain %016llx stride %llu parked %d\n",
                 (unsigned long long)g_cp, (unsigned long long)psx_cycle_count,
                 (unsigned long long)g_icount,
                 (unsigned long long)g_chain, (unsigned long long)g_stride, g_parked);
        send_line(s, out); return;
    }
    if (!strcmp(cmd, "chain")) {
        snprintf(out, sizeof out, "chain %016llx cp %llu cycle %llu\n",
                 (unsigned long long)g_chain, (unsigned long long)g_cp,
                 (unsigned long long)psx_cycle_count);
        send_line(s, out); return;
    }
    if (!strcmp(cmd, "stride")) {
        /* Stride fixes the checkpoint cycle boundaries; set ONCE before stepping so both
         * processes checkpoint at identical cycles. Changing it mid-run is refused. */
        unsigned long long n = 0;
        if (sscanf(line, "%*s %llu", &n) != 1 || n == 0) { send_line(s, "err stride\n"); return; }
        if (g_cp != 0) { send_line(s, "err stride-after-start\n"); return; }
        g_stride = (uint64_t)n; g_next_cp = 0; send_line(s, "ok\n"); return;
    }
    if (!strcmp(cmd, "step")) {           /* step <N-checkpoints>; parks at cp+N */
        unsigned long long n = 1;
        sscanf(line, "%*s %llu", &n);
        g_run_budget += (int64_t)n;
        g_go_token++;                       /* wake the parked guest */
        /* wait until it parks again (crossed N checkpoints), or the guest exits */
        int spins = 0;
        while (g_run_budget > 0 && spins < 1200000) { COSIM_SLEEP(1); spins++; }
        /* small settle so g_parked/g_chain reflect the checkpoint just recorded */
        int s2 = 0; while (!g_parked && g_run_budget <= 0 && s2 < 2000) { COSIM_SLEEP(1); s2++; }
        snprintf(out, sizeof out, "%s cp %llu cycle %llu icnt %llu chain %016llx\n",
                 g_parked ? "parked" : "running",
                 (unsigned long long)g_cp, (unsigned long long)psx_cycle_count,
                 (unsigned long long)g_icount,
                 (unsigned long long)g_chain);
        send_line(s, out); return;
    }
    if (!strcmp(cmd, "hash")) {
        Entry *e = &g_ring[g_cp & (RING_N - 1u)];
        snprintf(out, sizeof out, "hash %016llx pc %08x istat %08x imask %08x cyc %llu\n",
                 (unsigned long long)e->hash, e->pc, e->istat, e->imask,
                 (unsigned long long)e->cycle);
        send_line(s, out); return;
    }
    if (!strcmp(cmd, "sub")) {
        CosimSubHashes h; cosim_state_hash(&h);
        snprintf(out, sizeof out,
            "cpu %016llx irqctl %016llx ram %016llx scratch %016llx vram %016llx "
            "gpu %016llx spu %016llx cdrom %016llx dma %016llx sio %016llx "
            "timers %016llx clock %016llx dirty %016llx\n",
            (unsigned long long)h.cpu,(unsigned long long)h.irqctl,(unsigned long long)h.ram,
            (unsigned long long)h.scratch,(unsigned long long)h.vram,(unsigned long long)h.gpu,
            (unsigned long long)h.spu,(unsigned long long)h.cdrom,(unsigned long long)h.dma,
            (unsigned long long)h.sio,(unsigned long long)h.timers,(unsigned long long)h.clock,
            (unsigned long long)h.dirty);
        send_line(s, out); return;
    }
    if (!strcmp(cmd, "window")) {
        unsigned long long n = 32;
        sscanf(line, "%*s %llu", &n);
        if (n > RING_N) n = RING_N;
        if (n > g_cp) n = g_cp;
        for (uint64_t i = 0; i < n; i++) {
            uint64_t cp = g_cp - (n - 1 - i);
            Entry *e = &g_ring[cp & (RING_N - 1u)];
            snprintf(out, sizeof out, "row cp %llu pc %08x hash %016llx istat %08x cyc %llu\n",
                     (unsigned long long)e->cp, e->pc, (unsigned long long)e->hash,
                     e->istat, (unsigned long long)e->cycle);
            send_line(s, out);
        }
        send_line(s, "end\n"); return;
    }
    if (!strcmp(cmd, "inject")) {
        char what[8] = {0}; unsigned long a=0, b=0;
        if (sscanf(line, "%*s %7s %lu %lu", what, &a, &b) == 3) {
            if (!strcmp(what, "ram")) { cosim_inject_ram((uint32_t)a, (uint8_t)b); send_line(s, "ok\n"); return; }
            if (!strcmp(what, "reg")) { cosim_inject_reg((int)a, (uint32_t)b); send_line(s, "ok\n"); return; }
        }
        send_line(s, "err inject\n"); return;
    }
    if (!strcmp(cmd, "cpu")) {
        /* Full CPU field dump so the coordinator can field-diff a cpu-subsystem
         * divergence (which register / micro-state field actually split). */
        extern CPUState *debug_cpu_ptr;
        CPUState *c = debug_cpu_ptr;
        if (!c) { send_line(s, "err no-cpu\n"); return; }
        char *p = out; size_t rem = sizeof out;
        int w = snprintf(p, rem, "pc %08x hi %08x lo %08x", c->pc, c->hi, c->lo);
        p += w; rem -= w;
        for (int i = 0; i < 32; i++) { w = snprintf(p, rem, " r%d %08x", i, c->gpr[i]); p+=w; rem-=w; }
        for (int i = 0; i < 16; i++) { w = snprintf(p, rem, " c%d %08x", i, c->cop0[i]); p+=w; rem-=w; }
        w = snprintf(p, rem, " mdts %llx gtets %llx rfudge %02x rabw %02x ldw %02x ldabs %08x\n",
                     (unsigned long long)c->muldiv_ts_done, (unsigned long long)c->gte_ts_done,
                     c->read_fudge, c->read_absorb_which, c->ld_which_t, c->ld_absorb);
        send_line(s, out); return;
    }
    if (!strcmp(cmd, "gte")) {
        extern CPUState *debug_cpu_ptr;
        CPUState *c = debug_cpu_ptr;
        if (!c) { send_line(s, "err no-cpu\n"); return; }
        char big[4096];
        char *p = big; size_t rem = sizeof big;
        for (int i = 0; i < 32 && rem > 1; i++) {
            uint32_t val = c->gte_data[i];
            if ((i >= 8 && i <= 11) || i == 15 || i == 28 || i == 29 || i == 31) {
                val = gte_read_data(c, (uint8_t)i);
            }
            int w = snprintf(p, rem, " gd%d %08x", i, val);
            if (w < 0 || (size_t)w >= rem) { p += rem - 1; rem = 1; break; }
            p += w; rem -= (size_t)w;
        }
        for (int i = 0; i < 32 && rem > 1; i++) {
            int w = snprintf(p, rem, " gc%d %08x", i, c->gte_ctrl[i]);
            if (w < 0 || (size_t)w >= rem) { p += rem - 1; rem = 1; break; }
            p += w; rem -= (size_t)w;
        }
        snprintf(p, rem, "\n");
        send_line(s, big); return;
    }
    if (!strcmp(cmd, "dev")) {
        /* Device-timing field dump: the fields behind the `irqctl` and `gpu`
         * sub-hashes, so a device divergence can be field-diffed (esp.
         * cycles_since_vblank = VBLANK-raise-timing, and GPUSTAT). */
        extern void interrupts_cosim_dump(uint32_t*, int*);
        extern uint32_t gpu_read_gpustat(void);
        uint32_t csv = 0; int inexc = 0; interrupts_cosim_dump(&csv, &inexc);
        snprintf(out, sizeof out,
            "csv %08x inexc %d imask %08x istat %08x gpustat %08x\n",
            csv, inexc, i_mask, i_stat, gpu_read_gpustat());
        send_line(s, out); return;
    }
    if (!strcmp(cmd, "gpu")) {
        extern void gpu_cosim_dump(char *out, int cap);
        char big[4096];
        gpu_cosim_dump(big, (int)sizeof big);
        send_line(s, big); return;
    }
    if (!strcmp(cmd, "irqtrace")) {
        extern void interrupts_cosim_irq_dump(char *out, int cap);
        char big[32768];
        interrupts_cosim_irq_dump(big, (int)sizeof big);
        send_line(s, big); return;
    }
    if (!strcmp(cmd, "setpad")) {
        /* EXPERIMENTAL input-driven cosim: set the HELD pad state (applied every frame).
         * Coordinator issues the SAME value to both instances while parked at a
         * checkpoint, so input reaches both at the same guest cycle. `setpad <slot> <hex>`
         * (slot 0/1; 0xFFFF = released, active-low). Desync under this => nondeterministic. */
        unsigned slot = 0, val = 0xFFFFu;
        if (sscanf(line, "%*s %u %x", &slot, &val) >= 1 && slot < 2) {
            g_cosim_pad_hold[slot] = (int)(val & 0xFFFFu); send_line(s, "ok\n"); return;
        }
        send_line(s, "err setpad\n"); return;
    }
    if (!strcmp(cmd, "reset")) { cosim_state_reset(); send_line(s, "ok\n"); return; }
    send_line(s, "err unknown\n");
}

#ifdef _WIN32
static DWORD WINAPI server_thread(LPVOID p) { server_loop((unsigned short)(uintptr_t)p); return 0; }
#else
static void *server_thread(void *p) { server_loop((unsigned short)(uintptr_t)p); return NULL; }
#endif

/* Call once at startup (from the clean-build main, before the guest runs). Port from
 * env PSX_COSIM_PORT (default 4600). */
void cosim_init(void) {
    cosim_state_reset();
    unsigned short port = 4600;
    const char *e = getenv("PSX_COSIM_PORT");
    if (e && *e) port = (unsigned short)atoi(e);
    /* Stride fixed at launch (env) so the checkpoint icount boundaries are identical
     * in both processes before either runs a single instruction — no set-stride race.
     * Both stride and start are in RETIRED INSTRUCTIONS (icount), not guest cycles. */
    const char *st = getenv("PSX_COSIM_STRIDE");
    if (st && *st) { unsigned long long v = strtoull(st, 0, 10); if (v) g_stride = v; }
    const char *sc = getenv("PSX_COSIM_START_CYCLE");
    if (sc && *sc) { unsigned long long v = strtoull(sc, 0, 10); g_next_cp = (uint64_t)v; }
#ifdef _WIN32
    WSADATA w; WSAStartup(MAKEWORD(2,2), &w);
    CreateThread(NULL, 0, server_thread, (LPVOID)(uintptr_t)port, 0, NULL);
#else
    pthread_t t; pthread_create(&t, NULL, server_thread, (void*)(uintptr_t)port);
#endif
    fprintf(stdout, "cosim: first-divergence oracle listening on 127.0.0.1:%u\n", port);
}

#endif /* PSX_COSIM */
