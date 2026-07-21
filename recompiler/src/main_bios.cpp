// main_bios.cpp
// ----------------------------------------------------------------------------
// Phase 1a entry point: psxrecomp-bios.
//
// Loads bios/SCPH1001.BIN as a flat 524288-byte ROM mapped at 0xBFC00000,
// walks the bounded boot slice from the reset vector, and emits:
//   - generated/boot_slice.c            (only if walk succeeds)
//   - generated/boot_slice_manifest.json
//   - generated/unsupported_ops.json    (always; "[]" on success)
//
// Self-validates the emitted boot_slice.c with `<C compiler> -c` (or `cl /c`).
// If the C compiler is not on PATH, the tool fails — there is no skip path.
//
// CLI:
//   psxrecomp-bios <bios.bin> <out_dir> [--cc <path-to-c-compiler>]
//
// Exit codes:
//   0  success: walk completed, all instructions translated, boot_slice.c
//      compiled cleanly, manifest written, unsupported_ops.json == "[]".
//   1  any failure (file size, walk, unsupported, codegen self-validate, ...).
//   2  CLI usage error.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "fmt/format.h"

#include "bios_slice_walker.h"
#include "config_loader.h"
#include "full_function_emitter.h"
#include "function_discovery.h"
#include "mips_decoder.h"

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kBiosBase     = 0xBFC00000u;
constexpr size_t   kBiosSize     = 524288u;
constexpr uint32_t kSliceStart   = 0xBFC00000u;
constexpr uint32_t kMaxSliceBytes = 4096u;

// ----- SHA-256 (small portable implementation) ---------------------------
// Phase 1a needs a SHA-256 of the BIOS image as a proof artifact in the
// manifest. Implementation is a straight transcription of FIPS 180-4. No
// dependency on OpenSSL/CryptoAPI to keep the recompiler tool portable.

struct Sha256 {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buffer[64];
    size_t   buffer_len;

    static constexpr uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };

    void init() {
        state[0]=0x6a09e667; state[1]=0xbb67ae85; state[2]=0x3c6ef372; state[3]=0xa54ff53a;
        state[4]=0x510e527f; state[5]=0x9b05688c; state[6]=0x1f83d9ab; state[7]=0x5be0cd19;
        bitlen = 0;
        buffer_len = 0;
    }

    static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

    void transform(const uint8_t* data) {
        uint32_t m[64];
        for (int i = 0, j = 0; i < 16; ++i, j += 4) {
            m[i] = (uint32_t(data[j]) << 24) | (uint32_t(data[j+1]) << 16)
                 | (uint32_t(data[j+2]) << 8) | uint32_t(data[j+3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(m[i-15],7) ^ rotr(m[i-15],18) ^ (m[i-15] >> 3);
            uint32_t s1 = rotr(m[i-2],17) ^ rotr(m[i-2],19) ^ (m[i-2] >> 10);
            m[i] = m[i-16] + s0 + m[i-7] + s1;
        }
        uint32_t a=state[0],b=state[1],c=state[2],d=state[3],
                 e=state[4],f=state[5],g=state[6],h=state[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = h + S1 + ch + K[i] + m[i];
            uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
        state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
    }

    void update(const uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            buffer[buffer_len++] = data[i];
            if (buffer_len == 64) {
                transform(buffer);
                bitlen += 512;
                buffer_len = 0;
            }
        }
    }

    std::string finalize() {
        uint64_t total_bits = bitlen + uint64_t(buffer_len) * 8;
        buffer[buffer_len++] = 0x80;
        if (buffer_len > 56) {
            while (buffer_len < 64) buffer[buffer_len++] = 0;
            transform(buffer);
            buffer_len = 0;
        }
        while (buffer_len < 56) buffer[buffer_len++] = 0;
        for (int i = 7; i >= 0; --i) buffer[buffer_len++] = uint8_t((total_bits >> (i*8)) & 0xFF);
        transform(buffer);

        std::string out;
        out.reserve(64);
        for (int i = 0; i < 8; ++i) {
            for (int j = 3; j >= 0; --j) {
                uint8_t byte = uint8_t((state[i] >> (j*8)) & 0xFF);
                static const char* hex = "0123456789abcdef";
                out += hex[byte >> 4];
                out += hex[byte & 0xF];
            }
        }
        return out;
    }
};

constexpr uint32_t Sha256::K[64];

std::string sha256_hex(const std::vector<uint8_t>& data) {
    Sha256 s;
    s.init();
    s.update(data.data(), data.size());
    return s.finalize();
}

// ----- File I/O ----------------------------------------------------------

std::vector<uint8_t> load_file_strict(const fs::path& p, size_t expected_size) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        throw std::runtime_error(fmt::format("cannot open BIOS file: {}", p.string()));
    }
    f.seekg(0, std::ios::end);
    const std::streamsize size = f.tellg();
    if (size < 0) {
        throw std::runtime_error(fmt::format("cannot tellg() on BIOS file: {}", p.string()));
    }
    if (static_cast<size_t>(size) != expected_size) {
        throw std::runtime_error(fmt::format(
            "BIOS file {} has size {}, expected {} (Phase 1a refuses to operate on a non-standard image)",
            p.string(), static_cast<size_t>(size), expected_size));
    }
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(expected_size);
    f.read(reinterpret_cast<char*>(buf.data()), expected_size);
    if (!f) {
        throw std::runtime_error(fmt::format("short read on BIOS file: {}", p.string()));
    }
    return buf;
}

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            throw std::runtime_error(fmt::format("cannot open output file for write: {}", tmp.string()));
        }
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!f) {
            throw std::runtime_error(fmt::format("write error on: {}", tmp.string()));
        }
    }
    fs::rename(tmp, p);
}

