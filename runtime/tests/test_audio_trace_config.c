/* Focused two-mode contract test for audio_trace.c.
 * Build once normally and once with PSX_NO_DEBUG_TOOLS=1. */
#include "audio_trace.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

int main(void)
{
#if AUDIO_TRACE_ENABLED
    static const int16_t samples[] = { 1, 0, -32768, 300 };
    AudioTraceStats stats;
    AudioTraceEvent event;

    audio_trace_init();
    audio_trace_set_tap_rate(AUDIO_TAP_HOST, 48000u);
    audio_trace_note_frame(17u);
    audio_trace_pcm(AUDIO_TAP_SPU_OUT, samples, 2);
    audio_trace_event(AUDIO_EV_RENDER, 2u, 64u);
    audio_trace_get_stats(&stats);

    if (stats.tap_frames[AUDIO_TAP_SPU_OUT] != 2u)
        return fail("debug PCM frame count");
    if (stats.tap_nonzero[AUDIO_TAP_SPU_OUT] != 2u ||
        stats.tap_audible[AUDIO_TAP_SPU_OUT] != 1u ||
        stats.tap_peak[AUDIO_TAP_SPU_OUT] != 32768)
        return fail("debug PCM statistics");
    if (stats.pump_calls != 1u || stats.queue_hiwater != 64u ||
        stats.queue_lowater != 64u || stats.events_total != 1u)
        return fail("debug event statistics");
    if (audio_trace_tap_rate(AUDIO_TAP_HOST) != 48000u)
        return fail("debug tap rate");
    if (audio_trace_events_get(&event, 1u) != 1u ||
        event.kind != AUDIO_EV_RENDER || event.frame != 17u ||
        event.sample_idx != 2u || event.a != 2u || event.b != 64u)
        return fail("debug event contents");
#else
    int evaluated = 0;
    AudioTraceStats stats;
    AudioTraceStats zero;
    AudioTraceEvent event;
    void (*pcm_fn)(int, const int16_t *, int) = audio_trace_pcm;
    void (*event_fn)(uint16_t, uint32_t, uint32_t) = audio_trace_event;
    void (*frame_fn)(uint32_t) = audio_trace_note_frame;

    /* Function-like producer macros must discard argument evaluation. */
    audio_trace_pcm(++evaluated, (const int16_t *)(uintptr_t)++evaluated,
                    ++evaluated);
    audio_trace_event((uint16_t)++evaluated, (uint32_t)++evaluated,
                      (uint32_t)++evaluated);
    audio_trace_note_frame((uint32_t)++evaluated);
    if (evaluated != 0)
        return fail("lean producer evaluated an argument");

    /* Taking a producer's address bypasses its function-like macro and checks
     * that the public symbols remain linkable and inert. */
    pcm_fn(0, NULL, 0);
    event_fn(0, 0, 0);
    frame_fn(0);
    memset(&stats, 0xA5, sizeof(stats));
    memset(&zero, 0, sizeof(zero));
    audio_trace_get_stats(&stats);
    if (memcmp(&stats, &zero, sizeof(stats)) != 0)
        return fail("lean stats are not zero");
    if (audio_trace_tap_total(0) != 0 || audio_trace_events_total() != 0 ||
        audio_trace_events_get(&event, 1u) != 0)
        return fail("lean totals are not empty");
    if (audio_trace_tap_rate(0) != 44100u)
        return fail("lean default tap rate");
    if (audio_trace_dump_wav(0, "unused.wav", -1, 0) != -1)
        return fail("lean WAV dump did not fail cleanly");
#endif

    puts("PASS: audio trace configuration contract");
    return 0;
}
