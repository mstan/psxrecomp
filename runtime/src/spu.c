/*
 * spu.c - PS1 Sound Processing Unit register and direct ADPCM voice model.
 *
 * This is intentionally still a compact hardware model: it accepts SPU
 * register reads/writes, DMA4 transfers into 512KB SPU RAM, mixes the
 * 24 direct ADPCM voices, and accepts decoded CD/XA audio on the SPU CD
 * input bus. Reverb, noise, sweep volumes, and IRQ timing are not modeled yet.
 */

#include "spu.h"
#include "spu_gauss.h"
#include "spu_shadow.h"
#include "audio_trace.h"
#include "psx_cycles.h"

#include <string.h>

#define SPU_RAM_SIZE       (512 * 1024)
#define SPU_REG_COUNT      256
#define SPU_VOICE_COUNT    24
#define SPU_BLOCK_SAMPLES  28

static uint8_t  spu_ram[SPU_RAM_SIZE];
static uint16_t spu_regs[SPU_REG_COUNT];
static uint32_t transfer_addr;
static uint32_t key_on_count;
static uint64_t render_frames;
static uint64_t nonzero_frames;
static int32_t last_peak;
static int32_t peak;

/* End-block-reached latch (SPU register 0x1F801D9C/D9E on real hw).
 * Set when a voice decodes a block whose flag byte has bit 0 (loop end).
 * Polled by music engines to know when a one-shot voice has finished. */
static uint32_t endx_latch;

/* KEYON/KEYOFF latches: most recent values written, sticky across reads
 * so debug snapshots always see what the BIOS last requested even if the
 * BIOS clears them quickly. */
static uint32_t kon_latch;
static uint32_t koff_latch;

/* External vblank counter (debug_server.c) used as event timestamp. */
extern uint64_t s_frame_count;

/* ---- Always-on event ring -------------------------------------------- */
/* 1M entries × ~32B = 32 MB. Power of 2 so wrap is a mask. Beetle peaks at
 * a few hundred audible-voice events per chime second; recomp at <1k total
 * per chime. 1M gives ~minutes of headroom for game-scene capture too. */
#define SPU_EVENT_CAP (1u << 20)
static SpuEvent  s_events[SPU_EVENT_CAP];
static uint32_t  s_event_idx = 0;
static uint64_t  s_event_seq = 0;

/* CD input FIFO, fed by the CD-ROM XA decoder at 44.1 kHz stereo. */
#define SPU_CD_RING_FRAMES (44100u * 8u)
static int16_t  cd_ring[SPU_CD_RING_FRAMES * 2u];
static uint32_t cd_read_pos;
static uint32_t cd_write_pos;
static uint32_t cd_frame_count;
static uint64_t cd_push_frames;
static uint64_t cd_overflow_frames;
static uint64_t cd_underflow_frames;

/* ADSR phases — match Beetle's order so cross-process diffs read straight. */
#define ADSR_ATTACK   0
#define ADSR_DECAY    1
#define ADSR_SUSTAIN  2
#define ADSR_RELEASE  3

typedef struct {
    int active;
    uint32_t cur_addr;
    uint32_t repeat_addr;
    int16_t samples[SPU_BLOCK_SAMPLES];
    int16_t previous_samples[3];
    int sample_idx;
    uint32_t phase;
    int16_t hist1;
    int16_t hist2;
    uint8_t flags;

    /* ADSR envelope state. Stepped once per output sample (44.1 kHz). */
    uint16_t env_level;     /* 0..0x7FFF — applied to raw decoded sample */
    uint32_t adsr_divider;  /* fixed-point counter; level updates on overflow */
    uint8_t  adsr_phase;    /* ADSR_ATTACK / DECAY / SUSTAIN / RELEASE */
} SpuVoice;

static SpuVoice voices[SPU_VOICE_COUNT];

static void spu_event_record(uint8_t kind, int voice, uint32_t addr) {
    SpuEvent *e = &s_events[s_event_idx & (SPU_EVENT_CAP - 1u)];
    e->seq      = s_event_seq++;
    e->frame    = (uint32_t)s_frame_count;
    e->kind     = kind;
    e->voice    = (uint8_t)voice;
    e->pitch    = spu_regs[(uint32_t)voice * 8u + 2u];
    e->addr     = addr;
    /* PSX voice block layout (16-bit register indices from voice base):
     *   0=VOL_L 1=VOL_R 2=PITCH 3=START 4=ADSR_LO 5=ADSR_HI 6=CURVOL 7=LOOP */
    e->adsr_lo  = spu_regs[(uint32_t)voice * 8u + 4u];
    e->adsr_hi  = spu_regs[(uint32_t)voice * 8u + 5u];
    e->vol_l    = spu_regs[(uint32_t)voice * 8u + 0u];
    e->vol_r    = spu_regs[(uint32_t)voice * 8u + 1u];
    s_event_idx++;
}

/* PS1 envelope rate decoder. Ported verbatim from Beetle's CalcVCDelta
 * (beetle-psx/mednafen/psx/spu.cpp). Each call emits the per-step
 * `increment` to add to env_level, and `divinco` added to a divider —
 * level is updated only when divider crosses 0x8000. The combination
 * encodes both linear and pseudo-exponential ramps at PSX-faithful
 * rates (rates 0..127 span ~0.1 ms .. ~30+ s). */
static void calc_vc_delta(uint8_t zs, uint8_t speed, int log_mode, int dec_mode,
                          int inv_increment, int16_t current,
                          int *out_increment, int *out_divinco)
{
    int increment = (7 - (speed & 0x3));
    if (inv_increment) increment = ~increment;
    int divinco = 32768;

    if (speed < 0x2C)
        increment = (unsigned)increment << ((0x2F - speed) >> 2);
    if (speed >= 0x30)
        divinco >>= (speed - 0x2C) >> 2;

    if (log_mode) {
        if (dec_mode) {
            increment = (current * increment) >> 15;
        } else if ((current & 0x7FFF) >= 0x6000) {
            if (speed < 0x28) {
                increment >>= 2;
            } else if (speed >= 0x2C) {
                divinco >>= 2;
            } else {
                increment >>= 1;
                divinco   >>= 1;
            }
        }
    }

    if (divinco == 0 && speed < zs) divinco = 1;

    *out_increment = increment;
    *out_divinco   = divinco;
}