// ----- Output emission ---------------------------------------------------

std::string make_unsupported_json(const PSXRecompV4::WalkResult& w) {
    if (w.unsupported.empty()) {
        return "[]\n";
    }
    std::ostringstream os;
    os << "[\n";
    for (size_t i = 0; i < w.unsupported.size(); ++i) {
        const auto& u = w.unsupported[i];
        os << "  {\n"
           << fmt::format("    \"address\": \"0x{:08X}\",\n", u.address)
           << fmt::format("    \"raw_word\": \"0x{:08X}\",\n", u.raw)
           << fmt::format("    \"opcode_top6\": \"0x{:02X}\",\n", u.opcode_top6)
           << fmt::format("    \"decoded_format\": \"{}\",\n", u.decoded_format)
           << fmt::format("    \"reason\": \"{}\"\n", u.reason)
           << "  }" << (i + 1 < w.unsupported.size() ? "," : "") << "\n";
    }
    os << "]\n";
    return os.str();
}

std::string make_manifest_json(const PSXRecompV4::WalkResult& w,
                               const fs::path&               bios_path,
                               size_t                        bios_size,
                               const std::string&            bios_sha256) {
    std::ostringstream os;
    os << "{\n";
    os << "  \"schema_version\": 1,\n";
    os << "  \"bios_image\": {\n";
    os << fmt::format("    \"path\": \"{}\",\n", bios_path.generic_string());
    os << fmt::format("    \"size\": {},\n", bios_size);
    os << fmt::format("    \"sha256\": \"{}\"\n", bios_sha256);
    os << "  },\n";
    os << "  \"slices\": [\n";
    os << "    {\n";
    os << fmt::format("      \"address\": \"0x{:08X}\",\n", w.start_addr);
    os << "      \"type\": \"function_entry\",\n";
    os << "      \"discovered_by\": \"reset_vector\",\n";
    os << fmt::format("      \"instructions\": {},\n", w.instruction_count);
    os << fmt::format("      \"ends_at\": \"0x{:08X}\",\n", w.end_addr);
    os << fmt::format("      \"termination_reason\": \"{}\"", w.termination_reason);
    if (w.termination_reason == "j" || w.termination_reason == "jal") {
        os << ",\n";
        os << fmt::format("      \"terminator_target\": \"0x{:08X}\"", w.terminator_target);
    }
    if (!w.termination_note.empty()) {
        os << ",\n";
        os << fmt::format("      \"termination_note\": \"{}\"", w.termination_note);
    }
    os << "\n    }\n";
    os << "  ]\n";
    os << "}\n";
    return os.str();
}

std::string make_boot_slice_c(const PSXRecompV4::WalkResult& w,
                              const std::string&             bios_sha256) {
    std::ostringstream os;
    os << "/* AUTO-GENERATED by psxrecomp-bios. DO NOT EDIT.\n"
       << " *\n"
       << " * Source: bios/SCPH1001.BIN sha256=" << bios_sha256 << "\n"
       << fmt::format(" * Slice:  0x{:08X} .. 0x{:08X}  ({} instructions)\n",
                      w.start_addr, w.end_addr, w.instruction_count)
       << fmt::format(" * Termination: {}", w.termination_reason);
    if (w.termination_reason == "j" || w.termination_reason == "jal") {
        os << fmt::format("  -> 0x{:08X}", w.terminator_target);
    }
    os << "\n";
    if (!w.termination_note.empty()) {
        os << " * Note: " << w.termination_note << "\n";
    }
    os << " *\n"
       << " * This file is a Phase 1a proof artifact. It is intended to compile\n"
       << " * with `gcc -c` against generated/cpu_state.h. It is NOT linkable into\n"
       << " * a runnable binary by design — see cpu_state.h for the rationale.\n"
       << " */\n\n"
       << "#include \"cpu_state.h\"\n\n"
       << "void boot_slice(CPUState* cpu) {\n";

    for (const auto& e : w.emitted) {
        os << fmt::format("    /* 0x{:08X}: {:08X}  {} */\n", e.address, e.raw, e.comment);
        os << "    " << e.c_code << "\n";
    }

    // If walk terminated with "size_limit" the last instruction did not emit
    // a `return;`, so we add an explicit one. (For terminators the translator
    // already emits `return;`.)
    if (w.termination_reason == "size_limit") {
        os << "    /* slice ended at size_limit, no terminator instruction reached */\n";
        os << "    return;\n";
    }

    os << "}\n";
    return os.str();
}

// ----- Self-validation: gcc -c on the emitted boot_slice.c ----------------

