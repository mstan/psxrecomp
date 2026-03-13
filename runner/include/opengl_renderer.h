#pragma once

#include "gpu_renderer.h"
#include "gpu_state.h"
#include <cstdint>
#include <vector>

/**
 * OpenGL 3.3+ Renderer Implementation for PS1 GPU
 *
 * Implements the GPURenderer interface using modern OpenGL.
 * Uses OpenGL 3.3 Core Profile for wide platform support.
 *
 * Design:
 * - VRAM: 1024x512 RGB5A1 texture + FBO
 * - Shaders: 4 programs (primitive, textured, fill, copy)
 * - Batching: Accumulates primitives, flushes on state change
 * - HAL Pattern: Separates PS1 GPU semantics from OpenGL implementation
 */

// Forward declarations (avoid including heavy OpenGL headers in header)
struct GLFWwindow;

namespace PS1 {

// OpenGL vertex format (matches shader layout)
struct OpenGLVertex {
    float x, y;              // Screen position (pixels, 0-1023 x 0-511)
    float r, g, b, a;        // Vertex color (0.0-1.0)
    float u, v;              // Texture coordinates (0.0-1.0, normalized)
    float clut_x, clut_y;    // CLUT position (pixels)
    float texpage_x, texpage_y; // Texture page (pixels)
    uint32_t flags;          // Flags: bit 0=textured, bit 1=gouraud, bit 2=semi-transparent
};

/**
 * OpenGL 3.3+ Renderer
 *
 * Implements PS1 GPU rendering using modern OpenGL features:
 * - Programmable shader pipeline
 * - Vertex Array Objects (VAO)
 * - Framebuffer Objects (FBO) for VRAM
 * - Batched primitive rendering
 */
class OpenGLRenderer : public GPURenderer {
public:
    OpenGLRenderer();
    ~OpenGLRenderer() override;

    // Lifecycle
    bool Initialize() override;
    void Shutdown() override;

    // Primitive rendering
    void DrawTriangle(const Vertex v[3], const DrawState& state) override;
    void DrawLine(const Vertex v[2], const DrawState& state) override;
    void DrawPolyline(const Vertex* vertices, int count, const DrawState& state) override;
    void DrawRectangle(int x, int y, int w, int h,
                       uint8_t u0, uint8_t v0, uint16_t clut_x, uint16_t clut_y,
                       uint8_t r, uint8_t g, uint8_t b, const DrawState& state) override;

    // VRAM operations
    void FillRectangle(int x, int y, int w, int h, uint16_t color) override;
    void UploadToVRAM(int x, int y, int w, int h, const uint16_t* data) override;
    void DownloadFromVRAM(int x, int y, int w, int h, uint16_t* data) override;
    void CopyVRAM(int src_x, int src_y, int dst_x, int dst_y, int w, int h) override;

    // State management
    void SetDrawMode(const DrawMode& mode) override;
    void SetTextureWindow(const TextureWindow& window) override;
    void SetDrawingArea(const DrawingArea& area) override;
    void SetDrawingOffset(const DrawingOffset& offset) override;
    void SetMaskSettings(const MaskSettings& settings) override;
    void ClearTextureCache() override;

    // Display control
    void SetDisplayArea(int x, int y) override;
    void SetDisplayMode(int width, int height, bool is_24bit, bool interlace) override;
    void SetVerticalDisplayRange(int y1, int y2) override;
    void SetHorizontalDisplayRange(int x1, int x2) override;
    void Present() override;  // Swap buffers, update display
    void VSync() override;    // Called at 60Hz (NTSC) or 50Hz (PAL)

    // Window access (for test programs)
    void* GetWindow() const { return (void*)window_; }

    // Debug helpers
    void SaveVRAMDump(const char* path);       // F11: dump full 1024x512 VRAM as PPM
    void SaveScreenshot(const char* path);     // F12: capture window framebuffer as PPM
    void SaveScreenshotBMP(const char* path);  // Auto: capture window framebuffer as PNG
    void SaveVRAMDumpBMP(const char* path);    // Auto: dump full 1024x512 VRAM as PNG
    void FlushPrimitives();  // Flush pending vertices to VRAM (public for pre-screenshot sync)

private:
    // Initialization helpers
    bool InitializeWindow();
    bool InitializeOpenGL();
    bool InitializeShaders();
    bool InitializeBuffers();
    bool InitializeVRAM();

