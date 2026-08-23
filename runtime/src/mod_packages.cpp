#include "mod_packages.h"

#include "crc32.h"
#include "mod_plugins.h"
#include "psx_sha256.h"
#include "toml.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace PSXRecompV4 {
namespace {

constexpr uint32_t kMinFormatVersion = 1;
constexpr uint32_t kMaxFormatVersion = 5;
constexpr uint64_t kMaxArchiveBytes = 0xffffffffull; /* ZIP32 size fields */
constexpr uint32_t kMaxArchiveFiles = 4096;

std::map<std::string, ModBuiltinResolver>& builtin_resolvers() {
    static std::map<std::string, ModBuiltinResolver> value;
    return value;
}

struct RegisteredPlugin {
    PSXModActivationCallback activation = nullptr;
    PSXModVBlankCallback vblank = nullptr;
    /* Function-entry callbacks live in mod_runtime; this flag makes the id
     * visible to package resolve (manifest [[plugin]] / mod_plugin_registered). */
    bool function_entry = false;
};

std::map<std::string, RegisteredPlugin>& registered_plugins() {
    static std::map<std::string, RegisteredPlugin> value;
    return value;
}

void set_error(std::string* out, const std::string& value) {
    if (out) *out = value;
}

bool valid_id(const std::string& value) {
    if (value.empty() || value.size() > 96) return false;
    for (unsigned char c : value) {
        if (!(std::islower(c) || std::isdigit(c) || c == '.' || c == '-' || c == '_'))
            return false;
    }
    return value.front() != '.' && value.back() != '.';
}

bool valid_sha256(const std::string& value) {
    return value.size() == 64 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isdigit(c) || (c >= 'a' && c <= 'f');
        });
}

bool valid_web_url(const std::string& value) {
    return value.rfind("https://", 0) == 0 || value.rfind("http://", 0) == 0;
}

bool parse_hex_bytes(const std::string& text, std::vector<uint8_t>& out) {
    std::string compact;
    compact.reserve(text.size());
    for (unsigned char c : text) {
        if (!std::isspace(c) && c != '_') compact.push_back((char)c);
    }
    if (compact.size() % 2 != 0) return false;
    out.clear();
    out.reserve(compact.size() / 2);
    auto nibble = [](unsigned char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        c = (unsigned char)std::tolower(c);
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (size_t i = 0; i < compact.size(); i += 2) {
        const int hi = nibble((unsigned char)compact[i]);
        const int lo = nibble((unsigned char)compact[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back((uint8_t)((hi << 4) | lo));
    }
    return true;
}

bool parse_value_encoding(const std::string& text, ModValueEncoding& out) {
    static const std::pair<const char*, ModValueEncoding> values[] = {
        {"u8", ModValueEncoding::U8},
        {"u16le", ModValueEncoding::U16LE},
        {"u32le", ModValueEncoding::U32LE},
        {"mips_lui_ori_u32", ModValueEncoding::MipsLuiOriU32},
    };
    for (const auto& [name, encoding] : values) {
        if (text == name) {
            out = encoding;
            return true;
        }
    }
    return false;
}

bool parse_integer_predicate_op(const std::string& text,
                                ModIntegerPredicateOp& out) {
    static const std::pair<const char*, ModIntegerPredicateOp> values[] = {
        {"eq", ModIntegerPredicateOp::Equal},
        {"ne", ModIntegerPredicateOp::NotEqual},
        {"lt", ModIntegerPredicateOp::Less},
        {"le", ModIntegerPredicateOp::LessEqual},
        {"gt", ModIntegerPredicateOp::Greater},
        {"ge", ModIntegerPredicateOp::GreaterEqual},
    };
    for (const auto& [name, op] : values) {
        if (text == name) {
            out = op;
            return true;
        }
    }
    return false;
}

uint32_t value_encoding_size(ModValueEncoding encoding) {
    switch (encoding) {
    case ModValueEncoding::U8:
        return 1;
    case ModValueEncoding::U16LE:
        return 2;
    case ModValueEncoding::U32LE:
        return 4;
    case ModValueEncoding::MipsLuiOriU32:
        return 8;
    }
    return 0;
}

bool valid_mips_lui_ori_guard(const std::vector<uint8_t>& expected,
                              uint64_t offset) {
    if (offset > expected.size() || expected.size() - offset < 8)
        return false;
    const uint8_t* bytes =
        expected.data() + static_cast<size_t>(offset);
    const uint32_t lui =
        static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24);
    const uint32_t ori =
        static_cast<uint32_t>(bytes[4]) |
        (static_cast<uint32_t>(bytes[5]) << 8) |
        (static_cast<uint32_t>(bytes[6]) << 16) |
        (static_cast<uint32_t>(bytes[7]) << 24);
    const uint32_t lui_rs = (lui >> 21) & 0x1Fu;
    const uint32_t lui_rt = (lui >> 16) & 0x1Fu;
    const uint32_t ori_rs = (ori >> 21) & 0x1Fu;
    const uint32_t ori_rt = (ori >> 16) & 0x1Fu;
    return (lui >> 26) == 0x0Fu && lui_rs == 0 &&
           lui_rt != 0 && (ori >> 26) == 0x0Du &&
           ori_rs == lui_rt && ori_rt == lui_rt;
}

struct RelativeRange {
    uint64_t offset = 0;
    uint64_t size = 0;
};

std::vector<RelativeRange> patch_field_ranges(const ModPatchField& field) {
    if (!field.replacement.empty())
        return {{field.offset, field.replacement.size()}};
    if (field.replace_encoding == ModValueEncoding::MipsLuiOriU32)
        return {{field.offset, 2}, {field.offset + 4, 2}};
    return {{field.offset, value_encoding_size(field.replace_encoding)}};
}

std::string hex_bytes(const std::vector<uint8_t>& bytes) {
    std::ostringstream out;
    for (uint8_t byte : bytes)
        out << std::hex << std::setw(2) << std::setfill('0') << (unsigned)byte;
    return out.str();
}

struct SemVer {
    int64_t major = 0, minor = 0, patch = 0;
    std::string suffix;
    bool valid = false;
};

SemVer parse_semver(const std::string& text) {
    SemVer out;
    std::string core = text;
    const size_t dash = core.find('-');
    if (dash != std::string::npos) {
        out.suffix = core.substr(dash + 1);
        core.resize(dash);
    }
    std::array<int64_t*, 3> parts = {&out.major, &out.minor, &out.patch};
    size_t at = 0;
    for (size_t i = 0; i < parts.size(); ++i) {
        const size_t end = core.find('.', at);
        const std::string token = core.substr(at, end == std::string::npos ? end : end - at);
        if (token.empty() ||
            !std::all_of(token.begin(), token.end(), [](unsigned char c) { return std::isdigit(c); }))
            return out;
        try {
            *parts[i] = std::stoll(token);
        } catch (...) {
            return out;
        }
        if (end == std::string::npos) {
            if (i != 2) return out;
            at = core.size();
        } else {
            at = end + 1;
        }
    }
    if (at != core.size()) return out;
    out.valid = true;
    return out;
}

int compare_semver(const std::string& a, const std::string& b) {
    const SemVer av = parse_semver(a), bv = parse_semver(b);
    if (!av.valid || !bv.valid) return a.compare(b);
    if (av.major != bv.major) return av.major < bv.major ? -1 : 1;
    if (av.minor != bv.minor) return av.minor < bv.minor ? -1 : 1;
    if (av.patch != bv.patch) return av.patch < bv.patch ? -1 : 1;
    if (av.suffix.empty() != bv.suffix.empty()) return av.suffix.empty() ? 1 : -1;
    return av.suffix.compare(bv.suffix);
}

bool version_satisfies(const std::string& actual, const std::string& requirement) {
    if (requirement.empty() || requirement == "*") return true;
    if (requirement.rfind(">=", 0) == 0)
        return compare_semver(actual, requirement.substr(2)) >= 0;
    if (requirement.rfind("<=", 0) == 0)
        return compare_semver(actual, requirement.substr(2)) <= 0;
    if (requirement.rfind(">", 0) == 0)
        return compare_semver(actual, requirement.substr(1)) > 0;
    if (requirement.rfind("<", 0) == 0)
        return compare_semver(actual, requirement.substr(1)) < 0;
    if (requirement.rfind("^", 0) == 0) {
        const SemVer base = parse_semver(requirement.substr(1));
        const SemVer got = parse_semver(actual);
        return base.valid && got.valid && got.major == base.major &&
               compare_semver(actual, requirement.substr(1)) >= 0;
    }
    return actual == requirement;
}

std::string quote_toml(const std::string& value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '\\' || c == '"') out.push_back('\\');
        if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out.push_back((char)c);
    }
    out.push_back('"');
    return out;
}

bool read_file(const fs::path& path, std::vector<uint8_t>& out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        set_error(error, "cannot open " + path.string());
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0 || (uint64_t)size > kMaxArchiveBytes) {
        set_error(error, "archive is too large");
        return false;
    }
    in.seekg(0);
    out.resize((size_t)size);
    if (!out.empty() && !in.read((char*)out.data(), size)) {
        set_error(error, "cannot read " + path.string());
        return false;
    }
    return true;
}

uint16_t le16(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

struct ZipEntry {
    std::string name;
    uint16_t method = 0;
    uint32_t crc = 0;
    uint32_t compressed_size = 0;
    uint32_t size = 0;
    uint32_t local_offset = 0;
    bool directory = false;
};

bool safe_archive_name(const std::string& name) {
    if (name.empty() || name.size() > 512 || name[0] == '/' || name[0] == '\\')
        return false;
    if (name.size() >= 2 && std::isalpha((unsigned char)name[0]) && name[1] == ':')
        return false;
    fs::path p = fs::path(name).lexically_normal();
    for (const auto& part : p) {
        const std::string s = part.string();
        if (s == ".." || s == "." || s.empty()) return false;
    }
    return true;
}

bool parse_zip(const std::vector<uint8_t>& bytes, std::vector<ZipEntry>& entries,
               std::string* error) {
    if (bytes.size() < 22) {
        set_error(error, "not a ZIP archive");
        return false;
    }
    size_t eocd = std::string::npos;
    const size_t floor = bytes.size() > 65557 ? bytes.size() - 65557 : 0;
    for (size_t pos = bytes.size() - 22;; --pos) {
        if (le32(bytes.data() + pos) == 0x06054b50u) { eocd = pos; break; }
        if (pos == floor) break;
    }
    if (eocd == std::string::npos) {
        set_error(error, "ZIP end record is missing");
        return false;
    }
    const uint16_t count = le16(bytes.data() + eocd + 10);
    const uint32_t central_size = le32(bytes.data() + eocd + 12);
    const uint32_t central_offset = le32(bytes.data() + eocd + 16);
    if (count > kMaxArchiveFiles || (uint64_t)central_offset + central_size > bytes.size()) {
        set_error(error, "ZIP central directory is invalid");
        return false;
    }
    size_t at = central_offset;
    uint64_t expanded = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (at + 46 > bytes.size() || le32(bytes.data() + at) != 0x02014b50u) {
            set_error(error, "ZIP entry record is invalid");
            return false;
        }
        const uint16_t flags = le16(bytes.data() + at + 8);
        ZipEntry e;
        e.method = le16(bytes.data() + at + 10);
        e.crc = le32(bytes.data() + at + 16);
        e.compressed_size = le32(bytes.data() + at + 20);
        e.size = le32(bytes.data() + at + 24);
        const uint16_t name_len = le16(bytes.data() + at + 28);
        const uint16_t extra_len = le16(bytes.data() + at + 30);
        const uint16_t comment_len = le16(bytes.data() + at + 32);
        e.local_offset = le32(bytes.data() + at + 42);
        if (flags & 1u) {
            set_error(error, "encrypted ZIP entries are not supported");
            return false;
        }
        if (e.method != 0 && e.method != 8) {
            set_error(error, "ZIP compression method is not supported");
            return false;
        }
        if (at + 46ull + name_len + extra_len + comment_len > bytes.size()) {
            set_error(error, "ZIP entry name is truncated");
            return false;
        }
        e.name.assign((const char*)bytes.data() + at + 46, name_len);
        std::replace(e.name.begin(), e.name.end(), '\\', '/');
        e.directory = !e.name.empty() && e.name.back() == '/';
        if (!safe_archive_name(e.directory ? e.name.substr(0, e.name.size() - 1) : e.name)) {
            set_error(error, "unsafe ZIP path: " + e.name);
            return false;
        }
        expanded += e.size;
        if (expanded > kMaxArchiveBytes) {
            set_error(error, "expanded archive exceeds the size limit");
            return false;
        }
        entries.push_back(std::move(e));
        at += 46ull + name_len + extra_len + comment_len;
    }
    return true;
}

void zip_put_u16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back((uint8_t)(v & 0xff));
    out.push_back((uint8_t)((v >> 8) & 0xff));
}

void zip_put_u32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back((uint8_t)(v & 0xff));
    out.push_back((uint8_t)((v >> 8) & 0xff));
    out.push_back((uint8_t)((v >> 16) & 0xff));
    out.push_back((uint8_t)((v >> 24) & 0xff));
}

bool write_store_zip(const fs::path& root, std::vector<uint8_t>& out, std::string* error) {
    struct Item {
        std::string name;
        std::vector<uint8_t> data;
        uint32_t crc = 0;
        uint32_t local_offset = 0;
    };
    std::vector<Item> items;
    std::error_code ec;
    uint64_t total = 0;
    if (!fs::is_directory(root)) {
        set_error(error, "package directory is missing");
        return false;
    }
    for (const fs::directory_entry& entry :
         fs::recursive_directory_iterator(root, ec)) {
        if (ec) {
            set_error(error, "cannot read package directory: " + ec.message());
            return false;
        }
        if (!entry.is_regular_file(ec)) continue;
        fs::path rel = fs::relative(entry.path(), root, ec);
        if (ec) continue;
        std::string name = rel.generic_string();
        if (!safe_archive_name(name)) continue;
        Item item;
        item.name = std::move(name);
        if (!read_file(entry.path(), item.data, error)) return false;
        if (item.data.size() > 0xffffffffull) {
            set_error(error, "file exceeds ZIP32 size");
            return false;
        }
        total += item.data.size();
        if (total > 0xffffffffull) {
            set_error(error, "package exceeds ZIP32 size");
            return false;
        }
        if (items.size() >= kMaxArchiveFiles) {
            set_error(error, "package has too many files to transfer");
            return false;
        }
        item.crc = crc32_compute(item.data.data(), item.data.size());
        items.push_back(std::move(item));
    }
    if (items.empty()) {
        set_error(error, "package directory has no files");
        return false;
    }
    out.clear();
    out.reserve((size_t)total + items.size() * 96u + 64u);
    for (Item& item : items) {
        item.local_offset = (uint32_t)out.size();
        zip_put_u32(out, 0x04034b50u);
        zip_put_u16(out, 20);
        zip_put_u16(out, 0);
        zip_put_u16(out, 0);
        zip_put_u16(out, 0);
        zip_put_u16(out, 0);
        zip_put_u32(out, item.crc);
        zip_put_u32(out, (uint32_t)item.data.size());
        zip_put_u32(out, (uint32_t)item.data.size());
        zip_put_u16(out, (uint16_t)item.name.size());
        zip_put_u16(out, 0);
        out.insert(out.end(), item.name.begin(), item.name.end());
        out.insert(out.end(), item.data.begin(), item.data.end());
    }
    const uint32_t central_offset = (uint32_t)out.size();
    for (const Item& item : items) {
        zip_put_u32(out, 0x02014b50u);
        zip_put_u16(out, 20);
        zip_put_u16(out, 20);
        zip_put_u16(out, 0);
        zip_put_u16(out, 0);
        zip_put_u16(out, 0);
        zip_put_u16(out, 0);
        zip_put_u32(out, item.crc);
        zip_put_u32(out, (uint32_t)item.data.size());
        zip_put_u32(out, (uint32_t)item.data.size());
        zip_put_u16(out, (uint16_t)item.name.size());
        zip_put_u16(out, 0);
        zip_put_u16(out, 0);
        zip_put_u16(out, 0);
        zip_put_u16(out, 0);
        zip_put_u32(out, 0);
        zip_put_u32(out, item.local_offset);
        out.insert(out.end(), item.name.begin(), item.name.end());
    }
    const uint32_t central_size = (uint32_t)out.size() - central_offset;
    zip_put_u32(out, 0x06054b50u);
    zip_put_u16(out, 0);
    zip_put_u16(out, 0);
    zip_put_u16(out, (uint16_t)items.size());
    zip_put_u16(out, (uint16_t)items.size());
    zip_put_u32(out, central_size);
    zip_put_u32(out, central_offset);
    zip_put_u16(out, 0);
    return true;
}