std::string find_c_compiler(const std::optional<std::string>& cli_override) {
    if (cli_override) return *cli_override;
    // Prefer gcc, then clang, then cl. The recompiler aborts if none are usable.
    static const std::array<const char*, 3> candidates = { "gcc", "clang", "cl" };
    for (const char* c : candidates) {
        // Probe with --version. We don't parse output; non-zero exit means not present.
        // The null device differs by platform: NUL on Windows, /dev/null on
        // POSIX. Using ">NUL" on macOS/Linux both misdirects the redirect and
        // leaves a stray file named "NUL" in the cwd.
#ifdef _WIN32
        const std::string null_dev = "NUL";
#else
        const std::string null_dev = "/dev/null";
#endif
        const std::string probe = std::string(c) + " --version >" + null_dev + " 2>&1";
        if (std::system(probe.c_str()) == 0) {
            return c;
        }
    }
    throw std::runtime_error(
        "no C compiler found on PATH (tried: gcc, clang, cl). Phase 1a requires "
        "self-validating boot_slice.c via -c. Install one or pass --cc <path>.");
}

void self_validate_c_file(const fs::path& boot_slice_c, const fs::path& include_dir, const std::string& cc) {
    // Build a compile-only command. Use forward slashes; both gcc and cl accept them on Windows.
    std::string cmd;
    if (cc == "cl" || cc.size() >= 2 && cc.substr(cc.size()-2) == "cl") {
        cmd = fmt::format(
            "{} /nologo /c /TC /W3 /WX /I\"{}\" \"{}\" /Fo\"{}\"",
            cc, include_dir.string(), boot_slice_c.string(),
            (boot_slice_c.parent_path() / "boot_slice.obj").string());
    } else {
        cmd = fmt::format(
            "{} -std=c99 -Wall -Werror -c -I\"{}\" \"{}\" -o \"{}\"",
            cc, include_dir.string(), boot_slice_c.string(),
            (boot_slice_c.parent_path() / "boot_slice.o").string());
    }
    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        throw std::runtime_error(fmt::format(
            "self-validation FAILED: `{}` returned exit code {}. boot_slice.c does not compile cleanly.",
            cmd, rc));
    }
}

// ----- Post-emission grep for stub markers --------------------------------

void scan_for_stub_markers(const std::string& content) {
    static const std::array<const char*, 4> markers = { "TODO", "FIXME", "XXX", "for now" };
    for (const char* m : markers) {
        if (content.find(m) != std::string::npos) {
            throw std::runtime_error(fmt::format(
                "boot_slice.c contains forbidden stub marker \"{}\" — Phase 1a refuses to write a stubbed artifact",
                m));
        }
    }
}

// ----- Phase 1c: seed file parsing -----------------------------------------

std::vector<PSXRecompV4::Seed> load_seeds(const fs::path& seed_path) {
    std::ifstream f(seed_path);
    if (!f) {
        throw std::runtime_error(fmt::format("cannot open seed file: {}", seed_path.string()));
    }
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    std::vector<PSXRecompV4::Seed> seeds;

    // Find the "seeds" array and only parse entries within it.
    // Stop before the "excluded" array (or end of file).
    size_t seeds_key = content.find("\"seeds\"");
    if (seeds_key == std::string::npos) {
        throw std::runtime_error(fmt::format("no \"seeds\" key found in {}", seed_path.string()));
    }
    size_t seeds_arr_start = content.find('[', seeds_key);
    if (seeds_arr_start == std::string::npos) {
        throw std::runtime_error(fmt::format("no \"seeds\" array found in {}", seed_path.string()));
    }

    // Find the matching closing bracket for the seeds array.
    int depth = 0;
    size_t seeds_arr_end = seeds_arr_start;
    for (size_t i = seeds_arr_start; i < content.size(); ++i) {
        if (content[i] == '[') depth++;
        else if (content[i] == ']') {
            depth--;
            if (depth == 0) {
                seeds_arr_end = i;
                break;
            }
        }
    }

    std::string seeds_section = content.substr(seeds_arr_start, seeds_arr_end - seeds_arr_start + 1);

    // Parse seed entries within the bounded section.
    size_t pos = 0;
    while (true) {
        pos = seeds_section.find("\"address\"", pos);
        if (pos == std::string::npos) break;

        size_t colon = seeds_section.find(':', pos);
        if (colon == std::string::npos) break;
        size_t quote1 = seeds_section.find('"', colon + 1);
        if (quote1 == std::string::npos) break;
        size_t quote2 = seeds_section.find('"', quote1 + 1);
        if (quote2 == std::string::npos) break;
        std::string addr_str = seeds_section.substr(quote1 + 1, quote2 - quote1 - 1);

        size_t label_pos = seeds_section.find("\"label\"", quote2);
        std::string label_str;
        if (label_pos != std::string::npos && label_pos < seeds_section.find('}', quote2)) {
            size_t lcolon = seeds_section.find(':', label_pos);
            size_t lq1 = seeds_section.find('"', lcolon + 1);
            size_t lq2 = seeds_section.find('"', lq1 + 1);
            if (lq1 != std::string::npos && lq2 != std::string::npos) {
                label_str = seeds_section.substr(lq1 + 1, lq2 - lq1 - 1);
            }
        }

        size_t rat_pos = seeds_section.find("\"rationale\"", quote2);
        std::string rat_str;
        if (rat_pos != std::string::npos && rat_pos < seeds_section.find('}', quote2)) {
            size_t rcolon = seeds_section.find(':', rat_pos);
            size_t rq1 = seeds_section.find('"', rcolon + 1);
            size_t rq2 = seeds_section.find('"', rq1 + 1);
            if (rq1 != std::string::npos && rq2 != std::string::npos) {
                rat_str = seeds_section.substr(rq1 + 1, rq2 - rq1 - 1);
            }
        }

        PSXRecompV4::Seed seed;
        seed.address = static_cast<uint32_t>(std::stoul(addr_str, nullptr, 16));
        seed.label = label_str;
        seed.rationale = rat_str;
        seeds.push_back(std::move(seed));

        pos = quote2 + 1;
    }

    if (seeds.empty()) {
        throw std::runtime_error(fmt::format("no seeds found in {}", seed_path.string()));
    }
    return seeds;
}

