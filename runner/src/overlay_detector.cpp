#include "overlay_detector.h"
#include "ps1_exe_parser.h"
#include <algorithm>
#include <cctype>

namespace PS1 {

// Valid PS1 RAM range for overlay load addresses
static const uint32_t PS1_RAM_START = 0x80010000;
static const uint32_t PS1_RAM_END   = 0x801FFFFF;

bool OverlayDetector::IsValidMIPSInstruction(uint32_t instr) {
    // Reject obvious non-instructions
    if (instr == 0x00000000) return false;  // NOP (filler)
    if (instr == 0xFFFFFFFF) return false;  // Padding

    uint32_t opcode = (instr >> 26) & 0x3F;

    // Valid MIPS R3000 opcodes
    static const bool valid_opcodes[] = {
        true,  // 0x00 SPECIAL
        true,  // 0x01 REGIMM
        true,  // 0x02 J
        true,  // 0x03 JAL
        true,  // 0x04 BEQ
        true,  // 0x05 BNE
        true,  // 0x06 BLEZ
        true,  // 0x07 BGTZ
        true,  // 0x08 ADDI
        true,  // 0x09 ADDIU
        true,  // 0x0A SLTI
        true,  // 0x0B SLTIU
        true,  // 0x0C ANDI
        true,  // 0x0D ORI
        true,  // 0x0E XORI
        true,  // 0x0F LUI
        true,  // 0x10 COP0
        false, // 0x11 COP1 (unused on PS1)
        true,  // 0x12 COP2 (GTE)
        false, // 0x13 COP3 (unused)
        false, // 0x14 reserved
        false, // 0x15 reserved
        false, // 0x16 reserved
        false, // 0x17 reserved
        false, // 0x18 reserved
        false, // 0x19 reserved
        false, // 0x1A reserved
        false, // 0x1B reserved
        false, // 0x1C reserved
        false, // 0x1D reserved
        false, // 0x1E reserved
        false, // 0x1F reserved
        true,  // 0x20 LB
        true,  // 0x21 LH
        true,  // 0x22 LWL
        true,  // 0x23 LW
        true,  // 0x24 LBU
        true,  // 0x25 LHU
        true,  // 0x26 LWR
        false, // 0x27 reserved
        true,  // 0x28 SB
        true,  // 0x29 SH
        true,  // 0x2A SWL
        true,  // 0x2B SW
        false, // 0x2C reserved
        false, // 0x2D reserved
        true,  // 0x2E SWR
        false, // 0x2F reserved
        false, // 0x30 reserved (LWC0)
        false, // 0x31 reserved (LWC1)
        true,  // 0x32 LWC2 (GTE)
        false, // 0x33 reserved
        false, // 0x34 reserved
        false, // 0x35 reserved
        false, // 0x36 reserved
        false, // 0x37 reserved
        false, // 0x38 reserved (SWC0)
        false, // 0x39 reserved
        true,  // 0x3A SWC2 (GTE)
        false, // 0x3B reserved
        false, // 0x3C reserved
        false, // 0x3D reserved
        false, // 0x3E reserved
        false, // 0x3F reserved
    };

    return valid_opcodes[opcode];
}

std::vector<std::pair<std::string, size_t>> OverlayDetector::ExtractFilenames(
    const std::vector<uint8_t>& data)
{
    std::vector<std::pair<std::string, size_t>> results;

    size_t i = 0;
    while (i < data.size()) {
        // Look for start of a potential ISO 9660 filename
        // Valid chars: A-Z, 0-9, underscore, dot, semicolon
        if (!std::isupper(data[i]) && !std::isdigit(data[i])) {
            i++;
            continue;
        }

        // Try to extract a filename starting at i
        size_t start = i;
        std::string candidate;
        bool has_dot = false;
        bool has_extension = false;

        while (i < data.size() && candidate.size() < 32) {
            char c = (char)data[i];
            if (std::isupper(c) || std::isdigit(c) || c == '_') {
                candidate += c;
                i++;
            } else if (c == '.' && !has_dot) {
                has_dot = true;
                candidate += c;
                i++;
            } else if (c == ';' && has_dot) {
                // ISO 9660 version separator
                candidate += c;
                i++;
                // Read version number (usually "1")
                while (i < data.size() && std::isdigit((char)data[i]) && candidate.size() < 34) {
                    candidate += (char)data[i];
                    i++;
                }
                break;
            } else {
                break;
            }
        }

        // Validate: must have extension with 2-3 uppercase letters
        if (has_dot && candidate.size() >= 5) {
            size_t dot_pos = candidate.find('.');
            if (dot_pos != std::string::npos) {
                std::string ext = candidate.substr(dot_pos + 1);
                // Remove version if present
                size_t semi_pos = ext.find(';');
                if (semi_pos != std::string::npos) {
                    ext = ext.substr(0, semi_pos);
                }

                // Extension should be 2-3 uppercase letters
                if (ext.size() >= 2 && ext.size() <= 3) {
                    bool all_upper = true;
                    for (char c : ext) {
                        if (!std::isupper(c)) { all_upper = false; break; }
                    }
                    if (all_upper) {
                        has_extension = true;
                    }
                }
            }
        }

        if (has_extension) {
            results.push_back({candidate, start});
        }
    }

    return results;
}

uint32_t OverlayDetector::FindLoadAddress(
    const std::vector<uint8_t>& data,
    size_t filename_offset)
{
    // Search in a window after the filename (up to 256 bytes)
    size_t search_start = filename_offset;
    size_t search_end = std::min(filename_offset + 256, data.size());

    // Align to 4-byte boundary
    search_start = (search_start + 3) & ~3;

    for (size_t i = search_start; i + 7 < search_end; i += 4) {
        // Read potential LUI instruction (little-endian)
        uint32_t instr = (uint32_t)data[i] |
                        ((uint32_t)data[i+1] << 8) |
                        ((uint32_t)data[i+2] << 16) |
                        ((uint32_t)data[i+3] << 24);

        // Check for LUI pattern: opcode = 0x0F (bits 31-26)
        uint32_t opcode = (instr >> 26) & 0x3F;
        if (opcode != 0x0F) continue;  // Not LUI

        uint16_t upper16 = (uint16_t)(instr & 0xFFFF);

        // Upper 16 bits must be in PS1 RAM range: 0x8000-0x8001
        if (upper16 < 0x8000 || upper16 > 0x801F) continue;

        uint32_t addr_high = (uint32_t)upper16 << 16;

        // Look for ADDIU or ORI in next instruction for lower 16 bits
        if (i + 8 < search_end) {
            uint32_t next_instr = (uint32_t)data[i+4] |
                                 ((uint32_t)data[i+5] << 8) |
                                 ((uint32_t)data[i+6] << 16) |
                                 ((uint32_t)data[i+7] << 24);

            uint32_t next_op = (next_instr >> 26) & 0x3F;
            if (next_op == 0x09 || next_op == 0x0D) {  // ADDIU or ORI
                uint16_t lower16 = (uint16_t)(next_instr & 0xFFFF);
                uint32_t full_addr = addr_high | lower16;

                if (full_addr >= PS1_RAM_START && full_addr <= PS1_RAM_END) {
                    return full_addr;
                }
            }
        }

        // Also check just the upper portion - some overlays load at aligned addresses
        if (addr_high >= PS1_RAM_START && addr_high <= PS1_RAM_END) {
            return addr_high;
        }
    }

    return 0;  // Not found
}

std::vector<DetectedOverlay> OverlayDetector::DetectOverlays(
    const PSXRecomp::PS1Executable& exe)
{
    std::vector<DetectedOverlay> results;

    const auto& code = exe.code_data;
    if (code.empty()) return results;

    // Extract candidate filenames from the code section
    auto candidates = ExtractFilenames(code);

    for (const auto& [filename, offset] : candidates) {
        // Skip common non-overlay files
        if (filename.find("SYSTEM.CNF") != std::string::npos) continue;
        if (filename.find("SCUS_") != std::string::npos) continue;
        if (filename.find("SLUS_") != std::string::npos) continue;
        if (filename.find("SCES_") != std::string::npos) continue;
        if (filename.find("SCPS_") != std::string::npos) continue;

        // Only look for .BIN, .OVL, .DAT, .STR files (typical overlay formats)
        bool is_overlay_type = false;
        std::string upper = filename;
        for (char& c : upper) c = std::toupper(c);

        if (upper.find(".BIN") != std::string::npos ||
            upper.find(".OVL") != std::string::npos ||
            upper.find(".DAT") != std::string::npos ||
            upper.find(".PRG") != std::string::npos) {
            is_overlay_type = true;
        }

        if (!is_overlay_type) continue;

        // Try to find a load address near this filename
        uint32_t load_addr = FindLoadAddress(code, offset);

        // Also search BEFORE the filename (function might set up address first)
        if (load_addr == 0 && offset >= 256) {
            load_addr = FindLoadAddress(code, offset - 256);
        }

        // Create entry if we found a valid load address
        if (load_addr != 0) {
            // Check for duplicates
            bool duplicate = false;
            for (const auto& existing : results) {
                if (existing.filename == filename) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                results.push_back({filename, load_addr, 0});
            }
        } else {
            // Include with address=0 if no address found but filename looks like overlay
            // (user can filter these out)
            bool duplicate = false;
            for (const auto& existing : results) {
                if (existing.filename == filename) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                results.push_back({filename, 0, 0});
            }
        }
    }

    return results;
}

} // namespace PS1
