/*
 * Regression test for an ADPCM END block without REPEAT.
 *
 * The test includes spu.c so it can stage a voice exactly at the block
 * boundary, without exposing emulator internals in the runtime API.
 *
 * Build/run: ctest -R spu_end_without_repeat_test
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

static void fill_block(uint32_t addr, uint8_t flags, uint8_t packed) {
    spu_ram[addr] = 0x00; /* filter 0, shift 0 */
    spu_ram[addr + 1u] = flags;
    memset(spu_ram + addr + 2u, packed, 14u);
}

static SpuVoice *stage_end_boundary(uint8_t flags, uint32_t repeat_addr) {
    SpuVoice *v = &voices[0];
    memset(v, 0, sizeof(*v));
    v->active = 1;
    v->cur_addr = 0x310u; /* the block after the terminator */
    v->repeat_addr = repeat_addr;
    v->sample_idx = SPU_BLOCK_SAMPLES;
    v->env_level = 0x6000u;
    v->adsr_phase = ADSR_SUSTAIN;
    v->flags = flags;
    spu_regs[2] = 0x1000u; /* one decoded sample per output sample */
    return v;
}

static void test_end_without_repeat_is_silent(void) {
    const uint32_t repeat_addr = 0x120u;
    spu_init();

    /* Distinguish the required repeat-address decode from an incorrect
     * forward decode. Both blocks are deliberately non-silent: the envelope,
     * rather than convenient zero sample data, must make the result silent. */
    fill_block(repeat_addr, 0x04u, 0x11u);
    fill_block(0x310u, 0x00u, 0x77u);
    SpuVoice *v = stage_end_boundary(0x01u, repeat_addr);

    int16_t sample = voice_next_sample(0);

    CHECK(sample == 0, "END without REPEAT emits silence at the boundary");
    CHECK(v->env_level == 0, "END without REPEAT forces the envelope to zero");
    CHECK(v->adsr_phase == ADSR_RELEASE,
          "END without REPEAT enters Release");
    CHECK(!v->active, "zero-level Release deactivates the voice");
    CHECK(v->samples[0] == 4096,
          "decoder resumes at the latched repeat address, not forward RAM");
    CHECK(spu_event_total() == 1, "END stop records exactly one event");

    SpuEvent event;
    CHECK(spu_event_get(&event, 1) == 1 && event.kind == SPU_EV_END_STOP,
          "END stop event has the stop kind");
    CHECK(event.addr == repeat_addr,
          "END stop event reports the redirected repeat address");

    (void)voice_next_sample(0);
    CHECK(spu_event_total() == 1,
          "an inactive one-shot does not retrigger its END transition");
}

static void test_end_with_repeat_keeps_playing(void) {
    const uint32_t repeat_addr = 0x180u;
    spu_init();
    fill_block(repeat_addr, 0x00u, 0x11u);
    SpuVoice *v = stage_end_boundary(0x03u, repeat_addr);

    int16_t sample = voice_next_sample(0);

    CHECK(sample != 0, "END with REPEAT remains audible");
    CHECK(v->env_level != 0, "END with REPEAT preserves the envelope");
    CHECK(v->adsr_phase == ADSR_SUSTAIN,
          "END with REPEAT preserves the ADSR phase");
    CHECK(v->active, "END with REPEAT keeps the voice active");

    SpuEvent event;
    CHECK(spu_event_get(&event, 1) == 1 && event.kind == SPU_EV_END_LOOP,
          "END with REPEAT records a loop event");
}

int main(void) {
    test_end_without_repeat_is_silent();
    test_end_with_repeat_keeps_playing();
    printf(failures ? "FAILED (%d)\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
