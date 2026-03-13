#pragma once

#include <cstdint>
#include <string>
#include <fstream>
#include <vector>

/**
 * ISO/BIN/CUE Reader for PS1 CD-ROM images
 * Session 24-25 - Validation-first approach
 *
 * Features:
 * - Open ISO/BIN file
 * - Read sectors by LBA
 * - Parse ISO9660 filesystem
 * - List files in directory
 * - Read file contents by path
 */

namespace PS1 {

/**
 * Information about a file entry in the ISO filesystem
 */
struct ISOFileEntry {
    std::string name;       // Filename (without version suffix)
    uint32_t lba;           // Logical Block Address (sector number)
    uint32_t size;          // File size in bytes
    bool is_directory;      // True if this is a directory
};

/**
 * Root directory information from Primary Volume Descriptor
 */
struct RootDirectoryInfo {
    uint32_t lba;           // LBA of root directory data
    uint32_t size;          // Size of root directory in bytes
};

class ISOReader {
public:
    /**
     * Constructor
     */
    ISOReader();

    /**
     * Destructor - ensures file is closed
     */
    ~ISOReader();

    /**
     * Open an ISO/BIN file for reading
     * @param filename Path to .iso, .bin, or .cue file
     * @return true if opened successfully, false otherwise
     */
    bool Open(const std::string& filename);

    /**
     * Close the currently open file
     */
    void Close();

    /**
     * Read a single sector from the ISO
     * @param lba Logical Block Address (sector number)
     * @param buffer Buffer to read into (must be at least 2048 bytes)
     * @return true if read successfully, false otherwise
     */
    bool ReadSector(uint32_t lba, uint8_t* buffer);

    /**
     * Check if a file is currently open
     * @return true if file is open
     */
    bool IsOpen() const;

    /**
     * Get volume ID from Primary Volume Descriptor
     * @return Volume ID string (disc name)
     */
    std::string GetVolumeID() const;

    /**
     * Get the resolved path to the BIN file (set after Open())
     */
    std::string GetBinPath() const;

    /**
     * Get root directory information
     * @return RootDirectoryInfo containing LBA and size
     */
    RootDirectoryInfo GetRootDirectory() const;

    /**
     * List all files in a directory
     * @param path Directory path (empty string = root directory)
     * @return Vector of ISOFileEntry for all files/subdirectories
     */
    std::vector<ISOFileEntry> ListFiles(const std::string& path = "");

    /**
     * Find a file by name in the root directory
     * @param path Filename to search for (e.g., "SYSTEM.CNF" or "SCUS_942.36")
     * @param entry Output ISOFileEntry to populate with file info
     * @return true if found, false if not found
     */
    bool FindFile(const std::string& path, ISOFileEntry& entry);

    /**
     * Read entire file contents into buffer
     * @param path Filename to read
     * @param buffer Buffer to read into (must be large enough)
     * @param max_size Maximum bytes to read (buffer size limit)
     * @return Number of bytes read (0 if error or file not found)
     */
    size_t ReadFile(const std::string& path, uint8_t* buffer, size_t max_size);

    /**
     * Get file size without reading data
     * @param path Filename to query
     * @return File size in bytes (0 if file not found)
     */
    size_t GetFileSize(const std::string& path);

private:
    /**
     * Parse the Primary Volume Descriptor (sector 16)
     * Extracts volume ID and root directory information
     * @return true if parsed successfully, false otherwise
     */
    bool ParseVolumeDescriptor();

    /**
     * Helper: Read a 32-bit value in both-endian format (733 format)
     * @param data Pointer to 8-byte both-endian value
     * @return 32-bit value (reads little-endian half)
     */
    uint32_t Read733(const uint8_t* data) const;

    /**
     * Helper: Parse a directory record from raw sector data
     * @param data Pointer to directory record data
     * @param entry Output ISOFileEntry to populate
     * @return true if parsed successfully, false if invalid record
     */
    bool ParseDirectoryRecord(const uint8_t* data, ISOFileEntry& entry) const;

    /**
     * Helper: List files in a directory given its LBA and size
     * Used for subdirectory navigation
     */
    std::vector<ISOFileEntry> ListFilesByLBA(uint32_t lba, uint32_t dir_size);

    std::ifstream file_;
    bool is_open_;
    std::string volume_id_;
    std::string bin_path_;
    RootDirectoryInfo root_dir_;
};

} // namespace PS1
