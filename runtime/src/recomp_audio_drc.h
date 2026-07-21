/* recomp_audio_drc.h - shared audio clock-domain bridge for the *recomp ecosystems.
 *
 * One persistent band-limited polyphase windowed-sinc fractional resampler driven
 * by a P-only normalized-fill controller, fed by a millisecond-measured SPSC ring,
 * with smooth fade-based startup / underrun / overflow handling.
 *
 * It exists because every runner (NES/SNES/GBA/N64/Genesis) is video-master /
 * audio-slaved: guest audio is produced ~1 frame per wall-clock frame while the
 * host device consumes at its own crystal rate. The unsynchronized clocks drift,
 * and the historical "fix" was to discard samples (drop/flush/decimate or a
 * nearest-neighbor servo) -- every discard is a click, and continuous micro
 * discard/dup is a sustained crackle. This bridge replaces all of that: the
 * controller nudges the *resample ratio* by <= +/-0.5% to hold the ring near a
 * target fill, and the band-limited resampler keeps the waveform continuous.
 *
 * Single-file. Define RECOMP_AUDIO_DRC_IMPL in exactly ONE translation unit.
 * C99 and C++ compatible. Depends only on the C stdlib + libm.
 *
 * Threading: rab_push() is called by the producer (emulation thread), rab_pull()
 * by the consumer (host audio callback). One producer, one consumer. The shared
 * state touched across threads (in_count / out_pos / ring) is accessed in a
 * single-producer/single-consumer pattern; integate with whatever memory ordering
 * the host runner already uses for its existing queue (a mutex around push/pull is
 * fine and is what most of these runners already have).
 */
#ifndef RECOMP_AUDIO_DRC_H
#define RECOMP_AUDIO_DRC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int    channels;        /* 1 or 2                                          */
    double source_rate;     /* nominal guest source sample rate (Hz)           */
    double host_rate;       /* host device sample rate (Hz)                     */
    int    taps;            /* prototype length; 16 min, 32 preferred          */
    int    phases;          /* fractional phases; 256-1024 (512 default)       */
    double target_ms;       /* steady-state ring fill target (default 180)     */
    double ring_ms;         /* ring capacity in ms (default 280)               */
    double kp;              /* proportional gain (default 0.02)                */
    double ki_per_s;        /* optional integral gain per second (default 0):
                               P-only is bounded and cannot wind up across
                               bursty frame/stage transitions                  */
    double max_correction;  /* clamp on ratio correction (default 0.005)       */
    double err_lp_ms;       /* error low-pass time constant ms (default 75)    */
    double slew_pp_per_s;   /* correction slew, percent-points/sec (default 0.75) */
    double deadband_ms;     /* +/- band around target with no correction (1.0)  */
    double em_low_ms;       /* below this = underrun emergency (default 12)     */
    double em_high_ms;      /* above this = overflow emergency (default 235)    */
} rab_config;

typedef struct {
    uint64_t pushed_frames;
    uint64_t pulled_frames;
    uint64_t underrun_events;   /* times the ring ran dry mid-pull            */
    uint64_t overflow_drops;    /* source frames dropped on ring overflow     */
    double   last_fill_ms;
    double   last_correction;   /* applied ratio correction (signed)          */
} rab_stats;

typedef struct rab_bridge {
    rab_config cfg;
    int    half;            /* taps/2                                          */
    float *coeffs;          /* phases*taps, DC-normalized per phase            */
    float *ring;            /* cap_frames*channels, interleaved float          */
    int64_t cap;            /* ring capacity in frames                         */

    /* SPSC stream position */
    uint64_t in_count;      /* frames ever pushed (monotonic)                  */
    double   out_pos;       /* fractional source-frame read cursor             */

    /* controller */
    double cur_step;        /* current source frames consumed per output frame */
    double err_lp;          /* low-passed normalized error                     */
    double corr;            /* current (slew-limited) correction               */
    double corr_i;          /* integral term: holds steady clock-skew with
                               zero standing fill error (see controller)       */

    /* fade / emergency */
    double gain;            /* 0..1 smooth gate for startup/underrun           */
    int    primed;          /* set once fill first reaches target              */
    float  last_out[2];     /* last emitted sample per channel (for holds)     */

    rab_stats stats;
} rab_bridge;

/* Fill a config with the locked-in ecosystem defaults. You must still set
 * channels, source_rate and host_rate afterwards. */
