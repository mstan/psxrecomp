#pragma once

#include <cstdint>
#include <string>
#include <fstream>
#include <memory>
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

/**
 * A CD track parsed from the .cue sheet (or the synthesized single data
 * track for a bare .bin/.iso). Multi-track discs (Tomba 2 etc.) carry a
 * Red Book CD-DA audio track after the data track; the CD model's TOC
 * commands (GetTN/GetTD) must report them.
 */
struct CDTrack {
    int      number;     // 1-based track number
    bool     is_audio;   // true = CD-DA audio (Red Book); false = data
    uint32_t start_lba;  // disc-relative start LBA (cue INDEX 01; track 1 = 0)
    uint32_t pregap_lba = 0; // cue INDEX 00, or INDEX 01 when no pregap exists
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
     * Read a raw 2352-byte sector from a BIN image.
     * @param lba Logical Block Address (sector number)
     * @param buffer Buffer to read into (must be at least 2352 bytes)
     * @return true if the image stores raw sectors and read succeeded
     */
    bool ReadRawSector(uint32_t lba, uint8_t* buffer);

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
     * Get number of addressable sectors in the mounted image.
     * Raw BIN images count 2352-byte sectors; cooked ISO images count 2048-byte sectors.
     */
    uint32_t GetSectorCount();

    /**
     * CD-track TOC accessors (multi-track .cue support).
     * TrackCount() is >= 1 (a bare image synthesizes one data track).
     * TrackStartLBA(n)/TrackIsAudio(n) take a 1-based track number.
     */
    int      TrackCount() const;
    uint32_t TrackStartLBA(int track) const;
    uint32_t TrackPregapLBA(int track) const;
    bool     TrackIsAudio(int track) const;

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
    struct ImageFile {
        std::string path;
        std::ifstream stream;
        uint32_t base_lba = 0;
        uint32_t sector_count = 0;
        uint32_t sector_size = 0;
        uint32_t data_offset = 0;
    };

    ImageFile* FindImageFile(uint32_t lba);

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

    bool is_open_;
    std::string volume_id_;
    std::string bin_path_;
    RootDirectoryInfo root_dir_;
    std::vector<std::unique_ptr<ImageFile>> files_;
    std::vector<CDTrack> tracks_;   // from the .cue TOC; >=1 entry after Open()
};

} // namespace PS1
