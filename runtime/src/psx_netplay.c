#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "psx_netplay.h"

#include "memcard.h"
#include "savestate.h"
#include "sio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <sched.h>
#include <unistd.h>
#endif

#if defined(PSX_HAS_RECOMP_NET)
#include "recomp_net/recomp_net.h"
#endif

void psx_netplay_config_defaults(PsxNetplayConfig *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->local_slot = 0;
    cfg->input_player = -1;
    cfg->input_delay = 2;
    cfg->session_id = 1;
    strncpy(cfg->bind_hostport, "0.0.0.0:7777", sizeof(cfg->bind_hostport) - 1);
    strncpy(cfg->peer_hostport, "127.0.0.1:7778", sizeof(cfg->peer_hostport) - 1);
}

static unsigned env_u(const char *name, unsigned def)
{
    const char *v = getenv(name);
    if (!v || !v[0]) return def;
    return (unsigned)strtoul(v, NULL, 10);
}

void psx_netplay_apply_env(PsxNetplayConfig *cfg)
{
    const char *v;
    if (!cfg) return;
    v = getenv("PSX_NETPLAY");
    if (v && v[0] && v[0] != '0') cfg->enabled = 1;
    v = getenv("PSX_NET_SLOT");
    if (v && v[0]) cfg->local_slot = (int)strtol(v, NULL, 10);
    v = getenv("PSX_NET_INPUT_PLAYER");
    if (v && v[0]) cfg->input_player = (int)strtol(v, NULL, 10);
    v = getenv("PSX_NET_DELAY");
    if (v && v[0]) cfg->input_delay = (int)strtol(v, NULL, 10);
    cfg->session_id = env_u("PSX_NET_SESSION_ID", cfg->session_id);
    v = getenv("PSX_NET_BIND");
    if (v && v[0]) {
        strncpy(cfg->bind_hostport, v, sizeof(cfg->bind_hostport) - 1);
        cfg->bind_hostport[sizeof(cfg->bind_hostport) - 1] = '\0';
    }
    v = getenv("PSX_NET_PEER");
    if (v && v[0]) {
        strncpy(cfg->peer_hostport, v, sizeof(cfg->peer_hostport) - 1);
        cfg->peer_hostport[sizeof(cfg->peer_hostport) - 1] = '\0';
    }
}

void psx_netplay_normalize_pad(PsxNetPad *pad)
{
    const int dead = 24; /* ~SDL-ish center deadzone in 0..255 space */
    if (!pad) return;
    pad->connected = 1;
    if (pad->lx > (uint8_t)(0x80 - dead) && pad->lx < (uint8_t)(0x80 + dead)) pad->lx = 0x80;
    if (pad->ly > (uint8_t)(0x80 - dead) && pad->ly < (uint8_t)(0x80 + dead)) pad->ly = 0x80;
    if (pad->rx > (uint8_t)(0x80 - dead) && pad->rx < (uint8_t)(0x80 + dead)) pad->rx = 0x80;
    if (pad->ry > (uint8_t)(0x80 - dead) && pad->ry < (uint8_t)(0x80 + dead)) pad->ry = 0x80;
    if (!pad->analog) {
        pad->lx = pad->ly = pad->rx = pad->ry = 0x80;
    }
}

static void force_session_pads_connected(int slot_count)
{
    int i;
    if (slot_count < 2) slot_count = 2;
    if (slot_count > 2) slot_count = 2;
    for (i = 0; i < slot_count; ++i) {
        sio_connect_pad(i);
        sio_set_pad_config_capable(i, 1);
    }
}

void psx_netplay_release_pads(void)
{
    force_session_pads_connected(2);
    sio_set_pad_state_slot(0, 0xFFFFu);
    sio_set_pad_state_slot(1, 0xFFFFu);
    sio_set_pad_sticks(0, 0x80, 0x80, 0x80, 0x80);
    sio_set_pad_sticks(1, 0x80, 0x80, 0x80, 0x80);
    sio_request_pad_type(0, 1);
    sio_request_pad_type(1, 1);
}

#if !defined(PSX_HAS_RECOMP_NET)

int  psx_netplay_active(void) { return 0; }
int  psx_netplay_is_running(void) { return 0; }
int  psx_netplay_local_slot(void) { return -1; }
int  psx_netplay_input_player(void) { return 0; }
uint32_t psx_netplay_sim_tick(void) { return 0; }
int  psx_netplay_start(const PsxNetplayConfig *cfg)
{
    (void)cfg;
    return -1;
}
void psx_netplay_shutdown(void) {}
void psx_netplay_stage_local(const PsxNetPad *pad) { (void)pad; }
int  psx_netplay_needs_local_sample(void) { return 0; }
int  psx_netplay_input_desync(uint32_t *tick, uint32_t *local_hash, uint32_t *remote_hash)
{
    (void)tick;
    (void)local_hash;
    (void)remote_hash;
    return 0;
}
int  psx_netplay_peer_disconnected(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return 0;
}
void psx_netplay_bind_guest_saves(void) {}
int  psx_netplay_is_host(void) { return 0; }
int  psx_netplay_request_save(int slot) { (void)slot; return 0; }
int  psx_netplay_request_load(int slot) { (void)slot; return 0; }
int  psx_netplay_in_load_barrier(void) { return 0; }
int  psx_netplay_poll_admit(void) { return 1; }
void psx_netplay_finish_frame(void) {}
void psx_netplay_wait_recv(int timeout_ms) { (void)timeout_ms; }

