/*
 * spu.c - PS1 Sound Processing Unit register and ADPCM voice/DSP model.
 *
 * A compact hardware model: SPU register reads/writes, DMA4 transfers in
 * both directions over 512KB SPU RAM, the 24 ADPCM voices with pitch,
 * Gaussian interpolation and ADSR envelopes, decoded CD/XA audio on the SPU
 * CD input bus, the hardware noise generator (NON voices), volume sweep
 * envelopes (per-voice and main L/R), the 22050 Hz reverb engine with its
 * SPU-RAM work area, the CD/voice capture buffers at 0x000..0xFFF, and the
 * SPU RAM IRQ address compare (I_STAT bit 9, SPUSTAT bit 6).
 *
 * The reverb/noise/sweep/IRQ model is a CLEAN-ROOM implementation from
 * hardware documentation only (psx-spx register map + documented algorithm);
 * no emulator source was consulted. Points where the documentation is silent
 * are flagged inline with "DOCUMENTED-GAP" comments so the Beetle runtime
 * oracle comparison can quantify them from OUTPUT, not source.
 */

#include "spu.h"
#include "spu_gauss.h"
#include "spu_shadow.h"
#include "audio_trace.h"
#include "crc32.h"
#include "psx_cycles.h"

#include <stdio.h>
#include <stdlib.h>
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

/* ---- SPU IRQ (I_STAT bit 9) ------------------------------------------- */
/* Register 0x1F801DA4 holds the IRQ address in 8-byte units (value << 3 =
 * byte address). While SPUCNT bit 6 is set, ANY SPU RAM access touching
 * that 8-byte unit latches irq_flag (mirrored into SPUSTAT bit 6) and
 * raises interrupt 9. Writing SPUCNT with bit 6 CLEAR acknowledges the
 * latch and re-arms it. The raise is edge-triggered on the latch's 0->1
 * transition: while the latch is set, further hits do not re-raise. */
extern void psx_irq_raise(uint32_t bit, uint32_t detail);
static uint8_t irq_flag;

/* ---- Noise generator ---------------------------------------------------
 * 16-bit LFSR clocked from SPUCNT bits 8-13 (bits 8-9 = frequency STEP
 * 4..7, bits 10-13 = frequency SHIFT 0..15). See noise_clock() for the
 * divider derivation. Voices with their NON bit set (0x1F801D94/96) output
 * the live LFSR value in place of their interpolated ADPCM sample. */
static uint16_t noise_lfsr;   /* self-seeds from 0 via the xor-1 parity term */
static int32_t  noise_timer;

/* ---- Reverb engine state ------------------------------------------------
 * The engine runs at 22050 Hz (one step per TWO 44100 Hz output frames).
 * rev_cur is the current buffer address (absolute byte address inside the
 * work area [mBASE<<3, 0x80000)). The remaining fields carry the 44.1<->22.05
 * kHz boundary state: the held first-of-pair input frame and the last engine
 * output used for reconstruction (see rev_reconstruct). */
static uint32_t rev_cur;
static uint8_t  rev_phase;        /* 0 = first frame of a 2-frame pair */
static int32_t  rev_in_hold_l;
static int32_t  rev_in_hold_r;
static int32_t  rev_out_l;        /* most recent engine output (post vLOUT/vROUT) */
static int32_t  rev_out_r;

/* ---- Capture buffers ----------------------------------------------------
 * Real SPU RAM behaviour: every 44100 Hz sample cycle the SPU writes the CD
 * input L to 0x000+pos, CD input R to 0x400+pos, voice 1 post-envelope
 * output to 0x800+pos and voice 3 post-envelope output to 0xC00+pos; pos
 * advances one halfword and wraps at 0x400. Games may park the IRQ address
 * inside these rings, so each write runs the IRQ check. */
static uint32_t capture_pos;

/* ---- Volume sweep envelopes ---------------------------------------------
 * Every volume register (24 voices x L/R + main L/R) is either DIRECT
 * (bit15=0: effective volume = signed bits14-0 << 1) or a live SWEEP
 * envelope (bit15=1) stepped once per 44100 Hz sample with the same rate
 * machinery as ADSR (calc_vc_delta). `level` is the authoritative current
 * volume in sweep mode and is refreshed from the register on direct writes,
 * so a later switch to sweep mode glides from the last direct level. */
typedef struct {
    int16_t  level;      /* live effective volume, full signed 16-bit */
    uint32_t divider;    /* rate divider, same overflow scheme as ADSR */
} SweepEnv;
static SweepEnv sweep_voice_env[SPU_VOICE_COUNT][2];  /* [voice][0=L 1=R] */
static SweepEnv sweep_main_env[2];                    /* [0=L 1=R] */

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
    e->addr     = addr;
    if (voice >= 0 && voice < SPU_VOICE_COUNT) {
        /* PSX voice block layout (16-bit register indices from voice base):
         *   0=VOL_L 1=VOL_R 2=PITCH 3=START 4=ADSR_LO 5=ADSR_HI 6=CURVOL 7=LOOP */
        e->pitch    = spu_regs[(uint32_t)voice * 8u + 2u];
        e->adsr_lo  = spu_regs[(uint32_t)voice * 8u + 4u];
        e->adsr_hi  = spu_regs[(uint32_t)voice * 8u + 5u];
        e->vol_l    = spu_regs[(uint32_t)voice * 8u + 0u];
        e->vol_r    = spu_regs[(uint32_t)voice * 8u + 1u];
    } else {
        /* Non-voice-attributable events (e.g. SPU_EV_IRQ, voice=0xFF). */
        e->pitch   = 0;
        e->adsr_lo = 0;
        e->adsr_hi = 0;
        e->vol_l   = 0;
        e->vol_r   = 0;
    }
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

/* Register shorthand: current live value of a named SPU register. */
#define RREG(a) (spu_regs[reg_index(a)])
#define RVOL(a) ((int16_t)RREG(a))

/* Direct-mode volume decode (bit15=0): bits 14-0 are a signed value and the
 * effective 16-bit volume is that value << 1 — bit 14 lands on the int16
 * sign bit, so sign extension falls out of the shift for free. Effective
 * volumes are applied as (sample * vol) >> 15 throughout, which for direct
 * registers is bit-identical to the previous (15-bit vol) >> 14 decode. */
static inline int16_t volume_reg_decode(uint16_t raw) {
    return (int16_t)((uint16_t)(raw << 1));
}

/* Called on every guest write to a volume register. Direct writes take
 * effect immediately; a sweep-mode write starts the sweep FROM the current
 * live level (documented reading: the sweep register programs an envelope,
 * it does not itself carry a target level). The divider restarts either way. */
static void sweep_env_write(SweepEnv *sw, uint16_t raw) {
    if (!(raw & 0x8000u))
        sw->level = volume_reg_decode(raw);
    sw->divider = 0;
}

