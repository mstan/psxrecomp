/*
 * psx_keybinds.c — configurable keyboard -> DualShock keybinds, INI-driven.
 *
 * INI lives next to the exe as keybinds.ini. Auto-generated with the framework's
 * historical default keyboard layout when missing, so behaviour is unchanged out
 * of the box. Edit + restart (or rebind live in the launcher's Controls page) to
 * apply. See psx_keybinds.h for the API contract and the PSX pad-word bit layout.
 */
#include "psx_keybinds.h"   /* pulls in psx_sdl.h (SDL2/SDL3 shim) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stddef.h>

/* Mouse-button pseudo-scancodes. Values sit above SDL's keyboard scancode
 * space (SDL_NUM_SCANCODES == 512), so they flow through the existing
 * bind/save/load/rebind machinery as ordinary SDL_Scancode values while
 * held() resolves them against SDL_GetMouseState() instead of the keyboard
 * array. INI names: Mouse1 (left) .. Mouse5 (X2), plus LMB/RMB/MMB aliases. */
#define PSXKB_MOUSE_SC_BASE 512                       /* + SDL_BUTTON_* (1..5) */
#define PSXKB_MOUSE_SC(btn) ((SDL_Scancode)(PSXKB_MOUSE_SC_BASE + (btn)))
#define PSXKB_IS_MOUSE_SC(sc) \
    ((int)(sc) > PSXKB_MOUSE_SC_BASE && (int)(sc) <= PSXKB_MOUSE_SC_BASE + 5)

/* PSX pad word bits (active-low), standard DualShock layout. Matches the
 * PAD_* masks in main.cpp / beetle_main.cpp. */
#define PSXKB_BIT_SELECT   (1u << 0)
#define PSXKB_BIT_L3       (1u << 1)
#define PSXKB_BIT_R3       (1u << 2)
#define PSXKB_BIT_START    (1u << 3)
#define PSXKB_BIT_UP       (1u << 4)
#define PSXKB_BIT_RIGHT    (1u << 5)
#define PSXKB_BIT_DOWN     (1u << 6)
#define PSXKB_BIT_LEFT     (1u << 7)
#define PSXKB_BIT_L2       (1u << 8)
#define PSXKB_BIT_R2       (1u << 9)
#define PSXKB_BIT_L1       (1u << 10)
#define PSXKB_BIT_R1       (1u << 11)
#define PSXKB_BIT_TRIANGLE (1u << 12)
#define PSXKB_BIT_CIRCLE   (1u << 13)
#define PSXKB_BIT_CROSS    (1u << 14)
#define PSXKB_BIT_SQUARE   (1u << 15)

/* ── Defaults ─────────────────────────────────────────────────────────────── */
/*
 * Every player slot uses the framework's historical hardcoded keyboard mapping
 * (pad_from_keyboard / pad_sticks_for in main.cpp):
 *   D-pad: Arrow keys      Start: Return     Select: Right Shift
 *   Cross: X   Circle: S   Square: Z   Triangle: A
 *   L1: Q  R1: W  L2: E  R2: R  L3: T  R3: Y
 *   Left analog stick: Arrow keys
 *   Right analog stick: unbound
 * Simultaneous multi-keyboard play still requires distinct binds per slot.
 */
#define PSXKB_PLAYER_DEFAULTS { \
    .up = SDL_SCANCODE_UP, .down = SDL_SCANCODE_DOWN, \
    .left = SDL_SCANCODE_LEFT, .right = SDL_SCANCODE_RIGHT, \
    .cross = SDL_SCANCODE_X, .circle = SDL_SCANCODE_S, \
    .square = SDL_SCANCODE_Z, .triangle = SDL_SCANCODE_A, \
    .l1 = SDL_SCANCODE_Q, .r1 = SDL_SCANCODE_W, \
    .l2 = SDL_SCANCODE_E, .r2 = SDL_SCANCODE_R, \
    .l3 = SDL_SCANCODE_T, .r3 = SDL_SCANCODE_Y, \
    .start = SDL_SCANCODE_RETURN, .select = SDL_SCANCODE_RSHIFT, \
    .ls_up = SDL_SCANCODE_UP, .ls_down = SDL_SCANCODE_DOWN, \
    .ls_left = SDL_SCANCODE_LEFT, .ls_right = SDL_SCANCODE_RIGHT, \
    .rs_up = SDL_SCANCODE_UNKNOWN, .rs_down = SDL_SCANCODE_UNKNOWN, \
    .rs_left = SDL_SCANCODE_UNKNOWN, .rs_right = SDL_SCANCODE_UNKNOWN, \
}

