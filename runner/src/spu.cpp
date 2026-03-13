/* spu.cpp — PS1 SPU emulator
 *
 * Handles SPU register writes at 0x1F801C00-0x1F801DFF, DMA4 data transfers
 * into 512KB SPU RAM, SPU-ADPCM decoding for 24 voices, and audio output via
 * WinMM at 44100 Hz stereo.
 *
 * SPU Register Map (offsets from 0x1F801C00):
 *   Voice n (n=0..23) at offset n*0x10:
 *     +0x00  Volume Left   (15-bit, bit15=sweep-mode)
 *     +0x02  Volume Right
 *     +0x04  Sample Rate   (0x1000 = 44100 Hz, pitch 1:1)
 *     +0x06  Start Address (byte_addr >> 3, i.e. units of 8 bytes)
 *     +0x08  ADSR Lo
 *     +0x0A  ADSR Hi
 *     +0x0C  Current Volume (read-only)
 *     +0x0E  Repeat Address (byte_addr >> 3)
 *   Global at offset 0x180 (= 0x1F801D80):
 *     +0x188 KON Lo  (voices 0-15,  write-only)
 *     +0x18A KON Hi  (voices 16-23, write-only)
 *     +0x18C KOFF Lo
 *     +0x18E KOFF Hi
 *     +0x1A6 Transfer Address (byte_addr >> 3)
 *     +0x1A8 Transfer FIFO data
 *     +0x1AA SPUCNT
 *     +0x1AE SPUSTAT (read-only; we return 0 = ready)
 *
 * ADSR 32-bit register (ADSR_Hi<<16 | ADSR_Lo):
 *   Bit 31     Attack Mode (0=Linear, 1=Exponential)
 *   Bits 30-26 Attack Shift (0-31)
 *   Bits 25-24 Attack Step  (0-3 → +7,+6,+5,+4)
 *   Bits 23-20 Decay Shift  (0-15; decay always exponential)
 *   Bits 19-16 Sustain Level (0-15 → actual = (N+1)*0x800)
 *   Bit  15    Sustain Mode (0=Linear, 1=Exponential)
 *   Bit  14    Sustain Direction (0=Increase, 1=Decrease)
 *   Bit  13    (unused)
 *   Bits 12-8  Sustain Shift (0-31)
 *   Bits  7-6  Sustain Step  (0-3)
 *   Bit   5    Release Mode (0=Linear, 1=Exponential)
 *   Bits  4-0  Release Shift (0-31)
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include "spu.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <atomic>
#include <thread>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */
#define SPU_RAM_SIZE     (512 * 1024)
#define SPU_NUM_VOICES   24
#define SPU_SAMPLE_RATE  44100

/* WinMM output buffer config */
#define SPU_BUF_SAMPLES  1024    /* stereo pairs per buffer */
#define SPU_NUM_BUFS     4

/* -------------------------------------------------------------------------
 * SPU RAM and register mirror
 * ---------------------------------------------------------------------- */
static uint8_t  g_spu_ram[SPU_RAM_SIZE];

/* Flat mirror of 0x1F801C00-0x1F801DFF as uint16_t[256].
 * Index = (phys_addr - 0x1F801C00) >> 1 */
static uint16_t g_spu_regs[256];

/* Current SPU RAM write pointer (byte address) for FIFO / DMA4 */
static uint32_t g_transfer_addr = 0;

/* Master volume scale: 0.0 = silent, 1.0 = full.  Applied in spu_mix(). */
static float g_spu_master_vol = 1.0f;

/* -------------------------------------------------------------------------
 * SPU-ADPCM filter coefficients (same K0/K1 as XA-ADPCM)
 * ---------------------------------------------------------------------- */
static const int K0[5] = {   0, 60, 115,  98, 122 };
static const int K1[5] = {   0,  0, -52, -55, -60 };

/* -------------------------------------------------------------------------
 * Per-voice runtime state
 * ---------------------------------------------------------------------- */