/* Step ADSR envelope by one output sample for voice `idx`. Mirrors
 * Beetle's PS_SPU::RunEnvelope. */
static void adsr_run(int idx, SpuVoice *v) {
    uint32_t raw = (uint32_t)spu_regs[(uint32_t)idx * 8u + 4u]
                 | ((uint32_t)spu_regs[(uint32_t)idx * 8u + 5u] << 16);

    int     Sl           = (int)(raw >> 0)  & 0x0F;
    int     Dr           = (int)(raw >> 4)  & 0x0F;
    int     Ar           = (int)(raw >> 8)  & 0x7F;
    int     attack_exp   = (int)((raw >> 15) & 1);
    int     Rr           = (int)(raw >> 16) & 0x1F;
    int     release_exp  = (int)((raw >> 21) & 1);
    int     Sr           = (int)(raw >> 22) & 0x7F;
    int     sustain_dec  = (int)((raw >> 30) & 1);
    int     sustain_exp  = (int)((raw >> 31) & 1);
    int     sustain_lvl  = (Sl + 1) << 11;

    /* Attack tops out at 0x7FFF — switch to Decay (Beetle does this
     * before the switch on Phase). */
    if (v->adsr_phase == ADSR_ATTACK && v->env_level == 0x7FFF)
        v->adsr_phase = ADSR_DECAY;

    int increment = 0, divinco = 0;
    int16_t uoflow_reset = 0;

    switch (v->adsr_phase) {
    case ADSR_ATTACK:
        calc_vc_delta(0x7F, (uint8_t)Ar, attack_exp, 0, 0,
                      (int16_t)v->env_level, &increment, &divinco);
        uoflow_reset = 0x7FFF;
        break;
    case ADSR_DECAY:
        calc_vc_delta(0x1F << 2, (uint8_t)(Dr << 2), 1, 1, 1,
                      (int16_t)v->env_level, &increment, &divinco);
        uoflow_reset = 0;
        break;
    case ADSR_SUSTAIN:
        calc_vc_delta(0x7F, (uint8_t)Sr, sustain_exp, sustain_dec, sustain_dec,
                      (int16_t)v->env_level, &increment, &divinco);
        uoflow_reset = sustain_dec ? 0 : 0x7FFF;
        break;
    case ADSR_RELEASE:
        calc_vc_delta(0x1F << 2, (uint8_t)(Rr << 2), release_exp, 1, 1,
                      (int16_t)v->env_level, &increment, &divinco);
        uoflow_reset = 0;
        break;
    default:
        return;
    }

    v->adsr_divider += (uint32_t)divinco;
    if (v->adsr_divider & 0x8000u) {
        uint16_t prev = v->env_level;
        v->adsr_divider = 0;
        v->env_level = (uint16_t)((int)v->env_level + increment);

        if (v->adsr_phase == ADSR_ATTACK) {
            /* If high bit just rolled over (0→1), clamp to uoflow_reset. */
            if (((prev ^ v->env_level) & v->env_level) & 0x8000u)
                v->env_level = (uint16_t)uoflow_reset;
        } else {
            if (v->env_level & 0x8000u)
                v->env_level = (uint16_t)uoflow_reset;
        }

        if (v->adsr_phase == ADSR_DECAY && v->env_level < (uint16_t)sustain_lvl)
            v->adsr_phase = ADSR_SUSTAIN;
    }
}

static inline int16_t clamp16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static inline int32_t abs32(int32_t v) {
    return v < 0 ? -v : v;
}

static inline uint32_t reg_index(uint32_t addr) {
    return (addr - 0x1F801C00u) >> 1;
}

static inline uint16_t voice_reg(int voice, int reg) {
    return spu_regs[(uint32_t)voice * 8u + (uint32_t)reg];
}

static inline int16_t direct_volume(uint16_t raw) {
    int32_t v;
    if (raw & 0x8000u) {
        /* Sweep mode is not modeled; use the magnitude as a direct volume. */
        v = (int32_t)(raw & 0x7FFFu);
    } else {
        v = (int32_t)(raw & 0x7FFFu);
        if (v & 0x4000) v -= 0x8000;
    }
    if (v > 0x3FFF) v = 0x3FFF;
    if (v < -0x4000) v = -0x4000;
    return (int16_t)v;
}

static inline int16_t cd_input_volume(uint16_t raw) {
    /* CD input volume registers use signed 16-bit linear gain; games commonly
     * program 0x7FFF for full-scale CD audio. */
    return (int16_t)raw;
}

void spu_cd_audio_reset(void) {
    memset(cd_ring, 0, sizeof(cd_ring));
    cd_read_pos = 0;
    cd_write_pos = 0;
    cd_frame_count = 0;
    cd_push_frames = 0;
    cd_overflow_frames = 0;
    cd_underflow_frames = 0;
}

