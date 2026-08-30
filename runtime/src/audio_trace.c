/*
 * audio_trace.c - always-on audio observability rings. See audio_trace.h.
 *
 * Memory: 3 PCM taps x 2^22 stereo frames x 4 bytes = 48 MiB static, plus a
 * 2^19-entry event ring (16 MiB). Static BSS, zero cost until touched; the
 * recording hot path is a memcpy per audio block and is compiled into
 * Release/production builds too (always-on ring-buffer discipline — probes
 * query history, they never arm recording).
 */
#include "audio_trace.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* 2^22 frames @ 44100 ~= 95 s per tap. Power of two so wrap is a mask. */
#define PCM_RING_FRAMES (1u << 22)
#define PCM_RING_MASK   (PCM_RING_FRAMES - 1u)

#define EV_RING_CAP  (1u << 19)
#define EV_RING_MASK (EV_RING_CAP - 1u)

/* Audibility threshold shared with snesrecomp's dropped_audible metric:
 * |sample| > 256 ~= -42 dBFS. Below that, gaps/drops are inaudible and must
 * not trip regression counters (snesrecomp a1c659f). */
#define AUDIBLE_ABS 256

typedef struct {
    int16_t          pcm[PCM_RING_FRAMES * 2];
    _Atomic uint64_t head;      /* frames ever written; ring pos = head & MASK */
    uint64_t         nonzero;
    uint64_t         audible;
    int32_t          peak;
} PcmTap;

static PcmTap s_taps[AUDIO_TAP_COUNT];

/* Per-tap sample rate for WAV headers. SPU-side taps are natively 44100;
 * the host tap follows the device rate in bridge/pull mode. */
static uint32_t s_tap_rate[AUDIO_TAP_COUNT] = { 44100u, 44100u, 44100u, 44100u };

void audio_trace_set_tap_rate(int tap, uint32_t rate)
{
    if (tap < 0 || tap >= AUDIO_TAP_COUNT || rate == 0) return;
    s_tap_rate[tap] = rate;
}

uint32_t audio_trace_tap_rate(int tap)
{
    if (tap < 0 || tap >= AUDIO_TAP_COUNT) return 44100u;
    return s_tap_rate[tap];
}

static AudioTraceEvent  s_events[EV_RING_CAP];
static _Atomic uint64_t s_event_head;

static _Atomic uint32_t s_noted_frame;

/* Host pump counters (single writer: the pump/render thread). */
static uint64_t s_pump_calls;
static uint64_t s_pump_skips;
static uint64_t s_underruns;
static uint32_t s_queue_hiwater;
static uint32_t s_queue_lowater = 0xFFFFFFFFu;
static uint64_t s_mute_events;
static uint64_t s_unmute_events;

void audio_trace_init(void)
{
    for (int t = 0; t < AUDIO_TAP_COUNT; t++) {
        atomic_store(&s_taps[t].head, 0);
        s_taps[t].nonzero = 0;
        s_taps[t].audible = 0;
        s_taps[t].peak = 0;
    }
    atomic_store(&s_event_head, 0);
    atomic_store(&s_noted_frame, 0);
    s_pump_calls = 0;
    s_pump_skips = 0;
    s_underruns = 0;
    s_queue_hiwater = 0;
    s_queue_lowater = 0xFFFFFFFFu;
    s_mute_events = 0;
    s_unmute_events = 0;
}

void audio_trace_note_frame(uint32_t frame)
{
    atomic_store_explicit(&s_noted_frame, frame, memory_order_relaxed);
}

