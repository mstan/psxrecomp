#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cctype>
#include "game_extras.h"
#include <windows.h>
#include <timeapi.h>    /* timeBeginPeriod / timeEndPeriod */

#include "psx_runtime.h"
#include "cdrom_stub.h"
#include "xa_audio.h"
#include "fmv_player.h"
#include "ps1_exe_parser.h"
#include "opengl_renderer.h"
#include "gpu_interpreter.h"
#include "automation.h"
#include "debug_server.h"
#include "spu.h"
#include <GLFW/glfw3.h>

using namespace PS1;

/* ---------------------------------------------------------------------------
 * Automation globals (declared in automation.h)
 * --------------------------------------------------------------------------- */
int      g_debug_mode = 0;
int      g_turbo    = 0;
uint32_t g_ps1_frame = 0;
int      g_snap_inject_requested = 0;
static const char* s_script_path  = NULL;
static const char* s_record_path  = NULL;
static FILE*       s_record_file  = NULL;

/* Snapshot save/load (--save-snapshot FRAME FILE / --load-snapshot FILE) */
static const char* s_snap_save_path  = NULL;
static uint32_t    s_snap_save_frame = UINT32_MAX;
static const char* s_snap_load_path  = NULL;
static uint16_t    s_rec_last_pad   = 0;
static uint32_t    s_rec_last_frame = 0;
static int         s_rec_last_turbo = 0;

static const char* rec_btn_name(uint16_t bit) {
    switch (bit) {
        case 0x0010: return "UP";
        case 0x0040: return "DOWN";
        case 0x0080: return "LEFT";
        case 0x0020: return "RIGHT";
        case 0x4000: return "CROSS";
        case 0x8000: return "SQUARE";
        case 0x1000: return "TRIANGLE";
        case 0x2000: return "CIRCLE";
        case 0x0008: return "START";
        case 0x0001: return "SELECT";
        case 0x0400: return "L1";
        case 0x0100: return "L2";
        case 0x0800: return "R1";
        case 0x0200: return "R2";
        default:     return NULL;
    }
}

static void record_tick(uint32_t frame, uint16_t pad, int turbo) {
    if (!s_record_path) return;
    bool pad_changed   = (pad   != s_rec_last_pad);
    bool turbo_changed = (turbo != s_rec_last_turbo);
    if (!pad_changed && !turbo_changed) return;

    /* Open file lazily on first change */
    if (!s_record_file) {
        s_record_file = fopen(s_record_path, "w");
        if (!s_record_file) {
            fprintf(stderr, "[REC] Cannot open %s for writing\n", s_record_path);
            s_record_path = NULL;
            return;
        }
        fprintf(s_record_file, "# PSXRecomp input recording\n");
        fprintf(s_record_file, "# Recorded at frame %u\n", frame);
        fflush(s_record_file);
    }

    /* Emit wait since last event */
    uint32_t delta = frame - s_rec_last_frame;
    if (delta > 0)
        fprintf(s_record_file, "wait %u\n", delta);

    /* Turbo change */
    if (turbo_changed)
        fprintf(s_record_file, "turbo %s\n", turbo ? "on" : "off");

    /* Button changes: hold newly-pressed, release newly-released */
    if (pad_changed) {
        uint16_t pressed  = pad & ~s_rec_last_pad;
        uint16_t released = s_rec_last_pad & ~pad;
        for (uint16_t bit = 1; bit; bit <<= 1) {
            if (pressed & bit) {
                const char* name = rec_btn_name(bit);
                if (name) fprintf(s_record_file, "hold %s\n", name);
            }
        }
        for (uint16_t bit = 1; bit; bit <<= 1) {
            if (released & bit) {
                const char* name = rec_btn_name(bit);
                if (name) fprintf(s_record_file, "release %s\n", name);
            }
        }
    }

    fflush(s_record_file);
    s_rec_last_pad   = pad;
    s_rec_last_frame = frame;
    s_rec_last_turbo = turbo;
}

/* ---------------------------------------------------------------------------
 * Generated entry point (in generated/tomba_full.c)
 * --------------------------------------------------------------------------- */
extern "C" void func_8006B58C(CPUState* cpu);

/* ---------------------------------------------------------------------------
 * GPU hook — called by runtime when DMA submits GPU packets.
 * Replace this pointer to route packets to the real renderer.
 * --------------------------------------------------------------------------- */
static OpenGLRenderer* g_renderer = nullptr;
static GPUInterpreter* g_gpu      = nullptr;

/* Set to 1 while inside the DrawOTag (FUN_80060B70) OT walker. */
extern "C" int g_in_drawtag = 0;

/* FMV decoder hook — uploads decoded RGB555 frame to VRAM */
extern "C" void fmv_vram_upload(int x, int y, int w, int h, const uint16_t* data) {
    if (g_renderer) g_renderer->UploadToVRAM(x, y, w, h, data);
}

/* FMV display area override — called before psx_present_frame() during FMV playback.
 * We save the FMV display settings and apply them at Present() time, letting GP1
 * commands flow through normally so the GPU state stays in sync. When FMV ends,
 * the renderer already has the correct game display area from the last GP1(05h). */
