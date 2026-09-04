#include "iso_reader.h"
#include "cue_sheet.h"
#include <libchdr/cdrom.h>
#include <libchdr/chd.h>
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <limits>

namespace PS1 {

// PS1 CD-ROM sector size (Mode 2, Form 1 user data)
constexpr size_t SECTOR_SIZE = 2048;

// Full sector size including headers/subchannel (2352 bytes for raw BIN files)
constexpr size_t RAW_SECTOR_SIZE = 2352;

// Offset to user data in raw sector (Mode 2, Form 1)
constexpr size_t RAW_DATA_OFFSET = 24;

// Primary Volume Descriptor location
constexpr uint32_t PVD_SECTOR = 16;

constexpr uint32_t CD_FRAMES_PER_SECOND = 75;
constexpr uint32_t CD_FRAMES_PER_MINUTE = 60 * CD_FRAMES_PER_SECOND;
constexpr uint32_t CD_LEAD_IN_FRAMES = 2 * CD_FRAMES_PER_SECOND;

static bool valid_bcd(uint8_t value) {
    return (value & 0x0f) <= 9 && (value >> 4) <= 9;
}

static uint32_t bcd_to_binary(uint8_t value) {
    return (value >> 4) * 10u + (value & 0x0f);
}

static uint8_t binary_to_bcd(uint32_t value) {
    return static_cast<uint8_t>(((value / 10u) << 4) | (value % 10u));
}

static uint16_t subq_crc(const uint8_t* data) {
    uint16_t crc = 0;
    for (int i = 0; i < 10; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                : static_cast<uint16_t>(crc << 1);
    }
    return static_cast<uint16_t>(~crc);
}

enum class CHDTrackMode {
    Mode1,
    Mode1Raw,
    Mode2,
    Mode2Form1,
    Mode2Form2,
    Mode2FormMix,
    Mode2Raw,
    Audio,
};

struct CHDSpan {
    uint32_t disc_start = 0;
    uint32_t sector_count = 0;
    uint64_t chd_frame_start = 0;
    CHDTrackMode mode = CHDTrackMode::Mode2Raw;
    bool stored = true;
};

struct CHDState {
    chd_file* file = nullptr;
    uint32_t hunk_bytes = 0;
    uint32_t frames_per_hunk = 0;
    uint32_t current_hunk = std::numeric_limits<uint32_t>::max();
    uint32_t disc_sector_count = 0;
    std::vector<uint8_t> hunk;
    std::vector<CHDSpan> spans;