/* Step one sweep envelope by one 44100 Hz output sample. No-op for
 * direct-mode registers. Sweep register layout (bit15=1):
 *   bit14   mode      0=linear 1=exponential
 *   bit13   direction 0=increase 1=decrease
 *   bit12   phase     0=positive 1=negative
 *   bit6-0  rate      (bits 0-1 step, bits 2-6 shift) — same 7-bit rate
 *                     format as ADSR, so calc_vc_delta is reused verbatim.
 *
 * DOCUMENTED-GAP: the documentation does not spell out how the "phase"
 * bit interacts with a level whose sign disagrees with it. Model chosen:
 * the envelope machinery always operates on a 0..0x7FFF working value in
 * the phase's domain (negative phase mirrors the level), and a level on the
 * wrong side of zero is clamped to 0 before stepping. Increase saturates at
 * 0x7FFF, decrease at 0, matching the ADSR clamp behaviour. Candidate for
 * oracle verification. */
static void sweep_env_step(SweepEnv *sw, uint16_t raw) {
    if (!(raw & 0x8000u)) return;
    int     exp_mode  = (raw >> 14) & 1;
    int     dec_mode  = (raw >> 13) & 1;
    int     neg_phase = (raw >> 12) & 1;
    uint8_t rate      = (uint8_t)(raw & 0x7F);

    int32_t working = sw->level;
    if (neg_phase) working = -working;
    if (working < 0) working = 0;
    if (working > 0x7FFF) working = 0x7FFF;

    int increment = 0, divinco = 0;
    calc_vc_delta(0x7F, rate, exp_mode, dec_mode, dec_mode,
                  (int16_t)working, &increment, &divinco);

    sw->divider += (uint32_t)divinco;
    if (sw->divider & 0x8000u) {
        sw->divider = 0;
        working += increment;
        if (working < 0) working = 0;
        if (working > 0x7FFF) working = 0x7FFF;
        sw->level = (int16_t)(neg_phase ? -working : working);
    }
}

/* Live effective volume of a volume register: the register decode in direct
 * mode, the sweep envelope's current level in sweep mode. */
static inline int16_t chan_volume(uint16_t raw, const SweepEnv *sw) {
    if (raw & 0x8000u) return sw->level;
    return volume_reg_decode(raw);
}

/* SPU RAM IRQ-address compare, called from EVERY SPU RAM access site (FIFO,
 * DMA both directions, ADPCM block fetch, capture-buffer writes, reverb
 * work-area reads/writes, and the transfer/IRQ-address/SPUCNT register
 * writes). `addr`/`len` describe the accessed byte range; the compare is at
 * 8-byte-unit granularity because that is the IRQ address register's unit. */