void audio_trace_pcm(int tap, const int16_t *stereo, int frames)
{
    if (tap < 0 || tap >= AUDIO_TAP_COUNT || !stereo || frames <= 0) return;
    PcmTap *t = &s_taps[tap];
    uint64_t head = atomic_load_explicit(&t->head, memory_order_relaxed);

    for (int f = 0; f < frames; f++) {
        int16_t l = stereo[f * 2 + 0];
        int16_t r = stereo[f * 2 + 1];
        uint32_t pos = (uint32_t)((head + (uint64_t)f) & PCM_RING_MASK);
        t->pcm[pos * 2 + 0] = l;
        t->pcm[pos * 2 + 1] = r;
        int32_t al = l < 0 ? -(int32_t)l : l;
        int32_t ar = r < 0 ? -(int32_t)r : r;
        int32_t a  = al > ar ? al : ar;
        if (a) t->nonzero++;
        if (a > AUDIBLE_ABS) t->audible++;
        if (a > t->peak) t->peak = a;
    }
    atomic_store_explicit(&t->head, head + (uint64_t)frames,
                          memory_order_release);
}

void audio_trace_event(uint16_t kind, uint32_t a, uint32_t b)
{
    uint64_t seq = atomic_load_explicit(&s_event_head, memory_order_relaxed);
    AudioTraceEvent *e = &s_events[(uint32_t)(seq & EV_RING_MASK)];
    e->seq        = seq;
    e->sample_idx = atomic_load_explicit(&s_taps[AUDIO_TAP_SPU_OUT].head,
                                         memory_order_relaxed);
    e->frame      = atomic_load_explicit(&s_noted_frame, memory_order_relaxed);
    e->kind       = kind;
    e->reserved   = 0;
    e->a          = a;
    e->b          = b;
    atomic_store_explicit(&s_event_head, seq + 1, memory_order_release);

    switch (kind) {
    case AUDIO_EV_RENDER:
        s_pump_calls++;
        if (b > s_queue_hiwater) s_queue_hiwater = b;
        if (b < s_queue_lowater) s_queue_lowater = b;
        break;
    case AUDIO_EV_PUMP_SKIP: s_pump_skips++;  break;
    case AUDIO_EV_UNDERRUN:  s_underruns++;   break;
    case AUDIO_EV_MUTE:      s_mute_events++; break;
    case AUDIO_EV_UNMUTE:    s_unmute_events++; break;
    default: break;
    }
}

void audio_trace_get_stats(AudioTraceStats *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    for (int t = 0; t < AUDIO_TAP_COUNT; t++) {
        out->tap_frames[t]  = atomic_load_explicit(&s_taps[t].head,
                                                   memory_order_acquire);
        out->tap_nonzero[t] = s_taps[t].nonzero;
        out->tap_audible[t] = s_taps[t].audible;
        out->tap_peak[t]    = s_taps[t].peak;
    }
    out->pump_calls     = s_pump_calls;
    out->pump_skips     = s_pump_skips;
    out->underruns      = s_underruns;
    out->queue_hiwater  = s_queue_hiwater;
    out->queue_lowater  = s_queue_lowater == 0xFFFFFFFFu ? 0 : s_queue_lowater;
    out->mute_events    = s_mute_events;
    out->unmute_events  = s_unmute_events;
    out->events_total   = atomic_load_explicit(&s_event_head,
                                               memory_order_acquire);
}

uint64_t audio_trace_tap_total(int tap)
{
    if (tap < 0 || tap >= AUDIO_TAP_COUNT) return 0;
    return atomic_load_explicit(&s_taps[tap].head, memory_order_acquire);
}

uint64_t audio_trace_events_total(void)
{
    return atomic_load_explicit(&s_event_head, memory_order_acquire);
}

uint32_t audio_trace_events_get(AudioTraceEvent *out, uint32_t max)
{
    if (!out || max == 0) return 0;
    uint64_t total = atomic_load_explicit(&s_event_head, memory_order_acquire);
    uint64_t avail = total < (uint64_t)EV_RING_CAP ? total : (uint64_t)EV_RING_CAP;
    if ((uint64_t)max > avail) max = (uint32_t)avail;
    uint64_t first = total - (uint64_t)max;
    for (uint32_t i = 0; i < max; i++)
        out[i] = s_events[(uint32_t)((first + i) & EV_RING_MASK)];
    return max;
}

