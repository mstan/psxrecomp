#include "opengl_renderer.h"
#include "game_extras.h"
#include <cstring>
#include <cstdio>
#include <string>
#include <thread>

extern uint32_t g_ps1_frame;
uint32_t g_pre_shot_flush = 0;
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// OpenGL and GLFW headers
#include <glad/glad.h>
#include <GLFW/glfw3.h>

namespace PS1 {

//==============================================================================
// Constructor / Destructor
//==============================================================================

OpenGLRenderer::OpenGLRenderer() {
    // Initialize VRAM pixel buffer (1024 x 512 x 16-bit = 1 MB)
    vram_pixels_ = new uint16_t[1024 * 512];
    std::memset(vram_pixels_, 0, 1024 * 512 * sizeof(uint16_t));

    // Initialize state to defaults
    draw_mode_ = {};
    texture_window_ = {};
    drawing_area_ = { 0, 0, 1023, 511 };  /* Full VRAM — overridden by E3/E4 */
    drawing_offset_ = {};
    mask_settings_ = {};
}

OpenGLRenderer::~OpenGLRenderer() {
    Shutdown();

    if (vram_pixels_) {
        delete[] vram_pixels_;
        vram_pixels_ = nullptr;
    }
}

//==============================================================================
// Lifecycle
//==============================================================================

bool OpenGLRenderer::Initialize() {
    if (opengl_initialized_) {
        return true;  // Already initialized - no-op
    }
    printf("[OpenGLRenderer] Initialize() called\n");

    // Step 1: Create GLFW window
    if (!InitializeWindow()) {
        printf("[OpenGLRenderer] ERROR: Failed to create GLFW window\n");
        return false;
    }

    // Step 2: Load OpenGL functions
    if (!InitializeOpenGL()) {
        printf("[OpenGLRenderer] ERROR: Failed to initialize OpenGL\n");
        return false;
    }

    // Step 3: Compile shaders
    if (!InitializeShaders()) {
        printf("[OpenGLRenderer] ERROR: Failed to compile shaders\n");
        return false;
    }

    // Step 4: Create vertex buffers
    if (!InitializeBuffers()) {
        printf("[OpenGLRenderer] ERROR: Failed to create buffers\n");
        return false;
    }

    // Step 5: Create VRAM texture and FBO
    if (!InitializeVRAM()) {
        printf("[OpenGLRenderer] ERROR: Failed to create VRAM texture\n");
        return false;
    }

    opengl_initialized_ = true;
    printf("[OpenGLRenderer] Initialization complete!\n");
    return true;
}

void OpenGLRenderer::Shutdown() {
    if (!opengl_initialized_) {
        return;
    }

    printf("[OpenGLRenderer] Shutdown() called\n");

    // Delete shaders
    if (primitive_shader_ != 0) {
        glDeleteProgram(primitive_shader_);
        primitive_shader_ = 0;
    }
    if (textured_shader_ != 0) {
        glDeleteProgram(textured_shader_);
        textured_shader_ = 0;
    }
    if (copy_shader_ != 0) {
        glDeleteProgram(copy_shader_);
        copy_shader_ = 0;
    }
    if (fill_shader_ != 0) {
        glDeleteProgram(fill_shader_);
        fill_shader_ = 0;
    }

    // Delete buffers (VAO, VBO)
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    if (vbo_ != 0) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }

    // Delete VRAM texture and FBO
    if (vram_texture_ != 0) {
        glDeleteTextures(1, &vram_texture_);
        vram_texture_ = 0;
    }
    if (vram_read_texture_ != 0) {
        glDeleteTextures(1, &vram_read_texture_);
        vram_read_texture_ = 0;
    }
    if (vram_fbo_ != 0) {
        glDeleteFramebuffers(1, &vram_fbo_);
        vram_fbo_ = 0;
    }

    // Destroy GLFW window
    if (window_ != nullptr) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }

    // Terminate GLFW
    glfwTerminate();

    opengl_initialized_ = false;
    printf("[OpenGLRenderer] Shutdown complete\n");
}

//==============================================================================
// Primitive Rendering
//==============================================================================

void OpenGLRenderer::DrawTriangle(const Vertex v[3], const DrawState& state) {
    if (!opengl_initialized_) {
        return;
    }

    // Check if primitive type changed (need to flush before switching)
    if (!vertex_buffer_.empty() && primitive_type_ != GL_TRIANGLES) {
        FlushPrimitives();
    }
    primitive_type_ = GL_TRIANGLES;

    // Per-polygon tpage overrides global E1 texture depth when present.
    // All PS1 textured polygons encode their own tpage/depth in vertex 1's texcoord word.
    uint32_t eff_depth = (state.textured && v[1].has_texpage)
        ? (uint32_t)v[1].texpage_depth
        : (uint32_t)state.texture_depth;

    // Flush if textured state, texture depth, or semi-transparent flag differs from current batch.
    // Textured/untextured need different shaders; different depths use different
    // sampling functions and cannot share a draw call.
    // Semi-transparent polygons need GL blending enabled; opaque ones do not — mixing
    // them in one batch causes the wrong blend state for one group.
    if (!vertex_buffer_.empty()) {
        bool batch_textured = (vertex_buffer_[0].flags & (1 << 0)) != 0;
        bool batch_semi     = (vertex_buffer_[0].flags & (1 << 2)) != 0;
        uint32_t batch_depth = (vertex_buffer_[0].flags >> 3) & 3;
        if (batch_textured != state.textured || (state.textured && batch_depth != eff_depth) ||
            batch_semi != state.semi_transparent) {
            FlushPrimitives();
        }
    }

    // Check if buffer is full (need to flush before adding)
    if (vertex_buffer_.size() + 3 > 4096) {
        FlushPrimitives();
    }

    // For textured triangles, extract CLUT and texpage from first two vertices
    // (PS1 primitive format: CLUT in first vertex, texpage in second vertex)
    float clut_x = 0.0f, clut_y = 0.0f;
    float texpage_x = 0.0f, texpage_y = 0.0f;

    if (state.textured) {
        // CLUT from first vertex (if present)
        if (v[0].has_clut) {
            clut_x = static_cast<float>(v[0].clut_x);
            clut_y = static_cast<float>(v[0].clut_y);
        }

        // Texpage from second vertex (if present).
        // texpage_x is a 0-15 base unit; each unit = 64 VRAM pixels.
        // texpage_y is a 0-3 base unit; each unit = 256 VRAM scanlines.
        // Per-polygon tpage (has_texpage=true) overrides global E1 draw mode.
        if (v[1].has_texpage) {
            texpage_x = static_cast<float>(v[1].texpage_x) * 64.0f;
            texpage_y = static_cast<float>(v[1].texpage_y) * 256.0f;
        }
    }

    /* DIAG: log first 10 textured polys per frame at key frames to diagnose depth/tpage */
    if (state.textured && (g_ps1_frame == 4248 || g_ps1_frame == 4410)) {
        static uint32_t s_dbg_frame = 0xFFFFFFFFu;
        static int s_dbg_cnt = 0;
        if (g_ps1_frame != s_dbg_frame) {
            s_dbg_cnt = 0; s_dbg_frame = g_ps1_frame;
            /* On new frame, probe terrain CLUTs from vram_pixels_ */
            auto probe = [&](int cx, int cy) {
                const uint16_t* p = vram_pixels_ + cy * 1024 + cx;
                printf("[CLUT2] f%u (%d,%d): %04X %04X %04X %04X\n",
                       g_ps1_frame, cx, cy, p[0], p[1], p[2], p[3]);
                fflush(stdout);
            };
            probe(144, 492); probe(160, 481); probe(288, 484);
        }
        if (s_dbg_cnt < 10) {
            /* [TPDBG] printf("[TPDBG] f%u #%d: has_tp=%d tpX=%g tpY=%g ...\n", ...); */
            fflush(stdout);
            ++s_dbg_cnt;
        }
    }

    // Convert 3 PS1 vertices to OpenGL format
    for (int i = 0; i < 3; i++) {
        OpenGLVertex gl_vert = {};

        // Position: PS1 coordinates (int16_t) to float
        // Drawing offset is already applied in v[i].x, v[i].y
        gl_vert.x = static_cast<float>(v[i].x);
        gl_vert.y = static_cast<float>(v[i].y);

        // Color: uint8_t (0-255) to float (0.0-1.0)
        gl_vert.r = v[i].r / 255.0f;
        gl_vert.g = v[i].g / 255.0f;
        gl_vert.b = v[i].b / 255.0f;
        gl_vert.a = 1.0f;

        // Texture coordinates: uint8_t (0-255) to float (0.0-1.0)
        if (state.textured && v[i].has_texture) {
            gl_vert.u = v[i].u / 255.0f;
            gl_vert.v = v[i].v / 255.0f;
        } else {
            gl_vert.u = 0.0f;
            gl_vert.v = 0.0f;
        }

        // CLUT position (same for all vertices in a triangle)
        gl_vert.clut_x = clut_x;
        gl_vert.clut_y = clut_y;

        // Texture page position (same for all vertices in a triangle)
        gl_vert.texpage_x = texpage_x;
        gl_vert.texpage_y = texpage_y;

        // Flags: Pack rendering mode into single uint32_t
        gl_vert.flags = 0;
        if (state.textured) gl_vert.flags |= (1 << 0);
        if (state.gouraud) gl_vert.flags |= (1 << 1);
        if (state.semi_transparent) gl_vert.flags |= (1 << 2);
        gl_vert.flags |= (eff_depth & 3) << 3; // bits 3-4: texture depth (per-polygon overrides global E1)

        // Add vertex to buffer
        vertex_buffer_.push_back(gl_vert);
    }


    // TODO: Check if state changed -> trigger flush
    // For now, we'll batch everything and flush manually in Present()
}

