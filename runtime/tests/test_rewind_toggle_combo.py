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
assert "            hotkey_pad_binding_down(g_hotkey_pad_fast_forward)) {" in MAIN
assert MAIN.count("ls.assist_pad_bind[PSX_ASSIST_BIND_FAST_FORWARD]") == \
    MAIN.count("ls.assist_pad_bind[PSX_ASSIST_BIND_SAVE_STATE_MENU]")
assert '"Fast-forward",' in MAIN
CFG = (ROOT / "recompiler" / "src" / "config_loader.cpp").read_text(encoding="utf-8")
assert 'h.contains("fast_forward_pad")' in CFG          # settings.toml read
assert 'f << "fast_forward_pad = "' in CFG               # settings.toml write

# Fast-forward toggle: a press-to-latch twin of the hold shortcut. Keyboard
# [KeyMap] TurboToggle (default F9) and pad [hotkeys] fast_forward_toggle_pad
# (unbound by default) flip one latch that feeds the same fast-forward block.
assert "static int           g_hotkey_pad_fast_forward_toggle = 0;" in MAIN
assert "PSX_ASSIST_BIND_FAST_FORWARD_TOGGLE" in MAIN
assert "static int g_manual_turbo_latched = 0;" in MAIN
assert "static void fast_forward_toggle_flip(void)" in MAIN
assert "fast_forward_toggle_poll_buttons();" in MAIN
assert "host_keymap_match_event(HOST_KEYMAP_TURBO_TOGGLE," in MAIN
assert "if (kb_turbo || g_manual_turbo_latched ||" in MAIN
assert MAIN.count("ls.assist_pad_bind[PSX_ASSIST_BIND_FAST_FORWARD_TOGGLE]") == \
    MAIN.count("ls.assist_pad_bind[PSX_ASSIST_BIND_FAST_FORWARD]")
assert '"Fast-forward toggle",' in MAIN
assert 'h.contains("fast_forward_toggle_pad")' in CFG
assert 'f << "fast_forward_toggle_pad = "' in CFG
KEYMAP_H = (ROOT / "runtime" / "include" / "host_keymap.h").read_text(encoding="utf-8")
KEYMAP_C = (ROOT / "runtime" / "src" / "host_keymap.c").read_text(encoding="utf-8")
assert "HOST_KEYMAP_TURBO_TOGGLE," in KEYMAP_H
assert 'ieq(name, "TurboToggle")' in KEYMAP_C
assert "add_bind(HOST_KEYMAP_TURBO_TOGGLE, (int)SDLK_F9" in KEYMAP_C
# A present-but-empty / "None" [KeyMap] line is an explicit unbind: no
# built-in default is re-applied for that action (launcher Backspace-to-clear).
assert "int explicit_unbound;" in KEYMAP_C
assert "static int want_default(HostKeymapAction action)" in KEYMAP_C
assert "s_actions[HOST_KEYMAP_TURBO].count == 0" not in KEYMAP_C
assert "if (want_default(HOST_KEYMAP_TURBO))" in KEYMAP_C

print("host shortcut guard passed")