#define PSXKB_DEFAULTS { \
    .player = { \
        PSXKB_PLAYER_DEFAULTS, \
        PSXKB_PLAYER_DEFAULTS, \
        PSXKB_PLAYER_DEFAULTS, \
        PSXKB_PLAYER_DEFAULTS, \
        PSXKB_PLAYER_DEFAULTS, \
    }, \
}

static PsxKeyBinds       s_binds         = PSXKB_DEFAULTS;
static const PsxKeyBinds s_default_binds = PSXKB_DEFAULTS;
/* Alternate bindings: zero-initialised = SDL_SCANCODE_UNKNOWN = no alt bound.
 * Primary OR alt asserts the input; both persist to keybinds.ini as
 * "primary, alt" on one line. */
static PsxKeyBinds       s_alt_binds;

typedef struct {
    const char *name;   /* ini key */
    const char *label;  /* pretty label for the launcher */
    size_t      offset; /* offset into PsxPlayerBinds */
    uint16_t    bit;    /* PSX pad-word bit, 0 for non-button (stick) inputs */
} ButtonDef;

static const ButtonDef s_buttons[] = {
    { "up",       "Up",         offsetof(PsxPlayerBinds, up),       PSXKB_BIT_UP       },
    { "down",     "Down",       offsetof(PsxPlayerBinds, down),     PSXKB_BIT_DOWN     },
    { "left",     "Left",       offsetof(PsxPlayerBinds, left),     PSXKB_BIT_LEFT     },
    { "right",    "Right",      offsetof(PsxPlayerBinds, right),    PSXKB_BIT_RIGHT    },
    { "cross",    "Cross (X)",  offsetof(PsxPlayerBinds, cross),    PSXKB_BIT_CROSS    },
    { "circle",   "Circle (O)", offsetof(PsxPlayerBinds, circle),   PSXKB_BIT_CIRCLE   },
    { "square",   "Square",     offsetof(PsxPlayerBinds, square),   PSXKB_BIT_SQUARE   },
    { "triangle", "Triangle",   offsetof(PsxPlayerBinds, triangle), PSXKB_BIT_TRIANGLE },
    { "l1",       "L1",         offsetof(PsxPlayerBinds, l1),       PSXKB_BIT_L1       },
    { "r1",       "R1",         offsetof(PsxPlayerBinds, r1),       PSXKB_BIT_R1       },
    { "l2",       "L2",         offsetof(PsxPlayerBinds, l2),       PSXKB_BIT_L2       },
    { "r2",       "R2",         offsetof(PsxPlayerBinds, r2),       PSXKB_BIT_R2       },
    { "l3",       "L3 (stick)", offsetof(PsxPlayerBinds, l3),       PSXKB_BIT_L3       },
    { "r3",       "R3 (stick)", offsetof(PsxPlayerBinds, r3),       PSXKB_BIT_R3       },
    { "start",    "Start",      offsetof(PsxPlayerBinds, start),    PSXKB_BIT_START    },
    { "select",   "Select",     offsetof(PsxPlayerBinds, select),   PSXKB_BIT_SELECT   },
    { "ls_up",    "L-Stick Up",    offsetof(PsxPlayerBinds, ls_up),    0 },
    { "ls_down",  "L-Stick Down",  offsetof(PsxPlayerBinds, ls_down),  0 },
    { "ls_left",  "L-Stick Left",  offsetof(PsxPlayerBinds, ls_left),  0 },
    { "ls_right", "L-Stick Right", offsetof(PsxPlayerBinds, ls_right), 0 },
    { "rs_up",    "R-Stick Up",    offsetof(PsxPlayerBinds, rs_up),    0 },
    { "rs_down",  "R-Stick Down",  offsetof(PsxPlayerBinds, rs_down),  0 },
    { "rs_left",  "R-Stick Left",  offsetof(PsxPlayerBinds, rs_left),  0 },
    { "rs_right", "R-Stick Right", offsetof(PsxPlayerBinds, rs_right), 0 },
};
#define PSXKB_N ((int)(sizeof(s_buttons) / sizeof(s_buttons[0])))