void OpenGLRenderer::DrawLine(const Vertex v[2], const DrawState& state) {
    if (!opengl_initialized_) {
        return;
    }

    // Check if primitive type changed (need to flush before switching)
    if (!vertex_buffer_.empty() && primitive_type_ != GL_LINES) {
        FlushPrimitives();
    }
    primitive_type_ = GL_LINES;

    // Check if buffer is full (need to flush before adding)
    if (vertex_buffer_.size() + 2 > 4096) {
        FlushPrimitives();
    }

    // Convert 2 PS1 vertices to OpenGL format
    for (int i = 0; i < 2; i++) {
        OpenGLVertex gl_vert = {};

        // Position: PS1 coordinates (int16_t) to float
        gl_vert.x = static_cast<float>(v[i].x);
        gl_vert.y = static_cast<float>(v[i].y);

        // Color: uint8_t (0-255) to float (0.0-1.0)
        gl_vert.r = v[i].r / 255.0f;
        gl_vert.g = v[i].g / 255.0f;
        gl_vert.b = v[i].b / 255.0f;
        gl_vert.a = 1.0f;

        // Texture coordinates (unused for lines, but set to 0)
        gl_vert.u = 0.0f;
        gl_vert.v = 0.0f;

        // CLUT and texpage (unused for lines)
        gl_vert.clut_x = 0.0f;
        gl_vert.clut_y = 0.0f;
        gl_vert.texpage_x = 0.0f;
        gl_vert.texpage_y = 0.0f;

        // Flags: Pack rendering mode
        gl_vert.flags = 0;
        if (state.textured) gl_vert.flags |= (1 << 0);  // Textured (unused for lines)
        if (state.gouraud) gl_vert.flags |= (1 << 1);   // Gouraud shading
        if (state.semi_transparent) gl_vert.flags |= (1 << 2);  // Semi-transparent

        // Add vertex to buffer
        vertex_buffer_.push_back(gl_vert);
    }
}

void OpenGLRenderer::DrawPolyline(const Vertex* vertices, int count, const DrawState& state) {
    if (!opengl_initialized_ || count < 2) {
        return;
    }

    // Check if primitive type changed (need to flush before switching)
    if (!vertex_buffer_.empty() && primitive_type_ != GL_LINE_STRIP) {
        FlushPrimitives();
    }
    primitive_type_ = GL_LINE_STRIP;

    // Check if buffer is full (need to flush before adding)
    if (vertex_buffer_.size() + count > 4096) {
        FlushPrimitives();
    }

    // Convert all PS1 vertices to OpenGL format
    for (int i = 0; i < count; i++) {
        OpenGLVertex gl_vert = {};

        // Position: PS1 coordinates (int16_t) to float
        gl_vert.x = static_cast<float>(vertices[i].x);
        gl_vert.y = static_cast<float>(vertices[i].y);

        // Color: uint8_t (0-255) to float (0.0-1.0)
        gl_vert.r = vertices[i].r / 255.0f;
        gl_vert.g = vertices[i].g / 255.0f;
        gl_vert.b = vertices[i].b / 255.0f;
        gl_vert.a = 1.0f;

        // Texture coordinates (unused for lines, but set to 0)
        gl_vert.u = 0.0f;
        gl_vert.v = 0.0f;

        // CLUT and texpage (unused for lines)
        gl_vert.clut_x = 0.0f;
        gl_vert.clut_y = 0.0f;
        gl_vert.texpage_x = 0.0f;
        gl_vert.texpage_y = 0.0f;

        // Flags: Pack rendering mode
        gl_vert.flags = 0;
        if (state.textured) gl_vert.flags |= (1 << 0);  // Textured (unused for lines)
        if (state.gouraud) gl_vert.flags |= (1 << 1);   // Gouraud shading
        if (state.semi_transparent) gl_vert.flags |= (1 << 2);  // Semi-transparent

        // Add vertex to buffer
        vertex_buffer_.push_back(gl_vert);
    }

    // Flush immediately after polyline (can't batch with other polylines easily)
    FlushPrimitives();
}

void OpenGLRenderer::DrawRectangle(int x, int y, int w, int h,
                                   uint8_t u0, uint8_t v0,
                                   uint16_t clut_x, uint16_t clut_y,
                                   uint8_t r, uint8_t g, uint8_t b,
                                   const DrawState& state) {
    if (!opengl_initialized_) {
        return;
    }

    static uint32_t s_rect_count = 0;
    ++s_rect_count;
    // Log only the first few rects (startup diagnostics only — never per-frame)
    if (s_rect_count <= 3) {
        printf("[DrawRect] #%u: pos=(%d,%d) size=%dx%d tex=%d semi=%d "
               "clut=(%d,%d) tpg=(%d,%d) depth=%d uv=(%d,%d) rgb=(%d,%d,%d)\n",
               s_rect_count, x, y, w, h, state.textured, state.semi_transparent,
               (int)clut_x, (int)clut_y,
               state.texpage_x_base * 64, state.texpage_y_base * 256,
               state.texture_depth,
               (int)u0, (int)v0, (int)r, (int)g, (int)b);
        fflush(stdout);
    }

    // Rectangles are rendered as triangles
    if (!vertex_buffer_.empty() && primitive_type_ != GL_TRIANGLES) {
        FlushPrimitives();
    }
    primitive_type_ = GL_TRIANGLES;

    // Flush if textured state or semi-transparent flag differs from current batch
    if (!vertex_buffer_.empty()) {
        bool batch_textured = (vertex_buffer_[0].flags & (1 << 0)) != 0;
        bool batch_semi     = (vertex_buffer_[0].flags & (1 << 2)) != 0;
        if (batch_textured != state.textured || batch_semi != state.semi_transparent) {
            FlushPrimitives();
        }
    }

    // Check if buffer is full (need 6 vertices for 2 triangles)
    if (vertex_buffer_.size() + 6 > 4096) {
        FlushPrimitives();
    }

    // Convert color to float
    float r_f = r / 255.0f;
    float g_f = g / 255.0f;
    float b_f = b / 255.0f;

    // Compute UV corners.  PS1 UV coords are 0-255 in texture-page space;
    // the shader multiplies them back by 255 internally, so we normalise here.
    float u_l  = u0 / 255.0f;
    float u_r  = (u0 + w) / 255.0f;
    float v_t  = v0 / 255.0f;
    float v_b  = (v0 + h) / 255.0f;

    // Texpage from draw mode: base unit (0-15) -> VRAM pixels (* 64 for X, * 256 for Y)
    float tpx  = static_cast<float>(state.texpage_x_base) * 64.0f;
    float tpy  = static_cast<float>(state.texpage_y_base) * 256.0f;

    // Create OpenGLVertex template (shared fields)
    OpenGLVertex vert_template = {};
    vert_template.r = r_f;
    vert_template.g = g_f;
    vert_template.b = b_f;
    vert_template.a = 1.0f;
    vert_template.clut_x  = static_cast<float>(clut_x);
    vert_template.clut_y  = static_cast<float>(clut_y);
    vert_template.texpage_x = tpx;
    vert_template.texpage_y = tpy;
    vert_template.flags = 0;
    if (state.textured) vert_template.flags |= (1 << 0);
    if (state.gouraud)  vert_template.flags |= (1 << 1);
    if (state.semi_transparent) vert_template.flags |= (1 << 2);
    vert_template.flags |= ((uint32_t)state.texture_depth & 3) << 3; // bits 3-4: texture depth

    // Triangle 1: TL, TR, BR
    OpenGLVertex vTL = vert_template;
    vTL.x = static_cast<float>(x);
    vTL.y = static_cast<float>(y);
    vTL.u = u_l; vTL.v = v_t;
    vertex_buffer_.push_back(vTL);

    OpenGLVertex vTR = vert_template;
    vTR.x = static_cast<float>(x + w);
    vTR.y = static_cast<float>(y);
    vTR.u = u_r; vTR.v = v_t;
    vertex_buffer_.push_back(vTR);

    OpenGLVertex vBR = vert_template;
    vBR.x = static_cast<float>(x + w);
    vBR.y = static_cast<float>(y + h);
    vBR.u = u_r; vBR.v = v_b;
    vertex_buffer_.push_back(vBR);

    // Triangle 2: TL, BR, BL
    vertex_buffer_.push_back(vTL);
    vertex_buffer_.push_back(vBR);

    OpenGLVertex vBL = vert_template;
    vBL.x = static_cast<float>(x);
    vBL.y = static_cast<float>(y + h);
    vBL.u = u_l; vBL.v = v_b;
    vertex_buffer_.push_back(vBL);
}

//==============================================================================
// VRAM Operations
//==============================================================================