static bool g_fmv_presenting = false;  /* true only during FMV player's own present */
static int  g_fmv_disp_x = 0, g_fmv_disp_y = 0;
static int  g_fmv_disp_w = 0, g_fmv_disp_h = 0;

extern "C" void fmv_force_display_area(int x, int y, int w, int h) {
    g_fmv_disp_x = x; g_fmv_disp_y = y;
    g_fmv_disp_w = w; g_fmv_disp_h = h;
    g_fmv_presenting = true;  /* next psx_present_frame is from FMV player */
}

extern "C" void gpu_submit_word(uint32_t word) {
    static uint32_t s_word_count = 0;
    ++s_word_count;
    /* [GP0] first 40 — re-enable when investigating GPU command stream */
    if (g_gpu) g_gpu->WriteGP0(word);
}

extern "C" void gpu_abort_streaming(void) {
    if (g_gpu) g_gpu->AbortStreaming();
}


extern "C" uint32_t gpu_read_word(void) {
    if (g_gpu) return g_gpu->ReadGPUREAD();
    return 0;
}

extern "C" void gpu_write_gp1(uint32_t cmd) {
    static uint32_t s_gp1_count = 0; ++s_gp1_count;
    /* [GP1] first 30 — re-enable: if (s_gp1_count <= 30) printf("[GP1] #%u 0x%08X\n", ...); */
    diag_track_gp1(cmd);
    if (g_gpu) g_gpu->WriteGP1(cmd);
}

/* ---------------------------------------------------------------------------
 * Debug server GPU state accessors — extern "C" for debug_server.c
 * --------------------------------------------------------------------------- */
extern "C" void psx_debug_get_display_area(uint16_t *x, uint16_t *y) {
    if (!g_gpu) { *x = *y = 0; return; }
    /* Access through the GPUSTAT and renderer state.
     * The GPUInterpreter owns the GPUState but it's private.
     * We use the renderer's cached display area instead. */
    if (g_renderer) {
        /* Read the renderer's display_area fields set by SetDisplayArea() */
        *x = (uint16_t)g_renderer->GetDisplayAreaX();
        *y = (uint16_t)g_renderer->GetDisplayAreaY();
    } else {
        *x = *y = 0;
    }
}

extern "C" void psx_debug_get_draw_offset(int16_t *x, int16_t *y) {
    if (g_renderer) {
        *x = (int16_t)g_renderer->GetDrawOffsetX();
        *y = (int16_t)g_renderer->GetDrawOffsetY();
    } else {
        *x = *y = 0;
    }
}

extern "C" void psx_debug_get_display_mode(uint8_t *h_res, uint8_t *v_res, uint8_t *enable) {
    /* Defaults */
    *h_res = 1;   /* 320 */
    *v_res = 0;   /* 240 */
    *enable = 0;  /* on */
}

extern "C" uint32_t psx_debug_get_gpustat(void) {
    if (g_gpu) return g_gpu->ReadGPUSTAT();
    return 0;
}

extern "C" int psx_debug_read_vram(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                    uint16_t *out, int max_pixels) {
    if (!g_renderer) return 0;
    int total = (int)w * (int)h;
    if (total > max_pixels) total = max_pixels;
    g_renderer->DownloadFromVRAM(x, y, w, h, out);
    return total;
}

/* ---------------------------------------------------------------------------
 * Key configuration — loaded from keyconfig.ini next to the executable.
 *
 * PS1 button bit mapping (16-bit word, 1 = pressed):
 *   0x0010 = D-Up     0x0040 = D-Down   0x0080 = D-Left   0x0020 = D-Right
 *   0x0008 = Start    0x0001 = Select
 *   0x1000 = Triangle 0x2000 = Circle   0x4000 = Cross    0x8000 = Square
 *   0x0400 = L1       0x0100 = L2       0x0800 = R1       0x0200 = R2
 * --------------------------------------------------------------------------- */
struct KeyConfig {
    int up, down, left, right;
    int cross, square, triangle, circle;
    int start, select;
    int l1, l2, r1, r2;
};

static KeyConfig g_key_config = {
    /* d-pad  */ GLFW_KEY_UP, GLFW_KEY_DOWN, GLFW_KEY_LEFT, GLFW_KEY_RIGHT,
    /* face   */ GLFW_KEY_S, GLFW_KEY_A, GLFW_KEY_W, GLFW_KEY_D,
    /* meta   */ GLFW_KEY_ENTER, GLFW_KEY_APOSTROPHE,
    /* should */ GLFW_KEY_1, GLFW_KEY_2, GLFW_KEY_0, GLFW_KEY_9,
};