struct VoiceState {
    bool     active;
    uint32_t cur_addr;      /* current byte address in SPU RAM */
    uint32_t counter;       /* pitch fractional counter (12-bit frac: 0..0xFFF) */
    int32_t  prev1, prev2;  /* ADPCM IIR history */

    /* ADSR envelope */
    int32_t  env_vol;       /* 0..0x7FFF */
    int      env_phase;     /* 0=attack 1=decay 2=sustain 3=release 4=off */

    /* Decoded block cache: 28 samples per 16-byte SPU-ADPCM block */
    int16_t  blk[28];
    int      blk_idx;       /* 0..27 — current position within block */

    int32_t  loop_start;    /* byte addr of loop-start (-1 = not seen yet) */
};

static VoiceState g_voices[SPU_NUM_VOICES];

/* -------------------------------------------------------------------------
 * Thread safety
 * ---------------------------------------------------------------------- */
static CRITICAL_SECTION g_cs;

/* -------------------------------------------------------------------------
 * WinMM audio output
 * ---------------------------------------------------------------------- */
static HWAVEOUT g_waveout  = NULL;
static WAVEHDR  g_hdrs[SPU_NUM_BUFS];
static int16_t  g_pcm [SPU_NUM_BUFS][SPU_BUF_SAMPLES * 2];  /* stereo */

static std::atomic<bool> g_running{false};
static std::thread       g_thread;

/* -------------------------------------------------------------------------
 * ADPCM block decode
 *
 * Decodes one 16-byte SPU-ADPCM block into 28 int16_t samples.
 *   blk[0]  = (filter<<4) | shift
 *   blk[1]  = flags (bit0=loop_end, bit1=loop_repeat, bit2=loop_start)
 *   blk[2..15] = 14 bytes of nibbles, two 4-bit samples per byte
 * ---------------------------------------------------------------------- */
static void decode_adpcm_block(const uint8_t* blk, int16_t* out,
                                int32_t* p1, int32_t* p2)
{
    int shift_raw = blk[0] & 0x0F;
    int filter    = (blk[0] >> 4) & 0x07;
    if (filter > 4) filter = 4;

    /* PS1 ADPCM left-shift = 12 - shift_raw; clamp to avoid UB on shift>=32 */
    int lsh = 12 - shift_raw;
    if (lsh < 0)  lsh = 0;
    if (lsh > 12) lsh = 12;

    for (int i = 0; i < 14; i++) {
        uint8_t byte = blk[2 + i];
        for (int j = 0; j < 2; j++) {
            int n = (j == 0) ? (byte & 0x0F) : ((byte >> 4) & 0x0F);
            if (n >= 8) n -= 16;                    /* sign-extend 4-bit nibble */
            int32_t t = (int32_t)n << lsh;
            t += (K0[filter] * (*p1) + K1[filter] * (*p2) + 32) >> 6;
            if (t >  32767) t =  32767;
            if (t < -32768) t = -32768;
            *out++ = (int16_t)t;
            *p2 = *p1;
            *p1 = t;
        }
    }
}

/* -------------------------------------------------------------------------
 * ADSR envelope update (called once per output sample per active voice)
 * ---------------------------------------------------------------------- */
