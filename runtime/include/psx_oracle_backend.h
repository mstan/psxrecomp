/*
 * psx_oracle_backend.h -- Abstract interface for PSX golden-oracle backends.
 *
 * A backend wraps a reference PS1 emulator (DuckStation, Mednafen, etc.)
 * and exposes frame-step + memory-peek operations so the debug server can
 * compare recomp state against ground truth byte-by-byte.
 *
 * Pattern follows snesrecomp/runner/src/snes_oracle_backend.h.
 */
#ifndef PSX_ORACLE_BACKEND_H
#define PSX_ORACLE_BACKEND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* R3000A register snapshot for cross-runtime comparison. */
typedef struct {
    uint32_t gpr[32];
    uint32_t pc;
    uint32_t hi, lo;
    uint32_t cop0_sr;
    uint32_t cop0_cause;
    uint32_t cop0_epc;
} PsxCpuRegs;

/* Abstract backend. One instance per emulator. */
typedef struct psx_oracle_backend {
    const char *name;                                /* "duckstation" */

    int      (*init)(const char *bios_path);         /* 0 = success */
    void     (*shutdown)(void);
    int      (*is_loaded)(void);                     /* 1 if booted */

    void     (*run_frame)(uint16_t pad1_buttons);    /* advance one VBlank */
    uint32_t (*get_frame_count)(void);

    /* Memory access (physical addresses, no MMIO side effects). */
    void     (*get_ram)(uint8_t *out_2mb);           /* copy 2 MB main RAM */
    void     (*get_vram)(uint16_t *out_1mb);         /* copy 1 MB GPU VRAM (1024x512 x 16bpp) */
    void     (*get_scratchpad)(uint8_t *out_1kb);    /* copy 1 KB scratchpad */
    uint8_t  (*read_byte)(uint32_t phys_addr);       /* handles RAM, scratchpad, BIOS */
    uint32_t (*read_word)(uint32_t phys_addr);

    /* CPU state. */
    void     (*get_cpu_regs)(PsxCpuRegs *out);

    /* Sync: step oracle until ram[addr & 0x1FFFFF] == val, max_frames limit. */
    int      (*sync_to_state)(uint32_t addr, uint32_t val, uint16_t pad, int max_frames);
} psx_oracle_backend_t;

/* Active backend (NULL when oracle is disabled or not yet initialized). */
extern const psx_oracle_backend_t *g_psx_oracle;

/* Convenience wrappers called from main.cpp / debug_server.c. */
int  psx_oracle_init(const char *bios_path);   /* selects + inits default backend */
void psx_oracle_shutdown(void);
void psx_oracle_run_frame(uint16_t pad1_buttons);

#ifdef __cplusplus
}
#endif
#endif /* PSX_ORACLE_BACKEND_H */