void OpenGLRenderer::FillRectangle(int x, int y, int w, int h, uint16_t color) {
    // Extend framebuffer clears to cover the full display height.
    // Games clear only the GP1(07h)-visible portion of each framebuffer
    // (e.g., 224 lines instead of 240), relying on CRT overscan to hide the
    // remaining lines.  On digital displays, stale content in those lines
    // causes visible flicker during scene transitions.  If a FillRect looks
    // like a full-buffer clear (width >= display width, height close to but
    // less than display height), extend it to the full display height.
    if (display_height_ > 0 && w >= display_width_ &&
        h >= display_height_ - 32 && h < display_height_ &&
        y + display_height_ <= 512) {
        h = display_height_;
    }

    // Clamp to VRAM bounds
    if (x >= 1024 || y >= 512 || w <= 0 || h <= 0) return;
    if (x + w > 1024) w = 1024 - x;
    if (y + h > 512)  h = 512  - y;

    vram_read_dirty_ = true;  // CPU wrote to VRAM — read texture needs resync

    // Update CPU-side VRAM copy
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            vram_pixels_[(y + dy) * 1024 + (x + dx)] = color;
        }
    }

    // Render through the FBO using fill_shader_ instead of glTexSubImage2D.
    // This avoids coherency issues between CPU texture uploads and FBO rendering
    // on the same texture (which can cause lost renders on some GPU drivers).
    if (opengl_initialized_ && fill_shader_ != 0) {
        // Flush any pending primitives first — FBO state must be clean.
        FlushPrimitives();

        // Convert 16-bit 1555 color to float RGBA
        float r = ((color >>  0) & 0x1F) / 31.0f;
        float g = ((color >>  5) & 0x1F) / 31.0f;
        float b = ((color >> 10) & 0x1F) / 31.0f;
        float a = ((color >> 15) & 1) ? 1.0f : 0.0f;

        // Set up fill quad as 2 triangles
        OpenGLVertex quad[6] = {};
        float fx = (float)x, fy = (float)y;
        float fw = (float)w, fh = (float)h;
        quad[0].x = fx;      quad[0].y = fy;
        quad[1].x = fx + fw; quad[1].y = fy;
        quad[2].x = fx + fw; quad[2].y = fy + fh;
        quad[3].x = fx;      quad[3].y = fy;
        quad[4].x = fx + fw; quad[4].y = fy + fh;
        quad[5].x = fx;      quad[5].y = fy + fh;

        // Bind VRAM FBO and render
        glBindFramebuffer(GL_FRAMEBUFFER, vram_fbo_);
        glViewport(0, 0, 1024, 512);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);

        glUseProgram(fill_shader_);
        GLint loc_color = glGetUniformLocation(fill_shader_, "uColor");
        glUniform4f(loc_color, r, g, b, a);

        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glUseProgram(0);

        // Invalidate cached GL state — we disabled scissor/blend without
        // updating the cache, so force the next ApplyDrawingArea/ApplyBlendMode
        // to re-apply the correct state.
        cached_state_.scissor_x = -1;
        cached_state_.scissor_y = -1;
        cached_state_.scissor_w = -1;
        cached_state_.scissor_h = -1;
        cached_state_.blending_enabled = false;
        cached_state_.blend_mode = 0xFF;
    }
}

void OpenGLRenderer::UploadToVRAM(int x, int y, int w, int h, const uint16_t* data) {
    if (!opengl_initialized_) {
        return;
    }

    glBindTexture(GL_TEXTURE_2D, vram_texture_);

    // PS1 VRAM: x wraps mod 1024, y wraps mod 512 within the upload rectangle.
    // Fast path for the common (non-wrapping) case.
    vram_read_dirty_ = true;  // CPU wrote to VRAM — read texture needs resync

    /* [UL-DIAG] CLUT restore trace for (288,480,48,31) — commented out (re-enable to diagnose foliage CLUT) */

    bool wraps_inner = ((x + w) > 1024) || ((y + h) > 512);
    if (!wraps_inner) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h,
                        GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, data);
        for (int dy = 0; dy < h; dy++)
            for (int dx = 0; dx < w; dx++)
                vram_pixels_[(y + dy) * 1024 + (x + dx)] = data[dy * w + dx];
    } else {
        // Wrapping path: write with modular addressing then re-upload full texture.
        printf("[UPLOAD-WRAP] f%u (%d,%d) %dx%d — full vram_texture_ re-upload!\n",
               g_ps1_frame, x, y, w, h);
        fflush(stdout);
        for (int row = 0; row < h; row++) {
            int vy = (y + row) & 511;
            for (int col = 0; col < w; col++) {
                int vx = (x + col) & 1023;
                vram_pixels_[vy * 1024 + vx] = data[row * w + col];
            }
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1024, 512, 0,
                     GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, vram_pixels_);
    }
}

void OpenGLRenderer::DownloadFromVRAM(int x, int y, int w, int h, uint16_t* data) {
    if (!opengl_initialized_) {
        return;
    }

    // Clamp to VRAM bounds
    if (x >= 1024 || y >= 512) return;
    if (x + w > 1024) w = 1024 - x;
    if (y + h > 512) h = 512 - y;

    // Read from GPU framebuffer
    glBindFramebuffer(GL_READ_FRAMEBUFFER, vram_fbo_);
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, data);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    /* [DL-DIAG] CLUT download trace for (288,480) — commented out (re-enable to diagnose foliage CLUT) */

    // Also update CPU-side VRAM copy
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            vram_pixels_[(y + dy) * 1024 + (x + dx)] = data[dy * w + dx];
        }
    }
}

void OpenGLRenderer::CopyVRAM(int src_x, int src_y, int dst_x, int dst_y, int w, int h) {
    if (!opengl_initialized_) {
        return;
    }

    vram_read_dirty_ = true;  // CPU wrote to VRAM — read texture needs resync

    // Clamp to VRAM bounds
    if (src_x >= 1024 || src_y >= 512 || dst_x >= 1024 || dst_y >= 512) return;
    if (src_x + w > 1024) w = 1024 - src_x;
    if (src_y + h > 512) h = 512 - src_y;
    if (dst_x + w > 1024) w = 1024 - dst_x;
    if (dst_y + h > 512) h = 512 - dst_y;

    // Use glBlitFramebuffer for fast VRAM-to-VRAM copy
    glBindFramebuffer(GL_READ_FRAMEBUFFER, vram_fbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, vram_fbo_);
    glBlitFramebuffer(src_x, src_y, src_x + w, src_y + h,
                      dst_x, dst_y, dst_x + w, dst_y + h,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    // Also update CPU-side VRAM copy
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            vram_pixels_[(dst_y + dy) * 1024 + (dst_x + dx)] =
                vram_pixels_[(src_y + dy) * 1024 + (src_x + dx)];
        }
    }
}

//==============================================================================
// State Management
//==============================================================================

void OpenGLRenderer::SetDrawMode(const DrawMode& mode) {
    // Flush pending primitives before changing texture depth or semi-transparency mode,
    // since these affect the shader dispatch path (uTextureDepth uniform) and blending.
    if (!vertex_buffer_.empty()) {
        if (mode.texture_depth != draw_mode_.texture_depth ||
            mode.semi_transparency != draw_mode_.semi_transparency) {
            FlushPrimitives();
        }
    }
    draw_mode_ = mode;
}

void OpenGLRenderer::SetTextureWindow(const TextureWindow& window) {
    // TODO: Update texture_window_, trigger flush if state changed
    texture_window_ = window;
}

void OpenGLRenderer::SetDrawingArea(const DrawingArea& area) {
    // Flush pending primitives before changing draw area — the scissor test
    // in FlushPrimitives uses drawing_area_, so we must render any queued
    // vertices under the OLD area before switching to the new one.
    bool area_changed = (area.x1 != drawing_area_.x1 || area.y1 != drawing_area_.y1 ||
                         area.x2 != drawing_area_.x2 || area.y2 != drawing_area_.y2);
    if (!vertex_buffer_.empty() && area_changed) {
        FlushPrimitives();
    }
    drawing_area_ = area;
}

void OpenGLRenderer::SetDrawingOffset(const DrawingOffset& offset) {
    drawing_offset_ = offset;
    static int s_offs_log = 0; ++s_offs_log;
    /* [DrawOfs] first 20 — re-enable: if (s_offs_log <= 20) printf("[DrawOfs] #%d: (%d,%d)\n", ...); */
}

void OpenGLRenderer::SetMaskSettings(const MaskSettings& settings) {
    // TODO: Update mask_settings_
    mask_settings_ = settings;
}

void OpenGLRenderer::ClearTextureCache() {
    // TODO: Invalidate any cached texture data
    // (Not critical for initial implementation)
}

//==============================================================================
// Display Control
//==============================================================================

void OpenGLRenderer::SetDisplayArea(int x, int y) {
    display_area_x_ = x;
    display_area_y_ = y;
}

void OpenGLRenderer::SetDisplayMode(int width, int height, bool is_24bit, bool interlace) {
    display_width_ = width;
    display_height_ = height;
    display_24bit_ = is_24bit;
    display_interlace_ = interlace;
}

void OpenGLRenderer::SetVerticalDisplayRange(int y1, int y2) {
    v_display_range_y1_ = y1;
    v_display_range_y2_ = y2;
}

void OpenGLRenderer::SetHorizontalDisplayRange(int x1, int x2) {
    h_display_range_x1_ = x1;
    h_display_range_x2_ = x2;
}