static void update_adsr(VoiceState* v, int vi)
{
    if (v->env_phase >= 4) return;

    uint32_t adsr32 = ((uint32_t)g_spu_regs[vi * 8 + 5] << 16)
                    |  (uint32_t)g_spu_regs[vi * 8 + 4];

    int attack_shift   = (adsr32 >> 26) & 0x1F;
    int attack_step    = (adsr32 >> 24) & 0x03;   /* 0=+7, 1=+6, 2=+5, 3=+4 */
    int decay_shift    = (adsr32 >> 20) & 0x0F;
    int sustain_level  = (int)(((adsr32 >> 16) & 0x0F) + 1) * 0x800;
    if (sustain_level > 0x7FFF) sustain_level = 0x7FFF;
    int release_shift  = (int)(adsr32 & 0x1F);

    /* Compute linear envelope rate from PS1 formula:
     *   rate = (7 - step) << max(0, 11 - shift)
     *        = (7 - step) >> max(0, shift - 11)  */
    auto env_rate = [](int step, int shift) -> int32_t {
        int add = 7 - step;
        if (shift <= 11) return (int32_t)add << (11 - shift);
        int rsh = shift - 11;
        if (rsh >= 30) return 1;
        return (int32_t)add >> rsh;
    };

    switch (v->env_phase) {

    case 0: { /* Attack */
        int32_t rate = env_rate(attack_step, attack_shift);
        if (rate < 1) rate = 1;
        v->env_vol += rate;
        if (v->env_vol >= 0x7FFF) {
            v->env_vol = 0x7FFF;
            v->env_phase = 1;   /* → Decay */
        }
        break;
    }

    case 1: { /* Decay (always exponential: decrement by fraction of current vol) */
        /* Approximate exponential: step = current_vol >> (decay_shift + 1) */
        int rs = decay_shift + 1;
        if (rs > 15) rs = 15;
        int32_t rate = v->env_vol >> rs;
        if (rate < 1) rate = 1;
        v->env_vol -= rate;
        if (v->env_vol <= sustain_level) {
            v->env_vol   = sustain_level;
            v->env_phase = 2;   /* → Sustain */
        }
        break;
    }

    case 2: /* Sustain — simplified: just hold at level */
        v->env_vol = sustain_level;
        break;

    case 3: { /* Release (exponential approximation) */
        int rs = release_shift + 1;
        if (rs > 15) rs = 15;
        int32_t rate = v->env_vol >> rs;
        if (rate < 1) rate = 1;
        v->env_vol -= rate;
        if (v->env_vol <= 0) {
            v->env_vol   = 0;
            v->env_phase = 4;
            v->active    = false;
        }
        break;
    }

    default:
        v->active = false;
        break;
    }
}

/* -------------------------------------------------------------------------
 * Mix all active SPU voices into an output buffer (stereo, 44100 Hz)
 * Called from the audio thread.
 * ---------------------------------------------------------------------- */