void rab_config_defaults(rab_config *c);

/* Allocate internal buffers and build the polyphase filter bank. Returns 0 on
 * success, non-zero on allocation/parameter failure. */
int  rab_init(rab_bridge *b, const rab_config *cfg);
void rab_free(rab_bridge *b);

/* Producer: append `frames` interleaved int16 source samples. Thread: emu side. */
void rab_push(rab_bridge *b, const int16_t *interleaved, int frames);

/* Consumer: render exactly `frames` interleaved int16 host samples (never
 * blocks; emits faded silence on underrun / before prime). Thread: callback. */
void rab_pull(rab_bridge *b, int16_t *out, int frames);

/* Current ring fill expressed in milliseconds of source audio. */
double rab_fill_ms(const rab_bridge *b);

void rab_get_stats(const rab_bridge *b, rab_stats *out);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------------------------------------------- */
#ifdef RECOMP_AUDIO_DRC_IMPL

#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef RAB_PI
#define RAB_PI 3.14159265358979323846
#endif

static double rab__sinc(double x) {
    if (x > -1e-9 && x < 1e-9) return 1.0;
    double p = RAB_PI * x;
    return sin(p) / p;
}

/* Blackman window over [-1,1] mapped from t/half. */
static double rab__blackman(double tn) { /* tn in [-1,1] */
    double a = RAB_PI * (tn + 1.0); /* 0..2pi */
    return 0.42 - 0.5 * cos(a) + 0.08 * cos(2.0 * a);
}

void rab_config_defaults(rab_config *c) {
    c->channels       = 2;
    c->source_rate    = 48000.0;
    c->host_rate      = 48000.0;
    c->taps           = 32;
    c->phases         = 512;
    /* Streamed stage transitions can pause PSX audio production for ~140 ms
     * while guest cadence catches up inside the same reporting window. A
     * 160 ms target proved just short once: the 1024-frame host callback
     * crossed the remaining edge and emitted 216 faded frames (4.9 ms).
     * Keep 180 ms so the measured pause retains more than one callback of
     * reserve. Sustained slow emulation still drains it and remains visible
     * in telemetry. A 280 ms ring preserves 100 ms of overflow headroom. */
    c->target_ms      = 180.0;
    c->ring_ms        = 280.0;
    c->kp             = 0.02;
    /* P-only intentionally: the former 0.02/s integral retained a +0.5%
     * correction after transition bursts and drained a steady 59.94 Hz
     * producer. At the measured ~0.1% source skew, kp=0.02 settles only ~4 ms
     * off target and cannot accumulate that destructive history. */
    c->ki_per_s       = 0.0;
    c->max_correction = 0.005;
    c->err_lp_ms      = 75.0;
    c->slew_pp_per_s  = 0.75;
    c->deadband_ms    = 1.0;
    c->em_low_ms      = 12.0;
    c->em_high_ms     = 235.0;
}

