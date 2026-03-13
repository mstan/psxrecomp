/* input_script.cpp — Scripted input system for automation.
 *
 * Parses a simple text script at load time, then executes one command per
 * frame via script_tick().  Supports turbo control, frame waits, RAM-polled
 * waits with timeout, assertions, button presses, screenshots, and exit. */

#include "automation.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <ctime>
#include <vector>
#include <string>

/* ── Button name → PS1 bitmask ─────────────────────────────────────────── */
static uint16_t parse_button(const char* name) {
    if (!strcmp(name, "UP"))       return 0x0010;
    if (!strcmp(name, "DOWN"))     return 0x0040;
    if (!strcmp(name, "LEFT"))     return 0x0080;
    if (!strcmp(name, "RIGHT"))    return 0x0020;
    if (!strcmp(name, "CROSS"))    return 0x4000;
    if (!strcmp(name, "SQUARE"))   return 0x8000;
    if (!strcmp(name, "TRIANGLE")) return 0x1000;
    if (!strcmp(name, "CIRCLE"))   return 0x2000;
    if (!strcmp(name, "START"))    return 0x0008;
    if (!strcmp(name, "SELECT"))   return 0x0001;
    if (!strcmp(name, "L1"))       return 0x0400;
    if (!strcmp(name, "L2"))       return 0x0100;
    if (!strcmp(name, "R1"))       return 0x0800;
    if (!strcmp(name, "R2"))       return 0x0200;
    return 0;
}

/* ── Trigger system — edge-triggered parallel rules ────────────────────── */
enum TriggerCond { TRIG_MEM16, TRIG_MEM8 };
enum TriggerAct  { TACT_PRESS, TACT_LOG, TACT_SCREENSHOT, TACT_TURBO_ON, TACT_TURBO_OFF };

struct Trigger {
    std::string label;
    TriggerCond cond;
    uint32_t    mem_offset;
    uint32_t    mem_value;
    int         delay;          /* frames to wait after edge before acting */
    bool        once;           /* if true, fire at most once ever */

    TriggerAct  action;
    uint16_t    btn;            /* for TACT_PRESS */
    std::string act_str;        /* for TACT_LOG / TACT_SCREENSHOT */

    /* runtime state */
    bool was_true;              /* previous condition result (edge detect) */
    int  countdown;             /* -1 = idle, >=0 = counting down */
    int  press_frames;          /* countdown for press release */
    bool spent;                 /* true if once-trigger already fired */
};

static std::vector<Trigger> s_triggers;

/* Forward declaration — defined after statics that it references */
static void triggers_eval(uint32_t ps1_frame, uint8_t* ram, uint8_t* scratch);

/* ── Command types ─────────────────────────────────────────────────────── */
enum CmdType {
    CMD_TURBO_ON, CMD_TURBO_OFF,
    CMD_WAIT,
    CMD_WAIT_MEM16, CMD_WAIT_MEM8,
    CMD_WAIT_SCRATCH16, CMD_WAIT_SCRATCH8,
    CMD_ASSERT_MEM16, CMD_ASSERT_MEM8,
    CMD_ASSERT_SCRATCH16, CMD_ASSERT_SCRATCH8,
    CMD_PRESS, CMD_HOLD, CMD_RELEASE,
    CMD_SCREENSHOT, CMD_LOG, CMD_EXIT,
    CMD_INJECT_SNAPSHOT,
};

struct ScriptCmd {
    CmdType type;
    uint32_t arg1;          /* frames / ram_offset / button mask / exit code */
    uint32_t arg2;          /* expected value */
    std::string str;        /* label / filename / log message */
};

/* ── Script state ──────────────────────────────────────────────────────── */
static std::vector<ScriptCmd> s_cmds;
static int      s_pc         = 0;
static bool     s_loaded     = false;
static bool     s_done       = false;
static int      s_exit_code  = -1;

/* Script-level defaults for triggers (set via "set" directive) */
static int  s_default_trigger_delay = 0;
static bool s_default_trigger_once  = false;

/* Wait state */
static int      s_wait_frames = 0;
static clock_t  s_wait_wall   = 0;   /* wall-clock safety timeout for mem waits */

/* Pad override */
static uint16_t s_pad_mask   = 0;
static int      s_pad_active = 0;    /* nonzero = script is controlling pad */
static int      s_press_frames = 0;  /* countdown for press command */