static void spu_mix(int16_t* out, int num_pairs)
{
    EnterCriticalSection(&g_cs);

    for (int i = 0; i < num_pairs; i++) {
        int32_t mix_l = 0, mix_r = 0;

        for (int vi = 0; vi < SPU_NUM_VOICES; vi++) {
            VoiceState* v = &g_voices[vi];
            if (!v->active) continue;

            /* --- Pitch advance ---
             * counter is a 12-bit fractional accumulator.
             * sample_rate reg 0x1000 = advance by 1.0 sample per output tick.
             * Higher values = higher pitch; lower = lower pitch. */
            uint16_t pitch = g_spu_regs[vi * 8 + 2];
            v->counter += pitch;
            int adv = (int)(v->counter >> 12);
            v->counter &= 0xFFFu;

            /* --- Advance through decoded blocks ---
             * Each SPU-ADPCM block is 16 bytes → 28 samples. */
            v->blk_idx += adv;

            while (v->blk_idx >= 28 && v->active) {
                v->blk_idx -= 28;

                /* Guard: don't read past SPU RAM */
                if (v->cur_addr + 16u > SPU_RAM_SIZE) {
                    v->active = false;
                    break;
                }

                const uint8_t* blk   = g_spu_ram + v->cur_addr;
                uint8_t        flags = blk[1];

                /* Loop-start flag: remember this address */
                if (flags & 0x04) v->loop_start = (int32_t)v->cur_addr;

                decode_adpcm_block(blk, v->blk, &v->prev1, &v->prev2);
                v->cur_addr += 16u;

                if (flags & 0x01) {         /* loop-end */
                    if (flags & 0x02) {     /* loop-repeat → jump to loop start */
                        uint32_t rep = (v->loop_start >= 0)
                            ? (uint32_t)v->loop_start
                            : ((uint32_t)g_spu_regs[vi * 8 + 7] * 8u);
                        v->cur_addr  = rep;
                        v->prev1     = 0;
                        v->prev2     = 0;
                        /* Immediately decode the first looped block */
                        if (rep + 16u <= SPU_RAM_SIZE) {
                            const uint8_t* rb = g_spu_ram + rep;
                            if (rb[1] & 0x04) v->loop_start = (int32_t)rep;
                            decode_adpcm_block(rb, v->blk, &v->prev1, &v->prev2);
                            v->cur_addr = rep + 16u;
                        }
                    } else {                /* one-shot end: begin release */
                        v->env_phase = 3;
                    }
                }
            }

            if (!v->active) continue;

            /* --- ADSR envelope --- */
            update_adsr(v, vi);
            if (!v->active) continue;

            /* --- Get sample (nearest-neighbour) --- */
            int idx = v->blk_idx < 28 ? v->blk_idx : 27;
            int32_t s = (int32_t)v->blk[idx];

            /* Apply ADSR volume: env_vol is 0..0x7FFF */
            s = (s * v->env_vol) >> 15;

            /* Apply voice volume (15-bit fixed, bit15 = sweep mode — ignored) */
            int16_t vl = (int16_t)(g_spu_regs[vi * 8 + 0] & 0x7FFFu);
            int16_t vr = (int16_t)(g_spu_regs[vi * 8 + 1] & 0x7FFFu);

            mix_l += (s * (int32_t)vl) >> 14;
            mix_r += (s * (int32_t)vr) >> 14;
        }

        /* Apply master volume */
        mix_l = (int32_t)(mix_l * g_spu_master_vol);
        mix_r = (int32_t)(mix_r * g_spu_master_vol);

        /* Clamp to 16-bit */
        if (mix_l >  32767) mix_l =  32767;
        if (mix_l < -32768) mix_l = -32768;
        if (mix_r >  32767) mix_r =  32767;
        if (mix_r < -32768) mix_r = -32768;

        out[i * 2]     = (int16_t)mix_l;
        out[i * 2 + 1] = (int16_t)mix_r;
    }

    LeaveCriticalSection(&g_cs);
}

/* -------------------------------------------------------------------------
 * Audio output thread
 * Keeps WinMM's buffer queue filled.  Rotation: fill oldest done buffer,
 * re-submit; natural WinMM timing provides accurate 44100 Hz clock.
 * ---------------------------------------------------------------------- */