void OpenGLRenderer::Present() {
    if (!opengl_initialized_) {
        return;
    }

    // Step 1: Flush any pending primitives to VRAM
    FlushPrimitives();

    // Step 2: Bind default framebuffer (screen)
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Step 3: Query actual framebuffer size (handles window resize) and compute
    // a letterboxed viewport that preserves the PS1 display aspect ratio.
    {
        int fb_w, fb_h;
        glfwGetFramebufferSize(window_, &fb_w, &fb_h);
        window_width_  = fb_w;
        window_height_ = fb_h;
    }
    // Compute visible pixel dimensions from H/V display ranges.
    // On a real PS1/CRT the overscan region outside the H/V display ranges is
    // hidden by the TV bezel.  We use these to crop the VRAM blit UV coords so
    // those lines don't appear on screen.
    // NOTE: the *aspect ratio* always uses display_width_/display_height_ (the
    // mode resolution), NOT visible_w/visible_h.  This preserves the 4:3 viewport
    // geometry that was correct before, while only the UV sampling changes.
    // H overscan: visible_w = (x2-x1)/dots_per_pixel.
    //   256px→10, 320px→8, 512px→5, 640px→4  (53.693MHz / pixel-clock).
    int visible_w = display_width_;
    int visible_h = display_height_;
    if (h_display_range_x2_ > h_display_range_x1_ && display_width_ > 0) {
        int dpp = (display_width_ <= 256) ? 10 :
                  (display_width_ <= 320) ? 8  :
                  (display_width_ <= 512) ? 5  : 4;
        int hw = (h_display_range_x2_ - h_display_range_x1_) / dpp;
        if (hw > 0 && hw < display_width_)
            visible_w = hw;
    }
    // Only apply the overscan crop for progressive modes (display_height_ <= 240).
    // In 480i (display_height_==480), the V range describes one field (240 lines) but
    // VRAM holds both fields stacked; dividing by that would cut the frame in half.
    if (v_display_range_y2_ > v_display_range_y1_ && display_height_ > 0 && display_height_ <= 240) {
        int vh = v_display_range_y2_ - v_display_range_y1_;
        if (vh > 0 && vh < display_height_)
            visible_h = vh;
    }

    {
        // Aspect ratio uses the full mode resolution so the viewport geometry is
        // unchanged from before this overscan fix.
        float game_w      = (display_width_  > 0) ? (float)display_width_  : 320.0f;
        float game_h      = (display_height_ > 0) ? (float)display_height_ : 240.0f;
        float game_aspect = game_w / game_h;
        float win_aspect  = (float)window_width_ / (float)window_height_;
        int vp_x, vp_y, vp_w, vp_h;
        if (win_aspect > game_aspect) {
            /* Window wider — pillarbox (black bars left/right) */
            vp_h = window_height_;
            vp_w = (int)(window_height_ * game_aspect);
            vp_x = (window_width_ - vp_w) / 2;
            vp_y = 0;
        } else {
            /* Window taller — letterbox (black bars top/bottom) */
            vp_w = window_width_;
            vp_h = (int)(window_width_ / game_aspect);
            vp_x = 0;
            vp_y = (window_height_ - vp_h) / 2;
        }
        glViewport(vp_x, vp_y, vp_w, vp_h);
    }

    // Step 4: Clear screen
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Step 5: Render the active PS1 framebuffer to the window.
    // Samples only the visible_w × visible_h region from VRAM (overscan excluded).
    //
    // If display_area_y_ is still 0 (GP1(05h) not yet received), fall back to
    // showing the full VRAM so there's always something on screen.
    float u0, v0, u1, v1;
    if (display_area_y_ == 0 && display_area_x_ == 0) {
        // Full VRAM fallback
        u0 = 0.0f; v0 = 0.0f; u1 = 1.0f; v1 = 1.0f;
    } else {
        u0 = display_area_x_ / 1024.0f;
        v0 = display_area_y_ / 512.0f;
        u1 = (display_area_x_ + visible_w) / 1024.0f;
        v1 = (display_area_y_ + visible_h) / 512.0f;
    }

    OpenGLVertex quad[6] = {};
    // Triangle 1: TL, TR, BR
    quad[0].x = 0.0f;    quad[0].y = 0.0f;    quad[0].u = u0; quad[0].v = v0;
    quad[1].x = 1024.0f; quad[1].y = 0.0f;    quad[1].u = u1; quad[1].v = v0;
    quad[2].x = 1024.0f; quad[2].y = 512.0f;  quad[2].u = u1; quad[2].v = v1;
    // Triangle 2: TL, BR, BL
    quad[3].x = 0.0f;    quad[3].y = 0.0f;    quad[3].u = u0; quad[3].v = v0;
    quad[4].x = 1024.0f; quad[4].y = 512.0f;  quad[4].u = u1; quad[4].v = v1;
    quad[5].x = 0.0f;    quad[5].y = 512.0f;  quad[5].u = u0; quad[5].v = v1;

    // Bind VAO and upload quad vertices
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);

    // Use copy shader (simple texture blit — no PS1 CLUT decoding)
    glUseProgram(copy_shader_);

    // Disable scissor test for fullscreen blit
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);

    // Bind VRAM texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, vram_texture_);
    glUniform1i(uloc_copy_source_, 0);

    // Draw fullscreen quad
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Unbind everything
    glBindVertexArray(0);
    glUseProgram(0);

    // Step 6: Swap buffers (display to screen)
    glfwSwapBuffers(window_);

    // Step 7: Poll for events (window close, input, etc.)
    glfwPollEvents();

    // Check for OpenGL errors
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        printf("[OpenGLRenderer] OpenGL error during Present: 0x%X\n", error);
    }
}

void OpenGLRenderer::VSync() {
    // TODO: Wait for vertical sync (60Hz NTSC / 50Hz PAL)
    // Can use glfwSwapInterval(1) for automatic vsync

    // STUB
}

//==============================================================================
// Initialization Helpers
//==============================================================================

bool OpenGLRenderer::InitializeWindow() {
    printf("[OpenGLRenderer] InitializeWindow() - Creating GLFW window...\n");

    // Initialize GLFW
    if (!glfwInit()) {
        printf("[OpenGLRenderer] ERROR: Failed to initialize GLFW\n");
        return false;
    }

    // Set OpenGL version hints (3.3 Core Profile)
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

#ifdef __APPLE__
    // macOS requires this for Core Profile
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    // Don't steal focus from the user's active window
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    // Create window
    window_ = glfwCreateWindow(window_width_, window_height_, game_get_name(), nullptr, nullptr);
    if (!window_) {
        printf("[OpenGLRenderer] ERROR: Failed to create GLFW window\n");
        glfwTerminate();
        return false;
    }

    // Make the OpenGL context current
    glfwMakeContextCurrent(window_);

    // Show the window minimized so it doesn't steal focus during automated runs
    glfwIconifyWindow(window_);
    glfwShowWindow(window_);

    // Disable driver VSync — we use our own 30 FPS software throttle.
    // With VSync on, glfwSwapBuffers() blocks until the next monitor refresh
    // (16.7 ms at 60 Hz).  Our ~33 ms render budget then falls on every 2nd
    // or 3rd vblank randomly, giving jittery 20-30 FPS instead of steady 30.
    glfwSwapInterval(0);

    printf("[OpenGLRenderer] GLFW window created successfully (%dx%d)\n", window_width_, window_height_);
    return true;
}

bool OpenGLRenderer::InitializeOpenGL() {
    printf("[OpenGLRenderer] InitializeOpenGL() - Loading OpenGL functions...\n");

    // Load OpenGL function pointers with GLAD
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        printf("[OpenGLRenderer] ERROR: Failed to initialize GLAD (OpenGL function loader)\n");
        return false;
    }

    // Print OpenGL version info
    const char* version = (const char*)glGetString(GL_VERSION);
    const char* renderer = (const char*)glGetString(GL_RENDERER);
    const char* vendor = (const char*)glGetString(GL_VENDOR);

    printf("[OpenGLRenderer] OpenGL initialized successfully!\n");
    printf("[OpenGLRenderer]   Version:  %s\n", version);
    printf("[OpenGLRenderer]   Renderer: %s\n", renderer);
    printf("[OpenGLRenderer]   Vendor:   %s\n", vendor);

    // Verify we have at least OpenGL 3.3
    GLint major, minor;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);

    if (major < 3 || (major == 3 && minor < 3)) {
        printf("[OpenGLRenderer] ERROR: OpenGL 3.3 or higher required (got %d.%d)\n", major, minor);
        return false;
    }

    printf("[OpenGLRenderer] OpenGL %d.%d Core Profile ready\n", major, minor);
    return true;
}

