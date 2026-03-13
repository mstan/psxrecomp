#include "iso_reader.h"
#include <filesystem>
#include <algorithm>
#include <cstring>

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

    // Handle .cue files - parse to find .bin file
    std::string bin_filename = filename;
    if (filename.ends_with(".cue") || filename.ends_with(".CUE")) {
        // Simple .cue parsing - look for FILE "..." BINARY line
        std::ifstream cue_file(filename);
        if (!cue_file.is_open()) {
            return false;
        }

        std::string line;
        while (std::getline(cue_file, line)) {
            // Look for: FILE "filename.bin" BINARY
            size_t file_pos = line.find("FILE");
            size_t binary_pos = line.find("BINARY");

            if (file_pos != std::string::npos && binary_pos != std::string::npos) {
                // Extract filename between quotes
                size_t quote1 = line.find('"', file_pos);
                size_t quote2 = line.find('"', quote1 + 1);

                if (quote1 != std::string::npos && quote2 != std::string::npos) {
                    std::string bin_name = line.substr(quote1 + 1, quote2 - quote1 - 1);

                    // If it's a relative path, combine with .cue directory
                    std::filesystem::path cue_path(filename);
                    std::filesystem::path bin_path(bin_name);

                    if (bin_path.is_relative()) {
                        bin_filename = (cue_path.parent_path() / bin_name).string();
                    } else {
                        bin_filename = bin_name;
                    }
                    break;
                }
            }
        }
        cue_file.close();
    }

    // Store the resolved bin path for callers (e.g. xa_audio)
    bin_path_ = bin_filename;

    // Open the ISO/BIN file in binary mode
    file_.open(bin_filename, std::ios::binary);
    if (!file_.is_open()) {
        return false;
    }

    is_open_ = true;

    // Parse the volume descriptor to extract filesystem metadata
    if (!ParseVolumeDescriptor()) {
        Close();
        return false;
    }

    return true;
}

void ISOReader::Close() {
    if (file_.is_open()) {
        file_.close();
    }
    is_open_ = false;
}

bool ISOReader::ReadSector(uint32_t lba, uint8_t* buffer) {
    if (!is_open_ || !buffer) {
        return false;
    }

    // Clear any error flags
    file_.clear();

    // Determine sector format by checking file size
    file_.seekg(0, std::ios::end);
    std::streampos file_size = file_.tellg();

    // Clear error flags after seeking
    file_.clear();

    // Check if file uses raw sectors (2352 bytes) or cooked sectors (2048 bytes)
    bool is_raw_format = (file_size % RAW_SECTOR_SIZE == 0);

    if (is_raw_format) {
        // Raw BIN format - read full sector, extract user data
        std::streampos offset = static_cast<std::streampos>(lba) * RAW_SECTOR_SIZE + RAW_DATA_OFFSET;
        file_.seekg(offset, std::ios::beg);

        if (!file_.good()) {
            file_.clear();
            return false;
        }

        file_.read(reinterpret_cast<char*>(buffer), SECTOR_SIZE);
        std::streamsize bytes_read = file_.gcount();
        bool success = (bytes_read == SECTOR_SIZE);
        file_.clear();
        return success;
    } else {
        // ISO format - sectors are already 2048 bytes
        std::streampos offset = static_cast<std::streampos>(lba) * SECTOR_SIZE;
        file_.seekg(offset, std::ios::beg);

        if (!file_.good()) {
            file_.clear();
            return false;
        }

        file_.read(reinterpret_cast<char*>(buffer), SECTOR_SIZE);
        std::streamsize bytes_read = file_.gcount();
        bool success = (bytes_read == SECTOR_SIZE);
        file_.clear();
        return success;
    }
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
