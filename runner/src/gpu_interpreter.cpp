#include "gpu_interpreter.h"
#include <cstdio>
#include <cstring>

extern "C" uint32_t g_ps1_frame;

namespace PS1 {

GPUInterpreter::GPUInterpreter(GPURenderer* renderer)
    : renderer(renderer) {
    state.Reset();
    if (renderer) {
        renderer->Initialize();
    }
}

GPUInterpreter::~GPUInterpreter() {
    if (renderer) {
        renderer->Shutdown();
    }
}

// ===== Public API =====

void GPUInterpreter::WriteGP0(uint32_t value) {
    // Push command word to FIFO
    PushFIFO(value);

    // Process FIFO immediately (simplified, could batch)
    ProcessGP0FIFO();
}

void GPUInterpreter::WriteGP1(uint32_t value) {
    // GP1 commands bypass FIFO and execute immediately
    HandleGP1Command(value);
}

uint32_t GPUInterpreter::ReadGPUREAD() {
    // Return next word from GPUREAD buffer (VRAM→CPU transfer)
    if (state.gpuread_count > 0) {
        uint32_t value = state.gpuread_buffer[state.gpuread_pos];
        state.gpuread_pos++;
        state.gpuread_count--;
        state.UpdateGPUSTAT();
        return value;
    }
    return 0;  // No data available
}

uint32_t GPUInterpreter::ReadGPUSTAT() {
    state.UpdateGPUSTAT();
    return state.gpustat;
}

void GPUInterpreter::VSync() {
    // Called at frame boundary (60Hz NTSC, 50Hz PAL)
    if (renderer) {
        renderer->VSync();
    }
}

void GPUInterpreter::Reset() {
    state.Reset();
}

void GPUInterpreter::AbortStreaming() {
    if (state.vram_transfer.active && state.vram_transfer.is_cpu_to_vram) {
        state.vram_transfer.active = false;
        state.vram_transfer.remaining_pixels = 0;
        cpu_vram_staging_.clear();
        /* Also reset command accumulation state.  After the 3-word header was
         * consumed, params_received==params_needed==3.  Without this reset the
         * next gpu_submit_word call would fire ExecuteGP0Command() for the
         * aborted CPUToVRAM, then call HandleGP0CPUToVRAM with stale params. */
        state.current_command = 0;
        state.params_received = 0;
        state.params_needed   = 0;
    }
}

// ===== FIFO Management =====

void GPUInterpreter::PushFIFO(uint32_t value) {
    if (FIFOFull()) {
        fprintf(stderr, "Warning: GP0 FIFO overflow, command dropped\n");
        return;
    }

    state.fifo[state.fifo_write_pos] = value;
    state.fifo_write_pos = (state.fifo_write_pos + 1) % 64;
    state.fifo_count++;
}

uint32_t GPUInterpreter::PopFIFO() {
    if (FIFOEmpty()) {
        return 0;
    }

    uint32_t value = state.fifo[state.fifo_read_pos];
    state.fifo_read_pos = (state.fifo_read_pos + 1) % 64;
    state.fifo_count--;
    return value;
}

bool GPUInterpreter::FIFOFull() const {
    return state.fifo_count >= 64;
}

bool GPUInterpreter::FIFOEmpty() const {
    return state.fifo_count == 0;
}

int GPUInterpreter::FIFOCount() const {
    return state.fifo_count;
}

// ===== GP0 Command Processing =====

/* CPU→VRAM streaming: accumulate pixels in staging buffer.
 * When all pixels for the transfer are collected, uploads the entire region
 * to VRAM in one call.  Called for each packed 32-bit word (2 pixels). */
void GPUInterpreter::StreamCPUToVRAMWord(uint16_t pixel0, uint16_t pixel1, bool second_valid) {
    VRAMTransfer& t = state.vram_transfer;
    if (!t.active || !t.is_cpu_to_vram) return;

    cpu_vram_staging_.push_back(pixel0);
    t.remaining_pixels--;

    if (second_valid && t.remaining_pixels > 0) {
        cpu_vram_staging_.push_back(pixel1);
        t.remaining_pixels--;
    }

    /* When all pixels received, upload the entire region at once */
    if (t.remaining_pixels == 0) {
        if (renderer && !cpu_vram_staging_.empty()) {
            renderer->UploadToVRAM(t.x, t.y, t.width, t.height,
                                   cpu_vram_staging_.data());
        }
        cpu_vram_staging_.clear();
    }
}

void GPUInterpreter::ProcessGP0FIFO() {
    while (!FIFOEmpty()) {
        /* === STREAMING MODE: drain CPU→VRAM data words directly to VRAM === */
        if (state.vram_transfer.active && state.vram_transfer.is_cpu_to_vram) {
            VRAMTransfer& t = state.vram_transfer;
            while (!FIFOEmpty() && t.remaining_pixels > 0) {
                uint32_t word = PopFIFO();
                uint16_t lo = word & 0xFFFFu;
                uint16_t hi = (word >> 16) & 0xFFFFu;
                bool two_pixels = (t.remaining_pixels >= 2);
                StreamCPUToVRAMWord(lo, hi, two_pixels);
                /* remaining_pixels decremented inside StreamCPUToVRAMWord */
            }
            if (t.remaining_pixels == 0) {
                static uint32_t s_upload_done = 0;
                ++s_upload_done;
                /* [CPUToVRAM] done — re-enable: if (s_upload_done <= 20) printf(...); */
                t.active = false;
                state.current_command = 0;
                state.params_received = 0;
                state.params_needed   = 0;
            }
            /* Either FIFO drained (break), or transfer done (loop again) */
            if (t.active) break;  /* still pending, need more words */
            continue;
        }

        /* === NORMAL MODE: accumulate header words then execute === */
        if (state.current_command == 0) {
            state.current_command = PopFIFO();
            state.command_params[0] = state.current_command;
            state.params_received = 1;
            state.params_needed = GetGP0ParameterCount(state.current_command);

            /* CPU→VRAM: only read the 3-word header here */
            if ((state.current_command >> 29) == 0x5) {
                state.params_needed = 3;
            }
        }

        /* Accumulate parameters */
        while (state.params_received < state.params_needed && !FIFOEmpty()) {
            uint32_t word = PopFIFO();
            if (state.params_received < 16) {
                state.command_params[state.params_received] = word;
            }
            state.params_received++;

            /* CPU→VRAM: after the 3-word header is complete, initialise streaming */
            if ((state.current_command >> 29) == 0x5 && state.params_received == 3) {
                uint32_t xy = state.command_params[1];
                uint32_t wh = state.command_params[2];
                uint16_t x = xy & 0x3FFu;
                uint16_t y = (xy >> 16) & 0x1FFu;
                uint16_t w = wh & 0xFFFFu;
                uint16_t h = (wh >> 16) & 0xFFFFu;
                if (w == 0 || h == 0) {
                    /* Degenerate: nothing to transfer */
                    state.current_command = 0;
                    state.params_received = 0;
                    state.params_needed   = 0;
                    break;
                }
                if (w > 1024) w = 1024;
                if (h > 512)  h = 512;
                static uint32_t s_upload_start = 0;
                ++s_upload_start;
                /* [CPUToVRAM] start — re-enable: if (s_upload_start <= 20) printf(...); */
                VRAMTransfer& t = state.vram_transfer;
                t.active            = true;
                t.is_cpu_to_vram    = true;
                t.x                 = x;
                t.y                 = y;
                t.width             = w;
                t.height            = h;
                t.remaining_pixels  = (uint32_t)w * h;
                /* Pre-allocate staging buffer to avoid repeated realloc */
                cpu_vram_staging_.clear();
                cpu_vram_staging_.reserve((uint32_t)w * h);
                /* No more header words to read — break out to streaming mode */
                state.params_needed = 3;  /* mark header done */
                break;
            }
        }

        /* If we just set up a streaming transfer, loop back to drain FIFO */
        if (state.vram_transfer.active && state.vram_transfer.is_cpu_to_vram) {
            continue;
        }

        /* Execute command when all parameters received */
        if (state.params_received >= state.params_needed) {
            ExecuteGP0Command();
            state.current_command = 0;
            state.params_received = 0;
            state.params_needed   = 0;
        } else {
            break;
        }
    }
}

void GPUInterpreter::ExecuteGP0Command() {
    uint32_t cmd = state.command_params[0];
    uint8_t opcode = (cmd >> 24) & 0xFF;
    uint8_t cmd_type = (cmd >> 29) & 0x7;  // Upper 3 bits

    switch (cmd_type) {
        case 0x0:  // Miscellaneous (0x00-0x1F)
            HandleGP0Miscellaneous(cmd);
            break;

        case 0x1:  // Polygon (0x20-0x3F)
            HandleGP0Polygon(state.command_params);
            break;

        case 0x2:  // Line (0x40-0x5F)
            HandleGP0Line(state.command_params);
            break;

        case 0x3:  // Rectangle (0x60-0x7F)
            HandleGP0Rectangle(state.command_params);
            break;

        case 0x4:  // VRAM-to-VRAM (0x80-0x9F)
            HandleGP0VRAMToVRAM(state.command_params);
            break;

        case 0x5:  // CPU-to-VRAM (0xA0-0xBF)
            HandleGP0CPUToVRAM(state.command_params);
            break;

        case 0x6:  // VRAM-to-CPU (0xC0-0xDF)
            HandleGP0VRAMToCPU(state.command_params);
            break;

        case 0x7:  // Environment (0xE0-0xFF)
            HandleGP0Environment(cmd);
            break;

        default:
            fprintf(stderr, "Warning: Unknown GP0 command 0x%02X\n", opcode);
            break;
    }
}

int GPUInterpreter::GetGP0ParameterCount(uint32_t command) const {
    uint8_t opcode = (command >> 24) & 0xFF;
    uint8_t cmd_type = (command >> 29) & 0x7;

    // Determine parameter count based on command type
    switch (cmd_type) {
        case 0x0:  // Miscellaneous
            if (opcode == 0x00) return 1;  // NOP
            if (opcode == 0x01) return 1;  // Clear cache
            if (opcode == 0x02) return 3;  // Fill rectangle
            if (opcode == 0x1F) return 1;  // Interrupt request
            return 1;  // Unknown, consume 1 word

        case 0x1: {  // Polygon
            bool quad     = (command >> 27) & 1;
            bool textured = (command >> 26) & 1;
            bool gouraud  = (command >> 28) & 1;
            int verts = quad ? 4 : 3;
            int tex   = textured ? 1 : 0;
            /* PS1 flat polygon layout:
             *   word 0         = cmd + shared color
             *   per vertex     = XY + (UV if textured)
             *   total          = 1 + verts*(1+tex)
             * PS1 gouraud polygon layout:
             *   word 0         = cmd + V0_color
             *   per vertex     = [color (V1+)] + XY + (UV if textured)
             *   total          = verts*(2+tex)    [V0 color is packed into word 0]
             */
            if (gouraud) {
                return verts * (2 + tex);
            } else {
                return 1 + verts * (1 + tex);
            }
        }

        case 0x2: {  // Line
            bool polyline = (command >> 27) & 1;
            bool gouraud = (command >> 28) & 1;
            if (polyline) {
                return -1;  // Variable length (terminated by 0x55555555)
            } else {
                return gouraud ? 4 : 3;  // Single line
            }
        }

        case 0x3: {  // Rectangle
            bool textured = (command >> 26) & 1;
            bool variable_size = ((command >> 27) & 1) == 0;
            return 2 + (textured ? 1 : 0) + (variable_size ? 1 : 0);
        }

        case 0x4:  // VRAM-to-VRAM blit
            return 4;

        case 0x5:  // CPU-to-VRAM (variable length, handled specially)
            return 3;  // Will be updated after reading WH

        case 0x6:  // VRAM-to-CPU
            return 3;

        case 0x7:  // Environment
            return 1;  // All environment commands are 1 word

        default:
            return 1;
    }
}

// ===== GP0 Command Handlers (Stubs for now) =====

void GPUInterpreter::HandleGP0Miscellaneous(uint32_t cmd) {
    uint8_t opcode = (cmd >> 24) & 0xFF;

    switch (opcode) {
        case 0x00:  // NOP
            // Do nothing
            break;

        case 0x01:  // Clear texture cache
            if (renderer) {
                renderer->ClearTextureCache();
            }
            break;

        case 0x02: {  // Fill rectangle in VRAM
            uint32_t color_word = state.command_params[0];
            uint32_t xy = state.command_params[1];
            uint32_t wh = state.command_params[2];

            /* GP0(0x02) color: bits[7:0]=R, bits[15:8]=G, bits[23:16]=B (24-bit RGB).
             * Convert to GL_UNSIGNED_SHORT_1_5_5_5_REV (PS1 native):
             *   R5 in bits[4:0], G5 in bits[9:5], B5 in bits[14:10], A=0 in bit[15]. */
            uint8_t r8 = (color_word >>  0) & 0xFF;
            uint8_t g8 = (color_word >>  8) & 0xFF;
            uint8_t b8 = (color_word >> 16) & 0xFF;
            uint16_t color = (uint16_t)((r8 >> 3) | ((g8 >> 3) << 5) | ((b8 >> 3) << 10));
            int16_t x = xy & 0x3FF;
            int16_t y = (xy >> 16) & 0x1FF;
            uint16_t w = wh & 0xFFFF;
            uint16_t h = (wh >> 16) & 0xFFFF;

            if (renderer) {
                renderer->FillRectangle(x, y, w, h, color);
            }
            break;
        }

        case 0x1F:  // Interrupt request
            // Set IRQ1 bit in GPUSTAT (bit 24)
            state.gpustat |= (1 << 24);
            break;

        default:
            fprintf(stderr, "Warning: Unknown GP0 misc command 0x%02X\n", opcode);
            break;
    }
}

void GPUInterpreter::HandleGP0Polygon(const uint32_t* params) {
    uint32_t cmd = params[0];
    uint8_t opcode = (cmd >> 24) & 0xFF;

    // Extract polygon attributes from command bits
    bool quad = (cmd >> 27) & 1;         // Bit 27: 0=Triangle, 1=Quad
    bool textured = (cmd >> 26) & 1;     // Bit 26: Textured
    bool semi_transparent = (cmd >> 25) & 1;  // Bit 25: Semi-transparency
    bool raw_texture = (cmd >> 24) & 1;  // Bit 24: Raw texture (skip modulation)
    bool gouraud = (cmd >> 28) & 1;      // Bit 28: Gouraud shading

    int num_verts = quad ? 4 : 3;

    // Parse vertices
    Vertex verts[4];
    int param_idx = 0;

    for (int i = 0; i < num_verts; i++) {
        // Extract color (first vertex or all vertices for Gouraud)
        if (i == 0 || gouraud) {
            uint32_t color_word = params[param_idx++];
            ExtractRGB(color_word, verts[i].r, verts[i].g, verts[i].b);
            // raw_texture: vertex color ignored by PS1 HW; use neutral 0x80
            if (raw_texture && textured) {
                verts[i].r = 0x80; verts[i].g = 0x80; verts[i].b = 0x80;
            }
        } else {
            // Flat shading: reuse first vertex color
            verts[i].r = verts[0].r;
            verts[i].g = verts[0].g;
            verts[i].b = verts[0].b;
        }

        // Extract vertex coordinates
        uint32_t coord_word = params[param_idx++];
        verts[i].x = ExtractX(coord_word);
        verts[i].y = ExtractY(coord_word);

        // Apply drawing offset
        ApplyDrawingOffset(verts[i].x, verts[i].y);

        // Extract texture coordinates if textured
        if (textured) {
            uint32_t tex_word = params[param_idx++];
            ExtractUV(tex_word, verts[i].u, verts[i].v);

            verts[i].has_texture = true;

            // First vertex: CLUT info
            if (i == 0) {
                ExtractCLUT(tex_word, verts[i].clut_x, verts[i].clut_y);
                verts[i].has_clut = true;
                verts[i].has_texpage = false;
            }
            // Second vertex: Texpage info
            else if (i == 1) {
                ExtractTexpage(tex_word, verts[i].texpage_x, verts[i].texpage_y, verts[i].texpage_depth);
                verts[i].has_texpage = true;
                verts[i].has_clut = false;
            }
            // Other vertices: just UVs
            else {
                verts[i].has_clut = false;
                verts[i].has_texpage = false;
            }
        } else {
            verts[i].has_texture = false;
            verts[i].has_clut = false;
            verts[i].has_texpage = false;
        }
    }

    // Build draw state
    DrawState draw_state = BuildDrawState(gouraud, textured, semi_transparent, raw_texture);

    // Log semi-transparent non-textured quads — catch the screen-darkening subtractive blend.
    // Only log the first 20 unique occurrences to avoid spam.
    /* [SEMI-QUAD] — re-enable when debugging semi-transparent rendering:
    if (semi_transparent && !textured && quad) {
        static uint32_t s_sub_count = 0;
        if (++s_sub_count <= 20) {
            printf("[SEMI-QUAD] f%u #%u abr=%d rgb=(%d,%d,%d) "
                   "verts:(%d,%d)(%d,%d)(%d,%d)(%d,%d)\n",
                   g_ps1_frame, s_sub_count, state.draw_mode.semi_transparency,
                   verts[0].r, verts[0].g, verts[0].b,
                   verts[0].x, verts[0].y, verts[1].x, verts[1].y,
                   verts[2].x, verts[2].y, verts[3].x, verts[3].y);
            fflush(stdout);
        }
    } */

    // Render polygon(s)
    if (renderer) {
        if (quad) {
            // Render quad as two triangles (1-2-3, 2-3-4).
            Vertex tri1[3] = {verts[0], verts[1], verts[2]};
            Vertex tri2[3] = {verts[1], verts[2], verts[3]};

            // PS1 quad format: CLUT lives in verts[0], texpage in verts[1].
            // tri2 starts with verts[1] (no CLUT) and verts[2] (no texpage).
            // Propagate so the renderer gets correct CLUT and texpage for tri2.
            tri2[0].has_clut    = verts[0].has_clut;
            tri2[0].clut_x      = verts[0].clut_x;
            tri2[0].clut_y      = verts[0].clut_y;
            tri2[1].has_texpage   = verts[1].has_texpage;
            tri2[1].texpage_x     = verts[1].texpage_x;
            tri2[1].texpage_y     = verts[1].texpage_y;
            tri2[1].texpage_depth = verts[1].texpage_depth;

            renderer->DrawTriangle(tri1, draw_state);
            renderer->DrawTriangle(tri2, draw_state);
        } else {
            // Render single triangle
            renderer->DrawTriangle(verts, draw_state);
        }
    }
}

void GPUInterpreter::HandleGP0Line(const uint32_t* params) {
    uint32_t cmd = params[0];
    uint8_t opcode = (cmd >> 24) & 0xFF;

    // Extract line attributes from command bits
    bool polyline = (cmd >> 27) & 1;         // Bit 27: 0=Single line, 1=Polyline
    bool semi_transparent = (cmd >> 25) & 1; // Bit 25: Semi-transparency
    bool gouraud = (cmd >> 28) & 1;          // Bit 28: Gouraud shading

    if (polyline) {
        // Polyline: Variable number of vertices, terminated by 0x55555555
        Vertex* vertices = new Vertex[256];  // Max polyline length
        int vert_count = 0;
        int param_idx = 0;

        // Parse vertices until terminator
        while (param_idx < state.params_received && vert_count < 256) {
            // Check for terminator
            if (param_idx > 0 &&
                (params[param_idx] == 0x55555555 || params[param_idx] == 0x50005000)) {
                break;
            }

            // Extract color (first vertex or all vertices for Gouraud)
            if (vert_count == 0 || gouraud) {
                uint32_t color_word = params[param_idx++];
                ExtractRGB(color_word, vertices[vert_count].r,
                          vertices[vert_count].g, vertices[vert_count].b);
            } else {
                // Flat shading: reuse first vertex color
                vertices[vert_count].r = vertices[0].r;
                vertices[vert_count].g = vertices[0].g;
                vertices[vert_count].b = vertices[0].b;
            }

            // Extract vertex coordinates
            uint32_t coord_word = params[param_idx++];
            vertices[vert_count].x = ExtractX(coord_word);
            vertices[vert_count].y = ExtractY(coord_word);

            // Apply drawing offset
            ApplyDrawingOffset(vertices[vert_count].x, vertices[vert_count].y);

            vertices[vert_count].has_texture = false;
            vertices[vert_count].has_clut = false;
            vertices[vert_count].has_texpage = false;

            vert_count++;
        }

        // Build draw state
        DrawState draw_state = BuildDrawState(gouraud, false, semi_transparent, false);

        // Render polyline
        if (renderer && vert_count >= 2) {
            renderer->DrawPolyline(vertices, vert_count, draw_state);
        }

        delete[] vertices;

    } else {
        // Single line: 2 vertices
        Vertex verts[2];
        int param_idx = 0;

        for (int i = 0; i < 2; i++) {
            // Extract color (first vertex or both for Gouraud)
            if (i == 0 || gouraud) {
                uint32_t color_word = params[param_idx++];
                ExtractRGB(color_word, verts[i].r, verts[i].g, verts[i].b);
            } else {
                // Flat shading: reuse first vertex color
                verts[i].r = verts[0].r;
                verts[i].g = verts[0].g;
                verts[i].b = verts[0].b;
            }

            // Extract vertex coordinates
            uint32_t coord_word = params[param_idx++];
            verts[i].x = ExtractX(coord_word);
            verts[i].y = ExtractY(coord_word);

            // Apply drawing offset
            ApplyDrawingOffset(verts[i].x, verts[i].y);

            verts[i].has_texture = false;
            verts[i].has_clut = false;
            verts[i].has_texpage = false;
        }

        // Build draw state
        DrawState draw_state = BuildDrawState(gouraud, false, semi_transparent, false);

        // Render single line
        if (renderer) {
            renderer->DrawLine(verts, draw_state);
        }
    }
}

void GPUInterpreter::HandleGP0Rectangle(const uint32_t* params) {
    uint32_t cmd = params[0];
    uint8_t opcode = (cmd >> 24) & 0xFF;

    // Extract rectangle attributes from command bits
    bool textured = (cmd >> 26) & 1;     // Bit 26: Textured
    bool semi_transparent = (cmd >> 25) & 1;  // Bit 25: Semi-transparency
    bool raw_texture = (cmd >> 24) & 1;  // Bit 24: Raw texture

    // Determine rectangle size from bits 27+29
    bool variable_size = ((cmd >> 27) & 1) == 0;
    uint8_t size_bits = ((cmd >> 27) & 3);  // Bits 27-28 for fixed sizes

    int param_idx = 0;

    // Extract color
    uint32_t color_word = params[param_idx++];
    uint8_t r, g, b;
    ExtractRGB(color_word, r, g, b);
    // For raw_texture mode vertex color bytes are ignored by the PS1 hardware.
    // Use neutral 0x80 so that texel * (0x80/255) * 2 ≈ texel (no modulation).
    if (raw_texture && textured) { r = 0x80; g = 0x80; b = 0x80; }

    // Extract top-left position
    uint32_t coord_word = params[param_idx++];
    int16_t x = ExtractX(coord_word);
    int16_t y = ExtractY(coord_word);

    // Apply drawing offset
    ApplyDrawingOffset(x, y);

    // Extract texture coordinates if textured
    uint8_t u = 0, v = 0;
    uint16_t clut_x = 0, clut_y = 0;
    if (textured) {
        uint32_t tex_word = params[param_idx++];
        ExtractUV(tex_word, u, v);
        ExtractCLUT(tex_word, clut_x, clut_y);
    }

    // Extract width and height
    int16_t width = 0, height = 0;

    if (variable_size) {
        // Variable size: read width/height from parameters
        uint32_t wh = params[param_idx++];
        width = wh & 0x3FF;         // Bits 0-9
        height = (wh >> 16) & 0x1FF; // Bits 16-24
    } else {
        // Fixed size: decode from command bits
        if (size_bits == 0) {
            // 1×1 dot
            width = 1;
            height = 1;
        } else if (size_bits == 1) {
            // 8×8 sprite
            width = 8;
            height = 8;
        } else if (size_bits == 2) {
            // 16×16 sprite
            width = 16;
            height = 16;
        } else {
            // Invalid - default to 16×16
            width = 16;
            height = 16;
        }
    }

    // Build draw state
    DrawState draw_state = BuildDrawState(false, textured, semi_transparent, raw_texture);

    // For textured rectangles, copy texture info from texpage state
    // (This matches PS1 hardware behavior where texpage is set via GP0(E1h))

    // Log semi-transparent non-textured rectangles — first 20 only.
    if (semi_transparent && !textured) {
        static uint32_t s_semi_rect_cnt = 0; ++s_semi_rect_cnt;
        /* [SEMI-RECT] first 20 — re-enable: if (s_semi_rect_cnt <= 20) printf(...); */
    }

    // Render rectangle — pass UV origin and CLUT (texpage comes via draw_state)
    if (renderer) {
        renderer->DrawRectangle(x, y, width, height, u, v, clut_x, clut_y, r, g, b, draw_state);
    }
}

void GPUInterpreter::HandleGP0VRAMToVRAM(const uint32_t* params) {
    // GP0(80h) - Copy Rectangle (VRAM to VRAM)
    // Word 1: Command
    // Word 2: Source X,Y
    // Word 3: Destination X,Y
    // Word 4: Width, Height

    uint32_t src_xy = params[1];
    uint32_t dst_xy = params[2];
    uint32_t wh = params[3];

    int16_t src_x = src_xy & 0x3FF;
    int16_t src_y = (src_xy >> 16) & 0x1FF;
    int16_t dst_x = dst_xy & 0x3FF;
    int16_t dst_y = (dst_xy >> 16) & 0x1FF;
    uint16_t width = ((wh & 0x3FF) + 0xF) & ~0xF;  // Round up to multiple of 16
    uint16_t height = (wh >> 16) & 0x1FF;

    // Call renderer to perform the copy
    if (renderer) {
        renderer->CopyVRAM(src_x, src_y, dst_x, dst_y, width, height);
    }
}

void GPUInterpreter::HandleGP0CPUToVRAM(const uint32_t* params) {
    // GP0(A0h) - Copy Rectangle (CPU to VRAM)
    // Word 1: Command
    // Word 2: Destination X,Y
    // Word 3: Width, Height
    // Word 4+: Data (halfwords packed into words)

    uint32_t dst_xy = params[1];
    uint32_t wh = params[2];

    int16_t dst_x = dst_xy & 0x3FF;
    int16_t dst_y = (dst_xy >> 16) & 0x1FF;
    uint16_t width = wh & 0xFFFF;
    uint16_t height = (wh >> 16) & 0xFFFF;

    // Guard against zero or overflow dimensions
    if (width == 0 || height == 0) return;
    uint32_t pixel_count = (uint32_t)width * (uint32_t)height;
    if (pixel_count > 1024u * 512u) {
        fprintf(stderr, "Warning: CPUToVRAM dimensions %dx%d overflow, skipping\n", width, height);
        return;
    }

    // Calculate number of data words
    int data_words = ((int)pixel_count + 1) / 2;  // Halfwords → words (round up)

    // Extract halfword data from parameter words
    uint16_t* data = new uint16_t[pixel_count];
    int halfword_idx = 0;

    for (int i = 0; i < data_words; i++) {
        uint32_t word = params[3 + i];
        data[halfword_idx++] = word & 0xFFFF;         // Low halfword
        if ((uint32_t)halfword_idx < pixel_count) {
            data[halfword_idx++] = (word >> 16) & 0xFFFF;  // High halfword
        }
    }

    static uint32_t s_cpu2vram = 0; ++s_cpu2vram;
    /* [GP0:A0] CPUToVRAM first 10 — re-enable: if (s_cpu2vram <= 10) printf(...); */

    // Upload to VRAM via renderer
    if (renderer) {
        renderer->UploadToVRAM(dst_x, dst_y, width, height, data);
    }

    delete[] data;
}

void GPUInterpreter::HandleGP0VRAMToCPU(const uint32_t* params) {
    // GP0(C0h) - Copy Rectangle (VRAM to CPU)
    // Word 1: Command
    // Word 2: Source X,Y
    // Word 3: Width, Height

    uint32_t src_xy = params[1];
    uint32_t wh = params[2];

    int16_t src_x = src_xy & 0x3FF;
    int16_t src_y = (src_xy >> 16) & 0x1FF;
    uint16_t width = wh & 0xFFFF;
    uint16_t height = (wh >> 16) & 0xFFFF;

    // Guard against zero or overflow dimensions
    if (width == 0 || height == 0) return;
    uint32_t pixel_count = (uint32_t)width * (uint32_t)height;
    if (pixel_count > 1024u * 512u) {
        fprintf(stderr, "Warning: VRAMToCPU dimensions %dx%d overflow, skipping\n", width, height);
        return;
    }

    // Download from VRAM via renderer
    uint16_t* data = new uint16_t[pixel_count];

    if (renderer) {
        renderer->DownloadFromVRAM(src_x, src_y, width, height, data);
    }

    // Pack halfwords into GPUREAD buffer
    int data_words = ((width * height) + 1) / 2;  // Halfwords → words (round up)
    state.gpuread_count = data_words;
    state.gpuread_pos = 0;
    int halfword_idx = 0;
    for (int i = 0; i < data_words; i++) {
        uint32_t word = 0;
        word |= data[halfword_idx++];  // Low halfword
        if ((uint32_t)halfword_idx < pixel_count) {
            word |= (uint32_t)data[halfword_idx++] << 16;  // High halfword
        }
        state.gpuread_buffer[i] = word;
    }

    delete[] data;

    // Update GPUSTAT to indicate data is ready
    state.UpdateGPUSTAT();
}

void GPUInterpreter::HandleGP0Environment(uint32_t cmd) {
    uint8_t opcode = (cmd >> 24) & 0xFF;

    switch (opcode) {
        case 0xE1: HandleDrawMode(cmd); break;
        case 0xE2: HandleTextureWindow(cmd); break;
        case 0xE3: HandleDrawingAreaTopLeft(cmd); break;
        case 0xE4: HandleDrawingAreaBottomRight(cmd); break;
        case 0xE5: HandleDrawingOffset(cmd); break;
        case 0xE6: HandleMaskBitSetting(cmd); break;
        case 0xE7: case 0xE8: break;  /* Undocumented NOPs */
        default:
            fprintf(stderr, "Warning: Unknown GP0 environment command 0x%02X\n", opcode);
            break;
    }
}

// ===== Environment Command Handlers =====

void GPUInterpreter::HandleDrawMode(uint32_t cmd) {
    // GP0(E1h) - Draw Mode Setting
    state.draw_mode.texpage_x_base = cmd & 0xF;
    state.draw_mode.texpage_y_base = (cmd >> 4) & 1;
    state.draw_mode.semi_transparency = (cmd >> 5) & 3;
    state.draw_mode.texture_depth = (cmd >> 7) & 3;
    state.draw_mode.dithering = (cmd >> 9) & 1;
    state.draw_mode.draw_to_display = (cmd >> 10) & 1;
    state.draw_mode.texpage_y_base_bit1 = (cmd >> 11) & 1;
    state.draw_mode.texture_disable = (cmd >> 12) & 1;
    state.draw_mode.h_flip = (cmd >> 13) & 1;

    // Update renderer state
    if (renderer) {
        renderer->SetDrawMode(state.draw_mode);
    }
}

void GPUInterpreter::HandleTextureWindow(uint32_t cmd) {
    // GP0(E2h) - Texture Window Setting
    state.texture_window.mask_x = cmd & 0x1F;
    state.texture_window.mask_y = (cmd >> 5) & 0x1F;
    state.texture_window.offset_x = (cmd >> 10) & 0x1F;
    state.texture_window.offset_y = (cmd >> 15) & 0x1F;

    if (renderer) {
        renderer->SetTextureWindow(state.texture_window);
    }
}

void GPUInterpreter::HandleDrawingAreaTopLeft(uint32_t cmd) {
    // GP0(E3h) - Set Drawing Area top-left
    state.drawing_area.x1 = cmd & 0x3FF;
    state.drawing_area.y1 = (cmd >> 10) & 0x3FF;

    if (renderer) {
        renderer->SetDrawingArea(state.drawing_area);
    }
}

void GPUInterpreter::HandleDrawingAreaBottomRight(uint32_t cmd) {
    // GP0(E4h) - Set Drawing Area bottom-right
    state.drawing_area.x2 = cmd & 0x3FF;
    state.drawing_area.y2 = (cmd >> 10) & 0x3FF;

    static uint32_t s_da_count = 0; ++s_da_count;
    /* [GP0:E3/E4] first 20 — re-enable: if (s_da_count <= 20) printf("[GP0:E3/E4] DrawArea TL...", ...); */

    if (renderer) {
        renderer->SetDrawingArea(state.drawing_area);
    }
}

void GPUInterpreter::HandleDrawingOffset(uint32_t cmd) {
    // GP0(E5h) - Set Drawing Offset
    // Sign-extend 11-bit values to 16-bit
    int16_t x = (cmd & 0x7FF);
    int16_t y = ((cmd >> 11) & 0x7FF);
    if (x & 0x400) x |= 0xF800;  // Sign extend bit 10
    if (y & 0x400) y |= 0xF800;  // Sign extend bit 10

    state.drawing_offset.x = x;
    state.drawing_offset.y = y;

    if (renderer) {
        renderer->SetDrawingOffset(state.drawing_offset);
    }
}

void GPUInterpreter::HandleMaskBitSetting(uint32_t cmd) {
    // GP0(E6h) - Mask Bit Setting
    state.mask_settings.set_mask_bit = cmd & 1;
    state.mask_settings.check_mask_bit = (cmd >> 1) & 1;

    if (renderer) {
        renderer->SetMaskSettings(state.mask_settings);
    }
}

// ===== GP1 Command Processing =====

void GPUInterpreter::HandleGP1Command(uint32_t cmd) {
    uint8_t opcode = (cmd >> 24) & 0xFF;
    uint32_t param = cmd & 0xFFFFFF;

    switch (opcode) {
        case 0x00: HandleGP1ResetGPU(); break;
        case 0x01: HandleGP1ResetCommandBuffer(); break;
        case 0x02: HandleGP1AckInterrupt(); break;
        case 0x03: HandleGP1DisplayEnable(param); break;
        case 0x04: HandleGP1DMADirection(param); break;
        case 0x05: HandleGP1DisplayAreaStart(param); break;
        case 0x06: HandleGP1HorizontalDisplayRange(param); break;
        case 0x07: HandleGP1VerticalDisplayRange(param); break;
        case 0x08: HandleGP1DisplayMode(param); break;
        case 0x10: HandleGP1GetGPUInfo(param); break;
        default:
            fprintf(stderr, "Warning: Unknown GP1 command 0x%02X\n", opcode);
            break;
    }
}

// ===== GP1 Command Handlers =====

void GPUInterpreter::HandleGP1ResetGPU() {
    state.Reset();
    if (renderer) {
        // Reset renderer state
        renderer->SetDrawMode(state.draw_mode);
        renderer->SetTextureWindow(state.texture_window);
        renderer->SetDrawingArea(state.drawing_area);
        renderer->SetDrawingOffset(state.drawing_offset);
        renderer->SetMaskSettings(state.mask_settings);
    }
}

void GPUInterpreter::HandleGP1ResetCommandBuffer() {
    // Clear GP0 FIFO only
    state.fifo_write_pos = 0;
    state.fifo_read_pos = 0;
    state.fifo_count = 0;
    state.current_command = 0;
    state.params_received = 0;
    state.params_needed = 0;
}

void GPUInterpreter::HandleGP1AckInterrupt() {
    // Clear IRQ1 bit in GPUSTAT (bit 24)
    state.gpustat &= ~(1 << 24);
}

void GPUInterpreter::HandleGP1DisplayEnable(uint32_t param) {
    state.display_control.display_enable = (param & 1) == 0;  // 0=on, 1=off
}

void GPUInterpreter::HandleGP1DMADirection(uint32_t param) {
    state.display_control.dma_direction = param & 3;
}

void GPUInterpreter::HandleGP1DisplayAreaStart(uint32_t param) {
    state.display_control.display_area_x = param & 0x3FF;
    state.display_control.display_area_y = (param >> 10) & 0x1FF;

    static uint32_t s_gp1_05_count = 0;
    /* [GP1:05] first 20 + every 300 + frame window — re-enable when investigating display area:
       if (++s_gp1_05_count <= 20 || s_gp1_05_count % 300 == 0 || (g_ps1_frame >= 1670 && g_ps1_frame <= 1690))
           printf("[GP1:05] #%u f%u display_area=(%d,%d)\n", s_gp1_05_count, g_ps1_frame, ...); */
    ++s_gp1_05_count;

    if (renderer) {
        renderer->SetDisplayArea(state.display_control.display_area_x,
                                  state.display_control.display_area_y);
    }
}

void GPUInterpreter::HandleGP1HorizontalDisplayRange(uint32_t param) {
    state.display_control.h_display_range_x1 = param & 0xFFF;
    state.display_control.h_display_range_x2 = (param >> 12) & 0xFFF;

    if (renderer) {
        renderer->SetHorizontalDisplayRange(state.display_control.h_display_range_x1,
                                             state.display_control.h_display_range_x2);
    }
}

void GPUInterpreter::HandleGP1VerticalDisplayRange(uint32_t param) {
    state.display_control.v_display_range_y1 = param & 0x3FF;
    state.display_control.v_display_range_y2 = (param >> 10) & 0x3FF;

    if (renderer) {
        renderer->SetVerticalDisplayRange(state.display_control.v_display_range_y1,
                                           state.display_control.v_display_range_y2);
    }
}

void GPUInterpreter::HandleGP1DisplayMode(uint32_t param) {
    state.display_control.h_resolution = param & 3;
    state.display_control.v_resolution = (param >> 2) & 1;
    state.display_control.video_mode = (param >> 3) & 1;
    state.display_control.color_depth_24bit = (param >> 4) & 1;
    state.display_control.interlace = (param >> 5) & 1;
    state.display_control.reverse_flag = (param >> 7) & 1;

    // Calculate display dimensions
    const int h_res_table[] = {256, 320, 512, 640};
    int width = h_res_table[state.display_control.h_resolution];
    int height = state.display_control.v_resolution ? 480 : 240;

    if (renderer) {
        renderer->SetDisplayMode(width, height,
                                  state.display_control.color_depth_24bit,
                                  state.display_control.interlace);
    }
}

void GPUInterpreter::HandleGP1GetGPUInfo(uint32_t param) {
    // Return GPU info via GPUREAD
    uint32_t info = 0;

    switch (param & 0xF) {
        case 2:  // Texture window
            info = (state.texture_window.offset_y << 15) |
                   (state.texture_window.offset_x << 10) |
                   (state.texture_window.mask_y << 5) |
                   (state.texture_window.mask_x);
            break;

        case 3:  // Draw area top-left
            info = (state.drawing_area.y1 << 10) | state.drawing_area.x1;
            break;

        case 4:  // Draw area bottom-right
            info = (state.drawing_area.y2 << 10) | state.drawing_area.x2;
            break;

        case 5:  // Draw offset
            info = ((state.drawing_offset.y & 0x7FF) << 11) |
                   (state.drawing_offset.x & 0x7FF);
            break;

        case 7:  // GPU type/version
            info = 2;  // GPU type 2 (with 2MB VRAM support, though we use 1MB)
            break;

        case 8:  // GPU version
            info = 0;  // Old GPU (v0)
            break;

        default:
            info = 0;
            break;
    }

    // Place in GPUREAD buffer
    state.gpuread_buffer[0] = info;
    state.gpuread_count = 1;
    state.gpuread_pos = 0;
}

// ===== Helper Functions =====

int16_t GPUInterpreter::ExtractX(uint32_t coord) {
    // Extract X coordinate (bits 0-10, sign-extended)
    int16_t x = coord & 0x7FF;
    if (x & 0x400) x |= 0xF800;  // Sign extend bit 10
    return x;
}

int16_t GPUInterpreter::ExtractY(uint32_t coord) {
    // Extract Y coordinate (bits 16-26, sign-extended)
    int16_t y = (coord >> 16) & 0x7FF;
    if (y & 0x400) y |= 0xF800;  // Sign extend bit 10
    return y;
}

void GPUInterpreter::ExtractRGB(uint32_t word, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = word & 0xFF;
    g = (word >> 8) & 0xFF;
    b = (word >> 16) & 0xFF;
}

void GPUInterpreter::ExtractUV(uint32_t word, uint8_t& u, uint8_t& v) {
    u = word & 0xFF;
    v = (word >> 8) & 0xFF;
}

void GPUInterpreter::ExtractCLUT(uint32_t word, uint16_t& x, uint16_t& y) {
    x = ((word >> 16) & 0x3F) * 16;  // CLUT X in 16-halfword units
    y = (word >> 22) & 0x1FF;        // CLUT Y (scanline)
}

void GPUInterpreter::ExtractTexpage(uint32_t word, uint8_t& x, uint8_t& y, uint8_t& depth) {
    x = (word >> 16) & 0xF;          // Texpage X base (0-15)
    y = ((word >> 20) & 1) | (((word >> 27) & 1) << 1);  // Texpage Y base (bits 20 and 27)
    depth = (word >> 23) & 3;        // Texture depth (0=4bit, 1=8bit, 2=15bit)
}

DrawState GPUInterpreter::BuildDrawState(bool gouraud, bool textured, bool semi_transparent, bool raw_texture) const {
    DrawState ds;
    ds.gouraud = gouraud;
    ds.textured = textured;
    ds.semi_transparent = semi_transparent;
    ds.raw_texture = raw_texture;
    ds.blend_mode = state.draw_mode.semi_transparency;
    ds.texpage_x_base = state.draw_mode.texpage_x_base;
    ds.texpage_y_base = (state.draw_mode.texpage_y_base) | (state.draw_mode.texpage_y_base_bit1 << 1);
    ds.texture_depth = state.draw_mode.texture_depth;
    ds.dithering = state.draw_mode.dithering;
    ds.check_mask_bit = state.mask_settings.check_mask_bit;
    ds.set_mask_bit = state.mask_settings.set_mask_bit;
    ds.tex_window_mask_x = state.texture_window.mask_x;
    ds.tex_window_mask_y = state.texture_window.mask_y;
    ds.tex_window_offset_x = state.texture_window.offset_x;
    ds.tex_window_offset_y = state.texture_window.offset_y;
    ds.draw_area_x1 = state.drawing_area.x1;
    ds.draw_area_y1 = state.drawing_area.y1;
    ds.draw_area_x2 = state.drawing_area.x2;
    ds.draw_area_y2 = state.drawing_area.y2;
    return ds;
}

void GPUInterpreter::ApplyDrawingOffset(int16_t& x, int16_t& y) const {
    x += state.drawing_offset.x;
    y += state.drawing_offset.y;
}

} // namespace PS1