size_t add_bios_vector_target_seeds(
    std::vector<PSXRecompV4::Seed>& seeds,
    const std::vector<uint8_t>& rom,
    uint32_t base_addr,
    uint32_t rom_end,
    const std::vector<PSXRecompV4::BiosVectorTable>& bios_vectors)
{
    std::set<uint32_t> known;
    for (const auto& seed : seeds) {
        known.insert(seed.address);
    }

    const uint32_t base_phys = base_addr & 0x1FFFFFFFu;
    const uint32_t rom_size = static_cast<uint32_t>(rom.size());
    size_t added = 0;

    for (const auto& bvt : bios_vectors) {
        const uint32_t table_phys = bvt.table_rom_addr & 0x1FFFFFFFu;
        if (table_phys < base_phys ||
            table_phys + bvt.table_count * 4u > base_phys + rom_size) {
            std::fprintf(stderr,
                "psxrecomp-bios: WARNING: vector table 0x%08X out of ROM range; "
                "not augmenting seeds from it\n",
                bvt.table_rom_addr);
            continue;
        }

        const uint32_t file_off = table_phys - base_phys;
        for (uint32_t i = 0; i < bvt.table_count; ++i) {
            const uint32_t off = file_off + i * 4u;
            const uint32_t entry =
                static_cast<uint32_t>(rom[off]) |
                (static_cast<uint32_t>(rom[off + 1]) << 8) |
                (static_cast<uint32_t>(rom[off + 2]) << 16) |
                (static_cast<uint32_t>(rom[off + 3]) << 24);
            if (entry == 0u) continue;
            if (entry < base_addr || entry > rom_end) continue;
            if (!known.insert(entry).second) continue;

            PSXRecompV4::Seed seed;
            seed.address = entry;
            seed.label = fmt::format("bios_vector_{:02X}_{:02X}",
                                     bvt.ram_addr & 0xFFu, i);
            seed.rationale = fmt::format(
                "ROM-resident target of configured BIOS vector 0x{:02X} table index 0x{:02X}",
                bvt.ram_addr & 0xFFu, i);
            seeds.push_back(std::move(seed));
            ++added;
        }
    }

    return added;
}

// ----- Phase 1c: artifact emission -----------------------------------------

std::string make_function_manifest_json(const PSXRecompV4::DiscoveryResult& dr,
                                        const std::string& bios_sha256) {
    std::ostringstream os;
    os << "{\n";
    os << "  \"schema\": \"psxrecomp phase1c function_manifest v1\",\n";
    os << fmt::format("  \"bios_sha256\": \"{}\",\n", bios_sha256);
    os << fmt::format("  \"total_functions\": {},\n", dr.total_functions);
    os << fmt::format("  \"total_instructions\": {},\n", dr.total_instructions);
    os << fmt::format("  \"total_edges\": {},\n", dr.total_edges);
    os << fmt::format("  \"total_indirect_jumps\": {},\n", dr.total_indirect_jumps);
    os << "  \"functions\": [\n";

    for (size_t i = 0; i < dr.functions.size(); ++i) {
        const auto& fn = dr.functions[i];
        os << "    {\n";
        os << fmt::format("      \"entry_addr\": \"0x{:08X}\",\n", fn.entry_addr);
        os << fmt::format("      \"normalized_addr\": \"0x{:08X}\",\n", fn.normalized_addr);
        os << fmt::format("      \"end_addr\": \"0x{:08X}\",\n", fn.end_addr);
        os << fmt::format("      \"instruction_count\": {},\n", fn.instruction_count);
        os << fmt::format("      \"termination_reason\": \"{}\",\n", fn.termination_reason);
        os << fmt::format("      \"discovered_by\": \"{}\",\n", fn.discovered_by);
        os << "      \"block_leaders\": [";
        for (size_t j = 0; j < fn.block_leaders.size(); ++j) {
            os << fmt::format("\"0x{:08X}\"", fn.block_leaders[j]);
            if (j + 1 < fn.block_leaders.size()) os << ", ";
        }
        os << "]\n";
        os << "    }" << (i + 1 < dr.functions.size() ? "," : "") << "\n";
    }

    os << "  ]\n";
    os << "}\n";
    return os.str();
}

