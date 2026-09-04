// disc_path.h — canonicalize whatever disc file the user actually picked.
//
// Players do not reliably know whether to hand us the .cue or the .bin, and
// both are legitimate things to find in a dump folder. Either pick must mount
// and must verify identically, so every entry point that accepts a disc path
// (game.toml [runtime] disc, --disc, the cached disc.cfg, the launcher's
// "Change Disc" picker, the first-run file dialog) routes through
// resolve_disc_path() first.
//
// The rule is "prefer the cue, but never require it":
//
//   picked a .cue that resolves        -> mount the cue      (keeps the TOC)
//   picked a .cue with a missing bin   -> mount a sibling image, if one exists
//   picked a raw .bin/.iso/.img/.car owned by a cue -> mount that cue
//   picked a raw .bin/.iso/.img/.car with no cue    -> mount it directly
//   picked a .chd                                  -> mount it directly (TOC embedded)
//
// Preferring the cue matters beyond tidiness: the cue is the only place the
// CD-DA track layout lives. Silently swapping a cue for its same-named .bin
// (which is what the runtime used to do) drops every audio track and pregap on
// a single-file multi-track dump.
//
// `data` is always the binary carrying the ISO9660 header and SYSTEM.CNF, so
// header / serial / CRC32 checks read the same bytes no matter which file the
// user picked — that is what keeps the "Disc verified" verdict stable across
// picks.

#pragma once

#include <filesystem>
#include <string>

namespace PSXRecompV4 {

struct DiscPathResolution {
    std::filesystem::path picked;   // what the caller asked for (absolute where possible)
    std::filesystem::path mount;    // hand this to ISOReader::Open / cdrom_init
    std::filesystem::path data;     // data-track binary; read identity/CRC from this

    bool from_cue        = false;   // `mount` is a cue sheet
    bool upgraded_to_cue = false;   // caller picked a bin; we found the cue that owns it
    bool cue_fallback    = false;   // caller picked an unusable cue; fell back to a bin

    // Human-readable note describing any substitution, "" when `mount` is just
    // `picked`. Suitable for a launcher warning or a startup banner.
    std::string note;
};

// Never fails: an unrecognizable or missing path resolves to itself, so the
// caller's existing "could not open the disc" reporting still fires.
DiscPathResolution resolve_disc_path(const std::filesystem::path& picked);

}  // namespace PSXRecompV4
