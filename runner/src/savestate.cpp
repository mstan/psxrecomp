/* savestate.cpp — Save/load full game state + scripted input system
 *
 * File format: flat binary, ~3.5MB uncompressed.
 *   [Header]  "PSX1" (4B) + version=1 (4B)
 *   [Section] id (4B) + size (4B) + data (size bytes)
 *   ...repeat...
 *   [Section] END marker
 *
 * Scripted input format (text):
 *   wait <frames>
 *   press <button>      — single-frame tap
 *   hold <button>       — hold until release
 *   release <button>    — release held button
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "savestate.h"
#include "psx_runtime.h"
#include "gpu_state.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <vector>

/* -------------------------------------------------------------------------
 * State directory
 * ---------------------------------------------------------------------- */
static char g_states_dir[512] = {};

void savestate_init(const char* exe_dir) {
    snprintf(g_states_dir, sizeof(g_states_dir), "%sstates", exe_dir);
    /* Create states directory if it doesn't exist */
    CreateDirectoryA(g_states_dir, NULL);
    printf("[SaveState] States dir: %s\n", g_states_dir);
    fflush(stdout);
}

void savestate_get_path(const char* name, char* out, int out_size) {
    /* If name already has .state extension, use as-is */
    const char* dot = strrchr(name, '.');
    if (dot && strcmp(dot, ".state") == 0) {
        snprintf(out, out_size, "%s/%s", g_states_dir, name);
    } else {
        snprintf(out, out_size, "%s/%s.state", g_states_dir, name);
    }
}

bool savestate_exists(const char* name) {
    char path[512];
    savestate_get_path(name, path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (f) { fclose(f); return true; }
    return false;
}

/* -------------------------------------------------------------------------
 * Save
 * ---------------------------------------------------------------------- */
static bool write_section(FILE* f, uint32_t id, const void* data, uint32_t size) {
    SaveStateSectionHeader hdr;
    hdr.id = id;
    hdr.size = size;
    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) return false;
    if (size > 0 && fwrite(data, size, 1, f) != 1) return false;
    return true;
}

bool savestate_save(const char* name,
                    const SaveStateRuntime& runtime,
                    const PS1::GPUState& gpu_state,
                    const uint16_t* vram_pixels,
                    const SpuSaveData& spu,
                    const uint8_t* ram, uint32_t ram_size,
                    const uint8_t* scratch, uint32_t scratch_size,
                    uint32_t xa_lba) {
    char path[512];
    savestate_get_path(name, path, sizeof(path));

    FILE* f = fopen(path, "wb");
    if (!f) {
        printf("[SaveState] ERROR: Cannot open %s for writing\n", path);
        fflush(stdout);
        return false;
    }

    /* Header */
    SaveStateHeader hdr;
    memcpy(hdr.magic, "PSX1", 4);
    hdr.version = 1;
    fwrite(&hdr, sizeof(hdr), 1, f);

    /* RAM (2MB) */
    write_section(f, SECTION_RAM, ram, ram_size);

    /* Scratchpad (1KB) */
    write_section(f, SECTION_SCRATCH, scratch, scratch_size);

    /* Runtime globals */
    write_section(f, SECTION_RUNTIME, &runtime, sizeof(runtime));

    /* GPU state (without the 1MB VRAM array inside GPUState — we save VRAM separately) */
    write_section(f, SECTION_GPU_STATE, &gpu_state, sizeof(gpu_state));

    /* VRAM pixels from renderer (1024*512*2 = 1MB) */
    write_section(f, SECTION_VRAM, vram_pixels, 1024 * 512 * 2);

    /* SPU */
    write_section(f, SECTION_SPU, &spu, sizeof(spu));

    /* XA audio */
    XaSaveData xa;
    xa.seek_lba = xa_lba;
    write_section(f, SECTION_XA, &xa, sizeof(xa));

    /* End marker */
    write_section(f, SECTION_END, nullptr, 0);

    fclose(f);

    /* Get file size for log */
    FILE* check = fopen(path, "rb");
    long file_size = 0;
    if (check) {
        fseek(check, 0, SEEK_END);
        file_size = ftell(check);
        fclose(check);
    }

    printf("[SaveState] Saved to %s (%.1f MB)\n", path, file_size / (1024.0 * 1024.0));
    fflush(stdout);
    return true;
}