#else /* PSX_HAS_RECOMP_NET */

#define NP_SANDBOX_DIR "saves/netplay"
#define NP_MC_BLOB_BYTES (4u + (size_t)MEMCARD_SIZE * 2u)
/* LOAD probe size==0 + this crc = post-load ready rendezvous (not SAVE coord). */
#define NP_LOAD_READY_CRC 0x4C4F4144u /* 'LOAD' */

typedef enum {
    NP_XFER_NONE = 0,
    NP_XFER_MC_PROBE,
    NP_XFER_MC_SEND,
    NP_XFER_SAVE_COORD,
    NP_XFER_SAVE_PROBE,
    NP_XFER_SAVE_SEND,
    NP_XFER_LOAD_PROBE,
    NP_XFER_LOAD_SEND,
    NP_XFER_LOAD_APPLYING, /* load staged; admit runs until savestate_poll fires */
    NP_XFER_LOAD_READY     /* local restore done; wait peer before lockstep */
} NpXferPhase;

typedef struct {
    RNetSession *session;
    PsxNetPad    staged;
    int          staged_valid;
    int          active;
    int          slot_count;
    int          local_slot;
    int          input_player; /* resolved 0/1 */
    int          needs_advance;
    int          latched_for_tick; /* 1 if staged pad frozen for current sim_tick */
    uint32_t     latched_sim_tick;
    /* Guest sandbox: personal roots restored on shutdown. */
    int          guest_sandbox;
    char         personal_save_dir[512];
    char         personal_mc0[512];
    char         personal_mc1[512];
    uint32_t     bios_checksum;
    uint32_t     entry_pc;
    /* Host-owned save/memcard sync. */
    NpXferPhase  xfer;
    int          xfer_slot;
    int          mc_sync_done;
    int          mc_sync_sent;
    int          local_save_staged;
    int          load_applied_local;
    int          load_ready_replied; /* READY exchanged; synced; stay LOAD_READY until admit */
    int          load_sync_done;     /* hard_resync+prime once at mutual ready */
} NetplayState;

static NetplayState g_np;

static void np_enter_load_ready(int slot);
static void np_commit_load_sync(void);
static void np_begin_load_apply(int slot);

static int np_file_crc(const uint8_t *data, size_t size, uint32_t *crc_out)
{
    if (!data || size == 0 || !crc_out) return 0;
    *crc_out = rnet_checksum(data, size);
    return 1;
}

static int np_slot_crc(int slot, uint32_t *size_out, uint32_t *crc_out)
{
    uint8_t *data = NULL;
    size_t size = 0;
    uint32_t crc;
    if (!savestate_read_slot(slot, &data, &size) || !data) return 0;
    if (!np_file_crc(data, size, &crc)) {
        free(data);
        return 0;
    }
    if (size_out) *size_out = (uint32_t)size;
    if (crc_out) *crc_out = crc;
    free(data);
    return 1;
}

static int np_build_mc_blob(uint8_t *out, size_t cap, size_t *out_size)
{
    uint8_t *p;
    if (!out || cap < NP_MC_BLOB_BYTES || !out_size) return -1;
    memset(out, 0, NP_MC_BLOB_BYTES);
    p = out;
    p[0] = memcard_is_present(0) ? 1u : 0u;
    p[1] = memcard_is_present(1) ? 1u : 0u;
    if (p[0] && memcard_export_raw(0, p + 4) != 0) return -1;
    if (p[1] && memcard_export_raw(1, p + 4 + MEMCARD_SIZE) != 0) return -1;
    *out_size = NP_MC_BLOB_BYTES;
    return 0;
}

static int np_apply_mc_blob(const uint8_t *data, size_t size)
{
    if (!data || size < NP_MC_BLOB_BYTES) return -1;
    if (data[0]) {
        if (memcard_import_raw(0, data + 4) != 0) return -1;
    }
    if (data[1]) {
        if (memcard_import_raw(1, data + 4 + MEMCARD_SIZE) != 0) return -1;
    }
    return 0;
}

static int np_mc_blob_crc(uint32_t *size_out, uint32_t *crc_out)
{
    uint8_t *blob = (uint8_t *)malloc(NP_MC_BLOB_BYTES);
    size_t sz = 0;
    uint32_t crc;
    if (!blob) return 0;
    if (np_build_mc_blob(blob, NP_MC_BLOB_BYTES, &sz) != 0) {
        free(blob);
        return 0;
    }
    if (!np_file_crc(blob, sz, &crc)) {
        free(blob);
        return 0;
    }
    if (size_out) *size_out = (uint32_t)sz;
    if (crc_out) *crc_out = crc;
    free(blob);
    return 1;
}

static int np_xfer_busy(void)
{
    if (!g_np.session) return 0;
    if (g_np.xfer != NP_XFER_NONE) return 1;
    return rnet_session_state_busy(g_np.session) ||
           rnet_session_state_take_ready(g_np.session, NULL, NULL, NULL, NULL);
}

