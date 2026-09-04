// launcher_device.h - controller-source conversion at the recomp-ui C ABI.

#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace PSXRecompV4 {

inline std::string trim_launcher_device(const std::string& device) {
    const auto first = std::find_if_not(device.begin(), device.end(),
        [](unsigned char c) { return std::isspace(c) != 0; });
    const auto last = std::find_if_not(device.rbegin(), device.rend(),
        [](unsigned char c) { return std::isspace(c) != 0; }).base();
    return first < last ? std::string(first, last) : std::string();
}

inline std::string normalize_launcher_device(const std::string& device) {
    std::string normalized = trim_launcher_device(device);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return normalized;
}

inline int launcher_source_from_device(const std::string& device) {
    const std::string normalized = normalize_launcher_device(device);
    if (normalized.empty() || normalized == "none") return 0;
    if (normalized == "keyboard") return 1;
    return 2;
}

inline std::string launcher_device_from_source(
    int source, const std::string& previous_device) {
    if (source <= 0) return "none";
    if (source == 1) return "keyboard";

    // recomp-ui's C ABI currently returns a source category, not the selected
    // controller GUID. Preserve an existing gamepad/GUID assignment; when the
    // user switched from None/Keyboard, persist the runtime's first-pad alias.
    if (launcher_source_from_device(previous_device) == 2) {
        return trim_launcher_device(previous_device);
    }
    return "gamepad";
}

// Pad mode for one seat on the way OUT of the launcher.
//
// Three sources of truth have to be reconciled, and they have a strict
// precedence:
//
//   1. A trusted mod's controller-mode override
//      (psx_mod_set_controller_mode_override, runtime/include/mod_plugins.h).
//      A game plugin that declared the pad type its patched code needs
//      outranks everything else. Pass -1 when no override is active.
//   2. game.toml [controller] lock_mode. A locked title supports exactly one
//      pad type, and the launcher HIDES its pad-mode selector for such a
//      title -- so whatever came back in ls.pad_mode[] for that seat was
//      never a player choice and must not be believed.
//   3. What the launcher returned.
//
// Both launcher-exit readback loops in runtime/src/main.cpp call this, and
// both call it BEFORE the seat's mode is written to settings.toml, so an
// unsupported mode can never be persisted either.
//
// Note what is deliberately NOT here: a keyboard seat is not forced to
// DIGITAL. It already behaves as a plain digital pad -- effective_player_mode()
// reports DIGITAL for any seat whose kind is keyboard, whatever this value
// says -- so overwriting the stored mode bought nothing and cost a durable
// corruption. A release install defaults Player 1 to the keyboard, and a
// locked title has no selector with which to repair the value, so an
// ANALOG-locked dual-analog game (Ape Escape) reached its DualShock as an
// SCPH-1080: sio.c took the 4-byte pad_response_len branch, right stick dead
// and left stick folded onto the D-pad. The matching launcher-side coercion is
// fixed in recomp-ui's launcher_model.c.
inline int resolve_player_mode_after_launcher(int launcher_mode,
                                              bool lock_mode,
                                              int locked_mode,
                                              int mod_override_mode) {
    if (mod_override_mode >= 0) return mod_override_mode;
    if (lock_mode) return locked_mode;
    return launcher_mode;
}

} // namespace PSXRecompV4
