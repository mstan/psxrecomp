"""Local CMake configure / build / launch for game repos.

Cross-platform (Windows / macOS / Linux). No force flags; builds stay under
the chosen build directory (default ``build-release``).

Before configure, missing OpenBIOS generated C under ``psxrecomp/generated/``
is regenerated (MIT OpenBIOS — no retail dump required) so runtime.cmake can
link a BIOS backend.
"""

from __future__ import annotations

import os
import platform
import re
import shutil
import subprocess
import threading
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from .gitops import CmdResult

DEFAULT_BUILD_DIR = "build-release"
DEFAULT_TARGET = "psx-runtime"
DEFAULT_BUILD_TYPE = "Release"
OPENBIOS_PROFILE = "bios/OpenBIOS.toml"
OPENBIOS_STEM = "OpenBIOS"
SCPH1001_PROFILE = "bios/SCPH1001.toml"
SCPH1001_STEM = "SCPH1001"
LogFn = Callable[[str], None]


@dataclass
class BuildHost:
    system: str  # Windows | Darwin | Linux | …
    label: str  # windows | macos | linux | other
    cmake: str | None
    ninja: str | None
    jobs: int


@dataclass
class LaunchHandle:
    proc: subprocess.Popen
    exe: Path
    cwd: Path
    env_overlay: dict[str, str] = field(default_factory=dict)

    @property
    def pid(self) -> int | None:
        return self.proc.pid if self.proc else None

    def poll(self) -> int | None:
        return self.proc.poll() if self.proc else None

    def terminate(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()


_active_launch: LaunchHandle | None = None
_launch_lock = threading.Lock()


def detect_host() -> BuildHost:
    system = platform.system()
    if system == "Windows":
        label = "windows"
    elif system == "Darwin":
        label = "macos"
    elif system == "Linux":
        label = "linux"
    else:
        label = "other"
    jobs = os.cpu_count() or 4
    return BuildHost(
        system=system,
        label=label,
        cmake=shutil.which("cmake"),
        ninja=shutil.which("ninja") or shutil.which("ninja-build"),
        jobs=jobs,
    )


def default_generator(host: BuildHost | None = None) -> str:
    host = host or detect_host()
    if host.ninja:
        return "Ninja"
    if host.label == "windows":
        # Leave empty → cmake picks VS / default generator.
        return ""
    return "Unix Makefiles"


def parse_env_text(text: str) -> dict[str, str]:
    """Parse ``KEY=VAL`` pairs from free text (space / newline / ``;`` separated).

    Values may be quoted with single or double quotes. Lines starting with ``#``
    are ignored.
    """
    env: dict[str, str] = {}
    if not text or not text.strip():
        return env
    # Normalize separators to newlines, but keep quoted spans intact via a
    # simple token walk on KEY=VAL forms.
    cleaned: list[str] = []
    for raw_line in text.replace(";", "\n").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        cleaned.append(line)
    blob = "\n".join(cleaned)
    # Match KEY=VALUE where VALUE is "…", '…', or non-space / until next KEY=
    pattern = re.compile(
        r"""(?P<key>[A-Za-z_][A-Za-z0-9_]*)\s*=\s*(?P<val>"[^"]*"|'[^']*'|\S+)"""
    )
    for m in pattern.finditer(blob):
        key = m.group("key")
        val = m.group("val")
        if (val.startswith('"') and val.endswith('"')) or (
            val.startswith("'") and val.endswith("'")
        ):
            val = val[1:-1]
        env[key] = val
    return env


def _run_stream(
    cmd: list[str],
    cwd: Path,
    *,
    log: LogFn | None = None,
    env: dict[str, str] | None = None,
) -> CmdResult:
    if log:
        log("$ " + " ".join(cmd))
    try:
        proc = subprocess.Popen(
            cmd,
            cwd=str(cwd),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            env=env,
            bufsize=1,
        )
    except OSError as exc:
        return CmdResult(False, f"Failed to start: {cmd[0]}", str(exc))

    assert proc.stdout is not None
    lines: list[str] = []
    for line in proc.stdout:
        line = line.rstrip("\n")
        lines.append(line)
        if log:
            log(line)
    code = proc.wait()
    detail = "\n".join(lines[-40:])
    if code != 0:
        return CmdResult(False, f"Command failed (exit {code})", detail)
    return CmdResult(True, "OK", detail)


def resolve_framework_root(root: Path) -> Path | None:
    """Return the psxrecomp framework root (bios/ + recompiler/), or None."""
    root = root.expanduser().resolve()
    for cand in (root / "psxrecomp", root):
        if (cand / "bios" / "OpenBIOS.toml").is_file() and (cand / "recompiler").is_dir():
            return cand
    return None


_MAX_PLAYERS_CMAKE_RE = re.compile(
    r"^\s*MAX_PLAYERS\s+(\d+)\s*$", re.MULTILINE | re.IGNORECASE
)
_MAX_PLAYERS_RANGE_RE = re.compile(
    r"MAX_PLAYERS must be in\s+(\d+)\.\.(\d+)", re.IGNORECASE
)


def project_max_players(root: Path) -> int | None:
    """``MAX_PLAYERS N`` from the game ``CMakeLists.txt``, if present."""
    cmake = root / "CMakeLists.txt"
    if not cmake.is_file():
        return None
    try:
        text = cmake.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    m = _MAX_PLAYERS_CMAKE_RE.search(text)
    return int(m.group(1)) if m else None


def framework_max_players_range(fw: Path) -> tuple[int, int] | None:
    """``(lo, hi)`` from runtime.cmake's FATAL_ERROR range check, if found."""
    cmake = fw / "runtime" / "runtime.cmake"
    if not cmake.is_file():
        return None
    try:
        text = cmake.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    m = _MAX_PLAYERS_RANGE_RE.search(text)
    if not m:
        return None
    return int(m.group(1)), int(m.group(2))


def preflight_max_players(root: Path) -> CmdResult | None:
    """Fail fast when the game asks for MAX_PLAYERS the nested framework rejects.

    Single-player titles (Ape Escape, Tomba, …) use ``MAX_PLAYERS 1``. Older
    psxrecomp pins only allowed 2..5 and abort configure with a cryptic
    FATAL_ERROR. Detect that before running cmake.
    """
    players = project_max_players(root)
    if players is None:
        return None
    fw = resolve_framework_root(root)
    if fw is None:
        return None
    rng = framework_max_players_range(fw)
    if rng is None:
        return None
    lo, hi = rng
    if lo <= players <= hi:
        return None
    pins = root / "framework_pins.txt"
    pin_hint = ""
    if pins.is_file():
        pin_hint = f" Check {pins.name} vs the checked-out psxrecomp commit."
    return CmdResult(
        False,
        f"MAX_PLAYERS {players} is outside nested psxrecomp range {lo}..{hi}. "
        f"Update the psxrecomp submodule to a pin that allows 1..8 "
        f"(runtime.cmake after single-player / rewind support).{pin_hint}",
    )


def diagnose_configure_failure(detail: str, root: Path) -> str | None:
    """Extra hint appended to a failed cmake configure message."""
    blob = detail or ""
    if "MAX_PLAYERS must be in" in blob and "got" in blob:
        pre = preflight_max_players(root)
        if pre is not None:
            return pre.message
        return (
            "MAX_PLAYERS rejected by nested psxrecomp. Single-player titles need "
            "a framework pin whose runtime.cmake allows 1..8 — update the "
            "psxrecomp submodule (and framework_pins.txt)."
        )
    if "generated" in blob.lower() and (
        "GEN_MARKER" in blob or "dispatch.c" in blob or "missing" in blob.lower()
    ):
        return (
            "Game generated C may be missing — run Generate (disc→C) before "
            "Configure, or ensure generated/<boot>_dispatch.c exists."
        )
    return None


def bios_backend_present(fw: Path, stem: str) -> bool:
    """True when generated/<stem>_{full,dispatch}.c look linkable."""
    dispatch = fw / "generated" / f"{stem}_dispatch.c"
    full = fw / "generated" / f"{stem}_full.c"
    if not dispatch.is_file() or not full.is_file():
        return False
    try:
        text = dispatch.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return False
    return f"{stem}_psx_bios_backend" in text


def _allow_no_bios(extra_args: list[str] | None) -> bool:
    if not extra_args:
        return False
    blob = " ".join(extra_args)
    return (
        "PSXRECOMP_ALLOW_NO_BIOS=ON" in blob
        or "PSXRECOMP_ALLOW_NO_BIOS:BOOL=ON" in blob
        or "PSXRECOMP_ALLOW_NO_BIOS=1" in blob
    )


def _find_psxrecomp_bios(fw: Path, game_root: Path | None = None) -> Path | None:
    names = ("psxrecomp-bios.exe", "psxrecomp-bios")
    dirs: list[Path] = [
        fw / "recompiler" / "build",
        fw / "recompiler" / "build-t2",
    ]
    if game_root is not None:
        dirs.append(game_root / "build-recompiler")
    for d in dirs:
        if not d.is_dir():
            continue
        for name in names:
            p = d / name
            if p.is_file():
                return p
        # Nested generator layouts (e.g. Debug/Release on MSVC)
        for sub in d.iterdir():
            if not sub.is_dir():
                continue
            for name in names:
                p = sub / name
                if p.is_file():
                    return p
    return None


def _recompiler_build_usable(build_dir: Path) -> bool:
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return False
    try:
        text = cache.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return False
    gen = ""
    for line in text.splitlines():
        if line.startswith("CMAKE_GENERATOR:INTERNAL="):
            gen = line.split("=", 1)[1].strip()
            break
    if gen.startswith("Ninja"):
        return (build_dir / "build.ninja").is_file()
    if "Makefiles" in gen:
        return (build_dir / "Makefile").is_file()
    if gen.startswith("Visual Studio"):
        return any(build_dir.glob("*.sln"))
    # Unknown generator — trust the cache and let cmake --build diagnose.
    return True


def _iter_recompiler_build_dirs(fw: Path) -> list[Path]:
    src = fw / "recompiler"
    if not src.is_dir():
        return []
    dirs: list[Path] = []
    for name in ("build-t2", "build"):
        dirs.append(src / name)
    dirs.extend(sorted(src.glob("cmake-build*")))
    return dirs


def _any_usable_recompiler_build(fw: Path) -> bool:
    return any(_recompiler_build_usable(d) for d in _iter_recompiler_build_dirs(fw))


def ensure_bios_emitter(
    fw: Path,
    *,
    game_root: Path | None = None,
    build_type: str = DEFAULT_BUILD_TYPE,
    dry_run: bool = False,
    log: LogFn | None = None,
) -> CmdResult:
    """Configure ``recompiler/build`` and build ``psxrecomp-bios`` if needed."""
    host = detect_host()
    if not host.cmake:
        return CmdResult(False, "cmake not found on PATH (needed to build psxrecomp-bios)")

    existing = _find_psxrecomp_bios(fw, game_root)
    if existing is not None and not dry_run:
        if log:
            log(f"BIOS emitter ready: {existing}")
        return CmdResult(True, f"BIOS emitter ready: {existing.name}")

    src = fw / "recompiler"
    if not (src / "CMakeLists.txt").is_file():
        return CmdResult(False, f"recompiler sources missing under {src}")

    # Prefer an already-finished tree (matches regen_bios.sh discovery order).
    build_dir = src / "build"
    for cand in _iter_recompiler_build_dirs(fw):
        if _recompiler_build_usable(cand):
            build_dir = cand
            break

    gen = default_generator(host)
    cfg = [host.cmake, "-S", str(src), "-B", str(build_dir)]
    if gen:
        cfg.extend(["-G", gen])
    cfg.append(f"-DCMAKE_BUILD_TYPE={build_type}")

    if dry_run:
        msg = "dry-run: " + " ".join(cfg)
        if log:
            log(msg)
            log(f"dry-run: {host.cmake} --build {build_dir} --target psxrecomp-bios")
        return CmdResult(True, msg)

    if not _recompiler_build_usable(build_dir):
        if log:
            log("Configuring recompiler for psxrecomp-bios…")
        build_dir.mkdir(parents=True, exist_ok=True)
        r = _run_stream(cfg, fw, log=log)
        if not r.ok:
            return CmdResult(
                False,
                "Failed to configure recompiler (needed for OpenBIOS regen)",
                r.detail,
            )

    jobs = str(host.jobs)
    build_cmd = [
        host.cmake,
        "--build",
        str(build_dir),
        "--target",
        "psxrecomp-bios",
        "-j",
        jobs,
    ]
    if log:
        log("Building psxrecomp-bios…")
    r = _run_stream(build_cmd, fw, log=log)
    if not r.ok:
        return CmdResult(False, "Failed to build psxrecomp-bios", r.detail)

    bios = _find_psxrecomp_bios(fw, game_root)
    if bios is None:
        return CmdResult(
            False,
            f"psxrecomp-bios not found after build under {build_dir}",
        )
    return CmdResult(True, f"Built BIOS emitter: {bios.name}")


def _regen_bios_profile(
    fw: Path,
    profile_rel: str,
    *,
    game_root: Path | None = None,
    dry_run: bool = False,
    log: LogFn | None = None,
) -> CmdResult:
    """Regen one BIOS profile into ``fw/generated/`` (canonical regen_bios.sh)."""
    profile = fw / profile_rel
    if not profile.is_file():
        return CmdResult(False, f"BIOS profile missing: {profile}")

    script = fw / "tools" / "regen_bios.sh"
    bash = shutil.which("bash") or shutil.which("bash.exe")

    if script.is_file() and bash:
        cmd = [bash, str(script), "--config", profile_rel]
        if dry_run:
            msg = "dry-run: " + " ".join(cmd) + f"  (cwd={fw})"
            if log:
                log(msg)
            return CmdResult(True, msg)
        return _run_stream(cmd, fw, log=log)

    # Fallback without bash: invoke emitter + optional fingerprint helper.
    r = ensure_bios_emitter(fw, game_root=game_root, dry_run=dry_run, log=log)
    if not r.ok:
        return r
    if dry_run:
        return CmdResult(True, f"dry-run: psxrecomp-bios --config {profile_rel}")

    bios = _find_psxrecomp_bios(fw, game_root)
    if bios is None:
        return CmdResult(False, "psxrecomp-bios missing after ensure")
    (fw / "generated").mkdir(parents=True, exist_ok=True)
    r = _run_stream([str(bios), "--config", profile_rel], fw, log=log)
    if not r.ok:
        return CmdResult(False, f"psxrecomp-bios failed for {profile_rel}", r.detail)

    # Best-effort fingerprint (staleness WARN in runtime.cmake).
    fp = fw / "tools" / "bios_emitter_fingerprint.sh"
    if fp.is_file() and bash:
        stem = OPENBIOS_STEM if "OpenBIOS" in profile_rel else SCPH1001_STEM
        try:
            proc = subprocess.run(
                [bash, str(fp), profile_rel],
                cwd=str(fw),
                capture_output=True,
                text=True,
                check=False,
            )
            if proc.returncode == 0 and proc.stdout.strip():
                out = fw / "generated" / f"{stem}.emitter.sha"
                out.write_text(proc.stdout, encoding="utf-8")
                if log:
                    log(f"Wrote fingerprint {out.name}")
        except OSError:
            pass
    return CmdResult(True, f"Regenerated BIOS profile {profile_rel}")


def ensure_bios_backends(
    root: Path,
    *,
    force: bool = False,
    include_scph1001: bool = True,
    dry_run: bool = False,
    log: LogFn | None = None,
) -> CmdResult:
    """Ensure linkable OpenBIOS (and optional SCPH1001) under ``psxrecomp/generated``.

    OpenBIOS is bundled and MIT-licensed. SCPH1001 is regenerated only when the
    retail dump ``bios/SCPH1001.BIN`` is already present beside the profile.
    """
    root = root.expanduser().resolve()
    fw = resolve_framework_root(root)
    if fw is None:
        return CmdResult(True, "No psxrecomp framework — skipping BIOS ensure")

    needed: list[tuple[str, str]] = []
    if force or not bios_backend_present(fw, OPENBIOS_STEM):
        needed.append((OPENBIOS_STEM, OPENBIOS_PROFILE))
    elif log:
        log(f"OpenBIOS backend already present under {fw / 'generated'}")

    scph_rom = fw / "bios" / "SCPH1001.BIN"
    if include_scph1001 and scph_rom.is_file():
        if force or not bios_backend_present(fw, SCPH1001_STEM):
            needed.append((SCPH1001_STEM, SCPH1001_PROFILE))
        elif log:
            log("SCPH1001 backend already present")

    if not needed:
        return CmdResult(True, "BIOS backends already generated")

    # Prefer regen_bios.sh (builds emitter + fingerprints). It does not configure
    # the recompiler — ensure a usable tree (or a found binary) first.
    script = fw / "tools" / "regen_bios.sh"
    bash = shutil.which("bash") or shutil.which("bash.exe")
    need_emitter_setup = not (
        _any_usable_recompiler_build(fw) or _find_psxrecomp_bios(fw, root) is not None
    )
    if need_emitter_setup or not (script.is_file() and bash):
        r = ensure_bios_emitter(fw, game_root=root, dry_run=dry_run, log=log)
        if not r.ok:
            return r

    done: list[str] = []
    for stem, profile in needed:
        if log:
            log(f"Regenerating {stem} via {profile}…")
        r = _regen_bios_profile(
            fw, profile, game_root=root, dry_run=dry_run, log=log
        )
        if not r.ok:
            return CmdResult(
                False,
                f"Failed to regenerate {stem}: {r.message}",
                r.detail,
            )
        if not dry_run and not bios_backend_present(fw, stem):
            return CmdResult(
                False,
                f"Regen finished but {stem} backend still missing under {fw / 'generated'}",
            )
        done.append(stem)

    return CmdResult(True, "BIOS ready: " + ", ".join(done))


def configure(
    root: Path,
    *,
    build_dir: str = DEFAULT_BUILD_DIR,
    build_type: str = DEFAULT_BUILD_TYPE,
    generator: str | None = None,
    extra_args: list[str] | None = None,
    dry_run: bool = False,
    log: LogFn | None = None,
    ensure_bios: bool = True,
) -> CmdResult:
    root = root.expanduser().resolve()
    host = detect_host()
    if not host.cmake:
        return CmdResult(False, "cmake not found on PATH")
    if not (root / "CMakeLists.txt").is_file():
        return CmdResult(False, f"No CMakeLists.txt in {root}")

    if ensure_bios and not _allow_no_bios(extra_args):
        bios_r = ensure_bios_backends(root, dry_run=dry_run, log=log)
        if not bios_r.ok:
            return bios_r
        if log and bios_r.message:
            log(bios_r.message)

    pre = preflight_max_players(root)
    if pre is not None:
        if log:
            log(pre.message)
        return pre

    bdir = Path(build_dir)
    if not bdir.is_absolute():
        bdir = root / bdir
    gen = generator if generator is not None else default_generator(host)
    cmd = [host.cmake, "-S", str(root), "-B", str(bdir)]
    if gen:
        cmd.extend(["-G", gen])
    cmd.append(f"-DCMAKE_BUILD_TYPE={build_type}")
    if extra_args:
        cmd.extend(extra_args)

    if dry_run:
        msg = "dry-run: " + " ".join(cmd)
        if log:
            log(msg)
        return CmdResult(True, msg)

    r = _run_stream(cmd, root, log=log)
    if r.ok:
        r = CmdResult(
            True,
            f"Configured {bdir.name} ({build_type}" + (f", {gen}" if gen else "") + ")",
            r.detail,
        )
        return r
    hint = diagnose_configure_failure(r.detail or "", root)
    if hint:
        msg = f"{r.message}\n{hint}"
        if log:
            log(hint)
        return CmdResult(False, msg, r.detail)
    return r


def build(
    root: Path,
    *,
    build_dir: str = DEFAULT_BUILD_DIR,
    target: str = DEFAULT_TARGET,
    jobs: int | None = None,
    dry_run: bool = False,
    log: LogFn | None = None,
) -> CmdResult:
    root = root.expanduser().resolve()
    host = detect_host()
    if not host.cmake:
        return CmdResult(False, "cmake not found on PATH")
    bdir = Path(build_dir)
    if not bdir.is_absolute():
        bdir = root / bdir
    if not bdir.is_dir():
        return CmdResult(False, f"Build dir missing — Configure first: {bdir}")

    j = jobs if jobs and jobs > 0 else host.jobs
    cmd = [host.cmake, "--build", str(bdir), "--target", target, "-j", str(j)]
    if dry_run:
        msg = "dry-run: " + " ".join(cmd)
        if log:
            log(msg)
        return CmdResult(True, msg)

    r = _run_stream(cmd, root, log=log)
    if r.ok:
        exe = find_runtime_exe(bdir)
        hint = f" → {exe.name}" if exe else ""
        r = CmdResult(True, f"Built {target} in {bdir.name}{hint}", r.detail)
    return r


def find_runtime_exe(build_dir: Path) -> Path | None:
    """Locate the game product binary under a CMake build tree."""
    build_dir = build_dir.expanduser().resolve()
    if not build_dir.is_dir():
        return None

    host = detect_host()
    suffixes = {""}
    if host.label == "windows":
        suffixes = {".exe"}

    # Prefer names that look like Recompiled products / known targets.
    ranked: list[tuple[int, Path]] = []
    skip_dirs = {
        "CMakeFiles",
        "_deps",
        ".cmake",
        "Testing",
        "CMakeTmp",
        "assets",
        "bios",
        "fonts",
        "img",
        "mods",
    }

    def consider(p: Path) -> None:
        if not p.is_file():
            return
        name = p.name
        lower = name.lower()
        if host.label == "windows":
            if not lower.endswith(".exe"):
                return
            stem = name[:-4]
        else:
            if any(
                lower.endswith(ext)
                for ext in (".so", ".dll", ".dylib", ".a", ".lib", ".pdb", ".cmake", ".ninja")
            ):
                return
            stem = name
        if stem.lower() in ("cmake", "ninja", "cpack", "ctest"):
            return
        score = 0
        if "recompil" in lower:
            score += 100
        if stem in ("psx-runtime", "psx-runtime.exe") or stem == "psx-runtime":
            score += 50
        if p.parent == build_dir:
            score += 20
        if host.label != "windows" and not os.access(p, os.X_OK):
            return
        ranked.append((score, p))

    for p in build_dir.iterdir():
        if p.is_file():
            consider(p)

    for sub in build_dir.iterdir():
        if not sub.is_dir() or sub.name in skip_dirs or sub.name.startswith("."):
            continue
        if sub.name in ("Debug", "Release", "RelWithDebInfo", "MinSizeRel") or host.label == "windows":
            for p in sub.iterdir():
                if p.is_file():
                    consider(p)

    if not ranked:
        return None
    ranked.sort(key=lambda t: (-t[0], t[1].name.lower()))
    return ranked[0][1]


def resolve_build_dir(root: Path, build_dir: str) -> Path:
    root = root.expanduser().resolve()
    bdir = Path(build_dir)
    if not bdir.is_absolute():
        bdir = root / bdir
    return bdir


def launch(
    root: Path,
    *,
    build_dir: str = DEFAULT_BUILD_DIR,
    exe: Path | str | None = None,
    env_text: str = "",
    extra_args: list[str] | None = None,
    dry_run: bool = False,
    log: LogFn | None = None,
) -> CmdResult:
    """Start the local product build (non-blocking)."""
    global _active_launch
    root = root.expanduser().resolve()
    bdir = resolve_build_dir(root, build_dir)
    exe_path = Path(exe) if exe else find_runtime_exe(bdir)
    if exe_path is None:
        return CmdResult(False, f"No runtime executable found under {bdir}")
    if not exe_path.is_file():
        return CmdResult(False, f"Executable missing: {exe_path}")

    overlay = parse_env_text(env_text)
    env = os.environ.copy()
    env.update(overlay)
    # Run from game root so relative game.toml / disc / saves resolve.
    cmd = [str(exe_path), *(extra_args or [])]

    if dry_run:
        preview = " ".join(f"{k}={v}" for k, v in overlay.items())
        msg = "dry-run: " + (f"env {preview} " if preview else "") + " ".join(cmd)
        if log:
            log(msg)
        return CmdResult(True, msg)

    with _launch_lock:
        if _active_launch and _active_launch.poll() is None:
            return CmdResult(
                False,
                f"Already running (pid {_active_launch.pid}) — Stop first",
            )
        try:
            kwargs: dict = {
                "cwd": str(root),
                "env": env,
            }
            host = detect_host()
            if host.label == "windows":
                # Detach from Studio console; GUI apps don't need a console.
                kwargs["creationflags"] = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
                kwargs["stdout"] = subprocess.DEVNULL
                kwargs["stderr"] = subprocess.DEVNULL
            else:
                kwargs["start_new_session"] = True
                kwargs["stdout"] = subprocess.DEVNULL
                kwargs["stderr"] = subprocess.DEVNULL
            proc = subprocess.Popen(cmd, **kwargs)
        except OSError as exc:
            return CmdResult(False, "Launch failed", str(exc))
        _active_launch = LaunchHandle(
            proc=proc, exe=exe_path, cwd=root, env_overlay=overlay
        )

    env_note = ""
    if overlay:
        env_note = " env=[" + ", ".join(sorted(overlay)) + "]"
    msg = f"Launched {exe_path.name} (pid {proc.pid}){env_note}"
    if log:
        log(msg)
    return CmdResult(True, msg)


def stop_launch() -> CmdResult:
    global _active_launch
    with _launch_lock:
        h = _active_launch
        if h is None or h.poll() is not None:
            _active_launch = None
            return CmdResult(False, "No running launch")
        pid = h.pid
        h.terminate()
        try:
            h.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            h.proc.kill()
        _active_launch = None
    return CmdResult(True, f"Stopped pid {pid}")


def launch_status() -> str:
    with _launch_lock:
        h = _active_launch
        if h is None:
            return "not running"
        code = h.poll()
        if code is None:
            return f"running pid={h.pid} ({h.exe.name})"
        return f"exited code={code} ({h.exe.name})"


# ---------------------------------------------------------------------------
# Local release export
#
# Studio could build a title but never bundle one: the only path to a
# distributable package was "install release.yml, commit, push, wait for CI".
# That made a local mingw/linux export something you did by hand, which is how
# a broken package (an empty Mods page) survived so long -- nobody produced one
# locally to look inside.
#
# This runs the SAME wrapper CI runs (scripts/package_setup_release.sh), so a
# local export and a CI release are the same artifact by construction rather
# than by convention.
# ---------------------------------------------------------------------------

def release_version(root: Path) -> str:
    """Version the packager will stamp: $RELEASE_VERSION, else the VERSION file."""
    env_v = os.environ.get("RELEASE_VERSION", "").strip()
    if env_v:
        return env_v
    vf = root / "VERSION"
    if vf.is_file():
        text = vf.read_text(encoding="utf-8", errors="replace").strip()
        if text:
            return text
    return "0.0.0"


def default_artifact_tag(host: BuildHost | None = None) -> str:
    """CI's matrix tag for this host, so local zips match the released names."""
    h = host or detect_host()
    if h.label == "windows":
        return "windows-x64"
    if h.label == "macos":
        return "macos-arm64"
    return "linux-x64"


def export_release(
    root: Path,
    build_dir: str = "build-release",
    artifact_tag: str | None = None,
    recompiler_build: str = "build-recompiler",
    *,
    exclude_dev_mods: bool = False,
    log: LogFn | None = None,
) -> CmdResult:
    """Package a distributable zip from an existing build. Returns its path.

    exclude_dev_mods reproduces what CI publishes -- channel = "developer"
    packages dropped. A local export keeps them by default, because the point
    of exporting locally is to test the work in progress.
    """
    wrapper = root / "scripts" / "package_setup_release.sh"
    if not wrapper.is_file():
        return CmdResult(
            False,
            "No scripts/package_setup_release.sh",
            "Emit it first (Migrate -> packager), or install release CI.",
        )
    build_path = root / build_dir
    if not build_path.is_dir():
        return CmdResult(False, f"No build directory: {build_dir}",
                         "Build the runtime target first.")
    # Fail here rather than inside the packager: this is the exact condition
    # that produced mod-less releases, and the message should name the cause.
    if not (build_path / "mods").is_dir():
        return CmdResult(
            False,
            f"{build_dir}/mods is missing",
            "runtime.cmake stages the mod catalog next to the exe on every "
            "build. Rebuild the runtime target; packaging without it would "
            "ship a title whose Mods page is empty.",
        )
    tag = artifact_tag or default_artifact_tag()
    env = dict(os.environ)
    env.setdefault("RELEASE_VERSION", release_version(root))
    # Explicit either way: the packager defaults this from $CI, and a studio
    # export must not inherit that by accident on a CI-flavoured machine.
    env["EXCLUDE_DEV_MODS"] = "1" if exclude_dev_mods else "0"
    res = _run_stream(
        ["bash", str(wrapper), build_dir, tag, recompiler_build],
        root, log=log, env=env,
    )
    if not res.ok:
        return res
    zips = sorted((root / "dist").glob(f"*-{tag}.zip"),
                  key=lambda p: p.stat().st_mtime, reverse=True)
    if not zips:
        return CmdResult(False, "Packager reported success but wrote no zip",
                         res.detail)
    return CmdResult(True, f"Exported {zips[0].name}", str(zips[0]))
