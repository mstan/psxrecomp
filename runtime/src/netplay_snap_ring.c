/* MotK snap save/load over retcomm-rbengine ring + boot_state serialize. */

#if !defined(PSX_HAS_RECOMP_NET)
/* Empty TU when netplay/rbengine is not linked. */
#else

#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "netplay_snap_ring.h"
#include "boot_state.h"

#include <stdio.h>
#if defined(_WIN32)
#  include <windows.h>
#else
#  include <time.h>
#endif

static double snap_mono_ms(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
#endif
}

int netplay_snap_ring_save(NetplaySnapRing *r, uint32_t tick,
                           const CPUState *cpu, uint32_t bios_checksum,
                           uint32_t entry_pc)
{
    uint8_t *data = NULL;
    size_t len = 0;
    double t0, dt;
    static uint32_t s_tele_n;
    static double s_tele_ms_sum;
    static uint64_t s_tele_bytes_sum;
    static uint64_t s_tele_dirty_sum;
    static uint32_t s_tele_incr_n;
    if (!r || !cpu) return 0;
    t0 = snap_mono_ms();
    if (!boot_state_save_buffer_raw(cpu, bios_checksum, entry_pc, &data, &len))
        return 0;
    dt = snap_mono_ms() - t0;
    s_tele_n++;
    s_tele_ms_sum += dt;
    s_tele_bytes_sum += (uint64_t)len;
    s_tele_dirty_sum += (uint64_t)boot_state_last_vram_dirty_rows();
    if (boot_state_last_vram_incremental())
        s_tele_incr_n++;
    if ((s_tele_n & 63u) == 0u) {
        fprintf(stderr,
                "psxrecomp: rb snap tele n=%u last=%.2fms avg=%.2fms "
                "bytes=%zu avg_bytes=%.0f dirty_rows=%u incr=%d "
                "(avg_dirty=%.1f incr%%=%.0f)\n",
                (unsigned)s_tele_n, dt, s_tele_ms_sum / (double)s_tele_n,
                len, (double)s_tele_bytes_sum / (double)s_tele_n,
                (unsigned)boot_state_last_vram_dirty_rows(),
                boot_state_last_vram_incremental(),
                (double)s_tele_dirty_sum / (double)s_tele_n,
                100.0 * (double)s_tele_incr_n / (double)s_tele_n);
        fflush(stderr);
    }
    if (!netplay_snap_ring_store(r, tick, data, len))
        return 0;
    return 1;
}

int netplay_snap_ring_load(NetplaySnapRing *r, uint32_t tick, CPUState *cpu,
                           uint32_t bios_checksum, uint32_t entry_pc)
{
    size_t size = 0;
    const uint8_t *data = netplay_snap_ring_peek(r, tick, &size);
    if (!data || !size || !cpu) return 0;
    return boot_state_load_buffer(data, size, bios_checksum, entry_pc, cpu);
}

#endif /* PSX_HAS_RECOMP_NET */
