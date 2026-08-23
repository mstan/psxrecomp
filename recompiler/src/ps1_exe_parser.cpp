#include "ps1_exe_parser.h"
#include <fstream>
#include <cstring>
#include <fmt/core.h>

namespace PSXRecomp {

bool apply_static_analysis_bound(PS1Executable& exe,
                                 uint32_t configured_size,
                                 std::string& error_msg) {
    const uint32_t image_size = exe.header.file_size;
    const uint64_t configured_end =
        static_cast<uint64_t>(exe.header.load_address) + configured_size;

    if (configured_size == 0) {
        error_msg = "game.text_size must be nonzero";
        return false;
    }
    if ((configured_size & 3u) != 0) {
        error_msg = fmt::format(
            "game.text_size 0x{:X} is not instruction-aligned", configured_size);
        return false;
    }
    /* 8 MiB, not 2 MiB: the 8 MB hardware mod maps unique DRAM across the
     * full window and overlay shards may live in the high banks (WipEout 3
     * ntscfull8 trampolines at 0x80780000+). */
    if (configured_end > 0x80800000ull) {
        error_msg = fmt::format(
            "game.text_size 0x{:X} extends past PS1 RAM", configured_size);
        return false;
    }

    const uint32_t effective_size =
        configured_size < image_size ? configured_size : image_size;
    const uint64_t effective_end =
        static_cast<uint64_t>(exe.header.load_address) + effective_size;
    if (exe.header.initial_pc < exe.header.load_address ||
        static_cast<uint64_t>(exe.header.initial_pc) >= effective_end) {
        error_msg = fmt::format(
            "game.text_size 0x{:X} excludes entry_pc 0x{:08X}",
            configured_size, exe.header.initial_pc);
        return false;
    }

    if (configured_size > image_size) {
        // Existing config generation reserves the remainder of the final 4 KiB
        // page for the runtime overlay floor. That value is not evidence that
        // bytes beyond the EXE payload exist, so it must not widen analysis.
        const uint64_t rounded =
            (static_cast<uint64_t>(image_size) + 0xFFFull) & ~0xFFFull;
        if (configured_size != rounded) {
            error_msg = fmt::format(
                "game.text_size 0x{:X} exceeds PS-X EXE size 0x{:X} "
                "and is not its canonical page-rounded reservation",
                configured_size, image_size);
            return false;
        }
        return true;
    }

    if (configured_size == image_size) return true;

    if ((configured_size & 0xFFFu) != 0) {
        error_msg = fmt::format(
            "truncated game.text_size 0x{:X} must be 4 KiB page-aligned",
            configured_size);
        return false;
    }
    if (exe.code_data.size() != image_size) {
        error_msg = fmt::format(
            "cannot apply analysis bound: EXE payload is {} bytes, header is {}",
            exe.code_data.size(), image_size);
        return false;
    }

    exe.header.file_size = configured_size;
    exe.code_data.resize(configured_size);
    return true;
}

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

    if (header.file_size > 8 * 1024 * 1024) {  // > 8MB (8 MB-mod capacity)
        error_msg = fmt::format(
            "Suspicious file size: {} bytes (> 8MB). PSX host RAM capacity is 8MB",
            header.file_size
        );
        return false;
    }

    // Check load region doesn't overflow RAM
    uint32_t end_address = header.load_address + header.file_size;
    if (end_address > 0x80800000) {  // Beyond 8MB host RAM capacity
        error_msg = fmt::format(
            "Load region overflows RAM: 0x{:08X}-0x{:08X} (beyond 0x80800000)",
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

    // Some EXEs (e.g. Kula World SCES-01000) store KUSEG addresses
    // (0x00011000) instead of KSEG0 (0x80011000). KUSEG 0x0-0x1FFFFFFF
    // mirrors the same physical RAM, so normalize to KSEG0 before
    // validation. Only remap addresses that fall inside the 2 MB RAM
    // mirror; 0 stays 0 (unused fields like initial_gp).
    auto to_kseg0 = [](uint32_t& addr) {
        if (addr != 0 && addr < 0x00200000) addr |= 0x80000000;
    };
    to_kseg0(exe.header.initial_pc);
    to_kseg0(exe.header.initial_gp);
    to_kseg0(exe.header.load_address);
    to_kseg0(exe.header.memfill_start);
    to_kseg0(exe.header.initial_sp);
    to_kseg0(exe.header.initial_fp);
    to_kseg0(exe.header.stack_base);

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