static void spu_irq_check(uint32_t addr, uint32_t len) {
    if (!(RREG(0x1F801DAAu) & 0x0040u)) return;   /* SPUCNT.6 IRQ enable */
    if (len == 0) return;
    uint32_t target = RREG(0x1F801DA4u);          /* in 8-byte units */
    uint32_t a = addr & (SPU_RAM_SIZE - 1u);
    uint32_t first = a >> 3;
    uint32_t last  = (a + len - 1u) >> 3;
    for (uint32_t u = first; u <= last; u++) {
        if ((u & 0xFFFFu) == target) {
            if (!irq_flag) {
                irq_flag = 1;
                psx_irq_raise(9u, u << 3);
                spu_event_record(SPU_EV_IRQ, 0xFF, u << 3);
            }
            return;
        }
    }
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

    /* The 16-byte ADPCM block fetch is an SPU RAM access: IRQ-address games
     * (CD streaming double-buffers, MDEC audio) park the IRQ inside a voice's
     * sample buffer and rely on this exact compare firing. */
    spu_irq_check(addr, 16u);

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

/* ---- Noise generator ---------------------------------------------------- */
/* Documented LFSR update: the new low bit is the parity term
 * (bit15 XOR bit12 XOR bit11 XOR bit10 XOR 1) and the register shifts left.
 * The XOR-with-1 makes the all-zero state self-starting.
 *
 * Clock rate derivation from SPUCNT bits 8-13 (step = 4 + bits8-9,
 * shift = bits10-13): a down-counter is decremented by `step` once per
 * 44100 Hz sample cycle and the LFSR clocks on underflow, whereupon the
 * counter is topped up by (0x20000 >> shift) — reloading twice if a single
 * reload does not clear the deficit (only reachable at the largest shifts).
 * shift=0 gives the slowest clock (lowest frequency), shift=15 the fastest,
 * and the 4..7 step scales the rate within a shift octave.
 *
 * DOCUMENTED-GAP: this divider wiring is the least precisely documented part
 * of the noise unit; the shift/step semantics above are the documented
 * reading, but the exact counter width/reload behaviour is a candidate for
 * oracle verification against Beetle's audio OUTPUT. */
static void noise_clock(uint16_t ctrl) {
    int     step   = 4 + ((ctrl >> 8) & 3);
    int     shift  = (ctrl >> 10) & 0x0F;
    int32_t reload = (int32_t)(0x20000u >> shift);
    noise_timer -= step;
    if (noise_timer < 0) {
        uint16_t parity = (uint16_t)(((noise_lfsr >> 15) ^ (noise_lfsr >> 12) ^
                                      (noise_lfsr >> 11) ^ (noise_lfsr >> 10) ^ 1u) & 1u);
        noise_lfsr = (uint16_t)((noise_lfsr << 1) | parity);
        noise_timer += reload;
        if (noise_timer < 0) noise_timer += reload;  /* worst case: step 7, reload 4 */
    }
}

/* ---- Capture buffers ----------------------------------------------------- */
static void capture_write(uint32_t base, int16_t s) {
    uint32_t a = (base + capture_pos) & (SPU_RAM_SIZE - 1u);
    spu_irq_check(a, 2u);
    spu_ram[a]      = (uint8_t)((uint16_t)s & 0xFFu);
    spu_ram[a + 1u] = (uint8_t)(((uint16_t)s >> 8) & 0xFFu);
}

/* ---- Reverb engine -------------------------------------------------------
 * Register map (psx-spx). All address/offset registers are in units of 8
 * bytes; all volumes are signed Q0.15 (divide by 32768). */
#define SPU_R_VLOUT   0x1F801D84u  /* reverb output volume L */
#define SPU_R_VROUT   0x1F801D86u  /* reverb output volume R */
#define SPU_R_MBASE   0x1F801DA2u  /* work area start address */
#define SPU_R_DAPF1   0x1F801DC0u  /* APF offset 1 */
#define SPU_R_DAPF2   0x1F801DC2u  /* APF offset 2 */
#define SPU_R_VIIR    0x1F801DC4u  /* reflection volume 1 */
#define SPU_R_VCOMB1  0x1F801DC6u  /* comb volume 1 */
#define SPU_R_VCOMB2  0x1F801DC8u  /* comb volume 2 */
#define SPU_R_VCOMB3  0x1F801DCAu  /* comb volume 3 */
#define SPU_R_VCOMB4  0x1F801DCCu  /* comb volume 4 */
#define SPU_R_VWALL   0x1F801DCEu  /* reflection volume 2 */
#define SPU_R_VAPF1   0x1F801DD0u  /* APF volume 1 */
#define SPU_R_VAPF2   0x1F801DD2u  /* APF volume 2 */
#define SPU_R_MLSAME  0x1F801DD4u  /* same-side reflection addr 1 L (src/dst) */
#define SPU_R_MRSAME  0x1F801DD6u  /* same-side reflection addr 1 R (src/dst) */
#define SPU_R_MLCOMB1 0x1F801DD8u  /* comb addr 1 L (src) */
#define SPU_R_MRCOMB1 0x1F801DDAu  /* comb addr 1 R (src) */
#define SPU_R_MLCOMB2 0x1F801DDCu  /* comb addr 2 L (src) */
#define SPU_R_MRCOMB2 0x1F801DDEu  /* comb addr 2 R (src) */
#define SPU_R_DLSAME  0x1F801DE0u  /* same-side reflection addr 2 L (src) */
#define SPU_R_DRSAME  0x1F801DE2u  /* same-side reflection addr 2 R (src) */
#define SPU_R_MLDIFF  0x1F801DE4u  /* diff-side reflection addr 1 L (src/dst) */
#define SPU_R_MRDIFF  0x1F801DE6u  /* diff-side reflection addr 1 R (src/dst) */
#define SPU_R_MLCOMB3 0x1F801DE8u  /* comb addr 3 L (src) */
#define SPU_R_MRCOMB3 0x1F801DEAu  /* comb addr 3 R (src) */
#define SPU_R_MLCOMB4 0x1F801DECu  /* comb addr 4 L (src) */
#define SPU_R_MRCOMB4 0x1F801DEEu  /* comb addr 4 R (src) */
#define SPU_R_DLDIFF  0x1F801DF0u  /* diff-side reflection addr 2 L (src) */
#define SPU_R_DRDIFF  0x1F801DF2u  /* diff-side reflection addr 2 R (src) */
#define SPU_R_MLAPF1  0x1F801DF4u  /* APF addr 1 L (src/dst) */
#define SPU_R_MRAPF1  0x1F801DF6u  /* APF addr 1 R (src/dst) */
#define SPU_R_MLAPF2  0x1F801DF8u  /* APF addr 2 L (src/dst) */
#define SPU_R_MRAPF2  0x1F801DFAu  /* APF addr 2 R (src/dst) */
#define SPU_R_VLIN    0x1F801DFCu  /* reverb input volume L */
#define SPU_R_VRIN    0x1F801DFEu  /* reverb input volume R */

/* Signed Q0.15 volume multiply (the reverb pipeline's only multiply shape). */
static inline int32_t rev_mul(int16_t vol, int32_t s) {
    return (int32_t)(((int64_t)vol * (int64_t)s) >> 15);
}

static inline uint32_t rev_mbase(void) {
    return (((uint32_t)RREG(SPU_R_MBASE)) << 3) & 0x7FFFEu;
}

/* Effective work-area address for an address register plus a byte bias
 * (bias -2 implements the documented [x-2] taps, bias -dAPFn*8 the APF
 * delay taps). A register value r denotes byte offset r*8 from the current
 * buffer address; the result wraps inside [mBASE<<3, 0x80000) and never
 * escapes it, whatever the register values. */
static uint32_t rev_ea(uint32_t reg_addr, int32_t bias) {
    uint32_t base = rev_mbase();
    uint32_t size = SPU_RAM_SIZE - base;
    int64_t  rel  = (int64_t)rev_cur - (int64_t)base
                  + ((int64_t)RREG(reg_addr) << 3) + (int64_t)bias;
    rel %= (int64_t)size;
    if (rel < 0) rel += (int64_t)size;
    return (base + (uint32_t)rel) & 0x7FFFEu;
}

static int16_t rev_read(uint32_t reg_addr, int32_t bias) {
    uint32_t a = rev_ea(reg_addr, bias);
    spu_irq_check(a, 2u);
    return (int16_t)((uint16_t)spu_ram[a] | ((uint16_t)spu_ram[a + 1u] << 8));
}

/* Work-area store, saturated to signed 16-bit. Writes (and their IRQ
 * checks) happen only while SPUCNT bit 7 is set; reads always occur.
 * DOCUMENTED-GAP: whether a SUPPRESSED write still drives the address bus
 * (and could therefore trip the IRQ compare) is not documented; this model
 * only checks on accesses that actually happen. */
static void rev_write(uint32_t reg_addr, int32_t v, int wren) {
    if (!wren) return;
    uint32_t a = rev_ea(reg_addr, 0);
    int16_t  s = clamp16(v);
    spu_irq_check(a, 2u);
    spu_ram[a]      = (uint8_t)((uint16_t)s & 0xFFu);
    spu_ram[a + 1u] = (uint8_t)(((uint16_t)s >> 8) & 0xFFu);
}

/* One documented reverb step at 22050 Hz. `in_l`/`in_r` are the reverb-send
 * mix (voices with EON set + CD audio if SPUCNT bits 0 and 2). Leaves the
 * vLOUT/vROUT-scaled result in rev_out_l/rev_out_r and advances the buffer
 * address. RAM accesses are performed in the documented statement order so
 * the dAPF1==0 / dAPF2==0 corner (re-read of a just-written cell) behaves
 * as literally written. */
static void reverb_step(int32_t in_l, int32_t in_r, int wren) {
    int32_t dapf1 = -((int32_t)RREG(SPU_R_DAPF1) << 3);
    int32_t dapf2 = -((int32_t)RREG(SPU_R_DAPF2) << 3);
    int16_t vIIR  = RVOL(SPU_R_VIIR);
    int16_t vWALL = RVOL(SPU_R_VWALL);
    int16_t vAPF1 = RVOL(SPU_R_VAPF1);
    int16_t vAPF2 = RVOL(SPU_R_VAPF2);

    /* Input from the mixer. */
    int32_t Lin = rev_mul(RVOL(SPU_R_VLIN), in_l);
    int32_t Rin = rev_mul(RVOL(SPU_R_VRIN), in_r);

    /* Same-side reflection (L->L, R->R):
     * [mLSAME] = (Lin + [dLSAME]*vWALL - [mLSAME-2])*vIIR + [mLSAME-2] */
    {
        int32_t prev_l = rev_read(SPU_R_MLSAME, -2);
        rev_write(SPU_R_MLSAME,
                  rev_mul(vIIR, Lin + rev_mul(vWALL, rev_read(SPU_R_DLSAME, 0))
                                - prev_l) + prev_l,
                  wren);
        int32_t prev_r = rev_read(SPU_R_MRSAME, -2);
        rev_write(SPU_R_MRSAME,
                  rev_mul(vIIR, Rin + rev_mul(vWALL, rev_read(SPU_R_DRSAME, 0))
                                - prev_r) + prev_r,
                  wren);
    }

    /* Different-side reflection (L->R, R->L) — note the CROSSED d*DIFF
     * sources: mLDIFF takes dRDIFF, mRDIFF takes dLDIFF. */
    {
        int32_t prev_l = rev_read(SPU_R_MLDIFF, -2);
        rev_write(SPU_R_MLDIFF,
                  rev_mul(vIIR, Lin + rev_mul(vWALL, rev_read(SPU_R_DRDIFF, 0))
                                - prev_l) + prev_l,
                  wren);
        int32_t prev_r = rev_read(SPU_R_MRDIFF, -2);
        rev_write(SPU_R_MRDIFF,
                  rev_mul(vIIR, Rin + rev_mul(vWALL, rev_read(SPU_R_DLDIFF, 0))
                                - prev_r) + prev_r,
                  wren);
    }

    /* Early echo: 4-tap comb filter. */
    int32_t lout = rev_mul(RVOL(SPU_R_VCOMB1), rev_read(SPU_R_MLCOMB1, 0))
                 + rev_mul(RVOL(SPU_R_VCOMB2), rev_read(SPU_R_MLCOMB2, 0))
                 + rev_mul(RVOL(SPU_R_VCOMB3), rev_read(SPU_R_MLCOMB3, 0))
                 + rev_mul(RVOL(SPU_R_VCOMB4), rev_read(SPU_R_MLCOMB4, 0));
    int32_t rout = rev_mul(RVOL(SPU_R_VCOMB1), rev_read(SPU_R_MRCOMB1, 0))
                 + rev_mul(RVOL(SPU_R_VCOMB2), rev_read(SPU_R_MRCOMB2, 0))
                 + rev_mul(RVOL(SPU_R_VCOMB3), rev_read(SPU_R_MRCOMB3, 0))
                 + rev_mul(RVOL(SPU_R_VCOMB4), rev_read(SPU_R_MRCOMB4, 0));

    /* Late reverb all-pass filter 1:
     * Lout = Lout - vAPF1*[mLAPF1-dAPF1]; [mLAPF1] = Lout;
     * Lout = Lout*vAPF1 + [mLAPF1-dAPF1] */
    lout = lout - rev_mul(vAPF1, rev_read(SPU_R_MLAPF1, dapf1));
    rev_write(SPU_R_MLAPF1, lout, wren);
    lout = rev_mul(vAPF1, clamp16(lout)) + rev_read(SPU_R_MLAPF1, dapf1);
    rout = rout - rev_mul(vAPF1, rev_read(SPU_R_MRAPF1, dapf1));
    rev_write(SPU_R_MRAPF1, rout, wren);
    rout = rev_mul(vAPF1, clamp16(rout)) + rev_read(SPU_R_MRAPF1, dapf1);

    /* Late reverb all-pass filter 2. */
    lout = lout - rev_mul(vAPF2, rev_read(SPU_R_MLAPF2, dapf2));
    rev_write(SPU_R_MLAPF2, lout, wren);
    lout = rev_mul(vAPF2, clamp16(lout)) + rev_read(SPU_R_MLAPF2, dapf2);
    rout = rout - rev_mul(vAPF2, rev_read(SPU_R_MRAPF2, dapf2));
    rev_write(SPU_R_MRAPF2, rout, wren);
    rout = rev_mul(vAPF2, clamp16(rout)) + rev_read(SPU_R_MRAPF2, dapf2);

    /* Output to mixer. */
    rev_out_l = rev_mul(RVOL(SPU_R_VLOUT), clamp16(lout));
    rev_out_r = rev_mul(RVOL(SPU_R_VROUT), clamp16(rout));

    /* Documented advance: BufferAddress = MAX(mBASE, (BufferAddress+2) AND 7FFFEh) */
    {
        uint32_t base = rev_mbase();
        rev_cur = (rev_cur + 2u) & 0x7FFFEu;
        if (rev_cur < base) rev_cur = base;
    }
}

/* ⚠ DOCUMENTED-GAP — the ONE known deliberate deviation from hardware.
 * The reverb engine runs at 22050 Hz but the real SPU's 22.05 -> 44.1 kHz
 * reconstruction filter (and the matching 44.1 -> 22.05 kHz input
 * decimation) is NOT specified in the documentation this implementation is
 * built from, and consulting an emulator's filter is off-limits for license
 * reasons. Chosen documented-behaviour stand-ins, both trivially swappable:
 *   - input:  box decimation (average of the two 44.1 kHz frames per step),
 *   - output: this function — linear interpolation between successive
 *             22050 Hz engine results (even output frame = the step result,
 *             odd frame = midpoint of the two neighbouring results).
 * A later Beetle-output oracle comparison should quantify the spectral
 * difference; replace ONLY this function (and the input average at the
 * call site in spu_render) if a better-documented filter emerges. */
static inline int32_t rev_reconstruct(int32_t prev_step, int32_t cur_step) {
    return (prev_step + cur_step) >> 1;
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
    int16_t  vol_l;    /* per-voice volume, 1.14 scale (effective int16 >> 1) */
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

    /* Noise mode (NON bit set): the voice outputs the live noise LFSR value
     * INSTEAD of its interpolated ADPCM sample. Everything else — block
     * decoding/address advance, ENDX latching, pitch stepping, the ADSR
     * envelope and the volume stages — behaves exactly as for ADPCM. */
    int16_t raw_s;
    uint32_t non_mask = (uint32_t)spu_regs[reg_index(0x1F801D94u)]
                      | ((uint32_t)spu_regs[reg_index(0x1F801D96u)] << 16);
    if (non_mask & (1u << idx)) {
        raw_s = (int16_t)noise_lfsr;
    } else {
        raw_s = spu_gaussian_interpolate(v->previous_samples,
                                         v->samples,
                                         v->sample_idx,
                                         v->phase);
    }
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
    irq_flag = 0;
    /* Hardware power-on LFSR value is undocumented; 0 is safe because the
     * xor-1 parity term self-starts the register within 16 clocks. */
    noise_lfsr = 0;
    noise_timer = 0;
    rev_cur = 0;
    rev_phase = 0;
    rev_in_hold_l = 0;
    rev_in_hold_r = 0;
    rev_out_l = 0;
    rev_out_r = 0;
    capture_pos = 0;
    memset(sweep_voice_env, 0, sizeof(sweep_voice_env));
    memset(sweep_main_env, 0, sizeof(sweep_main_env));
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
    int enabled  = (ctrl & 0x8000u) != 0;
    int cd_on    = (ctrl & 0x0001u) != 0;
    int cd_rev   = cd_on && (ctrl & 0x0004u) != 0;  /* CD reverb send needs CD enable */
    int rev_wren = (ctrl & 0x0080u) != 0;           /* reverb work-area write enable */
    int16_t cd_vol_l = cd_input_volume(spu_regs[reg_index(0x1F801DB0u)]);
    int16_t cd_vol_r = cd_input_volume(spu_regs[reg_index(0x1F801DB2u)]);
    uint32_t eon = (uint32_t)spu_regs[reg_index(0x1F801D98u)]
                 | ((uint32_t)spu_regs[reg_index(0x1F801D9Au)] << 16);

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

    /* MotK intro FMV (and other XA-only scenes): active_mask stays 0 while
     * CD/XA plays. Keep capture / noise / reverb / main sweeps (issue #103
     * fidelity — FMV is exactly the CD-with-reverb case) but skip the
     * 24-voice walk and per-voice sweep steps. Shadow tap stays on the
     * general path. */
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
        for (int f = 0; f < frames; f++) {
            int32_t mix_l = 0;
            int32_t mix_r = 0;
            int32_t rev_send_l = 0;
            int32_t rev_send_r = 0;
            int16_t main_l = chan_volume(spu_regs[reg_index(0x1F801D80u)],
                                        &sweep_main_env[0]);
            int16_t main_r = chan_volume(spu_regs[reg_index(0x1F801D82u)],
                                        &sweep_main_env[1]);

            sweep_env_step(&sweep_main_env[0],
                           spu_regs[reg_index(0x1F801D80u)]);
            sweep_env_step(&sweep_main_env[1],
                           spu_regs[reg_index(0x1F801D82u)]);

            int16_t cd_l = 0;
            int16_t cd_r = 0;
            if (cd_audio_pop(&cd_l, &cd_r)) {
                /* got a frame */
            } else if (cd_on && cd_push_frames != 0) {
                cd_underflow_frames++;
            }
            if (cd_on) {
                int32_t ccl = ((int32_t)cd_l * cd_vol_l) >> 15;
                int32_t ccr = ((int32_t)cd_r * cd_vol_r) >> 15;
                mix_l += ccl;
                mix_r += ccr;
                if (cd_rev) {
                    rev_send_l += ccl;
                    rev_send_r += ccr;
                }
            }

            /* No active voices → capture voice 1/3 slots stay 0. */
            capture_write(0x0000u, cd_l);
            capture_write(0x0400u, cd_r);
            capture_write(0x0800u, 0);
            capture_write(0x0C00u, 0);
            capture_pos = (capture_pos + 2u) & 0x3FFu;

            noise_clock(ctrl);

            {
                int32_t wet_l, wet_r;
                if (rev_phase == 0) {
                    rev_in_hold_l = rev_send_l;
                    rev_in_hold_r = rev_send_r;
                    wet_l = rev_out_l;
                    wet_r = rev_out_r;
                    rev_phase = 1;
                } else {
                    int32_t prev_l = rev_out_l;
                    int32_t prev_r = rev_out_r;
                    reverb_step(clamp16((rev_in_hold_l + rev_send_l) >> 1),
                                clamp16((rev_in_hold_r + rev_send_r) >> 1),
                                rev_wren);
                    wet_l = rev_reconstruct(prev_l, rev_out_l);
                    wet_r = rev_reconstruct(prev_r, rev_out_r);
                    rev_phase = 0;
                }
                mix_l += wet_l;
                mix_r += wet_r;
            }

            mix_l = clamp16(mix_l);
            mix_r = clamp16(mix_r);
            mix_l = ((int32_t)mix_l * main_l) >> 15;
            mix_r = ((int32_t)mix_r * main_r) >> 15;

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
        /* Main L/R volumes are sweep-aware and can glide per sample. Kept
         * live-per-frame; the >>1 below preserves the shadow tap's historical
         * 1.14 volume scale (identical bytes for direct-mode registers). */
        int16_t main_l = chan_volume(spu_regs[reg_index(0x1F801D80u)], &sweep_main_env[0]);
        int16_t main_r = chan_volume(spu_regs[reg_index(0x1F801D82u)], &sweep_main_env[1]);

        if (enabled) {
            int32_t voice_l = 0;
            int32_t voice_r = 0;
            int32_t rev_send_l = 0;
            int32_t rev_send_r = 0;
            int16_t v1_out = 0;   /* voice 1 post-envelope output (capture) */
            int16_t v3_out = 0;   /* voice 3 post-envelope output (capture) */

            /* Volume sweeps step once per 44100 Hz sample on the same rate
             * machinery as ADSR; no-ops for direct-mode registers. Voice
             * sweeps only matter while a voice is active. */
            sweep_env_step(&sweep_main_env[0], spu_regs[reg_index(0x1F801D80u)]);
            sweep_env_step(&sweep_main_env[1], spu_regs[reg_index(0x1F801D82u)]);
            if (any_voice) {
                for (int v = 0; v < SPU_VOICE_COUNT; v++) {
                    sweep_env_step(&sweep_voice_env[v][0], voice_reg(v, 0));
                    sweep_env_step(&sweep_voice_env[v][1], voice_reg(v, 1));
                }
            }

            if (any_voice) {
                for (int v = 0; v < SPU_VOICE_COUNT; v++) {
                    int16_t s = voice_next_sample(v);
                    if (v == 1) v1_out = s;
                    if (v == 3) v3_out = s;
                    int16_t vl = chan_volume(voice_reg(v, 0), &sweep_voice_env[v][0]);
                    int16_t vr = chan_volume(voice_reg(v, 1), &sweep_voice_env[v][1]);
                    if (s_shadow_tap_on && f < SPU_SHADOW_TAP_FRAMES) {
                        SpuShadowVoiceTap *t = &s_shadow_tap[f].voice[v];
                        /* Tap keeps its historical 1.14 volume scale. */
                        t->vol_l = (int16_t)(vl >> 1);
                        t->vol_r = (int16_t)(vr >> 1);
                    }
                    if (!s) continue;
                    int32_t cl = ((int32_t)s * vl) >> 15;
                    int32_t cr = ((int32_t)s * vr) >> 15;
                    voice_l += cl;
                    voice_r += cr;
                    /* Per-voice reverb send: EON voices feed the reverb input
                     * bus with their post-envelope, post-voice-volume output. */
                    if (eon & (1u << v)) {
                        rev_send_l += cl;
                        rev_send_r += cr;
                    }
                }
            }
            mix_l = voice_l;
            mix_r = voice_r;
            if (voice_sum_pos < voice_sum_cap) {
                s_voice_sum[voice_sum_pos * 2 + 0] = clamp16(voice_l);
                s_voice_sum[voice_sum_pos * 2 + 1] = clamp16(voice_r);
                voice_sum_pos++;
            }

            /* CD input bus. The bus runs continuously while the SPU is
             * enabled (the capture buffers record it regardless of SPUCNT
             * bit 0); bit 0 only gates its contribution to the mix, and
             * bit 2 (with bit 0) its reverb send. */
            int16_t cd_l = 0;
            int16_t cd_r = 0;
            if (cd_audio_pop(&cd_l, &cd_r)) {
                /* got a frame */
            } else if (cd_on && cd_push_frames != 0) {
                cd_underflow_frames++;
            }
            if (cd_on) {
                int32_t ccl = ((int32_t)cd_l * cd_vol_l) >> 15;
                int32_t ccr = ((int32_t)cd_r * cd_vol_r) >> 15;
                mix_l += ccl;
                mix_r += ccr;
                if (cd_rev) {
                    rev_send_l += ccl;
                    rev_send_r += ccr;
                }
            }

            /* Capture buffers: CD input L/R at 0x000/0x400, voice 1/3
             * post-envelope output at 0x800/0xC00; one halfword per 44100 Hz
             * sample, wrapping every 0x400 bytes. Each store runs the IRQ
             * check — games park the IRQ address here. The CD samples are
             * recorded PRE CD-volume (the raw input bus).
             * DOCUMENTED-GAP: whether the CD capture value is pre- or
             * post-CD-volume is not settled by the documentation; the raw
             * input-bus reading was chosen. Candidate for oracle check. */
            capture_write(0x0000u, cd_l);
            capture_write(0x0400u, cd_r);
            capture_write(0x0800u, v1_out);
            capture_write(0x0C00u, v3_out);
            capture_pos = (capture_pos + 2u) & 0x3FFu;

            /* Noise LFSR clock: free-running at the SPUCNT bits 8-13 rate,
             * once per output sample cycle. Clocked AFTER the voice walk, so
             * noise voices see the value from the previous cycle (whether
             * hardware clocks before or after voice processing within the
             * sample cycle is undocumented). */
            noise_clock(ctrl);

            /* Reverb: the engine runs one step per TWO output frames
             * (22050 Hz). The engine always steps while the SPU is enabled —
             * SPUCNT bit 7 gates only its work-area WRITES (reads, IRQ
             * checks on those reads, address advance and output still
             * happen), so a frozen work area keeps ringing through vLOUT
             * until the game silences it. See rev_reconstruct for the
             * 22.05 <-> 44.1 kHz boundary model. */
            {
                int32_t wet_l, wet_r;
                if (rev_phase == 0) {
                    /* First frame of the pair: hold the input, output the
                     * most recent engine result. */
                    rev_in_hold_l = rev_send_l;
                    rev_in_hold_r = rev_send_r;
                    wet_l = rev_out_l;
                    wet_r = rev_out_r;
                    rev_phase = 1;
                } else {
                    int32_t prev_l = rev_out_l;
                    int32_t prev_r = rev_out_r;
                    reverb_step(clamp16((rev_in_hold_l + rev_send_l) >> 1),
                                clamp16((rev_in_hold_r + rev_send_r) >> 1),
                                rev_wren);
                    wet_l = rev_reconstruct(prev_l, rev_out_l);
                    wet_r = rev_reconstruct(prev_r, rev_out_r);
                    rev_phase = 0;
                }
                mix_l += wet_l;
                mix_r += wet_r;
            }

            /* The 16-bit mix bus saturates BEFORE the main volume applies
             * (documented mixer order: sum -> saturate -> main volume). */
            mix_l = clamp16(mix_l);
            mix_r = clamp16(mix_r);
            mix_l = ((int32_t)mix_l * main_l) >> 15;
            mix_r = ((int32_t)mix_r * main_r) >> 15;
        }

        out_stereo[f * 2 + 0] = clamp16(mix_l);
        out_stereo[f * 2 + 1] = clamp16(mix_r);
        int32_t frame_peak = abs32(out_stereo[f * 2 + 0]);
        int32_t right_peak = abs32(out_stereo[f * 2 + 1]);
        if (right_peak > frame_peak) frame_peak = right_peak;
        if (frame_peak) nonzero_frames++;
        if (frame_peak > block_peak) block_peak = frame_peak;

        if (s_shadow_tap_on && f < SPU_SHADOW_TAP_FRAMES) {
            /* Tap keeps its historical 1.14 volume scale. */
            s_shadow_tap[f].main_l  = (int16_t)(main_l >> 1);
            s_shadow_tap[f].main_r  = (int16_t)(main_r >> 1);
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
    /* Historical 1.14 scale (effective int16 volume >> 1) for continuity
     * with older captures; identical values for direct-mode registers. */
    out->main_l = (int16_t)(chan_volume(spu_regs[reg_index(0x1F801D80u)],
                                        &sweep_main_env[0]) >> 1);
    out->main_r = (int16_t)(chan_volume(spu_regs[reg_index(0x1F801D82u)],
                                        &sweep_main_env[1]) >> 1);
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
                /* SPUSTAT (psx-spx): bits 5-0 mirror SPUCNT bits 5-0 (the
                 * current SPU mode), bit 6 is the IRQ flag (cleared by
                 * writing SPUCNT with bit 6 clear), bit 7 follows SPUCNT.5
                 * (DMA r/w request), bit 10 is the data-transfer busy flag —
                 * 0 here because this runtime completes FIFO/DMA transfers
                 * instantly. The old hardcoded 0x0400 held busy PERMANENTLY
                 * asserted; Sony code paths never polled it to zero, but
                 * OpenBIOS's shell MOD player waits for (SPUSTAT & 0x7FF)
                 * == 0 after clearing SPUCNT and spun forever. Bit 11 is
                 * "currently writing the SECOND half of the capture
                 * buffers" (capture offset >= 0x200). */
                uint16_t cnt = spu_regs[reg_index(0x1F801DAAu)];
                uint32_t st = (uint32_t)((cnt & 0x3Fu) | (((cnt >> 5) & 1u) << 7));
                if (irq_flag) st |= 0x40u;
                if (capture_pos & 0x200u) st |= 0x800u;
                return st;
            }
            /* Current main volume L/R (psx-spx 1F801DB8h/1F801DBAh): the
             * LIVE sweep-aware level as a signed 16-bit value. */
            if (addr == 0x1F801DB8u) {
                return (uint32_t)(uint16_t)chan_volume(
                    spu_regs[reg_index(0x1F801D80u)], &sweep_main_env[0]);
            }
            if (addr == 0x1F801DBAu) {
                return (uint32_t)(uint16_t)chan_volume(
                    spu_regs[reg_index(0x1F801D82u)], &sweep_main_env[1]);
            }
            /* A volume register in SWEEP mode reads back the envelope's
             * CURRENT level, not the sweep-parameter word — the live value
             * is what actually multiplies the samples.
             * DOCUMENTED-GAP: the exact readback encoding for a sweeping
             * volume register is not settled; the live level as a signed
             * 16-bit value was chosen. Candidate for oracle verification. */
            if (idx < (uint32_t)SPU_VOICE_COUNT * 8u && (idx & 7u) <= 1u
                && (spu_regs[idx] & 0x8000u)) {
                return (uint32_t)(uint16_t)
                    sweep_voice_env[idx >> 3][idx & 7u].level;
            }
            if ((addr == 0x1F801D80u || addr == 0x1F801D82u)
                && (spu_regs[idx] & 0x8000u)) {
                return (uint32_t)(uint16_t)
                    sweep_main_env[(addr >> 1) & 1u].level;
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

    /* Per-voice CURRENT volume L/R (psx-spx 1F801E00h..1F801E5Fh): the live
     * sweep-aware effective volume of each voice, two halfwords per voice. */
    if (addr >= 0x1F801E00u && addr < 0x1F801E60u) {
        uint32_t half = (addr - 0x1F801E00u) >> 1;
        int v  = (int)(half >> 1);
        int ch = (int)(half & 1u);
        if (v < SPU_VOICE_COUNT) {
            return (uint32_t)(uint16_t)chan_volume(
                voice_reg(v, ch), &sweep_voice_env[v][ch]);
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

            /* Volume registers feed the sweep envelopes: a direct write
             * takes effect immediately, a sweep-mode write starts gliding
             * from the current live level. */
            if (idx < (uint32_t)SPU_VOICE_COUNT * 8u && (idx & 7u) <= 1u) {
                sweep_env_write(&sweep_voice_env[idx >> 3][idx & 7u],
                                (uint16_t)value);
            }
            if (addr == 0x1F801D80u)
                sweep_env_write(&sweep_main_env[0], (uint16_t)value);
            if (addr == 0x1F801D82u)
                sweep_env_write(&sweep_main_env[1], (uint16_t)value);

            /* Reverb work-area base: writing mBASE also resets the current
             * buffer address to the area start. */
            if (addr == 0x1F801DA2u) {
                rev_cur = rev_mbase();
            }

            /* IRQ address write: re-evaluate the compare against the
             * current transfer address. DOCUMENTED-GAP: whether pointing
             * the IRQ address AT the resting transfer address fires
             * immediately (vs only on the next actual access) is not
             * settled; the immediate re-check was chosen so a match is
             * never missed. Candidate for oracle verification. */
            if (addr == 0x1F801DA4u) {
                spu_irq_check(transfer_addr, 2u);
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
                /* Setting the transfer address is an IRQ compare site (same
                 * DOCUMENTED-GAP as the IRQ-address write above). */
                spu_irq_check(transfer_addr, 2u);
            }

            if (addr == 0x1F801DA8u) {
                /* Manual FIFO write: an SPU RAM access at the transfer
                 * address — run the IRQ compare before storing. */
                spu_irq_check(transfer_addr, 2u);
                if (transfer_addr + 1 < SPU_RAM_SIZE) {
                    spu_ram[transfer_addr]     = (uint8_t)(value & 0xFF);
                    spu_ram[transfer_addr + 1] = (uint8_t)((value >> 8) & 0xFF);
                }
                transfer_addr = (transfer_addr + 2) % SPU_RAM_SIZE;
            }

            if (addr == 0x1F801DAAu) {
                /* SPUCNT: writing with bit 6 CLEAR acknowledges the IRQ
                 * latch and re-arms it; writing with bit 6 SET re-evaluates
                 * the compare against the resting transfer address (same
                 * DOCUMENTED-GAP as the address-register writes above). */
                if (!(value & 0x0040u)) {
                    irq_flag = 0;
                } else {
                    spu_irq_check(transfer_addr, 2u);
                }
            }
        }
    }
}

void spu_dma_write(uint32_t word) {
    spu_irq_check(transfer_addr, 4u);
    if (transfer_addr + 3 < SPU_RAM_SIZE) {
        spu_ram[transfer_addr]     = (uint8_t)(word & 0xFF);
        spu_ram[transfer_addr + 1] = (uint8_t)((word >> 8) & 0xFF);
        spu_ram[transfer_addr + 2] = (uint8_t)((word >> 16) & 0xFF);
        spu_ram[transfer_addr + 3] = (uint8_t)((word >> 24) & 0xFF);
    }
    transfer_addr = (transfer_addr + 4) % SPU_RAM_SIZE;
}

/* DMA4 SPU->CPU direction: read one little-endian 32-bit word from SPU RAM
 * at the transfer address, advance it by 4 and run the IRQ compare — the
 * exact mirror of spu_dma_write. Titles carry state through SPU RAM across
 * EXE transitions via this path; returning zeros breaks them. */
uint32_t spu_dma_read(void) {
    uint32_t word = 0;
    spu_irq_check(transfer_addr, 4u);
    if (transfer_addr + 3 < SPU_RAM_SIZE) {
        word = (uint32_t)spu_ram[transfer_addr]
             | ((uint32_t)spu_ram[transfer_addr + 1] << 8)
             | ((uint32_t)spu_ram[transfer_addr + 2] << 16)
             | ((uint32_t)spu_ram[transfer_addr + 3] << 24);
    }
    transfer_addr = (transfer_addr + 4) % SPU_RAM_SIZE;
    return word;
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
    out->vol_cur_l   = chan_volume(voice_reg(idx, 0), &sweep_voice_env[idx][0]);
    out->vol_cur_r   = chan_volume(voice_reg(idx, 1), &sweep_voice_env[idx][1]);
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

    /* ---- SPU DSP fidelity state (issue #103) ---- */
    out->irq_flag     = irq_flag;
    out->reverb_on    = (out->ctrl >> 7) & 1u;
    out->irq_addr     = ((uint32_t)spu_regs[reg_index(0x1F801DA4u)]) << 3;
    out->reverb_mbase = rev_mbase();
    out->reverb_cur   = rev_cur;
    out->capture_pos  = capture_pos;
    out->noise_lfsr   = noise_lfsr;
    out->noise_pad    = 0;
    uint32_t sl = 0, sr = 0;
    for (int i = 0; i < SPU_VOICE_COUNT; i++) {
        if (voice_reg(i, 0) & 0x8000u) sl |= (1u << i);
        if (voice_reg(i, 1) & 0x8000u) sr |= (1u << i);
    }
    out->sweep_l_mask = sl;
    out->sweep_r_mask = sr;
    out->sweep_main = ((spu_regs[reg_index(0x1F801D80u)] >> 15) & 1u)
                    | (((spu_regs[reg_index(0x1F801D82u)] >> 15) & 1u) << 1);
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

/* SpuVoice host sizeof has padding; wire is packed LE fields. 94 bytes of
 * classic voice state + 12 bytes of per-voice volume-sweep envelope state
 * (L then R: int16 level + uint32 divider each) = 106 bytes. */
#define SPU_VOICE_WIRE_BYTES ( \
    4u + 4u + 4u + (28u * 2u) + (3u * 2u) + 4u + 4u + 2u + 2u + 1u + 2u + 4u + 1u \
    + 2u * (2u + 4u))

static int spu_w_voice(PstW *w, int idx) {
    const SpuVoice *v = &voices[idx];
    if (!pst_w_i32(w, (int32_t)v->active) || !pst_w_u32(w, v->cur_addr) ||
        !pst_w_u32(w, v->repeat_addr))
        return 0;
    for (int i = 0; i < SPU_BLOCK_SAMPLES; i++)
        if (!pst_w_i16(w, v->samples[i])) return 0;
    for (int i = 0; i < 3; i++)
        if (!pst_w_i16(w, v->previous_samples[i])) return 0;
    if (!(pst_w_i32(w, (int32_t)v->sample_idx) && pst_w_u32(w, v->phase) &&
          pst_w_i16(w, v->hist1) && pst_w_i16(w, v->hist2) && pst_w_u8(w, v->flags) &&
          pst_w_u16(w, v->env_level) && pst_w_u32(w, v->adsr_divider) &&
          pst_w_u8(w, v->adsr_phase)))
        return 0;
    for (int ch = 0; ch < 2; ch++)
        if (!pst_w_i16(w, sweep_voice_env[idx][ch].level) ||
            !pst_w_u32(w, sweep_voice_env[idx][ch].divider))
            return 0;
    return 1;
}
static int spu_r_voice(PstR *r, int idx) {
    SpuVoice *v = &voices[idx];
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
    for (int ch = 0; ch < 2; ch++)
        if (!pst_r_i16(r, &sweep_voice_env[idx][ch].level) ||
            !pst_r_u32(r, &sweep_voice_env[idx][ch].divider))
            return 0;
    return 1;
}

/* Global trailer: 20 classic bytes + 44 bytes of DSP fidelity state
 * (IRQ latch, noise LFSR + divider, reverb address/phase/boundary state,
 * capture position, main L/R sweep envelopes). */
#define SPU_SNAPSHOT_TAIL_BYTES \
    (20u + 1u + 2u + 4u + 4u + 1u + 4u + 4u + 4u + 4u + 4u + 2u * (2u + 4u))

uint32_t spu_snapshot_bytes(void) {
    return (uint32_t)(SPU_REG_COUNT * 2u) +
           (SPU_VOICE_COUNT * SPU_VOICE_WIRE_BYTES) + SPU_SNAPSHOT_TAIL_BYTES;
}

void spu_snapshot_write(uint8_t *p) {
    PstW w;
    uint32_t n = spu_snapshot_bytes();
    pst_w_init(&w, p, n);
    for (uint32_t i = 0; i < SPU_REG_COUNT; i++)
        pst_w_u16(&w, spu_regs[i]);
    for (int i = 0; i < SPU_VOICE_COUNT; i++)
        spu_w_voice(&w, i);
    pst_w_u32(&w, transfer_addr);
    pst_w_u32(&w, key_on_count);
    pst_w_u32(&w, endx_latch);
    pst_w_u32(&w, kon_latch);
    pst_w_u32(&w, koff_latch);
    pst_w_u8(&w, irq_flag);
    pst_w_u16(&w, noise_lfsr);
    pst_w_i32(&w, noise_timer);
    pst_w_u32(&w, rev_cur);
    pst_w_u8(&w, rev_phase);
    pst_w_i32(&w, rev_in_hold_l);
    pst_w_i32(&w, rev_in_hold_r);
    pst_w_i32(&w, rev_out_l);
    pst_w_i32(&w, rev_out_r);
    pst_w_u32(&w, capture_pos);
    for (int ch = 0; ch < 2; ch++) {
        pst_w_i16(&w, sweep_main_env[ch].level);
        pst_w_u32(&w, sweep_main_env[ch].divider);
    }
}

int spu_snapshot_read(const uint8_t *p, uint32_t len) {
    PstR r;
    if (len != spu_snapshot_bytes()) return 0;
    pst_r_init(&r, p, len);
    for (uint32_t i = 0; i < SPU_REG_COUNT; i++)
        if (!pst_r_u16(&r, &spu_regs[i])) return 0;
    for (int i = 0; i < SPU_VOICE_COUNT; i++)
        if (!spu_r_voice(&r, i)) return 0;
    if (!pst_r_u32(&r, &transfer_addr) || !pst_r_u32(&r, &key_on_count) ||
        !pst_r_u32(&r, &endx_latch) || !pst_r_u32(&r, &kon_latch) ||
        !pst_r_u32(&r, &koff_latch))
        return 0;
    if (!pst_r_u8(&r, &irq_flag) || !pst_r_u16(&r, &noise_lfsr) ||
        !pst_r_i32(&r, &noise_timer) || !pst_r_u32(&r, &rev_cur) ||
        !pst_r_u8(&r, &rev_phase) ||
        !pst_r_i32(&r, &rev_in_hold_l) || !pst_r_i32(&r, &rev_in_hold_r) ||
        !pst_r_i32(&r, &rev_out_l) || !pst_r_i32(&r, &rev_out_r) ||
        !pst_r_u32(&r, &capture_pos))
        return 0;
    for (int ch = 0; ch < 2; ch++) {
        if (!pst_r_i16(&r, &sweep_main_env[ch].level) ||
            !pst_r_u32(&r, &sweep_main_env[ch].divider))
            return 0;
    }
    return 1;
}
uint8_t*  spu_get_ram_ptr(void){ return spu_ram; }
uint32_t  spu_get_ram_bytes(void){ return (uint32_t)sizeof(spu_ram); }

void spu_snapshot_part_digests(SpuSnapPartDigests *out)
{
    static uint8_t *buf;
    static uint32_t cap;
    uint32_t n;
    uint32_t regs_n;
    uint32_t voices_n;
    uint32_t crc;

    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    n = spu_snapshot_bytes();
    if (!n)
        return;
    if (n > cap) {
        uint8_t *nb = (uint8_t *)realloc(buf, n);
        if (!nb)
            return;
        buf = nb;
        cap = n;
    }
    spu_snapshot_write(buf);
    regs_n = (uint32_t)(SPU_REG_COUNT * 2u);
    voices_n = (uint32_t)(SPU_VOICE_COUNT * SPU_VOICE_WIRE_BYTES);
    if (regs_n + voices_n + SPU_SNAPSHOT_TAIL_BYTES != n)
        return;
    crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, buf, regs_n);
    out->regs = crc ^ 0xFFFFFFFFu;
    crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, buf + regs_n, voices_n);
    out->voices = crc ^ 0xFFFFFFFFu;
    crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, buf + regs_n + voices_n, SPU_SNAPSHOT_TAIL_BYTES);
    out->tail = crc ^ 0xFFFFFFFFu;
}
