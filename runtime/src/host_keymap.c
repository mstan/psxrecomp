/* host_keymap.c — config.ini [KeyMap] for host volume hotkeys.
 *
 * Value format matches recomp-ui / MetalWarriors ParseKeyArray:
 *   [Ctrl+][Alt+][Shift+]<SDL_GetKeyName>
 * Comma-separated multi-binds are accepted. Empty / unknown => leave slot
 * unbound; after parse, empty VolumeUp/Down fall back to Keypad +/-.
 */

#include "host_keymap.h"
#include "psx_sdl.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOST_KEYMAP_MAX_BINDS 4

typedef struct HostKeyBind {
    int keycode;
    int scancode;
    int mods; /* KMOD_CTRL | KMOD_ALT | KMOD_SHIFT subset */
} HostKeyBind;

typedef struct HostKeyAction {
    HostKeyBind binds[HOST_KEYMAP_MAX_BINDS];
    int count;
    /* 1 when [KeyMap] names this action with an empty / "None" / "(unbound)"
     * value: the user cleared it, so apply_defaults must NOT put the built-in
     * key back. A MISSING line still gets the default. */
    int explicit_unbound;
} HostKeyAction;


static HostKeyAction s_actions[HOST_KEYMAP_ACTION_COUNT];

static int want_default(HostKeymapAction action) {
    return s_actions[action].count == 0 && !s_actions[action].explicit_unbound;
}

static int ieq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

static int starts_ci(const char *s, const char *pfx) {
    if (!s || !pfx) return 0;
    while (*pfx) {
        if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*pfx))
            return 0;
        ++s;
        ++pfx;
    }
    return 1;
}

static void trim_inplace(char *s) {
    char *e;
    if (!s) return;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        memmove(s, s + 1, strlen(s));
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ||
                     e[-1] == '\n'))
        *--e = 0;
}

static void clear_all(void) {
    memset(s_actions, 0, sizeof(s_actions));
}

static void add_bind(HostKeymapAction action, int keycode, int scancode, int mods) {
    HostKeyAction *a;
    if (action < 0 || action >= HOST_KEYMAP_ACTION_COUNT) return;
    if (keycode == 0 || keycode == SDLK_UNKNOWN) return;
    a = &s_actions[action];
    if (a->count >= HOST_KEYMAP_MAX_BINDS) return;
    a->binds[a->count].keycode = keycode;
    a->binds[a->count].scancode = scancode;
    a->binds[a->count].mods = mods;
    a->count++;
}

static void apply_defaults(void) {
    if (want_default(HOST_KEYMAP_FULLSCREEN)) {
        add_bind(HOST_KEYMAP_FULLSCREEN, (int)SDLK_RETURN, (int)SDL_SCANCODE_RETURN, KMOD_ALT);
        add_bind(HOST_KEYMAP_FULLSCREEN, (int)SDLK_f, (int)SDL_SCANCODE_F, KMOD_CTRL);
    }
    if (want_default(HOST_KEYMAP_TURBO))
        add_bind(HOST_KEYMAP_TURBO, (int)SDLK_TAB, (int)SDL_SCANCODE_TAB, 0);
    if (want_default(HOST_KEYMAP_VOLUME_UP))
        add_bind(HOST_KEYMAP_VOLUME_UP, (int)SDLK_KP_PLUS, (int)SDL_SCANCODE_KP_PLUS, 0);
    if (want_default(HOST_KEYMAP_VOLUME_DOWN))
        add_bind(HOST_KEYMAP_VOLUME_DOWN, (int)SDLK_KP_MINUS, (int)SDL_SCANCODE_KP_MINUS, 0);
    if (want_default(HOST_KEYMAP_DISPLAY_PERF))
        add_bind(HOST_KEYMAP_DISPLAY_PERF, (int)SDLK_f, (int)SDL_SCANCODE_F, 0);
#if defined(PSX_HAS_RBENGINE_SNAP)
    if (want_default(HOST_KEYMAP_REWIND))
        add_bind(HOST_KEYMAP_REWIND, (int)SDLK_F8, (int)SDL_SCANCODE_F8, 0);
#endif
    if (want_default(HOST_KEYMAP_SAVE_STATE_MENU))
        add_bind(HOST_KEYMAP_SAVE_STATE_MENU, (int)SDLK_F7, (int)SDL_SCANCODE_F7, 0);
    if (want_default(HOST_KEYMAP_SCANLINES))
        add_bind(HOST_KEYMAP_SCANLINES, (int)SDLK_F6, (int)SDL_SCANCODE_F6, 0);
    if (want_default(HOST_KEYMAP_TURBO_TOGGLE))
        add_bind(HOST_KEYMAP_TURBO_TOGGLE, (int)SDLK_F9, (int)SDL_SCANCODE_F9, 0);
}