int rab_init(rab_bridge *b, const rab_config *cfg) {
    if (!b || !cfg) return 1;
    if (cfg->channels < 1 || cfg->channels > 2) return 2;
    if (cfg->taps < 4 || (cfg->taps & 1)) return 3;
    if (cfg->phases < 16) return 4;
    if (cfg->source_rate <= 0.0 || cfg->host_rate <= 0.0) return 5;

    memset(b, 0, sizeof(*b));
    b->cfg  = *cfg;
    b->half = cfg->taps / 2;

    /* Prototype lowpass cutoff (normalized to source Nyquist). When downsampling
     * (host < source) we must pull the cutoff in to suppress aliases; when
     * upsampling we keep the full source band. A small guard avoids the corner. */
    double fc = (cfg->host_rate < cfg->source_rate)
                    ? (cfg->host_rate / cfg->source_rate) : 1.0;
    fc *= 0.92;

    size_t ncoef = (size_t)cfg->phases * (size_t)cfg->taps;
    b->coeffs = (float *)malloc(ncoef * sizeof(float));
    if (!b->coeffs) return 6;

    for (int p = 0; p < cfg->phases; ++p) {
        double frac = (double)p / (double)cfg->phases; /* [0,1) sub-sample delay */
        double sum = 0.0;
        float *row = b->coeffs + (size_t)p * cfg->taps;
        for (int k = 0; k < cfg->taps; ++k) {
            /* input tap index relative to the interpolation point */
            double t = (double)(k - (b->half - 1)) - frac;
            double tn = t / (double)b->half;          /* window arg in [-~1,~1] */
            double w  = (tn <= -1.0 || tn >= 1.0) ? 0.0 : rab__blackman(tn);
            double h  = fc * rab__sinc(fc * t) * w;
            row[k] = (float)h;
            sum += h;
        }
        /* normalize each phase to unit DC gain to kill level ripple / zipper */
        if (sum > 1e-12) {
            float inv = (float)(1.0 / sum);
            for (int k = 0; k < cfg->taps; ++k) row[k] *= inv;
        }
    }

    double cap_ms = cfg->ring_ms;
    if (cap_ms < cfg->target_ms + 40.0) cap_ms = cfg->target_ms + 40.0;
    b->cap = (int64_t)(cfg->source_rate * cap_ms / 1000.0) + cfg->taps + 2;
    b->ring = (float *)calloc((size_t)b->cap * cfg->channels, sizeof(float));
    if (!b->ring) { free(b->coeffs); b->coeffs = NULL; return 7; }

    b->in_count = 0;
    /* Start the read cursor half a window in so the polyphase filter always has
     * valid history behind it; otherwise the first half-1 outputs can't be
     * computed and the cursor would never advance. */
    b->out_pos  = (double)(b->half - 1);
    b->cur_step = cfg->source_rate / cfg->host_rate;
    b->err_lp   = 0.0;
    b->corr     = 0.0;
    b->gain     = 0.0;
    b->primed   = 0;
    return 0;
}

void rab_free(rab_bridge *b) {
    if (!b) return;
    free(b->coeffs); b->coeffs = NULL;
    free(b->ring);   b->ring   = NULL;
}

double rab_fill_ms(const rab_bridge *b) {
    double fill = (double)b->in_count - b->out_pos;
    if (fill < 0.0) fill = 0.0;
    return fill * 1000.0 / b->cfg.source_rate;
}

void rab_push(rab_bridge *b, const int16_t *in, int frames) {
    int ch = b->cfg.channels;
    /* Track fill incrementally — the old per-frame (in_count - out_pos)
     * recompute sat under SDL_LockAudioDevice for every SPU sample and
     * correlated with MotK's post-XA FPS cliff. */
    double fill = (double)b->in_count - b->out_pos;
    const double cap_lim = (double)(b->cap - 1);
    for (int f = 0; f < frames; ++f) {
        if (fill >= cap_lim) {
            /* Overflow emergency: producer outran consumer past the ring. Drop the
             * oldest source frame by nudging the read cursor forward. Rare under
             * DRC; the resampler's continuous history limits the audible seam. */
            b->out_pos += 1.0;
            b->stats.overflow_drops++;
            fill -= 1.0;
        }
        int64_t w = (int64_t)(b->in_count % (uint64_t)b->cap);
        float *slot = b->ring + w * ch;
        for (int c = 0; c < ch; ++c)
            slot[c] = (float)in[f * ch + c] * (1.0f / 32768.0f);
        b->in_count++;
        fill += 1.0;
    }
    b->stats.pushed_frames += (uint64_t)frames;
}