static void audio_thread_func()
{
    WAVEFORMATEX wfx  = {};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = SPU_SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = 4;
    wfx.nAvgBytesPerSec = SPU_SAMPLE_RATE * 4;

    if (waveOutOpen(&g_waveout, WAVE_MAPPER, &wfx,
                    0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        fprintf(stderr, "[SPU] waveOutOpen failed\n");
        fflush(stderr);
        return;
    }

    memset(g_hdrs, 0, sizeof(g_hdrs));
    memset(g_pcm,  0, sizeof(g_pcm));

    /* Pre-fill and submit all buffers */
    for (int i = 0; i < SPU_NUM_BUFS; i++) {
        spu_mix(g_pcm[i], SPU_BUF_SAMPLES);
        WAVEHDR* h       = &g_hdrs[i];
        h->lpData        = reinterpret_cast<LPSTR>(g_pcm[i]);
        h->dwBufferLength = SPU_BUF_SAMPLES * 4;
        h->dwFlags        = 0;
        waveOutPrepareHeader(g_waveout, h, sizeof(WAVEHDR));
        waveOutWrite(g_waveout, h, sizeof(WAVEHDR));
    }

    int cur = 0;
    while (g_running.load(std::memory_order_relaxed)) {
        WAVEHDR* h = &g_hdrs[cur];

        /* Wait for this buffer to be consumed */
        while (!(h->dwFlags & WHDR_DONE) &&
               g_running.load(std::memory_order_relaxed)) {
            Sleep(1);
        }
        if (!g_running.load(std::memory_order_relaxed)) break;

        /* Refill and resubmit */
        waveOutUnprepareHeader(g_waveout, h, sizeof(WAVEHDR));
        spu_mix(g_pcm[cur], SPU_BUF_SAMPLES);
        h->lpData         = reinterpret_cast<LPSTR>(g_pcm[cur]);
        h->dwBufferLength = SPU_BUF_SAMPLES * 4;
        h->dwFlags        = 0;
        waveOutPrepareHeader(g_waveout, h, sizeof(WAVEHDR));
        waveOutWrite(g_waveout, h, sizeof(WAVEHDR));

        cur = (cur + 1) % SPU_NUM_BUFS;
    }

    /* Drain and close */
    waveOutReset(g_waveout);
    for (int i = 0; i < SPU_NUM_BUFS; i++) {
        if (g_hdrs[i].dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(g_waveout, &g_hdrs[i], sizeof(WAVEHDR));
    }
    waveOutClose(g_waveout);
    g_waveout = NULL;
}

/* =========================================================================
 * Public API
 * ====================================================================== */

void spu_init(void)
{
    memset(g_spu_ram,  0, sizeof(g_spu_ram));
    memset(g_spu_regs, 0, sizeof(g_spu_regs));
    memset(g_voices,   0, sizeof(g_voices));
    for (int i = 0; i < SPU_NUM_VOICES; i++) {
        g_voices[i].loop_start = -1;
        g_voices[i].blk_idx    = 28;  /* force decode on first use */
    }
    g_transfer_addr = 0;

    InitializeCriticalSection(&g_cs);

    g_running.store(true, std::memory_order_relaxed);
    g_thread = std::thread(audio_thread_func);

    printf("[SPU] Initialised — 24 voices, 44100 Hz stereo\n");
    fflush(stdout);
}

void spu_shutdown(void)
{
    g_running.store(false, std::memory_order_relaxed);
    if (g_thread.joinable()) g_thread.join();
    DeleteCriticalSection(&g_cs);
}

void spu_set_master_volume(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g_spu_master_vol = v;
}

/* -------------------------------------------------------------------------
 * Key-on helper: start voices indicated by mask.
 * base_bit = 0 for KON Lo (voices 0-15), 16 for KON Hi (voices 16-23).
 * ---------------------------------------------------------------------- */
static void key_on(uint16_t mask, int base_bit)
{
    for (int i = 0; i < 16; i++) {
        int vi = base_bit + i;
        if (vi >= SPU_NUM_VOICES) break;
        if (!(mask & (1u << i))) continue;

        VoiceState* v = &g_voices[vi];
        uint16_t start_reg = g_spu_regs[vi * 8 + 3];   /* StartAddr reg */
        uint16_t pitch     = g_spu_regs[vi * 8 + 2];
        uint32_t adsr32    = ((uint32_t)g_spu_regs[vi * 8 + 5] << 16)
                           |  (uint32_t)g_spu_regs[vi * 8 + 4];

        static uint32_t s_kon = 0;
        /* [SPU KON] first 30 — re-enable printf when investigating voice keying */
        ++s_kon;

        v->cur_addr    = (uint32_t)start_reg * 8u;
        v->counter     = 0;
        v->prev1       = 0;
        v->prev2       = 0;
        v->env_vol     = 0;
        v->env_phase   = 0;    /* Attack */
        v->blk_idx     = 28;   /* force decode of first block on next tick */
        v->loop_start  = -1;
        v->active      = true;
    }
}

/* -------------------------------------------------------------------------
 * Key-off helper: begin release phase for indicated voices.
 * ---------------------------------------------------------------------- */
static void key_off(uint16_t mask, int base_bit)
{
    for (int i = 0; i < 16; i++) {
        int vi = base_bit + i;
        if (vi >= SPU_NUM_VOICES) break;
        if (!(mask & (1u << i))) continue;
        if (g_voices[vi].env_phase < 3)
            g_voices[vi].env_phase = 3;  /* Release */
    }
}

/* -------------------------------------------------------------------------
 * spu_write_half — handle a 16-bit write to a SPU hardware register.
 * ---------------------------------------------------------------------- */
void spu_write_half(uint32_t addr, uint16_t val)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < 0x1F801C00u || phys >= 0x1F801E00u) return;

    uint32_t idx = (phys - 0x1F801C00u) >> 1;
    if (idx >= 256u) return;

    /* Always store to register mirror so reads return the written value */
    g_spu_regs[idx] = val;

    EnterCriticalSection(&g_cs);

    switch (phys) {

    /* Transfer Address: val * 8 = byte address in SPU RAM */
    case 0x1F801DA6u:
        g_transfer_addr = (uint32_t)val * 8u;
        break;

    /* Transfer FIFO: write 2 bytes at current transfer address */
    case 0x1F801DA8u:
        if (g_transfer_addr + 1u < SPU_RAM_SIZE) {
            g_spu_ram[g_transfer_addr]     = (uint8_t)(val & 0xFF);
            g_spu_ram[g_transfer_addr + 1] = (uint8_t)(val >> 8);
            g_transfer_addr += 2u;
        }
        break;

    /* KON Lo — key on voices 0-15 */
    case 0x1F801D88u:
        key_on(val, 0);
        break;

    /* KON Hi — key on voices 16-23 */
    case 0x1F801D8Au:
        key_on(val, 16);
        break;

    /* KOFF Lo — begin release for voices 0-15 */
    case 0x1F801D8Cu:
        key_off(val, 0);
        break;

    /* KOFF Hi — begin release for voices 16-23 */
    case 0x1F801D8Eu:
        key_off(val, 16);
        break;

    default:
        break;
    }

    LeaveCriticalSection(&g_cs);
}

