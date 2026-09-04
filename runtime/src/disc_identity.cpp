// disc_identity.cpp — see disc_identity.h.
//
// The header detection mirrors the proven raw-sector logic previously inlined
// in main.cpp (handles both 2352-byte raw and 2048-byte cooked sector layouts,
// which is what real .bin/.cue and .iso dumps use respectively).

#include "disc_identity.h"

#include "crc32.h"
#include "disc_path.h"
#include "iso_reader.h"
#include "psx_sha256.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace PSXRecompV4 {

namespace {

std::string uppercase_ascii(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

bool read_at(std::ifstream& f, uint64_t offset, uint8_t* out, size_t len) {
    f.clear();
    f.seekg((std::streamoff)offset, std::ios::beg);
    if (!f.good()) return false;
    f.read(reinterpret_cast<char*>(out), (std::streamsize)len);
    return f.gcount() == (std::streamsize)len;
}

bool is_iso_pvd(const uint8_t* sector) {
    return sector[0] == 0x01 && std::memcmp(&sector[1], "CD001", 5) == 0 && sector[6] == 0x01;
}

// "SCUS-94236" -> region. PSX serial prefixes encode the territory.
std::string region_from_serial(const std::string& serial) {
    if (serial.size() < 4) return std::string();
    const std::string p = uppercase_ascii(serial.substr(0, 4));
    if (p == "SCUS" || p == "SLUS" || p == "LSP9") return "NTSC-U";
    if (p == "SCES" || p == "SLES" || p == "SCED" || p == "SLED") return "PAL";
    if (p == "SCPS" || p == "SLPS" || p == "SLPM" || p == "SCPM" || p == "ESPM") return "NTSC-J";
    return std::string();
}

// Normalize a scanned EXE-style id "SCUS_942.36" to the game-id form
// "SCUS-94236". Pass-through for things already in game-id form. Returns ""
// if the token doesn't look like a serial.
std::string normalize_serial(const std::string& raw) {
    std::string s;
    for (char c : raw) {
        if (std::isalnum((unsigned char)c)) s += (char)std::toupper((unsigned char)c);
        // drop separators ('_', '.', '-')
    }
    // Expect 4 letters + 5 digits.
    if (s.size() < 9) return std::string();
    for (int i = 0; i < 4; i++) if (!std::isalpha((unsigned char)s[i])) return std::string();
    for (int i = 4; i < 9; i++) if (!std::isdigit((unsigned char)s[i])) return std::string();
    return s.substr(0, 4) + "-" + s.substr(4, 5);
}

// Scan the SYSTEM.CNF text region for the BOOT serial. PSX SYSTEM.CNF holds a
// line like `BOOT = cdrom:\SCUS_942.36;1`. We look for "cdrom:" and read the
// following filename token, then normalize it.
std::string scan_boot_serial(const std::vector<uint8_t>& buf) {
    static const char kKey[] = "cdrom:";
    const size_t klen = sizeof(kKey) - 1;
    if (buf.size() < klen) return std::string();
    for (size_t i = 0; i + klen < buf.size(); i++) {
        bool hit = true;
        for (size_t k = 0; k < klen; k++) {
            char a = (char)std::tolower((unsigned char)buf[i + k]);
            if (a != kKey[k]) { hit = false; break; }
        }
        if (!hit) continue;
        // Read the token after "cdrom:", skipping any leading path separators.
        size_t j = i + klen;
        while (j < buf.size() && (buf[j] == '\\' || buf[j] == '/')) j++;
        std::string token;
        while (j < buf.size()) {
            char c = (char)buf[j];
            if (c == ';' || c == '\r' || c == '\n' || c == ' ' || c == '\t' || c == 0) break;
            token += c;
            j++;
            if (token.size() > 32) break;
        }
        const size_t slash = token.find_last_of("\\/");
        if (slash != std::string::npos) token = token.substr(slash + 1);
        const std::string norm = normalize_serial(token);
        if (!norm.empty()) return norm;
    }
    return std::string();
}

uint32_t crc32_stream(std::ifstream& f) {
    f.clear();
    f.seekg(0, std::ios::beg);
    uint32_t crc = 0xFFFFFFFFu;
    std::vector<uint8_t> chunk(1u << 20);  // 1 MiB
    while (f.good()) {
        f.read(reinterpret_cast<char*>(chunk.data()), (std::streamsize)chunk.size());
        const std::streamsize got = f.gcount();
        if (got <= 0) break;
        crc = crc32_update(crc, chunk.data(), (size_t)got);
    }
    return crc ^ 0xFFFFFFFFu;
}

std::string hex_sha256(const std::string& canonical) {
    uint8_t dig[32];
    psx_sha256_compute(reinterpret_cast<const uint8_t*>(canonical.data()),
                       canonical.size(), dig);
    char hex[65];
    for (int i = 0; i < 32; i++)
        std::snprintf(hex + i * 2, 3, "%02x", dig[i]);
    hex[64] = '\0';
    return std::string(hex);
}

void fill_toc_from_mount(DiscIdentity& v, const DiscPathResolution& resolved) {
    v.from_cue = resolved.from_cue;
    v.cue_fallback = resolved.cue_fallback;
    v.upgraded_to_cue = resolved.upgraded_to_cue;

    PS1::ISOReader reader;
    if (!reader.Open(resolved.mount.string())) {
        v.toc_opened = false;
        if (v.detail.empty())
            v.detail = "Could not mount the disc image for TOC inspection "
                       "(missing track FILE or unreadable cue).";
        return;
    }
    v.toc_opened = true;
    v.track_count = reader.TrackCount();
    if (v.track_count < 1) v.track_count = 1;
    v.leadout_lba = reader.GetSectorCount();

    // Canonical TOC string — peers with the same mount geometry share disc_fp.
    // Version prefix lets us evolve the format without silent collisions.
    std::ostringstream toc;
    toc << "psxrecomp-toc-v1\n";
    toc << "tracks=" << v.track_count << "\n";
    toc << "leadout=" << v.leadout_lba << "\n";
    for (int t = 1; t <= v.track_count; t++) {
        toc << "t=" << t
            << ",a=" << (reader.TrackIsAudio(t) ? 1 : 0)
            << ",start=" << reader.TrackStartLBA(t)
            << ",pregap=" << reader.TrackPregapLBA(t) << "\n";
    }
    v.disc_fp = hex_sha256(toc.str());
}

bool is_chd_path(const fs::path& path) {
    std::string ext = path.extension().string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext == ".chd";
}

bool crc32_chd(PS1::ISOReader& disc, uint32_t& result) {
    uint32_t crc = 0xFFFFFFFFu;
    std::vector<uint8_t> sector(2352);
    const uint32_t count = disc.GetSectorCount();
    for (uint32_t lba = 0; lba < count; ++lba) {
        if (!disc.ReadRawSector(lba, sector.data())) return false;
        crc = crc32_update(crc, sector.data(), sector.size());
    }
    result = crc ^ 0xFFFFFFFFu;
    return true;
}

}  // namespace

void apply_netplay_disc_expect(DiscIdentity& id, const NetplayDiscExpect& expect) {
    id.netplay_ok = true;
    id.netplay_detail.clear();

    if (!id.toc_opened) {
        id.netplay_ok = false;
        id.netplay_detail = "Disc TOC could not be opened for netplay.";
        return;
    }
    if (expect.require_cue) {
        if (id.cue_fallback || !id.from_cue) {
            id.netplay_ok = false;
            id.netplay_detail =
                "Netplay requires the full .cue (with all track files), "
                "not a bare .bin / incomplete dump.";
            return;
        }
    }
    if (expect.required_tracks > 0 && id.track_count != expect.required_tracks) {
        id.netplay_ok = false;
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Disc has %d track(s); netplay requires %d "
                      "(use the supported multi-track .cue dump).",
                      id.track_count, expect.required_tracks);
        id.netplay_detail = buf;
        return;
    }
    if (expect.has_required_leadout && id.leadout_lba != expect.required_leadout_lba) {
        id.netplay_ok = false;
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Disc lead-out LBA %u != required %u.",
                      (unsigned)id.leadout_lba,
                      (unsigned)expect.required_leadout_lba);
        id.netplay_detail = buf;
        return;
    }
    if (!expect.required_disc_fp.empty() && id.disc_fp != expect.required_disc_fp) {
        id.netplay_ok = false;
        id.netplay_detail = "Disc TOC fingerprint does not match this build's "
                            "required dump.";
        return;
    }
}