/* ---- WAV dump --------------------------------------------------------- */

static void wav_write_header(FILE *fp, uint32_t data_bytes, uint32_t rate)
{
    uint8_t h[44];
    uint32_t byte_rate   = rate * 4u;
    uint32_t riff_size   = 36u + data_bytes;
    memcpy(h + 0, "RIFF", 4);
    h[4] = (uint8_t)riff_size; h[5] = (uint8_t)(riff_size >> 8);
    h[6] = (uint8_t)(riff_size >> 16); h[7] = (uint8_t)(riff_size >> 24);
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[17] = 0; h[18] = 0; h[19] = 0;   /* fmt chunk size */
    h[20] = 1;  h[21] = 0;                          /* PCM */
    h[22] = 2;  h[23] = 0;                          /* stereo */
    h[24] = (uint8_t)rate; h[25] = (uint8_t)(rate >> 8);
    h[26] = (uint8_t)(rate >> 16); h[27] = (uint8_t)(rate >> 24);
    h[28] = (uint8_t)byte_rate; h[29] = (uint8_t)(byte_rate >> 8);
    h[30] = (uint8_t)(byte_rate >> 16); h[31] = (uint8_t)(byte_rate >> 24);
    h[32] = 4; h[33] = 0;                           /* block align */
    h[34] = 16; h[35] = 0;                          /* bits per sample */
    memcpy(h + 36, "data", 4);
    h[40] = (uint8_t)data_bytes; h[41] = (uint8_t)(data_bytes >> 8);
    h[42] = (uint8_t)(data_bytes >> 16); h[43] = (uint8_t)(data_bytes >> 24);
    fwrite(h, 1, sizeof(h), fp);
}

int64_t audio_trace_dump_wav(int tap, const char *path,
                             int64_t start, uint64_t count)
{
    if (tap < 0 || tap >= AUDIO_TAP_COUNT || !path || !path[0]) return -1;
    PcmTap *t = &s_taps[tap];

    /* Snapshot the head; everything in [head - avail, head) is stable
     * (append-only, single writer) unless the writer laps us — the ring
     * holds ~95 s, a dump takes well under a second. */
    uint64_t head  = atomic_load_explicit(&t->head, memory_order_acquire);
    uint64_t avail = head < (uint64_t)PCM_RING_FRAMES ? head
                                                      : (uint64_t)PCM_RING_FRAMES;
    if (avail == 0) return -1;
    uint64_t oldest = head - avail;

    uint64_t s;   /* absolute first frame of the slice */
    if (start < 0) {
        s = oldest;
        if (count == 0 || count > avail) count = avail;
    } else {
        s = (uint64_t)start;
        if (s < oldest) s = oldest;             /* evicted prefix clamps */
        if (s >= head) return -1;
        uint64_t max = head - s;
        if (count == 0 || count > max) count = max;
    }
    /* Cap a single WAV at the ring capacity (paranoia; avail <= cap). */
    if (count > (uint64_t)PCM_RING_FRAMES) count = PCM_RING_FRAMES;

    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    /* Stamp the tap's true rate (SPU taps: exactly 44100; host tap: device
     * rate in bridge mode). A mislabeled header silently poisons every
     * offline A/B (snesrecomp da33c06). */
    wav_write_header(fp, (uint32_t)(count * 4u), s_tap_rate[tap]);

    uint64_t done = 0;
    while (done < count) {
        uint32_t pos   = (uint32_t)((s + done) & PCM_RING_MASK);
        uint32_t chunk = PCM_RING_FRAMES - pos;
        if ((uint64_t)chunk > count - done) chunk = (uint32_t)(count - done);
        if (fwrite(&t->pcm[(size_t)pos * 2u], 4u, chunk, fp) != chunk) {
            fclose(fp);
            return -1;
        }
        done += chunk;
    }
    fclose(fp);
    return (int64_t)count;
}
