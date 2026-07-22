// disc_identity.h — disc-image identification + verification.
//
// Single source of truth for "is this the right disc?". Reads a .cue (or raw
// .bin/.iso), checks for the ISO9660 PVD, extracts the volume id and the
// PlayStation boot serial (from SYSTEM.CNF), derives the region from the
// serial prefix, and optionally compares against an expected serial / CRC32.
//
// Used both by the runtime's launch-time disc check (runtime/src/main.cpp)
// and by the integrated launcher's "Disc verified" badge (recomp-ui), so
// the two never drift apart.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace PSXRecompV4 {

struct DiscIdentity {
    bool        opened       = false;  // data file (cue-referenced bin or raw image) opened
    bool        has_header   = false;  // ISO9660 "CD001" PVD found at a PS1 disc offset
    std::string volume_id;             // PVD volume identifier (trimmed), "" if none
    std::string detected_serial;       // serial parsed from SYSTEM.CNF BOOT line, normalized
                                       // to the "SCUS-94236" form; "" if not found
    std::string region;                // "NTSC-U" | "NTSC-J" | "PAL" derived from the serial
                                       // prefix (detected first, else expected); "" if unknown

    // Serial comparison vs the expected game id (only meaningful when one was given).
    bool        expected_serial_given = false;
    bool        serial_matches        = false;  // expected serial present in early disc metadata

    // Full-file CRC32 (IEEE 802.3 over the data track / image bytes).
    bool        crc_computed   = false;
    uint32_t    crc            = 0;
    bool        expected_crc_given = false;
    bool        crc_matches        = false;

    std::string detail;  // human-readable note (failure reason or extra info)
};

// Identify and (optionally) verify a disc image.
//   path             : a .cue file or a raw .bin/.iso image
//   expected_serial  : the game id, e.g. "SCUS-94236" ("" skips the serial check)
//   expected_crc     : full-file CRC32 to match against
//   has_expected_crc : whether expected_crc is meaningful
//   compute_crc      : compute the full-file CRC32 (streamed; can be slow on big
//                      images — callers typically only request it when there is an
//                      expected CRC to compare against)
DiscIdentity identify_disc(const std::filesystem::path& path,
                           const std::string& expected_serial,
                           uint32_t expected_crc, bool has_expected_crc,
                           bool compute_crc);

}  // namespace PSXRecompV4
