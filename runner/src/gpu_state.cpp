#include "gpu_state.h"
#include <cstring>  // memset

namespace PS1 {

// Constructor - initialize to power-on defaults
GPUState::GPUState() {
    Reset();
}

// Reset GPU to initial state (GP1(00h))
void GPUState::Reset() {
    // Clear FIFO
    fifo_write_pos = 0;
    fifo_read_pos = 0;
    fifo_count = 0;
    memset(fifo, 0, sizeof(fifo));

    // Reset GPUSTAT to default value
    // Bit 26 (ready for command) = 1
    // Bit 28 (ready for DMA) = 1
    // Bit 31 (drawing even/odd lines) = 0
    gpustat = 0x14802000;

    // Reset draw mode (GP0(E1h)) to defaults
    draw_mode.texpage_x_base = 0;
    draw_mode.texpage_y_base = 0;
    draw_mode.texpage_y_base_bit1 = 0;
    draw_mode.semi_transparency = 0;  // B/2+F/2
    draw_mode.texture_depth = 0;      // 4-bit
    draw_mode.dithering = false;
    draw_mode.draw_to_display = false;
    draw_mode.texture_disable = false;
    draw_mode.h_flip = false;

    // Reset texture window (GP0(E2h))
    texture_window.mask_x = 0;
    texture_window.mask_y = 0;
    texture_window.offset_x = 0;
    texture_window.offset_y = 0;

    // Reset drawing area (GP0(E3h/E4h))
    drawing_area.x1 = 0;
    drawing_area.y1 = 0;
    drawing_area.x2 = 0;
    drawing_area.y2 = 0;

    // Reset drawing offset (GP0(E5h))
    drawing_offset.x = 0;
    drawing_offset.y = 0;

    // Reset mask settings (GP0(E6h))
    mask_settings.set_mask_bit = false;
    mask_settings.check_mask_bit = false;

    // Reset display control (GP1)
    display_control.display_enable = true;  // Display on
    display_control.dma_direction = 0;      // DMA off
    display_control.display_area_x = 0;
    display_control.display_area_y = 0;
    display_control.h_display_range_x1 = 0x200;  // Default NTSC range
    display_control.h_display_range_x2 = 0xC00;
    display_control.v_display_range_y1 = 0x10;
    display_control.v_display_range_y2 = 0x100;
    display_control.h_resolution = 0;     // 256 pixels
    display_control.v_resolution = false; // 240 lines
    display_control.video_mode = false;   // NTSC
    display_control.color_depth_24bit = false;  // 15-bit
    display_control.interlace = false;
    display_control.reverse_flag = false;

    // Reset VRAM transfer state
    vram_transfer.active = false;
    vram_transfer.x = 0;
    vram_transfer.y = 0;
    vram_transfer.width = 0;
    vram_transfer.height = 0;
    vram_transfer.remaining = 0;
    vram_transfer.is_cpu_to_vram = false;

    // Reset command processing state
    current_command = 0;
    params_received = 0;
    params_needed = 0;
    memset(command_params, 0, sizeof(command_params));

    // Clear GPUREAD buffer
    gpuread_count = 0;
    gpuread_pos = 0;

    // Note: VRAM is NOT cleared on reset (persists across reset)
    // Only clear on power-on (which this constructor represents)
    memset(vram, 0, sizeof(vram));
}

// Update GPUSTAT register based on current state
void GPUState::UpdateGPUSTAT() {
    gpustat = 0;

    // Bits 0-3: Texture page X base (from draw_mode)
    gpustat |= (draw_mode.texpage_x_base & 0xF);

    // Bit 4: Texture page Y base bit 0
    gpustat |= (draw_mode.texpage_y_base & 1) << 4;

    // Bits 5-6: Semi-transparency mode
    gpustat |= (draw_mode.semi_transparency & 3) << 5;

    // Bits 7-8: Texture depth
    gpustat |= (draw_mode.texture_depth & 3) << 7;

    // Bit 9: Dithering
    if (draw_mode.dithering) gpustat |= (1 << 9);

    // Bit 10: Draw to display area
    if (draw_mode.draw_to_display) gpustat |= (1 << 10);

    // Bit 11: Set mask bit
    if (mask_settings.set_mask_bit) gpustat |= (1 << 11);

    // Bit 12: Check mask bit
    if (mask_settings.check_mask_bit) gpustat |= (1 << 12);

    // Bit 13: Interlace field (always 0 for now)
    // gpustat |= (interlace_field & 1) << 13;

    // Bit 14: Reverse flag
    if (draw_mode.h_flip) gpustat |= (1 << 14);

    // Bit 15: Texture Y base bit 1
    gpustat |= (draw_mode.texpage_y_base_bit1 & 1) << 15;

    // Bits 16-23: Video mode (from display_control)
    uint32_t video_mode_bits = 0;
    video_mode_bits |= (display_control.h_resolution & 3);
    video_mode_bits |= (display_control.v_resolution ? 1 : 0) << 2;
    video_mode_bits |= (display_control.video_mode ? 1 : 0) << 3;
    video_mode_bits |= (display_control.color_depth_24bit ? 1 : 0) << 4;
    video_mode_bits |= (display_control.interlace ? 1 : 0) << 5;
    // Bits 6-7 are h_resolution extended (for 368 pixel mode)
    gpustat |= (video_mode_bits & 0xFF) << 16;

    // Bit 24: Interrupt request (not yet implemented, always 0)
    // gpustat |= (irq_requested ? 1 : 0) << 24;

    // Bit 25: DMA/Data request
    // Meaning depends on DMA direction
    if (display_control.dma_direction == 0) {
        // DMA off: always 0
    } else if (display_control.dma_direction == 1) {
        // FIFO: ready if FIFO not full
        if (fifo_count < 64) gpustat |= (1 << 25);
    } else if (display_control.dma_direction == 2) {
        // CPUtoGP0: ready if FIFO not full
        if (fifo_count < 64) gpustat |= (1 << 25);
    } else {
        // GPUREADtoCPU: ready if data available
        if (gpuread_count > 0) gpustat |= (1 << 25);
    }

    // Bit 26: Ready for GP0 command
    // Ready if FIFO not full
    if (fifo_count < 64) gpustat |= (1 << 26);

    // Bit 27: Ready for VRAM→CPU transfer (GPUREAD)
    // Ready if data available in GPUREAD buffer
    if (gpuread_count > 0) gpustat |= (1 << 27);

    // Bit 28: Ready for DMA block
    // Ready if FIFO has space (simplified)
    if (fifo_count < 32) gpustat |= (1 << 28);

    // Bits 29-30: DMA direction
    gpustat |= (display_control.dma_direction & 3) << 29;

    // Bit 31: Drawing odd/even lines (interlace)
    // Always 0 for now
    // gpustat |= (drawing_odd_lines ? 1 : 0) << 31;
}

} // namespace PS1