void spu_cd_audio_push(const int16_t* stereo, int frames) {
    if (!stereo || frames <= 0) return;

    uint32_t in_frames = (uint32_t)frames;
    if (in_frames > SPU_CD_RING_FRAMES) {
        uint32_t skip = in_frames - SPU_CD_RING_FRAMES;
        stereo += skip * 2u;
        cd_overflow_frames += skip;
        in_frames = SPU_CD_RING_FRAMES;
    }

    if (cd_frame_count + in_frames > SPU_CD_RING_FRAMES) {
        uint32_t drop = (cd_frame_count + in_frames) - SPU_CD_RING_FRAMES;
        cd_read_pos = (cd_read_pos + drop) % SPU_CD_RING_FRAMES;
        cd_frame_count -= drop;
        cd_overflow_frames += drop;
    }

    for (uint32_t i = 0; i < in_frames; i++) {
        cd_ring[cd_write_pos * 2u + 0u] = stereo[i * 2u + 0u];
        cd_ring[cd_write_pos * 2u + 1u] = stereo[i * 2u + 1u];
        cd_write_pos = (cd_write_pos + 1u) % SPU_CD_RING_FRAMES;
    }
    cd_frame_count += in_frames;
    cd_push_frames += in_frames;

    /* T2 tap: what the CD/XA decoder feeds the SPU CD input bus. The event
     * stamps the GLOBAL GUEST CYCLE clock (low 32 bits) so push-to-push
     * spacing measures true delivery cadence — the spu_out sample_idx stamp
     * is pump-chunk quantized and useless for that. */
    audio_trace_pcm(AUDIO_TAP_CD_IN, stereo, (int)in_frames);
    audio_trace_event(AUDIO_EV_CD_PUSH, (uint32_t)psx_get_cycle_count(),
                      cd_frame_count);
}

static int cd_audio_pop(int16_t* left, int16_t* right) {
    if (cd_frame_count == 0) return 0;
    *left = cd_ring[cd_read_pos * 2u + 0u];
    *right = cd_ring[cd_read_pos * 2u + 1u];
    cd_read_pos = (cd_read_pos + 1u) % SPU_CD_RING_FRAMES;
    cd_frame_count--;
    return 1;
}

static void decode_block(SpuVoice *v) {
    static const int f0[5] = { 0, 60, 115, 98, 122 };
    static const int f1[5] = { 0, 0, -52, -55, -60 };

    uint32_t addr = v->cur_addr & (SPU_RAM_SIZE - 1u);
    if (addr + 16u > SPU_RAM_SIZE) addr = 0;

    uint8_t header = spu_ram[addr + 0u];
    uint8_t flags = spu_ram[addr + 1u];
    int shift = header & 0x0F;
    int filter = (header >> 4) & 0x0F;
    if (filter > 4) filter = 0;
    if (shift > 12) shift = 12;

    /* Gaussian interpolation reaches three samples into the preceding ADPCM
     * block. KEYON clears samples[], so the first block naturally sees
     * silence for its unavailable history. */
    v->previous_samples[0] = v->samples[SPU_BLOCK_SAMPLES - 3];
    v->previous_samples[1] = v->samples[SPU_BLOCK_SAMPLES - 2];
    v->previous_samples[2] = v->samples[SPU_BLOCK_SAMPLES - 1];

    int out = 0;
    for (int b = 0; b < 14; b++) {
        uint8_t packed = spu_ram[addr + 2u + (uint32_t)b];
        for (int n = 0; n < 2; n++) {
            int sample4 = (n == 0) ? (packed & 0x0F) : (packed >> 4);
            if (sample4 & 0x08) sample4 -= 0x10;

            int32_t s = sample4 << 12;
            s >>= shift;
            s += ((int32_t)v->hist1 * f0[filter] +
                  (int32_t)v->hist2 * f1[filter] + 32) >> 6;
            s = clamp16(s);
            v->hist2 = v->hist1;
            v->hist1 = (int16_t)s;
            v->samples[out++] = (int16_t)s;
        }
    }

    if (flags & 0x04u) {
        v->repeat_addr = addr;
        /* The auto-latch updates the guest-visible register too (Beetle
         * spu.cpp:386 + read path 1302): drivers read it back to save
         * loop points. */
        int v_idx = (int)(v - voices);
        if (v_idx >= 0 && v_idx < SPU_VOICE_COUNT)
            spu_regs[(uint32_t)v_idx * 8u + 7u] = (uint16_t)(addr >> 3);
    }
    v->flags = flags;
    v->sample_idx = 0;
    v->cur_addr = (addr + 16u) & (SPU_RAM_SIZE - 1u);

    /* Latch end-block-reached so the BIOS music engine sees ENDX[v] = 1
     * when it polls 0x1F801D9C/D9E. Without this latch one-shot music
     * engines never advance, leaving subsequent voices unkeyed. */
    if (flags & 0x01u) {
        int v_idx = (int)(v - voices);
        if (v_idx >= 0 && v_idx < SPU_VOICE_COUNT) {
            endx_latch |= (1u << v_idx);
        }
    }
}

/* ---- Verified-enhancement shadow tap (opt-in; see spu_shadow.{h,c}) ------
 *
 * When the float SPU shadow is enabled, the mix loop records — per output
 * frame, per contributing voice — the EXACT inputs the canon used to produce
 * that voice's contribution: the four decoded samples bracketing the current
 * sub-sample phase, the fractional phase, the envelope level, and the per-voice
 * L/R volumes. The shadow re-renders those in float with cubic interpolation.
 * Sourcing the decoded data + envelope from the canon means the shadow can only
 * differ in HOW a note is resampled, never in WHICH note plays.
 *
 * Inert and zero-cost when the shadow is disabled (s_shadow_tap_on == 0): the
 * mix loop's recording is guarded by that flag, set once from spu_render. */
typedef struct {
    int16_t  s[4];     /* decoded samples at sample_idx-1 .. sample_idx+2 */
    float    frac;     /* fractional phase in [0,1) at this output frame */
    uint16_t env;      /* env_level (0..0x7FFF) */
    int16_t  vol_l;    /* per-voice volume (already direct_volume-decoded) */
    int16_t  vol_r;
    uint8_t  active;
} SpuShadowVoiceTap;

/* One frame's worth of per-voice taps, plus the global scale used this block. */
typedef struct {
    SpuShadowVoiceTap voice[SPU_VOICE_COUNT];
    int16_t main_l;
    int16_t main_r;
    int     enabled;   /* SPU control enable bit this block */
} SpuShadowFrameTap;

/* Sized to the largest spu_render block (main.cpp caps at 2048 frames). */
#define SPU_SHADOW_TAP_FRAMES 2048
static SpuShadowFrameTap s_shadow_tap[SPU_SHADOW_TAP_FRAMES];
static int               s_shadow_tap_on = 0;   /* set by spu_render when enabled */
static int               s_shadow_tap_frame = 0;

