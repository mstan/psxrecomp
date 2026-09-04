/* Feature macros before any system headers (strict C11 / -std=c11). */
#if !defined(_WIN32)
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#if !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif
#endif

#include "host_time.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static LARGE_INTEGER s_qpc_freq;
static int s_qpc_freq_init;
static HANDLE s_sleep_timer;
static int s_sleep_timer_init;

static void sleep_timer_ensure(void)
{
    if (s_sleep_timer_init)
        return;
    s_sleep_timer_init = 1;
    s_sleep_timer =
        CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                               TIMER_ALL_ACCESS);
    if (!s_sleep_timer)
        s_sleep_timer = CreateWaitableTimerW(NULL, FALSE, NULL);
}

uint64_t psx_host_mono_ms(void)
{
    LARGE_INTEGER counter;

    if (!s_qpc_freq_init) {
        if (!QueryPerformanceFrequency(&s_qpc_freq))
            s_qpc_freq.QuadPart = 0;
        s_qpc_freq_init = 1;
    }
    if (s_qpc_freq.QuadPart == 0)
        return (uint64_t)GetTickCount64();
    if (!QueryPerformanceCounter(&counter))
        return (uint64_t)GetTickCount64();
    return (uint64_t)((counter.QuadPart * 1000ULL) / (uint64_t)s_qpc_freq.QuadPart);
}

void psx_host_sleep_micros(unsigned usec)
{
    LARGE_INTEGER due;
    LONGLONG hundred_ns;

    if (usec == 0u)
        return;

    sleep_timer_ensure();
    if (s_sleep_timer) {
        hundred_ns = -((LONGLONG)usec * 10LL);
        if (hundred_ns >= 0)
            hundred_ns = -1LL;
        due.QuadPart = hundred_ns;
        if (SetWaitableTimer(s_sleep_timer, &due, 0, NULL, NULL, FALSE)) {
            (void)WaitForSingleObject(s_sleep_timer, INFINITE);
            return;
        }
    }
    if (usec < 1000u) {
        Sleep(1u);
        return;
    }
    Sleep((DWORD)(usec / 1000u));
}

#else /* !_WIN32 */

#include <errno.h>
#include <time.h>
#include <unistd.h>

uint64_t psx_host_mono_ms(void)
{
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000ull +
               (uint64_t)ts.tv_nsec / 1000000ull;
#endif
    return (uint64_t)time(NULL) * 1000ull;
}

void psx_host_sleep_micros(unsigned usec)
{
    struct timespec req;
    struct timespec rem;

    if (usec == 0u)
        return;
    req.tv_sec = (time_t)(usec / 1000000u);
    req.tv_nsec = (long)(usec % 1000000u) * 1000L;
    while (nanosleep(&req, &rem) != 0) {
        if (errno != EINTR)
            break;
        req = rem;
    }
}

#endif /* _WIN32 */

void psx_host_sleep_ms(unsigned ms)
{
    if (ms == 0u)
        return;
    if (ms > 1000000u)
        ms = 1000000u;
    psx_host_sleep_micros(ms * 1000u);
}
