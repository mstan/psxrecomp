#pragma once

#include "gpu_state.h"
#include "gpu_renderer.h"
#include <cstdint>
#include <vector>

/**
 * PS1 GPU Command Interpreter
 *
 * Receives GP0/GP1 commands, parses them, updates GPU state,
 * and dispatches rendering commands to the backend.
 */

namespace PS1 {

class GPUInterpreter {
public:
    GPUInterpreter(GPURenderer* renderer);
    ~GPUInterpreter();

    // Public API (called by recompiled game code)
    void WriteGP0(uint32_t value);
    void WriteGP1(uint32_t value);
    uint32_t ReadGPUREAD();
    uint32_t ReadGPUSTAT();

    // Frame timing (called by main loop)
    void VSync();

    // Reset GPU state
    void Reset();

    // Abort any pending CPUToVRAM streaming transfer.
    // Called after DrawOTag OT-chain walk: pixel data for CPUToVRAM commands
    // embedded in an OT chain arrives via a SEPARATE DMA2 block-mode transfer,
    // NOT from subsequent OT entries, so streaming must not bleed across entries.
    void AbortStreaming();


private:
    GPUState state;
    GPURenderer* renderer;

    // Staging buffer for CPU→VRAM streaming transfers
    std::vector<uint16_t> cpu_vram_staging_;

    // ===== FIFO Management =====
    void PushFIFO(uint32_t value);
    uint32_t PopFIFO();
    bool FIFOFull() const;
    bool FIFOEmpty() const;
    int FIFOCount() const;

    // ===== GP0 Command Processing =====
    void ProcessGP0FIFO();
    void ExecuteGP0Command();
    int GetGP0ParameterCount(uint32_t command) const;
    void StreamCPUToVRAMWord(uint16_t pixel0, uint16_t pixel1, bool second_valid);

    // GP0 Command Handlers
    void HandleGP0Miscellaneous(uint32_t cmd);
    void HandleGP0Polygon(const uint32_t* params);
    void HandleGP0Line(const uint32_t* params);
    void HandleGP0Rectangle(const uint32_t* params);
    void HandleGP0VRAMToVRAM(const uint32_t* params);
    void HandleGP0CPUToVRAM(const uint32_t* params);
    void HandleGP0VRAMToCPU(const uint32_t* params);
    void HandleGP0Environment(uint32_t cmd);

    // Specific environment commands
    void HandleDrawMode(uint32_t cmd);           // GP0(E1h)
    void HandleTextureWindow(uint32_t cmd);      // GP0(E2h)
    void HandleDrawingAreaTopLeft(uint32_t cmd); // GP0(E3h)
    void HandleDrawingAreaBottomRight(uint32_t cmd); // GP0(E4h)
    void HandleDrawingOffset(uint32_t cmd);      // GP0(E5h)
    void HandleMaskBitSetting(uint32_t cmd);     // GP0(E6h)

    // ===== GP1 Command Processing =====
    void HandleGP1Command(uint32_t cmd);

    // GP1 Command Handlers
    void HandleGP1ResetGPU();                    // GP1(00h)
    void HandleGP1ResetCommandBuffer();          // GP1(01h)
    void HandleGP1AckInterrupt();                // GP1(02h)
    void HandleGP1DisplayEnable(uint32_t param); // GP1(03h)
    void HandleGP1DMADirection(uint32_t param);  // GP1(04h)
    void HandleGP1DisplayAreaStart(uint32_t param); // GP1(05h)
    void HandleGP1HorizontalDisplayRange(uint32_t param); // GP1(06h)
    void HandleGP1VerticalDisplayRange(uint32_t param);   // GP1(07h)
    void HandleGP1DisplayMode(uint32_t param);   // GP1(08h)
    void HandleGP1GetGPUInfo(uint32_t param);    // GP1(10h)

    // ===== Helper Functions =====

    // Extract coordinate components
    static int16_t ExtractX(uint32_t coord);
    static int16_t ExtractY(uint32_t coord);

    // Extract color components
    static void ExtractRGB(uint32_t word, uint8_t& r, uint8_t& g, uint8_t& b);

    // Extract texture coordinates
    static void ExtractUV(uint32_t word, uint8_t& u, uint8_t& v);

    // Extract CLUT position
    static void ExtractCLUT(uint32_t word, uint16_t& x, uint16_t& y);

    // Extract texture page info
    static void ExtractTexpage(uint32_t word, uint8_t& x, uint8_t& y, uint8_t& depth);

    // Build DrawState from current GPU state
    DrawState BuildDrawState(bool gouraud, bool textured, bool semi_transparent, bool raw_texture) const;

    // Apply drawing offset to coordinates
    void ApplyDrawingOffset(int16_t& x, int16_t& y) const;
};

} // namespace PS1
