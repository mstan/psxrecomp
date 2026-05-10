#ifndef PSXRECOMP_V4_SPU_H
#define PSXRECOMP_V4_SPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void spu_init(void);
void spu_render(int16_t* out_stereo, int frames);

typedef struct SpuDebugInfo {
    uint32_t ctrl;
    uint32_t active_mask;
    int16_t main_l;
    int16_t main_r;
    uint32_t key_on_count;
    uint64_t render_frames;
    uint64_t nonzero_frames;
    int32_t last_peak;
    int32_t peak;
} SpuDebugInfo;

void spu_debug_info(SpuDebugInfo* out);

/* ---- Per-voice register snapshot. Mirrors the fields the Beetle oracle
 * exposes via PS_SPU::GetRegister(GSREG_V0_*) so both backends produce a
 * structurally-identical spu_voices payload that diff tooling can compare. */
typedef struct SpuVoiceState {
    int      active;        /* our internal "is voicing" flag (no ADSR yet) */
    uint16_t vol_ctrl_l;    /* raw voice reg 0 */
    uint16_t vol_ctrl_r;    /* raw voice reg 1 */
    uint16_t pitch;         /* raw voice reg 2 */
    uint16_t start_lo;      /* raw voice reg 3 (start_addr >> 3) */
    uint16_t adsr_lo;       /* raw voice reg 5 */
    uint16_t adsr_hi;       /* raw voice reg 6 */
    uint16_t loop_lo;       /* raw voice reg 7 (loop_addr >> 3) */
    uint32_t cur_addr;      /* current decode pointer (byte addr in SPU RAM) */
    uint32_t repeat_addr;   /* effective loop point (byte addr in SPU RAM) */
    uint8_t  last_flags;    /* flag byte from most recently decoded block */
    uint8_t  sample_idx;    /* position inside the current 28-sample block */
    uint16_t phase;         /* sub-sample phase counter (0..0x1000) */
} SpuVoiceState;

typedef struct SpuGlobalState {
    uint16_t ctrl;          /* SPUCONT (0x1F801DAA) */
    uint16_t main_vol_l;    /* raw 0x1F801D80 */
    uint16_t main_vol_r;    /* raw 0x1F801D82 */
    uint32_t kon_latch;     /* last 24-bit KEYON value */
    uint32_t koff_latch;    /* last 24-bit KEYOFF value */
    uint32_t pmon;          /* pitch mod (0x1F801D90/92) */
    uint32_t non;           /* noise mode (0x1F801D94/96) */
    uint32_t eon;           /* reverb mode (0x1F801D98/9A) */
    uint32_t endx;          /* end-block reached latch (0x1F801D9C/9E) */
    uint32_t active_mask;   /* recomp-local "still voicing" mask */
} SpuGlobalState;

void spu_get_voice_state(int voice, SpuVoiceState* out);
void spu_get_global_state(SpuGlobalState* out);

/* ---- Always-on per-voice event ring. Records KEYON, KEYOFF, voice
 * end-flag-stop and end-flag-loop events with a frame timestamp. Allocated
 * at spu_init; never armed/disarmed. Query the window of interest. */
typedef enum {
    SPU_EV_KEYON     = 1,
    SPU_EV_KEYOFF    = 2,
    SPU_EV_END_STOP  = 3,   /* loop_end without repeat → voice silenced */
    SPU_EV_END_LOOP  = 4    /* loop_end with repeat → cur_addr=repeat_addr */
} SpuEventKind;

typedef struct SpuEvent {
    uint64_t seq;
    uint32_t frame;
    uint8_t  kind;
    uint8_t  voice;
    uint16_t pitch;
    uint32_t addr;          /* start_addr for KEYON; cur_addr for KEYOFF/END_*; repeat_addr for END_LOOP */
    uint16_t adsr_lo;
    uint16_t adsr_hi;
    uint16_t vol_l;
    uint16_t vol_r;
} SpuEvent;

uint64_t spu_event_total(void);
uint32_t spu_event_get(SpuEvent* out, uint32_t max_count);  /* most recent up to max_count */
void     spu_event_reset(void);

/* MMIO read/write (0x1F801C00-0x1F801FFF) */
uint32_t spu_read(uint32_t addr);
void spu_write(uint32_t addr, uint32_t value);

/* DMA channel 4 interface */
void spu_dma_write(uint32_t word);
int spu_dma_ready(void);

/* Get pointer to SPU RAM for direct access (512KB) */
const uint8_t* spu_get_ram(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_V4_SPU_H */
