#include "psx_keybinds.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void expect_word(const char *label, uint16_t expected, uint16_t actual) {
    if (expected == actual) return;
    fprintf(stderr, "FAIL %s: expected=0x%04X actual=0x%04X\n",
            label, (unsigned)expected, (unsigned)actual);
    failures++;
}

static void expect_scancode(const char *label, SDL_Scancode expected,
                            SDL_Scancode actual) {
    if (expected == actual) return;
    fprintf(stderr, "FAIL %s: expected=%d actual=%d\n",
            label, (int)expected, (int)actual);
    failures++;
}

int main(int argc, char **argv) {
    uint8_t keys[SDL_NUM_SCANCODES];
    memset(keys, 0, sizeof(keys));

    /* With an argument, exercise the same keybinds.ini the runtime loads. */
    if (argc > 1) psx_keybinds_init(argv[1]);

    expect_scancode("start binding", SDL_SCANCODE_RETURN,
                    psx_keybinds_get_button(1, PSX_KB_START));
    expect_scancode("select binding", SDL_SCANCODE_RSHIFT,
                    psx_keybinds_get_button(1, PSX_KB_SELECT));

    expect_word("released", 0xFFFFu, psx_keybinds_pad_word(keys, 1));

    keys[SDL_SCANCODE_RSHIFT] = 1;
    expect_word("select held", 0xFFFEu, psx_keybinds_pad_word(keys, 1));

    keys[SDL_SCANCODE_RETURN] = 1;
    expect_word("select+start first poll", 0xFFF6u,
                psx_keybinds_pad_word(keys, 1));
    expect_word("select+start consecutive poll", 0xFFF6u,
                psx_keybinds_pad_word(keys, 1));

    keys[SDL_SCANCODE_RETURN] = 0;
    expect_word("start released, select remains", 0xFFFEu,
                psx_keybinds_pad_word(keys, 1));
    keys[SDL_SCANCODE_RETURN] = 1;
    expect_word("start re-pressed with select held", 0xFFF6u,
                psx_keybinds_pad_word(keys, 1));

    if (failures) {
        fprintf(stderr, "keyboard pad chord: %d failure(s)\n", failures);
        return 1;
    }
    fprintf(stderr, "keyboard pad chord: passed\n");
    return 0;
}
