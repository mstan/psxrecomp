#pragma once

#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

// Write *content* to *path* only when bytes differ (or the file is missing).
// Preserves mtime on identical output so Ninja/ccache stay incremental across
// regenerate runs that emit the same C.
// Returns true if the file was written, false if unchanged. Throws on I/O error.
inline bool write_file_if_changed(const std::filesystem::path& path,
                                  const std::string& content) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) {
        const auto sz = fs::file_size(path, ec);
        if (!ec && sz == content.size()) {
            if (content.empty()) return false;
            std::ifstream in(path, std::ios::binary);
            if (in) {
                std::vector<char> buf(content.size());
                in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
                if (static_cast<std::size_t>(in.gcount()) == content.size() &&
                    std::memcmp(buf.data(), content.data(), content.size()) == 0) {
                    return false; // unchanged
                }
            }
        }
    }
    fs::create_directories(path.parent_path(), ec);
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("cannot open for write: " + tmp.string());
        }
        if (!content.empty()) {
            out.write(content.data(), static_cast<std::streamsize>(content.size()));
            if (!out) {
                throw std::runtime_error("write failed: " + tmp.string());
            }
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        ec.clear();
        fs::rename(tmp, path, ec);
        if (ec) {
            fs::remove(tmp, ec);
            throw std::runtime_error("cannot replace: " + path.string());
        }
    }
    return true;
}