static void np_enter_guest_sandbox(void)
{
    const char *dir = savestate_dir();
    const char *p0 = NULL;
    const char *p1 = NULL;
    uint32_t bios = 0, entry = 0;

    savestate_get_integrity(&bios, &entry);
    g_np.bios_checksum = bios;
    g_np.entry_pc = entry;
    if (dir && dir[0])
        strncpy(g_np.personal_save_dir, dir, sizeof(g_np.personal_save_dir) - 1);
    (void)memcard_debug_info(0, &p0, NULL, NULL, NULL);
    (void)memcard_debug_info(1, &p1, NULL, NULL, NULL);
    if (p0) strncpy(g_np.personal_mc0, p0, sizeof(g_np.personal_mc0) - 1);
    if (p1) strncpy(g_np.personal_mc1, p1, sizeof(g_np.personal_mc1) - 1);

    savestate_configure(NP_SANDBOX_DIR, bios, entry);
    (void)memcard_rebind_dir(NP_SANDBOX_DIR);
    g_np.guest_sandbox = 1;
}

static void np_leave_guest_sandbox(void)
{
    if (!g_np.guest_sandbox) return;
    memcard_flush_all();
    (void)memcard_rebind_paths(
        g_np.personal_mc0[0] ? g_np.personal_mc0 : NULL,
        g_np.personal_mc1[0] ? g_np.personal_mc1 : NULL);
    (void)memcard_reload_bound();
    if (g_np.personal_save_dir[0])
        savestate_configure(g_np.personal_save_dir, g_np.bios_checksum, g_np.entry_pc);
    g_np.guest_sandbox = 0;
}

