#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Forward declaration
namespace PSXRecomp {
    class PS1Executable;
}

namespace PS1 {

/**
 * Represents a detected overlay in a PS1 executable
 */
struct DetectedOverlay {
    std::string filename;      // ISO filename (e.g., "OVER0.BIN")
    uint32_t load_address;     // PS1 RAM address where overlay loads (e.g., 0x80100000)
    uint32_t size;             // Estimated size (0 if unknown)
};

/**
 * Detects overlay loading patterns in PS1 executables by analyzing MIPS code
 */
class OverlayDetector {
public:
    OverlayDetector() = default;

    /**
     * Scans a PS1 executable for overlay loading patterns
     * Returns list of detected overlays with filenames and load addresses
     */
    std::vector<DetectedOverlay> DetectOverlays(const PSXRecomp::PS1Executable& exe);

private:
    /**
     * Scan raw bytes for sequences that look like ISO 9660 filenames
     * e.g. "OVER0.BIN;1", "STAGE1.BIN;1"
     */
    std::vector<std::pair<std::string, size_t>> ExtractFilenames(
        const std::vector<uint8_t>& data);

    /**
     * Look for LUI + ADDIU/ORI load address pattern near a filename in code
     * Returns reconstructed 32-bit load address, or 0 if not found
     */
    uint32_t FindLoadAddress(
        const std::vector<uint8_t>& data,
        size_t filename_offset);

    /**
     * Check if a 32-bit value is a valid MIPS instruction opcode
     */
    bool IsValidMIPSInstruction(uint32_t instr);
};

} // namespace PS1