/* ── INI parsing helpers ──────────────────────────────────────────────────── */

static void trim(char *s) {
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n-1])) s[--n] = '\0';
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
}

static SDL_Scancode name_to_scancode(const char *name) {
    if (!name || !*name) return SDL_SCANCODE_UNKNOWN;
    SDL_Scancode sc = SDL_GetScancodeFromName(name);
    if (sc != SDL_SCANCODE_UNKNOWN) return sc;
    char buf[32];
    size_t i = 0;
    for (; name[i] && i < sizeof(buf) - 1; i++) buf[i] = (char)tolower((unsigned char)name[i]);
    buf[i] = '\0';
    if (!strcmp(buf, "enter") || !strcmp(buf, "return")) return SDL_SCANCODE_RETURN;
    if (!strcmp(buf, "tab"))                              return SDL_SCANCODE_TAB;
    if (!strcmp(buf, "space"))                            return SDL_SCANCODE_SPACE;
    if (!strcmp(buf, "lshift"))                           return SDL_SCANCODE_LSHIFT;
    if (!strcmp(buf, "rshift"))                           return SDL_SCANCODE_RSHIFT;
    if (!strcmp(buf, "lctrl"))                            return SDL_SCANCODE_LCTRL;
    if (!strcmp(buf, "rctrl"))                            return SDL_SCANCODE_RCTRL;
    if (!strcmp(buf, "lalt"))                             return SDL_SCANCODE_LALT;
    if (!strcmp(buf, "ralt"))                             return SDL_SCANCODE_RALT;
    if (!strcmp(buf, "backslash"))                        return SDL_SCANCODE_BACKSLASH;
    if (!strcmp(buf, "escape") || !strcmp(buf, "esc"))    return SDL_SCANCODE_ESCAPE;
    if (!strcmp(buf, "backspace"))                        return SDL_SCANCODE_BACKSPACE;
    if (!strcmp(buf, "none") || !strcmp(buf, ""))         return SDL_SCANCODE_UNKNOWN;
    /* Mouse buttons: Mouse1..Mouse5 (SDL button order: 1=left, 2=middle,
     * 3=right, 4=X1, 5=X2) plus the common aliases. */
    if (!strncmp(buf, "mouse", 5) && buf[5] >= '1' && buf[5] <= '5' && !buf[6])
        return PSXKB_MOUSE_SC(buf[5] - '0');
    if (!strcmp(buf, "lmb") || !strcmp(buf, "mouse left"))   return PSXKB_MOUSE_SC(SDL_BUTTON_LEFT);
    if (!strcmp(buf, "mmb") || !strcmp(buf, "mouse middle")) return PSXKB_MOUSE_SC(SDL_BUTTON_MIDDLE);
    if (!strcmp(buf, "rmb") || !strcmp(buf, "mouse right"))  return PSXKB_MOUSE_SC(SDL_BUTTON_RIGHT);
    if (!strcmp(buf, "mouse x1"))                            return PSXKB_MOUSE_SC(SDL_BUTTON_X1);
    if (!strcmp(buf, "mouse x2"))                            return PSXKB_MOUSE_SC(SDL_BUTTON_X2);
    return SDL_SCANCODE_UNKNOWN;
}

static const char *scancode_to_name(SDL_Scancode sc) {
    if (sc == SDL_SCANCODE_UNKNOWN) return "None";
    if (PSXKB_IS_MOUSE_SC(sc)) {
        static const char *mouse_names[5] =
            { "Mouse1", "Mouse2", "Mouse3", "Mouse4", "Mouse5" };
        return mouse_names[(int)sc - PSXKB_MOUSE_SC_BASE - 1];
    }
    const char *name = SDL_GetScancodeName(sc);
    return (name && name[0]) ? name : "None";
}

/* ── File I/O ─────────────────────────────────────────────────────────────── */

static char s_ini_path[1024] = {0};

/* Resolve keybinds.ini alongside exe_path. exe_path may be a file (argv[0] /
 * the exe) or a directory (the exe dir the launcher passes) — either works. */