bool OpenGLRenderer::InitializeShaders() {
    printf("[OpenGLRenderer] InitializeShaders() - Compiling shaders...\n");

    // ========== 1. PRIMITIVE SHADER (Flat/Gouraud, Untextured) ==========
    {
        const char* vertex_source = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;

out vec4 vColor;

void main() {
    // VRAM coords (0-1023 x, 0-511 y) to NDC. No Y-flip here: FBO rows must
    // match CPU-uploaded texture rows (both VRAM Y=0 -> row 0 -> UV.y=0).
    // Y-flip only happens in copy_shader_ when blitting to the screen.
    float x = (aPos.x / 1024.0) * 2.0 - 1.0;
    float y = (aPos.y / 512.0) * 2.0 - 1.0;
    gl_Position = vec4(x, y, 0.0, 1.0);
    vColor = aColor;
}
)";

        const char* fragment_source = R"(
#version 330 core
in vec4 vColor;
out vec4 FragColor;

uniform bool uDithering;         // Dithering enabled?
uniform bool uCheckMaskBit;      // Check mask bit before drawing?
uniform bool uSetMaskBit;        // Set mask bit when drawing?
uniform sampler2D uVRAMTexture;  // For mask bit reading

// 4x4 ordered dithering matrix (Bayer matrix)
const int dither_matrix[16] = int[](
    -4,  0, -3,  1,
     2, -2,  3, -1,
    -3,  1, -4,  0,
     3, -1,  2, -2
);

void main() {
    vec4 color = vColor;

    // Apply dithering if enabled (before 15-bit quantization)
    if (uDithering) {
        int x = int(gl_FragCoord.x) & 3;  // mod 4
        int y = int(gl_FragCoord.y) & 3;  // mod 4
        int dither_value = dither_matrix[y * 4 + x];

        // Add dither offset (range: -4 to 3, normalized to -1/64 to 3/64)
        color.rgb += vec3(float(dither_value) / 255.0);
        color.rgb = clamp(color.rgb, 0.0, 1.0);
    }

    // Check mask bit if enabled (read from VRAM at current pixel)
    if (uCheckMaskBit) {
        // Read current VRAM pixel at this location
        vec2 vram_coord = vec2(gl_FragCoord.x / 1024.0, gl_FragCoord.y / 512.0);
        vec4 existing_pixel = texture(uVRAMTexture, vram_coord);

        // If alpha component indicates mask bit is set, discard this pixel
        // In RGB5A1 format, alpha > 0.5 means bit 15 is set (mask bit)
        if (existing_pixel.a > 0.5) {
            discard;
        }
    }

    // Convert to 15-bit color space (5-5-5) for PS1 accuracy
    color.rgb = floor(color.rgb * 31.0) / 31.0;

    // Set mask bit if enabled (force alpha to 1.0)
    if (uSetMaskBit) {
        color.a = 1.0;
    } else {
        color.a = 0.0;
    }

    FragColor = color;
}
)";

        GLuint vs = CompileShader(GL_VERTEX_SHADER, vertex_source);
        GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragment_source);
        if (vs == 0 || fs == 0) {
            printf("[OpenGLRenderer] ERROR: Failed to compile primitive shader\n");
            return false;
        }

        primitive_shader_ = LinkProgram(vs, fs);
        if (primitive_shader_ == 0) {
            return false;
        }

        printf("[OpenGLRenderer]   - Primitive shader compiled (program ID: %u)\n", primitive_shader_);

        uloc_prim_dithering_  = glGetUniformLocation(primitive_shader_, "uDithering");
        uloc_prim_check_mask_ = glGetUniformLocation(primitive_shader_, "uCheckMaskBit");
        uloc_prim_set_mask_   = glGetUniformLocation(primitive_shader_, "uSetMaskBit");
        uloc_prim_vram_       = glGetUniformLocation(primitive_shader_, "uVRAMTexture");
    }

    // ========== 2. TEXTURED SHADER (with CLUT support - 4-bit/8-bit/15-bit) ==========
    {
        const char* vertex_source = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in vec2 aCLUT;
layout(location = 4) in vec2 aTexpage;

out vec4 vColor;
out vec2 vTexCoord;
out vec2 vCLUT;
out vec2 vTexpage;

void main() {
    float x = (aPos.x / 1024.0) * 2.0 - 1.0;
    float y = (aPos.y / 512.0) * 2.0 - 1.0;
    gl_Position = vec4(x, y, 0.0, 1.0);  // No Y-flip: keep FBO/CPU-upload rows aligned
    vColor = aColor;
    vTexCoord = aTexCoord;
    vCLUT = aCLUT;
    vTexpage = aTexpage;
}
)";

        const char* fragment_source = R"(
#version 330 core
in vec4 vColor;
in vec2 vTexCoord;
in vec2 vCLUT;
in vec2 vTexpage;
out vec4 FragColor;

uniform sampler2D uVRAMTexture;  // Full 1024x512 VRAM
uniform int uTextureDepth;       // 0=4bit, 1=8bit, 2=15bit
uniform vec4 uTextureWindow;     // (mask_x, mask_y, offset_x, offset_y) in pixels
uniform bool uDithering;         // Dithering enabled?
uniform bool uCheckMaskBit;      // Check mask bit before drawing?
uniform bool uSetMaskBit;        // Set mask bit when drawing?

// Sample 15-bit direct color texture
vec4 SampleTexture15Bit(vec2 uv, vec2 texpage) {
    // Calculate texture page base address
    float texpage_x = texpage.x;  // Already in pixels (0-960 in 64-pixel steps)
    float texpage_y = texpage.y;  // Already in pixels (0 or 256)

    // Apply texture coordinates (0.0-1.0 maps to 0-255 pixels in PS1 space)
    float tex_x = texpage_x + (uv.x * 255.0);
    float tex_y = texpage_y + (uv.y * 255.0);

    // Apply texture window (if enabled)
    if (uTextureWindow.x > 0.0 || uTextureWindow.y > 0.0) {
        // AND mask (wrapping)
        tex_x = mod(tex_x, uTextureWindow.x) + uTextureWindow.z;
        tex_y = mod(tex_y, uTextureWindow.y) + uTextureWindow.w;
    }

    // Convert to VRAM texture coordinates (0.0-1.0)
    vec2 vram_uv = vec2(tex_x / 1024.0, tex_y / 512.0);

    // Sample from VRAM. No channel swizzle needed: GL_RGBA+1_5_5_5_REV maps
    // PS1 R→texel.r, PS1 G→texel.g, PS1 B→texel.b directly.
    vec4 texel = texture(uVRAMTexture, vram_uv);
    return texel;
}

// Sample 4-bit CLUT indexed texture
vec4 SampleTexture4Bit(vec2 uv, vec2 texpage, vec2 clut) {
    // texpage.x is already in VRAM pixels (0, 64, 128, ... up to 960).
    // For 4-bit packing, 4 texels share one 16-bit VRAM word, so the
    // VRAM X offset of a texel at index i is texpage.x + i/4.
    float tex_x = texpage.x + (uv.x * 255.0) / 4.0;
    float tex_y = texpage.y + (uv.y * 255.0);

    // Apply texture window
    if (uTextureWindow.x > 0.0 || uTextureWindow.y > 0.0) {
        tex_x = mod(tex_x, uTextureWindow.x / 4.0) + uTextureWindow.z / 4.0;
        tex_y = mod(tex_y, uTextureWindow.y) + uTextureWindow.w;
    }

    // Sample 16-bit value from VRAM (contains 4 indices, 4 bits each)
    vec2 vram_uv = vec2(tex_x / 1024.0, tex_y / 512.0);
    vec4 texel_raw = texture(uVRAMTexture, vram_uv);

    // Convert to 16-bit RGB5A1 value using int arithmetic (NVIDIA 3.3 compat)
    // Use +0.5 rounding to avoid float precision loss (n/31.0 * 31.0 can be n-epsilon)
    int p16 = int(texel_raw.r * 31.0 + 0.5) |
              (int(texel_raw.g * 31.0 + 0.5) << 5) |
              (int(texel_raw.b * 31.0 + 0.5) << 10) |
              (int(texel_raw.a + 0.5) << 15);

    // Extract 4-bit index based on X coordinate (which nibble)
    int pixel_x_4 = int(uv.x * 255.0);
    int nibble_index = pixel_x_4 & 3;  // 0-3
    int color_index = (p16 >> (nibble_index * 4)) & 0xF;  // Extract 4 bits

    // Look up color in CLUT (CLUT is 16x1 pixels at clut position)
    float clut_x = clut.x + float(color_index);
    float clut_y = clut.y;
    vec2 clut_uv = vec2(clut_x / 1024.0, clut_y / 512.0);

    // Sample CLUT color. GL_RGBA+1_5_5_5_REV maps bits[4:0]→R, bits[9:5]→G,
    // bits[14:10]→B, bit[15]→A — matching the PS1 pixel layout directly.
    // No channel swizzle needed.
    vec4 raw = texture(uVRAMTexture, clut_uv);
    return raw;
}

// Sample 8-bit CLUT indexed texture
vec4 SampleTexture8Bit(vec2 uv, vec2 texpage, vec2 clut) {
    // texpage.x is already in VRAM pixels. 8-bit packing: 2 texels per VRAM word.
    float texpage_x = texpage.x;
    float texpage_y = texpage.y;

    // Apply texture coordinates
    float tex_x = texpage_x + (uv.x * 255.0) / 2.0;  // Divide by 2 for packing
    float tex_y = texpage_y + (uv.y * 255.0);

    // Apply texture window
    if (uTextureWindow.x > 0.0 || uTextureWindow.y > 0.0) {
        tex_x = mod(tex_x, uTextureWindow.x / 2.0) + uTextureWindow.z / 2.0;
        tex_y = mod(tex_y, uTextureWindow.y) + uTextureWindow.w;
    }

    // Sample 16-bit value from VRAM (contains 2 indices, 8 bits each)
    vec2 vram_uv = vec2(tex_x / 1024.0, tex_y / 512.0);
    vec4 texel_raw8 = texture(uVRAMTexture, vram_uv);

    // Convert to 16-bit RGB5A1 value using int arithmetic (NVIDIA 3.3 compat)
    // Use +0.5 rounding to avoid float precision loss
    int p16b = int(texel_raw8.r * 31.0 + 0.5) |
               (int(texel_raw8.g * 31.0 + 0.5) << 5) |
               (int(texel_raw8.b * 31.0 + 0.5) << 10) |
               (int(texel_raw8.a + 0.5) << 15);

    // Extract 8-bit index based on X coordinate (low or high byte)
    int pixel_x_8 = int(uv.x * 255.0);
    int byte_index = pixel_x_8 & 1;  // 0 or 1
    int color_index = (p16b >> (byte_index * 8)) & 0xFF;  // Extract 8 bits

    // Look up color in CLUT (CLUT is 256x1 pixels at clut position)
    float clut_x = clut.x + float(color_index);
    float clut_y = clut.y;
    vec2 clut_uv = vec2(clut_x / 1024.0, clut_y / 512.0);

    // Sample CLUT color. No channel swizzle needed (GL_RGBA+1_5_5_5_REV maps
    // PS1 R→texel.r, PS1 G→texel.g, PS1 B→texel.b directly).
    vec4 raw8 = texture(uVRAMTexture, clut_uv);
    return raw8;
}