static void np_apply_ready_state(void)
{
    rnet_u8 op = 0, slot = 0;
    const void *data = NULL;
    size_t size = 0;

    if (!rnet_session_state_take_ready(g_np.session, &op, &slot, &data, &size))
        return;
    if (!data || size == 0) {
        rnet_session_state_finish(g_np.session, 0);
        g_np.xfer = NP_XFER_NONE;
        return;
    }

    if (op == RNET_STATE_OP_SRAM) {
        if (g_np.local_slot != 0 && np_apply_mc_blob((const uint8_t *)data, size) != 0) {
            rnet_session_state_finish(g_np.session, 0);
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        g_np.mc_sync_done = 1;
        rnet_session_state_finish(g_np.session, 0);
        g_np.xfer = NP_XFER_NONE;
        g_np.needs_advance = 0;
        g_np.latched_for_tick = 0;
        return;
    }

    if (op == RNET_STATE_OP_SAVE) {
        if (g_np.local_slot != 0) {
            if (!savestate_write_slot((int)slot, data, size)) {
                printf("psxrecomp: netplay guest save slot=%u — write failed\n", (unsigned)slot);
                fflush(stdout);
                rnet_session_state_finish(g_np.session, 0);
                g_np.xfer = NP_XFER_NONE;
                return;
            }
            /* Post-transfer hash verify against wire CRC. */
            {
                uint32_t got_sz = 0, got_crc = 0;
                if (!np_slot_crc((int)slot, &got_sz, &got_crc) ||
                    got_sz != (uint32_t)size ||
                    got_crc != rnet_checksum((const rnet_u8 *)data, size)) {
                    printf("psxrecomp: netplay guest save slot=%u — post-CRC mismatch\n",
                           (unsigned)slot);
                    fflush(stdout);
                    rnet_session_state_finish(g_np.session, 0);
                    g_np.xfer = NP_XFER_NONE;
                    return;
                }
            }
            printf("psxrecomp: netplay guest save slot=%u — synced (%zu bytes)\n",
                   (unsigned)slot, size);
            fflush(stdout);
        } else {
            printf("psxrecomp: netplay save slot=%u — transfer complete\n", (unsigned)slot);
            fflush(stdout);
        }
        rnet_session_state_finish(g_np.session, 0);
        g_np.xfer = NP_XFER_NONE;
        return;
    }

    /* LOAD transfer (hash miss): guest writes sandbox, both enter apply barrier. */
    if (g_np.local_slot != 0) {
        if (!savestate_write_slot((int)slot, data, size)) {
            rnet_session_state_finish(g_np.session, 0);
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        {
            uint32_t got_sz = 0, got_crc = 0;
            if (!np_slot_crc((int)slot, &got_sz, &got_crc) ||
                got_sz != (uint32_t)size ||
                got_crc != rnet_checksum((const rnet_u8 *)data, size)) {
                rnet_session_state_finish(g_np.session, 0);
                g_np.xfer = NP_XFER_NONE;
                return;
            }
        }
        (void)savestate_request_load_protocol((int)slot);
    }
    rnet_session_state_finish(g_np.session, 0);
    np_begin_load_apply((int)slot);
    printf("psxrecomp: netplay load slot=%u — applying after transfer…\n", (unsigned)slot);
    fflush(stdout);
}

static void np_guest_handle_probe(void)
{
    rnet_u8 op = 0, slot = 0;
    rnet_u32 size = 0, crc = 0;
    int match = 0;

    if (g_np.local_slot == 0) return;
    if (!rnet_session_state_probe_pending(g_np.session, &op, &slot, &size, &crc))
        return;

    /* Post-load ready rendezvous (must be before SAVE size==0 coord). */
    if (op == RNET_STATE_OP_LOAD && size == 0 && crc == NP_LOAD_READY_CRC) {
        if (g_np.xfer == NP_XFER_LOAD_APPLYING) {
            if (savestate_pending())
                return; /* still need guest cycles to apply */
            if (savestate_take_load_completed()) {
                np_enter_load_ready((int)slot);
            } else if (!g_np.load_applied_local) {
                return; /* staged but not yet applied */
            }
        }
        if (g_np.xfer != NP_XFER_LOAD_READY && !g_np.load_applied_local) {
            return;
        }
        /* ACK host ready, commit resync+prime once, then stay in LOAD_READY
         * until try_admit succeeds (host must probe_finish first). */
        if (rnet_session_state_probe_reply(g_np.session, 1) != 0)
            return;
        np_commit_load_sync();
        if (!g_np.load_ready_replied) {
            g_np.load_ready_replied = 1;
            printf("psxrecomp: netplay load slot=%u — ready acked, waiting lockstep…\n",
                   (unsigned)slot);
            fflush(stdout);
        }
        return;
    }

    if (size == 0) {
        /* SAVE coordinate local write (admit is not stalled for size==0). */
        if (!g_np.local_save_staged) {
            if (savestate_request_save_protocol((int)slot)) {
                g_np.local_save_staged = 1;
                printf("psxrecomp: netplay guest save slot=%u — writing sandbox…\n",
                       (unsigned)slot);
                fflush(stdout);
            } else {
                (void)rnet_session_state_probe_reply(g_np.session, 0);
                return;
            }
        }
        if (savestate_pending()) return;
        if (!savestate_slot_exists((int)slot)) return;
        g_np.local_save_staged = 0;
        (void)rnet_session_state_probe_reply(g_np.session, 1);
        printf("psxrecomp: netplay guest save slot=%u — local write done\n", (unsigned)slot);
        fflush(stdout);
        return;
    }

    if (op == RNET_STATE_OP_SRAM) {
        uint32_t local_sz = 0, local_crc = 0;
        match = np_mc_blob_crc(&local_sz, &local_crc) && local_sz == size && local_crc == crc;
        (void)rnet_session_state_probe_reply(g_np.session, match);
        if (match) g_np.mc_sync_done = 1;
        return;
    }

    {
        uint32_t local_sz = 0, local_crc = 0;
        match = np_slot_crc((int)slot, &local_sz, &local_crc) && local_sz == size &&
                local_crc == crc;
        (void)rnet_session_state_probe_reply(g_np.session, match);
        if (match && op == RNET_STATE_OP_LOAD) {
            if (g_np.xfer != NP_XFER_LOAD_APPLYING && g_np.xfer != NP_XFER_LOAD_READY) {
                (void)savestate_request_load_protocol((int)slot);
                np_begin_load_apply((int)slot);
                printf("psxrecomp: netplay guest load slot=%u — hashes match, applying…\n",
                       (unsigned)slot);
                fflush(stdout);
            } else {
                /* Retransmit of hash probe — already staging/applying. */
            }
        }
    }
}

static void np_host_drive_xfer(void)
{
    int match = 0;
    uint32_t size = 0, crc = 0;
    uint8_t *buf = NULL;
    size_t n = 0;

    if (g_np.local_slot != 0 || !g_np.session) return;

    switch (g_np.xfer) {
    case NP_XFER_MC_PROBE:
        if (!rnet_session_state_probe_take_reply(g_np.session, &match))
            return;
        rnet_session_state_probe_finish(g_np.session);
        if (match) {
            g_np.mc_sync_done = 1;
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        {
            uint8_t *blob = (uint8_t *)malloc(NP_MC_BLOB_BYTES);
            size_t sz = 0;
            if (!blob || np_build_mc_blob(blob, NP_MC_BLOB_BYTES, &sz) != 0 ||
                rnet_session_state_begin(g_np.session, RNET_STATE_OP_SRAM, 0, blob, sz) != 0) {
                free(blob);
                g_np.mc_sync_done = 1;
                g_np.xfer = NP_XFER_NONE;
                return;
            }
            free(blob);
            g_np.xfer = NP_XFER_MC_SEND;
        }
        return;

    case NP_XFER_SAVE_COORD:
        if (savestate_pending()) return;
        if (!savestate_slot_exists(g_np.xfer_slot)) return;
        if (!rnet_session_state_probe_take_reply(g_np.session, &match))
            return;
        rnet_session_state_probe_finish(g_np.session);
        if (!match) {
            /* Guest failed to save — still ship host blob. */
        }
        if (!np_slot_crc(g_np.xfer_slot, &size, &crc) ||
            rnet_session_state_probe(g_np.session, RNET_STATE_OP_SAVE, (rnet_u8)g_np.xfer_slot, size,
                                     crc) != 0) {
            printf("psxrecomp: netplay save slot=%d — hash probe failed\n", g_np.xfer_slot);
            fflush(stdout);
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        printf("psxrecomp: netplay save slot=%d — hash probe (%u bytes)\n", g_np.xfer_slot,
               (unsigned)size);
        fflush(stdout);
        g_np.xfer = NP_XFER_SAVE_PROBE;
        return;

    case NP_XFER_SAVE_PROBE:
        if (!rnet_session_state_probe_take_reply(g_np.session, &match))
            return;
        rnet_session_state_probe_finish(g_np.session);
        if (match) {
            printf("psxrecomp: netplay save slot=%d — hashes match, skip transfer\n",
                   g_np.xfer_slot);
            fflush(stdout);
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        if (!savestate_read_slot(g_np.xfer_slot, &buf, &n) || !buf ||
            rnet_session_state_begin(g_np.session, RNET_STATE_OP_SAVE, (rnet_u8)g_np.xfer_slot, buf,
                                     n) != 0) {
            free(buf);
            printf("psxrecomp: netplay save slot=%d — transfer begin failed\n", g_np.xfer_slot);
            fflush(stdout);
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        printf("psxrecomp: netplay save slot=%d — transferring %zu bytes to guest\n",
               g_np.xfer_slot, n);
        fflush(stdout);
        free(buf);
        g_np.xfer = NP_XFER_SAVE_SEND;
        return;

    case NP_XFER_LOAD_PROBE:
        if (!rnet_session_state_probe_take_reply(g_np.session, &match))
            return;
        rnet_session_state_probe_finish(g_np.session);
        if (match) {
            (void)savestate_request_load_protocol(g_np.xfer_slot);
            np_begin_load_apply(g_np.xfer_slot);
            printf("psxrecomp: netplay load slot=%d — hashes match, applying…\n",
                   g_np.xfer_slot);
            fflush(stdout);
            return;
        }
        if (!savestate_read_slot(g_np.xfer_slot, &buf, &n) || !buf) {
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        (void)savestate_request_load_protocol(g_np.xfer_slot);
        g_np.load_applied_local = 0;
        g_np.load_sync_done = 0;
        if (rnet_session_state_begin(g_np.session, RNET_STATE_OP_LOAD, (rnet_u8)g_np.xfer_slot, buf,
                                     n) != 0) {
            free(buf);
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        free(buf);
        printf("psxrecomp: netplay load slot=%d — transferring %zu bytes\n", g_np.xfer_slot, n);
        fflush(stdout);
        g_np.xfer = NP_XFER_LOAD_SEND;
        return;

    case NP_XFER_MC_SEND:
    case NP_XFER_SAVE_SEND:
    case NP_XFER_LOAD_SEND:
        /* apply_ready runs first and clears take_ready (LOAD → LOAD_APPLYING). */
        if (rnet_session_state_take_ready(g_np.session, NULL, NULL, NULL, NULL)) {
            if (g_np.xfer == NP_XFER_MC_SEND)
                g_np.mc_sync_done = 1;
            rnet_session_state_finish(g_np.session, 0);
            if (g_np.xfer == NP_XFER_LOAD_SEND) {
                np_begin_load_apply(g_np.xfer_slot);
            } else {
                g_np.xfer = NP_XFER_NONE;
            }
        }
        return;

    case NP_XFER_LOAD_READY:
        if (!rnet_session_state_probe_take_reply(g_np.session, &match))
            return;
        /* Mutual ready: drop probe stall first, then resync+prime. Stay in
         * LOAD_READY until try_admit (do not drop the app barrier early). */
        rnet_session_state_probe_finish(g_np.session);
        np_commit_load_sync();
        g_np.load_ready_replied = 1;
        printf("psxrecomp: netplay load slot=%d — mutual ready, waiting lockstep…\n",
               g_np.xfer_slot);
        fflush(stdout);
        return;

    default:
        return;
    }
}

static void np_prime_after_hard_resync(void)
{
    uint8_t bytes[PSX_NETPLAY_PAD_BYTES];
    /* Released digital + centered sticks — delay tip only; real pads resume after. */
    bytes[0] = 0xFFu;
    bytes[1] = 0xFFu;
    bytes[2] = bytes[3] = bytes[4] = bytes[5] = 0x80u;
    bytes[6] = 1u;
    bytes[7] = 1u;
    rnet_session_prime_delay_inputs(g_np.session, bytes, (rnet_u16)PSX_NETPLAY_PAD_BYTES);
}

/* Stage restore. Keep INPUT flowing so try_admit can still run guest cycles
 * for savestate_poll — suppress starts at enter_load_ready / hard_resync. */
static void np_begin_load_apply(int slot)
{
    g_np.xfer = NP_XFER_LOAD_APPLYING;
    g_np.load_applied_local = 0;
    g_np.load_sync_done = 0;
    g_np.xfer_slot = slot;
}

/* Once per load, at mutual ready (guest READY ACK / host take_reply). */
static void np_commit_load_sync(void)
{
    if (g_np.load_sync_done || !g_np.session)
        return;
    rnet_session_hard_resync(g_np.session);
    np_prime_after_hard_resync(); /* clears suppress + emits fresh tip */
    g_np.load_sync_done = 1;
    g_np.needs_advance = 0;
    g_np.latched_for_tick = 0;
    g_np.staged_valid = 0;
}

static void np_enter_load_ready(int slot)
{
    /* Do not hard_resync/prime here — the later-applying peer would clear the
     * earlier peer's tip and stall resume. Sync runs at mutual ready. */
    g_np.load_applied_local = 1;
    g_np.load_ready_replied = 0;
    g_np.load_sync_done = 0;
    g_np.needs_advance = 0;
    g_np.latched_for_tick = 0;
    g_np.staged_valid = 0;
    g_np.xfer = NP_XFER_LOAD_READY;
    g_np.xfer_slot = slot;
    /* Stop pre-resync tips until hard_resync + prime (avoids tick%128 clobber). */
    if (g_np.session)
        rnet_session_set_input_send_suppress(g_np.session, 1);
}

/* After both peers stage a load: run until restore completes, then rendezvous.
 * hard_resync+prime happens once at mutual ready (not at apply). */
static void np_drive_load_barrier(void)
{
    if (g_np.xfer != NP_XFER_LOAD_APPLYING)
        return;
    if (savestate_pending())
        return;
    if (!g_np.load_applied_local && !savestate_take_load_completed())
        return;

    np_enter_load_ready(g_np.xfer_slot);

    if (g_np.local_slot == 0) {
        if (rnet_session_state_probe(g_np.session, RNET_STATE_OP_LOAD, (rnet_u8)g_np.xfer_slot, 0,
                                     NP_LOAD_READY_CRC) != 0) {
            printf("psxrecomp: netplay load slot=%d — ready probe failed\n", g_np.xfer_slot);
            fflush(stdout);
            g_np.xfer = NP_XFER_NONE;
            g_np.load_applied_local = 0;
            if (g_np.session)
                rnet_session_set_input_send_suppress(g_np.session, 0);
            return;
        }
        printf("psxrecomp: netplay load slot=%d — applied, waiting for guest…\n",
               g_np.xfer_slot);
        fflush(stdout);
    } else {
        printf("psxrecomp: netplay guest load slot=%d — applied, waiting for host…\n",
               g_np.xfer_slot);
        fflush(stdout);
    }
}

static void np_maybe_start_mc_sync(void)
{
    uint32_t size = 0, crc = 0;
    if (g_np.local_slot != 0 || g_np.mc_sync_sent || g_np.mc_sync_done)
        return;
    if (!rnet_session_is_running(g_np.session)) return;
    if (np_xfer_busy()) return;
    if (!np_mc_blob_crc(&size, &crc)) {
        g_np.mc_sync_done = 1;
        return;
    }
    if (rnet_session_state_probe(g_np.session, RNET_STATE_OP_SRAM, 0, size, crc) != 0) {
        g_np.mc_sync_done = 1;
        return;
    }
    g_np.mc_sync_sent = 1;
    g_np.xfer = NP_XFER_MC_PROBE;
}

static void encode_pad(const PsxNetPad *pad, RNetInputSample *out, rnet_u32 tick)
{
    PsxNetPad n = *pad;
    psx_netplay_normalize_pad(&n);
    memset(out, 0, sizeof(*out));
    out->tick = tick;
    out->size = PSX_NETPLAY_PAD_BYTES;
    out->bytes[0] = (rnet_u8)(n.buttons & 0xFFu);
    out->bytes[1] = (rnet_u8)((n.buttons >> 8) & 0xFFu);
    out->bytes[2] = n.lx;
    out->bytes[3] = n.ly;
    out->bytes[4] = n.rx;
    out->bytes[5] = n.ry;
    out->bytes[6] = n.analog ? 1u : 0u;
    out->bytes[7] = 1u;
    out->valid = 1;
}

static void decode_pad(const RNetInputSample *in, PsxNetPad *pad)
{
    memset(pad, 0, sizeof(*pad));
    pad->buttons = 0xFFFFu;
    pad->lx = pad->ly = pad->rx = pad->ry = 0x80u;
    pad->analog = 1;
    pad->connected = 1;
    if (!in || !in->valid || in->size < PSX_NETPLAY_PAD_BYTES) return;
    pad->buttons = (uint16_t)in->bytes[0] | ((uint16_t)in->bytes[1] << 8);
    pad->lx = in->bytes[2];
    pad->ly = in->bytes[3];
    pad->rx = in->bytes[4];
    pad->ry = in->bytes[5];
    pad->analog = in->bytes[6] ? 1u : 0u;
    pad->connected = 1;
    psx_netplay_normalize_pad(pad);
}

static void apply_pad_slot(int slot, const PsxNetPad *pad)
{
    if (slot < 0 || slot > 1 || !pad) return;
    sio_set_pad_connected(slot, 1);
    sio_set_pad_config_capable(slot, 1);
    sio_set_pad_state_slot(slot, pad->buttons);
    sio_set_pad_sticks(slot, pad->lx, pad->ly, pad->rx, pad->ry);
    sio_request_pad_type(slot, pad->analog ? 1 : 0);
}

static void host_sample_local(rnet_u32 tick, RNetInputSample *out, void *ctx)
{
    NetplayState *st = (NetplayState *)ctx;
    PsxNetPad pad;
    memset(&pad, 0, sizeof(pad));
    pad.buttons = 0xFFFFu;
    pad.lx = pad.ly = pad.rx = pad.ry = 0x80u;
    pad.analog = 1;
    pad.connected = 1;
    if (st->staged_valid) pad = st->staged;
    pad.connected = 1;
    encode_pad(&pad, out, tick);
}

static void host_publish(rnet_u32 tick, const RNetInputSample *by_slot, int slots, void *ctx)
{
    int i;
    (void)tick;
    (void)ctx;
    if (!by_slot || slots <= 0) return;
    force_session_pads_connected(slots);
    for (i = 0; i < slots && i < 2; ++i) {
        PsxNetPad pad;
        decode_pad(&by_slot[i], &pad);
        apply_pad_slot(i, &pad);
    }
}

int psx_netplay_active(void)
{
    return g_np.active && g_np.session != NULL;
}

int psx_netplay_is_running(void)
{
    return psx_netplay_active() && rnet_session_is_running(g_np.session);
}

int psx_netplay_local_slot(void)
{
    return psx_netplay_active() ? g_np.local_slot : -1;
}

int psx_netplay_input_player(void)
{
    return psx_netplay_active() ? g_np.input_player : 0;
}

uint32_t psx_netplay_sim_tick(void)
{
    if (!psx_netplay_active()) return 0;
    return rnet_session_sim_tick(g_np.session);
}

void psx_netplay_stage_local(const PsxNetPad *pad)
{
    if (!pad) {
        g_np.staged_valid = 0;
        return;
    }
    /* Once running, freeze the first sample for the current sim tick so
     * re-admits / barrier retries cannot change the INPUT_CONFIRM hash. */
    if (psx_netplay_active() && rnet_session_is_running(g_np.session)) {
        uint32_t t = rnet_session_sim_tick(g_np.session);
        if (g_np.latched_for_tick && g_np.latched_sim_tick == t)
            return;
        g_np.staged = *pad;
        psx_netplay_normalize_pad(&g_np.staged);
        g_np.staged_valid = 1;
        g_np.latched_for_tick = 1;
        g_np.latched_sim_tick = t;
        return;
    }
    /* Linking: keep refreshing released/local pads until START. */
    g_np.staged = *pad;
    psx_netplay_normalize_pad(&g_np.staged);
    g_np.staged_valid = 1;
}

int psx_netplay_needs_local_sample(void)
{
    if (!psx_netplay_active()) return 0;
    if (!rnet_session_is_running(g_np.session)) return 1; /* linking */
    {
        uint32_t t = rnet_session_sim_tick(g_np.session);
        return !(g_np.latched_for_tick && g_np.latched_sim_tick == t);
    }
}

int psx_netplay_input_desync(uint32_t *tick, uint32_t *local_hash, uint32_t *remote_hash)
{
    if (!psx_netplay_active()) return 0;
    return rnet_session_input_desync(g_np.session, tick, local_hash, remote_hash);
}

int psx_netplay_peer_disconnected(uint32_t timeout_ms)
{
    if (!psx_netplay_active()) return 0;
    if (timeout_ms == 0) timeout_ms = 1500u;
    return rnet_session_peer_disconnected(g_np.session, (rnet_u64)timeout_ms);
}

#if defined(__linux__)
static int peer_is_loopback(const char *peer_hostport)
{
    if (!peer_hostport || !peer_hostport[0]) return 0;
    if (strncmp(peer_hostport, "127.", 4) == 0) return 1;
    if (strncmp(peer_hostport, "localhost:", 10) == 0) return 1;
    if (strncmp(peer_hostport, "::1:", 4) == 0) return 1;
    if (strcmp(peer_hostport, "::1") == 0) return 1;
    return 0;
}

/* Same-machine MotK FMV: lockstep syncs both peers' MDEC peaks; pinning each
 * slot to a disjoint CPU half cut headless FMV ~40 → ~45 in A/B. */
static void pin_localhost_peer_cpus(int local_slot)
{
    long ncpu;
    cpu_set_t set;
    int i, lo, hi;

    ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 4) return;
    CPU_ZERO(&set);
    if (local_slot <= 0) {
        lo = 0;
        hi = (int)(ncpu / 2);
    } else {
        lo = (int)(ncpu / 2);
        hi = (int)ncpu;
    }
    for (i = lo; i < hi; i++)
        CPU_SET(i, &set);
    (void)sched_setaffinity(0, sizeof(set), &set);
}
#endif

int psx_netplay_start(const PsxNetplayConfig *cfg)
{
    RNetConfig rcfg;
    RNetHostVTable host;
    int in_player;

    if (!cfg || !cfg->enabled) return -1;
    if (g_np.session) psx_netplay_shutdown();

    rnet_config_init_defaults(&rcfg);
    rcfg.slot_count = 2;
    rcfg.local_slot = (rnet_u8)(cfg->local_slot < 0 ? 0 : (cfg->local_slot > 1 ? 1 : cfg->local_slot));
    rcfg.input_delay = (rnet_u8)(cfg->input_delay < 0 ? 0 : (cfg->input_delay > 16 ? 16 : cfg->input_delay));
    rcfg.session_id = cfg->session_id ? cfg->session_id : 1u;

    /* Host resolves auto (-1) before start; accept only 0/1 here. */
    in_player = (cfg->input_player == 1) ? 1 : 0;

    memset(&host, 0, sizeof(host));
    host.sample_local = host_sample_local;
    host.publish = host_publish;
    host.ctx = &g_np;

    g_np.session = rnet_session_create(&rcfg, &host);
    if (!g_np.session) return -2;
    if (rnet_session_start_lan(g_np.session, cfg->bind_hostport, cfg->peer_hostport) != 0) {
        rnet_session_destroy(g_np.session);
        g_np.session = NULL;
        return -3;
    }
    g_np.active = 1;
    g_np.slot_count = (int)rcfg.slot_count;
    g_np.local_slot = (int)rcfg.local_slot;
    g_np.input_player = in_player;
    g_np.staged_valid = 0;
    g_np.needs_advance = 0;
    g_np.latched_for_tick = 0;
    g_np.latched_sim_tick = 0;
    g_np.xfer = NP_XFER_NONE;
    g_np.xfer_slot = 0;
    g_np.mc_sync_done = 0;
    g_np.mc_sync_sent = 0;
    g_np.local_save_staged = 0;
    g_np.load_applied_local = 0;
    g_np.guest_sandbox = 0;

#if defined(__linux__)
    if (peer_is_loopback(cfg->peer_hostport))
        pin_localhost_peer_cpus(g_np.local_slot);
#endif

    psx_netplay_release_pads();
    return 0;
}

void psx_netplay_bind_guest_saves(void)
{
    if (!psx_netplay_active() || g_np.local_slot == 0 || g_np.guest_sandbox)
        return;
    np_enter_guest_sandbox();
}

void psx_netplay_shutdown(void)
{
    if (g_np.session) {
        (void)rnet_session_send_bye(g_np.session);
        rnet_session_destroy(g_np.session);
        g_np.session = NULL;
    }
    np_leave_guest_sandbox();
    memset(&g_np, 0, sizeof(g_np));
}

int psx_netplay_is_host(void)
{
    return psx_netplay_active() && g_np.local_slot == 0;
}

int psx_netplay_request_save(int slot)
{
    if (!psx_netplay_active() || !rnet_session_is_running(g_np.session))
        return 0;
    if (g_np.local_slot != 0)
        return 1; /* guest: host-only; ignore */
    if (np_xfer_busy() || !g_np.mc_sync_done)
        return 1;
    if (slot < 0) slot = 0;
    if (slot >= SAVESTATE_SLOTS) slot = SAVESTATE_SLOTS - 1;

    if (!savestate_request_save_protocol(slot))
        return 1;
    /* Coord probe (size=0) does not stall admit — both peers must keep
     * running until savestate_poll writes the .pst, then hash-probe stalls. */
    if (rnet_session_state_probe(g_np.session, RNET_STATE_OP_SAVE, (rnet_u8)slot, 0, 0) != 0)
        return 1;
    g_np.xfer = NP_XFER_SAVE_COORD;
    g_np.xfer_slot = slot;
    printf("psxrecomp: netplay save slot=%d — coordinating local writes…\n", slot);
    fflush(stdout);
    return 1;
}

int psx_netplay_request_load(int slot)
{
    uint32_t size = 0, crc = 0;
    if (!psx_netplay_active() || !rnet_session_is_running(g_np.session))
        return 0;
    if (g_np.local_slot != 0)
        return 1;
    if (np_xfer_busy() || !g_np.mc_sync_done)
        return 1;
    if (slot < 0) slot = 0;
    if (slot >= SAVESTATE_SLOTS) slot = SAVESTATE_SLOTS - 1;
    if (!np_slot_crc(slot, &size, &crc))
        return 1;
    if (rnet_session_state_probe(g_np.session, RNET_STATE_OP_LOAD, (rnet_u8)slot, size, crc) != 0)
        return 1;
    g_np.xfer = NP_XFER_LOAD_PROBE;
    g_np.xfer_slot = slot;
    g_np.load_applied_local = 0;
    printf("psxrecomp: netplay load slot=%d — hash probe (%u bytes)\n", slot, (unsigned)size);
    fflush(stdout);
    return 1;
}

int psx_netplay_in_load_barrier(void)
{
    if (!psx_netplay_active())
        return 0;
    return (g_np.xfer == NP_XFER_LOAD_APPLYING || g_np.xfer == NP_XFER_LOAD_READY) ? 1 : 0;
}

int psx_netplay_poll_admit(void)
{
    rnet_u32 sim;
    if (!psx_netplay_active()) return 1;

    rnet_session_pump(g_np.session);
    np_guest_handle_probe();
    np_apply_ready_state();
    np_drive_load_barrier();
    np_host_drive_xfer();

    if (!rnet_session_is_running(g_np.session)) {
        psx_netplay_release_pads();
        return 0;
    }

    np_maybe_start_mc_sync();
    /* Both peers stall until initial memcard hash-agree / transfer finishes. */
    if (!g_np.mc_sync_done)
        return 0;

    /* Post-load: allow admit only while savestate_poll still needs guest
     * cycles. After restore (or during ready rendezvous) freeze the sim clock
     * so peers cannot drift before hard_resync + prime. */
    if (g_np.xfer == NP_XFER_LOAD_APPLYING && !savestate_pending())
        return 0;

    /* Both peers: after mutual ready + sync, stay in LOAD_READY until try_admit
     * succeeds (fresh tip exchange + INPUT_CONFIRM). Dropping the barrier early
     * on the host let it spin on confirm with FPS/present already "live". */
    if (g_np.xfer == NP_XFER_LOAD_READY) {
        if (g_np.load_sync_done && g_np.load_ready_replied && !g_np.needs_advance) {
            sim = rnet_session_sim_tick(g_np.session);
            if (rnet_session_try_admit(g_np.session, sim)) {
                g_np.xfer = NP_XFER_NONE;
                g_np.load_applied_local = 0;
                g_np.load_ready_replied = 0;
                g_np.load_sync_done = 0;
                g_np.needs_advance = 1;
                printf("psxrecomp: netplay load slot=%d — peer ready, resuming lockstep\n",
                       g_np.xfer_slot);
                fflush(stdout);
                return 1;
            }
            force_session_pads_connected(g_np.slot_count);
        }
        return 0;
    }

    /* Already published this tick and waiting for finish_frame — do not
     * re-admit / re-sample (would desync the delay rings). */
    if (g_np.needs_advance) return 1;

    sim = rnet_session_sim_tick(g_np.session);
    if (rnet_session_try_admit(g_np.session, sim)) {
        g_np.needs_advance = 1;
        return 1;
    }
    force_session_pads_connected(g_np.slot_count);
    return 0;
}

void psx_netplay_finish_frame(void)
{
    if (!psx_netplay_active()) return;
    if (!g_np.needs_advance) return;
    rnet_session_advance(g_np.session);
    g_np.needs_advance = 0;
    g_np.latched_for_tick = 0;
}

void psx_netplay_wait_recv(int timeout_ms)
{
    if (!psx_netplay_active()) return;
    (void)rnet_session_wait_recv(g_np.session, timeout_ms);
}

#endif /* PSX_HAS_RECOMP_NET */