static void derive_ini_path(const char *exe_path) {
    if (!exe_path || !*exe_path) {
        strcpy(s_ini_path, "keybinds.ini");
        return;
    }
    /* Find the last path separator; if none, treat the whole thing as a dir. */
    const char *slash = NULL;
    for (const char *p = exe_path; *p; p++)
        if (*p == '/' || *p == '\\') slash = p;

    /* Heuristic: a path ending in a separator, or with no extension in the last
     * component, is treated as a directory; otherwise strip the file name. */
    size_t len = strlen(exe_path);
    int ends_sep = (exe_path[len-1] == '/' || exe_path[len-1] == '\\');
    const char *last = slash ? slash + 1 : exe_path;
    int has_ext = strchr(last, '.') != NULL;

    char dir[1024];
    if (ends_sep) {
        snprintf(dir, sizeof(dir), "%s", exe_path);
    } else if (!has_ext) {
        /* directory path without trailing sep */
        snprintf(dir, sizeof(dir), "%s/", exe_path);
    } else if (slash) {
        size_t dl = (size_t)(slash - exe_path) + 1;
        if (dl >= sizeof(dir)) dl = sizeof(dir) - 1;
        memcpy(dir, exe_path, dl);
        dir[dl] = '\0';
    } else {
        dir[0] = '\0';
    }
    snprintf(s_ini_path, sizeof(s_ini_path), "%skeybinds.ini", dir);
}

static void write_player_section(FILE *f, const char *section,
                                 const PsxPlayerBinds *pb,
                                 const PsxPlayerBinds *alt) {
    fprintf(f, "[%s]\n", section);
    for (int i = 0; i < PSXKB_N; i++) {
        SDL_Scancode sc = *(const SDL_Scancode *)((const char *)pb  + s_buttons[i].offset);
        SDL_Scancode al = *(const SDL_Scancode *)((const char *)alt + s_buttons[i].offset);
        if (al != SDL_SCANCODE_UNKNOWN)
            fprintf(f, "%-9s = %s, %s\n", s_buttons[i].name,
                    scancode_to_name(sc), scancode_to_name(al));
        else
            fprintf(f, "%-9s = %s\n", s_buttons[i].name, scancode_to_name(sc));
    }
    fprintf(f, "\n");
}

static void write_ini(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
        "# PSXRecomp Keyboard Keybinds (keyboard -> DualShock).\n"
        "# Edit values and restart, or rebind live in the launcher's Controls page.\n"
        "# Use SDL key names. Common: A B C ... Z, 0-9, F1-F12, Up Down Left Right,\n"
        "# Return, Tab, Space, Left Shift, Right Shift, Left Ctrl, Right Ctrl,\n"
        "# Backspace, Escape, Backslash. Use \"None\" to leave an input unbound.\n"
        "# Mouse buttons also bind: Mouse1 (left), Mouse2 (middle), Mouse3 (right),\n"
        "# Mouse4/Mouse5 (side); aliases LMB, MMB, RMB.\n"
        "# Each input accepts an optional SECOND binding after a comma - both\n"
        "# assert the input:  cross = X, Mouse1\n"
        "#\n"
        "# Buttons: up/down/left/right, cross/circle/square/triangle, l1/r1/l2/r2,\n"
        "# l3/r3 (stick clicks), start/select. ls_* / rs_* are the left/right\n"
        "# analog-stick DIRECTIONS driven from the keyboard (analog pad modes).\n"
        "#\n"
        "# Every player slot defaults to the same keyboard map. Rebind per slot\n"
        "# for simultaneous multi-keyboard play (route a port to \"Keyboard\").\n"
        "\n");
    for (int p = 0; p < PSXKB_MAX_PLAYERS; ++p) {
        char section[16];
        snprintf(section, sizeof(section), "player%d", p + 1);
        write_player_section(f, section, &s_binds.player[p],
                             &s_alt_binds.player[p]);
    }
    fclose(f);
    printf("[Keybinds] Wrote %s\n", path);
}

static int player_all_unbound(const PsxPlayerBinds *pb) {
    for (int i = 0; i < PSXKB_N; i++) {
        SDL_Scancode sc = *(const SDL_Scancode *)((const char *)pb + s_buttons[i].offset);
        if (sc != SDL_SCANCODE_UNKNOWN) return 0;
    }
    return 1;
}

