#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime globals snapshot — serialized/restored by savestate */
typedef struct SaveStateRuntime {
    uint32_t cdrom_lba;
    uint32_t heap_base;
    uint32_t heap_size;
    uint32_t heap_ptr;
    uint32_t display_entry;
    uint32_t loading_entry;
    uint32_t loading_sp;
    uint32_t secondary_entry;
    uint32_t secondary_sp;
    int      display_ready;
    int      frame_flip_running;
    /* Saved MIPS register banks */
    uint32_t main_saved[35];
    uint32_t display_saved[35];
    uint32_t loading_saved[35];
    uint32_t secondary_saved[35];
    /* FMV intercept state */
    int      fmv_init;
    int      snd_ticks;
    int      snd_done;
} SaveStateRuntime;

/* SPU state snapshot */
typedef struct SpuSaveData {
    uint8_t  spu_ram[512 * 1024];
    uint16_t spu_regs[256];
    uint32_t transfer_addr;
    /* Per-voice state (simplified — active/addr/counter/env/etc.) */
    struct SpuVoiceSave {
        int      active;
        uint32_t cur_addr;
        uint32_t counter;
        int32_t  prev1, prev2;
        int32_t  env_vol;
        int      env_phase;
        int16_t  blk[28];
        int      blk_idx;
        int32_t  loop_start;
    } voices[24];
} SpuSaveData;

/* Accessor functions in runtime.c */
void psx_savestate_get_runtime(SaveStateRuntime* out);
void psx_savestate_restore_runtime(const SaveStateRuntime* in);
void psx_recreate_fibers(void* cpu_ptr);

/* Accessor functions in spu.cpp */
void spu_save_state(SpuSaveData* out);
void spu_load_state(const SpuSaveData* in);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

#include "gpu_state.h"

/* Savestate section IDs */
enum SaveStateSection : uint32_t {
    SECTION_RUNTIME   = 0x52544D00, /* "RTM\0" */
    SECTION_GPU_STATE = 0x47505500, /* "GPU\0" */
    SECTION_VRAM      = 0x5652414D, /* "VRAM" */
    SECTION_SPU       = 0x53505500, /* "SPU\0" */
    SECTION_XA        = 0x58410000, /* "XA\0\0" */
    SECTION_RAM       = 0x52414D00, /* "RAM\0" */
    SECTION_SCRATCH   = 0x53435200, /* "SCR\0" */
    SECTION_END       = 0x454E4400, /* "END\0" */
};

/* Savestate file header */
struct SaveStateHeader {
    char     magic[4]; /* "PSX1" */
    uint32_t version;  /* 1 */
};

/* Section header in file */
struct SaveStateSectionHeader {
    uint32_t id;
    uint32_t size;
};

/* XA audio state */
struct XaSaveData {
    uint32_t seek_lba;
};

/* Initialize savestate system. exe_dir = directory of the executable. */
void savestate_init(const char* exe_dir);

/* Save full state to file. Returns true on success. */
bool savestate_save(const char* name,
                    const SaveStateRuntime& runtime,
                    const PS1::GPUState& gpu_state,
                    const uint16_t* vram_pixels,
                    const SpuSaveData& spu,
                    const uint8_t* ram, uint32_t ram_size,
                    const uint8_t* scratch, uint32_t scratch_size,
                    uint32_t xa_lba);

/* Load state from file. Returns true on success. */
bool savestate_load(const char* name,
                    SaveStateRuntime* runtime,
                    PS1::GPUState* gpu_state,
                    uint16_t* vram_pixels,
                    SpuSaveData* spu,
                    uint8_t* ram, uint32_t ram_size,
                    uint8_t* scratch, uint32_t scratch_size,
                    uint32_t* xa_lba);

/* Check if a save state file exists. */
bool savestate_exists(const char* name);

/* Get full path for a state name. */
void savestate_get_path(const char* name, char* out, int out_size);

/* --- Scripted Input --- */

/* Load a script file. Returns true on success. */
bool script_load(const char* path);

/* Poll scripted input for current frame. Returns button mask to OR in.
 * Call once per frame; increments internal frame counter. */
uint16_t script_poll(void);

/* Returns true if a script is loaded and still has events remaining. */
bool script_active(void);

#endif /* __cplusplus */