/* -------------------------------------------------------------------------
 * Load
 * ---------------------------------------------------------------------- */
bool savestate_load(const char* name,
                    SaveStateRuntime* runtime,
                    PS1::GPUState* gpu_state,
                    uint16_t* vram_pixels,
                    SpuSaveData* spu,
                    uint8_t* ram, uint32_t ram_size,
                    uint8_t* scratch, uint32_t scratch_size,
                    uint32_t* xa_lba) {
    char path[512];
    savestate_get_path(name, path, sizeof(path));

    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("[SaveState] ERROR: Cannot open %s for reading\n", path);
        fflush(stdout);
        return false;
    }

    /* Verify header */
    SaveStateHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 || memcmp(hdr.magic, "PSX1", 4) != 0) {
        printf("[SaveState] ERROR: Invalid header in %s\n", path);
        fclose(f);
        return false;
    }
    if (hdr.version != 1) {
        printf("[SaveState] ERROR: Unsupported version %u in %s\n", hdr.version, path);
        fclose(f);
        return false;
    }

    /* Read sections */
    bool got_ram = false, got_scratch = false, got_runtime = false;
    bool got_gpu = false, got_vram = false, got_spu = false, got_xa = false;

    while (!feof(f)) {
        SaveStateSectionHeader shdr;
        if (fread(&shdr, sizeof(shdr), 1, f) != 1) break;

        if (shdr.id == SECTION_END) break;

        switch (shdr.id) {
        case SECTION_RAM:
            if (shdr.size == ram_size) {
                fread(ram, ram_size, 1, f);
                got_ram = true;
            } else {
                fseek(f, shdr.size, SEEK_CUR);
            }
            break;
        case SECTION_SCRATCH:
            if (shdr.size == scratch_size) {
                fread(scratch, scratch_size, 1, f);
                got_scratch = true;
            } else {
                fseek(f, shdr.size, SEEK_CUR);
            }
            break;
        case SECTION_RUNTIME:
            if (shdr.size == sizeof(SaveStateRuntime)) {
                fread(runtime, sizeof(SaveStateRuntime), 1, f);
                got_runtime = true;
            } else {
                fseek(f, shdr.size, SEEK_CUR);
            }
            break;
        case SECTION_GPU_STATE:
            if (shdr.size == sizeof(PS1::GPUState)) {
                fread(gpu_state, sizeof(PS1::GPUState), 1, f);
                got_gpu = true;
            } else {
                fseek(f, shdr.size, SEEK_CUR);
            }
            break;
        case SECTION_VRAM:
            if (shdr.size == 1024 * 512 * 2) {
                fread(vram_pixels, 1024 * 512 * 2, 1, f);
                got_vram = true;
            } else {
                fseek(f, shdr.size, SEEK_CUR);
            }
            break;
        case SECTION_SPU:
            if (shdr.size == sizeof(SpuSaveData)) {
                fread(spu, sizeof(SpuSaveData), 1, f);
                got_spu = true;
            } else {
                fseek(f, shdr.size, SEEK_CUR);
            }
            break;
        case SECTION_XA: {
            XaSaveData xa;
            if (shdr.size == sizeof(xa)) {
                fread(&xa, sizeof(xa), 1, f);
                *xa_lba = xa.seek_lba;
                got_xa = true;
            } else {
                fseek(f, shdr.size, SEEK_CUR);
            }
            break;
        }
        default:
            /* Unknown section — skip */
            fseek(f, shdr.size, SEEK_CUR);
            break;
        }
    }

    fclose(f);

    printf("[SaveState] Loaded from %s: RAM=%d SCR=%d RT=%d GPU=%d VRAM=%d SPU=%d XA=%d\n",
           path, got_ram, got_scratch, got_runtime, got_gpu, got_vram, got_spu, got_xa);
    fflush(stdout);

    return got_ram && got_scratch && got_runtime;
}