// 4x4 ordered dithering matrix (Bayer matrix)
const int dither_matrix[16] = int[](
    -4,  0, -3,  1,
     2, -2,  3, -1,
    -3,  1, -4,  0,
     3, -1,  2, -2
);

void main() {
    vec4 texel;

    // Sample texture based on depth mode
    if (uTextureDepth == 0) {
        // 4-bit CLUT
        texel = SampleTexture4Bit(vTexCoord, vTexpage, vCLUT);
    } else if (uTextureDepth == 1) {
        // 8-bit CLUT
        texel = SampleTexture8Bit(vTexCoord, vTexpage, vCLUT);
    } else {
        // 15-bit direct color
        texel = SampleTexture15Bit(vTexCoord, vTexpage);
    }

    // Check for transparent color key (RGB = 0,0,0 with A = 0)
    if (texel.rgb == vec3(0.0, 0.0, 0.0) && texel.a < 0.5) {
        discard;
    }

    // Modulate with vertex color (PS1 behavior: multiply and divide by 128, approximated as *2)
    vec4 color = texel * vColor * 2.0;

    // Apply dithering if enabled (before 15-bit quantization)
    if (uDithering) {
        int x = int(gl_FragCoord.x) & 3;  // mod 4
        int y = int(gl_FragCoord.y) & 3;  // mod 4
        int dither_value = dither_matrix[y * 4 + x];

        // Add dither offset (range: -4 to 3, normalized to -1/64 to 3/64)
        color.rgb += vec3(float(dither_value) / 255.0);
        color.rgb = clamp(color.rgb, 0.0, 1.0);
    }

    // Check mask bit if enabled (read from VRAM at current pixel)
    if (uCheckMaskBit) {
        // Read current VRAM pixel at this location
        vec2 vram_coord = vec2(gl_FragCoord.x / 1024.0, gl_FragCoord.y / 512.0);
        vec4 existing_pixel = texture(uVRAMTexture, vram_coord);

        // If alpha component indicates mask bit is set, discard this pixel
        if (existing_pixel.a > 0.5) {
            discard;
        }
    }

    // Convert to 15-bit color space (5-5-5)
    color.rgb = floor(color.rgb * 31.0) / 31.0;

    // Set mask bit if enabled (force alpha to 1.0)
    if (uSetMaskBit) {
        color.a = 1.0;
    } else {
        color.a = 0.0;
    }

    FragColor = color;
}
)";

        GLuint vs = CompileShader(GL_VERTEX_SHADER, vertex_source);
        GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragment_source);
        if (vs == 0 || fs == 0) {
            printf("[OpenGLRenderer] ERROR: Failed to compile textured shader\n");
            return false;
        }

        textured_shader_ = LinkProgram(vs, fs);
        if (textured_shader_ == 0) {
            return false;
        }

        printf("[OpenGLRenderer]   - Textured shader compiled (program ID: %u)\n", textured_shader_);

        uloc_tex_dithering_  = glGetUniformLocation(textured_shader_, "uDithering");
        uloc_tex_check_mask_ = glGetUniformLocation(textured_shader_, "uCheckMaskBit");
        uloc_tex_set_mask_   = glGetUniformLocation(textured_shader_, "uSetMaskBit");
        uloc_tex_vram_       = glGetUniformLocation(textured_shader_, "uVRAMTexture");
        uloc_tex_depth_      = glGetUniformLocation(textured_shader_, "uTextureDepth");
        uloc_tex_window_     = glGetUniformLocation(textured_shader_, "uTextureWindow");
    }

    // ========== 3. FILL SHADER (for GP0(02h) - Fill Rectangle) ==========
    {
        const char* vertex_source = R"(
#version 330 core
layout(location = 0) in vec2 aPos;

void main() {
    float x = (aPos.x / 1024.0) * 2.0 - 1.0;
    float y = (aPos.y / 512.0) * 2.0 - 1.0;
    gl_Position = vec4(x, y, 0.0, 1.0);  // No Y-flip (fill_shader renders to FBO)
}
)";

        const char* fragment_source = R"(
#version 330 core
out vec4 FragColor;

uniform vec4 uColor;

void main() {
    FragColor = uColor;
}
)";

        GLuint vs = CompileShader(GL_VERTEX_SHADER, vertex_source);
        GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragment_source);
        if (vs == 0 || fs == 0) {
            printf("[OpenGLRenderer] ERROR: Failed to compile fill shader\n");
            return false;
        }

        fill_shader_ = LinkProgram(vs, fs);
        if (fill_shader_ == 0) {
            return false;
        }

        printf("[OpenGLRenderer]   - Fill shader compiled (program ID: %u)\n", fill_shader_);
    }

    // ========== 4. COPY SHADER (for VRAM-to-VRAM blits) ==========
    {
        const char* vertex_source = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 2) in vec2 aTexCoord;

out vec2 vTexCoord;

void main() {
    float x = (aPos.x / 1024.0) * 2.0 - 1.0;
    float y = (aPos.y / 512.0) * 2.0 - 1.0;
    gl_Position = vec4(x, -y, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

        const char* fragment_source = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D uSourceTexture;

void main() {
    FragColor = texture(uSourceTexture, vTexCoord);
}
)";

        GLuint vs = CompileShader(GL_VERTEX_SHADER, vertex_source);
        GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragment_source);
        if (vs == 0 || fs == 0) {
            printf("[OpenGLRenderer] ERROR: Failed to compile copy shader\n");
            return false;
        }

        copy_shader_ = LinkProgram(vs, fs);
        if (copy_shader_ == 0) {
            return false;
        }

        printf("[OpenGLRenderer]   - Copy shader compiled (program ID: %u)\n", copy_shader_);

        uloc_copy_source_ = glGetUniformLocation(copy_shader_, "uSourceTexture");
    }

    printf("[OpenGLRenderer] All 4 shaders compiled successfully!\n");
    return true;
}

bool OpenGLRenderer::InitializeBuffers() {
    printf("[OpenGLRenderer] InitializeBuffers() - Creating VAO and VBO...\n");

    // Generate Vertex Array Object and Vertex Buffer Object
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);

    // Bind VAO
    glBindVertexArray(vao_);

    // Bind VBO
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);

    // Allocate buffer (dynamic - will be updated frequently)
    // Reserve space for 4096 vertices (generous batch size)
    vertex_buffer_.reserve(4096);
    glBufferData(GL_ARRAY_BUFFER, sizeof(OpenGLVertex) * 4096, nullptr, GL_DYNAMIC_DRAW);

    // Set up vertex attributes (matches OpenGLVertex structure)
    // Layout location 0: Position (vec2)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(OpenGLVertex),
                          (void*)offsetof(OpenGLVertex, x));

    // Layout location 1: Color (vec4)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(OpenGLVertex),
                          (void*)offsetof(OpenGLVertex, r));

    // Layout location 2: Texture Coordinates (vec2)
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(OpenGLVertex),
                          (void*)offsetof(OpenGLVertex, u));

    // Layout location 3: CLUT Position (vec2)
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(OpenGLVertex),
                          (void*)offsetof(OpenGLVertex, clut_x));

    // Layout location 4: Texpage Position (vec2)
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, sizeof(OpenGLVertex),
                          (void*)offsetof(OpenGLVertex, texpage_x));

    // Layout location 5: Flags (uint)
    glEnableVertexAttribArray(5);
    glVertexAttribIPointer(5, 1, GL_UNSIGNED_INT, sizeof(OpenGLVertex),
                           (void*)offsetof(OpenGLVertex, flags));

    // Unbind VAO
    glBindVertexArray(0);

    printf("[OpenGLRenderer] VAO and VBO created (max 4096 vertices per batch)\n");
    return true;
}

