/*
 * spu.c - PS1 Sound Processing Unit register and direct ADPCM voice model.
 *
 * This is intentionally still a compact hardware model: it accepts SPU
 * register reads/writes, DMA4 transfers into 512KB SPU RAM, and mixes the
 * 24 direct ADPCM voices. Reverb, noise, sweep volumes, XA/CD input and IRQ
 * timing are not modeled yet.
 */

#include "spu.h"

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

    if (flags & 0x04u) v->repeat_addr = addr;
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

static int16_t voice_next_sample(int idx) {
    SpuVoice *v = &voices[idx];
    if (!v->active) return 0;

    if (v->sample_idx >= SPU_BLOCK_SAMPLES) {
        if (v->flags & 0x01u) {
            if (v->flags & 0x02u) {
                v->cur_addr = v->repeat_addr & (SPU_RAM_SIZE - 1u);
                spu_event_record(SPU_EV_END_LOOP, idx, v->repeat_addr);
            } else {
                /* End-without-repeat triggers Release on real hardware
                 * (Beetle's RunDecoder calls ReleaseEnvelope here). The
                 * voice keeps decoding past the end block — whatever
                 * follows in SPU RAM — while env_level decays to 0.
                 * Garbage samples are masked by the dying envelope, so
                 * by the time anything would be audible it's silent. */
                spu_event_record(SPU_EV_END_STOP, idx, v->cur_addr);
                v->adsr_phase = ADSR_RELEASE;
                v->adsr_divider = 0;
                v->flags = 0;  /* don't re-enter this branch */
            }
        }
        decode_block(v);
    }

    int16_t raw_s = v->samples[v->sample_idx];
    /* Apply envelope (0..0x7FFF as a 15-bit gain). */
    int32_t shaped = ((int32_t)raw_s * (int32_t)v->env_level) >> 15;
    if (shaped > 32767)  shaped = 32767;
    if (shaped < -32768) shaped = -32768;

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
}

void spu_render(int16_t* out_stereo, int frames) {
    if (!out_stereo || frames <= 0) return;

    uint16_t ctrl = spu_regs[reg_index(0x1F801DAAu)];
    int enabled = (ctrl & 0x8000u) != 0;
    int16_t main_l = direct_volume(spu_regs[reg_index(0x1F801D80u)]);
    int16_t main_r = direct_volume(spu_regs[reg_index(0x1F801D82u)]);

    int32_t block_peak = 0;
    for (int f = 0; f < frames; f++) {
        int32_t mix_l = 0;
        int32_t mix_r = 0;

        if (enabled) {
            for (int v = 0; v < SPU_VOICE_COUNT; v++) {
                int16_t s = voice_next_sample(v);
                if (!s) continue;
                int16_t vl = direct_volume(voice_reg(v, 0));
                int16_t vr = direct_volume(voice_reg(v, 1));
                mix_l += ((int32_t)s * vl) >> 14;
                mix_r += ((int32_t)s * vr) >> 14;
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
    }
    render_frames += (uint64_t)frames;
    last_peak = block_peak;
    if (block_peak > peak) peak = block_peak;
}

void spu_debug_info(SpuDebugInfo* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->ctrl = spu_regs[reg_index(0x1F801DAAu)];
    out->main_l = direct_volume(spu_regs[reg_index(0x1F801D80u)]);
    out->main_r = direct_volume(spu_regs[reg_index(0x1F801D82u)]);
    for (int i = 0; i < SPU_VOICE_COUNT; i++) {
        if (voices[i].active) out->active_mask |= (1u << i);
    }
    out->key_on_count = key_on_count;
    out->render_frames = render_frames;
    out->nonzero_frames = nonzero_frames;
    out->last_peak = last_peak;
    out->peak = peak;
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
            spu_regs[idx] = (uint16_t)value;

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
