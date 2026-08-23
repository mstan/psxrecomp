#!/usr/bin/env python3
"""Headless disc → generate / rebuild / optional PGO for psxrecomp games.

Commands:
  verify-disc        Hash-check a dump against game.toml [prepare_disc]
  generate           Ensure emitters + prepare disc + run psxrecomp-game → generated/
  rebuild            cmake --build; if [pgo] enabled, instrument → train → use
  pgo-train          Standalone PGO train (same as rebuild's PGO phase)
  ensure-toolchain   Resolve / download cmake-clang-v1 into the shared cache
  ensure-emitters    Build psxrecomp-game + psxrecomp-bios when missing

Exit codes: 0 ok · 1 runtime · 2 usage · 3 disc verify fail
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Optional

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))
from sdk_progress import ProgressReporter  # noqa: E402
from toolchain_pack import (  # noqa: E402
    ensure_toolchain as _ensure_toolchain_pack,
    resolve_toolchain_bin,
    toolchain_bin_runs,
)

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_USAGE = 2
EXIT_VERIFY = 3


def resolve_embedded_toolchain_bin(project_root: Path) -> Optional[Path]:
    """Return a usable toolchain bin/ (project, env, or shared cache)."""
    return resolve_toolchain_bin(project_root)


def activate_embedded_toolchain(
    project_root: Path, progress: Optional[ProgressReporter] = None
) -> bool:
    """Prepend resolved toolchain bin/ to PATH for cmake/ninja/clang."""
    log = progress.log if progress else None
    bin_dir = resolve_toolchain_bin(project_root)
    if not bin_dir or not toolchain_bin_runs(bin_dir, log=log):
        return False
    from toolchain_pack import activate_toolchain_bin

    activate_toolchain_bin(bin_dir, log=log)
    return True


def ensure_toolchain_for_rebuild(
    project_root: Path,
    progress: ProgressReporter,
    *,
    from_zip: str = "",
    download: bool = True,
    min_version: str = "",
) -> bool:
    """Ensure cmake is available via cache / download / offline zip."""
    try:
        _ensure_toolchain_pack(
            project_root,
            from_zip=Path(from_zip) if from_zip else None,
            download=download and not from_zip,
            min_version=min_version,
            log=progress.log,
        )
        return True
    except Exception as exc:  # noqa: BLE001 — surface to progress UI
        progress.log(f"Toolchain ensure: {exc}")
        return False


def prune_after_rebuild(
    project_root: Path,
    build_dir: Path,
    modes: set[str],
    progress: ProgressReporter,
) -> None:
    """Free disk after a successful rebuild (wizard / one-shot setup)."""
    if not modes:
        return
    if "toolchain" in modes or "all" in modes:
        tc = project_root / "toolchain"
        if tc.is_dir():
            shutil.rmtree(tc, ignore_errors=True)
            progress.log(f"Pruned {tc}")
    if "build-intermediates" in modes or "all" in modes:
        if build_dir.is_dir():
            for name in ("CMakeFiles", ".ninja_deps", ".ninja_log", "CMakeCache.txt",
                         "cmake_install.cmake", "build.ninja", "compile_commands.json"):
                p = build_dir / name
                if p.is_dir():
                    shutil.rmtree(p, ignore_errors=True)
                elif p.is_file():
                    try:
                        p.unlink()
                    except OSError:
                        pass
            # Drop object/lib digests but keep the launch binary + assets/.
            for p in build_dir.rglob("*"):
                if not p.is_file():
                    continue
                if p.suffix in {".o", ".obj", ".a", ".lib", ".pdb", ".ilk", ".exp"}:
                    try:
                        p.unlink()
                    except OSError:
                        pass
            progress.log(f"Pruned build intermediates under {build_dir}")
    if "build-tree" in modes:
        # Keep only the executable + assets next to it, then wipe the rest of
        # the build dir by moving keepers aside — used by aggressive cleanup.
        if build_dir.is_dir():
            keep_names = set()
            for p in build_dir.iterdir():
                if p.is_file() and os.access(p, os.X_OK):
                    keep_names.add(p.name)
                if p.name.lower().endswith(".exe"):
                    keep_names.add(p.name)
            assets = build_dir / "assets"
            staging = build_dir.parent / f".prune-keep-{build_dir.name}"
            if staging.exists():
                shutil.rmtree(staging, ignore_errors=True)
            staging.mkdir(parents=True, exist_ok=True)
            for name in keep_names:
                src = build_dir / name
                if src.is_file():
                    shutil.move(str(src), str(staging / name))
            if assets.is_dir():
                shutil.move(str(assets), str(staging / "assets"))
            shutil.rmtree(build_dir, ignore_errors=True)
            staging.rename(build_dir)
            progress.log(f"Pruned build tree to binary+assets under {build_dir}")


def clamp_future_mtimes(
    root: Path,
    *,
    skip: Optional[Path] = None,
    now: Optional[float] = None,
) -> int:
    """Clamp mtimes ahead of *now* so Ninja does not infinite-reconfigure.

    Release zips often preserve CI clocks that are slightly ahead of a user's
    clock (timezone / skew). Ninja then treats every source as newer than
    ``build.ninja``, re-runs CMake forever, and fails with
    ``manifest 'build.ninja' still dirty after 100 tries``.
    """
    if not root.is_dir():
        return 0
    stamp = time.time() if now is None else now
    skip_res: Optional[Path] = None
    if skip is not None:
        try:
            skip_res = skip.resolve()
        except OSError:
            skip_res = skip
    n = 0
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        dpath = Path(dirpath)
        try:
            d_res = dpath.resolve()
        except OSError:
            d_res = dpath
        # Skip the active build tree entirely (outputs are rewritten anyway).
        if skip_res is not None and (
            d_res == skip_res or skip_res in d_res.parents
        ):
            dirnames[:] = []
            continue
        # Never descend into VCS metadata; prune the build dir at the parent.
        pruned: list[str] = []
        for x in dirnames:
            if x == ".git":
                continue
            if skip_res is not None:
                try:
                    if (dpath / x).resolve() == skip_res:
                        continue
                except OSError:
                    pass
            pruned.append(x)
        dirnames[:] = pruned
        for name in filenames:
            p = dpath / name
            try:
                mtime = p.stat().st_mtime
            except OSError:
                continue
            if mtime > stamp:
                try:
                    os.utime(p, (stamp, stamp), follow_symlinks=False)
                    n += 1
                except OSError:
                    pass
    return n


def _parse_array_items(inner: str) -> list[Any]:
    items: list[Any] = []
    if not inner.strip():
        return items
    for part in re.split(r",(?=(?:[^\"]*\"[^\"]*\")*[^\"]*$)", inner):
        part = part.strip().rstrip(",").strip()
        if not part:
            continue
        if part.startswith('"') and part.endswith('"'):
            items.append(part[1:-1])
        else:
            try:
                items.append(int(part, 0))
            except ValueError:
                items.append(part)
    return items


def parse_toml_simple(text: str) -> dict[str, dict[str, Any]]:
    sections: dict[str, dict[str, Any]] = {"": {}}
    cur = ""
    array_key: Optional[str] = None
    array_buf: list[str] = []
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if array_key is not None:
            array_buf.append(line)
            joined = " ".join(array_buf)
            if "]" in joined:
                inner = joined.split("]", 1)[0]
                if inner.startswith("["):
                    inner = inner[1:]
                sections[cur][array_key] = _parse_array_items(inner)
                array_key = None
                array_buf = []
            continue
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            cur = line[1:-1].strip()
            sections.setdefault(cur, {})
            continue
        if "=" not in line:
            continue
        key, val = line.split("=", 1)
        key, val = key.strip(), val.strip()
        if val.startswith("["):
            if val.endswith("]") and val.count("[") == val.count("]"):
                sections[cur][key] = _parse_array_items(val[1:-1])
            else:
                array_key = key
                array_buf = [val[1:]]
            continue
        if val.startswith('"') and val.endswith('"'):
            sections[cur][key] = val[1:-1]
        elif val.lower() in ("true", "false"):
            sections[cur][key] = val.lower() == "true"
        else:
            try:
                sections[cur][key] = int(val, 0)
            except ValueError:
                sections[cur][key] = val
    return sections


def file_hashes(path: Path) -> tuple[str, str, int]:
    h_md5, h_sha1 = hashlib.md5(), hashlib.sha1()
    size = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1024 * 1024)
            if not chunk:
                break
            size += len(chunk)
            h_md5.update(chunk)
            h_sha1.update(chunk)
    return h_md5.hexdigest(), h_sha1.hexdigest(), size


def resolve_cue_bin(cue_path: Path) -> Path:
    text = cue_path.read_text(encoding="utf-8", errors="replace")
    m = re.search(r'FILE\s+"([^"]+)"\s+BINARY', text, re.I)
    if not m:
        m = re.search(r"FILE\s+(\S+)\s+BINARY", text, re.I)
    if not m:
        raise ValueError(f"no BINARY FILE in cue: {cue_path}")
    cand = Path(m.group(1))
    if not cand.is_absolute():
        cand = cue_path.parent / cand
    if not cand.is_file():
        raise ValueError(f"cue references missing bin: {cand}")
    return cand


def _find_recompiler_tool(project_root: Path, basename: str, env_name: str) -> Path:
    env = os.environ.get(env_name, "").strip()
    if env:
        p = Path(env).expanduser()
        if p.is_file():
            return p.resolve()
    names = [basename, f"{basename}.exe"]
    search_dirs = [
        # The dedicated analyzer tree comes FIRST. ensure_analyzer() builds and
        # source-stamps that one, and an older psxrecomp-analyze left behind in
        # the shared emitters tree would otherwise shadow every rebuild — the
        # tool would be rebuilt correctly and then never used.
        project_root / "psxrecomp" / "recompiler" / "build-analyze",
        ROOT / "recompiler" / "build-analyze",
        project_root / "psxrecomp" / "recompiler" / "build",
        project_root / "psxrecomp" / "recompiler" / "build" / "Release",
        project_root / "build-recompiler",
        ROOT / "recompiler" / "build",
        ROOT / "recompiler" / "build" / "Release",
    ]
    for d in search_dirs:
        for name in names:
            c = d / name
            if c.is_file():
                return c.resolve()
    which = shutil.which(basename)
    if which:
        return Path(which).resolve()
    raise FileNotFoundError(
        f"{basename} not found. Build it:\n"
        "  cmake -S psxrecomp/recompiler -B psxrecomp/recompiler/build -G Ninja\n"
        f"  cmake --build psxrecomp/recompiler/build --target {basename}\n"
        f"Or set {env_name}=/path/to/{basename}"
    )


def find_psxrecomp_game(project_root: Path) -> Path:
    return _find_recompiler_tool(project_root, "psxrecomp-game", "PSXRECOMP_GAME")


def find_psxrecomp_bios(project_root: Path) -> Path:
    return _find_recompiler_tool(project_root, "psxrecomp-bios", "PSXRECOMP_BIOS")


def find_psxrecomp_analyze(project_root: Path) -> Path:
    return _find_recompiler_tool(project_root, "psxrecomp-analyze", "PSXRECOMP_ANALYZE")


def find_emitters(project_root: Path) -> tuple[Path, Path]:
    """Return (psxrecomp-game, psxrecomp-bios); raises FileNotFoundError if either missing."""
    return find_psxrecomp_game(project_root), find_psxrecomp_bios(project_root)


def recompiler_source_dir(project_root: Path) -> Path:
    fw = framework_root(project_root)
    src = fw / "recompiler"
    if not (src / "CMakeLists.txt").is_file():
        raise FileNotFoundError(
            f"recompiler sources missing under {src} "
            "(need a full psxrecomp checkout / submodule, not a pruned zipball)"
        )
    return src


def ensure_emitters(
    project_root: Path,
    progress: ProgressReporter,
    *,
    download_toolchain: bool = True,
    force: bool = False,
) -> tuple[Path, Path]:
    """Return emitter binaries, building them with the portable toolchain if needed.

    Matches setup_project.sh / tools/ci/build_emitters.sh so a GitHub fork can
    Generate & rebuild without a pre-staged setup-host SDK.
    """
    if not force:
        try:
            game, bios = find_emitters(project_root)
            progress.log(f"Emitters ready: {game.name}, {bios.name}")
            return game, bios
        except FileNotFoundError:
            pass

    progress.phase(
        "emitters",
        pct=0.12,
        message="Building psxrecomp-game + psxrecomp-bios…",
    )
    _build_recompiler_targets(
        project_root,
        progress,
        # psxrecomp-analyze is deliberately NOT built here. It has its own
        # ensure_analyzer()/build tree, and requesting it from the shared
        # emitters build breaks `ensure-emitters` outright on any checkout whose
        # recompiler/CMakeLists.txt predates the target ("ninja: error: unknown
        # target"). The emitters path must keep working on old framework pins.
        ("psxrecomp-game", "psxrecomp-bios"),
        download_toolchain=download_toolchain,
        clean=force,
    )

    try:
        game, bios = find_emitters(project_root)
    except FileNotFoundError as exc:
        raise RuntimeError(
            f"emitters build finished but binaries not found: {exc}"
        ) from exc
    progress.log(f"Emitters built: {game} , {bios}")
    return game, bios


# Source files that make up psxrecomp-analyze. Kept in step with the target's
# source list in recompiler/CMakeLists.txt.
ANALYZER_SOURCES = (
    "recompiler/src/ps1_exe_parser.cpp",
    "recompiler/src/mips_decoder.cpp",
    "recompiler/src/function_analysis.cpp",
    "recompiler/src/analysis_db.cpp",
    "recompiler/src/bios_call_names.cpp",
    "recompiler/src/analysis_export.cpp",
    "recompiler/src/widescreen_scan.cpp",
    "recompiler/src/main_analyze.cpp",
    "recompiler/include/analysis_db.h",
    "recompiler/src/analysis_export.h",
    "recompiler/include/widescreen_scan.h",
    "runtime/include/ws_cull_detect.h",
)


def _analyzer_source_hash(project_root: Path) -> str:
    """Digest of the analyzer's sources, or "" if they cannot be read.

    The toolchain stamp notices a changed COMPILER; nothing noticed changed
    CODE, so a psxrecomp-analyze built before a feature landed kept being reused
    and rejected the very options the caller had just learned to pass. This is
    the same guard `psxrecomp-game --codegen-hash` provides for the emitters,
    computed here rather than baked in so it needs no CMake support.
    """
    import hashlib

    fw = framework_root(project_root)
    h = hashlib.sha256()
    for rel in ANALYZER_SOURCES:
        p = fw / rel
        if not p.is_file():
            return ""
        h.update(rel.encode("utf-8"))
        h.update(p.read_bytes())
    return h.hexdigest()


def _analyzer_stamp_path(tool: Path) -> Path:
    return tool.parent / (tool.name + ".srcstamp")


def ensure_analyzer(
    project_root: Path,
    progress: ProgressReporter,
    *,
    download_toolchain: bool = True,
) -> Path:
    """Return psxrecomp-analyze, building ONLY that target when it is missing.

    Every build tree created before this target existed has game+bios but no
    analyzer, so `analyze` has to be able to add one. It must not do that by
    rebuilding the emitters: they already work, a relink is slow, and on a repo
    whose tree was configured with the portable cmake-clang toolchain against a
    newer system glibc the psxrecomp-game link fails outright on
    ``__isoc23_strtoul`` — a pre-existing toolchain mismatch that has nothing to
    do with analysis. Building the one target that is actually missing avoids
    dragging that failure into an unrelated command.
    """
    src = recompiler_source_dir(project_root)
    dedicated = src / "build-analyze"

    # If this project already has the dedicated tree, always run the build step
    # rather than trusting the binary's presence. Ninja is a no-op when nothing
    # changed (tens of milliseconds), and it is the only thing that notices an
    # analyzer SOURCE edit — the toolchain stamp catches a changed compiler, not
    # changed code, so a stale binary would otherwise keep producing old-format
    # reports indefinitely.
    want = _analyzer_source_hash(project_root)
    try:
        existing = find_psxrecomp_analyze(project_root)
        stamp = _analyzer_stamp_path(existing)
        have = stamp.read_text(encoding="utf-8").strip() if stamp.is_file() else ""
        if want and have == want:
            return existing
        progress.log(
            "psxrecomp-analyze is stale for the current sources — rebuilding"
            if have
            else "psxrecomp-analyze has no source stamp — rebuilding once"
        )
    except FileNotFoundError:
        pass

    progress.phase("emitters", pct=0.12, message="Building psxrecomp-analyze…")
    _build_recompiler_targets(
        project_root,
        progress,
        ("psxrecomp-analyze",),
        download_toolchain=download_toolchain,
        build_dir_override=dedicated,
        # The analyzer needs fmt and nothing else. Configuring its own tree with
        # libchdr and the test suite off keeps `analyze` independent of whatever
        # state the emitters' build directory is in — a half-configured cache, a
        # cold FetchContent needing the network, or a project path containing a
        # colon, all of which fail the shared configure for reasons that have
        # nothing to do with static analysis.
        extra_cmake_args=("-DPSXRECOMP_ENABLE_CHD=OFF", "-DBUILD_TESTING=OFF"),
    )
    try:
        tool = find_psxrecomp_analyze(project_root)
    except FileNotFoundError as exc:
        raise RuntimeError(
            f"psxrecomp-analyze build finished but the binary was not found: {exc}"
        ) from exc
    if want:
        try:
            _analyzer_stamp_path(tool).write_text(want, encoding="utf-8")
        except OSError:
            pass
    progress.log(f"Analyzer built: {tool}")
    return tool


def _toolchain_stamp(clang_cxx: Optional[str], cmake_tool: Optional[str]) -> str:
    """Identity of the toolchain that a build directory's objects were made with.

    The portable toolchain is addressed through a stable ``latest`` symlink, so
    an upgrade behind it leaves CMAKE_CXX_COMPILER unchanged and CMake's
    compiler-change detection never fires. Ninja then relinks object files
    built by the previous toolchain.

    That is not theoretical: cmake-clang-v1 v1.0.10 shipped no sysroot and
    compiled against the host's glibc, so `strtoul` became `__isoc23_strtoul`
    (glibc >= 2.38). v1.0.14 added a bundled older sysroot that exports no such
    symbol, and every build tree from before the upgrade failed to link with
    "undefined symbol: __isoc23_strtoul" while nothing looked out of date.

    Resolving the symlink puts the concrete version in the stamp, so the
    upgrade becomes visible and the tree can be wiped.
    """
    parts: list[str] = []
    for tool in (clang_cxx, cmake_tool):
        if not tool:
            continue
        real = Path(tool).resolve()
        parts.append(str(real))
        try:
            parts.append(str(real.stat().st_size))
        except OSError:
            pass
        manifest = real.parent.parent / "retcomm-toolchain.json"
        if manifest.is_file():
            try:
                parts.append(manifest.read_text(encoding="utf-8", errors="replace"))
            except OSError:
                pass
    return "\n".join(parts)


def _build_recompiler_targets(
    project_root: Path,
    progress: ProgressReporter,
    targets: tuple[str, ...],
    *,
    download_toolchain: bool = True,
    build_dir_override: Path | None = None,
    extra_cmake_args: tuple[str, ...] = (),
    clean: bool = False,
) -> Path:
    """Configure recompiler/ and build `targets`. Returns the build directory."""
    if not activate_embedded_toolchain(project_root, progress):
        if not download_toolchain or not ensure_toolchain_for_rebuild(
            project_root, progress, download=True
        ):
            if not (_which_tool("cmake") and _which_tool("ninja")):
                raise RuntimeError(
                    "Cannot build emitters: no portable toolchain and no "
                    "cmake+ninja on PATH. Finish the launcher toolchain step, "
                    "or run: python3 psxrecomp/psxrecomp_cli.py ensure-toolchain"
                )
            progress.log("Using system cmake/ninja on PATH for emitters")
        elif not activate_embedded_toolchain(project_root, progress):
            raise RuntimeError(
                "Toolchain ensure succeeded but bin/ is not usable for emitters"
            )

    src = recompiler_source_dir(project_root)
    # Prefer recompiler/build (packaging / RetComM harvest layout); also keep
    # project-root build-recompiler if that is where prior binaries lived.
    build_dir = src / "build"
    try:
        existing_game = find_psxrecomp_game(project_root)
        # Rebuild into the directory that already holds a partial install.
        if existing_game.parent != build_dir:
            alt = project_root / "build-recompiler"
            if existing_game.parent.resolve() == alt.resolve():
                build_dir = alt
    except FileNotFoundError:
        pass

    if build_dir_override is not None:
        build_dir = build_dir_override

    build_dir.mkdir(parents=True, exist_ok=True)
    ninja = _which_tool("ninja")
    clang_c = _which_tool("clang")
    clang_cxx = _which_tool("clang++")
    cmake = _which_tool("cmake")
    if cmake is None:
        raise RuntimeError("cmake not found on PATH after toolchain activate")

    # Wipe when the caller asked for a clean build, or when the toolchain that
    # produced this tree's objects is not the one about to link them.
    stamp_file = build_dir / ".retcomm-toolchain-stamp"
    stamp = _toolchain_stamp(clang_cxx, cmake)
    if clean:
        progress.log(f"Clean rebuild requested — removing {build_dir}")
        shutil.rmtree(build_dir, ignore_errors=True)
        build_dir.mkdir(parents=True, exist_ok=True)
    elif stamp and stamp_file.is_file():
        try:
            previous = stamp_file.read_text(encoding="utf-8", errors="replace")
        except OSError:
            previous = ""
        if previous and previous != stamp:
            progress.log(
                "Toolchain changed since this build directory was configured — "
                f"removing {build_dir} so objects are rebuilt against it"
            )
            shutil.rmtree(build_dir, ignore_errors=True)
            build_dir.mkdir(parents=True, exist_ok=True)
    elif stamp and not stamp_file.is_file() and (build_dir / "CMakeCache.txt").is_file():
        # A tree configured before stamping existed. Its objects may predate the
        # current toolchain with no way to tell, and a mismatch surfaces only as
        # an undefined symbol at link time. Rebuild once, then stamp.
        progress.log(
            f"Unstamped build directory {build_dir} — rebuilding once so its "
            "objects are known to match the active toolchain"
        )
        shutil.rmtree(build_dir, ignore_errors=True)
        build_dir.mkdir(parents=True, exist_ok=True)

    cache_file = build_dir / "CMakeCache.txt"
    gen: list[str] = []
    if ninja is not None:
        cached_gen = _read_cmake_cache_generator(cache_file)
        if cached_gen and cached_gen != "Ninja":
            progress.log(f'Replacing emitters cmake generator "{cached_gen}" with Ninja…')
            shutil.rmtree(build_dir, ignore_errors=True)
            build_dir.mkdir(parents=True, exist_ok=True)
        gen = ["-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}"]
    elif sys.platform == "win32":
        progress.log(
            "warning: ninja not on PATH — emitters cmake may pick NMake Makefiles"
        )

    cmake_args = [
        str(cmake),
        "-S",
        str(src),
        "-B",
        str(build_dir),
        *gen,
        "-DCMAKE_BUILD_TYPE=Release",
    ]
    if clang_c is not None:
        cmake_args.append(f"-DCMAKE_C_COMPILER={clang_c}")
    if clang_cxx is not None:
        cmake_args.append(f"-DCMAKE_CXX_COMPILER={clang_cxx}")
    # Portable llvm-mingw / cmake-clang-v1: static CRT so staged emitters need
    # no MSYS2 GCC DLLs (same as tools/ci/build_emitters.sh).
    if clang_c is not None or clang_cxx is not None:
        cmake_args.append("-DPSXRECOMP_STATIC_CLI=ON")
    cmake_args.extend(extra_cmake_args)

    progress.log(" ".join(cmake_args))
    proc = subprocess.run(
        cmake_args, cwd=str(project_root), capture_output=True, text=True
    )
    for stream in (proc.stdout, proc.stderr):
        if stream:
            for line in stream.splitlines():
                if line.strip():
                    progress.log(line)
    if proc.returncode != 0:
        raise RuntimeError(
            f"emitters cmake configure failed (exit {proc.returncode})"
        )

    jobs = os.environ.get("CMAKE_BUILD_PARALLEL_LEVEL") or str(os.cpu_count() or 4)
    build_cmd = [str(cmake), "--build", str(build_dir), "--parallel", jobs]
    for target in targets:
        build_cmd += ["--target", target]
    progress.log(" ".join(build_cmd))
    proc = subprocess.run(build_cmd, capture_output=True, text=True)
    for stream in (proc.stdout, proc.stderr):
        if stream:
            for line in stream.splitlines():
                if line.strip():
                    progress.log(line)
    if proc.returncode != 0:
        raise RuntimeError(
            f"recompiler cmake build failed (exit {proc.returncode}) for "
            + ", ".join(targets)
        )
    if stamp:
        try:
            stamp_file.write_text(stamp, encoding="utf-8")
        except OSError:
            pass
    return build_dir


def framework_root(project_root: Path) -> Path:
    """Directory containing bios/*.toml and recompiler/ (usually …/psxrecomp)."""
    cand = project_root / "psxrecomp"
    if (cand / "bios" / "OpenBIOS.toml").is_file() or (
        (cand / "bios").is_dir() and (cand / "recompiler").is_dir()
    ):
        if (cand / "bios").is_dir():
            return cand
    if (ROOT / "bios").is_dir() and (ROOT / "recompiler").is_dir():
        return ROOT
    return cand


def _copy_missing(src: Path, dest: Path) -> None:
    if src.is_dir():
        dest.mkdir(parents=True, exist_ok=True)
        for child in src.iterdir():
            _copy_missing(child, dest / child.name)
        return
    if not src.is_file():
        return
    if dest.is_file():
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dest)


def ensure_framework(
    project_root: Path, *, progress: Optional[ProgressReporter] = None
) -> Path:
    """Ensure project_root/psxrecomp has BIOS profiles + seeds for local generate.

    GitHub zipballs omit git submodules, so RetComM source trees often lack
    psxrecomp/bios. Seed from the SDK pack that ships this CLI (ROOT).
    """
    fw = project_root / "psxrecomp"
    need_seed = not (fw / "bios" / "OpenBIOS.toml").is_file()
    if need_seed and (ROOT / "bios").is_dir():
        if progress:
            progress.log(
                f"Seeding psxrecomp BIOS profiles from SDK -> {fw}"
            )
        fw.mkdir(parents=True, exist_ok=True)
        _copy_missing(ROOT / "bios", fw / "bios")
        seeds_src = ROOT / "recompiler" / "seeds"
        if seeds_src.is_dir():
            _copy_missing(seeds_src, fw / "recompiler" / "seeds")
        (fw / "recompiler" / "build").mkdir(parents=True, exist_ok=True)
    # psxrecomp-bios walks up from bios/*.toml looking for .gitignore/.git/
    # CMakeLists.txt to find the framework root. Zipball trees lack the
    # submodule .git — plant a marker so rom = "bios/openbios.bin" resolves.
    if (fw / "bios").is_dir() and not any(
        (fw / m).exists() for m in (".gitignore", ".git", "CMakeLists.txt")
    ):
        marker = fw / ".gitignore"
        if not marker.is_file():
            marker.write_text(
                "# RetComM SDK seed marker (project-root for psxrecomp-bios)\n",
                encoding="utf-8",
            )
    return framework_root(project_root)


def bios_backend_present(fw: Path, stem: str) -> bool:
    dispatch = fw / "generated" / f"{stem}_dispatch.c"
    full = fw / "generated" / f"{stem}_full.c"
    if not dispatch.is_file() or not full.is_file():
        return False
    try:
        text = dispatch.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return False
    return f"{stem}_psx_bios_backend" in text


def regen_bios_profile(
    project_root: Path,
    profile_rel: str,
    *,
    progress: ProgressReporter,
) -> None:
    fw = framework_root(project_root)
    profile = fw / profile_rel
    if not profile.is_file():
        raise FileNotFoundError(f"BIOS profile not found: {profile}")
    bios_tool = find_psxrecomp_bios(project_root)
    progress.log(f"regen BIOS via {bios_tool.name} --config {profile_rel}")
    (fw / "generated").mkdir(parents=True, exist_ok=True)
    # Pass a framework-relative config path and cwd=fw so toml paths like
    # rom = "bios/openbios.bin" resolve under the framework root (not bios/bios/).
    proc = subprocess.run(
        [str(bios_tool), "--config", profile_rel],
        cwd=str(fw),
        capture_output=True,
        text=True,
    )
    for stream in (proc.stdout, proc.stderr):
        if not stream:
            continue
        for line in stream.splitlines():
            if line.strip():
                progress.log(line)
    if proc.returncode != 0:
        raise RuntimeError(
            f"psxrecomp-bios failed for {profile_rel} (exit {proc.returncode})"
        )


def load_sections(config: Path) -> dict[str, dict[str, Any]]:
    return parse_toml_simple(config.read_text(encoding="utf-8"))


def verify_disc_path(
    disc: Path,
    prep: dict[str, Any],
    *,
    skip_hash: bool,
    progress: ProgressReporter,
) -> dict[str, Any]:
    path = disc.resolve()
    if path.suffix.lower() == ".cue":
        path = resolve_cue_bin(path)
    md5, sha1, size = file_hashes(path)
    identity = {
        "path": str(path),
        "md5": md5,
        "sha1": sha1,
        "size": size,
        "verified": False,
    }
    progress.event("disc", **identity)
    sizes = [int(s) for s in (prep.get("known_sizes") or [])]
    md5s = [str(x).lower() for x in (prep.get("known_md5") or [])]
    sha1s = [str(x).lower() for x in (prep.get("known_sha1") or [])]
    if not md5s and not sha1s and not sizes:
        identity["verified"] = True
        return identity
    if skip_hash:
        return identity
    ok = (md5 in md5s) or (sha1 in sha1s)
    if not ok and sizes and size in sizes and not md5s and not sha1s:
        ok = True
    if not ok:
        raise DiscVerifyError(
            f"disc digests not in prepare_disc.known_* "
            f"(size={size} md5={md5} sha1={sha1})"
        )
    identity["verified"] = True
    return identity


class DiscVerifyError(Exception):
    pass


def cmd_verify_disc(args: argparse.Namespace, progress: ProgressReporter) -> int:
    config = Path(args.config).expanduser().resolve()
    if not config.is_file():
        progress.error(f"config not found: {config}", code=EXIT_USAGE)
        return EXIT_USAGE
    project_root = (
        Path(args.project_root).expanduser().resolve()
        if args.project_root
        else config.parent
    )
    activate_embedded_toolchain(project_root, progress)
    disc = Path(args.disc).expanduser()
    if not disc.is_absolute():
        disc = (project_root / disc).resolve()
    else:
        disc = disc.resolve()
    if not disc.is_file():
        progress.error(f"disc not found: {disc}", code=EXIT_USAGE)
        return EXIT_USAGE
    secs = load_sections(config)
    prep = secs.get("prepare_disc") or {}
    progress.phase("verify", pct=0.1, message=f"Verifying {disc.name}")
    try:
        identity = verify_disc_path(
            disc, prep, skip_hash=bool(args.skip_hash_check), progress=progress
        )
    except DiscVerifyError as exc:
        progress.error(str(exc), code=EXIT_VERIFY, verify_failed=True)
        return EXIT_VERIFY
    progress.phase("done", pct=1.0, message="Disc OK")
    progress.result(ok=True, **identity)
    return EXIT_OK


def run_prepare_disc(
    project_root: Path,
    config: Path,
    source: Path,
    progress: ProgressReporter,
) -> Path:
    script = ROOT / "tools" / "prepare_disc.py"
    if not script.is_file():
        raise RuntimeError(f"missing {script}")
    progress.phase("prepare_disc", pct=0.15, message="Normalizing disc image...")
    cmd = [
        sys.executable,
        str(script),
        "--config",
        str(config),
        "--project-root",
        str(project_root),
        str(source),
    ]
    proc = subprocess.run(cmd, cwd=str(project_root), capture_output=True, text=True)
    out = (proc.stdout or "") + (proc.stderr or "")
    for line in out.splitlines():
        if line.strip():
            progress.log(line)
    if proc.returncode != 0:
        raise RuntimeError(f"prepare_disc failed (exit {proc.returncode})")
    marker = "RESULT_CUE="
    cue = None
    for line in out.splitlines():
        if line.startswith(marker):
            cue = Path(line[len(marker) :].strip())
    if not cue or not cue.is_file():
        raise RuntimeError("prepare_disc did not print RESULT_CUE=")
    return cue.resolve()


def cmd_generate(args: argparse.Namespace, progress: ProgressReporter) -> int:
    config = Path(args.config).expanduser().resolve()
    if not config.is_file():
        progress.error(f"config not found: {config}", code=EXIT_USAGE)
        return EXIT_USAGE
    project_root = (
        Path(args.project_root).expanduser().resolve()
        if args.project_root
        else config.parent
    )
    activate_embedded_toolchain(project_root, progress)
    secs = load_sections(config)
    game = secs.get("game") or {}
    prep = secs.get("prepare_disc") or {}
    recomp = secs.get("recompiler") or {}
    runtime = secs.get("runtime") or {}
    openbios_allowed = bool(runtime.get("openbios", True))

    disc_arg = args.disc
    if not disc_arg:
        disc_arg = game.get("disc") or ""
    if not disc_arg:
        progress.error("no --disc and game.disc empty", code=EXIT_USAGE)
        return EXIT_USAGE
    disc = Path(str(disc_arg)).expanduser()
    if not disc.is_absolute():
        disc = (project_root / disc).resolve()
    else:
        disc = disc.resolve()

    progress.log(f"generate --disc {disc}")
    progress.phase("verify", pct=0.05, message=f"Checking disc {disc.name}")
    try:
        if disc.is_file():
            verify_disc_path(
                disc, prep, skip_hash=bool(args.skip_hash_check), progress=progress
            )
    except DiscVerifyError as exc:
        progress.error(str(exc), code=EXIT_VERIFY, verify_failed=True)
        return EXIT_VERIFY

    boot = str(prep.get("boot_exe") or Path(str(game.get("exe") or "")).name)
    out_rel = str(prep.get("out_dir") or "prepared_disc")
    boot_path = project_root / out_rel / boot
    working_disc = disc
    # Normalize library dumps when boot EXE missing or source looks like ISO/raw.
    need_prep = (not boot_path.is_file()) or disc.suffix.lower() in (
        ".iso",
        ".ISO",
    )
    if need_prep or args.force_prepare:
        try:
            working_disc = run_prepare_disc(project_root, config, disc, progress)
        except Exception as exc:  # noqa: BLE001
            progress.error(str(exc), code=EXIT_ERROR)
            return EXIT_ERROR
    if not boot_path.is_file():
        progress.error(f"boot EXE missing after prepare: {boot_path}", code=EXIT_ERROR)
        return EXIT_ERROR

    # psxrecomp-game reads [game].exe from game.toml only — it does not take
    # the wizard/CLI disc directory. If prepare_disc.out_dir and game.exe
    # disagree (legacy default out_dir=prepared_disc), stage the boot EXE
    # to the path the emitter will open.
    game_exe_rel = str(game.get("exe") or "").strip()
    if game_exe_rel:
        game_exe_path = Path(game_exe_rel).expanduser()
        if not game_exe_path.is_absolute():
            game_exe_path = project_root / game_exe_path
        game_exe_path = game_exe_path.resolve()
        try:
            if game_exe_path != boot_path.resolve():
                game_exe_path.parent.mkdir(parents=True, exist_ok=True)
                if (not game_exe_path.is_file()) or (
                    game_exe_path.stat().st_mtime_ns < boot_path.stat().st_mtime_ns
                ):
                    shutil.copy2(boot_path, game_exe_path)
                    progress.log(
                        f"Staged boot EXE for emitter: {game_exe_path}"
                    )
        except OSError as exc:
            progress.error(
                f"boot EXE ready at {boot_path} but cannot stage to "
                f"game.exe path {game_exe_path}: {exc}",
                code=EXIT_ERROR,
            )
            return EXIT_ERROR

    # ---- BIOS backends (local only; CI ships none) ----
    fw = ensure_framework(project_root, progress=progress)
    try:
        ensure_emitters(
            project_root,
            progress,
            download_toolchain=not bool(getattr(args, "no_toolchain_download", False)),
            force=bool(getattr(args, "force_emitters", False)),
        )
    except Exception as exc:  # noqa: BLE001
        progress.error(str(exc), code=EXIT_ERROR)
        return EXIT_ERROR

    bios_arg = (getattr(args, "bios", None) or "").strip()
    staged_retail = False
    if bios_arg:
        bios_path = Path(bios_arg).expanduser()
        if not bios_path.is_absolute():
            bios_path = (project_root / bios_path).resolve()
        else:
            bios_path = bios_path.resolve()
        if not bios_path.is_file():
            progress.error(f"BIOS not found: {bios_path}", code=EXIT_USAGE)
            return EXIT_USAGE
        dest = fw / "bios" / "SCPH1001.BIN"
        progress.phase("bios", pct=0.15, message="Staging retail BIOS dump...")
        progress.log(f"generate --bios {bios_path}")
        try:
            dest.parent.mkdir(parents=True, exist_ok=True)
            if dest.resolve() != bios_path.resolve():
                shutil.copy2(bios_path, dest)
        except OSError as exc:
            progress.error(f"failed to stage BIOS: {exc}", code=EXIT_ERROR)
            return EXIT_ERROR
        try:
            if args.force_bios or not bios_backend_present(fw, "SCPH1001"):
                progress.phase(
                    "bios", pct=0.2, message="Generating SCPH1001 BIOS C..."
                )
                regen_bios_profile(
                    project_root, "bios/SCPH1001.toml", progress=progress
                )
            else:
                progress.log(
                    "SCPH1001 backend already present — skipping bios regen "
                    "(pass --force-bios to regenerate)"
                )
            staged_retail = True
        except Exception as exc:  # noqa: BLE001
            progress.error(str(exc), code=EXIT_ERROR)
            return EXIT_ERROR
    elif not openbios_allowed and not bios_backend_present(fw, "SCPH1001"):
        progress.error(
            "This title requires a retail BIOS dump. Pass --bios SCPH1001.BIN "
            "(or pick one in the setup wizard).",
            code=EXIT_USAGE,
        )
        return EXIT_USAGE

    if openbios_allowed and (
        args.force_bios or not bios_backend_present(fw, "OpenBIOS")
    ):
        try:
            progress.phase(
                "bios", pct=0.25, message="Generating OpenBIOS backend C..."
            )
            regen_bios_profile(project_root, "bios/OpenBIOS.toml", progress=progress)
        except Exception as exc:  # noqa: BLE001
            # Prefer retail when --bios was given; OpenBIOS is then best-effort.
            if staged_retail:
                progress.log(f"OpenBIOS regen skipped (retail BIOS ok): {exc}")
            else:
                progress.error(str(exc), code=EXIT_ERROR)
                return EXIT_ERROR

    try:
        game_tool, _bios_tool = find_emitters(project_root)
    except FileNotFoundError as exc:
        progress.error(str(exc), code=EXIT_ERROR)
        return EXIT_ERROR

    out_dir = project_root / str(recomp.get("out_dir") or "generated")
    progress.phase(
        "emit",
        pct=0.35,
        message=f"Running psxrecomp-game -> {out_dir}",
    )
    cmd = [str(game_tool), "--config", str(config)]
    # Product Generate: collapse reserved-opcode WARNING spam in the wizard log.
    # The emitter also dedupes to once-per-PC unless PSXRECOMP_VERBOSE_RI_WARNINGS=1.
    verbose_ri = os.environ.get("PSXRECOMP_VERBOSE_RI_WARNINGS", "").strip() not in (
        "",
        "0",
    )
    proc = subprocess.run(
        cmd,
        cwd=str(project_root),
        capture_output=True,
        text=True,
    )
    ri_warn = 0
    for stream in (proc.stdout, proc.stderr):
        if not stream:
            continue
        for line in stream.splitlines():
            if not line.strip():
                continue
            if (
                not verbose_ri
                and "WARNING: reserved opcode" in line
                and "emitting guest RI" in line
            ):
                ri_warn += 1
                continue
            progress.log(line)
    if ri_warn:
        progress.log(
            f"Note: suppressed {ri_warn} reserved-opcode WARNING line(s) "
            "(data-as-code sites; RI raises still emitted). "
            "Set PSXRECOMP_VERBOSE_RI_WARNINGS=1 for full detail."
        )
    if proc.returncode != 0:
        progress.error(
            f"psxrecomp-game failed (exit {proc.returncode})", code=EXIT_ERROR
        )
        return EXIT_ERROR

    marker_name = args.gen_marker or f"{boot}_dispatch.c"
    marker = out_dir / marker_name
    if not marker.is_file():
        # Accept any *_dispatch.c
        hits = list(out_dir.glob("*_dispatch.c"))
        if not hits:
            progress.error(f"generate produced no dispatch under {out_dir}", code=EXIT_ERROR)
            return EXIT_ERROR
        marker = hits[0]

    progress.phase("done", pct=1.0, message="Generate complete")
    progress.result(
        ok=True,
        out_dir=str(out_dir),
        marker=str(marker),
        disc=str(working_disc),
        boot_exe=str(boot_path),
    )
    return EXIT_OK


def _which_tool(name: str) -> Optional[Path]:
    """Resolve *name* or *name*.exe from PATH (Windows PATHEXT-friendly)."""
    for cand in (name, f"{name}.exe"):
        found = shutil.which(cand)
        if found:
            return Path(found)
    return None


def _read_cmake_cache_generator(cache_file: Path) -> str:
    if not cache_file.is_file():
        return ""
    try:
        text = cache_file.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
    for line in text.splitlines():
        if line.startswith("CMAKE_GENERATOR:INTERNAL="):
            return line.split("=", 1)[1].strip()
    return ""


def _cmake_configure(
    project_root: Path,
    build_dir: Path,
    *,
    pgo: str,
    extra: list[str],
    progress: ProgressReporter,
) -> None:
    build_dir.mkdir(parents=True, exist_ok=True)
    cache_file = build_dir / "CMakeCache.txt"
    ninja = _which_tool("ninja")
    clang_c = _which_tool("clang")
    clang_cxx = _which_tool("clang++")

    # Prefer Ninja + pack clang when the portable toolchain is on PATH.
    # On Windows, a prior failed configure often leaves CMakeCache stuck on
    # "NMake Makefiles" with no compiler — wipe that and force Ninja.
    gen: list[str] = []
    if ninja is not None:
        cached_gen = _read_cmake_cache_generator(cache_file)
        if cached_gen and cached_gen != "Ninja":
            progress.log(f'Replacing cmake generator "{cached_gen}" with Ninja…')
            shutil.rmtree(build_dir, ignore_errors=True)
            build_dir.mkdir(parents=True, exist_ok=True)
        gen = ["-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}"]
    elif sys.platform == "win32":
        progress.log(
            "warning: ninja not on PATH — cmake may pick NMake Makefiles "
            "(ensure cmake-clang-v1 bin is active)"
        )

    compiler_args: list[str] = []
    if clang_c is not None:
        compiler_args.append(f"-DCMAKE_C_COMPILER={clang_c}")
    if clang_cxx is not None:
        compiler_args.append(f"-DCMAKE_CXX_COMPILER={clang_cxx}")

    cmd = [
        "cmake",
        "-S",
        str(project_root),
        "-B",
        str(build_dir),
        *gen,
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DPSX_PGO={pgo}",
        *compiler_args,
        *extra,
    ]
    progress.log(" ".join(cmd))
    proc = subprocess.run(cmd, cwd=str(project_root), capture_output=True, text=True)
    for stream in (proc.stdout, proc.stderr):
        if stream:
            for line in stream.splitlines():
                if line.strip():
                    progress.log(line)
    if proc.returncode != 0:
        raise RuntimeError(f"cmake configure failed (exit {proc.returncode})")




def _cmake_build(
    build_dir: Path,
    target: str,
    progress: ProgressReporter,
) -> None:
    jobs = os.environ.get("CMAKE_BUILD_PARALLEL_LEVEL") or str(
        os.cpu_count() or 4
    )
    cmd = [
        "cmake",
        "--build",
        str(build_dir),
        "--parallel",
        jobs,
        "--target",
        target,
    ]
    progress.log(" ".join(cmd))
    proc = subprocess.run(cmd, capture_output=True, text=True)
    for stream in (proc.stdout, proc.stderr):
        if stream:
            for line in stream.splitlines():
                if line.strip():
                    progress.log(line)
    if proc.returncode != 0:
        err = f"cmake --build failed (exit {proc.returncode})"
        combined = "\n".join(
            s for s in (proc.stdout or "", proc.stderr or "") if s
        )
        if "still dirty after" in combined:
            err += (
                " — Ninja reconfigure loop (often release-zip mtimes ahead of "
                "the system clock). Re-run after the CLI clamps future mtimes, "
                "or touch the project tree / fix the clock."
            )
        raise RuntimeError(err)


def _soft_stop(pid: int, timeout: int = 30) -> None:
    try:
        os.kill(pid, 15)  # SIGTERM
    except ProcessLookupError:
        return
    for _ in range(timeout):
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return
        time.sleep(1)
    try:
        os.kill(pid, 9)
    except ProcessLookupError:
        pass


def _pgo_train_warning(*, hide_video: bool) -> str:
    if hide_video:
        return (
            "WARNING: PGO training is running (no video window). Do not cancel "
            "or kill this process until training finishes — interrupting may "
            "corrupt profiles and force a restart of this step."
        )
    return (
        "WARNING: PGO training is running. Do not close the game window or "
        "interact with the application until training finishes — closing or "
        "clicking may corrupt profiles and force a restart of this step."
    )


def run_pgo_train(
    project_root: Path,
    build_dir: Path,
    *,
    exe_basename: str,
    disc: Path,
    config: Path,
    train_secs: int,
    train_runs: int,
    mute_host_audio: bool,
    hide_video: bool,
    progress: ProgressReporter,
) -> None:
    exe = build_dir / exe_basename
    if not exe.is_file():
        exe = build_dir / f"{exe_basename}.exe"
    if not exe.is_file():
        raise RuntimeError(f"train binary missing: {exe_basename} under {build_dir}")

    pgo_dir = build_dir / "pgo"
    if pgo_dir.exists():
        shutil.rmtree(pgo_dir)
    pgo_dir.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    env.setdefault("PSX_BIOS_HLE", "0")
    env["LLVM_PROFILE_FILE"] = str(pgo_dir / "bpe-%p.profraw")
    # Discard SDL output; SPU/CD still advance (profiles stay valid).
    if mute_host_audio:
        env["PSX_HOST_MUTE"] = "1"
    else:
        env.pop("PSX_HOST_MUTE", None)
    # No on-screen FMV (photosensitive / abrasive content). Guest MDEC/GPU
    # still run; host present is skipped. Matches prior CI train shape.
    if hide_video:
        env["PSX_HEADLESS"] = "1"
        env.setdefault("SDL_VIDEODRIVER", "dummy")
    else:
        env.pop("PSX_HEADLESS", None)

    warning = _pgo_train_warning(hide_video=hide_video)
    progress.phase("pgo_train", pct=0.35, message=warning)
    # Always surface on the console even when JSONL owns stdout.
    print(f"\n*** {warning} ***\n", file=sys.stderr, flush=True)
    if mute_host_audio:
        progress.log("Host audio muted for train (PSX_HOST_MUTE=1; SPU still runs).")
    else:
        progress.log("Host audio unmuted for train (speakers will play).")
    if hide_video:
        progress.log(
            "Video hidden for train (--headless / PSX_HEADLESS=1; "
            "guest FMV decode still runs, nothing shown on screen)."
        )
    else:
        progress.log("Video window visible for train (host present paths included).")

    for run in range(1, train_runs + 1):
        progress.phase(
            "pgo_train",
            pct=0.4 + 0.4 * (run - 1) / max(train_runs, 1),
            message=(
                f"WARNING: PGO train run {run}/{train_runs} ({train_secs}s) — "
                + (
                    "Do not cancel this process."
                    if hide_video
                    else "Do not close or interact with the application."
                )
            ),
        )
        cmd = [
            str(exe),
            "--no-launcher",
            "--game",
            str(config),
            "--disc",
            str(disc),
        ]
        if hide_video:
            cmd.append("--headless")
        proc = subprocess.Popen(
            cmd,
            cwd=str(project_root),
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        time.sleep(train_secs)
        if proc.poll() is None:
            _soft_stop(proc.pid)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    n_gcda = len(list(build_dir.rglob("*.gcda")))
    n_raw = len(list(pgo_dir.glob("*.profraw")))
    progress.log(f"PGO profiles: {n_gcda} .gcda, {n_raw} .profraw")
    if n_raw >= 1:
        merge = shutil.which("llvm-profdata")
        if not merge:
            # macOS xcrun
            try:
                r = subprocess.run(
                    ["xcrun", "--find", "llvm-profdata"],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                if r.returncode == 0 and r.stdout.strip():
                    merge = "xcrun"
            except OSError:
                pass
        raws = list(pgo_dir.glob("*.profraw"))
        out = pgo_dir / "default.profdata"
        if merge == "xcrun":
            cmd = ["xcrun", "llvm-profdata", "merge", "-sparse", *[str(p) for p in raws], "-o", str(out)]
        elif merge:
            cmd = [merge, "merge", "-sparse", *[str(p) for p in raws], "-o", str(out)]
        else:
            raise RuntimeError("llvm-profdata not found (Clang PGO merge)")
        subprocess.run(cmd, check=True)
    if n_gcda < 1 and not (pgo_dir / "default.profdata").is_file():
        raise RuntimeError("no PGO profiles written — train did not flush")


def cmd_rebuild(args: argparse.Namespace, progress: ProgressReporter) -> int:
    config = Path(args.config).expanduser().resolve()
    if not config.is_file():
        progress.error(f"config not found: {config}", code=EXIT_USAGE)
        return EXIT_USAGE
    project_root = (
        Path(args.project_root).expanduser().resolve()
        if args.project_root
        else config.parent
    )
    build_dir = Path(args.build_dir).expanduser()
    if not build_dir.is_absolute():
        build_dir = (project_root / build_dir).resolve()
    else:
        build_dir = build_dir.resolve()
    target = args.target or "psx-runtime"
    exe_basename = args.exe_basename or "psx-runtime"

    secs = load_sections(config)
    pgo = secs.get("pgo") or {}
    # Opt-in only: requires game.toml [pgo] enabled = true (or --force-pgo).
    pgo_enabled = bool(pgo.get("enabled", False)) and not args.no_pgo
    if args.force_pgo:
        pgo_enabled = True

    game = secs.get("game") or {}
    prep = secs.get("prepare_disc") or {}
    if args.disc:
        disc = Path(args.disc).expanduser()
        if not disc.is_absolute():
            disc = (project_root / disc).resolve()
        else:
            disc = disc.resolve()
    else:
        cue_name = prep.get("cue_name")
        if cue_name:
            disc = (
                project_root / str(prep.get("out_dir") or "bpe") / str(cue_name)
            ).resolve()
        elif game.get("disc"):
            disc = (project_root / str(game["disc"])).resolve()
            if disc.suffix.lower() == ".bin":
                cue = disc.with_suffix(".cue")
                if cue.is_file():
                    disc = cue
        else:
            disc = None

    zip_arg = getattr(args, "toolchain_zip", "") or ""
    no_dl = bool(getattr(args, "no_toolchain_download", False))
    if zip_arg:
        if not ensure_toolchain_for_rebuild(
            project_root, progress, from_zip=zip_arg, download=False
        ):
            progress.error(
                f"Failed to install toolchain from zip: {zip_arg}",
                code=EXIT_ERROR,
            )
            return EXIT_ERROR
    elif not activate_embedded_toolchain(project_root, progress):
        if no_dl or not ensure_toolchain_for_rebuild(
            project_root, progress, download=True
        ):
            if not (shutil.which("cmake") or shutil.which("cmake.exe")):
                progress.error(
                    "No portable toolchain and no cmake on PATH. "
                    "Pass --toolchain-zip PATH, allow download, "
                    "or set PSXRECOMP_TOOLCHAIN_DIR / RETCOMM_TOOLCHAIN_DIR.",
                    code=EXIT_ERROR,
                )
                return EXIT_ERROR
            progress.log("Using system cmake on PATH")

    cmake_extra = []
    if args.cmake_extra:
        cmake_extra.extend(args.cmake_extra)
    # Full playable link after local generate (not the CI setup-host shape).
    cmake_extra.append("-DBPE_FORCE_SETUP_HOST=OFF")
    cmake_extra.append("-DPSXRECOMP_ALLOW_NO_BIOS=OFF")

    clamped = clamp_future_mtimes(project_root, skip=build_dir)
    if clamped:
        progress.log(
            f"Clamped {clamped} future mtime(s) under {project_root} "
            "(avoids Ninja dirty-manifest loop from release-zip clocks)."
        )

    # mute_host_audio / hide_video: default ON; game.toml / CLI can disable.
    mute_host = True
    if "mute_host_audio" in pgo:
        mute_host = bool(pgo.get("mute_host_audio"))
    if getattr(args, "pgo_audio", False):
        mute_host = False
    if getattr(args, "pgo_mute", False):
        mute_host = True

    hide_video = True
    if "hide_video" in pgo:
        hide_video = bool(pgo.get("hide_video"))
    if getattr(args, "pgo_video", False):
        hide_video = False
    if getattr(args, "pgo_hide_video", False):
        hide_video = True

    try:
        if not pgo_enabled:
            progress.phase("build", pct=0.2, message="cmake Release build...")
            _cmake_configure(
                project_root, build_dir, pgo="", extra=cmake_extra, progress=progress
            )
            _cmake_build(build_dir, target, progress)
        else:
            # Framework defaults (60×2); game.toml / CLI may lengthen for hard titles.
            train_secs = int(getattr(args, "train_secs", 0) or 0)
            if train_secs <= 0:
                train_secs = int(pgo.get("train_secs") or 60)
            train_runs = int(getattr(args, "train_runs", 0) or 0)
            if train_runs <= 0:
                train_runs = int(pgo.get("train_runs") or 2)
            progress.phase(
                "pgo_instrument",
                pct=0.1,
                message="PGO instrumented build...",
            )
            _cmake_configure(
                project_root,
                build_dir,
                pgo="generate",
                extra=cmake_extra,
                progress=progress,
            )
            _cmake_build(build_dir, target, progress)
            if not disc or not Path(disc).is_file():
                raise RuntimeError(
                    f"PGO train needs a playable disc (got {disc!s}). "
                    "Pass --disc or set game.disc / prepare_disc.cue_name."
                )
            run_pgo_train(
                project_root,
                build_dir,
                exe_basename=exe_basename,
                disc=Path(disc),
                config=config,
                train_secs=train_secs,
                train_runs=train_runs,
                mute_host_audio=mute_host,
                hide_video=hide_video,
                progress=progress,
            )
            progress.phase("pgo_use", pct=0.85, message="PGO optimized rebuild...")
            _cmake_configure(
                project_root,
                build_dir,
                pgo="use",
                extra=cmake_extra,
                progress=progress,
            )
            _cmake_build(build_dir, target, progress)
    except Exception as exc:  # noqa: BLE001
        progress.error(str(exc), code=EXIT_ERROR)
        return EXIT_ERROR

    exe = build_dir / exe_basename
    if not exe.is_file():
        exe = build_dir / f"{exe_basename}.exe"
    if not exe.is_file():
        progress.error(f"build succeeded but binary missing: {exe}", code=EXIT_ERROR)
        return EXIT_ERROR

    prune_raw = (getattr(args, "prune_after", None) or "").strip()
    if prune_raw:
        modes = {m.strip() for m in prune_raw.split(",") if m.strip()}
        progress.phase("prune", pct=0.97, message="Pruning toolchain / build bulk...")
        prune_after_rebuild(project_root, build_dir, modes, progress)

    progress.phase("done", pct=1.0, message="Rebuild complete")
    progress.result(ok=True, exe=str(exe), pgo=pgo_enabled)
    return EXIT_OK


def cmd_pgo_train(args: argparse.Namespace, progress: ProgressReporter) -> int:
    args.force_pgo = True
    args.no_pgo = False
    return cmd_rebuild(args, progress)


def cmd_ensure_toolchain(args: argparse.Namespace, progress: ProgressReporter) -> int:
    """Resolve / download / unpack cmake-clang-v1 into the shared RetComM cache."""
    project_root = (
        Path(args.project_root).expanduser().resolve()
        if args.project_root
        else Path.cwd().resolve()
    )
    zip_arg = (args.from_zip or "").strip()
    # Default: reuse cache/env, else download unless --no-download.
    want_download = (not zip_arg) and not bool(getattr(args, "no_download", False))
    if getattr(args, "download", False):
        want_download = not zip_arg
    min_version = (getattr(args, "min_version", None) or "").strip()
    try:
        bin_dir = _ensure_toolchain_pack(
            project_root,
            from_zip=Path(zip_arg) if zip_arg else None,
            download=want_download,
            min_version=min_version,
            log=progress.log,
        )
    except Exception as exc:  # noqa: BLE001
        progress.error(str(exc), code=EXIT_ERROR)
        return EXIT_ERROR
    progress.phase("done", pct=1.0, message=f"Toolchain ready: {bin_dir}")
    progress.result(ok=True, toolchain_bin=str(bin_dir))
    return EXIT_OK


def cmd_ensure_emitters(args: argparse.Namespace, progress: ProgressReporter) -> int:
    """Build psxrecomp-game + psxrecomp-bios when missing (or --force)."""
    project_root = (
        Path(args.project_root).expanduser().resolve()
        if args.project_root
        else Path.cwd().resolve()
    )
    try:
        ensure_framework(project_root, progress=progress)
        game, bios = ensure_emitters(
            project_root,
            progress,
            download_toolchain=not bool(getattr(args, "no_download", False)),
            force=bool(getattr(args, "force", False)),
        )
    except Exception as exc:  # noqa: BLE001
        progress.error(str(exc), code=EXIT_ERROR)
        return EXIT_ERROR
    progress.phase("done", pct=1.0, message="Emitters ready")
    progress.result(ok=True, game=str(game), bios=str(bios))
    return EXIT_OK


def cmd_analyze(args: argparse.Namespace, progress: ProgressReporter) -> int:
    """Static function discovery over the project's boot EXE.

    Distinct from `generate`: this produces knowledge *about* the executable
    (function boundaries, call graph, recovered jump tables, inferred
    signatures) rather than C for the runtime, and it never consults the
    runtime or any capture set to do it.
    """
    project_root = (
        Path(args.project_root).expanduser().resolve()
        if args.project_root
        else Path.cwd().resolve()
    )
    config = Path(args.config).expanduser()
    if not config.is_absolute():
        config = (project_root / config).resolve()

    exe = str(getattr(args, "exe", "") or "").strip()
    seeds = ""
    if not exe:
        if not config.is_file():
            progress.error(
                f"no --exe given and config not found: {config}", code=EXIT_USAGE
            )
            return EXIT_USAGE
        secs = load_sections(config)
        exe = str((secs.get("game") or {}).get("exe") or "").strip()
        seeds = str((secs.get("recompiler") or {}).get("seeds") or "").strip()
        if not exe:
            progress.error(f"[game].exe not set in {config}", code=EXIT_USAGE)
            return EXIT_USAGE

    exe_path = Path(exe).expanduser()
    if not exe_path.is_absolute():
        exe_path = (project_root / exe_path).resolve()
    if not exe_path.is_file():
        progress.error(
            f"boot EXE not found: {exe_path} (run `prepare-disc` first?)",
            code=EXIT_USAGE,
        )
        return EXIT_USAGE

    activate_embedded_toolchain(project_root, progress)
    # Always go through ensure_analyzer: it owns both the "is it there" and the
    # "is it current" questions. Calling find_psxrecomp_analyze() here first
    # made the source-stamp check unreachable whenever a binary already existed,
    # which is exactly the case it was written for.
    try:
        tool = ensure_analyzer(project_root, progress)
    except Exception as exc:  # noqa: BLE001
        progress.error(str(exc), code=EXIT_ERROR)
        return EXIT_ERROR

    out_dir = Path(args.out).expanduser() if args.out else (project_root / "analysis")
    if not out_dir.is_absolute():
        out_dir = (project_root / out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    symbols = Path(args.symbols).expanduser() if args.symbols else (
        project_root / "symbols.toml"
    )
    if not symbols.is_absolute():
        symbols = (project_root / symbols).resolve()

    cmd = [
        str(tool),
        str(exe_path),
        "--out",
        str(out_dir),
        "--emit-tsv",
        str(out_dir / "functions.tsv"),
        "--top",
        str(int(getattr(args, "top", 15) or 15)),
    ]
    if symbols.is_file():
        cmd += ["--symbols", str(symbols)]
    if getattr(args, "no_refs", False):
        cmd.append("--no-refs")
    if getattr(args, "exact", False):
        cmd.append("--exact")
    seeds_arg = str(getattr(args, "seeds", "") or "").strip() or seeds
    if seeds_arg:
        seeds_path = Path(seeds_arg).expanduser()
        if not seeds_path.is_absolute():
            seeds_path = (project_root / seeds_path).resolve()
        if seeds_path.is_file():
            cmd += ["--seeds", str(seeds_path)]
    if getattr(args, "emit_symbols", False):
        cmd += ["--symbols", str(symbols)] if not symbols.is_file() else []
        cmd += [
            "--emit-symbols",
            "--min-confidence",
            str(getattr(args, "min_confidence", "high") or "high"),
        ]
    if getattr(args, "emit_ghidra", False):
        cmd += ["--emit-ghidra", str(out_dir / "psxrecomp_import.py")]
    if getattr(args, "emit_symbol_addrs", False):
        cmd += ["--emit-symbol-addrs", str(out_dir / "symbol_addrs.txt")]
    if getattr(args, "scan_widescreen", False):
        cmd += [
            "--scan-widescreen",
            "--emit-ws-sites",
            str(out_dir / "widescreen_sites.toml"),
        ]
    prev = out_dir / "functions.prev.tsv"
    if getattr(args, "diff", False) and prev.is_file():
        cmd += ["--diff", str(prev)]

    # Keep the last run so the next one can diff against it without the caller
    # having to manage snapshots.
    cur = out_dir / "functions.tsv"
    if cur.is_file():
        shutil.copy2(cur, prev)

    progress.phase("analyze", pct=0.3, message=f"Analyzing {exe_path.name}…")
    progress.log(" ".join(cmd))
    proc = subprocess.run(cmd, cwd=str(project_root), capture_output=True, text=True)
    for stream in (proc.stdout, proc.stderr):
        if stream:
            for line in stream.splitlines():
                if line.strip():
                    progress.log(line)
    if proc.returncode != 0:
        progress.error(
            f"psxrecomp-analyze failed (exit {proc.returncode})", code=EXIT_ERROR
        )
        return EXIT_ERROR

    progress.phase("done", pct=1.0, message=f"Analysis written to {out_dir}")
    progress.result(ok=True, out_dir=str(out_dir), exe=str(exe_path))
    return EXIT_OK


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="psxrecomp_cli", description=__doc__)
    sub = ap.add_subparsers(dest="command", required=True)

    def add_common(p: argparse.ArgumentParser) -> None:
        p.add_argument("--config", default="game.toml", help="game.toml path")
        p.add_argument("--project-root", default="", help="game project root")
        p.add_argument("--json-progress", action="store_true")

    v = sub.add_parser("verify-disc", help="verify disc digests")
    add_common(v)
    v.add_argument("--disc", required=True)
    v.add_argument("--skip-hash-check", action="store_true")
    v.set_defaults(handler=cmd_verify_disc)

    g = sub.add_parser(
        "generate", help="regen BIOS backends + prepare disc + generate game C"
    )
    add_common(g)
    g.add_argument("--disc", default="", help="source dump or working cue/bin")
    g.add_argument(
        "--bios",
        default="",
        help="optional retail BIOS dump (staged as bios/SCPH1001.BIN + regen)",
    )
    g.add_argument(
        "--force-bios",
        action="store_true",
        help="regenerate OpenBIOS even if generated/ backends already exist",
    )
    g.add_argument("--skip-hash-check", action="store_true")
    g.add_argument("--force-prepare", action="store_true")
    g.add_argument("--gen-marker", default="", help="expected marker under out_dir")
    g.add_argument(
        "--force-emitters",
        action="store_true",
        help="rebuild psxrecomp-game/bios even if binaries already exist",
    )
    g.add_argument(
        "--no-toolchain-download",
        action="store_true",
        help="when emitters are missing, do not download cmake-clang-v1",
    )
    g.set_defaults(handler=cmd_generate)

    r = sub.add_parser("rebuild", help="cmake build (+ optional local PGO)")
    add_common(r)
    r.add_argument("--build-dir", required=True)
    r.add_argument("--target", default="psx-runtime")
    r.add_argument("--exe-basename", default="")
    r.add_argument("--disc", default="", help="disc for PGO train")
    r.add_argument(
        "--no-pgo",
        action="store_true",
        help="skip PGO even if game.toml [pgo] enabled=true",
    )
    r.add_argument(
        "--force-pgo",
        action="store_true",
        help="run PGO even without [pgo] enabled=true",
    )
    r.add_argument(
        "--train-secs",
        type=int,
        default=0,
        help="override [pgo] train_secs (default framework 60)",
    )
    r.add_argument(
        "--train-runs",
        type=int,
        default=0,
        help="override [pgo] train_runs (default framework 2)",
    )
    r.add_argument(
        "--pgo-mute",
        action="store_true",
        help="force mute host speakers during train (default)",
    )
    r.add_argument(
        "--pgo-audio",
        action="store_true",
        help="allow host speakers during train (overrides mute default)",
    )
    r.add_argument(
        "--pgo-hide-video",
        action="store_true",
        help="force headless train with no on-screen video (default)",
    )
    r.add_argument(
        "--pgo-video",
        action="store_true",
        help="show a game window during train (includes host present paths)",
    )
    r.add_argument(
        "--prune-after",
        default="",
        help=(
            "comma list after success: toolchain, build-intermediates, "
            "build-tree, all (frees disk for one-shot wizard installs)"
        ),
    )
    r.add_argument(
        "--toolchain-zip",
        default="",
        help="offline cmake-clang-v1-*.zip to install into the shared cache",
    )
    r.add_argument(
        "--no-toolchain-download",
        action="store_true",
        help="do not fetch cmake-clang-v1 when no local pack is found",
    )
    r.add_argument("--cmake-extra", action="append", default=[])
    r.set_defaults(handler=cmd_rebuild)

    e = sub.add_parser(
        "ensure-toolchain",
        help="download or unpack cmake-clang-v1 into the shared cache",
    )
    e.add_argument("--project-root", default="", help="game project root (optional)")
    e.add_argument("--json-progress", action="store_true")
    e.add_argument(
        "--from-zip",
        default="",
        help="install from a local cmake-clang-v1-*.zip (offline)",
    )
    e.add_argument(
        "--download",
        action="store_true",
        help="force download even if a pack is already cached",
    )
    e.add_argument(
        "--no-download",
        action="store_true",
        help="only resolve env/cache/project toolchain/ (no network)",
    )
    e.add_argument(
        "--min-version",
        default="",
        help="require retcomm-toolchain.json version >= this (semver)",
    )
    e.set_defaults(handler=cmd_ensure_toolchain)

    em = sub.add_parser(
        "ensure-emitters",
        help="build psxrecomp-game + psxrecomp-bios when missing",
    )
    em.add_argument("--project-root", default="", help="game project root (optional)")
    em.add_argument("--json-progress", action="store_true")
    em.add_argument(
        "--force",
        action="store_true",
        help="rebuild even if emitter binaries already exist",
    )
    em.add_argument(
        "--no-download",
        action="store_true",
        help="do not fetch cmake-clang-v1 when no local pack is found",
    )
    em.set_defaults(handler=cmd_ensure_emitters)

    an = sub.add_parser(
        "analyze",
        help="static function discovery over the boot EXE (no runtime involved)",
    )
    an.add_argument("--project-root", default="", help="game project root")
    an.add_argument("--config", default="game.toml", help="game.toml path")
    an.add_argument("--exe", default="", help="override the boot EXE path")
    an.add_argument("--out", default="", help="output dir (default: <root>/analysis)")
    an.add_argument("--symbols", default="", help="symbols.toml (default: <root>/symbols.toml)")
    an.add_argument("--seeds", default="", help="seed address file (default: [recompiler].seeds)")
    an.add_argument("--top", default="15", help="rows in the stdout top lists")
    an.add_argument("--exact", action="store_true", help="reachability-only partition")
    an.add_argument("--no-refs", action="store_true", help="skip refs.json")
    an.add_argument("--diff", action="store_true", help="diff against the previous run")
    an.add_argument(
        "--emit-symbols",
        action="store_true",
        help="merge discovered functions into symbols.toml (existing names win)",
    )
    an.add_argument("--min-confidence", default="high", help="gate for --emit-symbols")
    an.add_argument("--emit-ghidra", action="store_true", help="write a Ghidra import script")
    an.add_argument(
        "--emit-symbol-addrs", action="store_true", help="write a decomp symbol map"
    )
    an.add_argument(
        "--scan-widescreen",
        action="store_true",
        help="also scan for [widescreen.cull] site candidates",
    )
    an.add_argument("--json-progress", action="store_true")
    an.set_defaults(handler=cmd_analyze)

    p = sub.add_parser("pgo-train", help="force PGO rebuild+train+use")
    add_common(p)
    p.add_argument("--build-dir", required=True)
    p.add_argument("--target", default="psx-runtime")
    p.add_argument("--exe-basename", default="")
    p.add_argument("--disc", default="")
    p.add_argument("--train-secs", type=int, default=0)
    p.add_argument("--train-runs", type=int, default=0)
    p.add_argument("--pgo-mute", action="store_true")
    p.add_argument("--pgo-audio", action="store_true")
    p.add_argument("--pgo-hide-video", action="store_true")
    p.add_argument("--pgo-video", action="store_true")
    p.add_argument("--cmake-extra", action="append", default=[])
    p.add_argument("--no-pgo", action="store_false", default=False)
    p.add_argument("--force-pgo", action="store_true", default=True)
    p.set_defaults(handler=cmd_pgo_train)

    return ap


def _configure_stdio() -> None:
    """Best-effort: don't crash on Windows cp1252 when logging Unicode."""
    for stream in (sys.stdout, sys.stderr):
        reconf = getattr(stream, "reconfigure", None)
        if callable(reconf):
            try:
                reconf(errors="replace")
            except Exception:  # noqa: BLE001
                pass


def main() -> int:
    _configure_stdio()
    args = build_parser().parse_args()
    progress = ProgressReporter(json_progress=bool(getattr(args, "json_progress", False)))
    try:
        return int(args.handler(args, progress))
    except BrokenPipeError:
        return EXIT_ERROR
    except KeyboardInterrupt:
        progress.error("interrupted", code=EXIT_ERROR)
        return EXIT_ERROR


if __name__ == "__main__":
    raise SystemExit(main())
