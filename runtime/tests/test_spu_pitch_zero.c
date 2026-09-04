/*
 * Regression test: a voice pitch of 0 holds the voice.
 *
 * VxPITCH is added to a 12-bit fractional phase accumulator, so zero means the
 * sample counter never advances: the voice sits on its current sample until
 * something changes the pitch or keys it off. It does NOT mean "play at 1.0x".
 *
 * spu.c used to coerce a zero pitch to 0x1000, which made a parked voice walk
 * forward through SPU RAM decoding whatever followed it as ADPCM. Alien
 * Resurrection (SLUS-00633) parks its two ambience voices exactly that way
 * when the pause menu opens — pitch 0, envelope frozen mid-sustain, no key-off
 * — and they ran roughly 0x790 bytes past their own repeat address, turning
 * the menu into a continuous buzz.
 *
 * The test includes spu.c so it can stage a voice directly, matching
 * test_spu_end_without_repeat.c.
 *
 * Build/run: ctest -R spu_pitch_zero_test
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/spu.c"

/* Dependencies owned by the surrounding runtime are inert in this unit test. */
uint64_t s_frame_count;

uint64_t psx_get_cycle_count(void) { return 0; }

void audio_trace_pcm(int tap, const int16_t *stereo, int frames) {
    (void)tap;
    (void)stereo;
    (void)frames;
}

void audio_trace_event(uint16_t kind, uint32_t a, uint32_t b) {
    (void)kind;
    (void)a;
    (void)b;
}

void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)bit; (void)detail; }

uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    (void)data;
    (void)len;
    return crc;
}

bool spu_shadow_enabled(void) { return false; }
void spu_shadow_reset(void) {}
void spu_shadow_process(int16_t *canon, int frames) {
    (void)canon;
    (void)frames;
}

static int failures;

#define CHECK(condition, message)                                               \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL: %s\n", message);                            \
            failures++;                                                         \
        } else {                                                                \
            printf("ok:   %s\n", message);                                     \
        }                                                                       \
    } while (0)

#define VOICE_START 0x200u

static void fill_block(uint32_t addr, uint8_t flags, uint8_t packed) {
    spu_ram[addr] = 0x00; /* filter 0, shift 0 */
    spu_ram[addr + 1u] = flags;
    memset(spu_ram + addr + 2u, packed, 14u);
}

/* Three consecutive blocks with different sample data, so a voice that walks
 * forward when it should be holding changes its output audibly rather than
 * silently reading more of the same. */
static void stage_ram(void) {
    fill_block(VOICE_START + 0x00u, 0x00u, 0x11u);
    fill_block(VOICE_START + 0x10u, 0x00u, 0x55u);
    fill_block(VOICE_START + 0x20u, 0x00u, 0x77u);
}

/* A voice mid-sustain at full envelope, about to decode its first block.
 * The ADSR registers are left at zero: sustain then holds 0x7FFF instead of
 * drifting, so any change in the emitted sample comes from the decoder, which
 * is what these tests are about. */
static SpuVoice *stage_voice(uint16_t pitch) {
    SpuVoice *v = &voices[0];
    memset(v, 0, sizeof(*v));
    v->active = 1;
    v->cur_addr = VOICE_START;
    v->repeat_addr = VOICE_START;
    v->sample_idx = SPU_BLOCK_SAMPLES;   /* forces the first decode */
    v->env_level = 0x7FFFu;
    v->adsr_phase = ADSR_SUSTAIN;
    v->flags = 0x00u;
    spu_regs[2] = pitch;   /* voice 0, pitch register */
    spu_regs[4] = 0x0000u; /* ADSR lo */
    spu_regs[5] = 0x0000u; /* ADSR hi */
    return v;
}

static void test_pitch_zero_holds_the_voice(void) {
    spu_init();
    stage_ram();
    SpuVoice *v = stage_voice(0x0000u);

    /* First call decodes the staged block and leaves the voice on sample 0. */
    int16_t first = voice_next_sample(0);
    uint32_t addr_after_first = v->cur_addr;
    CHECK(addr_after_first == VOICE_START + 0x10u,
          "the first call decodes exactly one block");
    CHECK(v->sample_idx == 0, "the first call leaves the voice on sample 0");

    int drifted_sample = 0, drifted_addr = 0, drifted_phase = 0;
    for (int i = 0; i < 500; i++) {
        if (voice_next_sample(0) != first) drifted_sample = 1;
        if (v->cur_addr != addr_after_first) drifted_addr = 1;
        if (v->phase != 0) drifted_phase = 1;
    }

    CHECK(!drifted_phase, "pitch 0 never advances the phase accumulator");
    CHECK(v->sample_idx == 0, "pitch 0 never advances the sample index");
    CHECK(!drifted_addr,
          "pitch 0 never decodes another block, so cur_addr stays put");
    CHECK(!drifted_sample, "pitch 0 emits the same sample every time");
    CHECK(v->active, "a held voice stays active");

    /* The failure this test exists for: 500 output samples is nearly 18 blocks
     * at full rate, so a coerced pitch would be deep into unrelated RAM. */
    CHECK(v->cur_addr < VOICE_START + 0x20u,
          "a held voice does not walk into the blocks that follow it");
}

static void test_nonzero_pitch_still_advances(void) {
    spu_init();
    stage_ram();
    SpuVoice *v = stage_voice(0x1000u);   /* one decoded sample per output */

    (void)voice_next_sample(0);           /* decode the first block */
    uint32_t addr_after_first = v->cur_addr;

    for (int i = 0; i < SPU_BLOCK_SAMPLES; i++)
        (void)voice_next_sample(0);

    CHECK(v->cur_addr == addr_after_first + 0x10u,
          "pitch 0x1000 decodes the next block after 28 samples");
    CHECK(v->active, "a playing voice stays active");
}

static void test_half_pitch_advances_half_as_fast(void) {
    spu_init();
    stage_ram();
    SpuVoice *v = stage_voice(0x0800u);   /* half rate */

    (void)voice_next_sample(0);
    for (int i = 0; i < SPU_BLOCK_SAMPLES; i++)
        (void)voice_next_sample(0);

    CHECK(v->sample_idx == SPU_BLOCK_SAMPLES / 2,
          "pitch 0x0800 advances one sample every two outputs");
}

static void test_pitch_zero_still_steps_the_envelope(void) {
    spu_init();
    stage_ram();
    SpuVoice *v = stage_voice(0x0000u);
    (void)voice_next_sample(0);

    /* Key-off on a parked voice must still release and retire it — otherwise
     * holding the sample would trade one stuck voice for another. Rr = 0 with
     * a linear release is the fastest decay the register allows. */
    v->adsr_phase = ADSR_RELEASE;
    v->adsr_divider = 0;
    spu_regs[5] = 0x0000u;

    uint16_t before = v->env_level;
    (void)voice_next_sample(0);
    CHECK(v->env_level < before,
          "the envelope still steps while the voice is held");

    for (int i = 0; i < 4096 && v->active; i++)
        (void)voice_next_sample(0);

    CHECK(!v->active, "a held voice still retires once Release reaches zero");
    CHECK(v->env_level == 0, "Release drives the envelope to zero");
}

int main(void) {
    test_pitch_zero_holds_the_voice();
    test_nonzero_pitch_still_advances();
    test_half_pitch_advances_half_as_fast();
    test_pitch_zero_still_steps_the_envelope();
    printf(failures ? "FAILED (%d)\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