DiscIdentity identify_disc(const fs::path& path,
                           const std::string& expected_serial,
                           uint32_t expected_crc, bool has_expected_crc,
                           bool compute_crc,
                           const NetplayDiscExpect* netplay_expect) {
    DiscIdentity v;
    v.expected_serial_given = !expected_serial.empty();
    v.expected_crc_given    = has_expected_crc;

    // Resolve through the shared picked-path resolver so identity is read from
    // the data track no matter which member of the dump the user handed us —
    // the .cue and its .bin must produce the same verdict, serial and CRC32.
    const DiscPathResolution resolved = resolve_disc_path(path);
    fill_toc_from_mount(v, resolved);

    const fs::path data_path = resolved.data;
    PS1::ISOReader disc;
    if (is_chd_path(resolved.mount)) {
        if (!disc.Open(resolved.mount.string())) {
            v.detail = "Could not open or decode the CHD disc image.";
            return v;
        }
        v.opened = true;

        uint8_t sector[2048];
        if (disc.ReadSector(16, sector) && is_iso_pvd(sector)) {
            v.has_header = true;
        }
        if (!v.has_header) {
            v.detail =
                "No ISO9660 CD001 header was found in the CHD data track.";
            v.region = region_from_serial(expected_serial);
            return v;
        }

        std::string vol((const char*)&sector[40], 32);
        const size_t last = vol.find_last_not_of(" \t\0");
        if (last != std::string::npos) vol.erase(last + 1); else vol.clear();
        v.volume_id = vol;

        const uint32_t scan_sectors =
            std::min<uint32_t>(disc.GetSectorCount(), 8192);
        std::vector<uint8_t> scan((size_t)scan_sectors * 2048);
        bool scan_ok = true;
        for (uint32_t lba = 0; lba < scan_sectors; ++lba) {
            if (!disc.ReadSector(lba, scan.data() + (size_t)lba * 2048)) {
                scan_ok = false;
                break;
            }
        }
        if (scan_ok) {
            v.detected_serial = scan_boot_serial(scan);
            if (v.expected_serial_given) {
                const std::string serial_id = uppercase_ascii(expected_serial);
                std::string exe_id = serial_id;
                if (serial_id.size() == 10 && serial_id[4] == '-')
                    exe_id = serial_id.substr(0, 4) + "_" +
                             serial_id.substr(5, 3) + "." +
                             serial_id.substr(8, 2);
                auto contains = [&](const std::string& needle) {
                    if (needle.empty() || needle.size() > scan.size()) return false;
                    return std::search(scan.begin(), scan.end(),
                        needle.begin(), needle.end()) != scan.end();
                };
                v.serial_matches = contains(serial_id) || contains(exe_id) ||
                    (!v.detected_serial.empty() &&
                     v.detected_serial == serial_id);
            }
        } else {
            v.detail = "The CHD header is present, but its data track could not be scanned.";
        }
        v.region = region_from_serial(
            !v.detected_serial.empty() ? v.detected_serial : expected_serial);
        if (compute_crc) {
            v.crc_computed = crc32_chd(disc, v.crc);
            if (!v.crc_computed) {
                v.detail = "The CHD opened, but a raw sector could not be decoded.";
            } else if (has_expected_crc) {
                v.crc_matches = v.crc == expected_crc;
            }
        }
        return v;
    }

    std::ifstream f(data_path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        v.detail = resolved.note.empty()
                       ? "Could not open the disc image or its CUE-referenced BIN file."
                       : resolved.note;
        if (netplay_expect)
            apply_netplay_disc_expect(v, *netplay_expect);
        else if (!v.toc_opened)
            v.netplay_ok = false;
        return v;
    }
    v.opened = true;
    const uint64_t size = (uint64_t)f.tellg();

    // PVD: try raw (2352) then cooked (2048) sector 16.
    uint8_t sector[2048];
    if ((size >= (16ull * 2352ull + 24ull + sizeof(sector))) &&
        read_at(f, 16ull * 2352ull + 24ull, sector, sizeof(sector)) && is_iso_pvd(sector)) {
        v.has_header = true;
    } else if ((size >= (16ull * 2048ull + sizeof(sector))) &&
               read_at(f, 16ull * 2048ull, sector, sizeof(sector)) && is_iso_pvd(sector)) {
        v.has_header = true;
    }

    if (!v.has_header) {
        v.detail = "No ISO9660 CD001 header was found at the standard PS1 disc locations.";
        // Region may still be inferable from the expected serial for display.
        v.region = region_from_serial(expected_serial);
        if (netplay_expect)
            apply_netplay_disc_expect(v, *netplay_expect);
        return v;
    }

    // PVD volume identifier: offset 40, 32 bytes (space-padded).
    {
        std::string vol((const char*)&sector[40], 32);
        const size_t last = vol.find_last_not_of(" \t\0");
        if (last != std::string::npos) vol.erase(last + 1); else vol.clear();
        v.volume_id = vol;
    }

    // Scan the early image for the boot serial + (if given) the expected serial.
    const size_t scan_len = (size_t)std::min<uint64_t>(size, 16ull * 1024ull * 1024ull);
    std::vector<uint8_t> scan(scan_len);
    if (read_at(f, 0, scan.data(), scan.size())) {
        v.detected_serial = scan_boot_serial(scan);

        if (v.expected_serial_given) {
            // Match either the game-id form (SCUS-94236) or the EXE form (SCUS_942.36).
            const std::string serial_id = uppercase_ascii(expected_serial);
            std::string exe_id = serial_id;  // SCUS-94236 -> SCUS_942.36
            if (serial_id.size() == 10 && serial_id[4] == '-')
                exe_id = serial_id.substr(0, 4) + "_" + serial_id.substr(5, 3) + "." +
                         serial_id.substr(8, 2);
            auto contains = [&](const std::string& needle) {
                if (needle.empty() || needle.size() > scan.size()) return false;
                return std::search(scan.begin(), scan.end(), needle.begin(), needle.end()) !=
                       scan.end();
            };
            v.serial_matches = contains(serial_id) || contains(exe_id);
            if (!v.serial_matches && !v.detected_serial.empty() &&
                v.detected_serial == serial_id)
                v.serial_matches = true;  // boot line agrees even if the literal bytes differ
        }
    } else {
        v.detail = "The ISO header is present, but the image could not be scanned.";
    }

    // Region: prefer the disc's own serial, fall back to the expected one.
    v.region = region_from_serial(!v.detected_serial.empty() ? v.detected_serial
                                                             : expected_serial);

    // CRC32 (full image), streamed.
    if (compute_crc) {
        v.crc          = crc32_stream(f);
        v.crc_computed = true;
        if (has_expected_crc) v.crc_matches = (v.crc == expected_crc);
    }

    if (netplay_expect)
        apply_netplay_disc_expect(v, *netplay_expect);

    return v;
}

}  // namespace PSXRecompV4
