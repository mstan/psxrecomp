// disc_identity.h — disc-image identification + verification.
//
// Single source of truth for "is this the right disc?". Reads a .cue, raw
// .bin/.iso/.img/.car, or CHD, checks for the ISO9660 PVD, extracts the volume id and the
// PlayStation boot serial (from SYSTEM.CNF), derives the region from the
// serial prefix, and optionally compares against an expected serial / CRC32.
//
// Also mounts the resolved path via ISOReader to capture TOC geometry
// (track count, lead-out, per-track LBAs) and a stable disc_fp fingerprint
// used by netplay peer matching — data-track CRC alone cannot catch a
// Track-01-only mount vs a full Redump multi-track cue.
//
// Used both by the runtime's launch-time disc check (runtime/src/main.cpp)
// and by the shared launcher's "Disc verified" badge, so
// the two never drift apart.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace PSXRecompV4 {

struct DiscIdentity {
    bool        opened       = false;  // data image opened (including decoded CHD)
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

    // Mount / TOC (ISOReader on resolve_disc_path().mount).
    bool        from_cue       = false;  // mount path is a .cue
    bool        cue_fallback   = false;  // resolver fell back from a broken cue to a bin
    bool        upgraded_to_cue = false; // caller picked bin; mounted owning cue
    bool        toc_opened     = false;  // ISOReader::Open succeeded on mount
    int         track_count    = 0;      // >= 1 when toc_opened
    uint32_t    leadout_lba    = 0;      // iso sector count (GetTD track 0)
    std::string disc_fp;                 // lowercase hex SHA-256 of canonical TOC

    // Netplay gate (filled when NetplayDiscExpect is applied).
    bool        netplay_ok     = true;
    std::string netplay_detail;          // why netplay_ok is false

    std::string detail;  // human-readable note (failure reason or extra info)
};

// Optional title policy from game.toml [netplay] (0 / empty = do not check).
struct NetplayDiscExpect {
    bool        require_cue = false;
    int         required_tracks = 0;           // exact iso_track_count; 0 = skip
    bool        has_required_leadout = false;
    uint32_t    required_leadout_lba = 0;
    std::string required_disc_fp;              // exact fingerprint; empty = skip
};

// Identify and (optionally) verify a disc image.
//   path             : a .cue, raw .bin/.iso/.img/.car, or .chd image
//   expected_serial  : the game id, e.g. "SCUS-94236" ("" skips the serial check)
//   expected_crc     : full-file CRC32 to match against
//   has_expected_crc : whether expected_crc is meaningful
//   compute_crc      : compute the full-file CRC32 (streamed; can be slow on big
//                      images — callers typically only request it when there is an
//                      expected CRC to compare against)
//   netplay_expect   : optional TOC / cue policy; nullptr skips netplay_ok checks
//                      (disc_fp / track_count are still computed when mount opens)
DiscIdentity identify_disc(const std::filesystem::path& path,
                           const std::string& expected_serial,
                           uint32_t expected_crc, bool has_expected_crc,
                           bool compute_crc,
                           const NetplayDiscExpect* netplay_expect = nullptr);

// Apply / re-apply netplay mount policy onto an already-identified disc.
void apply_netplay_disc_expect(DiscIdentity& id, const NetplayDiscExpect& expect);

}  // namespace PSXRecompV4