std::string make_function_edges_json(const PSXRecompV4::DiscoveryResult& dr) {
    std::ostringstream os;
    os << "{\n";
    os << "  \"schema\": \"psxrecomp phase1c function_edges v1\",\n";
    os << fmt::format("  \"total_edges\": {},\n", dr.total_edges);
    os << "  \"edges\": [\n";

    for (size_t i = 0; i < dr.edges.size(); ++i) {
        const auto& e = dr.edges[i];
        os << "    {\n";
        os << fmt::format("      \"caller_func\": \"0x{:08X}\",\n", e.caller_func);
        os << fmt::format("      \"caller_pc\": \"0x{:08X}\",\n", e.caller_pc);
        os << fmt::format("      \"callee_addr\": \"0x{:08X}\",\n", e.callee_addr);
        os << fmt::format("      \"in_rom\": {}\n", e.in_rom ? "true" : "false");
        os << "    }" << (i + 1 < dr.edges.size() ? "," : "") << "\n";
    }

    os << "  ]\n";
    os << "}\n";
    return os.str();
}

std::string make_discovery_log_json(const PSXRecompV4::DiscoveryResult& dr,
                                    const std::vector<PSXRecompV4::Seed>& seeds) {
    std::ostringstream os;
    os << "{\n";
    os << "  \"schema\": \"psxrecomp phase1c discovery_run v1\",\n";
    os << "  \"seeds\": [\n";
    for (size_t i = 0; i < seeds.size(); ++i) {
        os << fmt::format("    {{\"address\": \"0x{:08X}\", \"label\": \"{}\"}}",
                          seeds[i].address, seeds[i].label);
        os << (i + 1 < seeds.size() ? ",\n" : "\n");
    }
    os << "  ],\n";
    os << "  \"counters\": {\n";
    os << fmt::format("    \"functions_discovered\": {},\n", dr.total_functions);
    os << fmt::format("    \"total_instructions\": {},\n", dr.total_instructions);
    os << fmt::format("    \"total_call_edges\": {},\n", dr.total_edges);
    os << fmt::format("    \"total_indirect_jumps\": {},\n", dr.total_indirect_jumps);
    os << fmt::format("    \"unsupported_instructions\": {}\n", dr.unsupported.size());
    os << "  },\n";

    // Opcode histogram.
    os << "  \"opcode_histogram\": {\n";
    std::vector<std::pair<std::string, uint32_t>> hist(dr.opcode_histogram.begin(),
                                                        dr.opcode_histogram.end());
    std::sort(hist.begin(), hist.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < hist.size(); ++i) {
        os << fmt::format("    \"{}\": {}", hist[i].first, hist[i].second);
        os << (i + 1 < hist.size() ? ",\n" : "\n");
    }
    os << "  },\n";

    // Indirect jumps summary.
    os << "  \"indirect_jumps\": [\n";
    for (size_t i = 0; i < dr.indirect_jumps.size(); ++i) {
        const auto& ij = dr.indirect_jumps[i];
        os << fmt::format("    {{\"func\": \"0x{:08X}\", \"pc\": \"0x{:08X}\", "
                          "\"reg\": {}, \"kind\": \"{}\"}}",
                          ij.func_addr, ij.pc, ij.reg, ij.kind);
        os << (i + 1 < dr.indirect_jumps.size() ? ",\n" : "\n");
    }
    os << "  ],\n";

    // Unsupported instructions.
    os << "  \"unsupported\": [\n";
    for (size_t i = 0; i < dr.unsupported.size(); ++i) {
        const auto& u = dr.unsupported[i];
        os << fmt::format("    {{\"address\": \"0x{:08X}\", \"raw\": \"0x{:08X}\", "
                          "\"reason\": \"{}\"}}",
                          u.address, u.raw, u.reason);
        os << (i + 1 < dr.unsupported.size() ? ",\n" : "\n");
    }
    os << "  ]\n";

    os << "}\n";
    return os.str();
}

// ----- Phase 1d: indirect jump artifacts -----------------------------------

std::string make_indirect_jumps_json(const PSXRecompV4::DiscoveryResult& dr) {
    std::ostringstream os;
    os << "{\n";
    os << "  \"schema\": \"psxrecomp phase1d indirect_jumps v1\",\n";
    os << fmt::format("  \"total_sites\": {},\n", dr.indirect_jumps.size());
    os << "  \"sites\": [\n";

    for (size_t i = 0; i < dr.indirect_jumps.size(); ++i) {
        const auto& ij = dr.indirect_jumps[i];
        os << "    {\n";
        os << fmt::format("      \"func_addr\": \"0x{:08X}\",\n", ij.func_addr);
        os << fmt::format("      \"pc\": \"0x{:08X}\",\n", ij.pc);
        os << fmt::format("      \"register\": {},\n", ij.reg);
        os << fmt::format("      \"register_name\": \"{}\",\n",
                          PSXRecomp::MipsDecoder::register_name(ij.reg));
        os << fmt::format("      \"kind\": \"{}\",\n", ij.kind);
        os << fmt::format("      \"classification\": \"{}\",\n", ij.classification);
        os << "      \"context_window\": [\n";
        for (size_t j = 0; j < ij.context_window.size(); ++j) {
            const auto& pi = ij.context_window[j];
            os << fmt::format("        {{\"address\": \"0x{:08X}\", \"raw\": \"0x{:08X}\", "
                              "\"mnemonic\": \"{}\"}}",
                              pi.address, pi.raw, pi.mnemonic);
            os << (j + 1 < ij.context_window.size() ? ",\n" : "\n");
        }
        os << "      ]\n";
        os << "    }" << (i + 1 < dr.indirect_jumps.size() ? "," : "") << "\n";
    }

    os << "  ]\n";
    os << "}\n";
    return os.str();
}