    // Shader compilation helpers
    uint32_t CompileShader(uint32_t type, const char* source);
    uint32_t LinkProgram(uint32_t vertex_shader, uint32_t fragment_shader);

    // Rendering helpers
    void ApplyBlendMode(uint8_t mode);
    void ApplyDrawingArea();
    void RenderToVRAM();

    // OpenGL context
    GLFWwindow* window_ = nullptr;
    int window_width_ = 960;
    int window_height_ = 720;
    bool opengl_initialized_ = false;

    // VRAM representation
    uint32_t vram_texture_ = 0;      // 1024x512 RGB5A1 texture (render target + display)
    uint32_t vram_read_texture_ = 0; // 1024x512 RGB5A1 texture (read-only, no FBO — avoids feedback loop)
    uint32_t vram_fbo_ = 0;          // Framebuffer for rendering to VRAM
    uint16_t* vram_pixels_ = nullptr; // CPU copy of VRAM (1 MB)

    // Shaders (OpenGL program objects)
    uint32_t primitive_shader_ = 0;  // Polygon/line/rectangle rendering
    uint32_t textured_shader_ = 0;   // Textured primitive rendering
    uint32_t copy_shader_ = 0;       // VRAM-to-VRAM copy
    uint32_t fill_shader_ = 0;       // Fill rectangle

    // Vertex buffers
    uint32_t vao_ = 0;               // Vertex Array Object
    uint32_t vbo_ = 0;               // Vertex Buffer Object
    std::vector<OpenGLVertex> vertex_buffer_;

    // Primitive type for current batch (GL_TRIANGLES, GL_LINES, GL_LINE_STRIP)
    uint32_t primitive_type_ = 0x0004;  // Default: GL_TRIANGLES (0x0004)

    // Current state (mirrors PS1 GPU state)
    DrawMode draw_mode_;
    TextureWindow texture_window_;
    DrawingArea drawing_area_;
    DrawingOffset drawing_offset_;
    MaskSettings mask_settings_;

    // vram_read_texture_ needs re-sync from vram_pixels_ only when the CPU has
    // written to VRAM (UploadToVRAM / FillRectangle / CopyVRAM).  GPU renders
    // write to vram_texture_ (FBO) which never feeds back into vram_read_texture_,
    // so we skip the sync when only GPU draws happened (avoids pipeline stalls).
    bool vram_read_dirty_ = true;

    // Cached uniform locations (queried once in InitializeShaders; never per-frame)
    int uloc_prim_dithering_     = -1;
    int uloc_prim_check_mask_    = -1;
    int uloc_prim_set_mask_      = -1;
    int uloc_prim_vram_          = -1;
    int uloc_tex_dithering_      = -1;
    int uloc_tex_check_mask_     = -1;
    int uloc_tex_set_mask_       = -1;
    int uloc_tex_vram_           = -1;
    int uloc_tex_depth_          = -1;
    int uloc_tex_window_         = -1;
    int uloc_copy_source_        = -1;

    // Cached state for optimization (avoid redundant OpenGL calls)
    struct CachedState {
        uint8_t blend_mode = 0xFF;       // 0xFF = invalid/unset
        bool dithering = false;
        bool check_mask_bit = false;
        bool set_mask_bit = false;
        int scissor_x = -1;
        int scissor_y = -1;
        int scissor_w = -1;
        int scissor_h = -1;
        bool blending_enabled = false;
    } cached_state_;

    // Display state
    int display_area_x_ = 0;
    int display_area_y_ = 0;
    int display_width_ = 320;
    int display_height_ = 240;
    bool display_24bit_ = false;
    bool display_interlace_ = false;

    // Display range (GP1(06h)/GP1(07h)) — video output scanline/dot-clock ranges
    int v_display_range_y1_ = 0;   // Vertical start (video scanline)
    int v_display_range_y2_ = 0;   // Vertical end (video scanline)
    int h_display_range_x1_ = 0;   // Horizontal start (dot clocks)
    int h_display_range_x2_ = 0;   // Horizontal end (dot clocks)
};

} // namespace PS1
