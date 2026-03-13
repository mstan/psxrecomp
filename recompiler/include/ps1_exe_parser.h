#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <optional>
#include <filesystem>

namespace PSXRecomp {

// PS-X EXE header structure (2048 bytes)
#pragma pack(push, 1)
struct PS1ExeHeader {
    // Offset 0x00-0x0F: Magic + padding
    char magic[8];           // "PS-X EXE"
    uint32_t pad0;
    uint32_t pad1;

    // Offset 0x10-0x1F: Core addresses
    uint32_t initial_pc;     // Entry point
    uint32_t initial_gp;     // Global pointer
    uint32_t load_address;   // RAM load address
    uint32_t file_size;      // Executable size (bytes)

    // Offset 0x20-0x2F: Memfill (BSS section)
    uint32_t unknown0;
    uint32_t unknown1;
    uint32_t memfill_start;
    uint32_t memfill_size;

    // Offset 0x30-0x3F: Stack setup
    uint32_t initial_sp;     // Stack pointer
    uint32_t initial_fp;     // Frame pointer
    uint32_t stack_base;
    uint32_t stack_offset;

    // Offset 0x40-0x7FF: Reserved (2048 - 64 bytes already used = 1984 bytes)
    uint8_t reserved[1984];

    // Helper methods
    uint32_t end_address() const {
        return load_address + file_size;
    }

    bool entry_in_range() const {
        return initial_pc >= load_address &&
               initial_pc < end_address();
    }

    uint32_t bss_end() const {
        return memfill_start + memfill_size;
    }
};
#pragma pack(pop)

static_assert(sizeof(PS1ExeHeader) == 2048, "PS1ExeHeader must be exactly 2048 bytes");

// Parsed PS1 executable with code data
class PS1Executable {
public:
    PS1ExeHeader header;
    std::vector<uint8_t> code_data;  // Raw binary (file_size bytes)

    // Computed properties
    uint32_t load_address() const { return header.load_address; }
    uint32_t entry_point() const { return header.initial_pc; }
    uint32_t code_size() const { return header.file_size; }
    uint32_t end_address() const { return header.end_address(); }

    // Access code as 32-bit words (for MIPS disassembly)
    std::optional<uint32_t> read_word(uint32_t address) const {
        if (address < load_address() || address >= end_address()) {
            return std::nullopt;  // Out of range
        }
        uint32_t offset = address - load_address();
        if (offset + 3 >= code_data.size()) {
            return std::nullopt;  // Partial word
        }
        // Little-endian read
        return (uint32_t)code_data[offset] |
               ((uint32_t)code_data[offset+1] << 8) |
               ((uint32_t)code_data[offset+2] << 16) |
               ((uint32_t)code_data[offset+3] << 24);
    }

    // Validate executable is well-formed
    bool validate(std::string& error_msg) const;
};

// Parser class
class PS1ExeParser {
public:
    // Parse PS1-EXE from file
    static std::optional<PS1Executable> parse_file(
        const std::filesystem::path& path,
        std::string& error_msg
    );

    // Parse PS1-EXE from memory buffer
    static std::optional<PS1Executable> parse_buffer(
        const std::vector<uint8_t>& buffer,
        std::string& error_msg
    );

    // Validate header (public for PS1Executable::validate)
    static bool validate_header(const PS1ExeHeader& header, std::string& error_msg);
};

} // namespace PSXRecomp
