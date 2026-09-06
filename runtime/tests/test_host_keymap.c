#include "host_keymap.h"
#include "psx_sdl.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static int mod_ctrl(void) { return (int)KMOD_CTRL; }
static int mod_alt(void) { return (int)KMOD_ALT; }

int main(int argc, char **argv) {
    uint8_t keys[SDL_NUM_SCANCODES];
    char label[32];
    const char *cfg = "host_keymap_test.ini";
    FILE *f;
    (void)argc;
    (void)argv;

    memset(keys, 0, sizeof(keys));
    host_keymap_load(NULL);
    check(host_keymap_match(HOST_KEYMAP_FULLSCREEN, (int)SDLK_RETURN, mod_alt()),
          "default fullscreen includes Alt+Return");
    check(host_keymap_match(HOST_KEYMAP_FULLSCREEN, (int)SDLK_f, mod_ctrl()),
          "default fullscreen includes Ctrl+F");
    keys[SDL_SCANCODE_TAB] = 1;
    check(host_keymap_down(HOST_KEYMAP_TURBO, keys, 0),
          "default turbo is held by Tab");
    keys[SDL_SCANCODE_TAB] = 0;
    check(host_keymap_match(HOST_KEYMAP_VOLUME_UP, (int)SDLK_KP_PLUS, 0),
          "default volume up is keypad plus");
    check(host_keymap_match(HOST_KEYMAP_VOLUME_DOWN, (int)SDLK_KP_MINUS, 0),
          "default volume down is keypad minus");
    check(host_keymap_match(HOST_KEYMAP_DISPLAY_PERF, (int)SDLK_f, 0),
          "default display perf is F");
    check(host_keymap_match_event(HOST_KEYMAP_DISPLAY_PERF,
                                  (int)SDLK_UNKNOWN,
                                  (int)SDL_SCANCODE_F, 0),
          "default display perf accepts its physical scancode");

    f = fopen(cfg, "wb");
    check(f != NULL, "create temporary config.ini");
    if (!f) return 1;
    fputs("[KeyMap]\n"
          "Fullscreen = F11\n"
          "Turbo = Q\n"
          "VolumeUp = Up\n"
          "VolumeDown = Down\n"
          "DisplayPerf = F10\n",
          f);
    fclose(f);

    host_keymap_load(cfg);
    check(!host_keymap_match(HOST_KEYMAP_FULLSCREEN, (int)SDLK_f, mod_ctrl()),
          "fullscreen rebind disables Ctrl+F fallback");
    check(host_keymap_match(HOST_KEYMAP_FULLSCREEN, (int)SDLK_F11, 0),
          "fullscreen rebind uses F11");
    memset(keys, 0, sizeof(keys));
    keys[SDL_SCANCODE_TAB] = 1;
    check(!host_keymap_down(HOST_KEYMAP_TURBO, keys, 0),
          "turbo rebind disables Tab fallback");
    keys[SDL_SCANCODE_TAB] = 0;
    keys[SDL_SCANCODE_Q] = 1;
    check(host_keymap_down(HOST_KEYMAP_TURBO, keys, 0),
          "turbo rebind uses Q");
    check(host_keymap_match(HOST_KEYMAP_VOLUME_UP, (int)SDLK_UP, 0),
          "volume up rebind uses Up");
    check(host_keymap_match(HOST_KEYMAP_VOLUME_DOWN, (int)SDLK_DOWN, 0),
          "volume down rebind uses Down");
    check(!host_keymap_match(HOST_KEYMAP_DISPLAY_PERF, (int)SDLK_f, 0),
          "display perf rebind disables F fallback");
    check(!host_keymap_match_event(HOST_KEYMAP_DISPLAY_PERF,
                                   (int)SDLK_UNKNOWN,
                                   (int)SDL_SCANCODE_F, 0),
          "display perf rebind disables the old F scancode");
    check(host_keymap_match(HOST_KEYMAP_DISPLAY_PERF, (int)SDLK_F10, 0),
          "display perf rebind uses F10");
    check(host_keymap_match_event(HOST_KEYMAP_DISPLAY_PERF,
                                  (int)SDLK_UNKNOWN,
                                  (int)SDL_SCANCODE_F10, 0),
          "display perf rebind accepts the F10 scancode");

    f = fopen(cfg, "wb");
    check(f != NULL, "create temporary explicit-unbind config.ini");
    if (!f) return 1;
    fputs("[KeyMap]\n"
          "SaveStateMenu = None\n",
          f);
    fclose(f);

    host_keymap_load(cfg);
    check(!host_keymap_match_event(HOST_KEYMAP_SAVE_STATE_MENU,
                                   (int)SDLK_UNKNOWN,
                                   (int)SDL_SCANCODE_F7, 0),
          "explicit save-state-menu unbind disables F7");
    check(host_keymap_label(HOST_KEYMAP_SAVE_STATE_MENU, label,
                            sizeof(label))[0] == 0,
          "explicit save-state-menu unbind has no fallback label");
    check(host_keymap_match(HOST_KEYMAP_FULLSCREEN, (int)SDLK_RETURN,
                            mod_alt()),
          "missing fullscreen line still keeps default after explicit unbind");

    remove(cfg);
    if (failures) {
        fprintf(stderr, "host_keymap_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("PASS: host keymap defaults and rebinds\n");
    return 0;
}