struct DeflateBits {
    const uint8_t* at = nullptr;
    const uint8_t* end = nullptr;
    uint64_t hold = 0;
    unsigned bits = 0;

    bool read(unsigned count, uint32_t& out) {
        while (bits < count) {
            if (at == end) return false;
            hold |= (uint64_t)*at++ << bits;
            bits += 8;
        }
        out = count == 32 ? (uint32_t)hold :
              (uint32_t)(hold & ((1ull << count) - 1));
        hold >>= count;
        bits -= count;
        return true;
    }
    void align_byte() {
        const unsigned drop = bits & 7u;
        hold >>= drop;
        bits -= drop;
    }
};

struct DeflateHuffman {
    std::array<uint16_t, 16> count{};
    std::vector<uint16_t> symbols;
};

bool build_huffman(const std::vector<uint8_t>& lengths, DeflateHuffman& out) {
    out = {};
    for (uint8_t length : lengths) {
        if (length > 15) return false;
        out.count[length]++;
    }
    if (out.count[0] == lengths.size()) return false;
    int left = 1;
    for (int length = 1; length <= 15; ++length) {
        left <<= 1;
        left -= out.count[(size_t)length];
        if (left < 0) return false;
    }
    std::array<uint16_t, 16> offsets{};
    for (size_t length = 1; length < 15; ++length)
        offsets[length + 1] = offsets[length] + out.count[length];
    out.symbols.resize(lengths.size() - out.count[0]);
    for (uint16_t symbol = 0; symbol < lengths.size(); ++symbol)
        if (lengths[symbol])
            out.symbols[offsets[lengths[symbol]]++] = symbol;
    return true;
}