std::string make_indirect_jump_classes_json(const PSXRecompV4::DiscoveryResult& dr) {
    // Count sites per classification.
    std::map<std::string, std::vector<std::string>> classes;
    for (const auto& ij : dr.indirect_jumps) {
        classes[ij.classification].push_back(
            fmt::format("0x{:08X}", ij.pc));
    }

    std::ostringstream os;
    os << "{\n";
    os << "  \"schema\": \"psxrecomp phase1d indirect_jump_classes v1\",\n";
    os << fmt::format("  \"total_sites\": {},\n", dr.indirect_jumps.size());
    os << "  \"classes\": {\n";

    size_t ci = 0;
    for (const auto& [cls, sites] : classes) {
        os << fmt::format("    \"{}\": {{\n", cls);
        os << fmt::format("      \"count\": {},\n", sites.size());
        os << "      \"sites\": [";
        for (size_t j = 0; j < sites.size(); ++j) {
            os << fmt::format("\"{}\"", sites[j]);
            if (j + 1 < sites.size()) os << ", ";
        }
        os << "]\n";
        os << "    }";
        ci++;
        os << (ci < classes.size() ? ",\n" : "\n");
    }

    os << "  }\n";
    os << "}\n";
    return os.str();
}

std::string make_unsupported_json_discovery(const PSXRecompV4::DiscoveryResult& dr) {
    if (dr.unsupported.empty()) {
        return "[]\n";
    }
    std::ostringstream os;
    os << "[\n";
    for (size_t i = 0; i < dr.unsupported.size(); ++i) {
        const auto& u = dr.unsupported[i];
        os << "  {\n"
           << fmt::format("    \"address\": \"0x{:08X}\",\n", u.address)
           << fmt::format("    \"raw_word\": \"0x{:08X}\",\n", u.raw)
           << fmt::format("    \"reason\": \"{}\"\n", u.reason)
           << "  }" << (i + 1 < dr.unsupported.size() ? "," : "") << "\n";
    }
    os << "]\n";
    return os.str();
}

// ----- Phase 1a boot slice mode (original) ---------------------------------

int run_boot_slice(const fs::path& bios_path, const fs::path& out_dir,
                   const std::optional<std::string>& cc_override) {
    // 1. Load + validate BIOS file.
    const auto rom = load_file_strict(bios_path, kBiosSize);
    const std::string sha = sha256_hex(rom);

    // 2. Walk the bounded slice.
    const auto walk = PSXRecompV4::BiosSliceWalker::walk(rom, kBiosBase, kSliceStart, kMaxSliceBytes);

    // 3. Always write unsupported_ops.json (presence == "we checked").
    const fs::path unsupported_path = out_dir / "unsupported_ops.json";
    write_file(unsupported_path, make_unsupported_json(walk));

    if (!walk.ok || !walk.unsupported.empty()) {
        std::fprintf(stderr,
            "psxrecomp-bios: %zu unsupported instructions in boot slice; "
            "see %s\n",
            walk.unsupported.size(), unsupported_path.string().c_str());
        return 1;
    }

    if (walk.termination_reason == "size_limit") {
        std::fprintf(stderr,
            "psxrecomp-bios: walk hit 4 KB cap with no JR/JALR/J/JAL/RFE terminator. "
            "Slice is technically valid but flagged for review. Refusing to "
            "auto-emit; re-run with explicit approval.\n");
        return 1;
    }

    // 4. Cross-check the manifest invariants.
    const uint32_t expected_end = walk.start_addr + 4u * walk.instruction_count - 4u;
    if (walk.end_addr != expected_end) {
        throw std::runtime_error(fmt::format(
            "manifest invariant violated: end_addr=0x{:08X}, expected 0x{:08X} "
            "(start=0x{:08X}, instructions={})",
            walk.end_addr, expected_end, walk.start_addr, walk.instruction_count));
    }

    // 5. Emit boot_slice.c, scan for stub markers, write atomically.
    const std::string c_src = make_boot_slice_c(walk, sha);
    scan_for_stub_markers(c_src);
    const fs::path boot_slice_path = out_dir / "boot_slice.c";
    write_file(boot_slice_path, c_src);

    // 6. Emit manifest.
    const std::string manifest = make_manifest_json(walk, bios_path, kBiosSize, sha);
    const fs::path manifest_path = out_dir / "boot_slice_manifest.json";
    write_file(manifest_path, manifest);

    // 7. Self-validate the C with `gcc -c`.
    const std::string cc = find_c_compiler(cc_override);
    self_validate_c_file(boot_slice_path, out_dir, cc);

    // 8. Success report.
    std::fprintf(stdout,
        "psxrecomp-bios: OK  slice=0x%08X..0x%08X  instructions=%u  termination=%s\n",
        walk.start_addr, walk.end_addr, walk.instruction_count,
        walk.termination_reason.c_str());
    return 0;
}