/* The shadow reads s_shadow_tap as SpuShadowFrameTapPub[] (spu.h). Guarantee
 * the internal and public layouts are identical so the cast is safe. */
typedef char spu_shadow_tap_voice_size_check[
    (sizeof(SpuShadowVoiceTap) == sizeof(SpuShadowVoiceTapPub)) ? 1 : -1];
typedef char spu_shadow_tap_frame_size_check[
    (sizeof(SpuShadowFrameTap) == sizeof(SpuShadowFrameTapPub)) ? 1 : -1];
typedef char spu_shadow_voice_count_check[
    (SPU_VOICE_COUNT == SPU_SHADOW_MAX_VOICES) ? 1 : -1];

const void* spu_shadow_tap_buffer(void) { return s_shadow_tap; }
int         spu_shadow_tap_count(void)  { return s_shadow_tap_frame; }

static int16_t voice_next_sample(int idx) {
    SpuVoice *v = &voices[idx];
    if (!v->active) return 0;

    if (v->sample_idx >= SPU_BLOCK_SAMPLES) {
        if (v->flags & 0x01u) {
            /* END flag: the decode pointer jumps to the latched repeat
             * address in BOTH the loop and stop cases — Beetle spu.cpp:333
             * (CurAddr = LoopAddr) does this unconditionally. */
            v->cur_addr = v->repeat_addr & (SPU_RAM_SIZE - 1u);
            if (v->flags & 0x02u) {
                spu_event_record(SPU_EV_END_LOOP, idx, v->repeat_addr);
            } else {
                /* END without REPEAT: hardware forces the envelope to ZERO
                 * and enters Release (Beetle spu.cpp:341-352 — "Force
                 * enveloping to 0 if not looping"). The voice keeps decoding
                 * from the repeat address, silently.
                 *
                 * The previous shape decoded FORWARD past the terminator
                 * with the envelope intact, assuming the release would mask
                 * whatever follows. X4's SFX bank breaks both assumptions:
                 * its one-shot samples end on an END-only block whose
                 * successor is non-silent filler (0x77 nibbles at shift 0 =
                 * +28672 DC) with a self-looping LOOPSTART flag, and its
                 * ADSR release shift is ~infinite. Every finished SFX voice
                 * parked there at full envelope; a handful of ice-block
                 * hits in the X4 attract demo railed the whole mix at
                 * +32767 for ~35 s ("static" / music cut-out). */
                spu_event_record(SPU_EV_END_STOP, idx, v->cur_addr);
                v->adsr_phase = ADSR_RELEASE;
                v->adsr_divider = 0;
                v->env_level = 0;
            }
        }
        decode_block(v);
    }

    int16_t raw_s = spu_gaussian_interpolate(v->previous_samples,
                                              v->samples,
                                              v->sample_idx,
                                              v->phase);
    /* Apply envelope (0..0x7FFF as a 15-bit gain). */
    int32_t shaped = ((int32_t)raw_s * (int32_t)v->env_level) >> 15;
    if (shaped > 32767)  shaped = 32767;
    if (shaped < -32768) shaped = -32768;

    /* Shadow tap: record the four decoded samples bracketing the current
     * sample position + the fractional phase + envelope, BEFORE the phase
     * advances. The optional shadow keeps its own cubic, block-clamped window;
     * the canonical hardware path above independently uses the preceding
     * block's tail for Gaussian interpolation. */
    if (s_shadow_tap_on && s_shadow_tap_frame < SPU_SHADOW_TAP_FRAMES) {
        SpuShadowVoiceTap *t =
            &s_shadow_tap[s_shadow_tap_frame].voice[idx];
        int si = v->sample_idx;
        int i0 = si - 1, i2 = si + 1, i3 = si + 2;
        if (i0 < 0) i0 = 0;
        if (i2 >= SPU_BLOCK_SAMPLES) i2 = SPU_BLOCK_SAMPLES - 1;
        if (i3 >= SPU_BLOCK_SAMPLES) i3 = SPU_BLOCK_SAMPLES - 1;
        t->s[0] = v->samples[i0];
        t->s[1] = v->samples[si];
        t->s[2] = v->samples[i2];
        t->s[3] = v->samples[i3];
        t->frac = (float)v->phase / 4096.0f;
        t->env  = v->env_level;
        t->active = 1;
    }

    /* Step envelope once per output sample (44.1 kHz). */
    adsr_run(idx, v);
    /* Deactivate once Release fully decays to silence. */
    if (v->adsr_phase == ADSR_RELEASE && v->env_level == 0) {
        v->active = 0;
    }

    uint32_t pitch = voice_reg(idx, 2) & 0x3FFFu;
    if (pitch == 0) pitch = 0x1000u;
    v->phase += pitch;
    while (v->phase >= 0x1000u) {
        v->phase -= 0x1000u;
        v->sample_idx++;
        if (v->sample_idx >= SPU_BLOCK_SAMPLES) break;
    }
    return (int16_t)shaped;
}

static void key_on(uint32_t mask) {
    for (int i = 0; i < SPU_VOICE_COUNT; i++) {
        if (!(mask & (1u << i))) continue;
        SpuVoice *v = &voices[i];
        memset(v, 0, sizeof(*v));
        v->active = 1;
        v->cur_addr = ((uint32_t)voice_reg(i, 3) << 3) & (SPU_RAM_SIZE - 1u);
        v->repeat_addr = ((uint32_t)voice_reg(i, 7) << 3) & (SPU_RAM_SIZE - 1u);
        v->sample_idx = SPU_BLOCK_SAMPLES;
        /* Reset ADSR — KEYON starts envelope at 0 in Attack phase
         * (matches Beetle's PS_SPU::ResetEnvelope). */
        v->env_level = 0;
        v->adsr_divider = 0;
        v->adsr_phase = ADSR_ATTACK;
        key_on_count++;
        endx_latch &= ~(1u << i);  /* KEYON clears ENDX bit on real hw */
        spu_event_record(SPU_EV_KEYON, i, v->cur_addr);
    }
}