/* -------------------------------------------------------------------------
 * spu_write_word — handle a 32-bit write (e.g. ADSR written as one store).
 * ---------------------------------------------------------------------- */
void spu_write_word(uint32_t addr, uint32_t val)
{
    /* Split into two halfword writes at the same addresses */
    spu_write_half(addr,     (uint16_t)(val & 0xFFFFu));
    spu_write_half(addr + 2, (uint16_t)(val >> 16));
}

/* -------------------------------------------------------------------------
 * spu_read_half — return the register mirror value (or 0 for SPUSTAT).
 * ---------------------------------------------------------------------- */
uint16_t spu_read_half(uint32_t addr)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < 0x1F801C00u || phys >= 0x1F801E00u) return 0;

    /* SPUSTAT (0x1F801DAE): return 0 = ready (no busy/error bits) */
    if (phys == 0x1F801DAEu) return 0;

    uint32_t idx = (phys - 0x1F801C00u) >> 1;
    if (idx >= 256u) return 0;
    return g_spu_regs[idx];
}

/* -------------------------------------------------------------------------
 * spu_dma_write — DMA4 bulk transfer: copy bytes from main RAM to SPU RAM.
 * ---------------------------------------------------------------------- */
void spu_dma_write(uint32_t src_ram_addr, uint32_t byte_count,
                   const uint8_t* ram_base, uint32_t ram_size)
{
    src_ram_addr &= 0x1FFFFFu;          /* strip KSEG bits, clamp to 2MB */
    if (src_ram_addr >= ram_size) return;
    if (src_ram_addr + byte_count > ram_size)
        byte_count = ram_size - src_ram_addr;

    uint32_t dst = g_transfer_addr;
    if (dst >= SPU_RAM_SIZE) return;
    if (dst + byte_count > SPU_RAM_SIZE)
        byte_count = SPU_RAM_SIZE - dst;

    EnterCriticalSection(&g_cs);
    memcpy(g_spu_ram + dst, ram_base + src_ram_addr, byte_count);
    g_transfer_addr += byte_count;
    LeaveCriticalSection(&g_cs);

    /* [SPU DMA] printf("[SPU DMA] %u bytes: RAM 0x%06X → SPU RAM 0x%05X\n", byte_count, ...); */
}