// ----- Phase 2: full BIOS emission ------------------------------------------

int run_emit_full(const fs::path& bios_path, const fs::path& out_dir,
                  const fs::path& seed_path,
                  const std::vector<PSXRecompV4::BiosVectorTable>& bios_vectors = {},
                  const std::vector<PSXRecompV4::BiosAlias>& bios_aliases = {}) {
    // 1. Load + validate BIOS file.
    const auto rom = load_file_strict(bios_path, kBiosSize);
    const std::string sha = sha256_hex(rom);

    // 2. Load seeds.
    auto seeds = load_seeds(seed_path);
    const size_t vector_seed_count =
        add_bios_vector_target_seeds(seeds, rom, kBiosBase,
                                     kBiosBase + static_cast<uint32_t>(kBiosSize) - 1,
                                     bios_vectors);
    std::fprintf(stdout, "psxrecomp-bios: loaded %zu seeds from %s\n",
                 seeds.size(), seed_path.string().c_str());
    if (vector_seed_count != 0) {
        std::fprintf(stdout,
            "psxrecomp-bios: added %zu ROM-resident BIOS vector target seeds\n",
            vector_seed_count);
    }

    // Also seed bios_alias targets — same reachability hole as vector-table
    // entries: a fixed-target trampoline's callee may never be jal'd in ROM.
    {
        const uint32_t base_phys = kBiosBase & 0x1FFFFFFFu;
        std::set<uint32_t> have;
        for (const auto& sd : seeds) have.insert(sd.address & 0x1FFFFFFFu);
        size_t alias_added = 0;
        for (const auto& ba : bios_aliases) {
            const uint32_t phys = ba.target_key & 0x1FFFFFFFu;
            if (phys < base_phys || phys >= base_phys + (uint32_t)kBiosSize) continue;
            if ((phys & 3u) != 0u) continue;
            if (!have.insert(phys).second) continue;
            seeds.push_back({kBiosBase | (phys - base_phys),
                             fmt::format("bios_alias_{:08X}", ba.ram_addr),
                             "ROM-resident target of configured BIOS alias"});
            alias_added++;
        }
        if (alias_added != 0) {
            std::fprintf(stdout,
                "psxrecomp-bios: added %zu BIOS alias target seeds\n",
                alias_added);
        }
    }

    // 3. Run discovery.
    const auto dr = PSXRecompV4::FunctionDiscovery::discover(
        rom, kBiosBase, kBiosBase + static_cast<uint32_t>(kBiosSize) - 1, seeds);

    if (!dr.ok) {
        // Write unsupported_ops.json for diagnosis, but continue — some
        // functions may be skippable (e.g. FPU).
        std::fprintf(stderr,
            "psxrecomp-bios: WARNING: discovery had %zu unsupported instructions; "
            "they will be skipped during emission\n",
            dr.unsupported.size());
    }

    // 4. Emit full C.
    const auto stats = PSXRecompV4::FullFunctionEmitter::emit(
        rom, kBiosBase, kBiosBase + static_cast<uint32_t>(kBiosSize) - 1,
        dr, sha, out_dir.string(), bios_vectors, bios_aliases);

    std::fprintf(stdout,
        "psxrecomp-bios: EMIT OK  emitted=%u  skipped=%u  instructions=%u  "
        "dispatch_entries=%u\n",
        stats.functions_emitted, stats.functions_skipped,
        stats.total_instructions, stats.dispatch_entries);

    if (stats.functions_skipped > 0) {
        std::fprintf(stdout, "psxrecomp-bios: skipped functions:\n");
        for (const auto& [addr, reason] : stats.skipped) {
            std::fprintf(stdout, "  0x%08X: %s\n", addr, reason.c_str());
        }
    }

    return 0;
}

// ----- Phase 1c discovery mode ---------------------------------------------