/* Screenshot request */
static bool     s_shot_pending = false;
static char     s_shot_path[512] = {};

/* ── Trigger evaluation (runs every frame, parallel to sequential script) ── */
static void triggers_eval(uint32_t ps1_frame, uint8_t* ram, uint8_t* scratch) {
    (void)scratch; /* triggers currently only use ram; scratch reserved for future */
    for (auto& t : s_triggers) {
        if (t.spent) continue;

        /* Evaluate condition */
        bool now_true = false;
        if (t.cond == TRIG_MEM16) {
            uint16_t v = (uint16_t)(ram[t.mem_offset] | (ram[t.mem_offset + 1] << 8));
            now_true = (v == (uint16_t)t.mem_value);
        } else {
            now_true = (ram[t.mem_offset] == (uint8_t)t.mem_value);
        }

        /* Edge detect: false→true starts the countdown */
        if (now_true && !t.was_true) {
            t.countdown = t.delay;
            printf("[TRIGGER] '%s' armed (frame %u, delay %d)\n",
                   t.label.c_str(), ps1_frame, t.delay);
            fflush(stdout);
        }

        /* If condition goes false while counting down, cancel */
        if (!now_true && t.countdown >= 0) {
            t.countdown = -1;
        }

        t.was_true = now_true;

        /* Tick countdown */
        if (t.countdown > 0) {
            t.countdown--;
            continue;
        }
        if (t.countdown != 0) continue; /* -1 = idle */

        /* Fire! */
        t.countdown = -1;
        printf("[TRIGGER] '%s' FIRED (frame %u)\n", t.label.c_str(), ps1_frame);
        fflush(stdout);

        switch (t.action) {
        case TACT_PRESS:
            s_pad_mask |= t.btn;
            t.press_frames = 2;
            break;
        case TACT_LOG:
            printf("[TRIGGER] %s (frame %u)\n", t.act_str.c_str(), ps1_frame);
            fflush(stdout);
            break;
        case TACT_SCREENSHOT:
            snprintf(s_shot_path, sizeof(s_shot_path), "C:/temp/%s", t.act_str.c_str());
            s_shot_pending = true;
            break;
        case TACT_TURBO_ON:  g_turbo = 1; break;
        case TACT_TURBO_OFF: g_turbo = 0; break;
        }

        if (t.once) t.spent = true;
    }

    /* Tick trigger press auto-release */
    for (auto& t : s_triggers) {
        if (t.press_frames > 0) {
            t.press_frames--;
            if (t.press_frames == 0) s_pad_mask &= ~t.btn;
        }
    }
}

/* ── Parsing ───────────────────────────────────────────────────────────── */
static char* skip_ws(char* p) { while (*p == ' ' || *p == '\t') p++; return p; }

static char* next_token(char* p, char* out, int outlen) {
    p = skip_ws(p);
    int i = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && i < outlen - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return p;
}

static char* rest_of_line(char* p, char* out, int outlen) {
    p = skip_ws(p);
    int i = 0;
    while (*p && *p != '\n' && *p != '\r' && i < outlen - 1)
        out[i++] = *p++;
    out[i] = '\0';
    /* trim trailing whitespace */
    while (i > 0 && (out[i-1] == ' ' || out[i-1] == '\t')) out[--i] = '\0';
    return p;
}

