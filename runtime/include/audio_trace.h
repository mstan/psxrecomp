/*
 * audio_trace.h - always-on audio observability rings (PCM taps + event ring).
 *
 * Port of the snesrecomp audio-campaign measurement harness (audio_trace.{c,h},
 * commit 67ead27 lineage) to the PSX pipeline, following the cross-system tap
 * model in F:/Projects/_audio_round2/ROUND2_PLAN.md: always-on ring buffers
 * recording every sample/event from process start, dumped AFTER the fact via
 * TCP debug commands — never arm-then-reproduce.
 *
 * Taps (PSX pipeline):
 *   AUDIO_TAP_SPU_OUT  "T1" — canonical spu_render() output @44100, recorded
 *                      inside spu_render so every caller (pump, fade tail) is
 *                      covered, PRE any host-side fade/mute gain.
 *   AUDIO_TAP_CD_IN    "T2" — decoded XA/CD-DA PCM pushed onto the SPU CD
 *                      input bus (already resampled to 44100 by the producer).
 *   AUDIO_TAP_HOST     "T3" — the exact bytes handed to the host audio API
 *                      (post fade ramps; what SDL_QueueAudio receives).
 *
 * The same module compiles into psx-beetle, where AUDIO_TAP_SPU_OUT records
 * the libretro audio_batch callback — Beetle's fully-mixed reference SPU
 * output — giving symmetric `audio_*` commands on ports 4370 and 4380.
 *
 * Threading: each tap has a single writer. Readers (debug-server thread)
 * take a stable [oldest, head) snapshot; the PCM rings hold ~95 s at 44100,
 * so a dump cannot be lapped mid-read in practice. Indices are C11 atomics
 * (release on publish, acquire on read).
 */
#ifndef PSX_AUDIO_TRACE_H
#define PSX_AUDIO_TRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AUDIO_TAP_SPU_OUT = 0,
    AUDIO_TAP_CD_IN   = 1,
    AUDIO_TAP_HOST    = 2,
    AUDIO_TAP_VOICES  = 3,   /* T0: 24-voice sum only (pre CD mix, pre main) */
    AUDIO_TAP_COUNT   = 4
};

/* Event kinds. REG_WRITE covers every SPU register store (addr/value), which
 * subsumes KEYON/KEYOFF/pitch/volume writes — the semantic KON ring stays in
 * spu.c. The rest instrument the host delivery path. */
enum {
    AUDIO_EV_NONE      = 0,
    AUDIO_EV_REG_WRITE = 1,  /* a=SPU reg addr, b=value */
    AUDIO_EV_RENDER    = 2,  /* a=frames rendered, b=host queued bytes before */
    AUDIO_EV_PUMP_SKIP = 3,  /* backpressure skip; a=queued bytes */
    AUDIO_EV_UNDERRUN  = 4,  /* host queue empty at pump entry; a=queued bytes */
    AUDIO_EV_MUTE      = 5,  /* turbo mute engaged; a=fade tail frames */
    AUDIO_EV_UNMUTE    = 6,  /* pumping resumed; a=fade-in frames pending */
    AUDIO_EV_CD_PUSH   = 7,  /* a=guest-cycle clock low32, b=ring fill after */
    AUDIO_EV_DMA_WRITE = 8,  /* SPU RAM DMA; a=words, b=transfer addr after */
    AUDIO_EV_XA_ZERO   = 9,  /* zero-run in decoded XA PCM; a=lba,
                                b=(stage<<28)|(start_frame<<14)|run_len.
                                stage 0=post-ADPCM-decode (native rate),
                                stage 1=post-resample+volume (44100). */
};

typedef struct {
    uint64_t seq;         /* monotone event counter */
    uint64_t sample_idx;  /* AUDIO_TAP_SPU_OUT frames produced when recorded */
    uint32_t frame;       /* host-noted frame counter (vblank / retro_run) */
    uint16_t kind;
    uint16_t reserved;
    uint32_t a;
    uint32_t b;
} AudioTraceEvent;

typedef struct {
    /* Per-tap PCM production. */
    uint64_t tap_frames[AUDIO_TAP_COUNT];      /* frames ever recorded */
    uint64_t tap_nonzero[AUDIO_TAP_COUNT];     /* frames with |L| or |R| > 0 */
    uint64_t tap_audible[AUDIO_TAP_COUNT];     /* frames with |L| or |R| > 256 (~ -42 dBFS) */
    int32_t  tap_peak[AUDIO_TAP_COUNT];        /* running peak |sample| */
    /* Host pump behavior. */
    uint64_t pump_calls;
    uint64_t pump_skips;         /* backpressure early-outs */
    uint64_t underruns;          /* pump entered with 0 queued bytes (post first audio) */
    uint32_t queue_hiwater;      /* max queued bytes observed at pump entry */
    uint32_t queue_lowater;      /* min queued bytes observed at pump entry (post first audio) */
    uint64_t mute_events;
    uint64_t unmute_events;
    /* Event ring. */
    uint64_t events_total;
} AudioTraceStats;

void audio_trace_init(void);

/* Record `frames` interleaved stereo s16 frames into tap `tap`. */
void audio_trace_pcm(int tap, const int16_t *stereo, int frames);

/* Record a pipeline event (timestamps applied internally). */
void audio_trace_event(uint16_t kind, uint32_t a, uint32_t b);

/* Host frame tag for event timestamps (call once per vblank / retro_run). */
void audio_trace_note_frame(uint32_t frame);

void     audio_trace_get_stats(AudioTraceStats *out);
uint64_t audio_trace_tap_total(int tap);
uint64_t audio_trace_events_total(void);

/* Taps default to 44100 Hz; the host tap may run at the device rate in
 * bridge/pull mode. The WAV dumper stamps this rate — a mislabeled header
 * silently poisons every offline A/B. */
void     audio_trace_set_tap_rate(int tap, uint32_t rate);
uint32_t audio_trace_tap_rate(int tap);

/* Copy the most recent `max` events, oldest first. Returns count copied. */
uint32_t audio_trace_events_get(AudioTraceEvent *out, uint32_t max);

/* Write a tap slice as a stereo s16 WAV at the tap's rate. start=-1,count=0
 * dumps the whole currently-buffered ring. `start`/`count` are absolute frame
 * indices on the tap's production timeline. Returns frames written, or -1 on
 * error (unwritable path / empty ring / slice already evicted). */
int64_t audio_trace_dump_wav(int tap, const char *path,
                             int64_t start, uint64_t count);

#ifdef __cplusplus
}
#endif

#endif /* PSX_AUDIO_TRACE_H */
