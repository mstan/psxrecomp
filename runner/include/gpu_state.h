#pragma once

#include <cstdint>

/**
 * PS1 GPU State Structure
 *
 * Maintains all GPU register state, FIFO, VRAM, and configuration.
 * This is the complete emulated state of the PS1 GPU.
 */

namespace PS1 {

// Drawing mode state (GP0(E1h))
struct DrawMode {
    uint8_t texpage_x_base;      // 0-15 (×64 halfwords = ×128 bytes)
    uint8_t texpage_y_base;      // 0-1 (×256 lines) - bit 0
    uint8_t texpage_y_base_bit1; // 0-1 (×256 lines) - bit 1
    uint8_t semi_transparency;   // 0-3 (blend mode: 0=B/2+F/2, 1=B+F, 2=B-F, 3=B+F/4)
    uint8_t texture_depth;       // 0=4bit, 1=8bit, 2=15bit
    bool dithering;              // 24bit→15bit dither enable
    bool draw_to_display;        // Allow drawing to display area
    bool texture_disable;        // Force untextured rendering
    bool h_flip;                 // Horizontal flip (textured rectangles only)
};

// Texture window settings (GP0(E2h))
struct TextureWindow {
    uint8_t mask_x;     // 0-31 (×8 pixels)
    uint8_t mask_y;     // 0-31 (×8 pixels)
    uint8_t offset_x;   // 0-31 (×8 pixels)
    uint8_t offset_y;   // 0-31 (×8 pixels)
};

// Drawing area (GP0(E3h/E4h))
struct DrawingArea {
    int16_t x1, y1;  // Top-left
    int16_t x2, y2;  // Bottom-right
};

// Drawing offset (GP0(E5h))
struct DrawingOffset {
    int16_t x, y;    // Signed offset (-1024 to +1023)
};

// Mask bit settings (GP0(E6h))
struct MaskSettings {
    bool set_mask_bit;    // Set bit 15 when drawing
    bool check_mask_bit;  // Don't draw to masked pixels
};

// Display control (GP1)
struct DisplayControl {
    bool display_enable;       // GP1(03h) - 0=on, 1=off
    uint8_t dma_direction;     // GP1(04h) - 0=off, 1=FIFO, 2=CPUtoGP0, 3=GPUREADtoCPU
    uint16_t display_area_x;   // GP1(05h) - VRAM X (halfword address)
    uint16_t display_area_y;   // GP1(05h) - VRAM Y (scanline)
    uint16_t h_display_range_x1, h_display_range_x2;  // GP1(06h) - horizontal range
    uint16_t v_display_range_y1, v_display_range_y2;  // GP1(07h) - vertical range
    uint8_t h_resolution;      // GP1(08h) - 0=256, 1=320, 2=512, 3=640
    bool v_resolution;         // GP1(08h) - 0=240, 1=480i
    bool video_mode;           // GP1(08h) - 0=NTSC, 1=PAL
    bool color_depth_24bit;    // GP1(08h) - 0=15bit, 1=24bit
    bool interlace;            // GP1(08h)
    bool reverse_flag;         // GP1(08h) - horizontal flip
};

// VRAM transfer state
struct VRAMTransfer {
    bool active;               // Transfer in progress?
    uint16_t x, y;             // Destination/source position
    uint16_t width, height;    // Transfer dimensions
    uint32_t remaining;        // Words remaining to transfer (legacy)
    uint32_t remaining_pixels; // Pixels remaining in streaming CPU→VRAM transfer
    bool is_cpu_to_vram;       // True=CPU→VRAM, False=VRAM→CPU
};

// Complete GPU state
struct GPUState {
    // GP0 Command FIFO (64 entries)
    uint32_t fifo[64];
    int fifo_write_pos;  // Current write position (0-63)
    int fifo_read_pos;   // Current read position (0-63)
    int fifo_count;      // Number of entries in FIFO (0-64)

    // GPUSTAT Register (GP1 status, read at 1F801814h)
    uint32_t gpustat;

    // Drawing mode state
    DrawMode draw_mode;

    // Texture window
    TextureWindow texture_window;

    // Drawing area (clipping rectangle)
    DrawingArea drawing_area;

    // Drawing offset (applied to all vertices)
    DrawingOffset drawing_offset;

    // Mask bit settings
    MaskSettings mask_settings;

    // Display control
    DisplayControl display_control;

    // VRAM transfer state
    VRAMTransfer vram_transfer;

    // Current command processing
    uint32_t current_command;       // First word of current command (0 if none)
    uint32_t command_params[16];    // Parameter buffer
    int params_received;            // Parameters received so far
    int params_needed;              // Total parameters needed for command

    // VRAM (1 MB = 1024×512 × 16-bit pixels)
    // Stored as [y][x] for cache-friendly row access
    uint16_t vram[512][1024];

    // GPUREAD response FIFO (for VRAM→CPU transfers)
    uint32_t gpuread_buffer[2048];  // Large enough for max transfer
    int gpuread_count;              // Words available to read
    int gpuread_pos;                // Current read position

    // Constructor - initialize to power-on defaults
    GPUState();

    // Reset GPU to initial state (GP1(00h))
    void Reset();

    // Update GPUSTAT register based on current state
    void UpdateGPUSTAT();
};

} // namespace PS1
