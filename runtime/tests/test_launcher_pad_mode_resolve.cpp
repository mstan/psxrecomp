// Pad-mode resolution at the launcher exit seam
// (PSXRecompV4::resolve_player_mode_after_launcher, runtime/include/launcher_device.h).
//
// What this guards, in order of how it broke:
//
// 1. A locked title must boot the pad type its game.toml declared, whatever
//    the launcher hands back. Ape Escape sets default_mode = "analog" +
//    lock_mode = true; it booted DIGITAL because the launcher returned 2 for
//    Player 1 (whose device defaults to keyboard on a release install) and the
//    old readback loop took ls.pad_mode[] verbatim. sio.c then answered with
//    the 4-byte digital pad response: right stick dead, left stick folded onto
//    the D-pad, and lock_mode hides the selector that could have fixed it.
//
// 2. An ACTIVE mod controller-mode override outranks the lock. The first
//    version of the clamp was an unconditional `if (lock) mode = locked`,
//    which is correct for (1) and silently wrong here: `goto session_reboot`
//    re-enters the emulator BELOW the block that applies
//    g_mod_controller_mode_override, so a soft return from the lobby never
//    re-runs it. The override used to survive a rematch only because it
//    round-tripped through ls.pad_mode[]; the clamp cut that path.
//
// 3. Whatever this returns is what gets persisted (seed.p_mode[] /
//    us.p_mode[]), so a mode the game does not support must never come out of
//    here for a locked title -- not even when the launcher returns garbage.
//
// PadMode values are the engine's (recompiler/src/config_loader.h:
// PAD_MODE_ANALOG = 1, PAD_MODE_DIGITAL = 2; 0 is Hybrid, mod-only). They are
// hard-coded here rather than dragged in through config_loader.h's dependency
// chain -- tests/test_launcher_pad_mode_wiring.py pins the enum so this cannot
// drift.

#include "launcher_device.h"

#include <cassert>
#include <cstdio>

namespace {

constexpr int kAnalog = 1;    // PAD_MODE_ANALOG
constexpr int kDigital = 2;   // PAD_MODE_DIGITAL
constexpr int kHybrid = 0;    // mod-only, never selectable
constexpr int kNoOverride = -1;

int resolve(int launcher_mode, bool lock, int locked, int override_mode) {
    return PSXRecompV4::resolve_player_mode_after_launcher(
        launcher_mode, lock, locked, override_mode);
}

}  // namespace

int main() {
    // ---- unlocked: the launcher owns the seat ------------------------------
    assert(resolve(kAnalog, false, kAnalog, kNoOverride) == kAnalog);
    assert(resolve(kDigital, false, kAnalog, kNoOverride) == kDigital);
    // The locked mode is ignored entirely when lock_mode is off, even when it
    // disagrees -- ctrl_locked_mode[] is populated for every title, locked or
    // not (main.cpp repopulates it outside the has_default_mode guard).
    assert(resolve(kDigital, false, kAnalog, kNoOverride) == kDigital);
    assert(resolve(kAnalog, false, kDigital, kNoOverride) == kAnalog);

    // ---- locked: the game owns the seat (the reported bug) -----------------
    // Ape Escape: ANALOG-locked, launcher returns DIGITAL because Player 1's
    // device defaulted to keyboard. Must come out ANALOG.
    assert(resolve(kDigital, true, kAnalog, kNoOverride) == kAnalog);
    // And the other direction: a digital-only title (X4, Tomba 2) whose
    // settings.toml predates the lock and still says analog.
    assert(resolve(kAnalog, true, kDigital, kNoOverride) == kDigital);
    // Idempotent when the launcher already agrees.
    assert(resolve(kAnalog, true, kAnalog, kNoOverride) == kAnalog);
    assert(resolve(kDigital, true, kDigital, kNoOverride) == kDigital);

    // ---- persistence safety: a locked seat NEVER yields the launcher's
    // value, so seed.p_mode[]/us.p_mode[] cannot receive an unsupported mode
    // however wrong ls.pad_mode[] is (stale Hybrid, an ABI-width change, a
    // settings.toml hand-edited to nonsense). -------------------------------
    for (int bogus = -8; bogus <= 8; ++bogus) {
        assert(resolve(bogus, true, kAnalog, kNoOverride) == kAnalog);
        assert(resolve(bogus, true, kDigital, kNoOverride) == kDigital);
    }
    // Hybrid specifically: mod-only, and the launcher migrates it away, but a
    // locked title must not adopt it even if one leaks through.
    assert(resolve(kHybrid, true, kAnalog, kNoOverride) == kAnalog);

    // ---- an active mod override wins over the lock ------------------------
    // psx_mod_set_controller_mode_override() only ever stores ANALOG or
    // DIGITAL (it rejects anything else loudly), so those are the two real
    // override values.
    assert(resolve(kDigital, true, kAnalog, kDigital) == kDigital);
    assert(resolve(kAnalog, true, kDigital, kAnalog) == kAnalog);
    // ...and over an unlocked launcher value too.
    assert(resolve(kAnalog, false, kAnalog, kDigital) == kDigital);
    assert(resolve(kDigital, false, kDigital, kAnalog) == kAnalog);

    // ---- the "no override" sentinel is negative, matching the direct apply
    // site (`if (g_mod_controller_mode_override[i] >= 0)` in main.cpp). Pinned
    // so the two cannot drift into disagreeing about what "unset" means: a
    // mode value of 0 would count as ACTIVE here, exactly as it does there.
    assert(resolve(kAnalog, true, kDigital, -1) == kDigital);
    assert(resolve(kAnalog, true, kDigital, -2) == kDigital);
    assert(resolve(kDigital, false, kAnalog, -1) == kDigital);
    assert(resolve(kDigital, true, kAnalog, kHybrid) == kHybrid);

    std::puts("launcher pad-mode resolution guard passed");
    return 0;
}
