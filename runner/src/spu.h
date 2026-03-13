/* spu.h — PS1 SPU (Sound Processing Unit) emulator interface
 *
 * The PS1 SPU lives at 0x1F801C00-0x1F801DFF (hardware registers) and has
 * 512KB of dedicated sample RAM.  24 voices decode SPU-ADPCM at 44100 Hz.
 *
 * Call sequence from the game:
 *   1. spu_write_half / spu_write_word  — voice register setup (vol, pitch, start addr, ADSR)
 *   2. spu_dma_write                    — DMA4 bulk transfer of ADPCM data to SPU RAM
 *   3. KON write (0x1F801D88/8A)        — key on voices → playback starts
 *   4. spu_mix_into_buffers             — background audio thread mixes and outputs
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call once at startup to open WinMM device and start audio thread */
void spu_init(void);

/* Call at shutdown */
void spu_shutdown(void);

/* Set master volume for SPU output. 0.0 = silent, 1.0 = full. Default 1.0. */
void spu_set_master_volume(float v);

/* Write a 16-bit value to a SPU hardware register address.
 * addr must be in 0x1F801C00-0x1F801DFF range (physical). */
void spu_write_half(uint32_t addr, uint16_t val);

/* Write a 32-bit value (e.g. ADSR as one word write) */
void spu_write_word(uint32_t addr, uint32_t val);

/* Read a 16-bit SPU register (e.g. SPUSTAT, transfer addr). */
uint16_t spu_read_half(uint32_t addr);

/* DMA4 bulk write: copy byte_count bytes from src_ram_addr (physical address
 * in main RAM) to SPU RAM at the current transfer address. */
void spu_dma_write(uint32_t src_ram_addr, uint32_t byte_count,
                   const uint8_t* ram_base, uint32_t ram_size);

#ifdef __cplusplus
}
#endif