int script_load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[SCRIPT] Cannot open: %s\n", path);
        return 0;
    }
    printf("[SCRIPT] Loading: %s\n", path);

    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char* p = skip_ws(line);
        if (*p == '#' || *p == '\0' || *p == '\n' || *p == '\r') continue;

        char cmd[64] = {};
        p = next_token(p, cmd, sizeof(cmd));

        /* Uppercase the command */
        for (char* c = cmd; *c; c++) *c = (char)toupper((unsigned char)*c);

        ScriptCmd sc = {};
        char tok1[128] = {}, tok2[128] = {}, tok3[512] = {};

        if (!strcmp(cmd, "SET")) {
            /* set trigger_delay <N>   — default delay for all subsequent triggers
             * set trigger_once on|off — default once for all subsequent triggers */
            p = next_token(p, tok1, sizeof(tok1));
            for (char* c = tok1; *c; c++) *c = (char)toupper((unsigned char)*c);
            if (!strcmp(tok1, "TRIGGER_DELAY")) {
                p = next_token(p, tok2, sizeof(tok2));
                s_default_trigger_delay = (int)strtol(tok2, NULL, 0);
                printf("[SCRIPT] set trigger_delay = %d\n", s_default_trigger_delay);
            } else if (!strcmp(tok1, "TRIGGER_ONCE")) {
                p = next_token(p, tok2, sizeof(tok2));
                for (char* c = tok2; *c; c++) *c = (char)toupper((unsigned char)*c);
                s_default_trigger_once = !strcmp(tok2, "ON") || !strcmp(tok2, "TRUE") || !strcmp(tok2, "1");
                printf("[SCRIPT] set trigger_once = %s\n", s_default_trigger_once ? "true" : "false");
            } else {
                fprintf(stderr, "[SCRIPT] Line %d: unknown setting '%s'\n", lineno, tok1);
            }
            fflush(stdout);
            continue;
        } else if (!strcmp(cmd, "TURBO")) {
            p = next_token(p, tok1, sizeof(tok1));
            for (char* c = tok1; *c; c++) *c = (char)toupper((unsigned char)*c);
            sc.type = !strcmp(tok1, "ON") ? CMD_TURBO_ON : CMD_TURBO_OFF;
        } else if (!strcmp(cmd, "WAIT_MEM16") || !strcmp(cmd, "WAIT_MEM8")) {
            sc.type = !strcmp(cmd, "WAIT_MEM16") ? CMD_WAIT_MEM16 : CMD_WAIT_MEM8;
            p = next_token(p, tok1, sizeof(tok1));
            p = next_token(p, tok2, sizeof(tok2));
            sc.arg1 = (uint32_t)strtoul(tok1, NULL, 0);
            sc.arg2 = (uint32_t)strtoul(tok2, NULL, 0);
        } else if (!strcmp(cmd, "WAIT_SCRATCH16") || !strcmp(cmd, "WAIT_SCRATCH8")) {
            sc.type = !strcmp(cmd, "WAIT_SCRATCH16") ? CMD_WAIT_SCRATCH16 : CMD_WAIT_SCRATCH8;
            p = next_token(p, tok1, sizeof(tok1));
            p = next_token(p, tok2, sizeof(tok2));
            sc.arg1 = (uint32_t)strtoul(tok1, NULL, 0);
            sc.arg2 = (uint32_t)strtoul(tok2, NULL, 0);
        } else if (!strcmp(cmd, "ASSERT_MEM16") || !strcmp(cmd, "ASSERT_MEM8")) {
            sc.type = !strcmp(cmd, "ASSERT_MEM16") ? CMD_ASSERT_MEM16 : CMD_ASSERT_MEM8;
            p = next_token(p, tok1, sizeof(tok1));
            p = next_token(p, tok2, sizeof(tok2));
            sc.arg1 = (uint32_t)strtoul(tok1, NULL, 0);
            sc.arg2 = (uint32_t)strtoul(tok2, NULL, 0);
            rest_of_line(p, tok3, sizeof(tok3));
            sc.str = tok3;
        } else if (!strcmp(cmd, "ASSERT_SCRATCH16") || !strcmp(cmd, "ASSERT_SCRATCH8")) {
            sc.type = !strcmp(cmd, "ASSERT_SCRATCH16") ? CMD_ASSERT_SCRATCH16 : CMD_ASSERT_SCRATCH8;
            p = next_token(p, tok1, sizeof(tok1));
            p = next_token(p, tok2, sizeof(tok2));
            sc.arg1 = (uint32_t)strtoul(tok1, NULL, 0);
            sc.arg2 = (uint32_t)strtoul(tok2, NULL, 0);
            rest_of_line(p, tok3, sizeof(tok3));
            sc.str = tok3;
        } else if (!strcmp(cmd, "WAIT")) {
            p = next_token(p, tok1, sizeof(tok1));
            sc.type = CMD_WAIT;
            sc.arg1 = (uint32_t)strtoul(tok1, NULL, 0);
        } else if (!strcmp(cmd, "PRESS") || !strcmp(cmd, "HOLD") || !strcmp(cmd, "RELEASE")) {
            p = next_token(p, tok1, sizeof(tok1));
            for (char* c = tok1; *c; c++) *c = (char)toupper((unsigned char)*c);
            uint16_t btn = parse_button(tok1);
            if (!btn) {
                fprintf(stderr, "[SCRIPT] Line %d: unknown button '%s'\n", lineno, tok1);
                continue;
            }
            sc.arg1 = btn;
            if (!strcmp(cmd, "PRESS"))        sc.type = CMD_PRESS;
            else if (!strcmp(cmd, "HOLD"))    sc.type = CMD_HOLD;
            else                              sc.type = CMD_RELEASE;
        } else if (!strcmp(cmd, "SCREENSHOT")) {
            sc.type = CMD_SCREENSHOT;
            rest_of_line(p, tok1, sizeof(tok1));
            if (tok1[0]) sc.str = tok1;
            else         sc.str = "script_shot.png";
        } else if (!strcmp(cmd, "LOG")) {
            sc.type = CMD_LOG;
            rest_of_line(p, tok3, sizeof(tok3));
            sc.str = tok3;
        } else if (!strcmp(cmd, "EXIT")) {
            sc.type = CMD_EXIT;
            p = next_token(p, tok1, sizeof(tok1));
            sc.arg1 = tok1[0] ? (uint32_t)strtoul(tok1, NULL, 0) : 0;
        } else if (!strcmp(cmd, "INJECT-SNAPSHOT") || !strcmp(cmd, "INJECT_SNAPSHOT")) {
            sc.type = CMD_INJECT_SNAPSHOT;
        } else if (!strcmp(cmd, "TRIGGER")) {
            /* trigger <label> when_mem16|when_mem8 <offset> <value> [delay <N>] [once] <action> [args] */
            Trigger trig = {};
            trig.countdown = -1;

            p = next_token(p, tok1, sizeof(tok1));
            trig.label = tok1;

            p = next_token(p, tok1, sizeof(tok1));
            for (char* c = tok1; *c; c++) *c = (char)toupper((unsigned char)*c);
            if (!strcmp(tok1, "WHEN_MEM16"))      trig.cond = TRIG_MEM16;
            else if (!strcmp(tok1, "WHEN_MEM8"))   trig.cond = TRIG_MEM8;
            else {
                fprintf(stderr, "[SCRIPT] Line %d: unknown trigger condition '%s'\n", lineno, tok1);
                continue;
            }

            p = next_token(p, tok1, sizeof(tok1));
            trig.mem_offset = (uint32_t)strtoul(tok1, NULL, 0);
            p = next_token(p, tok1, sizeof(tok1));
            trig.mem_value = (uint32_t)strtoul(tok1, NULL, 0);

            /* Start with script-level defaults; per-trigger keywords override */
            trig.delay = s_default_trigger_delay;
            trig.once = s_default_trigger_once;
            while (true) {
                char peek[64] = {};
                char* saved = p;
                p = next_token(p, peek, sizeof(peek));
                for (char* c = peek; *c; c++) *c = (char)toupper((unsigned char)*c);
                if (!strcmp(peek, "DELAY")) {
                    p = next_token(p, tok1, sizeof(tok1));
                    trig.delay = (int)strtol(tok1, NULL, 0);
                } else if (!strcmp(peek, "ONCE")) {
                    trig.once = true;
                } else {
                    /* Not a modifier — this is the action. Put it back. */
                    p = saved;
                    break;
                }
            }

            /* Action */
            p = next_token(p, tok1, sizeof(tok1));
            for (char* c = tok1; *c; c++) *c = (char)toupper((unsigned char)*c);
            if (!strcmp(tok1, "PRESS")) {
                trig.action = TACT_PRESS;
                p = next_token(p, tok2, sizeof(tok2));
                for (char* c = tok2; *c; c++) *c = (char)toupper((unsigned char)*c);
                trig.btn = parse_button(tok2);
                if (!trig.btn) {
                    fprintf(stderr, "[SCRIPT] Line %d: unknown button '%s' in trigger\n", lineno, tok2);
                    continue;
                }
            } else if (!strcmp(tok1, "LOG")) {
                trig.action = TACT_LOG;
                rest_of_line(p, tok3, sizeof(tok3));
                trig.act_str = tok3;
            } else if (!strcmp(tok1, "SCREENSHOT")) {
                trig.action = TACT_SCREENSHOT;
                rest_of_line(p, tok2, sizeof(tok2));
                trig.act_str = tok2[0] ? tok2 : "trigger_shot.png";
            } else if (!strcmp(tok1, "TURBO")) {
                p = next_token(p, tok2, sizeof(tok2));
                for (char* c = tok2; *c; c++) *c = (char)toupper((unsigned char)*c);
                trig.action = !strcmp(tok2, "ON") ? TACT_TURBO_ON : TACT_TURBO_OFF;
            } else {
                fprintf(stderr, "[SCRIPT] Line %d: unknown trigger action '%s'\n", lineno, tok1);
                continue;
            }

            s_triggers.push_back(trig);
            printf("[SCRIPT] Trigger '%s': %s 0x%X==0x%X delay=%d %s%s\n",
                   trig.label.c_str(),
                   trig.cond == TRIG_MEM16 ? "mem16" : "mem8",
                   trig.mem_offset, trig.mem_value, trig.delay,
                   trig.once ? "once " : "", tok1);
            continue;  /* triggers don't go in s_cmds */
        } else {
            fprintf(stderr, "[SCRIPT] Line %d: unknown command '%s'\n", lineno, cmd);
            continue;
        }

        s_cmds.push_back(sc);
    }
    fclose(f);

    printf("[SCRIPT] Loaded %d commands, %d triggers\n",
           (int)s_cmds.size(), (int)s_triggers.size());
    s_loaded = true;
    s_pc = 0;
    s_done = false;
    s_exit_code = -1;
    s_pad_mask = 0;
    s_pad_active = 1;  /* script controls pad from the start */
    s_press_frames = 0;
    s_shot_pending = false;
    return 1;
}