/* KEYOFF triggers Release phase, NOT immediate silence. The voice
 * keeps voicing while env_level decays from its current value to 0
 * at the configured Release rate. This is the boot-chime fade tail —
 * silencing immediately is what made channels appear to "cut out". */
static void key_off(uint32_t mask) {
    for (int i = 0; i < SPU_VOICE_COUNT; i++) {
        if (!(mask & (1u << i))) continue;
        if (!voices[i].active) continue;
        spu_event_record(SPU_EV_KEYOFF, i, voices[i].cur_addr);
        voices[i].adsr_phase = ADSR_RELEASE;
        voices[i].adsr_divider = 0;
        /* env_level preserved — release decays from wherever we are now. */
    }
}

void spu_init(void) {
    memset(spu_ram, 0, sizeof(spu_ram));
    memset(spu_regs, 0, sizeof(spu_regs));
    memset(voices, 0, sizeof(voices));
    memset(s_events, 0, sizeof(s_events));
    transfer_addr = 0;
    key_on_count = 0;
    render_frames = 0;
    nonzero_frames = 0;
    last_peak = 0;
    peak = 0;
    endx_latch = 0;
    kon_latch = 0;
    koff_latch = 0;
    s_event_idx = 0;
    s_event_seq = 0;
    spu_cd_audio_reset();
    s_shadow_tap_on = 0;
    s_shadow_tap_frame = 0;
    spu_shadow_reset();
}

void spu_render(int16_t* out_stereo, int frames) {
    if (!out_stereo || frames <= 0) return;

    uint16_t ctrl = spu_regs[reg_index(0x1F801DAAu)];
    int enabled = (ctrl & 0x8000u) != 0;
    int16_t main_l = direct_volume(spu_regs[reg_index(0x1F801D80u)]);
    int16_t main_r = direct_volume(spu_regs[reg_index(0x1F801D82u)]);
    int16_t cd_vol_l = cd_input_volume(spu_regs[reg_index(0x1F801DB0u)]);
    int16_t cd_vol_r = cd_input_volume(spu_regs[reg_index(0x1F801DB2u)]);

    int any_voice = 0;
    if (enabled) {
        for (int v = 0; v < SPU_VOICE_COUNT; v++) {
            if (voices[v].active) { any_voice = 1; break; }
        }
    }

    /* Shadow tap: arm recording for this block if the float SPU shadow is on.
     * Off by default => s_shadow_tap_on stays 0 and the mix loop is unchanged
     * and byte-identical to upstream. */
    s_shadow_tap_on = spu_shadow_enabled() ? 1 : 0;
    s_shadow_tap_frame = 0;
    if (s_shadow_tap_on) {
        int cap = frames < SPU_SHADOW_TAP_FRAMES ? frames : SPU_SHADOW_TAP_FRAMES;
        memset(s_shadow_tap, 0, (size_t)cap * sizeof(s_shadow_tap[0]));
    }

    /* MotK intro FMV: active_mask stays 0 while XA/CD audio plays. The old
     * path still walked 24 idle voices and called audio_trace_pcm(VOICES, 1)
     * per sample (~735 atomics/vblank). Host FPS fell off a cliff when XA
     * started (60 → ~4) with ~67% wall time in phase "other". */
    if (enabled && !any_voice && !s_shadow_tap_on) {
        static int16_t s_voice_silence[2048 * 2];
        int voice_tap_n = frames;
        if (voice_tap_n > 2048) voice_tap_n = 2048;
        memset(s_voice_silence, 0, (size_t)voice_tap_n * 2u * sizeof(int16_t));
        for (int off = 0; off < frames; ) {
            int n = frames - off;
            if (n > 2048) n = 2048;
            audio_trace_pcm(AUDIO_TAP_VOICES, s_voice_silence, n);
            off += n;
        }

        int32_t block_peak = 0;
        int cd_on = (ctrl & 0x0001u) != 0;
        for (int f = 0; f < frames; f++) {
            int32_t mix_l = 0;
            int32_t mix_r = 0;
            if (cd_on) {
                int16_t cd_l = 0;
                int16_t cd_r = 0;
                if (cd_audio_pop(&cd_l, &cd_r)) {
                    mix_l = ((int32_t)cd_l * cd_vol_l) >> 15;
                    mix_r = ((int32_t)cd_r * cd_vol_r) >> 15;
                } else if (cd_push_frames != 0) {
                    cd_underflow_frames++;
                }
            }
            mix_l = (mix_l * main_l) >> 14;
            mix_r = (mix_r * main_r) >> 14;
            out_stereo[f * 2 + 0] = clamp16(mix_l);
            out_stereo[f * 2 + 1] = clamp16(mix_r);
            int32_t frame_peak = abs32(out_stereo[f * 2 + 0]);
            int32_t right_peak = abs32(out_stereo[f * 2 + 1]);
            if (right_peak > frame_peak) frame_peak = right_peak;
            if (frame_peak) nonzero_frames++;
            if (frame_peak > block_peak) block_peak = frame_peak;
        }
        render_frames += (uint64_t)frames;
        last_peak = block_peak;
        if (block_peak > peak) peak = block_peak;
        spu_shadow_process(out_stereo, frames);
        audio_trace_pcm(AUDIO_TAP_SPU_OUT, out_stereo, frames);
        return;
    }

    int32_t block_peak = 0;
    /* Voice-sum tap is filled once per block (not per sample) — same bytes. */
    static int16_t s_voice_sum[2048 * 2];
    int voice_sum_cap = frames < 2048 ? frames : 2048;
    int voice_sum_pos = 0;

    for (int f = 0; f < frames; f++) {
        int32_t mix_l = 0;
        int32_t mix_r = 0;
        int32_t voice_l = 0;
        int32_t voice_r = 0;

        if (enabled) {
            if (any_voice) {
                for (int v = 0; v < SPU_VOICE_COUNT; v++) {
                    int16_t s = voice_next_sample(v);
                    int16_t vl = direct_volume(voice_reg(v, 0));
                    int16_t vr = direct_volume(voice_reg(v, 1));
                    if (s_shadow_tap_on && f < SPU_SHADOW_TAP_FRAMES) {
                        SpuShadowVoiceTap *t = &s_shadow_tap[f].voice[v];
                        t->vol_l = vl;
                        t->vol_r = vr;
                    }
                    if (!s) continue;
                    voice_l += ((int32_t)s * vl) >> 14;
                    voice_r += ((int32_t)s * vr) >> 14;
                }
            }
            mix_l = voice_l;
            mix_r = voice_r;
            if (voice_sum_pos < voice_sum_cap) {
                s_voice_sum[voice_sum_pos * 2 + 0] = clamp16(voice_l);
                s_voice_sum[voice_sum_pos * 2 + 1] = clamp16(voice_r);
                voice_sum_pos++;
            }
            if (ctrl & 0x0001u) {
                int16_t cd_l = 0;
                int16_t cd_r = 0;
                if (cd_audio_pop(&cd_l, &cd_r)) {
                    mix_l += ((int32_t)cd_l * cd_vol_l) >> 15;
                    mix_r += ((int32_t)cd_r * cd_vol_r) >> 15;
                } else if (cd_push_frames != 0) {
                    cd_underflow_frames++;
                }
            }
            mix_l = (mix_l * main_l) >> 14;
            mix_r = (mix_r * main_r) >> 14;
        }

        out_stereo[f * 2 + 0] = clamp16(mix_l);
        out_stereo[f * 2 + 1] = clamp16(mix_r);
        int32_t frame_peak = abs32(out_stereo[f * 2 + 0]);
        int32_t right_peak = abs32(out_stereo[f * 2 + 1]);
        if (right_peak > frame_peak) frame_peak = right_peak;
        if (frame_peak) nonzero_frames++;
        if (frame_peak > block_peak) block_peak = frame_peak;

        if (s_shadow_tap_on && f < SPU_SHADOW_TAP_FRAMES) {
            s_shadow_tap[f].main_l  = main_l;
            s_shadow_tap[f].main_r  = main_r;
            s_shadow_tap[f].enabled = enabled;
            s_shadow_tap_frame = f + 1;
        }
    }
    if (enabled) {
        /* Flush voice-sum tap; if the block was longer than the staging buf,
         * emit the remainder as silence (MotK FMV blocks are ≤2048). */
        if (voice_sum_pos > 0)
            audio_trace_pcm(AUDIO_TAP_VOICES, s_voice_sum, voice_sum_pos);
        for (int left = frames - voice_sum_pos; left > 0; ) {
            static int16_t z[256 * 2];
            int n = left > 256 ? 256 : left;
            memset(z, 0, (size_t)n * 2u * sizeof(int16_t));
            audio_trace_pcm(AUDIO_TAP_VOICES, z, n);
            left -= n;
        }
    }
    render_frames += (uint64_t)frames;
    last_peak = block_peak;
    if (block_peak > peak) peak = block_peak;

    /* Verified-enhancement shadow: re-render this block in float from the
     * tap, verify against the canon mix in `out_stereo`, and substitute only
     * while proven. No-op (byte-identical) when disabled. The canon mix above
     * stays the authoritative output AND the verify oracle. */
    spu_shadow_process(out_stereo, frames);

    /* T1 tap: the SPU's final output block as handed to the host layer
     * (post-shadow, pre host fade/mute). Placed here so every spu_render
     * caller — the vblank pump and the turbo fade tail — is covered. */
    audio_trace_pcm(AUDIO_TAP_SPU_OUT, out_stereo, frames);
}

