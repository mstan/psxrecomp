#pragma once
/* ---------------------------------------------------------------------------
 * diag_log.h — Diagnostic logging macros for PSXRecomp runner.
 *
 * All macros embed the PS1 frame number as a timestamp (f%u).
 * None call fflush() — stdout is buffered; the main loop flushes at frame end.
 *
 * Pick the right level:
 *
 *   LOG_ONCE(tag, fmt, ...)
 *     Fires exactly once per call-site. Good for init/boot events.
 *
 *   LOG_FIRST_N(n, tag, fmt, ...)
 *     Fires first N times per call-site. Good for "show me the first few calls".
 *
 *   LOG_PER_SEC(tag, fmt, ...)
 *     Fires at most once per wall-clock second. Good for hot paths where you
 *     want visibility without overwhelming the log.
 *
 *   LOG_ON_CHANGE(expr, tag, fmt, ...)
 *     Fires whenever the scalar expression changes value. This is the default
 *     choice for state variables, flags, and addresses — log the transition,
 *     not the steady state.
 *
 * RULE: Any LOG_* call added for a specific investigation MUST be commented
 * out or removed before the session ends. No exceptions.
 * --------------------------------------------------------------------------- */

#include <stdio.h>
#include <time.h>

extern unsigned int g_ps1_frame;

/* Fire exactly once per call-site. */
#define LOG_ONCE(tag, fmt, ...)                                                \
    do {                                                                       \
        static int _done = 0;                                                  \
        if (!_done) { _done = 1;                                               \
            printf("[" tag "] f%u " fmt "\n", g_ps1_frame, ##__VA_ARGS__); }  \
    } while (0)

/* Fire first N times per call-site. */
#define LOG_FIRST_N(n, tag, fmt, ...)                                          \
    do {                                                                       \
        static int _cnt = 0;                                                   \
        if (++_cnt <= (n))                                                     \
            printf("[" tag "] f%u #%d " fmt "\n",                             \
                   g_ps1_frame, _cnt, ##__VA_ARGS__);                          \
    } while (0)

/* Fire at most once per wall-clock second per call-site. */
#define LOG_PER_SEC(tag, fmt, ...)                                             \
    do {                                                                       \
        static time_t _last = 0;                                               \
        time_t _now = time(NULL);                                              \
        if (_now != _last) { _last = _now;                                     \
            printf("[" tag "] f%u " fmt "\n", g_ps1_frame, ##__VA_ARGS__); }  \
    } while (0)

/* Fire when a scalar expression changes value. Use simple lvalues or casts.
 * __typeof__ is a GCC/Clang extension — works in both C and C++ with GCC. */
#define LOG_ON_CHANGE(expr, tag, fmt, ...)                                     \
    do {                                                                       \
        typedef __typeof__(expr) _T_;                                          \
        static _T_ _prev_ = (_T_)0;                                           \
        static int  _init_ = 0;                                                \
        _T_ _cur_ = (expr);                                                    \
        if (!_init_ || _cur_ != _prev_) {                                      \
            _init_ = 1; _prev_ = _cur_;                                        \
            printf("[" tag "] f%u " fmt "\n", g_ps1_frame, ##__VA_ARGS__);    \
        }                                                                      \
    } while (0)