/* ── Per-frame execution ───────────────────────────────────────────────── */
void script_tick(uint32_t ps1_frame, uint8_t* ram, uint8_t* scratch) {
    if (!s_loaded) return;

    /* Triggers always run, even after sequential script is done */
    triggers_eval(ps1_frame, ram, scratch);

    if (s_done) return;

    /* Press countdown is handled in press_tick() (called from script_get_pad).
     * Do NOT decrement s_press_frames here — it would race with press_tick()
     * and steal the decrement, preventing the bit-clear from ever firing. */

    /* Process commands until one blocks (wait/wait_mem) */
    while (s_pc < (int)s_cmds.size()) {
        ScriptCmd& c = s_cmds[s_pc];

        switch (c.type) {
        case CMD_TURBO_ON:
            g_turbo = 1;
            printf("[SCRIPT] turbo ON (frame %u)\n", ps1_frame);
            fflush(stdout);
            s_pc++;
            break;

        case CMD_TURBO_OFF:
            g_turbo = 0;
            printf("[SCRIPT] turbo OFF (frame %u)\n", ps1_frame);
            fflush(stdout);
            s_pc++;
            break;

        case CMD_WAIT:
            if (s_wait_frames == 0) {
                s_wait_frames = (int)c.arg1;
            }
            s_wait_frames--;
            if (s_wait_frames <= 0) {
                s_wait_frames = 0;
                s_pc++;
                break;  /* continue processing next command this frame */
            }
            return;  /* still waiting — yield */

        case CMD_WAIT_MEM16: {
            uint32_t off = c.arg1;
            uint16_t val = (uint16_t)(ram[off] | (ram[off + 1] << 8));
            if (val == (uint16_t)c.arg2) {
                printf("[SCRIPT] wait_mem16 0x%X == 0x%X satisfied (frame %u)\n",
                       off, c.arg2, ps1_frame);
                fflush(stdout);
                s_wait_wall = 0;
                s_pc++;
                break;
            }
            if (s_wait_wall == 0) s_wait_wall = clock();
            double elapsed = (double)(clock() - s_wait_wall) / CLOCKS_PER_SEC;
            if (elapsed > 30.0) {
                printf("[SCRIPT] TIMEOUT: wait_mem16 0x%X == 0x%X (got 0x%X after %.1fs, frame %u)\n",
                       off, c.arg2, val, elapsed, ps1_frame);
                fflush(stdout);
                s_wait_wall = 0;
                s_pc++;
                break;
            }
            return;
        }

        case CMD_WAIT_MEM8: {
            uint32_t off = c.arg1;
            uint8_t val = ram[off];
            if (val == (uint8_t)c.arg2) {
                printf("[SCRIPT] wait_mem8 0x%X == 0x%X satisfied (frame %u)\n",
                       off, c.arg2, ps1_frame);
                fflush(stdout);
                s_wait_wall = 0;
                s_pc++;
                break;
            }
            if (s_wait_wall == 0) s_wait_wall = clock();
            double elapsed = (double)(clock() - s_wait_wall) / CLOCKS_PER_SEC;
            if (elapsed > 30.0) {
                printf("[SCRIPT] TIMEOUT: wait_mem8 0x%X == 0x%X (got 0x%X after %.1fs, frame %u)\n",
                       off, c.arg2, val, elapsed, ps1_frame);
                fflush(stdout);
                s_wait_wall = 0;
                s_pc++;
                break;
            }
            return;
        }

        case CMD_WAIT_SCRATCH16: {
            uint32_t off = c.arg1;
            uint16_t val = (uint16_t)(scratch[off] | (scratch[off + 1] << 8));
            if (val == (uint16_t)c.arg2) {
                printf("[SCRIPT] wait_scratch16 0x%X == 0x%X satisfied (frame %u)\n",
                       off, c.arg2, ps1_frame);
                fflush(stdout);
                s_wait_wall = 0;
                s_pc++;
                break;
            }
            if (s_wait_wall == 0) s_wait_wall = clock();
            double elapsed_s16 = (double)(clock() - s_wait_wall) / CLOCKS_PER_SEC;
            if (elapsed_s16 > 30.0) {
                printf("[SCRIPT] TIMEOUT: wait_scratch16 0x%X == 0x%X (got 0x%X after %.1fs, frame %u)\n",
                       off, c.arg2, val, elapsed_s16, ps1_frame);
                fflush(stdout);
                s_wait_wall = 0;
                s_pc++;
                break;
            }
            return;
        }

        case CMD_WAIT_SCRATCH8: {
            uint32_t off = c.arg1;
            uint8_t val = scratch[off];
            if (val == (uint8_t)c.arg2) {
                printf("[SCRIPT] wait_scratch8 0x%X == 0x%X satisfied (frame %u)\n",
                       off, c.arg2, ps1_frame);
                fflush(stdout);
                s_wait_wall = 0;
                s_pc++;
                break;
            }
            if (s_wait_wall == 0) s_wait_wall = clock();
            double elapsed_s8 = (double)(clock() - s_wait_wall) / CLOCKS_PER_SEC;
            if (elapsed_s8 > 30.0) {
                printf("[SCRIPT] TIMEOUT: wait_scratch8 0x%X == 0x%X (got 0x%X after %.1fs, frame %u)\n",
                       off, c.arg2, val, elapsed_s8, ps1_frame);
                fflush(stdout);
                s_wait_wall = 0;
                s_pc++;
                break;
            }
            return;
        }

        case CMD_ASSERT_MEM16: {
            uint32_t off = c.arg1;
            uint16_t val = (uint16_t)(ram[off] | (ram[off + 1] << 8));
            if (val == (uint16_t)c.arg2)
                printf("[ASSERT] PASS: %s (0x%X == 0x%X, frame %u)\n",
                       c.str.c_str(), off, c.arg2, ps1_frame);
            else
                printf("[ASSERT] FAIL: %s (0x%X: expected 0x%X, got 0x%X, frame %u)\n",
                       c.str.c_str(), off, c.arg2, val, ps1_frame);
            fflush(stdout);
            s_pc++;
            break;
        }

        case CMD_ASSERT_MEM8: {
            uint32_t off = c.arg1;
            uint8_t val = ram[off];
            if (val == (uint8_t)c.arg2)
                printf("[ASSERT] PASS: %s (0x%X == 0x%X, frame %u)\n",
                       c.str.c_str(), off, c.arg2, ps1_frame);
            else
                printf("[ASSERT] FAIL: %s (0x%X: expected 0x%X, got 0x%X, frame %u)\n",
                       c.str.c_str(), off, c.arg2, val, ps1_frame);
            fflush(stdout);
            s_pc++;
            break;
        }

        case CMD_ASSERT_SCRATCH16: {
            uint32_t off = c.arg1;
            uint16_t val = (uint16_t)(scratch[off] | (scratch[off + 1] << 8));
            if (val == (uint16_t)c.arg2)
                printf("[ASSERT] PASS: %s (scratch[0x%X] == 0x%X, frame %u)\n",
                       c.str.c_str(), off, c.arg2, ps1_frame);
            else
                printf("[ASSERT] FAIL: %s (scratch[0x%X]: expected 0x%X, got 0x%X, frame %u)\n",
                       c.str.c_str(), off, c.arg2, val, ps1_frame);
            fflush(stdout);
            s_pc++;
            break;
        }

        case CMD_ASSERT_SCRATCH8: {
            uint32_t off = c.arg1;
            uint8_t val = scratch[off];
            if (val == (uint8_t)c.arg2)
                printf("[ASSERT] PASS: %s (scratch[0x%X] == 0x%X, frame %u)\n",
                       c.str.c_str(), off, c.arg2, ps1_frame);
            else
                printf("[ASSERT] FAIL: %s (scratch[0x%X]: expected 0x%X, got 0x%X, frame %u)\n",
                       c.str.c_str(), off, c.arg2, val, ps1_frame);
            fflush(stdout);
            s_pc++;
            break;
        }

        case CMD_PRESS:
            s_pad_mask |= (uint16_t)c.arg1;
            s_press_frames = 2;
            printf("[SCRIPT] press 0x%04X (frame %u)\n", c.arg1, ps1_frame);
            fflush(stdout);
            s_pc++;
            return;  /* yield so the press is held for this frame */

        case CMD_HOLD:
            s_pad_mask |= (uint16_t)c.arg1;
            printf("[SCRIPT] hold 0x%04X (frame %u)\n", c.arg1, ps1_frame);
            fflush(stdout);
            s_pc++;
            break;

        case CMD_RELEASE:
            s_pad_mask &= ~(uint16_t)c.arg1;
            printf("[SCRIPT] release 0x%04X (frame %u)\n", c.arg1, ps1_frame);
            fflush(stdout);
            s_pc++;
            break;

        case CMD_SCREENSHOT:
            snprintf(s_shot_path, sizeof(s_shot_path), "C:/temp/%s", c.str.c_str());
            s_shot_pending = true;
            printf("[SCRIPT] screenshot → %s (frame %u)\n", s_shot_path, ps1_frame);
            fflush(stdout);
            s_pc++;
            break;

        case CMD_LOG:
            printf("[SCRIPT] %s (frame %u)\n", c.str.c_str(), ps1_frame);
            fflush(stdout);
            s_pc++;
            break;

        case CMD_INJECT_SNAPSHOT:
            printf("[SCRIPT] inject-snapshot (frame %u)\n", ps1_frame);
            fflush(stdout);
            g_snap_inject_requested = 1;
            s_pc++;
            return;  /* yield so main loop can apply it this frame */

        case CMD_EXIT:
            printf("[SCRIPT] exit %u (frame %u)\n", c.arg1, ps1_frame);
            fflush(stdout);
            s_exit_code = (int)c.arg1;
            s_done = true;
            return;
        }
    }

    /* Fell off the end of script */
    if (s_pc >= (int)s_cmds.size() && !s_done) {
        printf("[SCRIPT] End of script (frame %u)\n", ps1_frame);
        fflush(stdout);
        s_exit_code = 0;
        s_done = true;
    }
}

/* Handle press auto-release: called from script_get_pad each frame */
static void press_tick(void) {
    if (s_press_frames > 0) {
        s_press_frames--;
        if (s_press_frames == 0) {
            /* Find the most recent CMD_PRESS that was executed and clear its bit.
             * Walk backwards from current PC. */
            for (int i = s_pc - 1; i >= 0; i--) {
                if (s_cmds[i].type == CMD_PRESS) {
                    s_pad_mask &= ~(uint16_t)s_cmds[i].arg1;
                    break;
                }
            }
        }
    }
}

int script_get_pad(void) {
    if (!s_loaded) return -1;
    press_tick();
    return (int)s_pad_mask;
}

int script_wants_screenshot(char* buf, int buflen) {
    if (!s_shot_pending) return 0;
    s_shot_pending = false;
    if (buf && buflen > 0) {
        strncpy(buf, s_shot_path, buflen - 1);
        buf[buflen - 1] = '\0';
    }
    return 1;
}

int script_check_exit(void) {
    if (!s_loaded) return -1;
    return s_done ? s_exit_code : -1;
}
