// disc_path.cpp — see disc_path.h.

#include "disc_path.h"

#include "cue_sheet.h"

#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace PSXRecompV4 {

namespace {

// Raw image extensions we are willing to mount on their own, in preference
// order for the cue-fallback search.
const char* const kImageExtensions[] = { ".bin", ".img", ".iso", ".car" };

bool is_cue(const fs::path& p) { return path_has_extension_ci(p, ".cue"); }
bool is_chd(const fs::path& p) { return path_has_extension_ci(p, ".chd"); }

bool is_raw_image(const fs::path& p) {
    for (const char* ext : kImageExtensions) {
        if (path_has_extension_ci(p, ext)) return true;
    }
    return false;
}

bool same_file_path(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::exists(a, ec) && fs::exists(b, ec)) {
        ec.clear();
        if (fs::equivalent(a, b, ec) && !ec) return true;
        ec.clear();
    }
    return a.lexically_normal() == b.lexically_normal();
}

fs::path absolute_or_self(const fs::path& p) {
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    return ec ? p : abs;
}

// The BINARY payload an unusable cue still points at: the data track when it
// is present, else the first existing BINARY FILE entry. Empty when the cue
// names nothing that is both BINARY and on disk.
fs::path first_existing_binary(const CueSheet& sheet) {
    const size_t di = sheet.data_file_index();
    if (di < sheet.files.size() && sheet.files[di].is_binary && sheet.files[di].exists)
        return sheet.files[di].path;
    for (const CueFileRef& f : sheet.files) {
        if (f.is_binary && f.exists) return f.path;
    }
    return {};
}

// <stem>.bin / .img / .iso / .car next to a cue we could not use.
fs::path find_sibling_image(const fs::path& cue) {
    std::error_code ec;
    for (const char* ext : kImageExtensions) {
        fs::path cand = cue;
        cand.replace_extension(ext);
        ec.clear();
        if (fs::exists(cand, ec) && !fs::is_directory(cand, ec)) return cand;
    }
    // Case-insensitive sweep for filesystems where ".BIN" != ".bin".
    const fs::path dir = cue.parent_path();
    ec.clear();
    if (fs::is_directory(dir, ec)) {
        const fs::path stem = cue.stem();
        ec.clear();
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().stem() != stem) continue;
            if (is_raw_image(it->path())) return it->path();
        }
    }
    return {};
}

// The cue that owns `image`: try <stem>.cue first (the common single-bin
// layout), then any cue in the same directory whose FILE list names it (the
// redump multi-track layout, where the cue is "Game.cue" and the payloads are
// "Game (Track 01).bin", ...).
fs::path find_owning_cue(const fs::path& image) {
    std::error_code ec;

    auto cue_owns_image = [&](const fs::path& cue) {
        const CueSheet sheet = parse_cue_sheet(cue);
        if (!sheet.usable()) return false;
        for (const CueFileRef& f : sheet.files) {
            if (same_file_path(f.path, image)) return true;
        }
        return false;
    };

    fs::path same_stem = image;
    same_stem.replace_extension(".cue");
    if (fs::exists(same_stem, ec) && cue_owns_image(same_stem)) return same_stem;

    const fs::path dir = image.parent_path();
    ec.clear();
    if (!fs::is_directory(dir, ec)) return {};

    ec.clear();
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!is_cue(it->path())) continue;
        if (same_file_path(it->path(), same_stem)) continue;  // already tried
        if (cue_owns_image(it->path())) return it->path();
    }
    return {};
}

}  // namespace

DiscPathResolution resolve_disc_path(const fs::path& picked) {
    DiscPathResolution r;
    r.picked = absolute_or_self(picked);
    r.mount  = r.picked;
    r.data   = r.picked;

    if (r.picked.empty()) return r;

    // CHD is already a complete image + table of contents. It must not be
    // upgraded to a same-stem cue or treated as that cue's raw payload.
    if (is_chd(r.picked)) return r;

    if (is_cue(r.picked)) {
        const CueSheet sheet = parse_cue_sheet(r.picked);
        if (sheet.usable()) {
            const size_t di = sheet.data_file_index();
            r.from_cue = true;
            r.mount    = r.picked;
            r.data     = di < sheet.files.size() ? sheet.files[di].path : r.picked;
            return r;
        }

        // The cue is unusable. Recover to a raw image rather than failing the
        // launch outright — a renamed or missing bin is the single most common
        // way a hand-assembled dump folder is broken.
        //
        // Prefer a BINARY payload the cue itself names and that actually
        // exists: a cue whose only defect is a WAVE audio track still points
        // at a perfectly good data track. Only fall back to a stem-matched
        // sibling when the cue names nothing usable at all.
        fs::path sibling = first_existing_binary(sheet);
        if (sibling.empty()) sibling = find_sibling_image(r.picked);
        if (!sibling.empty()) {
            r.mount        = absolute_or_self(sibling);
            r.data         = r.mount;
            r.cue_fallback = true;
            r.note = "The cue sheet \"" + r.picked.filename().string() +
                     "\" does not resolve to a usable BINARY payload; falling back to \"" +
                     r.mount.filename().string() +
                     "\". CD audio tracks described by the cue will not be available.";
            return r;
        }

        if (!sheet.opened) {
            r.note = "The cue sheet \"" + r.picked.filename().string() + "\" could not be read.";
        } else if (sheet.has_non_binary_file()) {
            r.note = "The cue sheet \"" + r.picked.filename().string() +
                     "\" references a non-BINARY track payload (WAVE/MP3), which has no fixed "
                     "sector geometry and cannot be mounted.";
        } else if (sheet.files.empty()) {
            r.note = "The cue sheet \"" + r.picked.filename().string() + "\" names no FILE.";
        } else {
            std::string missing;
            for (const CueFileRef& f : sheet.files) {
                if (f.exists) continue;
                if (!missing.empty()) missing += ", ";
                missing += "\"" + f.name + "\"";
            }
            r.note = "The cue sheet \"" + r.picked.filename().string() +
                     "\" references files that are not next to it: " + missing + ".";
        }
        return r;
    }

    if (is_raw_image(r.picked)) {
        const fs::path cue = find_owning_cue(r.picked);
        if (!cue.empty()) {
            const CueSheet sheet = parse_cue_sheet(cue);
            const size_t di = sheet.data_file_index();
            r.mount           = absolute_or_self(cue);
            r.data            = di < sheet.files.size() ? sheet.files[di].path : r.picked;
            r.from_cue        = true;
            r.upgraded_to_cue = true;
            if (sheet.tracks.size() > 1) {
                r.note = "Mounting \"" + r.mount.filename().string() + "\" instead of \"" +
                         r.picked.filename().string() + "\" so the disc's " +
                         std::to_string(sheet.tracks.size()) +
                         " tracks (including CD audio) are available.";
            }
            return r;
        }
    }

    return r;
}

}  // namespace PSXRecompV4