static void rab__update_controller(rab_bridge *b, int pull_frames) {
    rab_config *c = &b->cfg;
    double fill_ms = rab_fill_ms(b);
    b->stats.last_fill_ms = fill_ms;

    if (!b->primed && fill_ms >= c->target_ms) b->primed = 1;

    /* One control update per pull. Use the callback's REAL duration: assuming
     * 12 ms made integral gain and slew run more than twice as fast with common
     * 256-frame/48 kHz callbacks. The resulting limit cycle repeatedly hit the
     * +0.5% clamp and drained an otherwise steady producer ring. */
    double dt_ms = (double)pull_frames * 1000.0 / c->host_rate;
    if (dt_ms < 0.1) dt_ms = 0.1;
    if (dt_ms > 100.0) dt_ms = 100.0;
    double err = (fill_ms - c->target_ms) / c->target_ms;
    if (fill_ms > c->target_ms - c->deadband_ms &&
        fill_ms < c->target_ms + c->deadband_ms) {
        err = 0.0; /* deadband */
    }
    double alpha = dt_ms / (c->err_lp_ms + dt_ms);
    b->err_lp += alpha * (err - b->err_lp);

    /* PI control. P alone cannot hold a sustained source-clock skew without a
     * standing fill error of corr/kp (a 0.3% skew cost 7.5 ms of permanent
     * fill deficit at kp=0.02/target=50 — margin that jitter spikes then
     * drained to underrun). The integral accumulates the steady skew so fill
     * returns to target; anti-windup: never integrate deeper into the clamp. */
    double target_corr = c->kp * b->err_lp + b->corr_i;
    int clamped_hi = target_corr >  c->max_correction;
    int clamped_lo = target_corr < -c->max_correction;
    if ((!clamped_hi && !clamped_lo) ||
        (clamped_hi && b->err_lp < 0.0) || (clamped_lo && b->err_lp > 0.0)) {
        b->corr_i += c->ki_per_s * (dt_ms / 1000.0) * b->err_lp;
        if (b->corr_i >  c->max_correction) b->corr_i =  c->max_correction;
        if (b->corr_i < -c->max_correction) b->corr_i = -c->max_correction;
    }
    if (target_corr >  c->max_correction) target_corr =  c->max_correction;
    if (target_corr < -c->max_correction) target_corr = -c->max_correction;

    /* slew limit (percentage points per second -> fraction per update) */
    double slew = (c->slew_pp_per_s / 100.0) * (dt_ms / 1000.0);
    double d = target_corr - b->corr;
    if (d >  slew) d =  slew;
    if (d < -slew) d = -slew;
    b->corr += d;

    /* A high queue (positive error) must be consumed FASTER -> larger step. */
    b->cur_step = (c->source_rate / c->host_rate) * (1.0 + b->corr);
    b->stats.last_correction = b->corr;
}

void rab_pull(rab_bridge *b, int16_t *out, int frames) {
    int ch = b->cfg.channels;
    rab__update_controller(b, frames);

    double gstep = 1.0 / (b->cfg.host_rate * 0.003); /* ~3ms fade in/out */
    double step  = b->cur_step;

    for (int f = 0; f < frames; ++f) {
        int64_t i   = (int64_t)floor(b->out_pos);
        double  fr  = b->out_pos - (double)i;
        int64_t newest_needed = i + b->half;
        int     have = b->primed && (newest_needed < (int64_t)b->in_count)
                       && ((i - b->half + 1) >= 0);

        float s[2] = {0.0f, 0.0f};
        if (have) {
            int phase = (int)(fr * b->cfg.phases);
            if (phase >= b->cfg.phases) phase = b->cfg.phases - 1;
            const float *cz = b->coeffs + (size_t)phase * b->cfg.taps;
            for (int k = 0; k < b->cfg.taps; ++k) {
                int64_t m = i - b->half + 1 + k;
                const float *sl = b->ring + (m % b->cap) * ch;
                float w = cz[k];
                s[0] += w * sl[0];
                if (ch == 2) s[1] += w * sl[1];
            }
            b->last_out[0] = s[0];
            if (ch == 2) b->last_out[1] = s[1];
            b->out_pos += step;             /* advance only when we consumed */
        } else {
            /* underrun / not yet primed: hold last sample, let gain fade it out */
            s[0] = b->last_out[0];
            if (ch == 2) s[1] = b->last_out[1];
            if (b->primed && newest_needed >= (int64_t)b->in_count)
                b->stats.underrun_events++;
        }

        /* smooth gate: fade toward 1 when delivering real audio, toward 0 else */
        double target_gain = have ? 1.0 : 0.0;
        if (b->gain < target_gain) {
            b->gain += gstep; if (b->gain > target_gain) b->gain = target_gain;
        } else if (b->gain > target_gain) {
            b->gain -= gstep; if (b->gain < target_gain) b->gain = target_gain;
        }

        for (int c = 0; c < ch; ++c) {
            double v = (double)s[c] * b->gain * 32768.0;
            if (v >  32767.0) v =  32767.0;
            if (v < -32768.0) v = -32768.0;
            out[f * ch + c] = (int16_t)lrint(v);
        }
    }
    b->stats.pulled_frames += (uint64_t)frames;
}

void rab_get_stats(const rab_bridge *b, rab_stats *out) { *out = b->stats; }

#endif /* RECOMP_AUDIO_DRC_IMPL */
#endif /* RECOMP_AUDIO_DRC_H */
