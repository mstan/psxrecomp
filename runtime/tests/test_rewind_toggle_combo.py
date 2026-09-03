#!/usr/bin/env python3
"""Guard PSX host controller shortcuts against single-button defaults."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")

assert "static int           g_hotkey_pad_rewind = 1272;" in MAIN
assert "static int           g_hotkey_pad_save_state_menu = 2040;" in MAIN
assert "PSX_HOTKEY_PAD_IS_BUTTON_COMBO" in MAIN
assert "PSX_HOTKEY_PAD_SELECT_R3" in MAIN
assert "static int hotkey_pad_binding_down(int binding)" in MAIN
assert "SDL_GameControllerGetButton(h, SDL_CONTROLLER_BUTTON_BACK)" in MAIN
assert "return hotkey_pad_binding_down(g_hotkey_pad_rewind);" in MAIN
assert "(btn & PAD_SELECT) == 0 && (btn & PAD_L3) == 0" not in MAIN
assert (
    "SDL_GameControllerGetButton(h, SDL_CONTROLLER_BUTTON_BACK) &&\n"
    "           SDL_GameControllerGetButton(h, SDL_CONTROLLER_BUTTON_LEFTSTICK)"
) not in MAIN
assert (
    "SDL_GameControllerGetButton(h, SDL_CONTROLLER_BUTTON_BACK) ||\n"
    "           SDL_GameControllerGetButton(h, SDL_CONTROLLER_BUTTON_RIGHTSTICK)"
) not in MAIN
assert "HOST_KEYMAP_SAVE_STATE_MENU" in MAIN
assert "key >= SDLK_F1 && key <= SDLK_F12" not in MAIN
assert "savestate_slot_from_function_key" not in MAIN
assert "savestate_submit_slot(f_slot" not in MAIN
assert "savestate_input_guard_arm();" in MAIN
assert "savestate_input_guard_active()" in MAIN
assert "out->buttons = 0xFFFFu;" in MAIN
assert "pad_from_keyboard(1) != 0xFFFFu" in MAIN
assert "savestate_menu_ignore_toggle_release" in MAIN
assert "savestate_menu_open_key" in MAIN
assert "savestate_menu_toggle(key)" in MAIN
assert "savestate_menu_toggle(0)" in MAIN
assert 'std::getenv("PSX_FAST_FORWARD_SPEED")' in MAIN
assert "bool manual_turbo_active = false;" in MAIN
assert "Fast forward: %dx" in MAIN
assert "g_frame_period_ms / (double)mult" in MAIN

# Fast-forward has a controller host shortcut alongside the keyboard Turbo key:
# a select+L1 chord by default, routed through the same combo matcher, offered
# to the launcher as the third host-shortcut row, and persisted as
# [hotkeys] fast_forward_pad.
assert "static int           g_hotkey_pad_fast_forward = 1528;" in MAIN
assert "PSX_HOTKEY_PAD_SELECT_L1" in MAIN
assert "PSX_ASSIST_BIND_FAST_FORWARD" in MAIN
assert "if (kb_turbo || hotkey_pad_binding_down(g_hotkey_pad_fast_forward)) {" in MAIN
assert MAIN.count("ls.assist_pad_bind[PSX_ASSIST_BIND_FAST_FORWARD]") == \
    MAIN.count("ls.assist_pad_bind[PSX_ASSIST_BIND_SAVE_STATE_MENU]")
assert '"Fast-forward",' in MAIN
CFG = (ROOT / "recompiler" / "src" / "config_loader.cpp").read_text(encoding="utf-8")
assert 'h.contains("fast_forward_pad")' in CFG          # settings.toml read
assert 'f << "fast_forward_pad = "' in CFG               # settings.toml write

print("host shortcut guard passed")