static int ParseKeyName(const char* name) {
    if (!name || !*name) return GLFW_KEY_UNKNOWN;
    /* Single uppercase letter A-Z */
    if (name[1] == '\0' && name[0] >= 'A' && name[0] <= 'Z')
        return GLFW_KEY_A + (name[0] - 'A');
    /* Single digit 0-9 */
    if (name[1] == '\0' && name[0] >= '0' && name[0] <= '9')
        return GLFW_KEY_0 + (name[0] - '0');
    /* Named keys */
    if (!strcmp(name, "UP"))         return GLFW_KEY_UP;
    if (!strcmp(name, "DOWN"))       return GLFW_KEY_DOWN;
    if (!strcmp(name, "LEFT"))       return GLFW_KEY_LEFT;
    if (!strcmp(name, "RIGHT"))      return GLFW_KEY_RIGHT;
    if (!strcmp(name, "ENTER"))      return GLFW_KEY_ENTER;
    if (!strcmp(name, "ESCAPE"))     return GLFW_KEY_ESCAPE;
    if (!strcmp(name, "SPACE"))      return GLFW_KEY_SPACE;
    if (!strcmp(name, "TAB"))        return GLFW_KEY_TAB;
    if (!strcmp(name, "BACKSPACE"))  return GLFW_KEY_BACKSPACE;
    if (!strcmp(name, "APOSTROPHE")) return GLFW_KEY_APOSTROPHE;
    if (!strcmp(name, "SEMICOLON"))  return GLFW_KEY_SEMICOLON;
    if (!strcmp(name, "COMMA"))      return GLFW_KEY_COMMA;
    if (!strcmp(name, "PERIOD"))     return GLFW_KEY_PERIOD;
    if (!strcmp(name, "LSHIFT"))     return GLFW_KEY_LEFT_SHIFT;
    if (!strcmp(name, "RSHIFT"))     return GLFW_KEY_RIGHT_SHIFT;
    if (!strcmp(name, "LCTRL"))      return GLFW_KEY_LEFT_CONTROL;
    if (!strcmp(name, "RCTRL"))      return GLFW_KEY_RIGHT_CONTROL;
    if (!strcmp(name, "LALT"))       return GLFW_KEY_LEFT_ALT;
    if (!strcmp(name, "RALT"))       return GLFW_KEY_RIGHT_ALT;
    if (!strcmp(name, "F1"))         return GLFW_KEY_F1;
    if (!strcmp(name, "F2"))         return GLFW_KEY_F2;
    if (!strcmp(name, "F3"))         return GLFW_KEY_F3;
    if (!strcmp(name, "F4"))         return GLFW_KEY_F4;
    if (!strcmp(name, "F5"))         return GLFW_KEY_F5;
    if (!strcmp(name, "F6"))         return GLFW_KEY_F6;
    if (!strcmp(name, "F7"))         return GLFW_KEY_F7;
    if (!strcmp(name, "F8"))         return GLFW_KEY_F8;
    return GLFW_KEY_UNKNOWN;
}

static void LoadKeyConfig(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        printf("[KeyConfig] Not found at %s — using defaults\n", path);
        return;
    }
    printf("[KeyConfig] Loading from %s\n", path);
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '[' || *p == '\n' || *p == '\r' || *p == '\0')
            continue;

        char key[64] = {}, val[64] = {};
        if (sscanf(p, "%63[^=]= %63s", key, val) != 2) continue;

        /* Trim trailing whitespace from key */
        char* end = key + strlen(key) - 1;
        while (end > key && (*end == ' ' || *end == '\t')) *end-- = '\0';

        /* Uppercase key for matching; keep raw val for numeric fields */
        for (char* c = key; *c; c++) *c = (char)toupper((unsigned char)*c);

        /* Audio section */
        if (!strcmp(key, "VOLUME")) {
            float v = (float)atof(val);
            xa_audio_set_volume(v);
            spu_set_master_volume(v);
            continue;
        }

        /* Uppercase val for key-name matching */
        for (char* c = val; *c; c++) *c = (char)toupper((unsigned char)*c);

        int code = ParseKeyName(val);
        if (code == GLFW_KEY_UNKNOWN) {
            printf("[KeyConfig] Unknown key '%s' for button '%s'\n", val, key);
            continue;
        }

        if      (!strcmp(key, "UP"))       g_key_config.up       = code;
        else if (!strcmp(key, "DOWN"))     g_key_config.down     = code;
        else if (!strcmp(key, "LEFT"))     g_key_config.left     = code;
        else if (!strcmp(key, "RIGHT"))    g_key_config.right    = code;
        else if (!strcmp(key, "CROSS"))    g_key_config.cross    = code;
        else if (!strcmp(key, "SQUARE"))   g_key_config.square   = code;
        else if (!strcmp(key, "TRIANGLE")) g_key_config.triangle = code;
        else if (!strcmp(key, "CIRCLE"))   g_key_config.circle   = code;
        else if (!strcmp(key, "START"))    g_key_config.start    = code;
        else if (!strcmp(key, "SELECT"))   g_key_config.select   = code;
        else if (!strcmp(key, "L1"))       g_key_config.l1       = code;
        else if (!strcmp(key, "L2"))       g_key_config.l2       = code;
        else if (!strcmp(key, "R1"))       g_key_config.r1       = code;
        else if (!strcmp(key, "R2"))       g_key_config.r2       = code;
        else printf("[KeyConfig] Unknown button '%s'\n", key);
    }
    fclose(f);
    printf("[KeyConfig] UP=%d DOWN=%d CROSS=%d START=%d\n",
           g_key_config.up, g_key_config.down, g_key_config.cross, g_key_config.start);
}