/* Older keybinds.ini left P2+ fully unbound. Promote empty slots to the shared
 * default map so every player can Reset / use keyboard with the P1 layout. */
static void promote_empty_players(void) {
    for (int p = 0; p < PSXKB_MAX_PLAYERS; ++p) {
        if (player_all_unbound(&s_binds.player[p]))
            s_binds.player[p] = s_default_binds.player[0];
    }
}

static void load_ini(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    PsxPlayerBinds *current = NULL, *current_alt = NULL;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end) *end = '\0';
            const char *section = line + 1;
            current = NULL; current_alt = NULL;
            if (!strncmp(section, "player", 6)) {
                int n = atoi(section + 6);
                if (n >= 1 && n <= PSXKB_MAX_PLAYERS) {
                    current = &s_binds.player[n - 1];
                    current_alt = &s_alt_binds.player[n - 1];
                }
            }
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line, *val = eq + 1;
        trim(key); trim(val);
        for (char *c = key; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (!current) continue;
        /* Optional alternate binding after a comma: "cross = X, Mouse1". */
        char *comma = strchr(val, ',');
        char *alt_val = NULL;
        if (comma) { *comma = '\0'; alt_val = comma + 1; trim(val); trim(alt_val); }
        for (int i = 0; i < PSXKB_N; i++) {
            if (!strcmp(key, s_buttons[i].name)) {
                *(SDL_Scancode *)((char *)current + s_buttons[i].offset) =
                    name_to_scancode(val);
                *(SDL_Scancode *)((char *)current_alt + s_buttons[i].offset) =
                    alt_val ? name_to_scancode(alt_val) : SDL_SCANCODE_UNKNOWN;
                break;
            }
        }
    }
    fclose(f);
    promote_empty_players();
    printf("[Keybinds] Loaded %s\n", path);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void psx_keybinds_init(const char *exe_path) {
    derive_ini_path(exe_path);
    FILE *test = fopen(s_ini_path, "r");
    if (test) { fclose(test); load_ini(s_ini_path); }
    else       write_ini(s_ini_path);
}

const PsxKeyBinds *psx_keybinds_get(void) { return &s_binds; }

static const PsxPlayerBinds *player_binds_c(int player) {
    if (player < 1 || player > PSXKB_MAX_PLAYERS) return &s_binds.player[0];
    return &s_binds.player[player - 1];
}
static PsxPlayerBinds *player_binds(int player) {
    if (player < 1 || player > PSXKB_MAX_PLAYERS) return &s_binds.player[0];
    return &s_binds.player[player - 1];
}

/* Is this single scancode (keyboard or mouse pseudo-code) currently held?
 * Mouse pseudo-scancodes MUST be checked before indexing keys[] — they sit
 * beyond the keyboard state array (SDL_NUM_SCANCODES entries). */
static int held_sc(const uint8_t *keys, SDL_Scancode sc) {
    if (PSXKB_IS_MOUSE_SC(sc)) {
        Uint32 m = SDL_GetMouseState(NULL, NULL);
        return (m & SDL_BUTTON((int)sc - PSXKB_MOUSE_SC_BASE)) != 0;
    }
    return sc != SDL_SCANCODE_UNKNOWN && (int)sc < SDL_NUM_SCANCODES && keys[sc];
}

/* Is the input at button-def index i held for this player, via either its
 * primary or its alternate binding? The alternate table uses the same player
 * slot as the primary table. */
static int held(const uint8_t *keys, const PsxPlayerBinds *pb, int i) {
    ptrdiff_t slot = pb - &s_binds.player[0];
    if (slot < 0 || slot >= PSXKB_MAX_PLAYERS) slot = 0;
    const PsxPlayerBinds *alt = &s_alt_binds.player[slot];
    SDL_Scancode p = *(const SDL_Scancode *)((const char *)pb  + s_buttons[i].offset);
    SDL_Scancode a = *(const SDL_Scancode *)((const char *)alt + s_buttons[i].offset);
    return held_sc(keys, p) || held_sc(keys, a);
}

