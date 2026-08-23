"""Audit an existing title repo against the New Project Layout (setup-host)."""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

from .models import (
    AuditReport,
    CheckResult,
    CheckStatus,
    LayoutClass,
    Severity,
)
from .naming import boot_exe_from_game_toml, infer_project_name
from .readme_metrics import (
    boxart_png_present,
    readme_has_boxart,
    readme_has_launcher,
    readme_has_metrics,
    readme_has_raid,
)


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def _gitmodules_paths(root: Path) -> dict[str, str]:
    """path → url from .gitmodules."""
    gm = root / ".gitmodules"
    if not gm.is_file():
        return {}
    text = _read(gm)
    out: dict[str, str] = {}
    path = url = None
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("[submodule"):
            if path and url:
                out[path] = url
            path = url = None
            continue
        if s.startswith("path"):
            _, _, v = s.partition("=")
            path = v.strip()
        elif s.startswith("url"):
            _, _, v = s.partition("=")
            url = v.strip()
    if path and url:
        out[path] = url
    return out


def _is_real_psxrecomp_tree(path: Path) -> bool:
    return (path / "runtime" / "runtime.cmake").is_file()


def _git_checkout_ok(path: Path) -> bool:
    """True when ``git -C path rev-parse`` sees a live work tree."""
    if not path.is_dir():
        return False
    try:
        proc = subprocess.run(
            ["git", "-C", str(path), "rev-parse", "--is-inside-work-tree"],
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError:
        return False
    return proc.returncode == 0 and proc.stdout.strip() == "true"


def diagnose_psxrecomp_checkout(root: Path) -> str | None:
    """Return a human reason when ``psxrecomp/`` is present but not a usable git checkout.

    Common after consolidating away ``psxrecomp-v4``: ``psxrecomp/.git`` still
    points at ``../.git/modules/psxrecomp-v4`` (deleted), so Studio module ops
    report ``checkout missing`` even though ``runtime.cmake`` is on disk.
    """
    dest = root / "psxrecomp"
    if not dest.is_dir():
        return None
    if not _is_real_psxrecomp_tree(dest):
        return None
    if _git_checkout_ok(dest):
        return None

    git_file = dest / ".git"
    if git_file.is_file():
        text = _read(git_file).strip()
        if text.lower().startswith("gitdir:"):
            rel = text.split(":", 1)[1].strip()
            target = (dest / rel).resolve() if rel else None
            if target is not None and not target.exists():
                return (
                    f"psxrecomp/.git points at missing gitdir ({rel}) — "
                    "broken submodule metadata (often leftover psxrecomp-v4)."
                )
            return f"psxrecomp/.git gitdir is unusable ({rel or text})."
    if git_file.is_dir():
        return "psxrecomp/.git exists but git rev-parse fails."
    return (
        "psxrecomp/ has runtime.cmake but is not a git checkout "
        "(absorbed into the parent tree or missing .git)."
    )


def _cmake_has(text: str, needle: str) -> bool:
    return needle in text


def audit_project(root: Path) -> AuditReport:
    root = root.resolve()
    name = infer_project_name(root)
    boot = boot_exe_from_game_toml(root / "game.toml")
    checks: list[CheckResult] = []
    notes: list[str] = []

    gm = _gitmodules_paths(root)
    has_v4 = "psxrecomp-v4" in gm or (root / "psxrecomp-v4").is_dir()
    has_psx = "psxrecomp" in gm or _is_real_psxrecomp_tree(root / "psxrecomp")
    stub_psx = (root / "psxrecomp").is_dir() and not _is_real_psxrecomp_tree(root / "psxrecomp")
    has_ui = "recomp-ui" in gm or (root / "recomp-ui").is_dir()

    cmake_path = root / "CMakeLists.txt"
    cmake = _read(cmake_path) if cmake_path.is_file() else ""

    # --- Submodule path ---
    if has_v4 and not has_psx:
        checks.append(
            CheckResult(
                id="submodule_psxrecomp",
                title="Root psxrecomp/ submodule",
                status=CheckStatus.FAIL,
                severity=Severity.REQUIRED,
                detail="Found psxrecomp-v4; rename to psxrecomp/ for New Project Layout.",
                fix_op="rename_psxrecomp_submodule",
            )
        )
    elif has_v4 and has_psx:
        checks.append(
            CheckResult(
                id="submodule_psxrecomp",
                title="Root psxrecomp/ submodule",
                status=CheckStatus.WARN,
                severity=Severity.REQUIRED,
                detail=(
                    "Both psxrecomp/ and psxrecomp-v4 present — keep psxrecomp/ "
                    "and delete the legacy psxrecomp-v4 tree."
                ),
                fix_op="rename_psxrecomp_submodule",
            )
        )
    elif stub_psx and has_v4:
        checks.append(
            CheckResult(
                id="submodule_psxrecomp",
                title="Root psxrecomp/ submodule",
                status=CheckStatus.FAIL,
                severity=Severity.REQUIRED,
                detail="psxrecomp/ is a stub (no runtime.cmake); real tree is psxrecomp-v4.",
                fix_op="rename_psxrecomp_submodule",
            )
        )
    elif has_psx:
        broken = diagnose_psxrecomp_checkout(root)
        if broken:
            checks.append(
                CheckResult(
                    id="submodule_psxrecomp",
                    title="Root psxrecomp/ submodule",
                    status=CheckStatus.FAIL,
                    severity=Severity.REQUIRED,
                    detail=broken + " Repair re-clones as a real submodule.",
                    fix_op="repair_psxrecomp_submodule",
                )
            )
        else:
            checks.append(
                CheckResult(
                    id="submodule_psxrecomp",
                    title="Root psxrecomp/ submodule",
                    status=CheckStatus.PASS,
                    severity=Severity.REQUIRED,
                    detail="psxrecomp/ present with runtime.cmake.",
                )
            )
    else:
        checks.append(
            CheckResult(
                id="submodule_psxrecomp",
                title="Root psxrecomp/ submodule",
                status=CheckStatus.FAIL,
                severity=Severity.REQUIRED,
                detail="No psxrecomp submodule found.",
                fix_op="ensure_psxrecomp_submodule",
            )
        )

    if has_ui:
        checks.append(
            CheckResult(
                id="submodule_recomp_ui",
                title="recomp-ui submodule",
                status=CheckStatus.PASS,
                severity=Severity.REQUIRED,
                detail="recomp-ui/ present (required for setup-host wizard).",
            )
        )
    else:
        checks.append(
            CheckResult(
                id="submodule_recomp_ui",
                title="recomp-ui submodule",
                status=CheckStatus.FAIL,
                severity=Severity.REQUIRED,
                detail="Missing recomp-ui/ — setup-host wizard needs it.",
                fix_op="ensure_recomp_ui_submodule",
            )
        )

    # --- CMake API ---
    if not cmake_path.is_file():
        checks.append(
            CheckResult(
                id="cmake_game_runtime",
                title="psxrecomp_add_game_runtime",
                status=CheckStatus.FAIL,
                severity=Severity.REQUIRED,
                detail="No CMakeLists.txt.",
                fix_op="rewrite_cmake_setup_host",
            )
        )
    elif _cmake_has(cmake, "psxrecomp_add_game_runtime"):
        detail = "Uses psxrecomp_add_game_runtime."
        status = CheckStatus.PASS
        fix = None
        if "psxrecomp-v4" in cmake:
            status = CheckStatus.WARN
            detail += " Still references psxrecomp-v4 path."
            fix = "rewrite_cmake_setup_host"
        checks.append(
            CheckResult(
                id="cmake_game_runtime",
                title="psxrecomp_add_game_runtime",
                status=status,
                severity=Severity.REQUIRED,
                detail=detail,
                fix_op=fix,
            )
        )
    elif _cmake_has(cmake, "psxrecomp_add_runtime_target"):
        checks.append(
            CheckResult(
                id="cmake_game_runtime",
                title="psxrecomp_add_game_runtime",
                status=CheckStatus.FAIL,
                severity=Severity.REQUIRED,
                detail="Legacy psxrecomp_add_runtime_target — rewrite to setup-host helper.",
                fix_op="rewrite_cmake_setup_host",
            )
        )
    else:
        checks.append(
            CheckResult(
                id="cmake_game_runtime",
                title="psxrecomp_add_game_runtime",
                status=CheckStatus.FAIL,
                severity=Severity.REQUIRED,
                detail="CMakeLists.txt does not call a known runtime helper.",
                fix_op="rewrite_cmake_setup_host",
            )
        )

    # Split-gen / GEN_FULL_GLOB
    if _cmake_has(cmake, "GEN_FULL_GLOB") or re.search(
        r"full_\*\.c|full_\[0-9\]", cmake
    ):
        checks.append(
            CheckResult(
                id="split_gen",
                title="Split-gen generated shards",
                status=CheckStatus.PASS,
                severity=Severity.RECOMMENDED,
                detail="CMake already globs *_full_*.c / GEN_FULL_GLOB.",
            )
        )
    elif cmake:
        checks.append(
            CheckResult(
                id="split_gen",
                title="Split-gen generated shards",
                status=CheckStatus.WARN,
                severity=Severity.RECOMMENDED,
                detail="No GEN_FULL_GLOB / shard glob — regenerate will need shard support.",
                fix_op="rewrite_cmake_setup_host",
            )
        )

    # Wizard
    wizard_ok = (
        _cmake_has(cmake, "PSX_SETUP_WIZARD")
        or _cmake_has(cmake, "ENABLE_SETUP_WIZARD")
    )
    if wizard_ok:
        checks.append(
            CheckResult(
                id="setup_wizard",
                title="Setup wizard flags",
                status=CheckStatus.PASS,
                severity=Severity.REQUIRED,
                detail="PSX_SETUP_WIZARD / ENABLE_SETUP_WIZARD present.",
            )
        )
    else:
        checks.append(
            CheckResult(
                id="setup_wizard",
                title="Setup wizard flags",
                status=CheckStatus.FAIL,
                severity=Severity.REQUIRED,
                detail="Missing wizard flags — setup-host CI will not open first-run UI.",
                fix_op="rewrite_cmake_setup_host",
            )
        )

    # codegen_setup
    if (root / "codegen_setup.c").is_file() and (root / "codegen_setup.h").is_file():
        cg = _read(root / "codegen_setup.c")
        if "psx_game_codegen_forward_if_built" in cg:
            checks.append(
                CheckResult(
                    id="codegen_setup",
                    title="codegen_setup.c / .h",
                    status=CheckStatus.PASS,
                    severity=Severity.REQUIRED,
                    detail="codegen_setup present with forward_if_built.",
                )
            )
        else:
            checks.append(
                CheckResult(
                    id="codegen_setup",
                    title="codegen_setup.c / .h",
                    status=CheckStatus.WARN,
                    severity=Severity.REQUIRED,
                    detail="codegen_setup.c missing psx_game_codegen_forward_if_built.",
                    fix_op="emit_codegen_setup",
                )
            )
    else:
        checks.append(
            CheckResult(
                id="codegen_setup",
                title="codegen_setup.c / .h",
                status=CheckStatus.FAIL,
                severity=Severity.REQUIRED,
                detail="Missing thin codegen_setup sources for setup-host.",
                fix_op="emit_codegen_setup",
            )
        )

    # Packager
    packager = root / "scripts" / "package_setup_release.sh"
    if packager.is_file():
        checks.append(
            CheckResult(
                id="setup_host_packager",
                title="scripts/package_setup_release.sh",
                status=CheckStatus.PASS,
                severity=Severity.REQUIRED,
                detail="Setup-host packager wrapper present.",
            )
        )
    else:
        checks.append(
            CheckResult(
                id="setup_host_packager",
                title="scripts/package_setup_release.sh",
                status=CheckStatus.FAIL,
                severity=Severity.REQUIRED,
                detail="Missing setup-host packager (required; prebuilt releases are not used).",
                fix_op="emit_packager",
            )
        )

    # CI
    wf = root / ".github" / "workflows" / "release.yml"
    if wf.is_file():
        wtext = _read(wf)
        if "YOUR_ZIP_PREFIX" in wtext or "YOUR_GAME_TITLE" in wtext:
            checks.append(
                CheckResult(
                    id="ci_release",
                    title="Setup-host release CI",
                    status=CheckStatus.WARN,
                    severity=Severity.REQUIRED,
                    detail="release.yml still has YOUR_* placeholders.",
                    fix_op="emit_ci_workflow",
                )
            )
        else:
            checks.append(
                CheckResult(
                    id="ci_release",
                    title="Setup-host release CI",
                    status=CheckStatus.PASS,
                    severity=Severity.REQUIRED,
                    detail=".github/workflows/release.yml present.",
                )
            )
    else:
        checks.append(
            CheckResult(
                id="ci_release",
                title="Setup-host release CI",
                status=CheckStatus.FAIL,
                severity=Severity.REQUIRED,
                detail="Missing setup-host release.yml workflow.",
                fix_op="emit_ci_workflow",
            )
        )

    # Legacy prebuilt packaging — warn only (setup-host exclusive policy)
    legacy_pack = False
    if (root / "packaging").is_dir() or (root / "tools" / "package_release.ps1").is_file():
        legacy_pack = True
        checks.append(
            CheckResult(
                id="legacy_prebuilt",
                title="Legacy prebuilt packaging",
                status=CheckStatus.WARN,
                severity=Severity.RECOMMENDED,
                detail=(
                    "Found packaging/ or tools/package_release.ps1. "
                    "This studio only ships setup-host zips — leave prebuilt scripts unused "
                    "or remove them after CI is green."
                ),
                fix_op="annotate_legacy_packaging",
            )
        )
    else:
        checks.append(
            CheckResult(
                id="legacy_prebuilt",
                title="Legacy prebuilt packaging",
                status=CheckStatus.PASS,
                severity=Severity.INFO,
                detail="No legacy prebuilt packager detected.",
            )
        )

    # VERSION
    ver_text = ""
    if (root / "VERSION").is_file():
        ver_text = _read(root / "VERSION").strip()
        checks.append(
            CheckResult(
                id="version_file",
                title="VERSION",
                status=CheckStatus.PASS,
                severity=Severity.RECOMMENDED,
                detail=ver_text or "(empty)",
            )
        )
    else:
        checks.append(
            CheckResult(
                id="version_file",
                title="VERSION",
                status=CheckStatus.FAIL,
                severity=Severity.RECOMMENDED,
                detail="Missing VERSION pin file.",
                fix_op="emit_version",
            )
        )

    # Lobby pin stamp vs VERSION (build tree) — drift breaks netplay lists.
    stamp_hits: list[tuple[str, str]] = []
    for build in sorted(root.glob("build*")):
        if not build.is_dir():
            continue
        for stamp in (
            build / "psx_game_version.txt",
            build / "Release" / "psx_game_version.txt",
        ):
            if stamp.is_file():
                stamp_hits.append(
                    (str(stamp.relative_to(root)).replace("\\", "/"),
                     _read(stamp).strip())
                )
    if ver_text and stamp_hits:
        bad = [(p, s) for p, s in stamp_hits if s and s.lstrip("vV") != ver_text.lstrip("vV")]
        if bad:
            detail = "; ".join(f"{p}={s} (VERSION={ver_text})" for p, s in bad[:3])
            checks.append(
                CheckResult(
                    id="version_stamp_match",
                    title="Lobby pin stamp",
                    status=CheckStatus.FAIL,
                    severity=Severity.REQUIRED,
                    detail=(
                        "psx_game_version.txt disagrees with VERSION — "
                        "rebuild with -DPSX_GAME_VERSION matching VERSION before "
                        "packaging/releasing. " + detail
                    ),
                )
            )
        else:
            checks.append(
                CheckResult(
                    id="version_stamp_match",
                    title="Lobby pin stamp",
                    status=CheckStatus.PASS,
                    severity=Severity.RECOMMENDED,
                    detail="Build stamp matches VERSION.",
                )
            )

    # symbols
    if (root / "symbols.toml").is_file():
        checks.append(
            CheckResult(
                id="symbols_toml",
                title="symbols.toml",
                status=CheckStatus.PASS,
                severity=Severity.RECOMMENDED,
                detail="symbols.toml present.",
            )
        )
    else:
        checks.append(
            CheckResult(
                id="symbols_toml",
                title="symbols.toml",
                status=CheckStatus.WARN,
                severity=Severity.RECOMMENDED,
                detail="Missing symbols.toml progressive map.",
                fix_op="emit_symbols_toml",
            )
        )

    sync = root / "tools" / "sync_symbols.py"
    if sync.is_file():
        checks.append(
            CheckResult(
                id="sync_symbols",
                title="tools/sync_symbols.py",
                status=CheckStatus.PASS,
                severity=Severity.OPTIONAL,
                detail="sync_symbols.py present.",
            )
        )
    else:
        checks.append(
            CheckResult(
                id="sync_symbols",
                title="tools/sync_symbols.py",
                status=CheckStatus.WARN,
                severity=Severity.OPTIONAL,
                detail="Missing tools/sync_symbols.py.",
                fix_op="emit_sync_symbols",
            )
        )

    # catalog / disc identity
    game_toml = _read(root / "game.toml")
    has_prepare = "[prepare_disc]" in game_toml
    has_catalog = (root / "catalog_identity.json").is_file()
    if has_catalog and has_prepare:
        checks.append(
            CheckResult(
                id="disc_identity",
                title="Disc / catalog identity",
                status=CheckStatus.PASS,
                severity=Severity.RECOMMENDED,
                detail="catalog_identity.json + [prepare_disc] present.",
            )
        )
    elif has_prepare or has_catalog:
        checks.append(
            CheckResult(
                id="disc_identity",
                title="Disc / catalog identity",
                status=CheckStatus.WARN,
                severity=Severity.RECOMMENDED,
                detail="Partial disc identity — re-run probe_disc with a Redump .cue.",
                fix_op="probe_disc_refresh",
            )
        )
    else:
        checks.append(
            CheckResult(
                id="disc_identity",
                title="Disc / catalog identity",
                status=CheckStatus.WARN,
                severity=Severity.RECOMMENDED,
                detail="No catalog_identity.json / [prepare_disc] — needed for RetComM gates.",
                fix_op="probe_disc_refresh",
            )
        )

    # boxart
    modern_box = root / "launcher_assets" / "img" / "boxart.tga"
    legacy_box_candidates = [
        root / "recomp" / "launcher" / "boxart.tga",
        root / "recomp" / "launcher" / "boxart.png",
    ]
    legacy_box = next((p for p in legacy_box_candidates if p.is_file()), None)
    if modern_box.is_file():
        checks.append(
            CheckResult(
                id="boxart",
                title="launcher_assets boxart",
                status=CheckStatus.PASS,
                severity=Severity.OPTIONAL,
                detail="launcher_assets/img/boxart.tga present.",
            )
        )
    elif legacy_box is not None:
        checks.append(
            CheckResult(
                id="boxart",
                title="launcher_assets boxart",
                status=CheckStatus.WARN,
                severity=Severity.OPTIONAL,
                detail=f"Boxart at {legacy_box.relative_to(root)} — relocate to launcher_assets/.",
                fix_op="relocate_boxart",
            )
        )
    else:
        checks.append(
            CheckResult(
                id="boxart",
                title="launcher_assets boxart",
                status=CheckStatus.WARN,
                severity=Severity.OPTIONAL,
                detail="No boxart found (optional).",
                fix_op="emit_boxart_stub",
            )
        )

    # Default RetComM-themed app icon (Windows .ico + PNG)
    app_ico = root / "assets" / "psxrecomp.ico"
    if app_ico.is_file():
        checks.append(
            CheckResult(
                id="app_icon",
                title="assets/psxrecomp app icon",
                status=CheckStatus.PASS,
                severity=Severity.OPTIONAL,
                detail="assets/psxrecomp.ico present.",
            )
        )
    else:
        checks.append(
            CheckResult(
                id="app_icon",
                title="assets/psxrecomp app icon",
                status=CheckStatus.WARN,
                severity=Severity.RECOMMENDED,
                detail="Missing assets/psxrecomp.ico (RetComM-themed default pad icon).",
                fix_op="ensure_app_icon",
            )
        )

    # gitignore essentials
    gi = _read(root / ".gitignore")
    missing_gi = [
        pat
        for pat in ("/generated/", "/disc/", "/bios/", "/dist/")
        if pat not in gi and pat.rstrip("/") not in gi
    ]
    if not (root / ".gitignore").is_file():
        checks.append(
            CheckResult(
                id="gitignore",
                title=".gitignore setup-host rules",
                status=CheckStatus.FAIL,
                severity=Severity.RECOMMENDED,
                detail="Missing .gitignore.",
                fix_op="merge_gitignore",
            )
        )
    elif missing_gi:
        checks.append(
            CheckResult(
                id="gitignore",
                title=".gitignore setup-host rules",
                status=CheckStatus.WARN,
                severity=Severity.RECOMMENDED,
                detail="Missing patterns: " + ", ".join(missing_gi),
                fix_op="merge_gitignore",
            )
        )
    else:
        checks.append(
            CheckResult(
                id="gitignore",
                title=".gitignore setup-host rules",
                status=CheckStatus.PASS,
                severity=Severity.RECOMMENDED,
                detail="Essential setup-host ignore rules present.",
            )
        )

    # pins
    if (root / "framework_pins.txt").is_file():
        checks.append(
            CheckResult(
                id="framework_pins",
                title="framework_pins.txt",
                status=CheckStatus.PASS,
                severity=Severity.OPTIONAL,
                detail="Pin snapshot present (gitlinks remain authoritative).",
            )
        )
    else:
        checks.append(
            CheckResult(
                id="framework_pins",
                title="framework_pins.txt",
                status=CheckStatus.WARN,
                severity=Severity.OPTIONAL,
                detail="No framework_pins.txt snapshot.",
                fix_op="record_framework_pins",
            )
        )

    # mods preloaded stub
    if (root / "mods" / "preloaded").is_dir():
        checks.append(
            CheckResult(
                id="mods_preloaded",
                title="mods/preloaded",
                status=CheckStatus.PASS,
                severity=Severity.OPTIONAL,
                detail="mods/preloaded present.",
            )
        )
    else:
        checks.append(
            CheckResult(
                id="mods_preloaded",
                title="mods/preloaded",
                status=CheckStatus.WARN,
                severity=Severity.OPTIONAL,
                detail="No mods/preloaded catalog stub.",
                fix_op="emit_mods_preloaded",
            )
        )

    # README download badges + boxart + RetComM Launcher (idempotent migrate op)
    readme_path = root / "README.md"
    readme_text = _read(readme_path) if readme_path.is_file() else ""
    missing_readme: list[str] = []
    if not readme_path.is_file():
        missing_readme.append("README.md")
    else:
        if not readme_has_metrics(readme_text):
            missing_readme.append("download badges")
        if not readme_has_boxart(readme_text):
            missing_readme.append("libretro boxart")
        if not readme_has_launcher(readme_text):
            missing_readme.append("RetComM Launcher section")
        if not readme_has_raid(readme_text):
            missing_readme.append("R.A.I.D. Discord footer")
    if not (root / ".github" / "raid-discord.png").is_file():
        missing_readme.append(".github/raid-discord.png")
    if not boxart_png_present(root):
        missing_readme.append("launcher_assets/img/boxart.png")
    if missing_readme:
        checks.append(
            CheckResult(
                id="readme_metrics",
                title="README download metrics / launcher / RAID / boxart",
                status=CheckStatus.WARN,
                severity=Severity.RECOMMENDED,
                detail="Missing: " + ", ".join(missing_readme),
                fix_op="patch_readme_metrics",
            )
        )
    else:
        checks.append(
            CheckResult(
                id="readme_metrics",
                title="README download metrics / launcher / RAID / boxart",
                status=CheckStatus.PASS,
                severity=Severity.RECOMMENDED,
                detail="Download badges, boxart, RetComM Launcher, and R.A.I.D. footer present.",
            )
        )

    # Classify layout
    fails = {c.id for c in checks if c.status == CheckStatus.FAIL}
    rec_warns = [
        c
        for c in checks
        if c.status == CheckStatus.WARN and c.severity == Severity.RECOMMENDED
    ]
    if legacy_pack or "psxrecomp_add_runtime_target" in cmake or has_v4:
        layout = LayoutClass.LEGACY_PACKAGING
    elif fails:
        if packager.is_file() or wizard_ok or (root / "codegen_setup.c").is_file():
            layout = LayoutClass.SETUP_HOST_PARTIAL
        else:
            layout = LayoutClass.UNKNOWN
    elif rec_warns:
        layout = LayoutClass.SETUP_HOST_PARTIAL
    else:
        layout = LayoutClass.SCAFFOLD_COMPLETE

    if boot is None:
        notes.append("Could not parse [game].exe from game.toml — set --boot-exe when applying.")
    notes.append(
        "Policy: setup-host releases only (no prebuilt generated-C zips)."
    )

    return AuditReport(
        root=str(root),
        layout=layout,
        project_name=name,
        boot_exe=boot,
        checks=checks,
        notes=notes,
    )