/* Called from runtime.c's FUN_80016940 override every PS1 frame-flip.
 * Pumps GLFW events, samples keyboard for joypad, and presents the frame. */
extern "C" void psx_set_pad1(uint16_t buttons);

extern "C" void psx_watchdog_reset(void);

extern "C" void psx_present_frame(void) {
    ++g_ps1_frame;
    psx_watchdog_reset();
    static char s_pending_shot[512] = {};

    /* Debug server per-frame integration */
    debug_server_poll();
    debug_server_record_frame();
    debug_server_check_watchpoints();
    game_post_frame(g_ps1_frame);
    debug_server_wait_if_paused();

    if (!g_renderer) return;
    GLFWwindow* win = (GLFWwindow*)g_renderer->GetWindow();
    if (win) {
        glfwPollEvents();

        /* Build joypad bitmask from keyconfig.
         * PS1 bit mapping (1 = pressed):
         *   0x0080 D-Up  0x0020 D-Down  0x0040 D-Left  0x0010 D-Right
         *   0x0008 Start 0x0001 Select
         *   0x1000 Tri   0x2000 Circle  0x4000 Cross   0x8000 Square
         *   0x0400 L1    0x0100 L2      0x0800 R1      0x0200 R2   */
        uint16_t pad = 0;
        if (glfwGetKey(win, g_key_config.up)       == GLFW_PRESS) pad |= 0x0010;
        if (glfwGetKey(win, g_key_config.down)     == GLFW_PRESS) pad |= 0x0040;
        if (glfwGetKey(win, g_key_config.left)     == GLFW_PRESS) pad |= 0x0080;
        if (glfwGetKey(win, g_key_config.right)    == GLFW_PRESS) pad |= 0x0020;
        if (glfwGetKey(win, g_key_config.cross)    == GLFW_PRESS) pad |= 0x4000;
        if (glfwGetKey(win, g_key_config.square)   == GLFW_PRESS) pad |= 0x8000;
        if (glfwGetKey(win, g_key_config.triangle) == GLFW_PRESS) pad |= 0x1000;
        if (glfwGetKey(win, g_key_config.circle)   == GLFW_PRESS) pad |= 0x2000;
        if (glfwGetKey(win, g_key_config.start)    == GLFW_PRESS) pad |= 0x0008;
        if (glfwGetKey(win, g_key_config.select)   == GLFW_PRESS) pad |= 0x0001;
        if (glfwGetKey(win, g_key_config.l1)       == GLFW_PRESS) pad |= 0x0400;
        if (glfwGetKey(win, g_key_config.l2)       == GLFW_PRESS) pad |= 0x0100;
        if (glfwGetKey(win, g_key_config.r1)       == GLFW_PRESS) pad |= 0x0800;
        if (glfwGetKey(win, g_key_config.r2)       == GLFW_PRESS) pad |= 0x0200;

        /* [PAD] pad-change log — re-enable when debugging input:
        static uint16_t s_last_pad = 0;
        if (pad != s_last_pad) {
            printf("[PAD] 0x%04X\n", pad);
            fflush(stdout);
            s_last_pad = pad;
        } */

        if (glfwGetKey(win, GLFW_KEY_ESCAPE) == GLFW_PRESS ||
            glfwWindowShouldClose(win)) {
            if (s_record_file) {
                fprintf(s_record_file, "# Recording ended at frame %u\n", g_ps1_frame);
                fclose(s_record_file);
                s_record_file = NULL;
                printf("[REC] Saved to %s\n", s_record_path);
                fflush(stdout);
            }
            xa_audio_shutdown();
            debug_server_shutdown();
            g_renderer->Shutdown();
            exit(0);
        }

        /* F6 = save snapshot */
        static bool s_f6_prev = false;
        bool f6 = glfwGetKey(win, GLFW_KEY_F6) == GLFW_PRESS;
        if (f6 && !s_f6_prev) {
            static char snap_path[256];
            snprintf(snap_path, sizeof(snap_path), "C:/temp/%s_snap.bin", game_get_name());
            g_renderer->FlushPrimitives();
            uint16_t* vram_buf = (uint16_t*)malloc(1024 * 512 * 2);
            if (vram_buf) {
                g_renderer->DownloadFromVRAM(0, 0, 1024, 512, vram_buf);
                FILE* sf = fopen(snap_path, "wb");
                if (sf) {
                    const char magic[4] = {'S','N','A','P'};
                    uint32_t version = 1;
                    fwrite(magic,             4,           1, sf);
                    fwrite(&version,          4,           1, sf);
                    fwrite(&g_ps1_frame,      4,           1, sf);
                    fwrite(psx_get_ram(),     2*1024*1024, 1, sf);
                    fwrite(psx_get_scratch(), 1024,        1, sf);
                    fwrite(vram_buf,          1024*512*2,  1, sf);
                    fclose(sf);
                    printf("[SNAP] F6: saved frame %u → %s\n", g_ps1_frame, snap_path);
                } else {
                    printf("[SNAP] F6: ERROR cannot write %s\n", snap_path);
                }
                free(vram_buf);
            }
            fflush(stdout);
        }
        s_f6_prev = f6;

        /* F11 = dump full VRAM to PPM; F12 = screenshot window to PPM */
        static bool s_f11_prev = false, s_f12_prev = false;
        bool f11 = glfwGetKey(win, GLFW_KEY_F11) == GLFW_PRESS;
        bool f12 = glfwGetKey(win, GLFW_KEY_F12) == GLFW_PRESS;
        if (f11 && !s_f11_prev) g_renderer->SaveVRAMDump("C:/temp/vram_dump.ppm");
        if (f12 && !s_f12_prev) g_renderer->SaveScreenshot("C:/temp/screenshot.ppm");
        s_f11_prev = f11;
        s_f12_prev = f12;

        /* F5 = toggle turbo mode */
        static bool s_f5_prev = false;
        bool f5 = glfwGetKey(win, GLFW_KEY_F5) == GLFW_PRESS;
        if (f5 && !s_f5_prev) {
            g_turbo = !g_turbo;
            printf("[TURBO] %s\n", g_turbo ? "ON" : "OFF");
            fflush(stdout);
        }
        s_f5_prev = f5;

        /* Input recording — captures raw keyboard + turbo before any script override */
        record_tick(g_ps1_frame, pad, g_turbo);

        /* Script integration — override pad and handle screenshots/exit */
        if (s_script_path) {
            script_tick(g_ps1_frame, psx_get_ram(), psx_get_scratch());
            int sp = script_get_pad();
            if (sp >= 0) pad = (uint16_t)sp;
            char shot[512];
            if (script_wants_screenshot(shot, sizeof(shot)) && g_renderer) {
                /* Flush pending vertices to VRAM now so Present() blits the full frame. */
                extern uint32_t g_pre_shot_flush;
                g_pre_shot_flush = 1;
                g_renderer->FlushPrimitives();
                g_pre_shot_flush = 0;
                /* Queue the window screenshot for AFTER Present() so the front buffer
                 * contains the just-blitted frame (reading GL_BACK before SwapBuffers
                 * gives garbage — the back buffer is undefined after the previous swap). */
                strncpy(s_pending_shot, shot, sizeof(s_pending_shot) - 1);
                /* VRAM dump reads from the FBO directly — safe to do now. */
                char vram_shot[520];
                snprintf(vram_shot, sizeof(vram_shot), "%.*s_vram.png",
                         (int)(strlen(shot) - 4), shot);
                g_renderer->SaveVRAMDumpBMP(vram_shot);
            }
            int ec = script_check_exit();
            if (ec >= 0) {
                /* If snapshot loading is active, give a 600-frame grace period
                 * (10 seconds at 60Hz) so the user can press F7 or inject-snapshot
                 * can fire before we exit. */
                if (s_snap_load_path) {
                    static int s_exit_countdown = -1;
                    static int s_exit_code = 0;
                    if (s_exit_countdown < 0) {
                        s_exit_countdown = 600;
                        s_exit_code = ec;
                        printf("[SCRIPT] End — waiting 600 frames for snapshot (press F7)\n");
                        fflush(stdout);
                    } else if (--s_exit_countdown <= 0) {
                        printf("[SCRIPT] Exit %d (timeout)\n", s_exit_code);
                        fflush(stdout);
                        exit(s_exit_code);
                    }
                } else {
                    printf("[SCRIPT] Exit %d\n", ec);
                    fflush(stdout);
                    exit(ec);
                }
            }
        }

        /* Debug server input override */
        {
            int dbg_input = debug_server_get_input_override();
            if (dbg_input >= 0) pad = (uint16_t)dbg_input;
        }

        psx_set_pad1(pad);
    }

    /* During FMV, only the FMV player's own present call should render.
     * The display fiber's extra Present() calls would glClear + redraw, causing
     * a brief black flash (edge flicker) between FMV frames. */
    if (fmv_player_is_active() && !g_fmv_presenting) {
        /* Still poll events + throttle, but skip the actual render */
        goto throttle;
    }
    g_fmv_presenting = false;  /* consumed — next call is from display fiber */

    /* During FMV: override display area for this Present(), then restore.
     * GP1 commands flow through normally (keeping GPU state in sync),
     * but we show the FMV region. When FMV ends, the last GP1(05h)
     * values are already in the renderer — no stale display area. */
    if (fmv_player_is_active() && g_fmv_disp_w > 0) {
        g_renderer->SetDisplayArea(g_fmv_disp_x, g_fmv_disp_y);
        g_renderer->SetDisplayMode(g_fmv_disp_w, g_fmv_disp_h, false, false);
    }

    /* [PRESENT-DBG] FMV→normal transition — re-enable when debugging FMV:
    {
        static bool s_was_fmv = false;
        bool is_fmv = fmv_player_is_active() != 0;
        if (s_was_fmv && !is_fmv) {
            printf("[PRESENT-DBG] FMV→normal transition, presenting frame %u\n", g_ps1_frame);
            fflush(stdout);
        }
        s_was_fmv = is_fmv;
    } */


    g_renderer->Present();

    /* Window screenshot: taken AFTER Present() so the front buffer holds the
     * just-blitted frame.  SaveScreenshotBMP reads GL_FRONT explicitly. */
    if (s_pending_shot[0] != '\0') {
        g_renderer->SaveScreenshotBMP(s_pending_shot);
        s_pending_shot[0] = '\0';
    }

    throttle:
    /* Throttle to 60 FPS — matches PS1 NTSC VBlank rate.
     * Gameplay logic waits for 2 VBlanks per frame so game speed stays 30 FPS.
     * Intro/menu/FMV screens run at 60 Hz and need 60 FPS to play at correct speed.
     * Uses QueryPerformanceCounter for sub-millisecond precision.
     * timeBeginPeriod(1) is called once at init to set the Windows timer
     * resolution to 1ms so Sleep() doesn't overshoot by a full 15ms tick. */
    if (!g_turbo) {
        static LARGE_INTEGER s_freq  = {};
        static LARGE_INTEGER s_last  = {};
        static bool s_init = false;
        if (!s_init) {
            QueryPerformanceFrequency(&s_freq);
            QueryPerformanceCounter(&s_last);
            timeBeginPeriod(1);  /* 1ms timer resolution — prevents Sleep from overshooting */
            s_init = true;
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        /* counts per frame at 60 Hz */
        LONGLONG frame_counts = s_freq.QuadPart / 60;
        LONGLONG elapsed = now.QuadPart - s_last.QuadPart;
        if (elapsed < frame_counts) {
            /* Sleep most of the wait (leave 2ms margin), then spin for precision */
            LONGLONG remaining = frame_counts - elapsed;
            LONGLONG sleep_ms  = (remaining * 1000 / s_freq.QuadPart) - 2;
            if (sleep_ms > 0) Sleep((DWORD)sleep_ms);
            /* Spin-wait for the last few milliseconds */
            do { QueryPerformanceCounter(&now); }
            while (now.QuadPart - s_last.QuadPart < frame_counts);
        }
        s_last = now;
    }

    /* [FPS] periodic FPS counter — re-enable when debugging performance:
    {
        static LARGE_INTEGER s_freq2 = {};
        static LARGE_INTEGER s_fps_t = {};
        static bool s_fps_init = false;
        static uint32_t s_fps_count = 0;
        if (!s_fps_init) {
            QueryPerformanceFrequency(&s_freq2);
            QueryPerformanceCounter(&s_fps_t);
            s_fps_init = true;
        }
        ++s_fps_count;
        if (s_fps_count >= 60) {
            LARGE_INTEGER now2;
            QueryPerformanceCounter(&now2);
            double secs = (double)(now2.QuadPart - s_fps_t.QuadPart) / (double)s_freq2.QuadPart;
            double fps = 60.0 / secs;
            if (secs >= 2.0) {
                printf("[FPS] %.1f (f%u%s)\n", fps, g_ps1_frame, g_turbo ? " turbo" : "");
                fflush(stdout);
            }
            s_fps_count = 0;
            s_fps_t = now2;
        }
    } */

    /* Snapshot save — triggered at a specific frame by --save-snapshot FRAME FILE */
    if (s_snap_save_path && g_ps1_frame == s_snap_save_frame) {
        g_renderer->FlushPrimitives();
        uint16_t* vram_buf = (uint16_t*)malloc(1024 * 512 * 2);
        if (vram_buf) {
            g_renderer->DownloadFromVRAM(0, 0, 1024, 512, vram_buf);
            FILE* sf = fopen(s_snap_save_path, "wb");
            if (sf) {
                const char magic[4] = {'S','N','A','P'};
                uint32_t version = 1;
                fwrite(magic,          4,           1, sf);
                fwrite(&version,       4,           1, sf);
                fwrite(&g_ps1_frame,   4,           1, sf);
                fwrite(psx_get_ram(),  2*1024*1024, 1, sf);
                fwrite(psx_get_scratch(), 1024,     1, sf);
                fwrite(vram_buf,       1024*512*2,  1, sf);
                fclose(sf);
                printf("[SNAP] Saved frame %u → %s\n", g_ps1_frame, s_snap_save_path);
            } else {
                printf("[SNAP] ERROR: cannot write %s\n", s_snap_save_path);
            }
            free(vram_buf);
        }
        fflush(stdout);
        s_snap_save_path = NULL; /* save once only */
        /* Close window so the automated run exits cleanly */
        GLFWwindow* sw = (GLFWwindow*)g_renderer->GetWindow();
        if (sw) glfwSetWindowShouldClose(sw, 1);
    }

    /* Snapshot restore — F7 or script inject-snapshot command */
    if (s_snap_load_path) {
        static bool s_f7_prev = false;
        bool f7 = (win && glfwGetKey(win, GLFW_KEY_F7) == GLFW_PRESS)
                  || g_snap_inject_requested;
        g_snap_inject_requested = 0;
        if (f7 && !s_f7_prev) {
            FILE* sf = fopen(s_snap_load_path, "rb");
            if (sf) {
                char magic[4]; uint32_t version = 0, saved_frame = 0;
                fread(magic, 4, 1, sf);
                fread(&version, 4, 1, sf);
                fread(&saved_frame, 4, 1, sf);
                if (memcmp(magic, "SNAP", 4) == 0 && version == 1) {
                    fread(psx_get_ram(),     2*1024*1024, 1, sf);
                    fread(psx_get_scratch(), 1024,        1, sf);
                    uint16_t* vram_buf = (uint16_t*)malloc(1024 * 512 * 2);
                    if (vram_buf) {
                        fread(vram_buf, 1024*512*2, 1, sf);
                        g_renderer->UploadToVRAM(0, 0, 1024, 512, vram_buf);
                        free(vram_buf);
                    }
                    printf("[SNAP] Snapshot applied at frame %u (F7, saved=%u)\n",
                           g_ps1_frame, saved_frame);
                } else {
                    printf("[SNAP] F7: invalid snapshot file\n");
                }
                fclose(sf);
            } else {
                printf("[SNAP] F7: cannot open %s\n", s_snap_load_path);
            }

            /* If recording is active, capture this F7 press as inject-snapshot */
            if (s_record_path) {
                if (!s_record_file) {
                    s_record_file = fopen(s_record_path, "w");
                    if (s_record_file) {
                        fprintf(s_record_file, "# PSXRecomp input recording\n");
                        fprintf(s_record_file, "# Recorded at frame %u\n", g_ps1_frame);
                    }
                }
                if (s_record_file) {
                    uint32_t delta = g_ps1_frame - s_rec_last_frame;
                    if (delta > 0) fprintf(s_record_file, "wait %u\n", delta);
                    fprintf(s_record_file, "inject-snapshot\n");
                    fflush(s_record_file);
                    s_rec_last_frame = g_ps1_frame;
                }
            }

            fflush(stdout);
        }
        s_f7_prev = f7;
    }

    /* Auto-save diagnostic screenshots at two fixed points only (frame 300 and 900).
     * Avoids repeating glReadPixels stalls that dropped FPS to 18-19 every 10 seconds. */
    if (g_ps1_frame == 300) {
        g_renderer->SaveScreenshotBMP("C:/temp/game_shot_01.png");
        g_renderer->SaveVRAMDumpBMP("C:/temp/game_vram.png");
        fflush(stdout);
    }
    if (g_ps1_frame == 900) {
        g_renderer->SaveScreenshotBMP("C:/temp/game_shot_02.png");
        fflush(stdout);
    }
    if (g_ps1_frame == 2500) {
        g_renderer->SaveScreenshotBMP("C:/temp/game_shot_03.png");
        fflush(stdout);
    }
    if (g_ps1_frame == 3000) {
        g_renderer->SaveScreenshotBMP("C:/temp/game_shot_03b.png");
        fflush(stdout);
    }
    if (g_ps1_frame == 3300) {
        g_renderer->SaveScreenshotBMP("C:/temp/game_shot_03c.png");
        fflush(stdout);
    }
    if (g_ps1_frame == 3600) {
        g_renderer->SaveScreenshotBMP("C:/temp/game_shot_03d.png");
        fflush(stdout);
    }
    if (g_ps1_frame == 4500) {
        g_renderer->SaveScreenshotBMP("C:/temp/game_shot_04.png");
        fflush(stdout);
    }
    if (g_ps1_frame == 5000) {
        g_renderer->SaveScreenshotBMP("C:/temp/game_shot_05.png");
        fflush(stdout);
    }
}

/* ---------------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------------- */
extern "C" void psxrecomp_runner_run(int argc, char** argv) {
    /* Parse CLI arguments: positional exe_path + cue_path, optional --script/--mmio-trace */
    const char* exe_path = NULL;
    const char* cue_path = NULL;
    int mmio_trace = 0;
    int debug_port = 0;  /* 0 = use default (4370) */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--script") && i + 1 < argc) {
            s_script_path = argv[++i];
        } else if (!strcmp(argv[i], "--record") && i + 1 < argc) {
            s_record_path = argv[++i];
        } else if (!strcmp(argv[i], "--save-snapshot") && i + 2 < argc) {
            s_snap_save_frame = (uint32_t)atoi(argv[i + 1]);
            s_snap_save_path  = argv[i + 2];
            i += 2;
        } else if (!strcmp(argv[i], "--load-snapshot") && i + 1 < argc) {
            s_snap_load_path = argv[++i];
        } else if (!strcmp(argv[i], "--mmio-trace")) {
            mmio_trace = 1;
        } else if (!strcmp(argv[i], "--debug-port") && i + 1 < argc) {
            debug_port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--debug")) {
            g_debug_mode = 1;
        } else if (!exe_path) {
            exe_path = argv[i];
        } else if (!cue_path) {
            cue_path = argv[i];
        }
    }
    if (!exe_path || !cue_path) {
        fprintf(stderr, "Usage: PSXRecompGame <exe> <cue> [--script <file>] [--record <file>]\n");
        exit(1);
    }

    printf("=== PSXRecomp v2 ===\n");
    printf("EXE: %s\n", exe_path);
    printf("CUE: %s\n", cue_path);
    if (s_script_path) printf("Script: %s\n", s_script_path);
    if (s_record_path) printf("Recording to: %s\n", s_record_path);
    printf("\n");

    /* Compute exe directory for keyconfig.ini */
    char exe_dir[512] = {};
    {
        strncpy(exe_dir, argv[0], sizeof(exe_dir) - 1);
        char* last_sep = nullptr;
        for (char* c = exe_dir; *c; c++) {
            if (*c == '\\') *c = '/';
            if (*c == '/') last_sep = c;
        }
        if (last_sep) *(last_sep + 1) = '\0';
        else { exe_dir[0] = '.'; exe_dir[1] = '/'; exe_dir[2] = '\0'; }
    }

    /* Load key config from same directory as the executable */
    {
        char config_path[512] = {};
        snprintf(config_path, sizeof(config_path), "%skeyconfig.ini", exe_dir);
        LoadKeyConfig(config_path);
    }

    /* Initialize CD-ROM reader */
    if (!psx_cdrom_init(cue_path)) {
        fprintf(stderr, "ERROR: Failed to open CD image: %s\n", cue_path);
        exit(1);
    }

    /* Load PS1 EXE — auto-detect and skip PS-X header if present */
    FILE* f = fopen(exe_path, "rb");
    if (!f) { perror("fopen"); exit(1); }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* exe_data = (uint8_t*)malloc(size);
    fread(exe_data, 1, size, f);
    fclose(f);

    /* PS-X EXE header is 2048 bytes and starts with "PS-X EXE" */
    const uint8_t* load_data = exe_data;
    uint32_t load_size = (uint32_t)size;
    if (size > 2048 && memcmp(exe_data, "PS-X EXE", 8) == 0) {
        load_data = exe_data + 2048;
        load_size = (uint32_t)(size - 2048);
        printf("Detected PS-X header, skipping 2048 bytes\n");
    }
    printf("Loaded %u bytes\n", load_size);

    /* Initialize runtime and load EXE into PS1 RAM at 0x80010000 */
    CPUState cpu;
    psx_runtime_init(&cpu);
    psx_install_crash_handler();
    if (mmio_trace) psx_mmio_trace_enable(1);
    psx_runtime_load(0x80010000u, load_data, load_size);
    free(exe_data);
    printf("Loaded into PS1 RAM at 0x80010000\n");

    /* Initialize renderer */
    OpenGLRenderer renderer;
    GPUInterpreter gpu(&renderer);
    g_renderer = &renderer;
    g_gpu      = &gpu;

    if (!renderer.Initialize()) {
        fprintf(stderr, "Renderer init failed\n");
        exit(1);
    }
    printf("Renderer ready\n\n");

    /* Initialize debug server */
    debug_server_init(debug_port);
    debug_server_set_glfw_window(renderer.GetWindow());

    /* Snapshot load — just validate the file exists and is valid; F7 applies it on demand. */
    if (s_snap_load_path) {
        FILE* sf = fopen(s_snap_load_path, "rb");
        if (!sf) {
            fprintf(stderr, "[SNAP] ERROR: cannot open %s\n", s_snap_load_path);
            exit(1);
        }
        char magic[4]; uint32_t version = 0, saved_frame = 0;
        fread(magic, 4, 1, sf); fread(&version, 4, 1, sf); fread(&saved_frame, 4, 1, sf);
        fclose(sf);
        if (memcmp(magic, "SNAP", 4) != 0 || version != 1) {
            fprintf(stderr, "[SNAP] ERROR: invalid snapshot file %s\n", s_snap_load_path);
            exit(1);
        }
        printf("[SNAP] Snapshot ready (saved frame %u) — press F7 to apply\n", saved_frame);
        fflush(stdout);
    }

    /* Load automation script if specified */
    if (s_script_path) {
        if (!script_load(s_script_path)) {
            fprintf(stderr, "Failed to load script: %s\n", s_script_path);
            exit(1);
        }
        /* Minimize window when running automated scripts so it doesn't
         * cover the user's windows or steal mouse focus.
         * Temporarily disabled for debugging — minimized window gives
         * black screenshots via glReadPixels on default framebuffer. */
        /* GLFWwindow* win = (GLFWwindow*)renderer.GetWindow();
        if (win) glfwIconifyWindow(win); */
    }

    /* Set up minimal CPU state */
    cpu.sp = 0x801FFF00u;  /* Stack near top of RAM */
    cpu.gp = 0x0u;         /* GP set by game init */

    /* Run game entry point */
    printf("Calling func_8006B58C (entry point)...\n");
    fflush(stdout);
    func_8006B58C(&cpu);
    printf("Entry point returned.\n");
    fflush(stdout);

    /* Minimal render loop — present frames until window closed */
    GLFWwindow* win = (GLFWwindow*)renderer.GetWindow();
    int frame = 0;
    while (win && !glfwWindowShouldClose(win)) {
        renderer.Present();
        renderer.VSync();
        frame++;
        if (frame <= 5 || frame % 60 == 0) {
            printf("[frame %d]\n", frame);
            fflush(stdout);
        }
    }

    debug_server_shutdown();
    renderer.Shutdown();
    printf("\nDone. Frames: %d\n", frame);
}