uint16_t psx_keybinds_pad_word(const uint8_t *keys, int player) {
    if (!keys || player < 1 || player > PSXKB_MAX_PLAYERS) return 0xFFFF;
    const PsxPlayerBinds *pb = player_binds_c(player);
    uint16_t b = 0xFFFF;   /* active-low: all released */
    for (int i = 0; i < PSXKB_N; i++) {
        if (s_buttons[i].bit && held(keys, pb, i))
            b &= (uint16_t)~s_buttons[i].bit;
    }
    return b;
}

void psx_keybinds_sticks(const uint8_t *keys, int player, uint8_t out[4]) {
    if (!keys || !out || player < 1 || player > PSXKB_MAX_PLAYERS) return;
    const PsxPlayerBinds *pb = player_binds_c(player);
    if (held(keys, pb, PSX_KB_LS_LEFT))  out[0] = 0x00;
    if (held(keys, pb, PSX_KB_LS_RIGHT)) out[0] = 0xFF;
    if (held(keys, pb, PSX_KB_LS_UP))    out[1] = 0x00;
    if (held(keys, pb, PSX_KB_LS_DOWN))  out[1] = 0xFF;
    if (held(keys, pb, PSX_KB_RS_LEFT))  out[2] = 0x00;
    if (held(keys, pb, PSX_KB_RS_RIGHT)) out[2] = 0xFF;
    if (held(keys, pb, PSX_KB_RS_UP))    out[3] = 0x00;
    if (held(keys, pb, PSX_KB_RS_DOWN))  out[3] = 0xFF;
}

int psx_keybinds_dpad_active(const uint8_t *keys, int player) {
    if (!keys || player < 1 || player > PSXKB_MAX_PLAYERS) return 0;
    const PsxPlayerBinds *pb = player_binds_c(player);
    return held(keys, pb, PSX_KB_UP)   || held(keys, pb, PSX_KB_DOWN) ||
           held(keys, pb, PSX_KB_LEFT) || held(keys, pb, PSX_KB_RIGHT);
}

/* ── Rebind API ───────────────────────────────────────────────────────────── */

int psx_keybinds_button_count(void) { return PSXKB_N; }

const char *psx_keybinds_button_name(int button) {
    if (button < 0 || button >= PSXKB_N) return "?";
    return s_buttons[button].name;
}
const char *psx_keybinds_button_label(int button) {
    if (button < 0 || button >= PSXKB_N) return "?";
    return s_buttons[button].label;
}

SDL_Scancode psx_keybinds_get_button(int player, int button) {
    if (button < 0 || button >= PSXKB_N) return SDL_SCANCODE_UNKNOWN;
    if (player < 1 || player > PSXKB_MAX_PLAYERS) return SDL_SCANCODE_UNKNOWN;
    return *(SDL_Scancode *)((char *)player_binds(player) + s_buttons[button].offset);
}

void psx_keybinds_set_button(int player, int button, SDL_Scancode sc) {
    if (button < 0 || button >= PSXKB_N) return;
    if (player < 1 || player > PSXKB_MAX_PLAYERS) return;
    *(SDL_Scancode *)((char *)player_binds(player) + s_buttons[button].offset) = sc;
}

static PsxPlayerBinds *player_alt_binds(int player) {
    if (player < 1 || player > PSXKB_MAX_PLAYERS)
        return &s_alt_binds.player[0];
    return &s_alt_binds.player[player - 1];
}

SDL_Scancode psx_keybinds_get_button_alt(int player, int button) {
    if (button < 0 || button >= PSXKB_N) return SDL_SCANCODE_UNKNOWN;
    return *(SDL_Scancode *)((char *)player_alt_binds(player) + s_buttons[button].offset);
}

void psx_keybinds_set_button_alt(int player, int button, SDL_Scancode sc) {
    if (button < 0 || button >= PSXKB_N) return;
    *(SDL_Scancode *)((char *)player_alt_binds(player) + s_buttons[button].offset) = sc;
}

void psx_keybinds_reset_player(int player) {
    if (player < 1 || player > PSXKB_MAX_PLAYERS) return;
    *player_binds(player) = s_default_binds.player[0];
    memset(player_alt_binds(player), 0, sizeof(PsxPlayerBinds)); /* alts: unbound */
}

void psx_keybinds_save(void) {
    if (!s_ini_path[0]) strcpy(s_ini_path, "keybinds.ini");
    write_ini(s_ini_path);
}