int run_discover(const fs::path& bios_path, const fs::path& out_dir,
                 const fs::path& seed_path) {
    // 1. Load + validate BIOS file.
    const auto rom = load_file_strict(bios_path, kBiosSize);
    const std::string sha = sha256_hex(rom);

    // 2. Load seeds.
    const auto seeds = load_seeds(seed_path);
    std::fprintf(stdout, "psxrecomp-bios: loaded %zu seeds from %s\n",
                 seeds.size(), seed_path.string().c_str());

    // 3. Run discovery.
    const auto dr = PSXRecompV4::FunctionDiscovery::discover(
        rom, kBiosBase, kBiosBase + static_cast<uint32_t>(kBiosSize) - 1, seeds);

    // 4. Write unsupported_ops.json.
    const fs::path unsupported_path = out_dir / "unsupported_ops.json";
    write_file(unsupported_path, make_unsupported_json_discovery(dr));

    // 5. Write function_manifest.json.
    const fs::path manifest_path = out_dir / "function_manifest.json";
    write_file(manifest_path, make_function_manifest_json(dr, sha));

    // 6. Write function_edges.json.
    const fs::path edges_path = out_dir / "function_edges.json";
    write_file(edges_path, make_function_edges_json(dr));

    // 7. Write discovery_run.log.json.
    const fs::path log_path = out_dir / "discovery_run.log.json";
    write_file(log_path, make_discovery_log_json(dr, seeds));

    // 8. Phase 1d artifacts: indirect jump detail and classification.
    const fs::path ij_path = out_dir / "indirect_jumps.json";
    write_file(ij_path, make_indirect_jumps_json(dr));

    const fs::path ijc_path = out_dir / "indirect_jump_classes.json";
    write_file(ijc_path, make_indirect_jump_classes_json(dr));

    // 9. Report.
    if (!dr.ok) {
        std::fprintf(stderr,
            "psxrecomp-bios: DISCOVERY FAILED: %zu unsupported instructions; "
            "see %s\n",
            dr.unsupported.size(), unsupported_path.string().c_str());
        return 1;
    }

    std::fprintf(stdout,
        "psxrecomp-bios: DISCOVERY OK  functions=%u  instructions=%u  "
        "edges=%u  indirect_jumps=%u\n",
        dr.total_functions, dr.total_instructions,
        dr.total_edges, dr.total_indirect_jumps);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "-h" || a == "--help") {
                std::fprintf(stdout,
                    "usage: psxrecomp-bios --config <path.toml>\n"
                    "       psxrecomp-bios <bios.bin> <out_dir> [--cc <c-compiler>]\n"
                    "       psxrecomp-bios <bios.bin> <out_dir> --discover <seeds.json>\n"
                    "       psxrecomp-bios <bios.bin> <out_dir> --emit-full <seeds.json>\n");
                return 0;
            }
        }

        // ── --config <path.toml> short-form ────────────────────────────
        // If --config is the first (or only) flag, all paths come from the
        // TOML. This is the going-forward invocation; the positional form
        // below stays for backwards compat.
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--config" && i + 1 < argc) {
                const fs::path config_path = argv[i + 1];
                const auto cfg = PSXRecompV4::load_bios_config(config_path);
                std::fprintf(stdout,
                    "psxrecomp-bios: --config %s\n"
                    "  rom        = %s\n"
                    "  seeds      = %s\n"
                    "  out_dir    = %s\n"
                    "  out_stem   = %s\n",
                    config_path.string().c_str(),
                    cfg.rom_path.string().c_str(),
                    cfg.seeds_path.string().c_str(),
                    cfg.out_dir.string().c_str(),
                    cfg.out_stem.c_str());
                return run_emit_full(cfg.rom_path, cfg.out_dir, cfg.seeds_path,
                                     cfg.bios_vectors, cfg.bios_aliases);
            }
            if (a == "--config=" || a.rfind("--config=", 0) == 0) {
                const fs::path config_path = a.substr(std::string("--config=").size());
                const auto cfg = PSXRecompV4::load_bios_config(config_path);
                return run_emit_full(cfg.rom_path, cfg.out_dir, cfg.seeds_path,
                                     cfg.bios_vectors, cfg.bios_aliases);
            }
        }

        // ── Positional form (legacy) ───────────────────────────────────
        if (argc < 3) {
            std::fprintf(stderr,
                "usage: psxrecomp-bios --config <path.toml>            # going-forward\n"
                "       psxrecomp-bios <bios.bin> <out_dir> [--cc <c-compiler>]\n"
                "       psxrecomp-bios <bios.bin> <out_dir> --discover <seeds.json>\n"
                "       psxrecomp-bios <bios.bin> <out_dir> --emit-full <seeds.json>\n");
            return 2;
        }
        const fs::path bios_path = argv[1];
        const fs::path out_dir   = argv[2];
        std::optional<std::string> cc_override;
        std::optional<fs::path> seed_path;
        std::optional<fs::path> emit_full_seed_path;

        for (int i = 3; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--cc" && i + 1 < argc) {
                cc_override = argv[++i];
            } else if (a == "--discover" && i + 1 < argc) {
                seed_path = argv[++i];
            } else if (a == "--emit-full" && i + 1 < argc) {
                emit_full_seed_path = argv[++i];
            } else {
                std::fprintf(stderr, "psxrecomp-bios: unknown argument: %s\n", a.c_str());
                return 2;
            }
        }

        if (emit_full_seed_path) {
            return run_emit_full(bios_path, out_dir, *emit_full_seed_path);
        } else if (seed_path) {
            return run_discover(bios_path, out_dir, *seed_path);
        } else {
            return run_boot_slice(bios_path, out_dir, cc_override);
        }

    } catch (const std::exception& ex) {
        std::fprintf(stderr, "psxrecomp-bios: FATAL: %s\n", ex.what());
        return 1;
    }
}
