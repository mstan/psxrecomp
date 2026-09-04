"""Derive project naming tokens (mirrors setup_project.sh)."""

from __future__ import annotations

import re
from pathlib import Path

from fill_tokens import derive_zip_prefix, sanitize_github_name


DEFAULT_LOBBY_HOST = "netplay.retcomm.net"
DEFAULT_LOBBY_URL = f"ws://{DEFAULT_LOBBY_HOST}:8765"


def project_cmake_name(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", name)


def env_prefix(cmake_name: str) -> str:
    return cmake_name.upper()


def window_title_from_name(name: str) -> str:
    title = re.sub(r"Recomp$", " Recompiled", name)
    title = title.replace("Recompiled Recompiled", "Recompiled")
    return title


def exe_basename(window_title: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", window_title)


def game_name_from_project(name: str) -> str:
    g = re.sub(r"(Recomp|Recompiled)$", "", name).rstrip()
    return g or name


def normalize_lobby_url(value: str | None) -> str:
    raw = (value or DEFAULT_LOBBY_HOST).strip()
    if raw.startswith("ws://") or raw.startswith("wss://"):
        return raw
    if "://" in raw:
        return raw
    if ":" in raw:
        return f"ws://{raw}"
    return f"ws://{raw}:8765"


def boot_exe_from_game_toml(game_toml: Path) -> str | None:
    """Best-effort parse of [game].exe basename (e.g. SCUS_944.23)."""
    if not game_toml.is_file():
        return None
    text = game_toml.read_text(encoding="utf-8", errors="replace")
    in_game = False
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            in_game = s == "[game]"
            continue
        if not in_game or "=" not in s or s.startswith("#"):
            continue
        key, _, val = s.partition("=")
        if key.strip() != "exe":
            continue
        val = val.strip().strip('"').strip("'")
        # path/to/SCUS_944.23 → SCUS_944.23
        return Path(val).name
    return None


def entry_pc_from_game_toml(game_toml: Path) -> str:
    if not game_toml.is_file():
        return "0x80010000"
    text = game_toml.read_text(encoding="utf-8", errors="replace")
    in_game = False
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            in_game = s == "[game]"
            continue
        if not in_game or "=" not in s or s.startswith("#"):
            continue
        key, _, val = s.partition("=")
        if key.strip() == "entry_pc":
            return val.strip().strip('"').strip("'")
    return "0x80010000"


def players_from_cmake(cmake: Path) -> int | None:
    if not cmake.is_file():
        return None
    text = cmake.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"MAX_PLAYERS\s+(\d+)", text)
    if m:
        return int(m.group(1))
    return None


def players_from_game_toml(game_toml: Path) -> int | None:
    """Best-effort parse of ``[game].players`` from game.toml."""
    if not game_toml.is_file():
        return None
    text = game_toml.read_text(encoding="utf-8", errors="replace")
    in_game = False
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            in_game = s == "[game]"
            continue
        if not in_game or "=" not in s or s.startswith("#"):
            continue
        key, _, val = s.partition("=")
        if key.strip() != "players":
            continue
        raw = val.strip().strip('"').strip("'")
        try:
            n = int(raw)
        except ValueError:
            return None
        return n
    return None


def configured_players(root: Path, *, default: int = 2) -> int:
    """Players from game.toml, else CMakeLists MAX_PLAYERS, else ``default``.

    Clamped to 1..8 to match the Migrate UI and sio pad slots.
    """
    root = root.expanduser().resolve()
    n = players_from_game_toml(root / "game.toml")
    if n is None:
        n = players_from_cmake(root / "CMakeLists.txt")
    if n is None:
        n = default
    if n < 1:
        n = 1
    if n > 8:
        n = 8
    return n


def netplay_configured(root: Path) -> bool:
    """True if the game repo already opts into netplay (CMake and/or game.toml)."""
    root = root.expanduser().resolve()
    cmake = root / "CMakeLists.txt"
    if cmake.is_file():
        try:
            text = cmake.read_text(encoding="utf-8", errors="replace")
        except OSError:
            text = ""
        for line in text.splitlines():
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            if "ENABLE_NETPLAY_IF_PRESENT" in s:
                return True
            if re.search(r"set\s*\(\s*PSX_NETPLAY\s+ON\b", s, re.IGNORECASE):
                return True
            if "NETPLAY_LOBBY_URL" in s:
                return True
    game_toml = root / "game.toml"
    if game_toml.is_file():
        try:
            gtext = game_toml.read_text(encoding="utf-8", errors="replace")
        except OSError:
            gtext = ""
        if re.search(r"^\[netplay\]\s*$", gtext, re.MULTILINE):
            return True
    return False


def ci_workflow_present(root: Path) -> bool:
    """True if setup-host ``.github/workflows/release.yml`` exists."""
    root = root.expanduser().resolve()
    return (root / ".github" / "workflows" / "release.yml").is_file()


def disc_probe_configured(root: Path) -> bool:
    """True if catalog/disc probe artifacts already exist."""
    root = root.expanduser().resolve()
    has_catalog = (root / "catalog_identity.json").is_file()
    game_toml = root / "game.toml"
    has_prepare = False
    if game_toml.is_file():
        try:
            has_prepare = "[prepare_disc]" in game_toml.read_text(
                encoding="utf-8", errors="replace"
            )
        except OSError:
            has_prepare = False
    return has_catalog or has_prepare


def window_title_from_cmake(cmake: Path) -> str | None:
    if not cmake.is_file():
        return None
    text = cmake.read_text(encoding="utf-8", errors="replace")
    m = re.search(r'WINDOW_TITLE\s+"([^"]+)"', text)
    return m.group(1) if m else None


def infer_project_name(root: Path) -> str:
    return root.name


def build_token_map(
    *,
    name: str,
    boot_exe: str,
    players: int,
    zip_prefix: str | None = None,
    window_title: str | None = None,
    entry_pc: str = "0x80010000",
    description: str = "",
    publisher: str = "",
    year: str = "",
    region: str = "USA",
    lobby_url: str = DEFAULT_LOBBY_URL,
    enable_recomp_ui: bool = True,
    enable_wizard: bool = True,
    enable_netplay: bool = False,
    has_boxart: bool = False,
    github_owner: str | None = None,
    github_repo: str | None = None,
) -> dict[str, str]:
    cmake_name = project_cmake_name(name)
    title = window_title or window_title_from_name(name)
    game = game_name_from_project(name)
    zp = zip_prefix or derive_zip_prefix(name)
    desc = description or "_Add a short pitch in catalog_identity.json / README._"
    publisher_disp = publisher or "—"
    year_disp = year or "—"

    if enable_recomp_ui:
        recomp_ui_block = "# recomp-ui submodule present (PSX_RECOMP_UI defaults ON)."
        codegen_arg = (
            '    CODEGEN_SETUP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/codegen_setup.c"'
        )
    else:
        recomp_ui_block = (
            'set(PSX_RECOMP_UI OFF CACHE BOOL\n'
            '    "No recomp-ui submodule in this scaffold" FORCE)'
        )
        codegen_arg = "    # CODEGEN_SETUP_SOURCES — add with recomp-ui / wizard later"

    if enable_netplay:
        netplay_block = (
            'if(EXISTS "${PSXRECOMP_ROOT}/lib/recomp-net/CMakeLists.txt")\n'
            "    set(PSX_NETPLAY ON CACHE BOOL\n"
            '        "Link recomp-net delay-sync (opt-in; needs recomp-net)" FORCE)\n'
            "endif()"
        )
        netplay_runtime = "    ENABLE_NETPLAY_IF_PRESENT"
        netplay_lobby = f'    NETPLAY_LOBBY_URL "{lobby_url}"'
    else:
        netplay_block = (
            "# Full netplay UI — enable after adding recomp-ui + testing:\n"
            '# if(EXISTS "${PSXRECOMP_ROOT}/lib/recomp-net/CMakeLists.txt")\n'
            '#     set(PSX_NETPLAY ON CACHE BOOL "…" FORCE)\n'
            "# endif()"
        )
        netplay_runtime = "    # ENABLE_NETPLAY_IF_PRESENT"
        netplay_lobby = f'    # NETPLAY_LOBBY_URL "ws://{DEFAULT_LOBBY_HOST}:8765"'

    if enable_wizard:
        wizard_block = (
            "set(PSX_SETUP_WIZARD ON CACHE BOOL\n"
            '    "Advertise first-run setup wizard + Generate & rebuild in recomp-ui" FORCE)'
        )
        wizard_runtime = "    ENABLE_SETUP_WIZARD"
    else:
        wizard_block = (
            "# First-run setup wizard — enable after testing:\n"
            '# set(PSX_SETUP_WIZARD ON CACHE BOOL "…" FORCE)'
        )
        wizard_runtime = "    # ENABLE_SETUP_WIZARD"

    if has_boxart:
        boxart = (
            '    LAUNCHER_BOXART "${CMAKE_CURRENT_SOURCE_DIR}/launcher_assets/img/boxart.tga"'
        )
    else:
        boxart = (
            '    # LAUNCHER_BOXART "${CMAKE_CURRENT_SOURCE_DIR}/launcher_assets/img/boxart.tga"'
        )

    app_icon = (
        '    APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/psxrecomp.ico"'
    )

    return {
        "PROJECT_CMAKE_NAME": cmake_name,
        "WINDOW_TITLE": title,
        "BOOT_EXE": boot_exe,
        "GAME_NAME": game,
        "ENV_PREFIX": env_prefix(cmake_name),
        "EXE_BASENAME": exe_basename(title),
        "ZIP_PREFIX": zp,
        "GITHUB_OWNER": sanitize_github_name(
            (github_owner or "").strip() or "TechnicallyComputers"
        ),
        "GITHUB_REPO": sanitize_github_name((github_repo or "").strip() or name),
        "DISC_HINT": f"your legally owned {game} disc",
        "GAME_TITLE": title,
        "PLAYERS": str(players),
        "DESCRIPTION": desc,
        "PUBLISHER": publisher_disp,
        "YEAR": year_disp,
        "REGION": region,
        "ENTRY_PC": entry_pc,
        "NETPLAY_RUNTIME_ARG": netplay_runtime,
        "NETPLAY_LOBBY_URL_ARG": netplay_lobby,
        "WIZARD_RUNTIME_ARG": wizard_runtime,
        "BOXART_CMAKE_ARG": boxart,
        "APP_ICON_CMAKE_ARG": app_icon,
        "NETPLAY_CMAKE_BLOCK": netplay_block,
        "WIZARD_CMAKE_BLOCK": wizard_block,
        "RECOMP_UI_CMAKE_BLOCK": recomp_ui_block,
        "CODEGEN_SETUP_ARG": codegen_arg,
    }
