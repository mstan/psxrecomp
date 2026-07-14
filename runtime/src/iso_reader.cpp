#include "iso_reader.h"
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <cstdio>

namespace PS1 {

// PS1 CD-ROM sector size (Mode 2, Form 1 user data)
constexpr size_t SECTOR_SIZE = 2048;

// Full sector size including headers/subchannel (2352 bytes for raw BIN files)
constexpr size_t RAW_SECTOR_SIZE = 2352;

// Offset to user data in raw sector (Mode 2, Form 1)
constexpr size_t RAW_DATA_OFFSET = 24;

// Primary Volume Descriptor location
constexpr uint32_t PVD_SECTOR = 16;

ISOReader::ISOReader()
    : is_open_(false) {
    root_dir_.lba = 0;
    root_dir_.size = 0;
}

ISOReader::~ISOReader() {
    Close();
}

bool ISOReader::Open(const std::string& filename) {
    // Close any previously opened file
    Close();

    // Check if file exists
    if (!std::filesystem::exists(filename)) {
        return false;
    }

    struct CueFileSpec { std::string path; };
    struct CueTrackSpec {
        int number = 0;
        bool is_audio = false;
        size_t file_index = 0;
        uint32_t index01 = 0;
        uint32_t index00 = 0;
        bool has_index00 = false;
        bool has_index01 = false;
    };
    std::vector<CueFileSpec> cue_files;
    std::vector<CueTrackSpec> cue_tracks;

    // Handle .cue files. Unlike a single-BIN cue, a multi-file cue gives each
    // track its own FILE and each INDEX is relative to that file. Parse the
    // complete sheet first; opening only the last FILE silently mounts an audio
    // track as sector zero and makes the BIOS see an all-zero filesystem.
    auto ends_with = [](const std::string& s, const std::string& suffix) {
        return s.size() >= suffix.size() &&
               s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (ends_with(filename, ".cue") || ends_with(filename, ".CUE")) {
        // Parse the .cue: resolve the FILE, AND build the track TOC from the
        // TRACK/INDEX lines. A bare .bin/.iso (no .cue) falls through to the
        // single-track synthesis below; a multi-track disc (data + CD-DA audio)
        // gets each track so the CD model's GetTN/GetTD report them correctly.
        std::ifstream cue_file(filename);
        if (!cue_file.is_open()) {
            return false;
        }

        std::string line;
        size_t current_file = static_cast<size_t>(-1);
        int current_track = -1;
        while (std::getline(cue_file, line)) {
            // FILE "filename.bin" BINARY
            size_t file_pos = line.find("FILE");
            if (file_pos != std::string::npos) {
                size_t quote1 = line.find('"', file_pos);
                size_t quote2 = line.find('"', quote1 + 1);
                if (quote1 != std::string::npos && quote2 != std::string::npos) {
                    // Compressed/audio-container payloads have no fixed raw
                    // sector geometry. Refuse them instead of silently mapping
                    // the following track onto the previous BINARY file.
                    if (line.find("BINARY", quote2) == std::string::npos)
                        return false;
                    std::string bin_name = line.substr(quote1 + 1, quote2 - quote1 - 1);
                    std::filesystem::path cue_path(filename);
                    std::filesystem::path bin_path(bin_name);
                    if (bin_path.is_relative()) bin_path = cue_path.parent_path() / bin_path;
                    cue_files.push_back({bin_path.lexically_normal().string()});
                    current_file = cue_files.size() - 1;
                    current_track = -1;
                }
                continue;
            }

            // TRACK NN MODE2/2352 | TRACK NN AUDIO
            int  tn = 0;
            char type[32] = {0};
            if (std::sscanf(line.c_str(), " TRACK %d %31s", &tn, type) == 2) {
                if (current_file == static_cast<size_t>(-1)) return false;
                CueTrackSpec t;
                t.number = tn;
                t.is_audio = (std::strstr(type, "AUDIO") != nullptr);
                t.file_index = current_file;
                cue_tracks.push_back(t);
                current_track = static_cast<int>(cue_tracks.size() - 1);
                continue;
            }

            // INDEX 01 is the track's program start; INDEX 00 begins its
            // pregap. CD-DA playback may intentionally start inside INDEX 00.
            int idx = 0, mm = 0, ss = 0, ff = 0;
            if (std::sscanf(line.c_str(), " INDEX %d %d:%d:%d", &idx, &mm, &ss, &ff) == 4
                && current_track >= 0) {
                auto& t = cue_tracks[static_cast<size_t>(current_track)];
                const uint32_t lba = static_cast<uint32_t>(((mm * 60 + ss) * 75) + ff);
                if (idx == 0) {
                    t.index00 = lba;
                    t.has_index00 = true;
                } else if (idx == 1) {
                    t.index01 = lba;
                    t.has_index01 = true;
                }
            }
        }
        cue_file.close();
        if (cue_files.empty()) return false;

        uint32_t next_base = 0;
        for (const auto& spec : cue_files) {
            auto image = std::make_unique<ImageFile>();
            image->path = spec.path;
            image->stream.open(image->path, std::ios::binary);
            if (!image->stream.is_open()) { Close(); return false; }
            image->stream.seekg(0, std::ios::end);
            const std::streampos size = image->stream.tellg();
            image->stream.clear();
            if (size <= 0 || (static_cast<uint64_t>(size) % RAW_SECTOR_SIZE) != 0) {
                Close(); return false;
            }
            image->base_lba = next_base;
            image->sector_size = RAW_SECTOR_SIZE;
            image->data_offset = RAW_DATA_OFFSET;
            image->sector_count = static_cast<uint32_t>(
                static_cast<uint64_t>(size) / RAW_SECTOR_SIZE);
            next_base += image->sector_count;
            files_.push_back(std::move(image));
        }

        for (const auto& spec : cue_tracks) {
            if (!spec.has_index01 || spec.file_index >= files_.size()) continue;
            CDTrack t;
            t.number = spec.number;
            t.is_audio = spec.is_audio;
            t.start_lba = files_[spec.file_index]->base_lba + spec.index01;
            t.pregap_lba = files_[spec.file_index]->base_lba +
                           (spec.has_index00 ? spec.index00 : spec.index01);
            tracks_.push_back(t);
            if (!t.is_audio && bin_path_.empty())
                bin_path_ = files_[spec.file_index]->path;
        }
    } else {
        auto image = std::make_unique<ImageFile>();
        image->path = filename;
        image->stream.open(filename, std::ios::binary);
        if (!image->stream.is_open()) return false;
        image->stream.seekg(0, std::ios::end);
        const std::streampos size = image->stream.tellg();
        image->stream.clear();
        if (size <= 0) return false;
        const uint64_t byte_size = static_cast<uint64_t>(size);
        const bool raw = (byte_size % RAW_SECTOR_SIZE) == 0;
        image->base_lba = 0;
        image->sector_size = raw ? RAW_SECTOR_SIZE : SECTOR_SIZE;
        image->data_offset = raw ? RAW_DATA_OFFSET : 0;
        image->sector_count = static_cast<uint32_t>(byte_size / image->sector_size);
        bin_path_ = filename;
        files_.push_back(std::move(image));
    }

    if (tracks_.empty()) tracks_.push_back({1, false, 0});
    if (bin_path_.empty() && !files_.empty()) bin_path_ = files_.front()->path;
    is_open_ = true;

    // Parse the volume descriptor to extract filesystem metadata when
    // present. Runtime CD-ROM access only needs sector reads, so keep the
    // image mounted even if an ISO9660 header is missing or nonstandard.
    if (!ParseVolumeDescriptor()) {
        volume_id_.clear();
        root_dir_.lba = 0;
        root_dir_.size = 0;
    }

    return true;
}

void ISOReader::Close() {
    for (auto& image : files_)
        if (image && image->stream.is_open()) image->stream.close();
    files_.clear();
    tracks_.clear();
    bin_path_.clear();
    volume_id_.clear();
    root_dir_.lba = 0;
    root_dir_.size = 0;
    is_open_ = false;
}

ISOReader::ImageFile* ISOReader::FindImageFile(uint32_t lba) {
    for (auto& image : files_) {
        if (lba >= image->base_lba && lba - image->base_lba < image->sector_count)
            return image.get();
    }
    return nullptr;
}

bool ISOReader::ReadSector(uint32_t lba, uint8_t* buffer) {
    if (!is_open_ || !buffer) {
        return false;
    }

    ImageFile* image = FindImageFile(lba);
    if (!image) return false;
    image->stream.clear();
    const uint32_t local_lba = lba - image->base_lba;
    const std::streampos offset = static_cast<std::streampos>(local_lba) *
                                  image->sector_size + image->data_offset;
    image->stream.seekg(offset, std::ios::beg);
    if (!image->stream.good()) { image->stream.clear(); return false; }
    image->stream.read(reinterpret_cast<char*>(buffer), SECTOR_SIZE);
    const bool success = image->stream.gcount() == SECTOR_SIZE;
    image->stream.clear();
    return success;
}

bool ISOReader::ReadRawSector(uint32_t lba, uint8_t* buffer) {
    if (!is_open_ || !buffer) {
        return false;
    }

    ImageFile* image = FindImageFile(lba);
    if (!image || image->sector_size != RAW_SECTOR_SIZE) return false;
    image->stream.clear();
    const uint32_t local_lba = lba - image->base_lba;
    const std::streampos offset = static_cast<std::streampos>(local_lba) * RAW_SECTOR_SIZE;
    image->stream.seekg(offset, std::ios::beg);
    if (!image->stream.good()) { image->stream.clear(); return false; }
    image->stream.read(reinterpret_cast<char*>(buffer), RAW_SECTOR_SIZE);
    const bool success = image->stream.gcount() == RAW_SECTOR_SIZE;
    image->stream.clear();
    return success;
}

bool ISOReader::IsOpen() const {
    return is_open_;
}

std::string ISOReader::GetVolumeID() const {
    return volume_id_;
}

std::string ISOReader::GetBinPath() const {
    return bin_path_;
}

uint32_t ISOReader::GetSectorCount() {
    if (!is_open_) {
        return 0;
    }

    if (files_.empty()) return 0;
    const auto& last = files_.back();
    return last->base_lba + last->sector_count;
}

int ISOReader::TrackCount() const {
    return static_cast<int>(tracks_.size());
}

uint32_t ISOReader::TrackStartLBA(int track) const {
    for (const auto& t : tracks_) {
        if (t.number == track) return t.start_lba;
    }
    return 0;
}

uint32_t ISOReader::TrackPregapLBA(int track) const {
    for (const auto& t : tracks_) {
        if (t.number == track) return t.pregap_lba;
    }
    return 0;
}

bool ISOReader::TrackIsAudio(int track) const {
    for (const auto& t : tracks_) {
        if (t.number == track) return t.is_audio;
    }
    return false;
}

RootDirectoryInfo ISOReader::GetRootDirectory() const {
    return root_dir_;
}

uint32_t ISOReader::Read733(const uint8_t* data) const {
    // Read little-endian half of both-endian 32-bit value
    return data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
}

bool ISOReader::ParseVolumeDescriptor() {
    // Read Primary Volume Descriptor from sector 16
    uint8_t pvd[SECTOR_SIZE];
    if (!ReadSector(PVD_SECTOR, pvd)) {
        return false;
    }

    // Verify ISO9660 signature: offset 1 should contain "CD001"
    if (pvd[0] != 0x01 || std::memcmp(&pvd[1], "CD001", 5) != 0) {
        return false;  // Not a valid ISO9660 disc
    }

    // Extract volume ID (offset 40, 32 bytes, space-padded ASCII)
    volume_id_.clear();
    for (int i = 0; i < 32; i++) {
        char c = pvd[40 + i];
        if (c != ' ' && c != '\0') {
            volume_id_ += c;
        }
    }

    // Extract root directory record (offset 156, 34 bytes)
    const uint8_t* root_record = &pvd[156];

    // Root directory LBA is at offset 2 within the directory record (both-endian 32-bit)
    root_dir_.lba = Read733(&root_record[2]);

    // Root directory size is at offset 10 within the directory record (both-endian 32-bit)
    root_dir_.size = Read733(&root_record[10]);

    return true;
}

bool ISOReader::ParseDirectoryRecord(const uint8_t* data, ISOFileEntry& entry) const {
    // Check record length (offset 0)
    uint8_t record_len = data[0];
    if (record_len == 0 || record_len < 33) {
        return false;  // Invalid or padding record
    }

    // Extract LBA (offset 2, both-endian 32-bit)
    entry.lba = Read733(&data[2]);

    // Extract file size (offset 10, both-endian 32-bit)
    entry.size = Read733(&data[10]);

    // Extract file flags (offset 25)
    uint8_t flags = data[25];
    entry.is_directory = (flags & 0x02) != 0;

    // Extract filename length (offset 32)
    uint8_t name_len = data[32];
    if (name_len == 0) {
        return false;  // Invalid record
    }

    // Extract filename (offset 33)
    const char* name_ptr = reinterpret_cast<const char*>(&data[33]);

    // Handle special directory entries
    if (name_len == 1 && name_ptr[0] == '\x00') {
        entry.name = ".";  // Current directory
        return true;
    }
    if (name_len == 1 && name_ptr[0] == '\x01') {
        entry.name = "..";  // Parent directory
        return true;
    }

    // Parse regular filename, strip version suffix (";1")
    entry.name.clear();
    for (uint8_t i = 0; i < name_len; i++) {
        char c = name_ptr[i];
        if (c == ';') {
            break;  // Stop at version separator
        }
        entry.name += c;
    }

    return true;
}

std::vector<ISOFileEntry> ISOReader::ListFilesByLBA(uint32_t lba, uint32_t dir_size) {
    std::vector<ISOFileEntry> results;

    if (!is_open_ || lba == 0 || dir_size == 0) {
        return results;
    }

    // Calculate number of sectors needed for directory data
    uint32_t num_sectors = (dir_size + 2047) / 2048;

    // Allocate buffer for directory data
    std::vector<uint8_t> dir_data(num_sectors * 2048, 0);

    // Read all directory sectors
    for (uint32_t i = 0; i < num_sectors; i++) {
        if (!ReadSector(lba + i, &dir_data[i * 2048])) {
            return results;  // Error reading sector
        }
    }

    // Parse directory records
    uint32_t offset = 0;
    while (offset < dir_size) {
        uint8_t record_len = dir_data[offset];

        if (record_len == 0) {
            // Skip to next sector boundary
            uint32_t sector_offset = offset % 2048;
            if (sector_offset != 0) {
                offset += (2048 - sector_offset);
                continue;
            }
            break;
        }

        if (offset + record_len > dir_data.size()) break;

        ISOFileEntry entry;
        if (ParseDirectoryRecord(&dir_data[offset], entry)) {
            if (entry.name != "." && entry.name != "..") {
                results.push_back(entry);
            }
        }

        offset += record_len;
    }

    return results;
}

std::vector<ISOFileEntry> ISOReader::ListFiles(const std::string& path) {
    std::vector<ISOFileEntry> results;

    // Check if file is open
    if (!is_open_) {
        return results;
    }

    if (path.empty()) {
        // List root directory
        RootDirectoryInfo root = GetRootDirectory();
        return ListFilesByLBA(root.lba, root.size);
    }

    // Non-empty path: navigate to that subdirectory within the root
    // Find the matching directory entry in root
    RootDirectoryInfo root = GetRootDirectory();
    std::vector<ISOFileEntry> root_entries = ListFilesByLBA(root.lba, root.size);

    std::string path_upper = path;
    std::transform(path_upper.begin(), path_upper.end(), path_upper.begin(),
                   [](unsigned char c) { return std::toupper(c); });

    for (const auto& e : root_entries) {
        if (!e.is_directory) continue;
        std::string name_upper = e.name;
        std::transform(name_upper.begin(), name_upper.end(), name_upper.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        if (name_upper == path_upper) {
            // Found the subdirectory — list its contents
            return ListFilesByLBA(e.lba, e.size > 0 ? e.size : 2048);
        }
    }

    return results;  // Directory not found
}

bool ISOReader::FindFile(const std::string& path, ISOFileEntry& entry) {
    // Check if file is open
    if (!is_open_) {
        return false;
    }

    // Check if path contains a directory separator
    size_t sep = path.find('/');
    if (sep == std::string::npos) {
        sep = path.find('\\');
    }

    if (sep != std::string::npos) {
        // Subdirectory path: "DIR/FILE" or "DIR\FILE"
        std::string dir_name  = path.substr(0, sep);
        std::string file_name = path.substr(sep + 1);

        // List the subdirectory
        std::vector<ISOFileEntry> sub_files = ListFiles(dir_name);

        std::string file_upper = file_name;
        std::transform(file_upper.begin(), file_upper.end(), file_upper.begin(),
                       [](unsigned char c) { return std::toupper(c); });

        for (const auto& f : sub_files) {
            std::string name_upper = f.name;
            std::transform(name_upper.begin(), name_upper.end(), name_upper.begin(),
                           [](unsigned char c) { return std::toupper(c); });
            if (name_upper == file_upper) {
                entry = f;
                return true;
            }
        }
        return false;
    }

    // Root-level file: search root directory
    std::vector<ISOFileEntry> files = ListFiles("");

    // Search for matching filename (case-insensitive comparison)
    for (const auto& file : files) {
        std::string file_upper = file.name;
        std::string path_upper = path;

        std::transform(file_upper.begin(), file_upper.end(), file_upper.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        std::transform(path_upper.begin(), path_upper.end(), path_upper.begin(),
                       [](unsigned char c) { return std::toupper(c); });

        if (file_upper == path_upper) {
            entry = file;
            return true;
        }
    }

    // File not found
    return false;
}

size_t ISOReader::ReadFile(const std::string& path, uint8_t* buffer, size_t max_size) {
    // Validate buffer pointer
    if (!buffer) {
        return 0;
    }

    // Find the file
    ISOFileEntry entry;
    if (!FindFile(path, entry)) {
        return 0;  // File not found
    }

    // Calculate how many bytes to read (min of file size and max_size)
    size_t bytes_to_read = std::min(static_cast<size_t>(entry.size), max_size);

    // Calculate number of sectors to read
    uint32_t sectors_to_read = (bytes_to_read + SECTOR_SIZE - 1) / SECTOR_SIZE;

    // Read sectors sequentially
    size_t bytes_read = 0;
    for (uint32_t i = 0; i < sectors_to_read; i++) {
        // Calculate how many bytes to read from this sector
        size_t bytes_remaining = bytes_to_read - bytes_read;
        size_t sector_bytes = std::min(bytes_remaining, SECTOR_SIZE);

        // Read sector into temporary buffer
        uint8_t sector_buffer[SECTOR_SIZE];
        if (!ReadSector(entry.lba + i, sector_buffer)) {
            return bytes_read;  // Error - return what we've read so far
        }

        // Copy data to output buffer
        std::memcpy(buffer + bytes_read, sector_buffer, sector_bytes);
        bytes_read += sector_bytes;
    }

    return bytes_read;
}

size_t ISOReader::GetFileSize(const std::string& path) {
    // Find the file
    ISOFileEntry entry;
    if (!FindFile(path, entry)) {
        return 0;  // File not found
    }

    // Return file size
    return entry.size;
}

} // namespace PS1
