#pragma once

#include <cstdint>
#include "gpu_state.h"

/**
 * PS1 GPU Renderer Interface (HAL)
 *
 * Abstract interface for graphics backends (OpenGL, Vulkan, etc.)
 * Separates GPU command interpretation from actual rendering.
 */

namespace PS1 {

// Vertex structure for primitives
struct Vertex {
    int16_t x, y;        // Screen coordinates (after drawing offset applied)
    uint8_t r, g, b;     // Vertex color (0-255)
    uint8_t u, v;        // Texture coordinates (0-255)

    // Texture info (only valid for specific vertices in textured primitives)
    uint16_t clut_x, clut_y;      // CLUT position (first vertex only)
    uint8_t texpage_x, texpage_y; // Texture page (second vertex only)
    uint8_t texpage_depth;        // Texture depth (0=4bit, 1=8bit, 2=15bit)

    bool has_texture;    // Texture coordinates valid for this vertex?
    bool has_clut;       // CLUT info valid for this vertex?
    bool has_texpage;    // Texpage info valid for this vertex?
};

// Draw state for primitives
struct DrawState {
    // Shading mode
    bool gouraud;            // True=Gouraud shading, False=Flat shading

    // Texture mapping
    bool textured;           // Texture mapped?
    bool raw_texture;        // Raw texture mode (skip modulation)?

    // Blending
    bool semi_transparent;   // Blending enabled?
    uint8_t blend_mode;      // 0-3 (0=B/2+F/2, 1=B+F, 2=B-F, 3=B+F/4)

    // Texture settings (from DrawMode)
    uint8_t texpage_x_base;  // Texture page X (0-15)
    uint8_t texpage_y_base;  // Texture page Y (0-1)
    uint8_t texture_depth;   // 0=4bit, 1=8bit, 2=15bit

    // Effects
    bool dithering;          // Dithering enabled?
    bool check_mask_bit;     // Check mask bit when drawing?
    bool set_mask_bit;       // Set mask bit when drawing?

    // Texture window (for wrapping/clamping)
    uint8_t tex_window_mask_x, tex_window_mask_y;
    uint8_t tex_window_offset_x, tex_window_offset_y;

    // Drawing area (clipping)
    int16_t draw_area_x1, draw_area_y1;
    int16_t draw_area_x2, draw_area_y2;
};

/**
 * Abstract GPU renderer interface
 *
 * Concrete implementations: OpenGLRenderer, VulkanRenderer
 */
class GPURenderer {
public:
    virtual ~GPURenderer() = default;

    // Lifecycle
    virtual bool Initialize() = 0;
    virtual void Shutdown() = 0;

    // Primitive rendering
    virtual void DrawTriangle(const Vertex v[3], const DrawState& state) = 0;
    virtual void DrawLine(const Vertex v[2], const DrawState& state) = 0;
    virtual void DrawPolyline(const Vertex* vertices, int count, const DrawState& state) = 0;
    virtual void DrawRectangle(int x, int y, int w, int h,
                               uint8_t u0, uint8_t v0, uint16_t clut_x, uint16_t clut_y,
                               uint8_t r, uint8_t g, uint8_t b, const DrawState& state) = 0;

    // VRAM operations
    virtual void FillRectangle(int x, int y, int w, int h, uint16_t color) = 0;
    virtual void UploadToVRAM(int x, int y, int w, int h, const uint16_t* data) = 0;
    virtual void DownloadFromVRAM(int x, int y, int w, int h, uint16_t* data) = 0;
    virtual void CopyVRAM(int src_x, int src_y, int dst_x, int dst_y, int w, int h) = 0;

    // State management
    virtual void SetDrawMode(const DrawMode& mode) = 0;
    virtual void SetTextureWindow(const TextureWindow& window) = 0;
    virtual void SetDrawingArea(const DrawingArea& area) = 0;
    virtual void SetDrawingOffset(const DrawingOffset& offset) = 0;
    virtual void SetMaskSettings(const MaskSettings& settings) = 0;
    virtual void ClearTextureCache() = 0;

    // Display control
    virtual void SetDisplayArea(int x, int y) = 0;
    virtual void SetDisplayMode(int width, int height, bool is_24bit, bool interlace) = 0;
    virtual void Present() = 0;  // Swap buffers, update display

    // Frame timing
    virtual void VSync() = 0;  // Called at 60Hz (NTSC) or 50Hz (PAL)

    // Display range (GP1(06h)/GP1(07h)) — controls visible portion of framebuffer
    virtual void SetVerticalDisplayRange(int y1, int y2) { (void)y1; (void)y2; }
    virtual void SetHorizontalDisplayRange(int x1, int x2) { (void)x1; (void)x2; }
};

} // namespace PS1
