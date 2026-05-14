/* freeze_heartbeat.c — see header for rationale. */

#include "freeze_heartbeat.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* State accessors. All defined in other compilation units; declared here
 * to avoid pulling in heavy headers. */
extern uint64_t s_frame_count;                 /* debug_server.c */
extern uint32_t g_debug_current_func_addr;     /* debug_server.c */
extern uint32_t g_debug_last_store_pc;         /* debug_server.c */
extern uint32_t i_stat;                        /* memory.c */
extern uint32_t i_mask;                        /* memory.c */

extern uint64_t psx_get_cycle_count(void);
extern void     psx_get_freeze_diag(uint64_t *total_checks,
                                    uint32_t *dispatch_count,
                                    int *in_exception,
                                    int *post_exception_cooldown,
                                    uint64_t *exception_entries,
                                    uint64_t *exception_reentry_blocks);
extern void     sio_get_freeze_diag(int *irq_pending, int *irq_countdown,
                                    uint16_t *sio_stat_out, uint16_t *sio_ctrl_out,
                                    int *card_active);
extern int      sio_get_mc_max_state(void);
extern int      sio_get_tx_writes(void);

/* dirty_ram_interp counters — defined as plain globals in dirty_ram_interp.c.
 * Declared weakly here in case a future build drops the unit; missing symbol
 * would only fail at link time and is easy to catch. */
extern uint64_t g_dirty_ram_blocks_run;
extern uint64_t g_dirty_ram_insns_run;

static int s_started = 0;
static char s_backend[32] = "psx-runtime";

#ifdef _WIN32
static HANDLE s_thread = NULL;
#endif

#define HB_FILE      "psx_freeze_heartbeat.json"
#define HB_INTERVAL_MS 100u

static void heartbeat_write(void) {
    uint64_t cyc = psx_get_cycle_count();
    uint64_t frame = s_frame_count;
    uint32_t cur_fn = g_debug_current_func_addr;
    uint32_t last_store = g_debug_last_store_pc;

    uint64_t total_checks = 0;
    uint32_t dispatch_count = 0;
    int in_exc = 0;
    int post_cool = 0;
    uint64_t exc_entries = 0;
    uint64_t exc_reentry = 0;
    psx_get_freeze_diag(&total_checks, &dispatch_count, &in_exc,
                        &post_cool, &exc_entries, &exc_reentry);

    int sio_irq_pending = 0;
    int sio_irq_countdown = 0;
    uint16_t sio_stat = 0;
    uint16_t sio_ctrl = 0;
    int card_active = 0;
    sio_get_freeze_diag(&sio_irq_pending, &sio_irq_countdown,
                        &sio_stat, &sio_ctrl, &card_active);

    int mc_max = sio_get_mc_max_state();
    int tx_writes = sio_get_tx_writes();

    /* Wall-clock seconds since epoch — coarse but enough to spot stalls. */
    long long wall = (long long)time(NULL);

    char buf[2048];
    int n = snprintf(buf, sizeof(buf),
        "{\n"
        "  \"backend\":\"%s\",\n"
        "  \"wall_clock_epoch\":%lld,\n"
        "  \"frame_count\":%llu,\n"
        "  \"psx_cycle_count\":%llu,\n"
        "  \"current_func\":\"0x%08X\",\n"
        "  \"last_store_pc\":\"0x%08X\",\n"
        "  \"total_checks\":%llu,\n"
        "  \"dispatch_count\":%u,\n"
        "  \"in_exception\":%d,\n"
        "  \"post_exception_cooldown\":%d,\n"
        "  \"exception_entries\":%llu,\n"
        "  \"exception_reentry_blocks\":%llu,\n"
        "  \"sio_irq_pending\":%d,\n"
        "  \"sio_irq_countdown\":%d,\n"
        "  \"sio_stat\":\"0x%04X\",\n"
        "  \"sio_ctrl\":\"0x%04X\",\n"
        "  \"sio_card_active\":%d,\n"
        "  \"i_stat\":\"0x%08X\",\n"
        "  \"i_mask\":\"0x%08X\",\n"
        "  \"mc_max_state\":%d,\n"
        "  \"tx_writes\":%d,\n"
        "  \"dirty_ram_blocks\":%llu,\n"
        "  \"dirty_ram_insns\":%llu\n"
        "}\n",
        s_backend,
        wall,
        (unsigned long long)frame,
        (unsigned long long)cyc,
        cur_fn,
        last_store,
        (unsigned long long)total_checks,
        dispatch_count,
        in_exc,
        post_cool,
        (unsigned long long)exc_entries,
        (unsigned long long)exc_reentry,
        sio_irq_pending,
        sio_irq_countdown,
        (unsigned)sio_stat,
        (unsigned)sio_ctrl,
        card_active,
        i_stat, i_mask,
        mc_max,
        tx_writes,
        (unsigned long long)g_dirty_ram_blocks_run,
        (unsigned long long)g_dirty_ram_insns_run);

    if (n <= 0) return;

    /* Atomic overwrite via .tmp + rename. Avoids a reader catching a
     * mid-write file and parsing partial JSON. Cheap on Windows
     * (MoveFileEx with REPLACE_EXISTING). */
    char tmp_path[64];
    snprintf(tmp_path, sizeof(tmp_path), HB_FILE ".tmp");

    FILE *f = fopen(tmp_path, "wb");
    if (!f) return;
    fwrite(buf, 1, (size_t)n, f);
    fclose(f);

#ifdef _WIN32
    MoveFileExA(tmp_path, HB_FILE,
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    rename(tmp_path, HB_FILE);
#endif
}

#ifdef _WIN32
static DWORD WINAPI heartbeat_thread(LPVOID arg) {
    (void)arg;
    for (;;) {
        heartbeat_write();
        Sleep(HB_INTERVAL_MS);
    }
}
#endif

void freeze_heartbeat_start(const char *backend_label) {
#ifdef PSX_NO_DEBUG_TOOLS
    (void)backend_label;
    return;
#endif
    if (s_started) return;
    if (backend_label && backend_label[0]) {
        size_t n = strlen(backend_label);
        if (n >= sizeof(s_backend)) n = sizeof(s_backend) - 1;
        memcpy(s_backend, backend_label, n);
        s_backend[n] = 0;
    }
#ifdef _WIN32
    s_thread = CreateThread(NULL, 0, heartbeat_thread, NULL, 0, NULL);
    if (s_thread) s_started = 1;
#endif
}
