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

// --------------------------------------------------------------------------
// Analysis-bound tag: how many TRAILING bytes of the payload are guard words
// --------------------------------------------------------------------------
//
// Overlay capture DELIBERATELY appends a coherent guard instruction past the
// end of a dirty-page run (runtime/src/overlay_capture.c, write_json_window:
// `size += 4u`, pinned by runtime/tests/test_interpreter_perf_guards.py). A
// MIPS branch sitting at the run's final word (...FFC) always executes its
// delay slot in the NEXT page (...000); supplying that one word lets overlay
// codegen emit exact semantics and lets the range manifest hash/watch it.
//
// So the guard word is a legal DELAY-SLOT SOURCE but an ILLEGAL BLOCK LEADER:
// the word AFTER it does not exist in the image, so a control transfer AT the
// guard word could never have its mandatory delay slot emitted. The
// recompiler must therefore carry two distinct bounds — a READ bound (the
// whole payload, so a delay slot at the guard word resolves) and an ANALYSIS
// bound (short of the guard words, so discovery and block construction can
// never lead there).
//
// The producer that wrapped the capture states the count explicitly, in a
// magic-tagged field in the otherwise-zero tail of the 2048-byte PS-X EXE
// header (tools/compile_overlays.py, make_psxexe). The recompiler is TOLD
// rather than inferring the count from the payload size: an inference from
// `size % 4096 == 4` fits every capture written by the current format but
// would silently shift underneath any future change to capture granularity,
// and pre-2026-07-25 captures are page-exact with no guard word at all.
//
// An absent or malformed tag means ZERO guard bytes, which is exactly what
// every genuine PS-X EXE (the BIOS, a retail main executable) is: those images
// have no guard word, their analysis bound equals their read bound, and their
// analysis is bit-for-bit unchanged by this mechanism.
namespace exe_tag {
// Offsets are into the 2048-byte header. Retail headers use only
// [0x00, 0x4C) plus an ASCII region string ending well before 0x100; the tail
// is zero-filled. The 8-byte magic makes a false positive on a real EXE
// effectively impossible even if some image did carry bytes there.
inline constexpr uint32_t kGuardMagicOffset = 0x7E0u;
inline constexpr uint32_t kGuardCountOffset = 0x7E8u;  // uint32, little-endian
inline constexpr char     kGuardMagic[8] =
    {'P', 'S', 'X', 'R', 'G', 'R', 'D', '1'};
// A guard run exists to cover branch delay slots at a page edge; one word is
// what the capture format appends. Cap the accepted value at a page so a
// corrupt tag can never shrink analysis to nothing.
inline constexpr uint32_t kMaxGuardBytes = 4096u;
}  // namespace exe_tag

// Decode the analysis-bound tag. Returns 0 when the tag is absent, when the
// magic does not match, or when the declared count is not a sane, strictly
// interior, instruction-aligned trailer of `file_size` bytes of payload.
uint32_t decode_analysis_guard_bytes(const PS1ExeHeader& header,
                                     uint32_t file_size);

// Parsed PS1 executable with code data
class PS1Executable {
public:
    PS1ExeHeader header;
    std::vector<uint8_t> code_data;  // Raw binary (file_size bytes)

    // Trailing bytes of code_data that exist ONLY to supply architectural
    // delay slots to instructions inside the analysis bound. Read-legal,
    // analysis-illegal. See the exe_tag comment above. Zero for every image
    // that is not an overlay capture wrapped by compile_overlays.
    uint32_t analysis_guard_bytes = 0;

    // Computed properties
    uint32_t load_address() const { return header.load_address; }
    uint32_t entry_point() const { return header.initial_pc; }
    uint32_t code_size() const { return header.file_size; }

    // READ bound: one past the last byte the image physically supplies. Used
    // by read_word so a mandatory delay slot living at a guard word resolves.
    uint32_t end_address() const { return header.end_address(); }

    // ANALYSIS bound: one past the last byte that may be DISCOVERED, made a
    // block leader, or made a block's exit instruction. Equal to
    // end_address() unless the producer declared trailing guard words.
    uint32_t analysis_end_address() const {
        const uint32_t hi = end_address();
        return (analysis_guard_bytes < hi - load_address())
                   ? hi - analysis_guard_bytes
                   : hi;
    }

    bool has_analysis_guard() const { return analysis_guard_bytes != 0; }

    // Access code as 32-bit words (for MIPS disassembly).
    //
    // Deliberately bounded by end_address(), NOT analysis_end_address(): the
    // entire point of a guard word is that it is readable so that a transfer
    // at the last analysable word can emit its delay slot.
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

// Restrict a parsed main executable to a title-verified static-analysis size.
// The original disc image is not modified; only this in-memory analysis view
// is shortened. Returns false without mutation when the configured bound is
// invalid. A canonical page-rounded size above the header size is accepted as
// an existing runtime reservation and leaves the analysis view unchanged.
bool apply_static_analysis_bound(PS1Executable& exe,
                                 uint32_t configured_size,
                                 std::string& error_msg);

} // namespace PSXRecomp