bool OpenGLRenderer::InitializeVRAM() {
    printf("[OpenGLRenderer] InitializeVRAM() - Creating VRAM texture and FBO...\n");

    // Create VRAM texture (1024x512, RGB5A1 format - matches PS1 VRAM)
    glGenTextures(1, &vram_texture_);
    glBindTexture(GL_TEXTURE_2D, vram_texture_);

    // Allocate texture storage (GL_RGB5_A1 = 16-bit per pixel, matches PS1)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB5_A1, 1024, 512, 0,
                 GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, vram_pixels_);

    // Set texture parameters (nearest neighbor, no filtering)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    printf("[OpenGLRenderer] VRAM texture created (1024x512, RGB5A1, %d KB)\n",
           (1024 * 512 * 2) / 1024);

    // Create read-only VRAM texture (sampled by shaders; NOT attached to any FBO).
    // Synced from vram_pixels_ at the start of each FlushPrimitives() to avoid the
    // OpenGL feedback loop that occurs when vram_texture_ is bound as both the FBO
    // attachment and a sampler in the same draw call.
    glGenTextures(1, &vram_read_texture_);
    glBindTexture(GL_TEXTURE_2D, vram_read_texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB5_A1, 1024, 512, 0,
                 GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, vram_pixels_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Create Framebuffer Object (FBO) for rendering to VRAM
    glGenFramebuffers(1, &vram_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, vram_fbo_);

    // Attach VRAM texture to FBO color attachment
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, vram_texture_, 0);

    // Verify FBO is complete
    GLenum fbo_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fbo_status != GL_FRAMEBUFFER_COMPLETE) {
        printf("[OpenGLRenderer] ERROR: VRAM FBO is incomplete! Status: 0x%X\n", fbo_status);

        // Print detailed error
        switch (fbo_status) {
            case GL_FRAMEBUFFER_UNDEFINED:
                printf("  - GL_FRAMEBUFFER_UNDEFINED\n");
                break;
            case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
                printf("  - GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT\n");
                break;
            case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
                printf("  - GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT\n");
                break;
            case GL_FRAMEBUFFER_UNSUPPORTED:
                printf("  - GL_FRAMEBUFFER_UNSUPPORTED\n");
                break;
            default:
                printf("  - Unknown error\n");
                break;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }

    printf("[OpenGLRenderer] VRAM FBO created and verified (complete)\n");

    // Unbind FBO (bind default framebuffer)
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Set up viewport for VRAM rendering (1024x512)
    glViewport(0, 0, 1024, 512);

    return true;
}

//==============================================================================
// Shader Helpers
//==============================================================================

uint32_t OpenGLRenderer::CompileShader(uint32_t type, const char* source) {
    GLuint shader = glCreateShader(type);
    if (shader == 0) {
        printf("[OpenGLRenderer] ERROR: Failed to create shader (type: 0x%X)\n", type);
        return 0;
    }

    // Set shader source and compile
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    // Check compilation status
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLchar info_log[512];
        glGetShaderInfoLog(shader, 512, nullptr, info_log);
        const char* type_name = (type == GL_VERTEX_SHADER) ? "VERTEX" : "FRAGMENT";
        printf("[OpenGLRenderer] ERROR: %s shader compilation failed:\n%s\n", type_name, info_log);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

uint32_t OpenGLRenderer::LinkProgram(uint32_t vertex_shader, uint32_t fragment_shader) {
    GLuint program = glCreateProgram();
    if (program == 0) {
        printf("[OpenGLRenderer] ERROR: Failed to create shader program\n");
        return 0;
    }

    // Attach shaders
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);

    // Link program
    glLinkProgram(program);

    // Check link status
    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        GLchar info_log[512];
        glGetProgramInfoLog(program, 512, nullptr, info_log);
        printf("[OpenGLRenderer] ERROR: Shader program linking failed:\n%s\n", info_log);
        glDeleteProgram(program);
        return 0;
    }

    // Detach and delete shaders (no longer needed after linking)
    glDetachShader(program, vertex_shader);
    glDetachShader(program, fragment_shader);
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    return program;
}

//==============================================================================
// Rendering Helpers
//==============================================================================

void OpenGLRenderer::ApplyBlendMode(uint8_t mode) {
    // PS1 semi-transparency modes:
    // Mode 0: B/2 + F/2 (average blend - 50% background + 50% foreground)
    // Mode 1: B + F (additive blend - background + foreground)
    // Mode 2: B - F (subtractive blend - background - foreground)
    // Mode 3: B + F/4 (additive with quarter foreground)

    // Check if blend mode changed (optimization: avoid redundant OpenGL calls)
    if (cached_state_.blend_mode == mode && cached_state_.blending_enabled == true) {
        return;  // State already applied
    }

    cached_state_.blend_mode = mode;
    cached_state_.blending_enabled = true;

    glEnable(GL_BLEND);

    switch (mode) {
        case 0:  // B/2 + F/2 (Average)
            // OpenGL approximation: SRC_ALPHA=0.5, DST_ALPHA=0.5
            // Use constant blend factors to achieve exact 50/50 mix
            glBlendFunc(GL_CONSTANT_ALPHA, GL_CONSTANT_ALPHA);
            glBlendColor(0.5f, 0.5f, 0.5f, 0.5f);
            glBlendEquation(GL_FUNC_ADD);
            break;

        case 1:  // B + F (Additive)
            glBlendFunc(GL_ONE, GL_ONE);
            glBlendEquation(GL_FUNC_ADD);
            break;

        case 2:  // B - F (Subtractive)
            glBlendFunc(GL_ONE, GL_ONE);
            glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
            break;

        case 3:  // B + F/4 (Additive with quarter brightness)
            // Foreground multiplied by 0.25, background by 1.0
            glBlendColor(0.25f, 0.25f, 0.25f, 0.25f);
            glBlendFunc(GL_CONSTANT_COLOR, GL_ONE);
            glBlendEquation(GL_FUNC_ADD);
            break;

        default:
            // Unknown mode - disable blending
            glDisable(GL_BLEND);
            cached_state_.blending_enabled = false;
            break;
    }
}

void OpenGLRenderer::ApplyDrawingArea() {
    // FBO shaders use y = (VRAM_Y/512)*2-1 (no flip), so gl_FragCoord.y == VRAM_Y.
    // OpenGL scissor uses bottom-left origin with y=gl_FragCoord.y. No flip needed.
    int x = drawing_area_.x1;
    int y = drawing_area_.y1;        // VRAM Y maps directly to gl_FragCoord.y
    int width = drawing_area_.x2 - drawing_area_.x1;
    int height = drawing_area_.y2 - drawing_area_.y1;

    // If the drawing area is zero-size (uninitialized or reset), use full VRAM.
    // The PS1 game may set area only once during setup; zero means "not set".
    if (width <= 0 || height <= 0) {
        x = 0; y = 0; width = 1024; height = 512;
    }

    // Clamp to VRAM bounds (0-1023, 0-511)
    x = std::max(0, std::min(x, 1023));
    y = std::max(0, std::min(y, 511));
    width = std::max(0, std::min(width, 1024 - x));
    height = std::max(0, std::min(height, 512 - y));

    // Check if scissor state changed (optimization: avoid redundant OpenGL calls)
    if (cached_state_.scissor_x == x &&
        cached_state_.scissor_y == y &&
        cached_state_.scissor_w == width &&
        cached_state_.scissor_h == height) {
        return;  // State already applied
    }

    cached_state_.scissor_x = x;
    cached_state_.scissor_y = y;
    cached_state_.scissor_w = width;
    cached_state_.scissor_h = height;

    // Enable scissor test for clipping primitives to drawing area
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, width, height);
    /* [Scissor] — re-enable with LOG_ON_CHANGE to log only when rect changes:
       LOG_ON_CHANGE(((uint64_t)x<<48)|((uint64_t)y<<32)|((uint32_t)width<<16)|height,
                     "Scissor", "(%d,%d,%d,%d)", x, y, width, height); */
}

