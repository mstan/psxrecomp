// launcher.h — host-side types shared with the Dear ImGui (recomp-ui) launch path.
//
// The UI itself lives in lib/recomp-ui (`recomp_launcher_run_window`). This
// header only carries Result / NetplayLaunch so main.cpp can apply launch
// outcomes (including CLI/env netplay).

#pragma once

#include <string>

#include "psx_netplay.h"

namespace psx_launcher {

enum class Result {
    Launch,      // proceed to boot
    Quit,        // user closed the launcher — exit
    Unavailable, // launcher could not initialise; boot as if skipped
};

/* Filled on Launch when netplay was armed (CLI/env today; lobby UI TBD). */
struct NetplayLaunch {
    bool enabled = false;
    PsxNetplayConfig cfg{};
    std::string display_name;
};

} // namespace psx_launcher
