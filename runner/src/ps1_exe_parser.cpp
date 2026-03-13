#include "ps1_exe_parser.h"
#include <fstream>
#include <cstring>
#include <fmt/core.h>

namespace PSXRecomp {

// Validate PS1-EXE header
bool PS1ExeParser::validate_header(const PS1ExeHeader& header, std::string& error_msg) {
    // Check magic identifier
    if (std::memcmp(header.magic, "PS-X EXE", 8) != 0) {
        error_msg = fmt::format(
            "Invalid magic identifier. Expected 'PS-X EXE', got '{:.8s}'",
            header.magic
        );
        return false;
    }

    // Check load address is in KSEG0 (cached kernel segment)
    if (header.load_address < 0x80000000 || header.load_address >= 0xA0000000) {
        error_msg = fmt::format(
            "Invalid load address 0x{:08X}. Must be in KSEG0 (0x80000000-0x9FFFFFFF)",
            header.load_address
        );
        return false;
    }

    // Check file size is non-zero and reasonable
    if (header.file_size == 0) {
        error_msg = "Invalid file size: 0 bytes";
        return false;
    }

    if (header.file_size > 2 * 1024 * 1024) {  // > 2MB
        error_msg = fmt::format(
            "Suspicious file size: {} bytes (> 2MB). PS1 only has 2MB RAM",
            header.file_size
        );
        return false;
    }

    // Check load region doesn't overflow RAM
    uint32_t end_address = header.load_address + header.file_size;
    if (end_address > 0x80200000) {  // Beyond 2MB RAM
        error_msg = fmt::format(
            "Load region overflows RAM: 0x{:08X}-0x{:08X} (beyond 0x80200000)",
            header.load_address, end_address
        );
        return false;
    }

    // Check entry point is valid
    if (header.initial_pc < 0x80000000 || header.initial_pc >= 0xA0000000) {
        error_msg = fmt::format(
            "Invalid entry point 0x{:08X}. Must be in KSEG0",
            header.initial_pc
        );
        return false;
    }

    // Warn if entry point is outside loaded range (may be overlay)
    if (!header.entry_in_range()) {
        error_msg = fmt::format(
            "Warning: Entry point 0x{:08X} is outside loaded range [0x{:08X}, 0x{:08X}). "
            "This may indicate overlay-based loading.",
            header.initial_pc, header.load_address, end_address
        );
        // This is a warning, not a hard error (return true)
    }

    return true;
}

// Validate entire executable
bool PS1Executable::validate(std::string& error_msg) const {
    // Check header
    if (!PS1ExeParser::validate_header(header, error_msg)) {
        return false;
    }

    // Check code data size matches header
    if (code_data.size() != header.file_size) {
        error_msg = fmt::format(
            "Code data size mismatch: header specifies {} bytes, got {}",
            header.file_size, code_data.size()
        );
        return false;
    }

    // Check BSS section doesn't overlap code
    if (header.memfill_size > 0) {
        uint32_t bss_start = header.memfill_start;
        uint32_t bss_end = header.bss_end();
        uint32_t code_end = end_address();

        // BSS should come after code
        if (bss_start < code_end) {
            error_msg = fmt::format(
                "BSS section [0x{:08X}, 0x{:08X}) overlaps code [0x{:08X}, 0x{:08X})",
                bss_start, bss_end, load_address(), code_end
            );
            return false;
        }
    }

    return true;
}

// Parse from file
std::optional<PS1Executable> PS1ExeParser::parse_file(
    const std::filesystem::path& path,
    std::string& error_msg
) {
    // Open file
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error_msg = fmt::format("Failed to open file: {}", path.string());
        return std::nullopt;
    }

    // Get file size
    std::streamsize file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    // Check minimum size (header + at least 1 instruction)
    if (file_size < 2048 + 4) {
        error_msg = fmt::format(
            "File too small: {} bytes (minimum 2052 bytes)",
            file_size
        );
        return std::nullopt;
    }

    // Read into buffer
    std::vector<uint8_t> buffer(file_size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), file_size)) {
        error_msg = "Failed to read file";
        return std::nullopt;
    }

    return parse_buffer(buffer, error_msg);
}

// Parse from memory buffer
std::optional<PS1Executable> PS1ExeParser::parse_buffer(
    const std::vector<uint8_t>& buffer,
    std::string& error_msg
) {
    // Check buffer size
    if (buffer.size() < 2048 + 4) {
        error_msg = fmt::format(
            "Buffer too small: {} bytes (minimum 2052 bytes)",
            buffer.size()
        );
        return std::nullopt;
    }

    PS1Executable exe;

    // Copy header (first 2048 bytes)
    std::memcpy(&exe.header, buffer.data(), sizeof(PS1ExeHeader));

    // Validate header
    if (!validate_header(exe.header, error_msg)) {
        return std::nullopt;
    }

    // Check file size matches buffer
    size_t expected_size = 2048 + exe.header.file_size;
    if (buffer.size() != expected_size) {
        error_msg = fmt::format(
            "File size mismatch: header specifies {} bytes total (2048 + {}), got {}",
            expected_size, exe.header.file_size, buffer.size()
        );
        return std::nullopt;
    }

    // Copy code data (after header)
    exe.code_data.resize(exe.header.file_size);
    std::memcpy(
        exe.code_data.data(),
        buffer.data() + 2048,
        exe.header.file_size
    );

    // Final validation
    if (!exe.validate(error_msg)) {
        return std::nullopt;
    }

    return exe;
}

} // namespace PSXRecomp