/* -------------------------------------------------------------------------
 * Scripted Input
 * ---------------------------------------------------------------------- */
struct ScriptEvent {
    uint32_t frame;        /* absolute frame number to fire */
    uint16_t button_mask;  /* PS1 button bits */
    int      action;       /* 0=press (1 frame), 1=hold, 2=release */
};

static std::vector<ScriptEvent> g_script_events;
static uint32_t g_script_frame = 0;
static uint16_t g_script_held  = 0;  /* currently held buttons */
static size_t   g_script_idx   = 0;  /* next event to process */
static bool     g_script_loaded = false;

static uint16_t parse_button_name(const char* name) {
    if (!strcmp(name, "UP"))       return 0x0080;
    if (!strcmp(name, "DOWN"))     return 0x0020;
    if (!strcmp(name, "LEFT"))     return 0x0040;
    if (!strcmp(name, "RIGHT"))    return 0x0010;
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

bool script_load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        printf("[Script] Cannot open %s\n", path);
        fflush(stdout);
        return false;
    }

    g_script_events.clear();
    g_script_frame = 0;
    g_script_held  = 0;
    g_script_idx   = 0;

    uint32_t cur_frame = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        /* Strip leading whitespace */
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        /* Skip comments and empty lines */
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;

        char cmd[32] = {}, arg[32] = {};
        if (sscanf(p, "%31s %31s", cmd, arg) < 1) continue;

        /* Uppercase both */
        for (char* c = cmd; *c; c++) *c = (char)toupper((unsigned char)*c);
        for (char* c = arg; *c; c++) *c = (char)toupper((unsigned char)*c);

        if (!strcmp(cmd, "WAIT")) {
            int frames = atoi(arg);
            if (frames > 0) cur_frame += (uint32_t)frames;
        } else if (!strcmp(cmd, "PRESS")) {
            uint16_t btn = parse_button_name(arg);
            if (btn) {
                /* Press = hold for 1 frame then release */
                ScriptEvent hold = { cur_frame, btn, 1 };
                ScriptEvent rel  = { cur_frame + 1, btn, 2 };
                g_script_events.push_back(hold);
                g_script_events.push_back(rel);
                cur_frame += 1; /* advance past the press frame */
            }
        } else if (!strcmp(cmd, "HOLD")) {
            uint16_t btn = parse_button_name(arg);
            if (btn) {
                ScriptEvent ev = { cur_frame, btn, 1 };
                g_script_events.push_back(ev);
            }
        } else if (!strcmp(cmd, "RELEASE")) {
            uint16_t btn = parse_button_name(arg);
            if (btn) {
                ScriptEvent ev = { cur_frame, btn, 2 };
                g_script_events.push_back(ev);
            }
        }
    }
    fclose(f);

    g_script_loaded = !g_script_events.empty();
    printf("[Script] Loaded %zu events from %s\n", g_script_events.size(), path);
    fflush(stdout);
    return g_script_loaded;
}

uint16_t script_poll(void) {
    if (!g_script_loaded) return 0;

    /* Process all events for current frame */
    while (g_script_idx < g_script_events.size() &&
           g_script_events[g_script_idx].frame <= g_script_frame) {
        const ScriptEvent& ev = g_script_events[g_script_idx];
        if (ev.action == 1) {
            /* Hold */
            g_script_held |= ev.button_mask;
        } else if (ev.action == 2) {
            /* Release */
            g_script_held &= ~ev.button_mask;
        }
        g_script_idx++;
    }

    g_script_frame++;
    return g_script_held;
}

bool script_active(void) {
    return g_script_loaded && g_script_idx < g_script_events.size();
}