void spu_debug_info(SpuDebugInfo* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->ctrl = spu_regs[reg_index(0x1F801DAAu)];
    out->main_l = direct_volume(spu_regs[reg_index(0x1F801D80u)]);
    out->main_r = direct_volume(spu_regs[reg_index(0x1F801D82u)]);
    out->cd_l = cd_input_volume(spu_regs[reg_index(0x1F801DB0u)]);
    out->cd_r = cd_input_volume(spu_regs[reg_index(0x1F801DB2u)]);
    for (int i = 0; i < SPU_VOICE_COUNT; i++) {
        if (voices[i].active) out->active_mask |= (1u << i);
    }
    out->key_on_count = key_on_count;
    out->render_frames = render_frames;
    out->nonzero_frames = nonzero_frames;
    out->last_peak = last_peak;
    out->peak = peak;
    out->cd_frames = cd_frame_count;
    out->cd_push_frames = cd_push_frames;
    out->cd_overflow_frames = cd_overflow_frames;
    out->cd_underflow_frames = cd_underflow_frames;
}

uint32_t spu_read(uint32_t addr) {
    if (addr >= 0x1F801C00u && addr <= 0x1F801DFFu) {
        uint32_t idx = reg_index(addr);
        if (idx < SPU_REG_COUNT) {
            if (addr == 0x1F801DAEu) {
                return 0x0400; /* SPUSTAT: ready */
            }
            /* ENDX (end-block-reached latch). Real hw sets bit v when voice
             * v decodes a block whose flag byte has bit 0; KEYON[v] clears
             * it. Without this latch, music engines that poll ENDX to wait
             * for a sample to finish never advance and downstream voices
             * never get keyed on. */
            if (addr == 0x1F801D9Cu) {
                return endx_latch & 0xFFFFu;
            }
            if (addr == 0x1F801D9Eu) {
                return (endx_latch >> 16) & 0xFFu;
            }
            /* Voice register 6 (byte offset 0x0C) is CURVOL / ADSR_LEVEL —
             * returns the live envelope level. PSX music engines poll this
             * to pick a "free" voice (env_level == 0). Without exposing the
             * real envelope, the BIOS sees every voice as silent and over-
             * recycles voices 0-3 instead of fanning across all 24. */
            if (addr >= 0x1F801C00u && addr < 0x1F801D80u
                && (idx & 7u) == 6u) {
                int v = (int)(idx >> 3);
                if (v >= 0 && v < SPU_VOICE_COUNT)
                    return voices[v].env_level;
            }
            return spu_regs[idx];
        }
    }

    return 0;
}