/* Parse one "Ctrl+Alt+PageUp" token into key+mods. */
static void parse_one_token(HostKeymapAction action, char *tok) {
    int mods = 0;
    SDL_Keycode key;
    SDL_Scancode sc;
    trim_inplace(tok);
    if (!tok[0] || ieq(tok, "(unbound)") || ieq(tok, "None")) return;
    for (;;) {
        if (starts_ci(tok, "Shift+")) {
            mods |= KMOD_SHIFT;
            tok += 6;
        } else if (starts_ci(tok, "Ctrl+")) {
            mods |= KMOD_CTRL;
            tok += 5;
        } else if (starts_ci(tok, "Alt+")) {
            mods |= KMOD_ALT;
            tok += 4;
        } else {
            break;
        }
    }
    trim_inplace(tok);
    if (!tok[0]) return;
    key = SDL_GetKeyFromName(tok);
    if (key == SDLK_UNKNOWN) {
        fprintf(stderr, "host_keymap: unknown key '%s'\n", tok);
        return;
    }
    sc = SDL_GetScancodeFromName(tok);
    add_bind(action, (int)key, (int)sc, mods);
}

static void parse_value(HostKeymapAction action, const char *value) {
    char buf[256];
    char *p;
    char *comma;
    if (!value) return;
    snprintf(buf, sizeof(buf), "%s", value);
    p = buf;
    while (p && *p) {
        comma = strchr(p, ',');
        if (comma) *comma = 0;
        parse_one_token(action, p);
        p = comma ? comma + 1 : NULL;
    }
}

static HostKeymapAction action_for_key(const char *name) {
    if (ieq(name, "Fullscreen")) return HOST_KEYMAP_FULLSCREEN;
    if (ieq(name, "Turbo")) return HOST_KEYMAP_TURBO;
    if (ieq(name, "VolumeUp")) return HOST_KEYMAP_VOLUME_UP;
    if (ieq(name, "VolumeDown")) return HOST_KEYMAP_VOLUME_DOWN;
    if (ieq(name, "DisplayPerf")) return HOST_KEYMAP_DISPLAY_PERF;
    if (ieq(name, "Rewind")) return HOST_KEYMAP_REWIND;
    if (ieq(name, "SaveStateMenu")) return HOST_KEYMAP_SAVE_STATE_MENU;
    if (ieq(name, "Scanlines")) return HOST_KEYMAP_SCANLINES;
    if (ieq(name, "TurboToggle")) return HOST_KEYMAP_TURBO_TOGGLE;
    return HOST_KEYMAP_ACTION_COUNT;
}

void host_keymap_load(const char *config_ini_path) {
    FILE *f;
    char line[512];
    int in_keymap = 0;

    clear_all();
    if (!config_ini_path || !config_ini_path[0]) {
        apply_defaults();
        return;
    }
    f = fopen(config_ini_path, "rb");
    if (!f) {
        apply_defaults();
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        char *eq;
        char *key;
        char *val;
        HostKeymapAction act;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == 0 || *p == ';' || *p == '#') continue;
        if (*p == '[') {
            char *end = strchr(p, ']');
            if (end) *end = 0;
            in_keymap = ieq(p + 1, "KeyMap");
            continue;
        }
        if (!in_keymap) continue;
        eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        key = p;
        val = eq + 1;
        trim_inplace(key);
        trim_inplace(val);
        act = action_for_key(key);
        if (act == HOST_KEYMAP_ACTION_COUNT) continue;
        /* Explicit empty / "None" unbinds: the user cleared it, so no
         * default fallback for that action (explicit_unbound). */
        s_actions[act].count = 0;
        s_actions[act].explicit_unbound = 0;
        parse_value(act, val);
        if (s_actions[act].count == 0)
            s_actions[act].explicit_unbound = 1;
    }
    fclose(f);
    apply_defaults();
}

