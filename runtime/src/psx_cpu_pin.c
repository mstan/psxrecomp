/*
 * psx_cpu_pin.c -- PSX-Link pair CPU placement (see psx_cpu_pin.h).
 *
 * Physical-core picking: logical CPUs are grouped by
 * (physical_package_id, core_id) from sysfs, each group scored by its best
 * cpuinfo_max_freq so heterogeneous parts (P/E cores) hand the two emulation
 * threads the two fastest DISTINCT physical cores instead of two E-cores or
 * two siblings of one P-core. Only CPUs already allowed by the process mask
 * are eligible, so an outer taskset still owns the budget.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1   /* CPU_SET/sched_getaffinity/pthread_setaffinity_np */
#endif

#include "psx_cpu_pin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)

#include <pthread.h>
#include <sched.h>

#define PIN_MAX_CPUS  1024
#define PIN_MAX_CORES 512

typedef struct {
    int pkg;
    int core;
    int best_cpu;       /* fastest eligible logical CPU of this core */
    long best_khz;
} PinCore;

static cpu_set_t g_prepin_mask;
static int       g_prepin_mask_valid;

static long pin_read_long(const char *path, long fallback) {
    FILE *f = fopen(path, "re");
    long v = fallback;
    if (!f) return fallback;
    if (fscanf(f, "%ld", &v) != 1) v = fallback;
    fclose(f);
    return v;
}

/* Returns the number of distinct physical cores found, filling `cores`
 * sorted fastest-first (freq desc, then pkg/core asc for determinism). */
static int pin_enumerate_cores(const cpu_set_t *allowed, PinCore *cores) {
    int ncores = 0;
    char path[128];
    for (int cpu = 0; cpu < PIN_MAX_CPUS; cpu++) {
        long core_id, pkg_id, khz;
        if (!CPU_ISSET(cpu, allowed)) continue;
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu);
        core_id = pin_read_long(path, -1);
        if (core_id < 0) continue;      /* no sysfs topology: cpu offline? */
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/topology/physical_package_id",
                 cpu);
        pkg_id = pin_read_long(path, 0);
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq",
                 cpu);
        khz = pin_read_long(path, 0);
        int found = -1;
        for (int i = 0; i < ncores; i++) {
            if (cores[i].pkg == (int)pkg_id && cores[i].core == (int)core_id) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            if (ncores >= PIN_MAX_CORES) continue;
            found = ncores++;
            cores[found].pkg = (int)pkg_id;
            cores[found].core = (int)core_id;
            cores[found].best_cpu = cpu;
            cores[found].best_khz = khz;
        } else if (khz > cores[found].best_khz) {
            cores[found].best_cpu = cpu;
            cores[found].best_khz = khz;
        }
    }
    /* Insertion sort: freq desc, pkg asc, core asc. Tiny N. */
    for (int i = 1; i < ncores; i++) {
        PinCore key = cores[i];
        int j = i - 1;
        while (j >= 0 &&
               (cores[j].best_khz < key.best_khz ||
                (cores[j].best_khz == key.best_khz &&
                 (cores[j].pkg > key.pkg ||
                  (cores[j].pkg == key.pkg && cores[j].core > key.core))))) {
            cores[j + 1] = cores[j];
            j--;
        }
        cores[j + 1] = key;
    }
    return ncores;
}

/* Latch the thread's affinity before any pin narrows it — later callers
 * (a second role in a test, the unpin of a helper thread) must see the
 * original budget, not the single pinned core. */
static void pin_latch_prepin_mask(void) {
    if (g_prepin_mask_valid) return;
    CPU_ZERO(&g_prepin_mask);
    if (sched_getaffinity(0, sizeof(g_prepin_mask), &g_prepin_mask) == 0)
        g_prepin_mask_valid = 1;
}

static void pin_apply(int role, int cpu) {
    cpu_set_t set;
    pin_latch_prepin_mask();
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
        fprintf(stdout, "psxrecomp: link pin — role %s: setaffinity(cpu%d) "
                        "failed, running unpinned\n",
                role ? "follower" : "driver", cpu);
        fflush(stdout);
        return;
    }
    fprintf(stdout, "psxrecomp: link pin — role %s emulation thread → cpu%d\n",
            role ? "follower" : "driver", cpu);
    fflush(stdout);
}

void psx_cpu_pin_link_role(int role) {
    static PinCore cores[PIN_MAX_CORES];
    cpu_set_t allowed;
    const char *env = getenv("PSX_LINK_PIN");
    int ncores;

    if (role != 0 && role != 1) return;
    if (env && env[0]) {
        if (env[0] == '0' && env[1] == '\0') {
            fprintf(stdout, "psxrecomp: link pin — disabled (PSX_LINK_PIN=0)\n");
            fflush(stdout);
            return;
        }
        /* Explicit "driver,follower" logical CPU ids. */
        {
            char *end = NULL;
            long a = strtol(env, &end, 0);
            if (end && *end == ',') {
                long b = strtol(end + 1, NULL, 0);
                pin_apply(role, (int)(role ? b : a));
                return;
            }
        }
        /* Unrecognized value: fall through to auto (loud enough via the
         * placement line itself). */
    }

    pin_latch_prepin_mask();
    if (!g_prepin_mask_valid) {
        fprintf(stdout, "psxrecomp: link pin — getaffinity failed, skipped\n");
        fflush(stdout);
        return;
    }
    allowed = g_prepin_mask;
    ncores = pin_enumerate_cores(&allowed, cores);
    /* Both sim threads at ~100% plus present/audio/net helpers: on fewer
     * than 3 physical cores a pin only removes scheduler freedom. */
    if (ncores < 3) {
        fprintf(stdout,
                "psxrecomp: link pin — only %d eligible physical core(s), "
                "skipped\n", ncores);
        fflush(stdout);
        return;
    }
    pin_apply(role, cores[role].best_cpu);
}

void psx_cpu_pin_unpin_self(void) {
    if (!g_prepin_mask_valid) return;
    (void)pthread_setaffinity_np(pthread_self(), sizeof(g_prepin_mask),
                                 &g_prepin_mask);
}

#else /* !__linux__ */

void psx_cpu_pin_link_role(int role) { (void)role; }
void psx_cpu_pin_unpin_self(void) {}

#endif