void spu_write(uint32_t addr, uint32_t value) {
    if (addr >= 0x1F801C00u && addr <= 0x1F801DFFu) {
        uint32_t idx = reg_index(addr);
        if (idx < SPU_REG_COUNT) {
            /* Event-ring every register store with its SPU_OUT sample-clock
             * timestamp: this is what lets offline analysis correlate a
             * write (key-on, pitch, volume) with the exact output sample
             * the render loop first honored it at — the write-to-render
             * quantization audit (snesrecomp 8ffc797 class). */
            audio_trace_event(AUDIO_EV_REG_WRITE, addr, value & 0xFFFFu);
            spu_regs[idx] = (uint16_t)value;

            /* Voice repeat/loop address (voice reg 7) is LIVE state on real
             * hardware: writing it after KEYON retargets where the next
             * END+REPEAT block jumps (Beetle spu.cpp:1150/333). X5's driver
             * uses exactly this to end one-shots on looped samples — KEYON,
             * then point the loop register at a silent tail block; no KEYOFF
             * is ever sent. Caching repeat_addr only at KEYON left voices
             * looping the loud sample body forever (measured: 3 stuck voices
             * re-looping ~1400x/s, +11 dB over the oracle, clipping). */
            if (idx < (uint32_t)SPU_VOICE_COUNT * 8u && (idx & 7u) == 7u) {
                int v = (int)(idx >> 3);
                voices[v].repeat_addr =
                    ((uint32_t)(uint16_t)value << 3) & (SPU_RAM_SIZE - 1u);
            }

            if (addr == 0x1F801D88u) {
                kon_latch = (kon_latch & 0xFFFF0000u) | (uint32_t)(uint16_t)value;
                key_on((uint32_t)(uint16_t)value);
            }
            if (addr == 0x1F801D8Au) {
                kon_latch = (kon_latch & 0x0000FFFFu) | ((uint32_t)(uint16_t)value << 16);
                key_on((uint32_t)(uint16_t)value << 16);
            }
            if (addr == 0x1F801D8Cu) {
                koff_latch = (koff_latch & 0xFFFF0000u) | (uint32_t)(uint16_t)value;
                key_off((uint32_t)(uint16_t)value);
            }
            if (addr == 0x1F801D8Eu) {
                koff_latch = (koff_latch & 0x0000FFFFu) | ((uint32_t)(uint16_t)value << 16);
                key_off((uint32_t)(uint16_t)value << 16);
            }

            if (addr == 0x1F801DA6u) {
                transfer_addr = ((uint32_t)(uint16_t)value) << 3;
                if (transfer_addr >= SPU_RAM_SIZE) transfer_addr = 0;
            }

            if (addr == 0x1F801DA8u) {
                if (transfer_addr + 1 < SPU_RAM_SIZE) {
                    spu_ram[transfer_addr]     = (uint8_t)(value & 0xFF);
                    spu_ram[transfer_addr + 1] = (uint8_t)((value >> 8) & 0xFF);
                }
                transfer_addr = (transfer_addr + 2) % SPU_RAM_SIZE;
            }
        }
    }
}

void spu_dma_write(uint32_t word) {
    if (transfer_addr + 3 < SPU_RAM_SIZE) {
        spu_ram[transfer_addr]     = (uint8_t)(word & 0xFF);
        spu_ram[transfer_addr + 1] = (uint8_t)((word >> 8) & 0xFF);
        spu_ram[transfer_addr + 2] = (uint8_t)((word >> 16) & 0xFF);
        spu_ram[transfer_addr + 3] = (uint8_t)((word >> 24) & 0xFF);
    }
    transfer_addr = (transfer_addr + 4) % SPU_RAM_SIZE;
}

int spu_dma_ready(void) {
    return 1;
}

const uint8_t* spu_get_ram(void) {
    return spu_ram;
}

void spu_get_voice_state(int idx, SpuVoiceState* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (idx < 0 || idx >= SPU_VOICE_COUNT) return;
    SpuVoice *v = &voices[idx];
    out->active      = v->active;
    out->vol_ctrl_l  = voice_reg(idx, 0);
    out->vol_ctrl_r  = voice_reg(idx, 1);
    out->pitch       = voice_reg(idx, 2);
    out->start_lo    = voice_reg(idx, 3);
    out->adsr_lo     = voice_reg(idx, 4);
    out->adsr_hi     = voice_reg(idx, 5);
    out->loop_lo     = voice_reg(idx, 7);
    out->cur_addr    = v->cur_addr;
    out->repeat_addr = v->repeat_addr;
    out->last_flags  = v->flags;
    out->sample_idx  = (uint8_t)v->sample_idx;
    out->phase       = (uint16_t)v->phase;
    out->env_level   = v->env_level;
    out->adsr_phase  = v->adsr_phase;
}

/* Debug peek into SPU RAM (spu_ram TCP command). Returns bytes copied. */
uint32_t spu_ram_peek(uint32_t addr, uint8_t *out, uint32_t len) {
    if (!out || addr >= SPU_RAM_SIZE) return 0;
    if (len > SPU_RAM_SIZE - addr) len = SPU_RAM_SIZE - addr;
    memcpy(out, spu_ram + addr, len);
    return len;
}

