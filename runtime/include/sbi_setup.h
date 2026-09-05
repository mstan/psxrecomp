// Launcher input validation only. The guest/runtime SBI reader is unchanged.
#pragma once
#include "psx_sha256.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace PSXRecompV4 {
struct SbiRequirement {
    uint64_t size;
    const char* data_sha256;
    const char* sbi_sha256;
    const char* title;
    const char* serial;
};
#include "sbi_registry.h"

struct SbiSetupResult {
    bool required = false;
    bool ready = true;
    std::string message;
};

inline std::string sbi_setup_hash(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    psx_sha256_ctx ctx;
    psx_sha256_init(&ctx);
    std::array<uint8_t, 65536> buffer;
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        psx_sha256_update(&ctx, buffer.data(), static_cast<size_t>(input.gcount()));
    }
    if (!input.eof()) return {};
    uint8_t digest[32];
    psx_sha256_final(&ctx, digest);
    std::string result;
    for (auto byte : digest) {
        result += "0123456789abcdef"[byte >> 4];
        result += "0123456789abcdef"[byte & 15];
    }
    return result;
}

inline SbiSetupResult check_sbi_setup(
    const std::filesystem::path& data_track, const std::filesystem::path& mounted_image,
    const SbiRequirement* requirements = kSbiRequirements,
    size_t count = sizeof(kSbiRequirements) / sizeof(kSbiRequirements[0])) {
    std::error_code error;
    const auto size = std::filesystem::file_size(data_track, error);
    if (error) return {}; // Existing disc validation owns unreadable media.
    std::string data_hash;
    for (size_t i = 0; i < count; ++i) {
        const auto& rule = requirements[i];
        if (size != rule.size) continue;
        if (data_hash.empty()) data_hash = sbi_setup_hash(data_track);
        if (data_hash != rule.data_sha256) continue;
        auto companion = mounted_image;
        companion.replace_extension(".sbi");
        const bool exists = std::filesystem::is_regular_file(companion, error);
        if (exists && sbi_setup_hash(companion) == rule.sbi_sha256)
            return {true, true, {}};
        std::string message = std::string(rule.title) + " (" + rule.serial + ") needs a matching SBI file.\n\n";
        message += exists ? "The selected disc's SBI file does not match this supported revision.\n\n"
                          : "The SBI file is missing. It contains disc subchannel data required by this revision.\n\n";
        message += "Place your lawfully obtained matching SBI file here:\n" + companion.string();
        message += "\n\nThen select the disc again. Play remains blocked until the file matches."
                   "\nSetup does not supply or download SBI files.";
        return {true, false, message};
    }
    return {}; // Unknown revision, not proof that subchannels are complete.
}
// Stage only CUE metadata and the companion. The user's disc stays untouched.
inline std::filesystem::path import_sbi_setup(
    const std::filesystem::path& data, const std::filesystem::path& mounted,
    const std::filesystem::path& selected, const std::filesystem::path& destination,
    const SbiRequirement* rules = kSbiRequirements,
    size_t count = sizeof(kSbiRequirements) / sizeof(kSbiRequirements[0])) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto size = fs::file_size(data, ec);
    if (ec) throw std::runtime_error("Select a readable disc first.");
    const SbiRequirement* matched = nullptr;
    std::string hash;
    for (size_t i = 0; i < count; ++i) {
        if (rules[i].size != size) continue;
        if (hash.empty()) hash = sbi_setup_hash(data);
        if (hash == rules[i].data_sha256) { matched = &rules[i]; break; }
    }
    if (!matched) throw std::runtime_error("The selected disc has no supported SBI requirement.");
    if (sbi_setup_hash(selected) != matched->sbi_sha256)
        throw std::runtime_error("This SBI file does not match the selected disc. Select its matching SBI file.");
    std::string cue;
    auto extension = mounted.extension().string();
    for (auto& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (extension == ".cue") {
        std::ifstream input(mounted);
        if (!input) throw std::runtime_error("Cannot read the selected CUE file.");
        const std::regex file_line(R"re(^(\s*FILE\s+)(?:"([^"]+)"|(\S+))(\s+.*)$)re", std::regex::icase);
        std::string line; bool found = false;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::smatch match;
            if (std::regex_match(line, match, file_line)) {
                fs::path track(match[2].matched ? match[2].str() : match[3].str());
                if (track.is_relative()) track = mounted.parent_path() / track;
                track = fs::absolute(track).lexically_normal();
                if (!fs::is_regular_file(track)) throw std::runtime_error("A CUE track file is missing.");
                line = match[1].str() + "\"" + track.generic_string() + "\"" + match[4].str();
                found = true;
            }
            cue += line + "\n";
        }
        if (!input.eof() || !found) throw std::runtime_error("Cannot import this CUE file.");
    } else if (extension == ".bin") {
        cue = "FILE \"" + fs::absolute(data).generic_string() + "\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n";
    } else throw std::runtime_error("Select this disc's CUE or BIN file first.");
    // Unique directory avoids replacing any existing disc or companion.
    fs::create_directories(destination);
    fs::path folder;
    for (unsigned n = 1; n != 100000; ++n) {
        auto candidate = destination / ("disc-" + std::to_string(n));
        if (fs::create_directory(candidate)) { folder = candidate; break; }
    }
    if (folder.empty()) throw std::runtime_error("Cannot create the SBI input folder.");
    try {
        fs::copy_file(selected, folder / "disc.sbi");
        if (sbi_setup_hash(folder / "disc.sbi") != matched->sbi_sha256)
            throw std::runtime_error("The SBI file changed during import. Select it again.");
        std::ofstream output(folder / "disc.cue", std::ios::binary);
        output << cue;
        output.close();
        if (!output) throw std::runtime_error("Cannot save the imported CUE file.");
    } catch (...) { fs::remove_all(folder, ec); throw; }
    return fs::absolute(folder / "disc.cue");
}
} // namespace PSXRecompV4