bool decode_symbol(DeflateBits& bits, const DeflateHuffman& table, uint16_t& symbol) {
    uint32_t code = 0, first = 0, index = 0;
    for (uint32_t length = 1; length <= 15; ++length) {
        uint32_t bit = 0;
        if (!bits.read(1, bit)) return false;
        code |= bit;
        const uint32_t count = table.count[length];
        if (code < first + count) {
            const uint32_t slot = index + code - first;
            if (slot >= table.symbols.size()) return false;
            symbol = table.symbols[slot];
            return true;
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return false;
}

bool inflate_deflate(const uint8_t* data, size_t size, size_t expected,
                     std::vector<uint8_t>& out) {
    static const uint16_t length_base[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,
        115,131,163,195,227,258};
    static const uint8_t length_extra[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const uint16_t distance_base[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
        1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const uint8_t distance_extra[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,
        12,12,13,13};
    DeflateBits input{data, data + size};
    out.clear();
    out.reserve(expected);
    bool final = false;
    while (!final) {
        uint32_t final_bit = 0, type = 0;
        if (!input.read(1, final_bit) || !input.read(2, type)) return false;
        final = final_bit != 0;
        if (type == 0) {
            input.align_byte();
            uint32_t length = 0, complement = 0;
            if (!input.read(16, length) || !input.read(16, complement) ||
                (length ^ 0xffffu) != complement ||
                out.size() + length > expected) return false;
            for (uint32_t i = 0; i < length; ++i) {
                uint32_t byte = 0;
                if (!input.read(8, byte)) return false;
                out.push_back((uint8_t)byte);
            }
            continue;
        }
        if (type == 3) return false;

        std::vector<uint8_t> literal_lengths;
        std::vector<uint8_t> distance_lengths;
        if (type == 1) {
            literal_lengths.resize(288);
            for (size_t i = 0; i <= 143; ++i) literal_lengths[i] = 8;
            for (size_t i = 144; i <= 255; ++i) literal_lengths[i] = 9;
            for (size_t i = 256; i <= 279; ++i) literal_lengths[i] = 7;
            for (size_t i = 280; i <= 287; ++i) literal_lengths[i] = 8;
            distance_lengths.assign(32, 5);
        } else {
            uint32_t hlit = 0, hdist = 0, hclen = 0;
            if (!input.read(5, hlit) || !input.read(5, hdist) ||
                !input.read(4, hclen)) return false;
            hlit += 257; hdist += 1; hclen += 4;
            static const uint8_t order[19] =
                {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            std::vector<uint8_t> code_lengths(19, 0);
            for (uint32_t i = 0; i < hclen; ++i) {
                uint32_t value = 0;
                if (!input.read(3, value)) return false;
                code_lengths[order[i]] = (uint8_t)value;
            }
            DeflateHuffman code_table;
            if (!build_huffman(code_lengths, code_table)) return false;
            std::vector<uint8_t> lengths;
            lengths.reserve(hlit + hdist);
            while (lengths.size() < hlit + hdist) {
                uint16_t symbol = 0;
                if (!decode_symbol(input, code_table, symbol)) return false;
                if (symbol <= 15) {
                    lengths.push_back((uint8_t)symbol);
                    continue;
                }
                uint32_t repeat = 0, extra = 0;
                uint8_t value = 0;
                if (symbol == 16) {
                    if (lengths.empty() || !input.read(2, extra)) return false;
                    repeat = extra + 3;
                    value = lengths.back();
                } else if (symbol == 17) {
                    if (!input.read(3, extra)) return false;
                    repeat = extra + 3;
                } else if (symbol == 18) {
                    if (!input.read(7, extra)) return false;
                    repeat = extra + 11;
                } else return false;
                if (lengths.size() + repeat > hlit + hdist) return false;
                lengths.insert(lengths.end(), repeat, value);
            }
            literal_lengths.assign(lengths.begin(), lengths.begin() + hlit);
            distance_lengths.assign(lengths.begin() + hlit, lengths.end());
        }
        DeflateHuffman literals, distances;
        if (!build_huffman(literal_lengths, literals) ||
            !build_huffman(distance_lengths, distances)) return false;
        for (;;) {
            uint16_t symbol = 0;
            if (!decode_symbol(input, literals, symbol)) return false;
            if (symbol < 256) {
                if (out.size() >= expected) return false;
                out.push_back((uint8_t)symbol);
                continue;
            }
            if (symbol == 256) break;
            if (symbol < 257 || symbol > 285) return false;
            const unsigned length_index = symbol - 257;
            uint32_t extra = 0;
            if (!input.read(length_extra[length_index], extra)) return false;
            const size_t length = length_base[length_index] + extra;
            uint16_t distance_symbol = 0;
            if (!decode_symbol(input, distances, distance_symbol) ||
                distance_symbol >= 30) return false;
            if (!input.read(distance_extra[distance_symbol], extra)) return false;
            const size_t distance = distance_base[distance_symbol] + extra;
            if (distance == 0 || distance > out.size() ||
                out.size() + length > expected) return false;
            for (size_t i = 0; i < length; ++i)
                out.push_back(out[out.size() - distance]);
        }
    }
    return out.size() == expected;
}

bool extract_zip(const std::vector<uint8_t>& bytes,
                 const std::vector<ZipEntry>& entries,
                 const fs::path& target, std::string* error) {
    std::error_code ec;
    fs::create_directories(target, ec);
    if (ec) {
        set_error(error, "cannot create staging directory: " + ec.message());
        return false;
    }
    for (const ZipEntry& e : entries) {
        const fs::path out = target / fs::path(e.name);
        if (e.directory) {
            fs::create_directories(out, ec);
            if (ec) {
                set_error(error, "cannot create archive directory: " + ec.message());
                return false;
            }
            continue;
        }
        if ((uint64_t)e.local_offset + 30 > bytes.size() ||
            le32(bytes.data() + e.local_offset) != 0x04034b50u) {
            set_error(error, "ZIP local entry is invalid");
            return false;
        }
        const uint16_t name_len = le16(bytes.data() + e.local_offset + 26);
        const uint16_t extra_len = le16(bytes.data() + e.local_offset + 28);
        const uint64_t data_at = (uint64_t)e.local_offset + 30 + name_len + extra_len;
        if (data_at + e.compressed_size > bytes.size()) {
            set_error(error, "ZIP entry payload is invalid");
            return false;
        }
        const uint8_t* compressed = bytes.data() + data_at;
        std::vector<uint8_t> expanded;
        const uint8_t* data = compressed;
        if (e.method == 0) {
            if (e.compressed_size != e.size) {
                set_error(error, "stored ZIP entry has inconsistent size");
                return false;
            }
        } else {
            if (!inflate_deflate(compressed, e.compressed_size, e.size, expanded)) {
                set_error(error, "cannot inflate ZIP entry: " + e.name);
                return false;
            }
            data = expanded.data();
        }
        if (crc32_compute(data, e.size) != e.crc) {
            set_error(error, "ZIP entry checksum failed: " + e.name);
            return false;
        }
        fs::create_directories(out.parent_path(), ec);
        if (ec) {
            set_error(error, "cannot create archive parent directory: " + ec.message());
            return false;
        }
        std::ofstream file(out, std::ios::binary | std::ios::trunc);
        if (!file || (e.size && !file.write((const char*)data, e.size))) {
            set_error(error, "cannot extract archive entry: " + e.name);
            return false;
        }
    }
    return true;
}

const ModPackage* find_selected(
    const std::map<std::string, std::map<std::string, ModPackage>>& packages,
    const std::string& id, const ModSelection& selection) {
    const auto p = packages.find(id);
    if (p == packages.end() || p->second.empty()) return nullptr;
    if (!selection.version.empty()) {
        const auto v = p->second.find(selection.version);
        return v == p->second.end() ? nullptr : &v->second;
    }
    const ModPackage* best = nullptr;
    for (const auto& [version, package] : p->second)
        if (!best || compare_semver(version, best->version) > 0) best = &package;
    return best;
}

bool target_matches(const ModPackage& package, const std::string& game,
                    const std::string& exe, const std::string& disc) {
    if (package.targets.empty()) return false;
    for (const ModTarget& target : package.targets) {
        /* "*" targets every game. Framework-owned mods (loading speed, and
         * anything else that is a property of the emulator rather than of a
         * particular disc) ship once and apply everywhere, instead of every
         * title carrying a copy of the same manifest. An empty target list
         * still matches nothing, so a malformed manifest fails loudly. */
        if (target.game_id != "*" && target.game_id != game) continue;
        /* An UNKNOWN local hash is not a mismatch. A release install has no
         * loose PS-X EXE next to it (bundles never ship disc content), so
         * exe_sha256 is empty there — comparing a pin against "" rejected
         * every image-pinned package on exactly the installs players use,
         * while the same package applied fine in a dev checkout that happens
         * to have the file. Skip a pin we cannot evaluate and let the
         * expected-byte guard at patch time do the verifying, which is what
         * mod_runtime_initialize's own comment says is the fallback. A pin
         * we CAN evaluate is still enforced. */
        if (!target.exe_sha256.empty() && !exe.empty() &&
            target.exe_sha256 != exe)
            continue;
        if (!target.disc_sha256.empty() && !disc.empty() &&
            target.disc_sha256 != disc)
            continue;
        return true;
    }
    return false;
}

std::string canonical_resolution(const std::vector<const ModPackage*>& ordered,
                                 const std::map<std::string, ModSelection>& selections,
                                 const std::vector<ModResolution::Write>& writes,
                                 const std::vector<ModResolution::Overlay>& overlays,
                                 const std::vector<ModResolution::DerivedDisc>& derived_discs,
                                 const std::vector<ModResolution::Plugin>& plugins,
                                 const std::string& source_disc_sha256) {
    std::ostringstream out;
    out << "source_disc=" << source_disc_sha256 << '\n';
    for (const ModPackage* package : ordered) {
        out << package->id << '@' << package->version << '\n';
        const auto sit = selections.find(package->id);
        for (const ModFeature& feature : package->features) {
            bool enabled = feature.default_enabled;
            const std::map<std::string, std::string>* values = nullptr;
            if (sit != selections.end()) {
                if (feature.legacy) {
                    enabled = sit->second.enabled;
                    values = &sit->second.values;
                } else {
                    const auto selected = sit->second.features.find(feature.id);
                    if (selected != sit->second.features.end()) {
                        if (selected->second.has_enabled)
                            enabled = selected->second.enabled;
                        values = &selected->second.values;
                    }
                }
            }
            out << "feature:" << feature.id << '='
                << (enabled ? "enabled" : "disabled") << '\n';
            for (const ModOption& option : package->options) {
                if (option.feature_id != feature.id) continue;
                const auto selected = values ? values->find(option.id) :
                                               std::map<std::string, std::string>::const_iterator{};
                const std::string value =
                    values && selected != values->end()
                    ? selected->second : option.default_value;
                out << "feature:" << feature.id << ':' << option.id
                    << '=' << value << '\n';
            }
        }
    }
    for (const ModResolution::Write& write : writes) {
        const char* target =
            write.target == ModPatchTarget::MainExe ? "main_exe" :
            write.target == ModPatchTarget::DiscRaw ? "disc_raw" :
                                                       "disc_user";
        if (write.fields.empty()) {
            out << target << '@' << std::hex << write.location << std::dec
                << ':' << hex_bytes(write.expected) << '>'
                << hex_bytes(write.replacement) << ':' << write.package_id
                << ':' << write.feature_id << '\n';
        } else {
            out << "sparse_" << target << '@' << std::hex
                << write.location << std::dec << ":guard="
                << hex_bytes(write.expected) << ":owned=";
            for (const ModResolution::Write::Field& field : write.fields)
                out << std::hex << field.offset << std::dec << '='
                    << hex_bytes(field.replacement) << ',';
            out << ':' << write.package_id << ':' << write.feature_id
                << '\n';
        }
    }
    for (const ModResolution::Overlay& overlay : overlays) {
        out << (overlay.target == ModPatchTarget::DiscRaw
                    ? "disc_raw_overlay" : "disc_user_overlay")
            << '@' << std::hex << overlay.location << std::dec << ':'
            << overlay.payload_sha256 << ':' << overlay.expected_sha256 << ':'
            << overlay.package_id << ':' << overlay.feature_id << '\n';
    }
    for (const ModResolution::DerivedDisc& derived : derived_discs) {
        out << "derived_disc:" << derived.kind << ':'
            << derived.patch_sha256 << ':' << derived.output_size << ':'
            << derived.output_sha256 << ':' << derived.package_id << '\n';
    }
    for (const ModResolution::Plugin& plugin : plugins) {
        out << "plugin:" << plugin.id << ':' << plugin.package_id << ':'
            << plugin.feature_id << '\n';
    }
    return out.str();
}

const ModFeature* find_feature(const ModPackage& package, const std::string& id) {
    const auto found = std::find_if(package.features.begin(), package.features.end(),
        [&](const ModFeature& feature) { return feature.id == id; });
    return found == package.features.end() ? nullptr : &*found;
}

const ModOption* find_option(const ModPackage& package,
                             const std::string& feature_id,
                             const std::string& id) {
    const auto option = std::find_if(package.options.begin(), package.options.end(),
        [&](const ModOption& item) {
            return item.feature_id == feature_id && item.id == id;
        });
    return option == package.options.end() ? nullptr : &*option;
}

bool constraint_satisfied(
    const ModPackage& package, const ModConstraint& constraint,
    const std::function<std::string(const std::string&)>& value_for,
    std::string* reason);

const ModFeatureSelection* find_feature_selection(const ModPackage& package,
                                                  const ModSelection& selection,
                                                  const std::string& feature_id) {
    const ModFeature* feature = find_feature(package, feature_id);
    if (!feature) return nullptr;
    if (feature->legacy) return nullptr;
    const auto found = selection.features.find(feature_id);
    return found == selection.features.end() ? nullptr : &found->second;
}

bool is_feature_enabled(const ModPackage& package, const ModSelection& selection,
                        const ModFeature& feature) {
    if (feature.legacy) return selection.enabled;
    const ModFeatureSelection* selected =
        find_feature_selection(package, selection, feature.id);
    return selected && selected->has_enabled
        ? selected->enabled : feature.default_enabled;
}

bool has_enabled_feature(const ModPackage& package,
                         const ModSelection& selection) {
    return std::any_of(package.features.begin(), package.features.end(),
        [&](const ModFeature& feature) {
            return is_feature_enabled(package, selection, feature);
        });
}

std::string effective_option_value(const ModPackage& package,
                                   const ModSelection& selection,
                                   const std::string& feature_id,
                                   const std::string& id) {
    const ModFeature* feature = find_feature(package, feature_id);
    if (feature && feature->legacy) {
        const auto selected = selection.values.find(id);
        if (selected != selection.values.end()) return selected->second;
    } else {
        const ModFeatureSelection* selected =
            find_feature_selection(package, selection, feature_id);
        if (selected) {
            const auto value = selected->values.find(id);
            if (value != selected->values.end()) return value->second;
        }
    }
    const ModOption* option = find_option(package, feature_id, id);
    return option ? option->default_value : std::string();
}

bool prospective_feature_enabled(
    const ModPackage& package, const ModSelection& selection,
    const std::string& feature_id, const std::string* override_feature_id,
    const bool* override_enabled) {
    if (override_feature_id && override_enabled &&
        feature_id == *override_feature_id)
        return *override_enabled;
    const ModFeature* feature = find_feature(package, feature_id);
    return feature && is_feature_enabled(package, selection, *feature);
}

std::string prospective_option_value(
    const ModPackage& package, const ModSelection& selection,
    const std::string& feature_id, const std::string& option_id,
    const std::string* override_feature_id,
    const std::string* override_option_id,
    const std::string* override_value) {
    if (override_feature_id && override_option_id && override_value &&
        feature_id == *override_feature_id && option_id == *override_option_id)
        return *override_value;
    return effective_option_value(package, selection, feature_id, option_id);
}

bool runtime_constraint_satisfied(
    const ModPackage& package, const ModSelection& selection,
    const ModConstraint& constraint,
    const std::string* override_feature_id, const bool* override_enabled,
    const std::string* override_option_feature_id,
    const std::string* override_option_id,
    const std::string* override_value, std::string* reason) {
    if (!prospective_feature_enabled(
            package, selection, constraint.feature_id, override_feature_id,
            override_enabled))
        return true;
    if (constraint.kind == ModConstraintKind::OrderedInteger) {
        return constraint_satisfied(
            package, constraint,
            [&](const std::string& id) {
                return prospective_option_value(
                    package, selection, constraint.feature_id, id,
                    override_option_feature_id, override_option_id,
                    override_value);
            },
            reason);
    }
    if (constraint.kind == ModConstraintKind::RequiresFeature) {
        const ModFeature* required =
            find_feature(package, constraint.required_feature_id);
        const std::string required_name =
            required && !required->name.empty()
                ? required->name
                : constraint.required_feature_id;
        if (!prospective_feature_enabled(
                package, selection, constraint.required_feature_id,
                override_feature_id, override_enabled)) {
            if (reason) *reason = "requires " + required_name + " to be enabled";
            return false;
        }
        if (!constraint.required_option_id.empty()) {
            const std::string actual = prospective_option_value(
                package, selection, constraint.required_feature_id,
                constraint.required_option_id, override_option_feature_id,
                override_option_id, override_value);
            if (actual != constraint.required_value) {
                const ModOption* option = find_option(
                    package, constraint.required_feature_id,
                    constraint.required_option_id);
                if (reason) {
                    *reason =
                        "requires " + required_name + " " +
                        (option && !option->label.empty()
                             ? option->label
                             : constraint.required_option_id) +
                        " = " + constraint.required_value;
                }
                return false;
            }
        }
        return true;
    }
    if (reason) *reason = "unsupported constraint kind";
    return false;
}

bool package_constraints_satisfied(
    const ModPackage& package, const ModSelection& selection,
    const std::string* override_feature_id, const bool* override_enabled,
    const std::string* override_option_feature_id,
    const std::string* override_option_id,
    const std::string* override_value,
    std::string* failing_feature_id, std::string* reason) {
    for (const ModConstraint& constraint : package.constraints) {
        std::string local_reason;
        if (!runtime_constraint_satisfied(
                package, selection, constraint, override_feature_id,
                override_enabled, override_option_feature_id,
                override_option_id, override_value, &local_reason)) {
            if (failing_feature_id) *failing_feature_id = constraint.feature_id;
            if (reason) *reason = local_reason;
            return false;
        }
    }
    return true;
}

void set_feature_selected(ModSelection& selection,
                          const std::string& feature_id, bool enabled) {
    ModFeatureSelection& feature = selection.features[feature_id];
    feature.enabled = enabled;
    feature.has_enabled = true;
}

bool apply_feature_requirements(const ModPackage& package,
                                ModSelection& selection,
                                const std::string& feature_id,
                                std::set<std::string>& visiting,
                                std::string* error) {
    if (!visiting.insert(feature_id).second) return true;
    for (const ModConstraint& constraint : package.constraints) {
        if (constraint.kind != ModConstraintKind::RequiresFeature ||
            constraint.feature_id != feature_id)
            continue;
        const ModFeature* required =
            find_feature(package, constraint.required_feature_id);
        if (!required || required->legacy) {
            set_error(error, feature_id + ": has an invalid feature requirement");
            visiting.erase(feature_id);
            return false;
        }
        set_feature_selected(selection, constraint.required_feature_id, true);
        if (!constraint.required_option_id.empty()) {
            selection.features[constraint.required_feature_id]
                .values[constraint.required_option_id] =
                constraint.required_value;
        }
        if (!apply_feature_requirements(
                package, selection, constraint.required_feature_id,
                visiting, error)) {
            visiting.erase(feature_id);
            return false;
        }
    }
    visiting.erase(feature_id);
    return true;
}

void cascade_unsatisfied_feature_requirements(const ModPackage& package,
                                              ModSelection& selection) {
    bool changed = false;
    do {
        changed = false;
        for (const ModConstraint& constraint : package.constraints) {
            if (constraint.kind != ModConstraintKind::RequiresFeature)
                continue;
            const ModFeature* dependent =
                find_feature(package, constraint.feature_id);
            if (!dependent || dependent->legacy ||
                !is_feature_enabled(package, selection, *dependent))
                continue;
            if (runtime_constraint_satisfied(
                    package, selection, constraint, nullptr, nullptr,
                    nullptr, nullptr, nullptr, nullptr))
                continue;
            set_feature_selected(selection, constraint.feature_id, false);
            changed = true;
        }
    } while (changed);
}

bool conditions_match(const ModPackage& package, const ModSelection& selection,
                      const std::string& feature_id,
                      const std::map<std::string, std::string>& conditions) {
    for (const auto& [id, value] : conditions) {
        if (effective_option_value(package, selection, feature_id, id) != value)
            return false;
    }
    return true;
}

bool feature_predicate_matches(
    const ModOverlay::FeaturePredicate& predicate,
    const std::map<std::string, const ModPackage*>& active_packages,
    const std::map<std::string, ModSelection>& selections) {
    if (!predicate.present) return true;
    bool enabled = false;
    const auto package_it = active_packages.find(predicate.package_id);
    if (package_it != active_packages.end()) {
        const ModPackage& package = *package_it->second;
        const ModFeature* feature =
            find_feature(package, predicate.feature_id);
        if (feature) {
            const auto selection_it = selections.find(predicate.package_id);
            const ModSelection blank;
            const ModSelection& selection =
                selection_it == selections.end() ? blank : selection_it->second;
            enabled = is_feature_enabled(package, selection, *feature);
        }
    }
    return enabled == predicate.enabled;
}

bool valid_option_value(const ModOption& option, const std::string& value);

void read_conditions(const toml::value& value, const std::vector<ModOption>& options,
                     const std::string& feature_id,
                     std::map<std::string, std::string>& when,
                     const char* label) {
    const std::string when_option =
        value.contains("when_option") ? toml::find<std::string>(value, "when_option") : "";
    const std::string when_value =
        value.contains("when_value") ? toml::find<std::string>(value, "when_value") : "";
    if (when_option.empty() != when_value.empty())
        throw std::runtime_error(
            std::string(label) + " condition requires both when_option and when_value");
    if (!when_option.empty()) when[when_option] = when_value;
    if (value.contains("when")) {
        const auto table = toml::find<std::map<std::string, std::string>>(value, "when");
        for (const auto& [id, condition_value] : table) {
            if (when.find(id) != when.end() && when[id] != condition_value)
                throw std::runtime_error(
                    std::string(label) + " has conflicting conditions for " + id);
            when[id] = condition_value;
        }
    }
    /* Bind map entries to real locals before the lambda — capturing a
     * structured binding is a C++20 extension and breaks MinGW C++17 builds. */
    for (const auto& entry : when) {
        const std::string& id = entry.first;
        const std::string& condition_value = entry.second;
        const auto option = std::find_if(
            options.begin(), options.end(),
            [&](const ModOption& item) {
                return item.feature_id == feature_id && item.id == id;
            });
        if (option == options.end())
            throw std::runtime_error(std::string(label) + " references unknown option");
        if (!valid_option_value(*option, condition_value))
            throw std::runtime_error(
                std::string(label) + " condition has invalid option value");
    }
}

bool ranges_overlap(ModPatchTarget a_target, uint64_t a_location, size_t a_size,
                    ModPatchTarget b_target, uint64_t b_location, size_t b_size);

template <typename Fn>
void for_each_owned_span(const ModResolution::Write& write, Fn&& fn) {
    if (write.fields.empty()) {
        fn(write.location, write.expected.data(), write.replacement.data(),
           write.replacement.size());
        return;
    }
    for (const ModResolution::Write::Field& field : write.fields) {
        fn(write.location + field.offset,
           write.expected.data() + static_cast<size_t>(field.offset),
           field.replacement.data(), field.replacement.size());
    }
}

bool writes_overlap(const ModResolution::Write& a, const ModResolution::Write& b) {
    if (a.target != b.target) return false;
    bool overlap = false;
    for_each_owned_span(
        a, [&](uint64_t a_location, const uint8_t*, const uint8_t*,
               size_t a_size) {
            for_each_owned_span(
                b, [&](uint64_t b_location, const uint8_t*, const uint8_t*,
                       size_t b_size) {
                    overlap = overlap || ranges_overlap(
                        a.target, a_location, a_size,
                        b.target, b_location, b_size);
                });
        });
    return overlap;
}

bool ranges_overlap(ModPatchTarget a_target, uint64_t a_location, size_t a_size,
                    ModPatchTarget b_target, uint64_t b_location, size_t b_size) {
    if (a_target != b_target) return false;
    const uint64_t a_end = a_location + a_size;
    const uint64_t b_end = b_location + b_size;
    return a_location < b_end && b_location < a_end;
}

bool identical_write(const ModResolution::Write& a, const ModResolution::Write& b) {
    if (a.target != b.target || a.location != b.location ||
        a.expected != b.expected || a.replacement != b.replacement ||
        a.fields.size() != b.fields.size())
        return false;
    for (size_t i = 0; i < a.fields.size(); ++i) {
        if (a.fields[i].offset != b.fields[i].offset ||
            a.fields[i].replacement != b.fields[i].replacement)
            return false;
    }
    return true;
}

bool write_overlap_mismatch(const ModResolution::Write& a,
                            const ModResolution::Write& b,
                            uint64_t& mismatch) {
    bool found = false;
    for_each_owned_span(
        a, [&](uint64_t a_location, const uint8_t* a_expected,
               const uint8_t* a_replacement, size_t a_size) {
            for_each_owned_span(
                b, [&](uint64_t b_location, const uint8_t* b_expected,
                       const uint8_t* b_replacement, size_t b_size) {
                    const uint64_t begin =
                        std::max(a_location, b_location);
                    const uint64_t end = std::min(
                        a_location + a_size, b_location + b_size);
                    for (uint64_t at = begin; !found && at < end; ++at) {
                        const size_t ai =
                            static_cast<size_t>(at - a_location);
                        const size_t bi =
                            static_cast<size_t>(at - b_location);
                        if (a_expected[ai] != b_expected[bi] ||
                            a_replacement[ai] != b_replacement[bi]) {
                            mismatch = at;
                            found = true;
                        }
                    }
                });
        });
    return found;
}

bool write_guard_overlap_mismatch(const ModResolution::Write& a,
                                  const ModResolution::Write& b,
                                  uint64_t& mismatch) {
    if (a.target != b.target) return false;
    const uint64_t begin = std::max(a.location, b.location);
    const uint64_t end = std::min(
        a.location + a.expected.size(), b.location + b.expected.size());
    for (uint64_t at = begin; at < end; ++at) {
        if (a.expected[static_cast<size_t>(at - a.location)] !=
            b.expected[static_cast<size_t>(at - b.location)]) {
            mismatch = at;
            return true;
        }
    }
    return false;
}

bool write_overlaps_range(const ModResolution::Write& write,
                          ModPatchTarget target, uint64_t location,
                          size_t size) {
    if (write.target != target) return false;
    bool overlap = false;
    for_each_owned_span(
        write, [&](uint64_t owned_location, const uint8_t*,
                   const uint8_t*, size_t owned_size) {
            overlap = overlap || ranges_overlap(
                write.target, owned_location, owned_size,
                target, location, size);
        });
    return overlap;
}

bool write_overlay_mismatch(const ModResolution::Write& write,
                            const ModResolution::Overlay& overlay,
                            uint64_t& mismatch) {
    bool found = false;
    for_each_owned_span(
        write, [&](uint64_t location, const uint8_t*,
                   const uint8_t* replacement, size_t size) {
            const uint64_t begin = std::max(location, overlay.location);
            const uint64_t end = std::min(
                location + size, overlay.location + overlay.payload.size());
            for (uint64_t at = begin; !found && at < end; ++at) {
                if (replacement[static_cast<size_t>(at - location)] !=
                    overlay.payload[
                        static_cast<size_t>(at - overlay.location)]) {
                    mismatch = at;
                    found = true;
                }
            }
        });
    return found;
}

bool overlay_overlap_mismatch(const ModResolution::Overlay& a,
                              const ModResolution::Overlay& b,
                              uint64_t& mismatch) {
    const uint64_t begin = std::max(a.location, b.location);
    const uint64_t end = std::min(
        a.location + a.payload.size(), b.location + b.payload.size());
    for (uint64_t at = begin; at < end; ++at) {
        if (a.payload[(size_t)(at - a.location)] !=
            b.payload[(size_t)(at - b.location)]) {
            mismatch = at;
            return true;
        }
    }
    return false;
}

std::string overlap_resource(ModPatchTarget target,
                             uint64_t a_location, size_t a_size,
                             uint64_t b_location, size_t b_size) {
    const char* name = target == ModPatchTarget::MainExe ? "main_exe" :
                       target == ModPatchTarget::DiscRaw ? "disc_raw" :
                                                          "disc_user";
    const uint64_t begin = std::max(a_location, b_location);
    const uint64_t end = std::min(
        a_location + a_size, b_location + b_size);
    std::ostringstream out;
    out << name << ":0x" << std::hex << begin << "-0x" << end;
    return out.str();
}

std::string byte_resource(ModPatchTarget target, uint64_t location) {
    return overlap_resource(target, location, 1, location, 1);
}

bool parse_canonical_int64(const std::string& value, int64_t& parsed) {
    try {
        size_t used = 0;
        parsed = std::stoll(value, &used);
        return used == value.size() && std::to_string(parsed) == value;
    } catch (...) {
        return false;
    }
}

bool integer_step_aligned(int64_t value, int64_t minimum, int64_t step) {
    if (value < minimum || step <= 0) return false;
    const uint64_t distance =
        static_cast<uint64_t>(value) - static_cast<uint64_t>(minimum);
    return distance % static_cast<uint64_t>(step) == 0;
}

bool valid_option_value(const ModOption& option, const std::string& value) {
    if (option.type == ModOptionType::Boolean)
        return value == "true" || value == "false";
    if (option.type == ModOptionType::Choice)
        return std::any_of(option.choices.begin(), option.choices.end(),
            [&](const ModChoice& choice) { return choice.value == value; });
    int64_t parsed = 0;
    return parse_canonical_int64(value, parsed) &&
           parsed >= option.min_value && parsed <= option.max_value &&
           integer_step_aligned(parsed, option.min_value, option.step);
}

bool integer_predicate_matches(
    const ModPackage& package, const ModSelection& selection,
    const std::string& feature_id,
    const ModIntegerPredicate& predicate) {
    if (!predicate.present) return true;
    int64_t selected = 0;
    if (!parse_canonical_int64(
            effective_option_value(
                package, selection, feature_id, predicate.option),
            selected))
        return false;
    switch (predicate.op) {
    case ModIntegerPredicateOp::Equal:
        return selected == predicate.value;
    case ModIntegerPredicateOp::NotEqual:
        return selected != predicate.value;
    case ModIntegerPredicateOp::Less:
        return selected < predicate.value;
    case ModIntegerPredicateOp::LessEqual:
        return selected <= predicate.value;
    case ModIntegerPredicateOp::Greater:
        return selected > predicate.value;
    case ModIntegerPredicateOp::GreaterEqual:
        return selected >= predicate.value;
    }
    return false;
}

bool constraint_satisfied(
    const ModPackage& package, const ModConstraint& constraint,
    const std::function<std::string(const std::string&)>& value_for,
    std::string* reason) {
    if (constraint.kind != ModConstraintKind::OrderedInteger) return false;
    for (size_t index = 1; index < constraint.options.size(); ++index) {
        const std::string& previous_id = constraint.options[index - 1];
        const std::string& current_id = constraint.options[index];
        int64_t previous = 0;
        int64_t current = 0;
        if (!parse_canonical_int64(value_for(previous_id), previous) ||
            !parse_canonical_int64(value_for(current_id), current)) {
            if (reason)
                *reason = "constraint references a non-integer value";
            return false;
        }
        const bool violated =
            constraint.direction == ModConstraintDirection::Nondecreasing
                ? previous > current
                : previous < current;
        if (violated) {
            if (reason) {
                const ModOption* previous_option = find_option(
                    package, constraint.feature_id, previous_id);
                const ModOption* current_option = find_option(
                    package, constraint.feature_id, current_id);
                *reason =
                    (previous_option ? previous_option->label : previous_id) +
                    (constraint.direction ==
                             ModConstraintDirection::Nondecreasing
                         ? " must be less than or equal to "
                         : " must be greater than or equal to ") +
                    (current_option ? current_option->label : current_id);
            }
            return false;
        }
    }
    return true;
}

bool checked_add_int64(int64_t left, int64_t right, int64_t& out) {
    if ((right > 0 && left > std::numeric_limits<int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<int64_t>::min() - right))
        return false;
    out = left + right;
    return true;
}

uint64_t value_encoding_max(ModValueEncoding encoding) {
    switch (encoding) {
    case ModValueEncoding::U8:
        return UINT8_MAX;
    case ModValueEncoding::U16LE:
        return UINT16_MAX;
    case ModValueEncoding::U32LE:
        return UINT32_MAX;
    case ModValueEncoding::MipsLuiOriU32:
        return UINT32_MAX;
    }
    return 0;
}

bool option_range_fits_encoding(const ModOption& option,
                                ModValueEncoding encoding,
                                int64_t addend) {
    int64_t low = 0;
    int64_t high = 0;
    return checked_add_int64(option.min_value, addend, low) &&
           checked_add_int64(option.max_value, addend, high) &&
           low >= 0 &&
           static_cast<uint64_t>(high) <= value_encoding_max(encoding);
}

bool encode_unsigned_value(ModValueEncoding encoding, int64_t value,
                           std::vector<uint8_t>& out) {
    if (value < 0 ||
        static_cast<uint64_t>(value) > value_encoding_max(encoding))
        return false;
    const uint32_t size = value_encoding_size(encoding);
    out.resize(size);
    const uint64_t encoded = static_cast<uint64_t>(value);
    if (encoding == ModValueEncoding::MipsLuiOriU32) return false;
    for (uint32_t i = 0; i < size; ++i)
        out[i] = static_cast<uint8_t>(encoded >> (i * 8));
    return true;
}

std::string fingerprint_text(const std::string& text) {
    uint8_t digest[32];
    psx_sha256_compute((const uint8_t*)text.data(), text.size(), digest);
    std::ostringstream out;
    for (uint8_t byte : digest)
        out << std::hex << std::setw(2) << std::setfill('0') << (unsigned)byte;
    return out.str();
}

} // namespace

bool mod_register_builtin_resolver(const std::string& id, ModBuiltinResolver resolver) {
    if (!valid_id(id) || !resolver) return false;
    return builtin_resolvers().emplace(id, std::move(resolver)).second;
}

void mod_clear_builtin_resolvers_for_tests() {
    builtin_resolvers().clear();
}

bool mod_register_activation_plugin(const std::string& id, void (*callback)(void)) {
    if (!valid_id(id) || !callback) return false;
    RegisteredPlugin& plugin = registered_plugins()[id];
    if (plugin.activation) return false;
    plugin.activation = callback;
    return true;
}

bool mod_register_vblank_plugin(const std::string& id, void (*callback)(void)) {
    if (!valid_id(id) || !callback) return false;
    RegisteredPlugin& plugin = registered_plugins()[id];
    if (plugin.vblank) return false;
    plugin.vblank = callback;
    return true;
}

bool mod_register_function_entry_plugin_id(const std::string& id) {
    if (!valid_id(id)) return false;
    registered_plugins()[id].function_entry = true;
    return true;
}

bool mod_plugin_registered(const std::string& id) {
    const auto found = registered_plugins().find(id);
    return found != registered_plugins().end() &&
        (found->second.activation || found->second.vblank ||
         found->second.function_entry);
}

void mod_invoke_activation_plugin(const std::string& id) {
    const auto found = registered_plugins().find(id);
    if (found != registered_plugins().end() && found->second.activation)
        found->second.activation();
}

void mod_invoke_vblank_plugin(const std::string& id) {
    const auto found = registered_plugins().find(id);
    if (found != registered_plugins().end() && found->second.vblank)
        found->second.vblank();
}

void mod_clear_plugins_for_tests() {
    registered_plugins().clear();
}

ModPackageManager::ModPackageManager(fs::path mods_root) : root_(std::move(mods_root)) {}

void ModPackageManager::set_root(fs::path mods_root) {
    root_ = std::move(mods_root);
    packages_.clear();
    selections_.clear();
}

bool ModPackageManager::read_manifest(const fs::path& path, ModPackage& out,
                                      std::string* error) {
    try {
        const toml::value cfg = toml::parse(path.string());
        out = {};
        out.format_version = (uint32_t)toml::find<int64_t>(cfg, "format_version");
        out.id = toml::find<std::string>(cfg, "id");
        out.version = toml::find<std::string>(cfg, "version");
        out.name = toml::find<std::string>(cfg, "name");
        out.author = cfg.contains("author") ? toml::find<std::string>(cfg, "author") : "";
        if (cfg.contains("author_link")) {
            std::set<std::string> linked_authors;
            for (const toml::value& v : toml::find(cfg, "author_link").as_array()) {
                ModAuthorLink link;
                link.name = toml::find<std::string>(v, "name");
                link.url = toml::find<std::string>(v, "url");
                if (link.name.empty() || !linked_authors.insert(link.name).second)
                    throw std::runtime_error("empty or duplicate author link name");
                if (!valid_web_url(link.url))
                    throw std::runtime_error("author link URL must use http or https");
                out.author_links.push_back(std::move(link));
            }
        }
        out.description =
            cfg.contains("description") ? toml::find<std::string>(cfg, "description") : "";
        out.license = cfg.contains("license") ? toml::find<std::string>(cfg, "license") : "";
        out.source_name =
            cfg.contains("source_name") ? toml::find<std::string>(cfg, "source_name") : "";
        out.source_url =
            cfg.contains("source_url") ? toml::find<std::string>(cfg, "source_url") : "";
        if (!out.source_url.empty() && !valid_web_url(out.source_url))
            throw std::runtime_error("source URL must use http or https");
        if (!out.source_url.empty() && out.source_name.empty())
            out.source_name = "Project page";
        out.resolver =
            cfg.contains("resolver") ? toml::find<std::string>(cfg, "resolver") : "declarative";
        out.save_compatibility = cfg.contains("save_compatibility")
            ? toml::find<std::string>(cfg, "save_compatibility") : "shared";
        out.root = path.parent_path();
        if (out.format_version < kMinFormatVersion ||
            out.format_version > kMaxFormatVersion)
            throw std::runtime_error("unsupported format_version");
        if (!valid_id(out.id)) throw std::runtime_error("invalid package id");
        if (!parse_semver(out.version).valid) throw std::runtime_error("invalid semantic version");
        if (out.name.empty()) throw std::runtime_error("package name is empty");
        if (out.resolver != "declarative" && out.resolver.rfind("builtin:", 0) != 0)
            throw std::runtime_error("resolver must be declarative or builtin:<id>");
        if (out.save_compatibility != "shared" && out.save_compatibility != "isolated")
            throw std::runtime_error("save_compatibility must be shared or isolated");

        if (cfg.contains("target")) {
            for (const toml::value& v : toml::find(cfg, "target").as_array()) {
                ModTarget target;
                target.game_id = toml::find<std::string>(v, "game_id");
                target.exe_sha256 =
                    v.contains("exe_sha256") ? toml::find<std::string>(v, "exe_sha256") : "";
                target.disc_sha256 =
                    v.contains("disc_sha256") ? toml::find<std::string>(v, "disc_sha256") : "";
                if (target.game_id.empty()) throw std::runtime_error("target game_id is empty");
                out.targets.push_back(std::move(target));
            }
        }
        if (out.targets.empty()) throw std::runtime_error("package has no [[target]] entries");

        if (cfg.contains("dependency")) {
            for (const toml::value& v : toml::find(cfg, "dependency").as_array()) {
                ModRequirement dep;
                dep.id = toml::find<std::string>(v, "id");
                dep.version = v.contains("version") ? toml::find<std::string>(v, "version") : "*";
                if (!valid_id(dep.id)) throw std::runtime_error("invalid dependency id");
                out.dependencies.push_back(std::move(dep));
            }
        }
        if (cfg.contains("conflicts"))
            out.conflicts = toml::find<std::vector<std::string>>(cfg, "conflicts");
        for (const std::string& id : out.conflicts)
            if (!valid_id(id)) throw std::runtime_error("invalid conflict id");

        const bool feature_style = cfg.contains("feature");
        if (feature_style) {
            std::set<std::string> feature_ids;
            for (const toml::value& v : toml::find(cfg, "feature").as_array()) {
                ModFeature feature;
                feature.id = toml::find<std::string>(v, "id");
                feature.name = toml::find<std::string>(v, "name");
                feature.author = v.contains("author")
                    ? toml::find<std::string>(v, "author") : "";
                feature.description = v.contains("description")
                    ? toml::find<std::string>(v, "description") : "";
                feature.group = v.contains("group")
                    ? toml::find<std::string>(v, "group") : "General";
                feature.default_enabled =
                    toml::find_or<bool>(v, "default_enabled", false);
                if (!valid_id(feature.id) ||
                    !feature_ids.insert(feature.id).second)
                    throw std::runtime_error("invalid or duplicate feature id");
                if (feature.name.empty())
                    throw std::runtime_error("feature name is empty");
                out.features.push_back(std::move(feature));
            }
            if (out.features.empty())
                throw std::runtime_error("package has no [[feature]] entries");
        } else {
            ModFeature feature;
            feature.id = "legacy";
            feature.name = out.name;
            feature.author = out.author;
            feature.description = out.description;
            feature.legacy = true;
            out.features.push_back(std::move(feature));
        }

        if (cfg.contains("option")) {
            std::set<std::pair<std::string, std::string>> option_ids;
            for (const toml::value& v : toml::find(cfg, "option").as_array()) {
                ModOption option;
                option.feature_id = feature_style
                    ? toml::find<std::string>(v, "feature") : "legacy";
                option.id = toml::find<std::string>(v, "id");
                option.label = toml::find<std::string>(v, "label");
                option.description =
                    v.contains("description") ? toml::find<std::string>(v, "description") : "";
                option.group = v.contains("group") ? toml::find<std::string>(v, "group") : "General";
                option.disabled_by =
                    v.contains("disabled_by") ? toml::find<std::string>(v, "disabled_by") : "";
                const std::string type = toml::find<std::string>(v, "type");
                if (!find_feature(out, option.feature_id))
                    throw std::runtime_error("option references unknown feature");
                if (!valid_id(option.id) ||
                    !option_ids.insert({option.feature_id, option.id}).second)
                    throw std::runtime_error("invalid or duplicate option id");
                if (type == "boolean") {
                    option.type = ModOptionType::Boolean;
                    option.default_value = toml::find_or<std::string>(v, "default", "false");
                    if (option.default_value != "true" && option.default_value != "false")
                        throw std::runtime_error("boolean default must be true or false");
                } else if (type == "choice") {
                    option.type = ModOptionType::Choice;
                    option.default_value = toml::find<std::string>(v, "default");
                    for (const toml::value& c : toml::find(v, "choice").as_array()) {
                        ModChoice choice;
                        choice.value = toml::find<std::string>(c, "value");
                        choice.label = toml::find<std::string>(c, "label");
                        option.choices.push_back(std::move(choice));
                    }
                    const auto found = std::find_if(option.choices.begin(), option.choices.end(),
                        [&](const ModChoice& c) { return c.value == option.default_value; });
                    if (found == option.choices.end())
                        throw std::runtime_error("choice default is not declared");
                } else if (type == "integer") {
                    option.type = ModOptionType::Integer;
                    option.min_value = toml::find<int64_t>(v, "min");
                    option.max_value = toml::find<int64_t>(v, "max");
                    option.step = toml::find_or<int64_t>(v, "step", 1);
                    const int64_t def = toml::find<int64_t>(v, "default");
                    if (option.min_value > option.max_value || option.step <= 0 ||
                        def < option.min_value || def > option.max_value ||
                        !integer_step_aligned(
                            def, option.min_value, option.step))
                        throw std::runtime_error("invalid integer bounds/default");
                    option.default_value = std::to_string(def);
                } else {
                    throw std::runtime_error("unknown option type");
                }
                out.options.push_back(std::move(option));
            }
            /* Resolve disabled_by AFTER the whole list is read, so it may name
             * an option declared later. A dangling or non-boolean reference is
             * a manifest bug that would silently leave the control always
             * enabled, so reject it here rather than at render time. */
            for (const ModOption& option : out.options) {
                if (option.disabled_by.empty()) continue;
                if (option.disabled_by == option.id)
                    throw std::runtime_error("option disabled_by references itself");
                const auto owner = std::find_if(
                    out.options.begin(), out.options.end(),
                    [&](const ModOption& o) {
                        return o.feature_id == option.feature_id &&
                               o.id == option.disabled_by;
                    });
                if (owner == out.options.end() ||
                    owner->type != ModOptionType::Boolean)
                    throw std::runtime_error(
                        "option disabled_by must name a boolean option "
                        "in the same feature");
            }
        }
        if (cfg.contains("constraint")) {
            if (out.format_version < 3)
                throw std::runtime_error(
                    "constraints require format_version 3");
            for (const toml::value& v :
                 toml::find(cfg, "constraint").as_array()) {
                ModConstraint constraint;
                constraint.feature_id =
                    toml::find<std::string>(v, "feature");
                const std::string kind =
                    toml::find<std::string>(v, "kind");
                if (!find_feature(out, constraint.feature_id))
                    throw std::runtime_error(
                        "constraint references unknown feature");
                if (kind == "ordered_integer") {
                    const std::string direction =
                        toml::find<std::string>(v, "direction");
                    constraint.options =
                        toml::find<std::vector<std::string>>(v, "options");
                    if (direction == "nondecreasing") {
                        constraint.direction =
                            ModConstraintDirection::Nondecreasing;
                    } else if (direction == "nonincreasing") {
                        constraint.direction =
                            ModConstraintDirection::Nonincreasing;
                    } else {
                        throw std::runtime_error(
                            "ordered_integer direction must be "
                            "nondecreasing or nonincreasing");
                    }
                    if (constraint.options.size() < 2)
                        throw std::runtime_error(
                            "ordered_integer requires at least two options");
                    std::set<std::string> option_ids;
                    for (const std::string& id : constraint.options) {
                        const ModOption* option =
                            find_option(out, constraint.feature_id, id);
                        if (!option || option->type != ModOptionType::Integer)
                            throw std::runtime_error(
                                "ordered_integer must reference same-feature "
                                "integer options");
                        if (!option_ids.insert(id).second)
                            throw std::runtime_error(
                                "ordered_integer contains a duplicate option");
                    }
                    std::string reason;
                    if (!constraint_satisfied(
                            out, constraint,
                            [&](const std::string& id) {
                                return find_option(
                                    out, constraint.feature_id, id)
                                    ->default_value;
                            },
                            &reason))
                        throw std::runtime_error(
                            "constraint defaults are invalid: " + reason);
                } else if (kind == "requires_feature") {
                    if (out.format_version < 4)
                        throw std::runtime_error(
                            "requires_feature constraints require format_version 4");
                    constraint.kind = ModConstraintKind::RequiresFeature;
                    constraint.required_feature_id =
                        toml::find<std::string>(v, "requires_feature");
                    if (!find_feature(out, constraint.required_feature_id))
                        throw std::runtime_error(
                            "requires_feature references unknown feature");
                    constraint.required_option_id =
                        toml::find_or<std::string>(v, "requires_option", "");
                    constraint.required_value =
                        toml::find_or<std::string>(v, "requires_value", "");
                    if (constraint.required_option_id.empty() !=
                        constraint.required_value.empty())
                        throw std::runtime_error(
                            "requires_feature option constraint requires both "
                            "requires_option and requires_value");
                    if (!constraint.required_option_id.empty()) {
                        const ModOption* option = find_option(
                            out, constraint.required_feature_id,
                            constraint.required_option_id);
                        if (!option)
                            throw std::runtime_error(
                                "requires_feature references unknown option");
                        if (!valid_option_value(
                                *option, constraint.required_value))
                            throw std::runtime_error(
                                "requires_feature references invalid option value");
                    }
                } else {
                    throw std::runtime_error("unknown constraint kind");
                }
                out.constraints.push_back(std::move(constraint));
            }
        }
        if (cfg.contains("patch")) {
            size_t declaration_index = 0;
            for (const toml::value& v : toml::find(cfg, "patch").as_array()) {
                ModPatch patch;
                patch.feature_id = feature_style
                    ? toml::find<std::string>(v, "feature") : "legacy";
                if (!find_feature(out, patch.feature_id))
                    throw std::runtime_error("patch references unknown feature");
                const std::string target = toml::find<std::string>(v, "target");
                if (target == "main_exe") {
                    patch.target = ModPatchTarget::MainExe;
                    const int64_t address = toml::find<int64_t>(v, "address");
                    if (address < 0) throw std::runtime_error("patch address is negative");
                    patch.location = (uint64_t)address;
                } else if (target == "disc_raw" || target == "disc") {
                    patch.target = ModPatchTarget::DiscRaw;
                    const int64_t offset = toml::find<int64_t>(v, "offset");
                    if (offset < 0) throw std::runtime_error("patch offset is negative");
                    patch.location = (uint64_t)offset;
                } else if (target == "disc_user") {
                    patch.target = ModPatchTarget::DiscUser;
                    const int64_t offset = toml::find<int64_t>(v, "offset");
                    if (offset < 0) throw std::runtime_error("patch offset is negative");
                    patch.location = (uint64_t)offset;
                } else {
                    throw std::runtime_error(
                        "patch target must be main_exe, disc_raw, or disc_user");
                }
                const std::string expected = toml::find<std::string>(v, "expected");
                if (!parse_hex_bytes(expected, patch.expected) ||
                    patch.expected.empty())
                    throw std::runtime_error(
                        "patch expected must be non-empty hex");
                const bool has_static_replace = v.contains("replace");
                const bool has_dynamic_replace = v.contains("replace_from");
                const bool has_sparse_fields = v.contains("fields");
                const unsigned replacement_forms =
                    static_cast<unsigned>(has_static_replace) +
                    static_cast<unsigned>(has_dynamic_replace) +
                    static_cast<unsigned>(has_sparse_fields);
                if (replacement_forms != 1)
                    throw std::runtime_error(
                        "patch requires exactly one of replace, replace_from, "
                        "or fields");
                if (has_static_replace) {
                    const std::string replacement =
                        toml::find<std::string>(v, "replace");
                    if (!parse_hex_bytes(replacement, patch.replacement) ||
                        patch.expected.size() != patch.replacement.size())
                        throw std::runtime_error(
                            "patch expected/replace must be equal-length non-empty hex");
                } else if (has_dynamic_replace) {
                    if (out.format_version < 2)
                        throw std::runtime_error(
                            "replace_from requires format_version 2");
                    const toml::value& replacement =
                        toml::find(v, "replace_from");
                    const auto& table = replacement.as_table();
                    for (const auto& [key, unused] : table) {
                        (void)unused;
                        if (key != "option" && key != "encoding" &&
                            key != "offset" && key != "addend" &&
                            key != "omit_when_default")
                            throw std::runtime_error(
                                "replace_from has unknown field: " + key);
                    }
                    patch.replace_from_option =
                        toml::find<std::string>(replacement, "option");
                    const std::string encoding =
                        toml::find<std::string>(replacement, "encoding");
                    const int64_t replace_offset =
                        toml::find_or<int64_t>(replacement, "offset", 0);
                    if (replace_offset < 0)
                        throw std::runtime_error(
                            "replace_from offset must not be negative");
                    patch.replace_offset =
                        static_cast<uint64_t>(replace_offset);
                    patch.replace_addend =
                        toml::find_or<int64_t>(replacement, "addend", 0);
                    patch.replace_omit_when_default =
                        toml::find_or<bool>(
                            replacement, "omit_when_default", false);
                    if (!parse_value_encoding(
                            encoding, patch.replace_encoding))
                        throw std::runtime_error(
                            "replace_from encoding is unsupported");
                    const bool mips_pair_encoding =
                        patch.replace_encoding ==
                            ModValueEncoding::MipsLuiOriU32;
                    if ((mips_pair_encoding ||
                         patch.replace_omit_when_default) &&
                        out.format_version < 3)
                        throw std::runtime_error(
                            "typed MIPS encodings and omit_when_default "
                            "require format_version 3");
                    const ModOption* option = find_option(
                        out, patch.feature_id, patch.replace_from_option);
                    if (!option || option->type != ModOptionType::Integer)
                        throw std::runtime_error(
                            "replace_from must reference a same-feature integer option");
                    const uint64_t encoded_size =
                        value_encoding_size(patch.replace_encoding);
                    if (patch.replace_offset > patch.expected.size() ||
                        encoded_size >
                            patch.expected.size() - patch.replace_offset)
                        throw std::runtime_error(
                            "replace_from value exceeds expected byte range");
                    if (!option_range_fits_encoding(
                            *option, patch.replace_encoding,
                            patch.replace_addend))
                        throw std::runtime_error(
                            "replace_from option range/addend does not fit encoding");
                    if (mips_pair_encoding) {
                        if (patch.target != ModPatchTarget::MainExe ||
                            patch.expected.size() != 8 ||
                            patch.replace_offset != 0 ||
                            patch.replace_addend != 0 ||
                            patch.location % 4 != 0)
                            throw std::runtime_error(
                                "typed MIPS LUI/ORI encoding requires one "
                                "aligned complete main_exe instruction pair "
                                "at offset zero without an addend");
                        const uint32_t lui =
                            static_cast<uint32_t>(patch.expected[0]) |
                            (static_cast<uint32_t>(patch.expected[1]) << 8) |
                            (static_cast<uint32_t>(patch.expected[2]) << 16) |
                            (static_cast<uint32_t>(patch.expected[3]) << 24);
                        const uint32_t ori =
                            static_cast<uint32_t>(patch.expected[4]) |
                            (static_cast<uint32_t>(patch.expected[5]) << 8) |
                            (static_cast<uint32_t>(patch.expected[6]) << 16) |
                            (static_cast<uint32_t>(patch.expected[7]) << 24);
                        const uint32_t lui_rs = (lui >> 21) & 0x1Fu;
                        const uint32_t lui_rt = (lui >> 16) & 0x1Fu;
                        const uint32_t ori_rs = (ori >> 21) & 0x1Fu;
                        const uint32_t ori_rt = (ori >> 16) & 0x1Fu;
                        if ((lui >> 26) != 0x0Fu || lui_rs != 0 ||
                            lui_rt == 0 || (ori >> 26) != 0x0Du ||
                            ori_rs != lui_rt || ori_rt != lui_rt)
                            throw std::runtime_error(
                                "typed MIPS encoding guard is not a linked "
                                "LUI/ORI register pair");
                    }
                } else {
                    if (out.format_version < 4)
                        throw std::runtime_error(
                            "sparse patch fields require format_version 4");
                    const toml::value fields_value =
                        toml::find(v, "fields");
                    const toml::array& fields =
                        fields_value.as_array();
                    if (fields.empty())
                        throw std::runtime_error(
                            "sparse patch fields must not be empty");
                    std::vector<RelativeRange> claimed;
                    for (const toml::value& field_value : fields) {
                        const auto& table = field_value.as_table();
                        for (const auto& [key, unused] : table) {
                            (void)unused;
                            if (key != "offset" && key != "replace" &&
                                key != "option" && key != "encoding" &&
                                key != "addend")
                                throw std::runtime_error(
                                    "sparse patch field has unknown field: " +
                                    key);
                        }
                        ModPatchField field;
                        const int64_t field_offset =
                            toml::find_or<int64_t>(
                                field_value, "offset", 0);
                        if (field_offset < 0)
                            throw std::runtime_error(
                                "sparse patch field offset must not be "
                                "negative");
                        field.offset =
                            static_cast<uint64_t>(field_offset);
                        const bool literal =
                            field_value.contains("replace");
                        const bool dynamic =
                            field_value.contains("option") ||
                            field_value.contains("encoding") ||
                            field_value.contains("addend");
                        if (literal == dynamic)
                            throw std::runtime_error(
                                "sparse patch field requires exactly one of "
                                "literal replace or option encoding");
                        if (literal) {
                            if (!parse_hex_bytes(
                                    toml::find<std::string>(
                                        field_value, "replace"),
                                    field.replacement) ||
                                field.replacement.empty())
                                throw std::runtime_error(
                                    "sparse patch literal field must be "
                                    "non-empty hex");
                        } else {
                            if (!field_value.contains("option") ||
                                !field_value.contains("encoding"))
                                throw std::runtime_error(
                                    "sparse dynamic field requires option "
                                    "and encoding");
                            field.replace_from_option =
                                toml::find<std::string>(
                                    field_value, "option");
                            if (!parse_value_encoding(
                                    toml::find<std::string>(
                                        field_value, "encoding"),
                                    field.replace_encoding))
                                throw std::runtime_error(
                                    "sparse dynamic field encoding is "
                                    "unsupported");
                            field.replace_addend =
                                toml::find_or<int64_t>(
                                    field_value, "addend", 0);
                            const ModOption* option = find_option(
                                out, patch.feature_id,
                                field.replace_from_option);
                            if (!option ||
                                option->type != ModOptionType::Integer)
                                throw std::runtime_error(
                                    "sparse dynamic field must reference a "
                                    "same-feature integer option");
                            if (!option_range_fits_encoding(
                                    *option, field.replace_encoding,
                                    field.replace_addend))
                                throw std::runtime_error(
                                    "sparse dynamic field option range/"
                                    "addend does not fit encoding");
                            if (field.replace_encoding ==
                                ModValueEncoding::MipsLuiOriU32) {
                                if (patch.target !=
                                        ModPatchTarget::MainExe ||
                                    field.replace_addend != 0 ||
                                    (patch.location + field.offset) % 4 != 0 ||
                                    !valid_mips_lui_ori_guard(
                                        patch.expected, field.offset))
                                    throw std::runtime_error(
                                        "sparse typed MIPS field requires an "
                                        "aligned linked main_exe LUI/ORI "
                                        "guard without an addend");
                            }
                        }
                        const uint64_t guard_span =
                            field.replacement.empty()
                                ? value_encoding_size(
                                      field.replace_encoding)
                                : field.replacement.size();
                        if (field.offset > patch.expected.size() ||
                            guard_span >
                                patch.expected.size() - field.offset)
                            throw std::runtime_error(
                                "sparse patch field exceeds expected guard");
                        for (const RelativeRange& range :
                             patch_field_ranges(field)) {
                            for (const RelativeRange& previous : claimed) {
                                if (range.offset <
                                        previous.offset + previous.size &&
                                    previous.offset <
                                        range.offset + range.size)
                                    throw std::runtime_error(
                                        "sparse patch fields overlap");
                            }
                            claimed.push_back(range);
                        }
                        patch.fields.push_back(std::move(field));
                    }
                }
                const uint64_t sector_size =
                    patch.target == ModPatchTarget::DiscRaw ? 2352 :
                    patch.target == ModPatchTarget::DiscUser ? 2048 : 0;
                if (sector_size != 0 &&
                    patch.location % sector_size + patch.expected.size() > sector_size)
                    throw std::runtime_error(
                        "disc patch may not cross a sector boundary");
                if (patch.target == ModPatchTarget::MainExe &&
                    (patch.location > UINT32_MAX ||
                     patch.expected.size() >
                         static_cast<uint64_t>(UINT32_MAX) + 1 -
                             patch.location))
                    throw std::runtime_error(
                        "main_exe patch exceeds 32-bit guest address space");
                if (sector_size != 0 &&
                    patch.location / sector_size > UINT32_MAX)
                    throw std::runtime_error(
                        "disc patch LBA exceeds runtime index range");
                patch.order = toml::find_or<int64_t>(
                    v, "order", (int64_t)declaration_index);
                read_conditions(v, out.options, patch.feature_id, patch.when, "patch");
                if (!patch.when.empty()) {
                    patch.when_option = patch.when.begin()->first;
                    patch.when_value = patch.when.begin()->second;
                }
                if (v.contains("when_integer")) {
                    if (out.format_version < 4)
                        throw std::runtime_error(
                            "when_integer requires format_version 4");
                    const toml::value predicate =
                        toml::find(v, "when_integer");
                    const auto& table = predicate.as_table();
                    for (const auto& [key, unused] : table) {
                        (void)unused;
                        if (key != "option" && key != "op" &&
                            key != "value")
                            throw std::runtime_error(
                                "when_integer has unknown field: " + key);
                    }
                    patch.when_integer.present = true;
                    patch.when_integer.option =
                        toml::find<std::string>(predicate, "option");
                    if (!parse_integer_predicate_op(
                            toml::find<std::string>(predicate, "op"),
                            patch.when_integer.op))
                        throw std::runtime_error(
                            "when_integer op must be eq, ne, lt, le, gt, "
                            "or ge");
                    patch.when_integer.value =
                        toml::find<int64_t>(predicate, "value");
                    const ModOption* option = find_option(
                        out, patch.feature_id,
                        patch.when_integer.option);
                    if (!option ||
                        option->type != ModOptionType::Integer)
                        throw std::runtime_error(
                            "when_integer must reference a same-feature "
                            "integer option");
                    if (patch.when_integer.value < option->min_value ||
                        patch.when_integer.value > option->max_value)
                        throw std::runtime_error(
                            "when_integer value is outside option bounds");
                    if ((patch.when_integer.op ==
                             ModIntegerPredicateOp::Equal ||
                         patch.when_integer.op ==
                             ModIntegerPredicateOp::NotEqual) &&
                        !integer_step_aligned(
                            patch.when_integer.value,
                            option->min_value, option->step))
                        throw std::runtime_error(
                            "when_integer equality value is not selectable");
                }
                out.patches.push_back(std::move(patch));
                ++declaration_index;
            }
        }
        if (cfg.contains("overlay")) {
            if (!feature_style)
                throw std::runtime_error(
                    "disc overlays require explicit [[feature]] ownership");
            size_t declaration_index = 0;
            for (const toml::value& v : toml::find(cfg, "overlay").as_array()) {
                ModOverlay overlay;
                overlay.feature_id = toml::find<std::string>(v, "feature");
                if (!find_feature(out, overlay.feature_id))
                    throw std::runtime_error("overlay references unknown feature");
                const std::string target = toml::find<std::string>(v, "target");
                if (target == "disc_raw")
                    overlay.target = ModPatchTarget::DiscRaw;
                else if (target == "disc_user")
                    overlay.target = ModPatchTarget::DiscUser;
                else
                    throw std::runtime_error(
                        "overlay target must be disc_raw or disc_user");
                const int64_t offset = toml::find<int64_t>(v, "offset");
                if (offset < 0)
                    throw std::runtime_error("overlay offset is negative");
                overlay.location = (uint64_t)offset;
                const std::string relative_file =
                    toml::find<std::string>(v, "file");
                if (!safe_archive_name(relative_file))
                    throw std::runtime_error("overlay file path is unsafe");
                overlay.file = out.root / fs::path(relative_file);
                overlay.sha256 = toml::find<std::string>(v, "sha256");
                overlay.expected_sha256 = v.contains("expected_sha256")
                    ? toml::find<std::string>(v, "expected_sha256") : "";
                if (!valid_sha256(overlay.sha256) ||
                    (!overlay.expected_sha256.empty() &&
                     !valid_sha256(overlay.expected_sha256)))
                    throw std::runtime_error(
                        "overlay hashes must be lowercase SHA-256");
                std::string file_error;
                std::vector<uint8_t> payload;
                if (!read_file(overlay.file, payload, &file_error))
                    throw std::runtime_error(file_error);
                if (payload.empty())
                    throw std::runtime_error("overlay payload is empty");
                const std::string actual = fingerprint_text(std::string(
                    (const char*)payload.data(), payload.size()));
                if (actual != overlay.sha256)
                    throw std::runtime_error("overlay payload checksum failed");
                overlay.size = payload.size();
                if (overlay.location >
                    std::numeric_limits<uint64_t>::max() - overlay.size)
                    throw std::runtime_error("overlay range overflows");
                overlay.order = toml::find_or<int64_t>(
                    v, "order", (int64_t)declaration_index);
                read_conditions(v, out.options, overlay.feature_id,
                                overlay.when, "overlay");
                if (v.contains("when_feature")) {
                    if (out.format_version < 4)
                        throw std::runtime_error(
                            "overlay when_feature requires format_version 4");
                    const toml::value& predicate =
                        toml::find(v, "when_feature");
                    for (const auto& [key, unused] : predicate.as_table()) {
                        (void)unused;
                        if (key != "package" && key != "feature" &&
                            key != "enabled")
                            throw std::runtime_error(
                                "overlay when_feature has unknown field: " +
                                key);
                    }
                    overlay.when_feature.present = true;
                    overlay.when_feature.package_id =
                        toml::find<std::string>(predicate, "package");
                    overlay.when_feature.feature_id =
                        toml::find<std::string>(predicate, "feature");
                    overlay.when_feature.enabled =
                        toml::find<bool>(predicate, "enabled");
                    if (!valid_id(overlay.when_feature.package_id) ||
                        !valid_id(overlay.when_feature.feature_id))
                        throw std::runtime_error(
                            "overlay when_feature references an invalid id");
                }
                out.overlays.push_back(std::move(overlay));
                ++declaration_index;
            }
            const bool guarded_stock = std::all_of(
                out.targets.begin(), out.targets.end(),
                [](const ModTarget& target) {
                    return valid_sha256(target.disc_sha256);
                });
            if (!out.overlays.empty() && !guarded_stock)
                throw std::runtime_error(
                    "feature disc overlays require an exact disc_sha256 "
                    "on every [[target]]");
        }
        if (cfg.contains("plugin")) {
            if (!feature_style)
                throw std::runtime_error(
                    "plugins require explicit [[feature]] ownership");
            if (out.format_version < 5)
                throw std::runtime_error(
                    "plugins require format_version 5");
            size_t declaration_index = 0;
            for (const toml::value& v :
                 toml::find(cfg, "plugin").as_array()) {
                ModPlugin plugin;
                plugin.feature_id =
                    toml::find<std::string>(v, "feature");
                if (!find_feature(out, plugin.feature_id))
                    throw std::runtime_error(
                        "plugin references unknown feature");
                plugin.id = toml::find<std::string>(v, "id");
                if (!valid_id(plugin.id))
                    throw std::runtime_error("invalid plugin id");
                plugin.order = toml::find_or<int64_t>(
                    v, "order", (int64_t)declaration_index);
                read_conditions(v, out.options, plugin.feature_id,
                                plugin.when, "plugin");
                out.plugins.push_back(std::move(plugin));
                ++declaration_index;
            }
        }
        if (cfg.contains("derived_disc")) {
            if (feature_style)
                throw std::runtime_error(
                    "derived_disc is a legacy conversion artifact and may not "
                    "be used by feature-style packages");
            for (const toml::value& v : toml::find(cfg, "derived_disc").as_array()) {
                ModDerivedDisc derived;
                derived.kind = toml::find_or<std::string>(v, "kind", "vcdiff");
                const std::string relative_patch = toml::find<std::string>(v, "patch");
                derived.patch_sha256 = toml::find<std::string>(v, "patch_sha256");
                const int64_t output_size = toml::find<int64_t>(v, "output_size");
                derived.output_sha256 = toml::find<std::string>(v, "output_sha256");
                if (derived.kind != "vcdiff")
                    throw std::runtime_error("derived_disc kind must be vcdiff");
                if (!safe_archive_name(relative_patch))
                    throw std::runtime_error("derived_disc patch path is unsafe");
                if (!valid_sha256(derived.patch_sha256) ||
                    !valid_sha256(derived.output_sha256))
                    throw std::runtime_error(
                        "derived_disc hashes must be lowercase SHA-256");
                if (output_size <= 0)
                    throw std::runtime_error("derived_disc output_size must be positive");
                derived.output_size = (uint64_t)output_size;
                read_conditions(v, out.options, "legacy",
                                derived.when, "derived_disc");
                if (!derived.when.empty()) {
                    derived.when_option = derived.when.begin()->first;
                    derived.when_value = derived.when.begin()->second;
                }
                derived.patch = out.root / fs::path(relative_patch);
                if (!fs::is_regular_file(derived.patch))
                    throw std::runtime_error(
                        "derived_disc patch asset is missing: " + relative_patch);
                out.derived_discs.push_back(std::move(derived));
            }
        }
        return true;
    } catch (const std::exception& ex) {
        set_error(error, path.string() + ": " + ex.what());
        return false;
    }
}

bool ModPackageManager::scan(std::string* error) {
    packages_.clear();
    std::error_code ec;
    const fs::path packages_root = root_ / "packages";
    if (!fs::exists(packages_root, ec)) return true;
    for (const fs::directory_entry& id_dir : fs::directory_iterator(packages_root, ec)) {
        if (ec) break;
        if (!id_dir.is_directory()) continue;
        for (const fs::directory_entry& version_dir : fs::directory_iterator(id_dir.path(), ec)) {
            if (ec) break;
            if (!version_dir.is_directory()) continue;
            const fs::path manifest = version_dir.path() / "manifest.toml";
            if (!fs::exists(manifest)) continue;
            ModPackage package;
            std::string parse_error;
            if (!read_manifest(manifest, package, &parse_error)) {
                continue;
            }
            if (package.id != id_dir.path().filename().string() ||
                package.version != version_dir.path().filename().string()) {
                set_error(error, "package path does not match manifest id/version: " +
                                 manifest.string());
                return false;
            }
            packages_[package.id][package.version] = std::move(package);
        }
    }
    if (ec) {
        set_error(error, "cannot scan packages: " + ec.message());
        return false;
    }
    return true;
}

bool ModPackageManager::load_state(std::string* error) {
    selections_.clear();
    const fs::path path = root_ / "state.toml";
    if (!fs::exists(path)) return true;
    try {
        const toml::value cfg = toml::parse(path.string());
        const int64_t version = toml::find<int64_t>(cfg, "format_version");
        if (version != 1 && version != 2)
            throw std::runtime_error("unsupported state format_version");
        if (cfg.contains("package")) {
            for (const toml::value& v : toml::find(cfg, "package").as_array()) {
                const std::string id = toml::find<std::string>(v, "id");
                if (!valid_id(id)) throw std::runtime_error("invalid state package id");
                ModSelection selection;
                selection.enabled = toml::find_or<bool>(v, "enabled", false);
                selection.version = toml::find_or<std::string>(v, "version", "");
                if (v.contains("values")) {
                    for (const auto& [key, value] : toml::find(v, "values").as_table()) {
                        if (value.is_string())
                            selection.values[key] = toml::get<std::string>(value);
                        else if (value.is_boolean())
                            selection.values[key] =
                                toml::get<bool>(value) ? "true" : "false";
                        else if (value.is_integer())
                            selection.values[key] =
                                std::to_string(toml::get<int64_t>(value));
                        else
                            throw std::runtime_error(
                                "state option values must be scalar");
                    }
                }
                selections_[id] = std::move(selection);
            }
        }
        if (version == 2 && cfg.contains("feature")) {
            for (const toml::value& v : toml::find(cfg, "feature").as_array()) {
                const std::string package_id =
                    toml::find<std::string>(v, "package_id");
                const std::string feature_id = toml::find<std::string>(v, "id");
                if (!valid_id(package_id) || !valid_id(feature_id))
                    throw std::runtime_error("invalid state feature identity");
                ModFeatureSelection feature;
                feature.enabled = toml::find<bool>(v, "enabled");
                feature.has_enabled = true;
                if (v.contains("values")) {
                    for (const auto& [key, value] :
                         toml::find(v, "values").as_table()) {
                        if (value.is_string())
                            feature.values[key] = toml::get<std::string>(value);
                        else if (value.is_boolean())
                            feature.values[key] =
                                toml::get<bool>(value) ? "true" : "false";
                        else if (value.is_integer())
                            feature.values[key] =
                                std::to_string(toml::get<int64_t>(value));
                        else
                            throw std::runtime_error(
                                "state feature option values must be scalar");
                    }
                }
                selections_[package_id].features[feature_id] =
                    std::move(feature);
            }
        }
        return true;
    } catch (const std::exception& ex) {
        set_error(error, path.string() + ": " + ex.what());
        return false;
    }
}

bool ModPackageManager::save_state(std::string* error) const {
    std::error_code ec;
    fs::create_directories(root_, ec);
    if (ec) {
        set_error(error, "cannot create mods directory: " + ec.message());
        return false;
    }
    const fs::path temp = root_ / "state.toml.tmp";
    const fs::path final = root_ / "state.toml";
    std::ofstream out(temp, std::ios::trunc);
    if (!out) {
        set_error(error, "cannot write " + temp.string());
        return false;
    }
    out << "format_version = 2\n";
    for (const auto& [id, selection] : selections_) {
        out << "\n[[package]]\n";
        out << "id = " << quote_toml(id) << "\n";
        if (!selection.version.empty())
            out << "version = " << quote_toml(selection.version) << "\n";
        const ModPackage* package = find_selected(packages_, id, selection);
        const bool legacy = package && package->features.size() == 1 &&
                            package->features.front().legacy;
        if (legacy) {
            out << "enabled = " << (selection.enabled ? "true" : "false") << "\n";
        }
        if (legacy && !selection.values.empty()) {
            out << "[package.values]\n";
            for (const auto& [key, value] : selection.values)
                out << key << " = " << quote_toml(value) << "\n";
        }
    }
    for (const auto& [package_id, selection] : selections_) {
        for (const auto& [feature_id, feature] : selection.features) {
            bool enabled = feature.enabled;
            if (!feature.has_enabled) {
                const ModPackage* package =
                    find_selected(packages_, package_id, selection);
                const ModFeature* manifest_feature =
                    package ? find_feature(*package, feature_id) : nullptr;
                if (manifest_feature)
                    enabled = manifest_feature->default_enabled;
            }
            out << "\n[[feature]]\n";
            out << "package_id = " << quote_toml(package_id) << "\n";
            out << "id = " << quote_toml(feature_id) << "\n";
            out << "enabled = " << (enabled ? "true" : "false") << "\n";
            if (!feature.values.empty()) {
                out << "[feature.values]\n";
                for (const auto& [key, value] : feature.values)
                    out << key << " = " << quote_toml(value) << "\n";
            }
        }
    }
    out.close();
    if (!out) {
        set_error(error, "cannot finish " + temp.string());
        return false;
    }
    fs::rename(temp, final, ec);
    if (ec) {
        fs::remove(final, ec);
        ec.clear();
        fs::rename(temp, final, ec);
    }
    if (ec) {
        set_error(error, "cannot publish state: " + ec.message());
        return false;
    }
    return true;
}

bool ModPackageManager::install_archive(const fs::path& archive,
                                        std::string* installed_id,
                                        std::string* installed_version,
                                        std::string* error) {
    std::vector<uint8_t> bytes;
    if (!read_file(archive, bytes, error)) return false;
    return install_archive_bytes(bytes.data(), bytes.size(), installed_id,
                                 installed_version, error);
}

bool ModPackageManager::install_archive_bytes(const uint8_t* data, size_t size,
                                              std::string* installed_id,
                                              std::string* installed_version,
                                              std::string* error) {
    if (!data || size == 0) {
        set_error(error, "empty archive");
        return false;
    }
    std::vector<uint8_t> bytes(data, data + size);
    std::vector<ZipEntry> entries;
    if (!parse_zip(bytes, entries, error))
        return false;
    const auto manifest_entry = std::find_if(entries.begin(), entries.end(),
        [](const ZipEntry& e) { return e.name == "manifest.toml" && !e.directory; });
    if (manifest_entry == entries.end()) {
        set_error(error, "archive root does not contain manifest.toml");
        return false;
    }

    std::error_code ec;
    fs::create_directories(root_ / ".staging", ec);
    if (ec) {
        set_error(error, "cannot create install staging root: " + ec.message());
        return false;
    }
    const std::string token =
        std::to_string((unsigned long long)crc32_compute(bytes.data(), bytes.size()));
    const fs::path staging = root_ / ".staging" / ("install-" + token);
    if (fs::exists(staging)) {
        set_error(error, "install staging path already exists; remove " + staging.string());
        return false;
    }
    if (!extract_zip(bytes, entries, staging, error)) {
        fs::remove_all(staging, ec);
        return false;
    }
    ModPackage package;
    if (!read_manifest(staging / "manifest.toml", package, error)) {
        fs::remove_all(staging, ec);
        return false;
    }
    const fs::path destination = root_ / "packages" / package.id / package.version;
    if (fs::exists(destination)) {
        fs::remove_all(staging, ec);
        set_error(error, "package version is already installed");
        return false;
    }
    fs::create_directories(destination.parent_path(), ec);
    if (ec) {
        fs::remove_all(staging, ec);
        set_error(error, "cannot create package directory: " + ec.message());
        return false;
    }
    fs::rename(staging, destination, ec);
    if (ec) {
        fs::remove_all(staging, ec);
        set_error(error, "cannot publish installed package: " + ec.message());
        return false;
    }
    package.root = destination;
    packages_[package.id][package.version] = package;
    if (installed_id) *installed_id = package.id;
    if (installed_version) *installed_version = package.version;
    return true;
}

bool ModPackageManager::export_archive(const std::string& id, const std::string& version,
                                       std::vector<uint8_t>& out,
                                       std::string* error) const {
    const auto pit = packages_.find(id);
    if (pit == packages_.end()) {
        set_error(error, "package is not installed");
        return false;
    }
    const auto vit = pit->second.find(version);
    if (vit == pit->second.end()) {
        set_error(error, "package version is not installed");
        return false;
    }
    return write_store_zip(vit->second.root, out, error);
}

bool ModPackageManager::remove_version(const std::string& id, const std::string& version,
                                       std::string* error) {
    const auto sit = selections_.find(id);
    const ModSelection blank;
    const ModSelection& current =
        sit == selections_.end() ? blank : sit->second;
    const ModPackage* selected = find_selected(packages_, id, current);
    if (selected && selected->version == version &&
        has_enabled_feature(*selected, current)) {
        set_error(error, "cannot remove an active package version");
        return false;
    }
    for (const auto& [other_id, versions] : packages_) {
        (void)versions;
        const auto other_selection = selections_.find(other_id);
        const ModSelection& selection =
            other_selection == selections_.end() ? blank :
                                                   other_selection->second;
        const ModPackage* package =
            find_selected(packages_, other_id, selection);
        if (!package || other_id == id ||
            !has_enabled_feature(*package, selection)) continue;
        for (const ModRequirement& dep : package->dependencies) {
            if (dep.id == id && version_satisfies(version, dep.version)) {
                set_error(error, "cannot remove a version required by " + other_id);
                return false;
            }
        }
    }
    const auto pit = packages_.find(id);
    if (pit == packages_.end() || pit->second.find(version) == pit->second.end()) {
        set_error(error, "package version is not installed");
        return false;
    }
    const fs::path path = pit->second.at(version).root;
    std::error_code ec;
    fs::remove_all(path, ec);
    if (ec) {
        set_error(error, "cannot remove package version: " + ec.message());
        return false;
    }
    packages_[id].erase(version);
    if (packages_[id].empty()) packages_.erase(id);
    return true;
}

bool ModPackageManager::set_enabled(const std::string& id, bool enabled, std::string* error) {
    const auto pit = packages_.find(id);
    if (pit == packages_.end()) {
        set_error(error, "package is not installed");
        return false;
    }
    const ModPackage* package = selected_package(id);
    if (!package) {
        set_error(error, "package/version is not installed");
        return false;
    }
    if (package->features.size() != 1 || !package->features.front().legacy) {
        set_error(error,
            "feature-style packages must be enabled per feature");
        return false;
    }
    selections_[id].enabled = enabled;
    return true;
}

bool ModPackageManager::select_version(const std::string& id, const std::string& version,
                                       std::string* error) {
    const auto pit = packages_.find(id);
    if (pit == packages_.end() || pit->second.find(version) == pit->second.end()) {
        set_error(error, "package version is not installed");
        return false;
    }
    selections_[id].version = version;
    return true;
}

bool ModPackageManager::set_option(const std::string& id, const std::string& option_id,
                                   const std::string& value, std::string* error) {
    const auto sit = selections_.find(id);
    const ModSelection empty;
    const ModSelection& selection = sit == selections_.end() ? empty : sit->second;
    const ModPackage* package = find_selected(packages_, id, selection);
    if (!package) {
        set_error(error, "package/version is not installed");
        return false;
    }
    if (package->features.size() != 1 || !package->features.front().legacy) {
        set_error(error,
            "feature-style package options must name a feature");
        return false;
    }
    const auto oit = std::find_if(package->options.begin(), package->options.end(),
        [&](const ModOption& option) {
            return option.feature_id == "legacy" && option.id == option_id;
        });
    if (oit == package->options.end()) {
        set_error(error, "unknown package option");
        return false;
    }
    if (!valid_option_value(*oit, value)) {
        set_error(error, "invalid option value");
        return false;
    }
    selections_[id].values[option_id] = value;
    return true;
}

bool ModPackageManager::set_feature_enabled(const std::string& package_id,
                                            const std::string& feature_id,
                                            bool enabled,
                                            std::string* error) {
    const ModPackage* package = selected_package(package_id);
    const ModFeature* feature =
        package ? find_feature(*package, feature_id) : nullptr;
    if (!feature || feature->legacy) {
        set_error(error, "unknown package feature");
        return false;
    }
    ModSelection package_selection = selections_[package_id];
    if (enabled) {
        std::set<std::string> visiting;
        if (!apply_feature_requirements(
                *package, package_selection, feature_id, visiting, error))
            return false;
    }
    set_feature_selected(package_selection, feature_id, enabled);
    if (!enabled)
        cascade_unsatisfied_feature_requirements(*package, package_selection);
    std::string failing_feature;
    std::string reason;
    if (!package_constraints_satisfied(
            *package, package_selection, nullptr, nullptr, nullptr, nullptr,
            nullptr, &failing_feature, &reason)) {
        set_error(error, package_id + "/" +
            (failing_feature.empty() ? feature_id : failing_feature) +
            ": " + reason);
        return false;
    }
    selections_[package_id] = std::move(package_selection);
    return true;
}

bool ModPackageManager::set_feature_option(const std::string& package_id,
                                           const std::string& feature_id,
                                           const std::string& option_id,
                                           const std::string& value,
                                           std::string* error) {
    const ModPackage* package = selected_package(package_id);
    const ModFeature* feature =
        package ? find_feature(*package, feature_id) : nullptr;
    const ModOption* option =
        package ? find_option(*package, feature_id, option_id) : nullptr;
    if (!feature || feature->legacy || !option) {
        set_error(error, "unknown feature option");
        return false;
    }
    if (!valid_option_value(*option, value)) {
        set_error(error, "invalid feature option value");
        return false;
    }
    ModSelection package_selection = selections_[package_id];
    package_selection.features[feature_id].values[option_id] = value;
    cascade_unsatisfied_feature_requirements(*package, package_selection);
    std::string failing_feature;
    std::string reason;
    if (!package_constraints_satisfied(
            *package, package_selection, nullptr, nullptr, nullptr, nullptr,
            nullptr, &failing_feature, &reason)) {
        set_error(error, package_id + "/" +
            (failing_feature.empty() ? feature_id : failing_feature) +
            ": " + reason);
        return false;
    }
    selections_[package_id] = std::move(package_selection);
    return true;
}

const ModPackage* ModPackageManager::selected_package(const std::string& id) const {
    const auto selection = selections_.find(id);
    const ModSelection blank;
    return find_selected(packages_, id,
                         selection == selections_.end() ? blank : selection->second);
}

const ModFeature* ModPackageManager::selected_feature(
    const std::string& package_id, const std::string& feature_id) const {
    const ModPackage* package = selected_package(package_id);
    return package ? find_feature(*package, feature_id) : nullptr;
}

bool ModPackageManager::feature_enabled(const std::string& package_id,
                                        const std::string& feature_id) const {
    const ModPackage* package = selected_package(package_id);
    const ModFeature* feature =
        package ? find_feature(*package, feature_id) : nullptr;
    if (!package || !feature) return false;
    const auto found = selections_.find(package_id);
    const ModSelection blank;
    return is_feature_enabled(
        *package, found == selections_.end() ? blank : found->second, *feature);
}

std::string ModPackageManager::feature_option_value(
    const std::string& package_id, const std::string& feature_id,
    const std::string& option_id) const {
    const ModPackage* package = selected_package(package_id);
    if (!package) return {};
    const auto found = selections_.find(package_id);
    const ModSelection blank;
    return effective_option_value(
        *package, found == selections_.end() ? blank : found->second,
        feature_id, option_id);
}

ModResolution ModPackageManager::resolve(const std::string& game_id,
                                         const std::string& exe_sha256,
                                         const std::string& disc_sha256) const {
    ModResolution result;
    std::map<std::string, const ModPackage*> active;
    for (const auto& [id, versions] : packages_) {
        (void)versions;
        const auto selected = selections_.find(id);
        const ModSelection blank;
        const ModSelection& selection =
            selected == selections_.end() ? blank : selected->second;
        const ModPackage* package = find_selected(packages_, id, selection);
        if (!package) {
            if (selected != selections_.end())
                result.errors.push_back(
                    "selected package/version is not installed: " + id);
            continue;
        }
        if (!has_enabled_feature(*package, selection)) continue;
        if (!target_matches(*package, game_id, exe_sha256, disc_sha256)) {
            result.errors.push_back("package does not target this game/image: " + id);
            continue;
        }
        active[id] = package;
    }

    for (const auto& [id, package] : active) {
        for (const ModRequirement& dep : package->dependencies) {
            const auto found = active.find(dep.id);
            if (found == active.end())
                result.errors.push_back(id + " requires enabled package " + dep.id);
            else if (!version_satisfies(found->second->version, dep.version))
                result.errors.push_back(id + " requires " + dep.id + " " + dep.version);
        }
        for (const std::string& conflict : package->conflicts)
            if (active.find(conflict) != active.end())
                result.errors.push_back(id + " conflicts with " + conflict);

        const auto selection = selections_.find(id);
        const ModSelection blank_selection;
        const ModSelection& effective_selection =
            selection == selections_.end()
                ? blank_selection
                : selection->second;
        std::string failing_feature;
        std::string reason;
        if (!package_constraints_satisfied(
                *package, effective_selection, nullptr, nullptr,
                nullptr, nullptr, nullptr, &failing_feature, &reason)) {
            result.errors.push_back(
                id + "/" + failing_feature + ": " + reason);
        }
        if (selection != selections_.end()) {
            for (const ModOption& option : package->options) {
                const ModFeature* feature =
                    find_feature(*package, option.feature_id);
                if (!feature ||
                    !is_feature_enabled(*package, selection->second, *feature))
                    continue;
                const std::map<std::string, std::string>* values = nullptr;
                if (feature->legacy) {
                    values = &selection->second.values;
                } else {
                    const auto feature_selection =
                        selection->second.features.find(feature->id);
                    if (feature_selection != selection->second.features.end())
                        values = &feature_selection->second.values;
                }
                if (!values) continue;
                const auto value = values->find(option.id);
                if (value != values->end() &&
                    !valid_option_value(option, value->second))
                    result.errors.push_back(
                        id + "/" + feature->id +
                        ": invalid value for " + option.id);
            }
        }
        if (package->resolver.rfind("builtin:", 0) == 0) {
            const std::string resolver_id = package->resolver.substr(8);
            const auto resolver = builtin_resolvers().find(resolver_id);
            if (resolver == builtin_resolvers().end())
                result.errors.push_back(id + ": built-in resolver is unavailable: " + resolver_id);
        }
    }

    enum class Visit { None, Active, Done };
    std::map<std::string, Visit> visits;
    std::function<void(const std::string&)> visit = [&](const std::string& id) {
        if (visits[id] == Visit::Done) return;
        if (visits[id] == Visit::Active) {
            result.errors.push_back("dependency cycle includes " + id);
            return;
        }
        visits[id] = Visit::Active;
        const ModPackage* package = active.at(id);
        std::vector<std::string> deps;
        for (const ModRequirement& dep : package->dependencies)
            if (active.find(dep.id) != active.end()) deps.push_back(dep.id);
        std::sort(deps.begin(), deps.end());
        for (const std::string& dep : deps) visit(dep);
        visits[id] = Visit::Done;
        result.ordered.push_back(package);
    };
    for (const auto& [id, package] : active) visit(id);

    if (!result.errors.empty()) {
        result.ordered.clear();
        return result;
    }

    ModBuiltinResolverContext resolver_context;
    resolver_context.active_packages = &active;
    resolver_context.selections = &selections_;
    for (const ModPackage* package : result.ordered) {
        const auto selected_it = selections_.find(package->id);
        const ModSelection blank;
        const ModSelection& selected =
            selected_it == selections_.end() ? blank : selected_it->second;
        if (package->resolver == "declarative") {
            for (const ModDerivedDisc& derived : package->derived_discs) {
                const ModFeature& legacy = package->features.front();
                if (!legacy.legacy ||
                    !is_feature_enabled(*package, selected, legacy) ||
                    !conditions_match(*package, selected, "legacy", derived.when))
                    continue;
                ModResolution::DerivedDisc resolved;
                resolved.kind = derived.kind;
                resolved.patch = derived.patch;
                resolved.patch_sha256 = derived.patch_sha256;
                resolved.output_size = derived.output_size;
                resolved.output_sha256 = derived.output_sha256;
                resolved.package_id = package->id;
                result.derived_discs.push_back(std::move(resolved));
            }
            std::vector<const ModPatch*> patches;
            patches.reserve(package->patches.size());
            for (const ModPatch& patch : package->patches) {
                const ModFeature* feature =
                    find_feature(*package, patch.feature_id);
                if (!feature ||
                    !is_feature_enabled(*package, selected, *feature) ||
                    !conditions_match(*package, selected,
                                      patch.feature_id, patch.when) ||
                    !integer_predicate_matches(
                        *package, selected, patch.feature_id,
                        patch.when_integer))
                    continue;
                patches.push_back(&patch);
            }
            std::stable_sort(patches.begin(), patches.end(),
                [](const ModPatch* a, const ModPatch* b) { return a->order < b->order; });
            for (const ModPatch* patch : patches) {
                ModResolution::Write write;
                write.target = patch->target;
                write.location = patch->location;
                write.expected = patch->expected;
                if (!patch->fields.empty()) {
                    bool encode_failed = false;
                    for (const ModPatchField& field : patch->fields) {
                        std::vector<uint8_t> encoded = field.replacement;
                        if (encoded.empty()) {
                            const std::string selected_value =
                                effective_option_value(
                                    *package, selected, patch->feature_id,
                                    field.replace_from_option);
                            int64_t parsed = 0;
                            int64_t adjusted = 0;
                            if (!parse_canonical_int64(
                                    selected_value, parsed) ||
                                !checked_add_int64(
                                    parsed, field.replace_addend,
                                    adjusted)) {
                                result.errors.push_back(
                                    package->id + "/" +
                                    patch->feature_id +
                                    ": could not encode sparse field option " +
                                    field.replace_from_option);
                                encode_failed = true;
                                break;
                            }
                            if (field.replace_encoding ==
                                ModValueEncoding::MipsLuiOriU32) {
                                if (adjusted < 0 ||
                                    static_cast<uint64_t>(adjusted) >
                                        UINT32_MAX) {
                                    result.errors.push_back(
                                        package->id + "/" +
                                        patch->feature_id +
                                        ": could not encode sparse field "
                                        "option " +
                                        field.replace_from_option);
                                    encode_failed = true;
                                    break;
                                }
                                const uint32_t value =
                                    static_cast<uint32_t>(adjusted);
                                const std::array<
                                    std::pair<uint64_t,
                                              std::vector<uint8_t>>, 2>
                                    halves{{
                                        {field.offset,
                                         {
                                             static_cast<uint8_t>(
                                                 value >> 16),
                                             static_cast<uint8_t>(
                                                 value >> 24),
                                         }},
                                        {field.offset + 4,
                                         {
                                             static_cast<uint8_t>(value),
                                             static_cast<uint8_t>(
                                                 value >> 8),
                                         }},
                                    }};
                                for (const auto& [offset, bytes] : halves) {
                                    if (std::equal(
                                            bytes.begin(), bytes.end(),
                                            write.expected.begin() +
                                                static_cast<size_t>(
                                                    offset)))
                                        continue;
                                    ModResolution::Write::Field resolved;
                                    resolved.offset = offset;
                                    resolved.replacement = bytes;
                                    write.fields.push_back(
                                        std::move(resolved));
                                }
                                continue;
                            }
                            if (!encode_unsigned_value(
                                    field.replace_encoding, adjusted,
                                    encoded)) {
                                result.errors.push_back(
                                    package->id + "/" +
                                    patch->feature_id +
                                    ": could not encode sparse field option " +
                                    field.replace_from_option);
                                encode_failed = true;
                                break;
                            }
                        }
                        if (std::equal(
                                encoded.begin(), encoded.end(),
                                write.expected.begin() +
                                    static_cast<size_t>(field.offset)))
                            continue;
                        ModResolution::Write::Field resolved;
                        resolved.offset = field.offset;
                        resolved.replacement = std::move(encoded);
                        write.fields.push_back(std::move(resolved));
                    }
                    if (encode_failed) continue;
                    if (write.fields.empty()) continue;
                    std::sort(
                        write.fields.begin(), write.fields.end(),
                        [](const ModResolution::Write::Field& a,
                           const ModResolution::Write::Field& b) {
                            return a.offset < b.offset;
                        });
                } else if (patch->replace_from_option.empty()) {
                    write.replacement = patch->replacement;
                } else {
                    const std::string selected_value =
                        effective_option_value(
                            *package, selected, patch->feature_id,
                            patch->replace_from_option);
                    const ModOption* source_option = find_option(
                        *package, patch->feature_id,
                        patch->replace_from_option);
                    if (patch->replace_omit_when_default &&
                        source_option &&
                        selected_value == source_option->default_value)
                        continue;
                    int64_t parsed = 0;
                    int64_t adjusted = 0;
                    std::vector<uint8_t> encoded;
                    if (!parse_canonical_int64(selected_value, parsed) ||
                        !checked_add_int64(
                            parsed, patch->replace_addend, adjusted)) {
                        result.errors.push_back(
                            package->id + "/" + patch->feature_id +
                            ": could not encode replace_from option " +
                            patch->replace_from_option);
                        continue;
                    }
                    write.replacement = write.expected;
                    if (patch->replace_encoding ==
                        ModValueEncoding::MipsLuiOriU32) {
                        if (adjusted < 0 ||
                            static_cast<uint64_t>(adjusted) > UINT32_MAX) {
                            result.errors.push_back(
                                package->id + "/" + patch->feature_id +
                                ": could not encode replace_from option " +
                                patch->replace_from_option);
                            continue;
                        }
                        const uint32_t value =
                            static_cast<uint32_t>(adjusted);
                        write.replacement[0] =
                            static_cast<uint8_t>(value >> 16);
                        write.replacement[1] =
                            static_cast<uint8_t>(value >> 24);
                        write.replacement[4] =
                            static_cast<uint8_t>(value);
                        write.replacement[5] =
                            static_cast<uint8_t>(value >> 8);
                    } else {
                        if (!encode_unsigned_value(
                                patch->replace_encoding, adjusted,
                                encoded)) {
                            result.errors.push_back(
                                package->id + "/" + patch->feature_id +
                                ": could not encode replace_from option " +
                                patch->replace_from_option);
                            continue;
                        }
                        std::copy(
                            encoded.begin(), encoded.end(),
                            write.replacement.begin() +
                                static_cast<size_t>(
                                    patch->replace_offset));
                    }
                    if (write.replacement == write.expected &&
                        !patch->replace_omit_when_default)
                        continue;
                }
                write.package_id = package->id;
                write.feature_id = patch->feature_id;
                result.writes.push_back(std::move(write));
            }
        } else {
            const std::string resolver_id = package->resolver.substr(8);
            const auto resolver = builtin_resolvers().find(resolver_id);
            if (resolver != builtin_resolvers().end() &&
                !resolver->second(
                    *package, selected, resolver_context,
                    result.writes, result.errors) &&
                result.errors.empty())
                result.errors.push_back(package->id + ": built-in resolver failed");
        }
        std::vector<const ModOverlay*> overlays;
        overlays.reserve(package->overlays.size());
        for (const ModOverlay& overlay : package->overlays) {
            const ModFeature* feature =
                find_feature(*package, overlay.feature_id);
            if (!feature ||
                !is_feature_enabled(*package, selected, *feature) ||
                !conditions_match(*package, selected,
                                  overlay.feature_id, overlay.when) ||
                !feature_predicate_matches(
                    overlay.when_feature, active, selections_))
                continue;
            overlays.push_back(&overlay);
        }
        std::stable_sort(overlays.begin(), overlays.end(),
            [](const ModOverlay* a, const ModOverlay* b) {
                return a->order < b->order;
            });
        for (const ModOverlay* overlay : overlays) {
            std::vector<uint8_t> payload;
            std::string payload_error;
            if (!read_file(overlay->file, payload, &payload_error)) {
                result.errors.push_back(
                    package->id + "/" + overlay->feature_id + ": " +
                    payload_error);
                continue;
            }
            const std::string actual = fingerprint_text(std::string(
                (const char*)payload.data(), payload.size()));
            if (payload.size() != overlay->size ||
                actual != overlay->sha256) {
                result.errors.push_back(
                    package->id + "/" + overlay->feature_id +
                    ": overlay payload changed after installation");
                continue;
            }
            ModResolution::Overlay resolved;
            resolved.target = overlay->target;
            resolved.location = overlay->location;
            resolved.payload = std::move(payload);
            resolved.payload_sha256 = overlay->sha256;
            resolved.expected_sha256 = overlay->expected_sha256;
            resolved.package_id = package->id;
            resolved.feature_id = overlay->feature_id;
            result.overlays.push_back(std::move(resolved));
        }
        std::vector<const ModPlugin*> plugins;
        plugins.reserve(package->plugins.size());
        for (const ModPlugin& plugin : package->plugins) {
            const ModFeature* feature =
                find_feature(*package, plugin.feature_id);
            if (!feature ||
                !is_feature_enabled(*package, selected, *feature) ||
                !conditions_match(*package, selected,
                                  plugin.feature_id, plugin.when))
                continue;
            plugins.push_back(&plugin);
        }
        std::stable_sort(
            plugins.begin(), plugins.end(),
            [](const ModPlugin* a, const ModPlugin* b) {
                return a->order < b->order;
            });
        for (const ModPlugin* plugin : plugins) {
            if (!mod_plugin_registered(plugin->id)) {
                result.errors.push_back(
                    package->id + "/" + plugin->feature_id +
                    ": trusted plugin is unavailable: " + plugin->id);
                continue;
            }
            ModResolution::Plugin resolved;
            resolved.id = plugin->id;
            resolved.package_id = package->id;
            resolved.feature_id = plugin->feature_id;
            result.plugins.push_back(std::move(resolved));
        }
    }
    if (result.derived_discs.size() > 1) {
        std::string providers;
        for (const auto& derived : result.derived_discs) {
            if (!providers.empty()) providers += ", ";
            providers += derived.package_id;
        }
        result.errors.push_back(
            "more than one derived-disc provider is active: " + providers);
    }
    std::vector<ModResolution::Plugin> coalesced_plugins;
    coalesced_plugins.reserve(result.plugins.size());
    for (const ModResolution::Plugin& plugin : result.plugins) {
        bool claimed = false;
        for (const ModResolution::Plugin& previous : coalesced_plugins) {
            if (plugin.id != previous.id) continue;
            if (plugin.package_id == previous.package_id &&
                plugin.feature_id == previous.feature_id) {
                claimed = true;
                break;
            }
            ModResolution::Diagnostic diagnostic;
            diagnostic.resource = "plugin:" + plugin.id;
            diagnostic.package_id = plugin.package_id;
            diagnostic.feature_id = plugin.feature_id;
            diagnostic.other_package_id = previous.package_id;
            diagnostic.other_feature_id = previous.feature_id;
            diagnostic.message =
                plugin.package_id + "/" + plugin.feature_id +
                " collides at " + diagnostic.resource + " with " +
                previous.package_id + "/" + previous.feature_id;
            result.diagnostics.push_back(diagnostic);
            result.errors.push_back(diagnostic.message);
            claimed = true;
            break;
        }
        if (!claimed) coalesced_plugins.push_back(plugin);
    }
    result.plugins = std::move(coalesced_plugins);
    std::vector<ModResolution::Write> coalesced;
    coalesced.reserve(result.writes.size());
    for (const ModResolution::Write& write : result.writes) {
        bool valid_write = !write.expected.empty();
        if (write.fields.empty()) {
            valid_write =
                valid_write &&
                write.expected.size() == write.replacement.size();
        } else {
            valid_write = valid_write && write.replacement.empty();
            uint64_t previous_end = 0;
            for (const ModResolution::Write::Field& field : write.fields) {
                if (field.replacement.empty() ||
                    field.offset > write.expected.size() ||
                    field.replacement.size() >
                        write.expected.size() - field.offset ||
                    field.offset < previous_end) {
                    valid_write = false;
                    break;
                }
                previous_end =
                    field.offset + field.replacement.size();
            }
        }
        if (!valid_write) {
            result.errors.push_back(
                write.package_id + "/" + write.feature_id +
                ": resolver emitted invalid write");
            continue;
        }
        bool duplicate = false;
        for (const ModResolution::Write& previous : coalesced) {
            if (identical_write(previous, write)) {
                duplicate = true;
                break;
            }
            uint64_t guard_mismatch = 0;
            if (write_guard_overlap_mismatch(
                    previous, write, guard_mismatch)) {
                ModResolution::Diagnostic diagnostic;
                diagnostic.resource =
                    byte_resource(write.target, guard_mismatch);
                diagnostic.package_id = write.package_id;
                diagnostic.feature_id = write.feature_id;
                diagnostic.other_package_id = previous.package_id;
                diagnostic.other_feature_id = previous.feature_id;
                diagnostic.message =
                    write.package_id + "/" + write.feature_id +
                    " has an incompatible guard at " +
                    diagnostic.resource + " with " +
                    previous.package_id + "/" +
                    previous.feature_id;
                result.diagnostics.push_back(diagnostic);
                result.errors.push_back(diagnostic.message);
                duplicate = true;
                break;
            }
            if (!writes_overlap(previous, write)) continue;
            uint64_t mismatch = 0;
            if (!write_overlap_mismatch(previous, write, mismatch))
                continue;
            ModResolution::Diagnostic diagnostic;
            diagnostic.resource = byte_resource(write.target, mismatch);
            diagnostic.package_id = write.package_id;
            diagnostic.feature_id = write.feature_id;
            diagnostic.other_package_id = previous.package_id;
            diagnostic.other_feature_id = previous.feature_id;
            diagnostic.message =
                write.package_id + "/" + write.feature_id +
                " collides at " + diagnostic.resource + " with " +
                previous.package_id + "/" + previous.feature_id;
            result.diagnostics.push_back(diagnostic);
            result.errors.push_back(diagnostic.message);
            duplicate = true;
            break;
        }
        if (!duplicate) coalesced.push_back(write);
    }
    result.writes = std::move(coalesced);
    std::vector<ModResolution::Overlay> coalesced_overlays;
    coalesced_overlays.reserve(result.overlays.size());
    for (const ModResolution::Overlay& overlay : result.overlays) {
        bool claimed = false;
        for (const ModResolution::Write& write : result.writes) {
            if (!write_overlaps_range(
                    write, overlay.target, overlay.location,
                    overlay.payload.size()))
                continue;
            uint64_t mismatch = 0;
            if (!write_overlay_mismatch(write, overlay, mismatch))
                continue;
            ModResolution::Diagnostic diagnostic;
            diagnostic.resource = byte_resource(overlay.target, mismatch);
            diagnostic.package_id = overlay.package_id;
            diagnostic.feature_id = overlay.feature_id;
            diagnostic.other_package_id = write.package_id;
            diagnostic.other_feature_id = write.feature_id;
            diagnostic.message =
                overlay.package_id + "/" + overlay.feature_id +
                " collides at " + diagnostic.resource + " with " +
                write.package_id + "/" + write.feature_id;
            result.diagnostics.push_back(diagnostic);
            result.errors.push_back(diagnostic.message);
            claimed = true;
            break;
        }
        if (claimed) continue;
        for (const ModResolution::Overlay& previous : coalesced_overlays) {
            if (!ranges_overlap(
                    overlay.target, overlay.location, overlay.payload.size(),
                    previous.target, previous.location, previous.payload.size()))
                continue;
            if (overlay.target == previous.target &&
                overlay.location == previous.location &&
                overlay.payload == previous.payload &&
                overlay.expected_sha256 == previous.expected_sha256) {
                claimed = true;
                break;
            }
            uint64_t mismatch = 0;
            if (!overlay_overlap_mismatch(previous, overlay, mismatch))
                continue;
            ModResolution::Diagnostic diagnostic;
            diagnostic.resource = byte_resource(overlay.target, mismatch);
            diagnostic.package_id = overlay.package_id;
            diagnostic.feature_id = overlay.feature_id;
            diagnostic.other_package_id = previous.package_id;
            diagnostic.other_feature_id = previous.feature_id;
            diagnostic.message =
                overlay.package_id + "/" + overlay.feature_id +
                " collides at " + diagnostic.resource + " with " +
                previous.package_id + "/" + previous.feature_id;
            result.diagnostics.push_back(diagnostic);
            result.errors.push_back(diagnostic.message);
            claimed = true;
            break;
        }
        if (!claimed) coalesced_overlays.push_back(overlay);
    }
    result.overlays = std::move(coalesced_overlays);
    if (!result.errors.empty()) {
        result.ordered.clear();
        result.writes.clear();
        result.overlays.clear();
        result.derived_discs.clear();
        result.plugins.clear();
        return result;
    }
    result.fingerprint = fingerprint_text(
        canonical_resolution(
            result.ordered, selections_, result.writes, result.overlays,
            result.derived_discs, result.plugins,
            disc_sha256));
    result.ok = true;
    return result;
}

} // namespace PSXRecompV4

extern "C" int psx_mod_register_vblank_plugin(
    const char* id, PSXModVBlankCallback callback) {
    return id &&
        PSXRecompV4::mod_register_vblank_plugin(id, callback) ? 1 : 0;
}

extern "C" int psx_mod_register_activation_plugin(
    const char* id, PSXModActivationCallback callback) {
    return id &&
        PSXRecompV4::mod_register_activation_plugin(id, callback) ? 1 : 0;
}