    ~CHDState() {
        if (file) chd_close(file);
    }
};

static bool parse_chd_track_mode(const char* text, CHDTrackMode& mode) {
    if (std::strcmp(text, "MODE1") == 0) mode = CHDTrackMode::Mode1;
    else if (std::strcmp(text, "MODE1_RAW") == 0) mode = CHDTrackMode::Mode1Raw;
    else if (std::strcmp(text, "MODE2") == 0) mode = CHDTrackMode::Mode2;
    else if (std::strcmp(text, "MODE2_FORM1") == 0) mode = CHDTrackMode::Mode2Form1;
    else if (std::strcmp(text, "MODE2_FORM2") == 0) mode = CHDTrackMode::Mode2Form2;
    else if (std::strcmp(text, "MODE2_FORM_MIX") == 0) mode = CHDTrackMode::Mode2FormMix;
    else if (std::strcmp(text, "MODE2_RAW") == 0) mode = CHDTrackMode::Mode2Raw;
    else if (std::strcmp(text, "AUDIO") == 0) mode = CHDTrackMode::Audio;
    else return false;
    return true;
}

static const CHDSpan* chd_span_for_lba(const CHDState& chd, uint32_t lba) {
    for (const CHDSpan& span : chd.spans) {
        if (lba >= span.disc_start &&
            lba - span.disc_start < span.sector_count) {
            return &span;
        }
    }
    return nullptr;
}

static bool read_chd_raw(CHDState& chd, uint32_t lba, uint8_t* buffer,
                         CHDTrackMode* mode_out) {
    const CHDSpan* span = chd_span_for_lba(chd, lba);
    if (!span) return false;
    if (mode_out) *mode_out = span->mode;
    if (!span->stored) {
        std::memset(buffer, 0, RAW_SECTOR_SIZE);
        return true;
    }

    const uint64_t frame =
        span->chd_frame_start + (uint64_t)(lba - span->disc_start);
    const uint64_t hunk_index64 = frame / chd.frames_per_hunk;
    if (hunk_index64 > std::numeric_limits<uint32_t>::max()) return false;
    const uint32_t hunk_index = (uint32_t)hunk_index64;
    const size_t hunk_offset =
        (size_t)(frame % chd.frames_per_hunk) * CD_FRAME_SIZE;
    if (hunk_offset + CD_MAX_SECTOR_DATA > chd.hunk.size()) return false;

    if (chd.current_hunk != hunk_index) {
        if (chd_read(chd.file, hunk_index, chd.hunk.data()) != CHDERR_NONE) {
            chd.current_hunk = std::numeric_limits<uint32_t>::max();
            return false;
        }
        chd.current_hunk = hunk_index;
    }

    std::memcpy(buffer, chd.hunk.data() + hunk_offset, RAW_SECTOR_SIZE);
    if (span->mode == CHDTrackMode::Audio) {
        // CD audio in CHD is canonical big-endian PCM. The existing BIN/CUE
        // path and CD-ROM mixer exchange little-endian signed samples.
        for (size_t i = 0; i < RAW_SECTOR_SIZE; i += 2)
            std::swap(buffer[i], buffer[i + 1]);
    }
    return true;
}

ISOReader::ISOReader()
    : is_open_(false) {
    root_dir_.lba = 0;
    root_dir_.size = 0;
}

ISOReader::~ISOReader() {
    Close();
}

bool ISOReader::Open(const std::string& filename) {
    // Close any previously opened image (also resets tracks/segments)
    Close();

    // Check if file exists
    if (!std::filesystem::exists(filename)) {
        return false;
    }

    if (PSXRecompV4::path_has_extension_ci(
            std::filesystem::path(filename), ".chd")) {
        auto state = std::make_unique<CHDState>();
        if (chd_open(filename.c_str(), CHD_OPEN_READ, nullptr, &state->file) !=
            CHDERR_NONE) {
            return false;
        }
        const chd_header* header = chd_get_header(state->file);
        if (!header || header->hunkbytes == 0 ||
            (header->hunkbytes % CD_FRAME_SIZE) != 0) {
            return false;
        }
        state->hunk_bytes = header->hunkbytes;
        state->frames_per_hunk = header->hunkbytes / CD_FRAME_SIZE;
        state->hunk.resize(header->hunkbytes);

        uint32_t disc_lba = 0;
        uint64_t chd_frame = 0;
        for (uint32_t index = 0; index < CD_MAX_TRACKS; ++index) {
            char metadata[256] = {};
            uint32_t metadata_length = 0;
            int number = 0;
            int frames = 0;
            int pregap = 0;
            int postgap = 0;
            char type[64] = {};
            char subtype[64] = {};
            char pgtype[64] = {};
            char pgsub[64] = {};

            chd_error err = chd_get_metadata(
                state->file, CDROM_TRACK_METADATA2_TAG, index,
                metadata, sizeof(metadata) - 1, &metadata_length, nullptr, nullptr);
            bool v2 = err == CHDERR_NONE;
            if (v2) {
                if (std::sscanf(
                        metadata,
                        "TRACK:%d TYPE:%63s SUBTYPE:%63s FRAMES:%d "
                        "PREGAP:%d PGTYPE:%63s PGSUB:%63s POSTGAP:%d",
                        &number, type, subtype, &frames, &pregap,
                        pgtype, pgsub, &postgap) != 8) {
                    return false;
                }
            } else {
                err = chd_get_metadata(
                    state->file, CDROM_TRACK_METADATA_TAG, index,
                    metadata, sizeof(metadata) - 1, &metadata_length, nullptr, nullptr);
                if (err == CHDERR_METADATA_NOT_FOUND) break;
                if (err != CHDERR_NONE ||
                    std::sscanf(metadata,
                        "TRACK:%d TYPE:%63s SUBTYPE:%63s FRAMES:%d",
                        &number, type, subtype, &frames) != 4) {
                    return false;
                }
            }

            CHDTrackMode mode;
            if (number != (int)index + 1 || frames <= 0 ||
                !parse_chd_track_mode(type, mode)) {
                return false;
            }

            const bool pregap_stored =
                v2 && pregap > 0 && (pgtype[0] == 'V' || pgtype[0] == 'v');
            const uint32_t stored_pregap =
                pregap_stored ? std::min<uint32_t>((uint32_t)pregap,
                                                    (uint32_t)frames) : 0;
            const uint32_t virtual_pregap =
                !pregap_stored && pregap > 0 ? (uint32_t)pregap : 0;
            const uint32_t pregap_lba = disc_lba;

            if (virtual_pregap) {
                state->spans.push_back(
                    {disc_lba, virtual_pregap, 0, mode, false});
                disc_lba += virtual_pregap;
            }

            state->spans.push_back(
                {disc_lba, (uint32_t)frames, chd_frame, mode, true});
            CDTrack track;
            track.number = number;
            track.is_audio = mode == CHDTrackMode::Audio;
            track.start_lba = disc_lba + stored_pregap;
            track.pregap_lba = pregap_lba;
            tracks_.push_back(track);

            disc_lba += (uint32_t)frames;
            chd_frame += (uint32_t)frames;
            chd_frame = (chd_frame + CD_TRACK_PADDING - 1) &
                        ~(uint64_t)(CD_TRACK_PADDING - 1);
        }
        if (tracks_.empty()) return false;

        state->disc_sector_count = disc_lba;
        bin_path_ = filename;
        chd_ = std::move(state);
        is_open_ = true;
        if (!ParseVolumeDescriptor()) {
            volume_id_.clear();
            root_dir_.lba = 0;
            root_dir_.size = 0;
        }
        if (!LoadSBICompanion(filename)) {
            Close();
            return false;
        }
        return true;
    }

    // Ordered list of BINARY files backing the disc. A bare .bin/.iso is a
    // single-entry list; a .cue contributes one entry per FILE line (redump
    // multi-track dumps ship one file per track).
    std::vector<std::string> bin_files;

    // Tracks parsed from the cue. INDEX times in a cue are relative to the
    // OWNING FILE, so remember which file each track belongs to and convert
    // to disc-relative LBAs once the segment table (with each file's first
    // disc sector) is built below.
    using PendingTrack = PSXRecompV4::CueTrackRef;
    std::vector<PendingTrack> pending_tracks;

    if (PSXRecompV4::path_has_extension_ci(std::filesystem::path(filename), ".cue")) {
        // Parse the .cue via the shared parser: resolve every FILE, AND build
        // the track TOC from the TRACK/INDEX lines. Single-file cues keep
        // their historical behavior; multi-file cues (data track + CD-DA audio
        // tracks in separate .bin files) map each file to a contiguous run of
        // disc sectors, concatenated in cue order.
        const PSXRecompV4::CueSheet sheet =
            PSXRecompV4::parse_cue_sheet(std::filesystem::path(filename));

        // A cue we cannot read, one that names no FILE, or one whose payload is
        // not raw BINARY (WAVE/MP3 have no fixed sector geometry — mis-mapping
        // the TOC would be worse than refusing the image) has nothing to mount.
        if (!sheet.opened || sheet.files.empty() || sheet.has_non_binary_file()) {
            return false;
        }

        bin_files.reserve(sheet.files.size());
        for (const PSXRecompV4::CueFileRef& f : sheet.files) {
            bin_files.push_back(f.path.string());
        }
        pending_tracks = sheet.tracks;
    } else {
        bin_files.push_back(filename);
    }

    // Build the segment table: open every file and lay it out at the next
    // disc-relative sector. Every file must open — a multi-file dump with a
    // missing track file would otherwise silently read the wrong sectors.
    uint32_t next_lba = 0;
    for (const std::string& bin_name : bin_files) {
        BinSegment seg;
        seg.path = bin_name;
        seg.file.open(bin_name, std::ios::binary);
        if (!seg.file.is_open()) {
            Close();
            return false;
        }
        seg.file.seekg(0, std::ios::end);
        const std::streampos file_size = seg.file.tellg();
        seg.file.clear();
        if (file_size <= 0) {
            Close();
            return false;
        }
        const uint64_t size = static_cast<uint64_t>(file_size);
        seg.raw          = (size % RAW_SECTOR_SIZE) == 0;
        seg.sector_count = static_cast<uint32_t>(size / (seg.raw ? RAW_SECTOR_SIZE
                                                                 : SECTOR_SIZE));
        seg.start_lba    = next_lba;
        next_lba += seg.sector_count;
        segments_.push_back(std::move(seg));
    }

    // Convert the pending cue tracks to disc-relative LBAs.
    for (const PendingTrack& p : pending_tracks) {
        CDTrack t;
        t.number    = p.number;
        t.is_audio  = p.is_audio;
        t.start_lba = segments_[p.file_index].start_lba + p.index01;
        t.pregap_lba = segments_[p.file_index].start_lba +
                       (p.has_index00 ? p.index00 : p.index01);
        tracks_.push_back(t);
    }

    // Synthesize a single data track for a bare .bin/.iso or a .cue with no
    // parseable TRACK entries, so TrackCount() is always >= 1.
    if (tracks_.empty()) {
        CDTrack t;
        t.number = 1; t.is_audio = false; t.start_lba = 0; t.pregap_lba = 0;
        tracks_.push_back(t);
    }

    // Store the resolved data-track path for callers
    bin_path_ = segments_.front().path;

    is_open_ = true;

    // Parse the volume descriptor to extract filesystem metadata when
    // present. Runtime CD-ROM access only needs sector reads, so keep the
    // image mounted even if an ISO9660 header is missing or nonstandard.
    if (!ParseVolumeDescriptor()) {
        volume_id_.clear();
        root_dir_.lba = 0;
        root_dir_.size = 0;
    }

    if (!LoadSBICompanion(filename)) {
        Close();
        return false;
    }

    return true;
}

void ISOReader::Close() {
    chd_.reset();
    for (BinSegment& seg : segments_) {
        if (seg.file.is_open()) {
            seg.file.close();
        }
    }
    segments_.clear();
    tracks_.clear();
    subq_replacements_.clear();
    bin_path_.clear();
    volume_id_.clear();
    root_dir_.lba = 0;
    root_dir_.size = 0;
    is_open_ = false;
}

bool ISOReader::LoadSBICompanion(const std::string& image_path) {
    std::filesystem::path path(image_path);
    path.replace_extension(".sbi");
    if (!std::filesystem::exists(path)) return true;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    char header[4] = {};
    file.read(header, sizeof(header));
    if (file.gcount() != sizeof(header) ||
        std::memcmp(header, "SBI\0", sizeof(header)) != 0) {
        return false;
    }

    while (true) {
        uint8_t record[14] = {};
        file.read(reinterpret_cast<char*>(record), sizeof(record));
        if (file.gcount() == 0 && file.eof()) break;
        if (file.gcount() != sizeof(record) || record[3] != 1 ||
            !valid_bcd(record[0]) || !valid_bcd(record[1]) ||
            !valid_bcd(record[2])) {
            return false;
        }
        const uint32_t absolute_frame =
            bcd_to_binary(record[0]) * CD_FRAMES_PER_MINUTE +
            bcd_to_binary(record[1]) * CD_FRAMES_PER_SECOND +
            bcd_to_binary(record[2]);
        if (absolute_frame < CD_LEAD_IN_FRAMES) return false;
        const uint32_t lba = absolute_frame - CD_LEAD_IN_FRAMES;
        std::array<uint8_t, 12> subq = {};
        std::memcpy(subq.data(), record + 4, 10);
        const uint16_t invalid_crc = static_cast<uint16_t>(subq_crc(subq.data()) ^ 0xffff);
        subq[10] = static_cast<uint8_t>(invalid_crc);
        subq[11] = static_cast<uint8_t>(invalid_crc >> 8);
        if (!subq_replacements_.emplace(lba, subq).second) return false;
    }
    return !subq_replacements_.empty();
}

bool ISOReader::ReadSubChannelQ(uint32_t lba, uint8_t* buffer, bool* valid) const {
    if (!buffer || !valid || !is_open_) return false;
    const auto replacement = subq_replacements_.find(lba);
    if (replacement != subq_replacements_.end()) {
        std::memcpy(buffer, replacement->second.data(), replacement->second.size());
        *valid = false;
        return true;
    }

    int track = 1;
    for (const CDTrack& candidate : tracks_) {
        if (candidate.start_lba > lba) break;
        track = candidate.number;
    }
    const uint32_t track_lba = TrackStartLBA(track);
    const uint32_t relative = lba >= track_lba ? lba - track_lba : 0;
    const uint32_t absolute = lba + 150;
    std::memset(buffer, 0, 12);
    buffer[0] = TrackIsAudio(track) ? 0x01 : 0x41;
    buffer[1] = binary_to_bcd(static_cast<uint32_t>(track));
    buffer[2] = 0x01;
    buffer[3] = binary_to_bcd(relative / CD_FRAMES_PER_MINUTE);
    buffer[4] = binary_to_bcd((relative / CD_FRAMES_PER_SECOND) % 60);
    buffer[5] = binary_to_bcd(relative % CD_FRAMES_PER_SECOND);
    buffer[6] = binary_to_bcd(absolute / CD_FRAMES_PER_MINUTE);
    buffer[7] = binary_to_bcd((absolute / CD_FRAMES_PER_SECOND) % 60);
    buffer[8] = binary_to_bcd(absolute % CD_FRAMES_PER_SECOND);
    const uint16_t crc = subq_crc(buffer);
    buffer[10] = static_cast<uint8_t>(crc);
    buffer[11] = static_cast<uint8_t>(crc >> 8);
    *valid = true;
    return true;
}

BinSegment* ISOReader::SegmentForLBA(uint32_t lba) {
    // Linear scan: images carry a handful of segments (one per track file).
    for (BinSegment& seg : segments_) {
        if (lba >= seg.start_lba && lba - seg.start_lba < seg.sector_count) {
            return &seg;
        }
    }
    return nullptr;
}

bool ISOReader::ReadSector(uint32_t lba, uint8_t* buffer) {
    if (!is_open_ || !buffer) {
        return false;
    }

    if (chd_) {
        uint8_t raw[RAW_SECTOR_SIZE];
        CHDTrackMode mode;
        if (!read_chd_raw(*chd_, lba, raw, &mode)) return false;
        const size_t offset =
            (mode == CHDTrackMode::Mode1 ||
             mode == CHDTrackMode::Mode1Raw) ? 16 : RAW_DATA_OFFSET;
        std::memcpy(buffer, raw + offset, SECTOR_SIZE);
        return true;
    }

    BinSegment* seg = SegmentForLBA(lba);
    if (!seg) {
        return false;
    }
    const uint32_t local_lba = lba - seg->start_lba;

    // Clear any error flags from a previous failed read
    seg->file.clear();

    if (seg->raw) {
        // Raw BIN format - read full sector, extract user data
        std::streampos offset =
            static_cast<std::streampos>(local_lba) * RAW_SECTOR_SIZE + RAW_DATA_OFFSET;
        seg->file.seekg(offset, std::ios::beg);

        if (!seg->file.good()) {
            seg->file.clear();
            return false;
        }

        seg->file.read(reinterpret_cast<char*>(buffer), SECTOR_SIZE);
        std::streamsize bytes_read = seg->file.gcount();
        bool success = (bytes_read == SECTOR_SIZE);
        seg->file.clear();
        return success;
    } else {
        // ISO format - sectors are already 2048 bytes
        std::streampos offset = static_cast<std::streampos>(local_lba) * SECTOR_SIZE;
        seg->file.seekg(offset, std::ios::beg);

        if (!seg->file.good()) {
            seg->file.clear();
            return false;
        }

        seg->file.read(reinterpret_cast<char*>(buffer), SECTOR_SIZE);
        std::streamsize bytes_read = seg->file.gcount();
        bool success = (bytes_read == SECTOR_SIZE);
        seg->file.clear();
        return success;
    }
}

bool ISOReader::ReadRawSector(uint32_t lba, uint8_t* buffer) {
    if (!is_open_ || !buffer) {
        return false;
    }

    if (chd_) return read_chd_raw(*chd_, lba, buffer, nullptr);

    BinSegment* seg = SegmentForLBA(lba);
    if (!seg || !seg->raw) {
        return false;
    }
    const uint32_t local_lba = lba - seg->start_lba;

    seg->file.clear();
    std::streampos offset = static_cast<std::streampos>(local_lba) * RAW_SECTOR_SIZE;
    seg->file.seekg(offset, std::ios::beg);
    if (!seg->file.good()) {
        seg->file.clear();
        return false;
    }

    seg->file.read(reinterpret_cast<char*>(buffer), RAW_SECTOR_SIZE);
    std::streamsize bytes_read = seg->file.gcount();
    bool success = (bytes_read == RAW_SECTOR_SIZE);
    seg->file.clear();
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
    if (chd_) return chd_->disc_sector_count;
    if (segments_.empty()) return 0;

    const BinSegment& last = segments_.back();
    return last.start_lba + last.sector_count;
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
