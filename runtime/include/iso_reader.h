#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <fstream>
#include <memory>
#include <unordered_map>
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

struct CHDState;

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
 * track for a bare .bin/.iso). Multi-track discs can carry a
 * Red Book CD-DA audio track after the data track; the CD model's TOC
 * commands (GetTN/GetTD) must report them.
 */
struct CDTrack {
    int      number;     // 1-based track number
    bool     is_audio;   // true = CD-DA audio (Red Book); false = data
    uint32_t start_lba;  // disc-relative start LBA (cue INDEX 01; track 1 = 0).
                         // Single-FILE cues: equals the .bin-relative INDEX time.
                         // Multi-FILE cues (redump "(Track N).bin" dumps): the
                         // owning file's first disc sector plus its INDEX time.
    uint32_t pregap_lba; // disc-relative INDEX 00, or INDEX 01 when absent
};

/**
 * One BINARY file backing a contiguous run of disc sectors. Single-file
 * images have exactly one segment; redump-style dumps have one per track
 * file, concatenated in cue order to form the flat disc-sector space that
 * ReadSector()/ReadRawSector() address.
 */
struct BinSegment {
    std::string   path;          // resolved on-disk path
    std::ifstream file;          // opened for the reader's lifetime
    uint32_t      start_lba;     // first disc-relative sector of this file
    uint32_t      sector_count;  // sectors stored in this file
    bool          raw;           // 2352-byte raw sectors (BIN) vs 2048 (ISO)
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
     * @param filename Path to .iso, .bin, .img, .car, .cue, or .chd file
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
     * Read generated or SBI-replacement subchannel Q for a disc LBA.
     * SBI records intentionally carry invalid CRC data; valid is false for
     * those sectors so the CD controller can retain its last valid Q packet.
     */
    bool ReadSubChannelQ(uint32_t lba, uint8_t* buffer, bool* valid) const;
    bool HasSubChannelReplacements() const { return !subq_replacements_.empty(); }

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
     * TrackStartLBA(n)/TrackPregapLBA(n)/TrackIsAudio(n) take a 1-based track number.
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

    /**
     * Helper: find the BinSegment containing a disc-relative LBA.
     * @return segment pointer, or nullptr when lba is past the disc end
     */
    BinSegment* SegmentForLBA(uint32_t lba);

    bool LoadSBICompanion(const std::string& image_path);

    bool is_open_;
    std::string volume_id_;
    std::string bin_path_;          // first (data) segment; kept for callers
    RootDirectoryInfo root_dir_;
    std::vector<CDTrack> tracks_;   // from the .cue TOC; >=1 entry after Open()
    std::vector<BinSegment> segments_;  // cue FILE entries in disc order; >=1 after Open()
    std::unique_ptr<CHDState> chd_; // present only for a directly mounted CHD
    std::unordered_map<uint32_t, std::array<uint8_t, 12>> subq_replacements_;
};

} // namespace PS1