int host_keymap_match_event(HostKeymapAction action, int keycode,
                            int scancode, int mod) {
    const HostKeyAction *a;
    const int relevant = (int)(KMOD_CTRL | KMOD_ALT | KMOD_SHIFT);
    int i;
    if (action < 0 || action >= HOST_KEYMAP_ACTION_COUNT) return 0;
    a = &s_actions[action];
    for (i = 0; i < a->count; i++) {
        if (a->binds[i].keycode != keycode &&
            (scancode <= 0 || a->binds[i].scancode != scancode))
            continue;
        if ((mod & relevant) == a->binds[i].mods) return 1;
    }
    return 0;
}

int host_keymap_match(HostKeymapAction action, int keycode, int mod) {
    return host_keymap_match_event(action, keycode, 0, mod);
}

int host_keymap_down(HostKeymapAction action, const uint8_t *keys, int mod) {
    const HostKeyAction *a;
    const int relevant = (int)(KMOD_CTRL | KMOD_ALT | KMOD_SHIFT);
    int i;
    if (!keys || action < 0 || action >= HOST_KEYMAP_ACTION_COUNT) return 0;
    a = &s_actions[action];
    for (i = 0; i < a->count; i++) {
        const int sc = a->binds[i].scancode;
        if (sc <= 0 || sc >= SDL_NUM_SCANCODES) continue;
        if (!keys[sc]) continue;
        if ((mod & relevant) == a->binds[i].mods) return 1;
    }
    return 0;
}

/* Rewind overlay FONT8 only draws ASCII 32..90 (space..Z); lowercase is
 * uppercased by the drawer. Map punctuation / odd SDL single-glyph names to
 * short tokens so binds like backtick don't render as "?". */
static const char *overlay_safe_key_token(const char *keyname) {
    if (!keyname || !keyname[0])
        return "?";
    if (!keyname[1]) {
        switch (keyname[0]) {
        case '`': case '~': return "GRAVE";
        case '-': case '_': return "MINUS";
        case '=': return "EQUAL";
        case '+': return "PLUS";
        case '[': case '{': return "LBRK";
        case ']': case '}': return "RBRK";
        case '\\': case '|': return "BSLH";
        case ';': case ':': return "SEMI";
        case '\'': case '"': return "APOS";
        case ',': case '<': return "COMMA";
        case '.': case '>': return "DOT";
        case '/': case '?': return "SLASH";
        default: break;
        }
    }
    return keyname;
}

/* Copy SDL key name into dst, uppercasing a-z and replacing any byte outside
 * the overlay glyph range with '?'. */
static void append_overlay_safe(char *out, size_t cap, size_t *n,
                                const char *keyname) {
    const char *tok = overlay_safe_key_token(keyname);
    size_t i;
    for (i = 0; tok[i] && *n + 1 < cap; i++) {
        unsigned char c = (unsigned char)tok[i];
        if (c >= 'a' && c <= 'z')
            c = (unsigned char)(c - 32);
        if (c < 32 || c > 90)
            c = '?';
        out[(*n)++] = (char)c;
    }
    if (*n < cap)
        out[*n] = 0;
    else if (cap)
        out[cap - 1] = 0;
}

const char *host_keymap_label(HostKeymapAction action, char *out, size_t cap) {
    const HostKeyAction *a;
    const HostKeyBind *b;
    const char *keyname;
    size_t n = 0;
    if (!out || cap == 0) return "";
    out[0] = 0;
    if (action < 0 || action >= HOST_KEYMAP_ACTION_COUNT) return out;
    a = &s_actions[action];
    if (a->count <= 0) {
        if (a->explicit_unbound)
            return out;
#if defined(PSX_HAS_RBENGINE_SNAP)
        if (action == HOST_KEYMAP_REWIND)
            snprintf(out, cap, "F8");
        else
#endif
        if (action == HOST_KEYMAP_SAVE_STATE_MENU)
            snprintf(out, cap, "F7");
        return out;
    }
    b = &a->binds[0];
    if (b->mods & KMOD_CTRL) {
        n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "Ctrl+");
    }
    if (b->mods & KMOD_ALT) {
        n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "Alt+");
    }
    if (b->mods & KMOD_SHIFT) {
        n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "Shift+");
    }
    keyname = SDL_GetKeyName((SDL_Keycode)b->keycode);
    append_overlay_safe(out, cap, &n, keyname);
    return out;
}