void spu_get_global_state(SpuGlobalState* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->ctrl       = spu_regs[reg_index(0x1F801DAAu)];
    out->main_vol_l = spu_regs[reg_index(0x1F801D80u)];
    out->main_vol_r = spu_regs[reg_index(0x1F801D82u)];
    out->kon_latch  = kon_latch & 0xFFFFFFu;
    out->koff_latch = koff_latch & 0xFFFFFFu;
    out->pmon = (uint32_t)spu_regs[reg_index(0x1F801D90u)] |
                ((uint32_t)spu_regs[reg_index(0x1F801D92u)] << 16);
    out->non  = (uint32_t)spu_regs[reg_index(0x1F801D94u)] |
                ((uint32_t)spu_regs[reg_index(0x1F801D96u)] << 16);
    out->eon  = (uint32_t)spu_regs[reg_index(0x1F801D98u)] |
                ((uint32_t)spu_regs[reg_index(0x1F801D9Au)] << 16);
    out->endx = endx_latch & 0xFFFFFFu;
    uint32_t am = 0;
    for (int i = 0; i < SPU_VOICE_COUNT; i++)
        if (voices[i].active) am |= (1u << i);
    out->active_mask = am;
}

uint64_t spu_event_total(void) { return s_event_seq; }

uint32_t spu_event_get(SpuEvent* out, uint32_t max_count) {
    if (!out || max_count == 0) return 0;
    uint64_t avail = s_event_seq < (uint64_t)SPU_EVENT_CAP
                     ? s_event_seq : (uint64_t)SPU_EVENT_CAP;
    if ((uint64_t)max_count > avail) max_count = (uint32_t)avail;
    /* Most recent N, oldest first. */
    uint32_t start = (s_event_idx + SPU_EVENT_CAP - max_count) & (SPU_EVENT_CAP - 1u);
    for (uint32_t i = 0; i < max_count; i++) {
        out[i] = s_events[(start + i) & (SPU_EVENT_CAP - 1u)];
    }
    return max_count;
}

void spu_event_reset(void) {
    s_event_idx = 0;
    s_event_seq = 0;
    memset(s_events, 0, sizeof(s_events));
}

/* ---- boot snapshot: complete SPU register + voice state (LE field wire) ---- */
#include "pst_wire.h"

/* SpuVoice host sizeof has padding; wire is packed LE fields (94 bytes). */
#define SPU_VOICE_WIRE_BYTES ( \
    4u + 4u + 4u + (28u * 2u) + (3u * 2u) + 4u + 4u + 2u + 2u + 1u + 2u + 4u + 1u)

static int spu_w_voice(PstW *w, const SpuVoice *v) {
    if (!pst_w_i32(w, (int32_t)v->active) || !pst_w_u32(w, v->cur_addr) ||
        !pst_w_u32(w, v->repeat_addr))
        return 0;
    for (int i = 0; i < SPU_BLOCK_SAMPLES; i++)
        if (!pst_w_i16(w, v->samples[i])) return 0;
    for (int i = 0; i < 3; i++)
        if (!pst_w_i16(w, v->previous_samples[i])) return 0;
    return pst_w_i32(w, (int32_t)v->sample_idx) && pst_w_u32(w, v->phase) &&
           pst_w_i16(w, v->hist1) && pst_w_i16(w, v->hist2) && pst_w_u8(w, v->flags) &&
           pst_w_u16(w, v->env_level) && pst_w_u32(w, v->adsr_divider) &&
           pst_w_u8(w, v->adsr_phase);
}
static int spu_r_voice(PstR *r, SpuVoice *v) {
    int32_t active = 0, sample_idx = 0;
    if (!pst_r_i32(r, &active) || !pst_r_u32(r, &v->cur_addr) ||
        !pst_r_u32(r, &v->repeat_addr))
        return 0;
    v->active = (int)active;
    for (int i = 0; i < SPU_BLOCK_SAMPLES; i++)
        if (!pst_r_i16(r, &v->samples[i])) return 0;
    for (int i = 0; i < 3; i++)
        if (!pst_r_i16(r, &v->previous_samples[i])) return 0;
    if (!pst_r_i32(r, &sample_idx) || !pst_r_u32(r, &v->phase) ||
        !pst_r_i16(r, &v->hist1) || !pst_r_i16(r, &v->hist2) ||
        !pst_r_u8(r, &v->flags) || !pst_r_u16(r, &v->env_level) ||
        !pst_r_u32(r, &v->adsr_divider) || !pst_r_u8(r, &v->adsr_phase))
        return 0;
    v->sample_idx = (int)sample_idx;
    return 1;
}

uint32_t spu_snapshot_bytes(void) {
    return (uint32_t)(SPU_REG_COUNT * 2u) +
           (SPU_VOICE_COUNT * SPU_VOICE_WIRE_BYTES) + 20u;
}

void spu_snapshot_write(uint8_t *p) {
    PstW w;
    uint32_t n = spu_snapshot_bytes();
    pst_w_init(&w, p, n);
    for (uint32_t i = 0; i < SPU_REG_COUNT; i++)
        pst_w_u16(&w, spu_regs[i]);
    for (int i = 0; i < SPU_VOICE_COUNT; i++)
        spu_w_voice(&w, &voices[i]);
    pst_w_u32(&w, transfer_addr);
    pst_w_u32(&w, key_on_count);
    pst_w_u32(&w, endx_latch);
    pst_w_u32(&w, kon_latch);
    pst_w_u32(&w, koff_latch);
}

int spu_snapshot_read(const uint8_t *p, uint32_t len) {
    PstR r;
    if (len != spu_snapshot_bytes()) return 0;
    pst_r_init(&r, p, len);
    for (uint32_t i = 0; i < SPU_REG_COUNT; i++)
        if (!pst_r_u16(&r, &spu_regs[i])) return 0;
    for (int i = 0; i < SPU_VOICE_COUNT; i++)
        if (!spu_r_voice(&r, &voices[i])) return 0;
    if (!pst_r_u32(&r, &transfer_addr) || !pst_r_u32(&r, &key_on_count) ||
        !pst_r_u32(&r, &endx_latch) || !pst_r_u32(&r, &kon_latch) ||
        !pst_r_u32(&r, &koff_latch))
        return 0;
    return 1;
}
uint8_t*  spu_get_ram_ptr(void){ return spu_ram; }
uint32_t  spu_get_ram_bytes(void){ return (uint32_t)sizeof(spu_ram); }