void OpenGLRenderer::FlushPrimitives() {
    static uint32_t s_flush_total = 0;
    static uint32_t s_flush_empty = 0;
    if (!opengl_initialized_ || vertex_buffer_.empty()) {
        ++s_flush_empty;
        /* [Flush] EMPTY — re-enable: if (s_flush_empty <= 3 || s_flush_empty % 300 == 0)
           printf("[Flush] EMPTY #%u (total calls=%u)\n", s_flush_empty, s_flush_total); */
        if (g_pre_shot_flush)
            printf("[PRE-SHOT-FLUSH] f%u EMPTY vertex buffer\n", g_ps1_frame);
        return;
    }
    ++s_flush_total;
    if (g_pre_shot_flush) {
        printf("[PRE-SHOT-FLUSH] f%u %zu verts  area=(%d,%d)-(%d,%d)  v0=(%.0f,%.0f)\n",
               g_ps1_frame, vertex_buffer_.size(),
               drawing_area_.x1, drawing_area_.y1, drawing_area_.x2, drawing_area_.y2,
               vertex_buffer_[0].x, vertex_buffer_[0].y);
        fflush(stdout);
    }
    /* [Flush] — re-enable: if (s_flush_total <= 5 || s_flush_total % 300 == 0)
       printf("[Flush] #%u: %zu verts first_depth=%d last_depth=%d\n", ...); */

    // Sync vram_read_texture_ from vram_pixels_ only when the CPU has written
    // to VRAM since the last flush.  After initial texture loading this flag
    // stays clear, so mid-frame flushes (texture-depth transitions etc.) incur
    // zero upload overhead.  Using the CPU-side copy avoids the GPU pipeline
    // stall that glCopyTexSubImage2D from the FBO would otherwise introduce.
    if (vram_read_dirty_) {
        glBindTexture(GL_TEXTURE_2D, vram_read_texture_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1024, 512,
                        GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, vram_pixels_);
        glBindTexture(GL_TEXTURE_2D, 0);
        vram_read_dirty_ = false;
        /* DIAG: probe terrain CLUT positions at key frames */
        if (g_ps1_frame == 4248 || g_ps1_frame == 4410) {
            auto probe = [&](int cx, int cy) {
                const uint16_t* p = vram_pixels_ + cy * 1024 + cx;
                printf("[CLUT2] f%u (%d,%d): %04X %04X %04X %04X\n",
                       g_ps1_frame, cx, cy, p[0], p[1], p[2], p[3]);
                fflush(stdout);
            };
            probe(144, 492); probe(160, 481); probe(288, 484);
        }
    }

    // Bind VRAM framebuffer (render to VRAM texture)
    glBindFramebuffer(GL_FRAMEBUFFER, vram_fbo_);

    // Set viewport to VRAM size
    glViewport(0, 0, 1024, 512);

    // Bind VAO (contains vertex attribute configuration)
    glBindVertexArray(vao_);

    // Upload vertex data to VBO
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    vertex_buffer_.size() * sizeof(OpenGLVertex),
                    vertex_buffer_.data());

    // Check if primitives are textured (by looking at first vertex flags)
    bool is_textured = !vertex_buffer_.empty() && (vertex_buffer_[0].flags & (1 << 0));

    // Select shader based on whether primitives are textured
    GLuint shader = is_textured ? textured_shader_ : primitive_shader_;
    glUseProgram(shader);

    // Set up common uniforms using pre-cached locations (no per-frame glGetUniformLocation)
    int loc_dithering  = is_textured ? uloc_tex_dithering_  : uloc_prim_dithering_;
    int loc_check_mask = is_textured ? uloc_tex_check_mask_ : uloc_prim_check_mask_;
    int loc_set_mask   = is_textured ? uloc_tex_set_mask_   : uloc_prim_set_mask_;
    int loc_vram       = is_textured ? uloc_tex_vram_       : uloc_prim_vram_;

    glUniform1i(loc_dithering,  draw_mode_.dithering          ? GL_TRUE : GL_FALSE);
    glUniform1i(loc_check_mask, mask_settings_.check_mask_bit ? GL_TRUE : GL_FALSE);
    glUniform1i(loc_set_mask,   mask_settings_.set_mask_bit   ? GL_TRUE : GL_FALSE);

    // Bind the read-only VRAM texture (no FBO attachment) to texture unit 0.
    // Shaders use this for CLUT/texture lookups.  Using vram_read_texture_ here
    // instead of vram_texture_ prevents the OpenGL feedback loop.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, vram_read_texture_);
    glUniform1i(loc_vram, 0);

    // If textured, set up additional texture uniforms
    if (is_textured) {
        // Pass texture depth (0=4bit, 1=8bit, 2=15bit) — read from vertex flags (bits 3-4),
        // not from draw_mode_.texture_depth (which may have been updated by a later E1h).
        int tex_depth = (vertex_buffer_[0].flags >> 3) & 3;
        glUniform1i(uloc_tex_depth_, tex_depth);

        // Pass texture window settings (mask_x, mask_y, offset_x, offset_y)
        // Convert from PS1 units (×8 pixels) to actual pixels
        float mask_x   = texture_window_.mask_x   * 8.0f;
        float mask_y   = texture_window_.mask_y   * 8.0f;
        float offset_x = texture_window_.offset_x * 8.0f;
        float offset_y = texture_window_.offset_y * 8.0f;

        glUniform4f(uloc_tex_window_, mask_x, mask_y, offset_x, offset_y);
    }

    // Apply semi-transparency blend mode if enabled
    bool is_semi_transparent = !vertex_buffer_.empty() && (vertex_buffer_[0].flags & (1 << 2));
    if (is_semi_transparent) {
        ApplyBlendMode(draw_mode_.semi_transparency);
    } else {
        // Disable blending if not needed (optimization: check cached state)
        if (cached_state_.blending_enabled) {
            glDisable(GL_BLEND);
            cached_state_.blending_enabled = false;
            cached_state_.blend_mode = 0xFF;  // Mark as invalid
        }
    }

    // Apply drawing area clipping (scissor test)
    ApplyDrawingArea();

    // Ensure clean GL state for rendering (paranoia — guard against stale state)
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);

    // Draw primitives using current primitive type (GL_TRIANGLES, GL_LINES, GL_LINE_STRIP)
    glDrawArrays(primitive_type_, 0, static_cast<GLsizei>(vertex_buffer_.size()));

    // Unbind everything
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);

    // Clear vertex buffer for next batch
    vertex_buffer_.clear();

    // Check for OpenGL errors
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        printf("[OpenGLRenderer] OpenGL error during FlushPrimitives: 0x%X\n", error);
    }
}

void OpenGLRenderer::RenderToVRAM() {
    // TODO: Render accumulated primitives to VRAM texture
    // This is called during FlushPrimitives()

    // STUB
}

void OpenGLRenderer::SaveVRAMDump(const char* path) {
    if (!opengl_initialized_) return;
    FlushPrimitives();  // flush any pending draws first

    // Read all 1024×512 pixels from the VRAM FBO as 8-bit RGB
    uint8_t* buf = new uint8_t[1024 * 512 * 3];
    glBindFramebuffer(GL_READ_FRAMEBUFFER, vram_fbo_);
    glReadPixels(0, 0, 1024, 512, GL_RGB, GL_UNSIGNED_BYTE, buf);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    // Write PPM.  OpenGL row 0 = FBO bottom = PS1 y=0 (PS1 top).
    // Write rows in ascending order so PPM row 0 = PS1 y=0 = image top.
    FILE* f = fopen(path, "wb");
    if (!f) { delete[] buf; printf("[SaveVRAMDump] ERROR: cannot open %s\n", path); return; }
    fprintf(f, "P6\n1024 512\n255\n");
    for (int y = 0; y < 512; y++)
        fwrite(&buf[y * 1024 * 3], 3, 1024, f);
    fclose(f);
    delete[] buf;
    printf("[SaveVRAMDump] saved %s  (display_area=%d,%d  draw_area=%d,%d-%d,%d)\n",
           path, display_area_x_, display_area_y_,
           drawing_area_.x1, drawing_area_.y1, drawing_area_.x2, drawing_area_.y2);
}

void OpenGLRenderer::SaveScreenshot(const char* path) {
    if (!opengl_initialized_) return;

    // Read the window's default framebuffer (the blit that Present() just drew)
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    uint8_t* buf = new uint8_t[window_width_ * window_height_ * 3];
    glReadPixels(0, 0, window_width_, window_height_, GL_RGB, GL_UNSIGNED_BYTE, buf);

    // Default framebuffer row 0 = bottom of window → flip rows for PPM (top-first)
    FILE* f = fopen(path, "wb");
    if (!f) { delete[] buf; printf("[SaveScreenshot] ERROR: cannot open %s\n", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", window_width_, window_height_);
    for (int y = window_height_ - 1; y >= 0; y--)
        fwrite(&buf[y * window_width_ * 3], 3, window_width_, f);
    fclose(f);
    delete[] buf;
    printf("[SaveScreenshot] saved %s  (%dx%d)\n", path, window_width_, window_height_);
}

void OpenGLRenderer::SaveScreenshotBMP(const char* path) {
    if (!opengl_initialized_) return;
    // Read the display area directly from the VRAM FBO — always correct regardless
    // of window double-buffer state (GL_BACK is undefined after glfwSwapBuffers).
    // Uses the same display_area_x_/y_ + display_width_/height_ as Present().
    FlushPrimitives();

    int dx = (display_area_x_ == 0 && display_area_y_ == 0) ? 0 : display_area_x_;
    int dy = (display_area_x_ == 0 && display_area_y_ == 0) ? 0 : display_area_y_;
    int dw = (display_width_  > 0) ? display_width_  : 320;
    int dh = (display_height_ > 0) ? display_height_ : 240;
    // Apply V display range crop (same as Present() — progressive only)
    if (v_display_range_y2_ > v_display_range_y1_ && dh <= 240) {
        int vh = v_display_range_y2_ - v_display_range_y1_;
        if (vh > 0 && vh < dh) dh = vh;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, vram_fbo_);
    uint8_t* buf = new uint8_t[dw * dh * 3];
    // VRAM FBO: y=0 at bottom (OpenGL convention), VRAM y maps directly to FBO y.
    // glReadPixels(dx, dy, dw, dh) reads rows dy..dy+dh-1; row 0 of buf = VRAM y=dy
    // = top of PS1 framebuffer. No row flip needed for top-down PNG.
    glReadPixels(dx, dy, dw, dh, GL_RGB, GL_UNSIGNED_BYTE, buf);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    std::string spath(path);
    std::thread([buf, dw, dh, spath]() {
        stbi_write_png(spath.c_str(), dw, dh, 3, buf, dw * 3);
        delete[] buf;
        printf("[AutoShot] saved %s\n", spath.c_str());
    }).detach();
}

void OpenGLRenderer::SaveVRAMDumpBMP(const char* path) {
    if (!opengl_initialized_) return;
    FlushPrimitives();
    uint8_t* buf = new uint8_t[1024 * 512 * 3];
    glBindFramebuffer(GL_READ_FRAMEBUFFER, vram_fbo_);
    glReadPixels(0, 0, 1024, 512, GL_RGB, GL_UNSIGNED_BYTE, buf);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    // Row flip + PNG encode + disk write on background thread.
    std::string spath(path);
    std::thread([buf, spath]() {
        uint8_t* flipped = new uint8_t[1024 * 512 * 3];
        for (int y = 0; y < 512; y++)
            memcpy(flipped + y * 1024 * 3, buf + (511 - y) * 1024 * 3, 1024 * 3);
        delete[] buf;
        stbi_write_png(spath.c_str(), 1024, 512, 3, flipped, 1024 * 3);
        delete[] flipped;
        printf("[AutoVRAM] saved %s\n", spath.c_str());
    }).detach();
}

} // namespace PS1
