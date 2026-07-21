// launcher.h — integrated RmlUi launcher front-end.
//
// Shown in the runtime's SDL/OpenGL window before the emulator boots: the user
// picks renderer / supersampling / AA / colour model / SPU-HQ, the BIOS, disc,
// memory cards and controllers, then presses LAUNCH. The chosen values are
// written back into the UserSettings the runtime then applies (and persisted to
// settings.toml by the caller).
//
// Design note — the launcher does NOT create or own the window or GL context;
// the caller passes an already-current GL 3.3 context. This keeps the module a
// pure overlay so a future "re-open settings while the game is running" path can
// reuse it without owning the window lifecycle.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "psx_netplay.h"

struct SDL_Window;

namespace PSXRecompV4 { struct UserSettings; }

namespace psx_launcher {

enum class Result {
    Launch,  // user pressed LAUNCH — proceed to boot with `io`
    Quit,    // user closed the window — caller should exit
    Unavailable, // launcher could not initialise (assets/GL); caller boots as if skipped
};

/* Optional behaviour for run(). */
struct RunOptions {
    /* Open directly on the lobby room (lobby WS already connected). */
    bool resume_netplay_room = false;
};

// Static facts about the game the launcher is configuring. Drives the title and
// the disc-verification badge. Extends naturally for later phases.
struct GameInfo {
    const char* name             = nullptr;  // display name, e.g. "Tomba!"
    const char* expected_serial  = nullptr;  // game id "SCUS-94236" (null = no serial check)
    uint32_t    expected_crc     = 0;        // full-file CRC32 of the data track
    bool        has_expected_crc = false;    // whether expected_crc is meaningful
    bool        allow_hybrid     = true;     // offer the "Hybrid" pad mode (false => Analog | D-Pad only)
    bool        lock_mode        = false;    // hide the whole pad-mode selector and force locked_mode (single-pad-type games)
    int         locked_mode      = 2;        // PAD_MODE_DIGITAL; the mode forced when lock_mode is true
    bool        lock_device      = false;    // hide the Player 1/2 controller cards entirely (fixed, auto-bound pad type; e.g. Ape Escape DualShock)
    bool        ws_offered       = true;     // offer the EXPERIMENTAL Widescreen toggle (false = hidden, game ships 4:3 only)
    bool        ws_ultrawide_offered = false; // separately offer experimental 21:9

    // Optional "Localization" dropdown. Populated from game.toml
    // [localization].languages. When non-empty the launcher shows a language
    // menu; empty => no menu (the general case — only Tsumu declares languages).
    // code feeds the runtime's translation layer ("off"/"jp"/"" = untranslated).
    struct Language { std::string code; std::string label; };
    std::vector<Language> languages;
};

/* Filled on Launch when the user started from the Netplay lobby path. */
struct NetplayLaunch {
    bool enabled = false;
    PsxNetplayConfig cfg{};
    std::string display_name;
};

// Run the launcher loop to completion. `gl_context` is an SDL_GLContext (void*
// to avoid leaking SDL types into this header) already created and current on
// `window`. `io` is seeded with the effective settings (game.toml ∪ settings.toml)
// and, on Result::Launch, updated in place with the user's choices. `assets_dir`
// is the directory holding launcher.rml / .rcss / fonts.
// When `out_net` is non-null, a successful netplay lobby launch sets it.
// Lobby WebSocket stays connected across a netplay Launch so the match can
// return to the same room; offline Launch / Quit still disconnect.
Result run(SDL_Window* window, void* gl_context,
           PSXRecompV4::UserSettings& io,
           const GameInfo& game, const char* assets_dir,
           NetplayLaunch* out_net = nullptr,
           const RunOptions& opts = {});

} // namespace psx_launcher
