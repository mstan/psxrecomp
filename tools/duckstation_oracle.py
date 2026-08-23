#!/usr/bin/env python3
"""duckstation_oracle.py -- set up, build, and run the patched DuckStation oracle.

    python3 duckstation_oracle.py doctor      # can this machine build it?
    python3 duckstation_oracle.py all         # fetch + patch + build + install
    python3 duckstation_oracle.py start --disc disc/game.cue
    python3 duckstation_oracle.py status

Why an oracle
-------------
DuckStation, patched to speak the same JSON-over-TCP debug protocol as
psx-runtime (on port 4371 instead of 4370), answers the first question any
render bug has to answer: did the guest code run the SAME on a known-good
emulator? Identical images mean the recompilation is faithful and the bug is in
our renderer; different images mean the guest diverged and tools/cosim.py can
bracket where.

Where it lives
--------------
NOT in the game repo. The oracle is a developer tool shared by every recomp
title, so it installs into the RetComM data root alongside the toolchains and
catalog:

    ~/.local/share/retcomm/oracle/duckstation/       (Linux / macOS)
    %LOCALAPPDATA%\\retcomm\\oracle\\duckstation\\      (Windows)

Override with RETCOMM_ORACLE_DIR, or move the whole root with RETCOMM_DATA_DIR
(the same variables RetComM Launcher and Studio already honour).

    <root>/src/          pinned upstream checkout with the oracle patch applied
    <root>/build/        cmake/ninja tree
    <root>/app/          the portable install that actually gets run
    <root>/oracle.json   manifest: upstream base, patch hash, build stamp

How it is built
---------------
Exactly the way upstream's own Linux CI builds it
(.github/workflows/linux-appimage-build.yml): system packages for the platform
libraries, plus the prebuilt dependency bundle from
github.com/duckstation/dependencies -- which carries Qt6, SDL3 and the rest, so
nothing here compiles Qt from source. The bundle version and its SHA-256 are
read out of the checkout's own dep/PREBUILT-VERSION and dep/PREBUILT-SHA256SUMS
rather than pinned here, so a base bump cannot silently pair a new source tree
with an old dependency set.

The upstream base itself is pinned in tools/duckstation/pin.json, shared with
the Windows setup.sh, because the oracle patch only applies to that tree.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import socket
import subprocess
import sys
import tarfile
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

HERE = Path(__file__).resolve().parent
PIN_PATH = HERE / "duckstation" / "pin.json"
PATCH_DIR = HERE / "duckstation"

# Platform libraries upstream's install-packages.sh installs on Ubuntu, mapped
# to what a check can actually see. pkg-config names where one exists; the rest
# are checked as headers because several ship no .pc file.
PKGCONFIG_REQUIRED = [
    ("libcurl", "curl development headers"),
    ("openssl", "OpenSSL development headers"),
    ("dbus-1", "D-Bus development headers"),
    ("alsa", "ALSA development headers"),
    ("libudev", "udev development headers"),
    ("libevdev", "libevdev development headers"),
    ("x11", "libX11 development headers"),
    ("xcb", "libxcb development headers"),
    ("wayland-client", "Wayland development headers"),
    ("freetype2", "FreeType development headers"),
    ("fontconfig", "fontconfig development headers"),
    ("harfbuzz", "HarfBuzz development headers"),
    ("egl", "EGL development headers"),
    ("zlib", "zlib development headers"),
]

# Distro hints. Not exhaustive package sets — enough to unblock, with a pointer
# at upstream's own list, which is the authority.
DISTRO_HINTS = {
    "arch": (
        "sudo pacman -S --needed base-devel cmake ninja clang lld nasm patchelf "
        "curl openssl dbus alsa-lib libevdev libinput systemd-libs libx11 libxcb "
        "xcb-util-cursor xcb-util-image xcb-util-keysyms xcb-util-renderutil "
        "xcb-util-wm libxkbcommon-x11 libxrandr libxss wayland libdecor "
        "libpipewire libpulse freetype2 fontconfig harfbuzz mesa libva zlib gtk3"
    ),
    "debian": (
        "sudo bash <checkout>/scripts/appimage/install-packages.sh   "
        "# upstream's own list (Ubuntu/Debian)"
    ),
}


class OracleError(RuntimeError):
    pass


# Mirror of CMakeModules/DuckStationBuildSummary.cmake's environment check, so
# the refusal is predicted here with an explanation instead of surfacing 20
# seconds into a cmake run as "Unsupported environment."
#
# Upstream refuses to configure on Arch-family and NixOS hosts, and its build
# scripts are CC-BY-NC-ND-4.0 with an explicit note that you may not distribute
# patches modifying the build system. We do not patch it. We build in an
# environment it supports instead -- which is what --container does.
def refused_environment() -> Optional[str]:
    osr = Path("/etc/os-release")
    if osr.is_file():
        try:
            text = osr.read_text()
        except OSError:
            text = ""
        for needle in ("ID=arch", "ID_LIKE=arch", "ID=nixos"):
            if needle in text:
                return f"/etc/os-release matches {needle}"
    for var in ("NIX_BUILD_TOP", "NIX_STORE", "IN_NIX_SHELL"):
        if os.environ.get(var):
            return f"${var} is set"
    if Path("/etc/NIXOS").exists():
        return "/etc/NIXOS exists"
    return None


def container_engine() -> Optional[str]:
    for engine in ("podman", "docker"):
        if which(engine):
            return engine
    return None


def image_tag(pin: Dict[str, Any]) -> str:
    return f"localhost/retcomm/duckstation-oracle:{pin['upstream_base'][:12]}"


def image_exists(engine: str, tag: str) -> bool:
    rc, _ = capture([engine, "image", "exists", tag])
    return rc == 0


def log(msg: str) -> None:
    print(f"[oracle] {msg}", flush=True)


# ---------------------------------------------------------------------------
# Locations
# ---------------------------------------------------------------------------

def data_root() -> Path:
    """The RetComM shared data root — same one the launcher and Studio use.

    The precedence must match studio_runner.cpp's retcomm_data_dir() exactly,
    XDG_DATA_HOME included. If the two disagree, the GUI reports an oracle
    that is not where this tool installed it, and neither side is obviously
    wrong — the worst kind of bug to chase.
    """
    env = os.environ.get("RETCOMM_DATA_DIR")
    if env:
        return Path(env).expanduser()
    if sys.platform == "win32":
        base = (os.environ.get("LOCALAPPDATA")
                or os.environ.get("USERPROFILE", str(Path.home())) + "/AppData/Local")
        return Path(base) / "retcomm"
    xdg = os.environ.get("XDG_DATA_HOME")
    if xdg:
        return Path(xdg).expanduser() / "retcomm"
    return Path.home() / ".local" / "share" / "retcomm"


def oracle_root() -> Path:
    env = os.environ.get("RETCOMM_ORACLE_DIR")
    if env:
        return Path(env).expanduser()
    return data_root() / "oracle" / "duckstation"


def download_cache() -> Path:
    return data_root() / "downloads"


def exe_name() -> str:
    return "duckstation-qt.exe" if sys.platform == "win32" else "duckstation-qt"


class Layout:
    def __init__(self, root: Optional[Path] = None):
        self.root = root or oracle_root()
        self.src = self.root / "src"
        self.build = self.root / "build"
        self.app = self.root / "app"
        self.manifest = self.root / "oracle.json"
        self.pidfile = self.root / "oracle.pid"
        self.logfile = self.root / "oracle.log"

    @property
    def binary(self) -> Path:
        return self.app / exe_name()

    @property
    def launcher(self) -> Path:
        return self.app / ("run-oracle.cmd" if sys.platform == "win32"
                           else "run-oracle")

    @property
    def built_binary(self) -> Path:
        return self.build / "bin" / exe_name()


def load_pin() -> Dict[str, Any]:
    if not PIN_PATH.is_file():
        raise OracleError(f"missing pin file: {PIN_PATH}")
    with open(PIN_PATH, "r", encoding="utf-8") as f:
        return json.load(f)


# ---------------------------------------------------------------------------
# Shell helpers
# ---------------------------------------------------------------------------

def run(cmd: List[str], cwd: Optional[Path] = None, env: Optional[Dict[str, str]] = None,
        check: bool = True, quiet: bool = False) -> int:
    if not quiet:
        log("$ " + " ".join(str(c) for c in cmd))
    proc = subprocess.run([str(c) for c in cmd], cwd=str(cwd) if cwd else None,
                          env=env, check=False)
    if check and proc.returncode != 0:
        raise OracleError(f"command failed ({proc.returncode}): {' '.join(map(str, cmd))}")
    return proc.returncode


def capture(cmd: List[str], cwd: Optional[Path] = None) -> Tuple[int, str]:
    proc = subprocess.run([str(c) for c in cmd], cwd=str(cwd) if cwd else None,
                          capture_output=True, text=True)
    return proc.returncode, (proc.stdout + proc.stderr).strip()


def which(name: str) -> Optional[str]:
    return shutil.which(name)


def distro_id() -> str:
    try:
        for line in Path("/etc/os-release").read_text().splitlines():
            if line.startswith("ID_LIKE="):
                return line.split("=", 1)[1].strip().strip('"').split()[0]
            if line.startswith("ID="):
                got = line.split("=", 1)[1].strip().strip('"')
    except OSError:
        return ""
    return got if "got" in dir() else ""


# ---------------------------------------------------------------------------
# doctor
# ---------------------------------------------------------------------------

def pick_compilers(cc: Optional[str], cxx: Optional[str]) -> Tuple[str, str, Optional[str]]:
    """Choose a C/C++ compiler and a linker.

    Upstream CI pins clang-19 for reproducibility. We take whatever clang the
    host has, because pinning a compiler we cannot install is worse than using a
    newer one; gcc is the fallback. A newer clang than CI's occasionally trips a
    diagnostic upstream has not seen, which is why --cc/--cxx exist.
    """
    if cc and cxx:
        return cc, cxx, ("lld" if which("ld.lld") else None)
    for c, x in (("clang", "clang++"), ("gcc", "g++"), ("cc", "c++")):
        if which(c) and which(x):
            linker = "lld" if (c == "clang" and which("ld.lld")) else None
            return c, x, linker
    raise OracleError("no C/C++ compiler found (need clang or gcc)")


def doctor(args: argparse.Namespace) -> int:
    lay = Layout(Path(args.root) if args.root else None)
    ok = True
    print(f"install root : {lay.root}")
    print(f"data root    : {data_root()}")
    print(f"platform     : {platform.system()} {platform.machine()}")
    print()

    print("required tools")
    for tool, why in (("git", "fetch the pinned upstream tree"),
                      ("cmake", "configure the build"),
                      ("ninja", "compile"),
                      ("curl", "download the prebuilt dependency bundle"),
                      ("tar", "extract it")):
        path = which(tool)
        print(f"  {'ok  ' if path else 'MISS'}  {tool:<8} {path or '(not on PATH)':<40} {why}")
        ok = ok and bool(path)

    try:
        cc, cxx, linker = pick_compilers(args.cc, args.cxx)
        print(f"  ok    compiler {cc} / {cxx}" + (f"  (-fuse-ld={linker})" if linker else ""))
    except OracleError as e:
        print(f"  MISS  compiler {e}")
        ok = False

    if which("cmake"):
        rc, out = capture(["cmake", "--version"])
        print(f"        {out.splitlines()[0] if out else ''}")

    print()
    if sys.platform.startswith("linux"):
        print("platform libraries (pkg-config)")
        missing = []
        have_pc = bool(which("pkg-config"))
        if not have_pc:
            print("  MISS  pkg-config — cannot check the platform libraries")
            ok = False
        else:
            for pc, why in PKGCONFIG_REQUIRED:
                rc, _ = capture(["pkg-config", "--exists", pc])
                good = rc == 0
                if not good:
                    missing.append(pc)
                print(f"  {'ok  ' if good else 'MISS'}  {pc:<16} {why}")
            if missing:
                ok = False
                hint = DISTRO_HINTS.get(distro_id() or "", "")
                print()
                print(f"  missing: {', '.join(missing)}")
                if hint:
                    print(f"  try: {hint}")
                print("  upstream's authoritative list: "
                      "<checkout>/scripts/appimage/install-packages.sh")
    print()

    if sys.platform.startswith("linux"):
        ecm = any(Path(c).is_file() for c in (
            "/usr/share/ECM/cmake/ECMConfig.cmake",
            "/usr/lib/cmake/ECM/ECMConfig.cmake",
            "/usr/lib64/cmake/ECM/ECMConfig.cmake"))
        print("optional")
        print(f"  {'ok  ' if ecm else '--  '}  extra-cmake-modules  "
              f"{'found' if ecm else 'absent'}")
        print("        Only needed for `build --wayland`. The default build "
              "passes -DENABLE_WAYLAND=OFF,")
        print("        because a headless offscreen oracle never opens a "
              "Wayland surface.")
        print()

    st = state(lay)
    print("install state")
    for k, v in st.items():
        print(f"  {k:<12} {v}")
    print()
    print("READY" if ok else "NOT READY — install what is marked MISS above")
    return 0 if ok else 1


# ---------------------------------------------------------------------------
# setup: fetch source, deps, patch
# ---------------------------------------------------------------------------

def git_repo_ok(path: Path) -> bool:
    return (path / ".git").exists()


def fetch_source(lay: Layout, pin: Dict[str, Any], force: bool = False) -> None:
    url = pin["upstream_url"]
    base = pin["upstream_base"]
    lay.root.mkdir(parents=True, exist_ok=True)

    if not git_repo_ok(lay.src):
        log(f"cloning {url} (blobless) -> {lay.src}")
        lay.src.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", "--filter=blob:none", "--no-checkout", url, str(lay.src)])

    rc, head = capture(["git", "rev-parse", "HEAD"], cwd=lay.src)
    if rc == 0 and head == base and not force:
        log(f"source already at pinned base {base[:12]}")
    else:
        rc, _ = capture(["git", "cat-file", "-t", base], cwd=lay.src)
        if rc != 0:
            log("fetching pinned base...")
            if run(["git", "fetch", "origin", base], cwd=lay.src, check=False) != 0:
                run(["git", "fetch", "--all"], cwd=lay.src)
        log(f"checking out pinned base {base[:12]}")
        # A dirty tree here is our own patch from a previous run; reset so the
        # patch application below is always from a known state.
        run(["git", "checkout", "--force", "--detach", base], cwd=lay.src)
        run(["git", "submodule", "update", "--init", "--recursive"], cwd=lay.src, check=False)


def deps_platform() -> Tuple[str, str]:
    """(archive name, extracted directory name) for this host."""
    mach = platform.machine().lower()
    if sys.platform == "win32":
        return ("deps-windows-x64.7z", "windows-x64")
    if sys.platform == "darwin":
        return ("deps-macos-universal.tar.xz", "macos-universal")
    if mach in ("x86_64", "amd64"):
        return ("deps-linux-x64.tar.xz", "linux-x64")
    raise OracleError(
        f"no prebuilt dependency bundle for {sys.platform}/{mach}. Upstream ships "
        f"x64 and cross-built arm64/armhf only; a native build here would need "
        f"the dependencies built from source.")


def expected_sha(src: Path, archive: str) -> Optional[str]:
    sums = src / "dep" / "PREBUILT-SHA256SUMS"
    if not sums.is_file():
        return None
    for line in sums.read_text().splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1].lstrip("*") == archive:
            return parts[0]
    return None


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch_deps(lay: Layout, force: bool = False) -> None:
    archive, dirname = deps_platform()
    version_file = lay.src / "dep" / "PREBUILT-VERSION"
    if not version_file.is_file():
        raise OracleError(f"missing {version_file} — is the source checkout complete?")
    version = version_file.read_text().strip()
    want = expected_sha(lay.src, archive)

    dest_dir = lay.src / "dep" / "prebuilt" / dirname
    if dest_dir.is_dir() and any(dest_dir.iterdir()) and not force:
        log(f"prebuilt deps already extracted ({dirname}, {version})")
        return

    cache = download_cache()
    cache.mkdir(parents=True, exist_ok=True)
    local = cache / f"{version}-{archive}"

    if local.is_file() and want:
        got = sha256_file(local)
        if got != want:
            log("cached bundle failed its checksum; re-downloading")
            local.unlink()

    if not local.is_file():
        url = (f"https://github.com/duckstation/dependencies/releases/download/"
               f"{version}/{archive}")
        log(f"downloading {archive} ({version})")
        tmp = local.with_suffix(local.suffix + ".part")
        run(["curl", "-L", "--fail", "--retry", "5", "--retry-all-errors",
             "-o", str(tmp), url])
        tmp.replace(local)

    if want:
        got = sha256_file(local)
        if got != want:
            raise OracleError(
                f"dependency bundle checksum mismatch for {archive}\n"
                f"  expected {want}\n  actual   {got}\n"
                f"  (expected value comes from the checkout's own "
                f"dep/PREBUILT-SHA256SUMS)")
        log(f"checksum ok ({want[:16]}…)")
    else:
        log("WARNING: no PREBUILT-SHA256SUMS entry — bundle not verified")

    target = lay.src / "dep" / "prebuilt"
    target.mkdir(parents=True, exist_ok=True)
    log(f"extracting into {target}")
    if archive.endswith(".7z"):
        seven = which("7z") or which("7za")
        if not seven:
            raise OracleError("7z not found — needed to extract the Windows bundle")
        run([seven, "x", "-y", str(local)], cwd=target)
    else:
        with tarfile.open(local) as tf:
            # Python 3.12+ warns without a filter; 'data' refuses absolute paths
            # and traversal, which is what we want for a downloaded archive.
            try:
                tf.extractall(target, filter="data")
            except TypeError:
                tf.extractall(target)
    if not dest_dir.is_dir():
        raise OracleError(f"extraction did not produce {dest_dir}")


def apply_patch(lay: Layout, pin: Dict[str, Any]) -> None:
    patch = PATCH_DIR / pin["patch"]
    if not patch.is_file():
        raise OracleError(f"missing oracle patch: {patch}")
    rc, _ = capture(["git", "apply", "--reverse", "--check", str(patch)], cwd=lay.src)
    if rc == 0:
        log("oracle patch already applied")
        return
    rc, out = capture(["git", "apply", "--check", str(patch)], cwd=lay.src)
    if rc != 0:
        raise OracleError(
            "the oracle patch does not apply to this tree and is not already "
            "applied. Either the pinned base moved, or the patch was regenerated "
            f"against a different one.\n  patch: {patch}\n  git says: {out}")
    log("applying oracle patch")
    run(["git", "apply", str(patch)], cwd=lay.src)


def cmd_setup(args: argparse.Namespace) -> int:
    lay = Layout(Path(args.root) if args.root else None)
    pin = load_pin()
    fetch_source(lay, pin, force=args.force)
    fetch_deps(lay, force=args.force)
    apply_patch(lay, pin)
    write_manifest(lay, pin, stage="setup")
    log(f"setup complete: {lay.src}")
    return 0


# ---------------------------------------------------------------------------
# builder image
# ---------------------------------------------------------------------------

def build_image(lay: Layout, pin: Dict[str, Any], engine: str, force: bool = False) -> str:
    tag = image_tag(pin)
    if image_exists(engine, tag) and not force:
        log(f"builder image present: {tag}")
        return tag
    pkgs = lay.src / "scripts" / "appimage" / "install-packages.sh"
    if not pkgs.is_file():
        raise OracleError(f"missing {pkgs} — run `setup` first")
    ctx = lay.root / "builder"
    if ctx.exists():
        shutil.rmtree(ctx)
    ctx.mkdir(parents=True)
    # The context carries upstream's own package list, copied verbatim out of
    # the pinned checkout so the image can never install a different set from
    # the one that tree expects.
    shutil.copy2(pkgs, ctx / "install-packages.sh")
    shutil.copy2(PATCH_DIR / "Containerfile", ctx / "Containerfile")
    log(f"building {tag} (ubuntu:22.04 + upstream's package list) — first time is slow")
    run([engine, "build", "-t", tag, "-f", str(ctx / "Containerfile"), str(ctx)])
    return tag


def in_container(engine: str, tag: str, lay: Layout, script: str,
                 env: Optional[Dict[str, str]] = None) -> None:
    cmd = [engine, "run", "--rm",
           "-v", f"{lay.root}:/oracle",
           "-w", "/oracle"]
    for k, v in (env or {}).items():
        cmd += ["-e", f"{k}={v}"]
    cmd += [tag, "bash", "-euo", "pipefail", "-c", script]
    run(cmd)


def cmd_image(args: argparse.Namespace) -> int:
    lay = Layout(Path(args.root) if args.root else None)
    pin = load_pin()
    engine = container_engine()
    if not engine:
        raise OracleError("no podman or docker found")
    build_image(lay, pin, engine, force=args.force)
    return 0


# ---------------------------------------------------------------------------
# build
# ---------------------------------------------------------------------------

def cmake_args(lay: Layout, args: argparse.Namespace, cc: str, cxx: str,
               linker: Optional[str], src: str, build: str) -> List[str]:
    cfg = ["cmake", "-S", src, "-B", build, "-G", "Ninja",
           "-DCMAKE_BUILD_TYPE=Release",
           f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_CXX_COMPILER={cxx}"]
    if linker:
        for v in ("EXE", "MODULE", "SHARED"):
            cfg.append(f"-DCMAKE_{v}_LINKER_FLAGS_INIT=-fuse-ld={linker}")
    if not args.wayland:
        # A headless offscreen oracle never opens a Wayland surface, and turning
        # it off drops the only dependency the prebuilt bundle does not cover
        # (extra-cmake-modules, for ECM/FindWayland).
        cfg.append("-DENABLE_WAYLAND=OFF")
    if args.lto:
        # Upstream CI enables IPO for shipping builds. An oracle does not need
        # it and it roughly doubles the build, so it is opt-in here.
        cfg.append("-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON")
    cfg.extend(args.cmake_arg or [])
    return cfg


def cmd_build(args: argparse.Namespace) -> int:
    lay = Layout(Path(args.root) if args.root else None)
    pin = load_pin()
    if not (lay.src / "CMakeLists.txt").is_file():
        raise OracleError(f"no source at {lay.src} — run `setup` first")

    if args.reconfigure and lay.build.exists():
        log("removing existing build tree (--reconfigure)")
        shutil.rmtree(lay.build)

    refused = refused_environment()
    use_container = args.container if args.container is not None else bool(refused)

    if use_container:
        engine = container_engine()
        if not engine:
            raise OracleError(
                f"this host cannot build DuckStation directly ({refused}) and no "
                f"podman/docker is installed to build it somewhere supported.\n"
                f"  Upstream refuses Arch-family and NixOS hosts in "
                f"CMakeModules/DuckStationBuildSummary.cmake, and its build "
                f"scripts are CC-BY-NC-ND with an explicit note that patches to "
                f"them may not be distributed — so we build in ubuntu:22.04, the "
                f"image its own CI uses, rather than working around the check.\n"
                f"  Install podman, or pass --no-container if you know this host "
                f"is supported.")
        if refused:
            log(f"host build is refused by upstream ({refused}); "
                f"building in a container instead")
        tag = build_image(lay, pin, engine, force=False)
        # clang-19 + lld is what upstream's Linux CI uses; the image installs
        # exactly that, so the container build does not guess at a compiler.
        cfg = cmake_args(lay, args, "clang-19", "clang++-19", "lld",
                         "/oracle/src", "/oracle/build")
        jobs = f"--parallel {args.jobs}" if args.jobs else "--parallel"
        script = " && ".join([
            " ".join(f"'{c}'" for c in cfg),
            f"cmake --build /oracle/build {jobs}",
        ])
        in_container(engine, tag, lay, script)
    else:
        cc, cxx, linker = pick_compilers(args.cc, args.cxx)
        run(cmake_args(lay, args, cc, cxx, linker, str(lay.src), str(lay.build)))
        build = ["cmake", "--build", str(lay.build)]
        build += ["--parallel", str(args.jobs)] if args.jobs else ["--parallel"]
        run(build)

    if not lay.built_binary.is_file():
        raise OracleError(f"build finished but {lay.built_binary} is missing")
    write_manifest(lay, pin, stage="build", container=use_container)
    log(f"built: {lay.built_binary}")
    return 0


# ---------------------------------------------------------------------------
# install: a portable app dir that survives rebuilds
# ---------------------------------------------------------------------------

def find_bios(explicit: Optional[str]) -> Optional[Path]:
    """Locate a retail BIOS image for the oracle.

    Order: what the caller passed, then the RetComM BIOS root, then the engine
    checkout this script lives in. Never downloads one — a BIOS dump is the
    user's own property and is not redistributable.
    """
    if explicit:
        p = Path(explicit).expanduser()
        return p if p.is_file() else None
    index = data_root() / "bios-index.json"
    if index.is_file():
        try:
            with open(index, "r", encoding="utf-8") as f:
                doc = json.load(f)
            root = doc.get("bios_root")
            for name in doc.get("files", []) or []:
                cand = Path(root or "") / (name if isinstance(name, str)
                                           else name.get("name", ""))
                if cand.is_file():
                    return cand
        except (OSError, json.JSONDecodeError, TypeError):
            pass
    for cand in (HERE.parent / "bios" / "SCPH1001.BIN",
                 HERE.parent / "bios" / "scph1001.bin"):
        if cand.is_file():
            return cand
    return None


def merge_ini(path: Path, sections: Dict[str, Dict[str, str]]) -> None:
    """Merge keys into a DuckStation settings.ini, preserving what is there.

    DuckStation writes its own defaults on first launch; we only override the
    handful of fields a headless oracle needs. Writing the whole file ourselves
    would mean guessing at a schema the emulator owns.
    """
    import configparser
    cp = configparser.ConfigParser(strict=False, interpolation=None)
    cp.optionxform = str  # DuckStation keys are case-sensitive
    if path.is_file():
        try:
            cp.read(path, encoding="utf-8")
        except configparser.Error:
            pass
    for sect, keys in sections.items():
        if not cp.has_section(sect):
            cp.add_section(sect)
        for k, v in keys.items():
            cp.set(sect, k, v)
    with open(path, "w", encoding="utf-8") as f:
        cp.write(f, space_around_delimiters=True)


def bundle_dirs(lay: Layout) -> List[Path]:
    """The prebuilt runtime pieces that have to travel with the binary.

    The binary's RUNPATH points at wherever it was linked — inside the build
    container, that path does not exist on the host — and Qt additionally needs
    its plugin tree. Copying both into the install makes `app/` self-contained
    and independent of both the container and the source checkout, which is the
    whole point of installing it outside the repo.
    """
    try:
        _, dirname = deps_platform()
    except OracleError:
        return []
    base = lay.src / "dep" / "prebuilt" / dirname
    return [d for d in (base / "lib", base / "plugins") if d.is_dir()]


def write_launcher(lay: Layout) -> None:
    if sys.platform == "win32":
        lay.launcher.write_text(
            "@echo off\r\nsetlocal\r\n"
            "set \"QT_PLUGIN_PATH=%~dp0plugins\"\r\n"
            "\"%~dp0duckstation-qt.exe\" %*\r\n")
        return
    lay.launcher.write_text(
        "#!/bin/sh\n"
        "# Self-contained launcher for the DuckStation oracle.\n"
        "# The binary's RUNPATH refers to the build environment, so point the\n"
        "# loader and Qt at the copies that live beside it here instead.\n"
        'APP="$(cd "$(dirname "$0")" && pwd)"\n'
        'LD_LIBRARY_PATH="$APP/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\n'
        'QT_PLUGIN_PATH="$APP/plugins"\n'
        "export LD_LIBRARY_PATH QT_PLUGIN_PATH\n"
        'exec "$APP/duckstation-qt" "$@"\n')
    lay.launcher.chmod(0o755)


def verify_install(lay: Layout) -> List[str]:
    """Report any shared library the installed binary still cannot resolve."""
    if not which("ldd") or not lay.binary.is_file():
        return []
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = str(lay.app / "lib") + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    proc = subprocess.run(["ldd", str(lay.binary)], capture_output=True, text=True, env=env)
    return [ln.strip() for ln in proc.stdout.splitlines() if "not found" in ln]


def seed_settings(lay: Layout, timeout: float = 20.0) -> None:
    """Let DuckStation write its own default settings.ini, then stop it.

    We only want to override a handful of fields, and the emulator owns that
    schema — hand-writing a whole settings.ini means guessing at it. Everything
    up to and including the config write happens before the startup blocks on
    anything, so a short run followed by a kill produces a complete default file.
    """
    if (lay.app / "settings.ini").is_file():
        return
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = str(lay.app / "lib") + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    env["QT_PLUGIN_PATH"] = str(lay.app / "plugins")
    if sys.platform.startswith("linux"):
        env["QT_QPA_PLATFORM"] = "offscreen"
    log("seeding DuckStation's own default settings.ini")
    proc = subprocess.Popen([str(lay.binary), "-nogui"], cwd=str(lay.app), env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            start_new_session=True)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if (lay.app / "settings.ini").is_file():
            time.sleep(0.5)   # let the write finish
            break
        if proc.poll() is not None:
            break
        time.sleep(0.25)
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    if not (lay.app / "settings.ini").is_file():
        log("WARNING: DuckStation did not write a settings.ini; writing a minimal one")


def cmd_install(args: argparse.Namespace) -> int:
    lay = Layout(Path(args.root) if args.root else None)
    pin = load_pin()
    if not lay.built_binary.is_file():
        raise OracleError(f"nothing built at {lay.built_binary} — run `build` first")

    src_bin = lay.build / "bin"
    if lay.app.exists():
        shutil.rmtree(lay.app)
    log(f"staging portable install -> {lay.app}")
    shutil.copytree(src_bin, lay.app, symlinks=True)

    for d in bundle_dirs(lay):
        dest = lay.app / d.name
        log(f"bundling {d.name}/ ({d})")
        shutil.copytree(d, dest, symlinks=True, dirs_exist_ok=True)

    write_launcher(lay)

    # Portable mode keeps settings, memcards and saves inside the install
    # instead of ~/.config/duckstation, so the oracle cannot inherit — or
    # disturb — a real DuckStation the user set up for playing games.
    (lay.app / "portable.txt").write_text("")

    bios = find_bios(args.bios)
    if bios:
        (lay.app / "bios").mkdir(exist_ok=True)
        shutil.copy2(bios, lay.app / "bios" / bios.name)
        log(f"bios: {bios} -> app/bios/{bios.name}")
    else:
        log("WARNING: no BIOS image found. Pass --bios <path>, or the oracle "
            "cannot boot a disc. (BIOS dumps are yours to supply.)")

    seed_settings(lay)

    settings = lay.app / "settings.ini"
    bios_name = bios.name if bios else ""
    merge_ini(settings, {
        "Main": {
            "SettingsVersion": "3",
            "SetupWizardIncomplete": "false",
            "ConfirmPowerOff": "false",
            "PauseOnFocusLoss": "false",
            "StartPaused": "false",
        },
        "BIOS": {
            "SearchDirectory": "bios",
            "PathNTSC-U": bios_name,
            "PathNTSC-J": bios_name,
            "PathPAL": bios_name,
            "PatchFastBoot": "true",
        },
        "GPU": {
            # Software is the right renderer for an oracle: deterministic, no
            # GPU context needed headless, and the debug server reads g_vram
            # directly so there is nothing to read back from a real surface.
            "Renderer": "Software",
        },
        "Audio": {"Backend": "Null"},
        "UI": {
            # This is the "don't show again" of the unofficial-build warning
            # (AutoUpdaterDialog::warnAboutUnofficialBuild). It is a MODAL
            # dialog, and under -nogui it is invisible, so an unofficial build
            # blocks forever in the event loop before the core thread ever
            # starts: one thread, no CPU, no port, no explanation. Setting the
            # key the dialog itself writes is the supported way past it — we do
            # not patch the check out. (DuckStation is CC-BY-NC-ND: building for
            # personal use is fine, redistributing a modified build is not, so
            # this install stays on this machine.)
            "UnofficialBuildWarningConfirmed": "true",
        },
    })
    log(f"settings: {settings}")

    missing = verify_install(lay)
    if missing:
        log("WARNING: the installed binary still cannot resolve:")
        for m in missing:
            log(f"    {m}")
        log("  the oracle will not start until these are found")
    else:
        log("link check: every shared library resolves")

    write_manifest(lay, pin, stage="install", bios=str(bios) if bios else None)
    log(f"installed: {lay.launcher}")
    return 0


# ---------------------------------------------------------------------------
# manifest / status
# ---------------------------------------------------------------------------

def write_manifest(lay: Layout, pin: Dict[str, Any], stage: str,
                   bios: Optional[str] = None,
                   container: Optional[bool] = None) -> None:
    doc: Dict[str, Any] = {}
    if lay.manifest.is_file():
        try:
            with open(lay.manifest, "r", encoding="utf-8") as f:
                doc = json.load(f)
        except (OSError, json.JSONDecodeError):
            doc = {}
    patch = PATCH_DIR / pin["patch"]
    doc.update({
        "kind": "psxrecomp-duckstation-oracle",
        "version": 1,
        "upstream_url": pin["upstream_url"],
        "upstream_base": pin["upstream_base"],
        "patch_sha256": sha256_file(patch) if patch.is_file() else None,
        "oracle_port": pin.get("oracle_port", 4371),
        "root": str(lay.root),
        f"{stage}_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    })
    if bios:
        doc["bios"] = bios
    if container is not None:
        doc["built_in_container"] = container
    lay.root.mkdir(parents=True, exist_ok=True)
    with open(lay.manifest, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)


def port_open(port: int, host: str = "127.0.0.1", timeout: float = 1.0) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def oracle_ping(port: int, timeout: float = 3.0) -> Optional[Dict[str, Any]]:
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    except OSError:
        return None
    try:
        s.sendall(b'{"id":1,"cmd":"ping"}\n')
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
        return json.loads(buf.split(b"\n")[0].decode(errors="replace"))
    except (OSError, json.JSONDecodeError):
        return None
    finally:
        s.close()


def state(lay: Layout) -> Dict[str, str]:
    pin = load_pin()
    port = int(pin.get("oracle_port", 4371))
    out = {
        "source": "present" if (lay.src / "CMakeLists.txt").is_file() else "absent",
        "deps": "absent",
        "patch": "unknown",
        "built": "yes" if lay.built_binary.is_file() else "no",
        "installed": "yes" if lay.binary.is_file() else "no",
        "running": "no",
    }
    try:
        _, dirname = deps_platform()
        d = lay.src / "dep" / "prebuilt" / dirname
        out["deps"] = "present" if d.is_dir() and any(d.iterdir()) else "absent"
    except OracleError:
        out["deps"] = "unsupported platform"
    if out["source"] == "present":
        patch = PATCH_DIR / pin["patch"]
        rc, _ = capture(["git", "apply", "--reverse", "--check", str(patch)], cwd=lay.src)
        out["patch"] = "applied" if rc == 0 else "not applied"
    if port_open(port):
        rep = oracle_ping(port)
        out["running"] = (f"yes — answering on {port}" if rep and rep.get("ok")
                          else f"something on {port}, not answering as an oracle")
    return out


def status_doc(lay: Layout) -> Dict[str, Any]:
    """Everything a caller (or a GUI) needs to decide what button to offer."""
    pin = load_pin()
    doc: Dict[str, Any] = {}
    if lay.manifest.is_file():
        try:
            with open(lay.manifest, "r", encoding="utf-8") as f:
                doc.update(json.load(f))
        except (OSError, json.JSONDecodeError):
            pass
    st = state(lay)
    running = st["running"] != "no"
    answering = st["running"].startswith("yes")
    doc.update({
        # Set AFTER the manifest merge: oracle.json carries its own `kind`
        # ("psxrecomp-duckstation-oracle") and would otherwise overwrite this
        # one, so every installed oracle would report a document Studio then
        # rejects as foreign — while an uninstalled one looked fine.
        "kind": "psxrecomp-oracle-status",
        "version": 1,
        "root": str(lay.root),
        "app": str(lay.app),
        "binary": str(lay.binary),
        "launcher": str(lay.launcher),
        "port": int(pin.get("oracle_port", 4371)),
        "source": st["source"] == "present",
        "deps": st["deps"] == "present",
        "patch_applied": st["patch"] == "applied",
        "built": st["built"] == "yes",
        "installed": st["installed"] == "yes",
        "running": running,
        "answering": answering,
        "running_detail": st["running"],
        "container_needed": bool(refused_environment()),
        "container_reason": refused_environment(),
        "container_engine": container_engine(),
    })
    # One word for a status line, so a GUI does not re-derive the ladder.
    if answering:
        doc["state"] = "answering"
    elif running:
        doc["state"] = "port-busy"
    elif doc["installed"]:
        doc["state"] = "installed"
    elif doc["built"]:
        doc["state"] = "built"
    elif doc["source"]:
        doc["state"] = "fetched"
    else:
        doc["state"] = "absent"
    return doc


def cmd_status(args: argparse.Namespace) -> int:
    lay = Layout(Path(args.root) if args.root else None)
    if args.json:
        print(json.dumps(status_doc(lay), indent=1))
        return 0
    print(f"root: {lay.root}")
    if lay.manifest.is_file():
        with open(lay.manifest, "r", encoding="utf-8") as f:
            doc = json.load(f)
        for k in ("upstream_base", "oracle_port", "bios", "setup_at", "build_at",
                  "install_at"):
            if k in doc:
                print(f"  {k:<14} {doc[k]}")
    for k, v in state(lay).items():
        print(f"  {k:<14} {v}")
    return 0


def cmd_path(args: argparse.Namespace) -> int:
    lay = Layout(Path(args.root) if args.root else None)
    print(lay.binary if args.binary else lay.root)
    return 0


# ---------------------------------------------------------------------------
# run
# ---------------------------------------------------------------------------

def cmd_start(args: argparse.Namespace) -> int:
    lay = Layout(Path(args.root) if args.root else None)
    pin = load_pin()
    port = int(args.port or pin.get("oracle_port", 4371))
    if not lay.binary.is_file():
        raise OracleError(f"not installed at {lay.binary} — run `all` first")
    if port_open(port):
        rep = oracle_ping(port)
        if rep and rep.get("ok"):
            log(f"already running and answering on {port}")
            return 0
        raise OracleError(
            f"port {port} is in use by something that is not the oracle. Find it "
            f"with:  ss -ltnp | grep {port}")

    cmd = [str(lay.launcher if lay.launcher.is_file() else lay.binary)]
    if args.disc:
        disc = Path(args.disc).expanduser().resolve()
        if not disc.is_file():
            raise OracleError(f"disc not found: {disc}")
        cmd.append(str(disc))
    else:
        cmd.append("-bios")
    cmd += ["-nogui", "-fastboot"]
    cmd += args.extra or []

    env = dict(os.environ)
    if args.headless and sys.platform.startswith("linux"):
        # No display needed: the software renderer plus the debug server's
        # direct g_vram reads mean nothing has to reach a real surface.
        env.setdefault("QT_QPA_PLATFORM", "offscreen")

    log("$ " + " ".join(cmd))
    with open(lay.logfile, "ab") as logf:
        logf.write(f"\n=== {time.strftime('%Y-%m-%d %H:%M:%S')} {' '.join(cmd)} ===\n"
                   .encode())
        proc = subprocess.Popen(cmd, cwd=str(lay.app), env=env,
                                stdout=logf, stderr=subprocess.STDOUT,
                                start_new_session=True)
    lay.pidfile.write_text(str(proc.pid))
    log(f"pid {proc.pid}, log {lay.logfile}")

    deadline = time.time() + args.wait
    while time.time() < deadline:
        if proc.poll() is not None:
            tail = lay.logfile.read_text(errors="replace").splitlines()[-15:]
            raise OracleError("oracle exited during startup:\n  " + "\n  ".join(tail))
        rep = oracle_ping(port, timeout=1.0)
        if rep and rep.get("ok"):
            log(f"oracle answering on {port} (frame {rep.get('frame', '?')})")
            return 0
        time.sleep(0.5)
    log(f"WARNING: no answer on {port} after {args.wait}s — it may still be booting; "
        f"check {lay.logfile}")
    return 1


def cmd_stop(args: argparse.Namespace) -> int:
    lay = Layout(Path(args.root) if args.root else None)
    if not lay.pidfile.is_file():
        log("no pidfile; nothing started by this tool")
        return 0
    try:
        pid = int(lay.pidfile.read_text().strip())
    except ValueError:
        lay.pidfile.unlink(missing_ok=True)
        return 0
    try:
        os.kill(pid, 15)
        log(f"sent SIGTERM to {pid}")
    except ProcessLookupError:
        log(f"pid {pid} was not running")
    except OSError as e:
        raise OracleError(f"could not stop {pid}: {e}")
    lay.pidfile.unlink(missing_ok=True)
    return 0


def cmd_all(args: argparse.Namespace) -> int:
    rc = cmd_setup(args)
    if rc:
        return rc
    rc = cmd_build(args)
    if rc:
        return rc
    return cmd_install(args)


# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=None,
                    help="install root (default: $RETCOMM_ORACLE_DIR, else "
                         "<retcomm data root>/oracle/duckstation)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def build_opts(p):
        p.add_argument("--cc", default=None, help="C compiler (default: clang, else gcc)")
        p.add_argument("--cxx", default=None, help="C++ compiler")
        p.add_argument("--jobs", type=int, default=0)
        p.add_argument("--lto", action="store_true",
                       help="enable IPO/LTO as upstream release CI does (slower)")
        p.add_argument("--reconfigure", action="store_true",
                       help="delete the build tree first")
        p.add_argument("--container", dest="container", action="store_true",
                       default=None,
                       help="build inside ubuntu:22.04 via podman/docker "
                            "(automatic on hosts upstream refuses)")
        p.add_argument("--no-container", dest="container", action="store_false",
                       help="force a direct host build")
        p.add_argument("--wayland", action="store_true",
                       help="build with Wayland support (needs extra-cmake-modules; "
                            "an offscreen oracle does not use it)")
        p.add_argument("--cmake-arg", action="append", default=None, metavar="-DFOO=BAR")

    p = sub.add_parser("doctor", help="check host prerequisites")
    p.add_argument("--cc", default=None)
    p.add_argument("--cxx", default=None)
    p.set_defaults(func=doctor)

    p = sub.add_parser("setup", help="fetch pinned source + deps, apply the oracle patch")
    p.add_argument("--force", action="store_true", help="re-fetch even if present")
    p.set_defaults(func=cmd_setup)

    p = sub.add_parser("image", help="build the ubuntu:22.04 builder image")
    p.add_argument("--force", action="store_true", help="rebuild even if present")
    p.set_defaults(func=cmd_image)

    p = sub.add_parser("build", help="configure + compile")
    build_opts(p)
    p.set_defaults(func=cmd_build)

    p = sub.add_parser("install", help="stage the portable app dir")
    p.add_argument("--bios", default=None, help="path to a PSX BIOS image")
    p.set_defaults(func=cmd_install)

    p = sub.add_parser("all", help="setup + build + install")
    p.add_argument("--force", action="store_true")
    p.add_argument("--bios", default=None)
    build_opts(p)
    p.set_defaults(func=cmd_all)

    p = sub.add_parser("start", help="launch the oracle headless")
    p.add_argument("--disc", default=None, help="cue/bin/chd to boot")
    p.add_argument("--port", type=int, default=0)
    p.add_argument("--wait", type=float, default=45.0, help="seconds to wait for a ping")
    p.add_argument("--headless", action="store_true", default=True)
    p.add_argument("--windowed", dest="headless", action="store_false",
                   help="show a window (needs a display)")
    p.add_argument("--extra", nargs=argparse.REMAINDER)
    p.set_defaults(func=cmd_start)

    p = sub.add_parser("stop", help="stop an oracle started by this tool")
    p.set_defaults(func=cmd_stop)

    p = sub.add_parser("status", help="where it is and what state it is in")
    p.add_argument("--json", action="store_true",
                   help="machine-readable, for RetComM Studio")
    p.set_defaults(func=cmd_status)

    p = sub.add_parser("path", help="print the install root")
    p.add_argument("--binary", action="store_true", help="print the executable instead")
    p.set_defaults(func=cmd_path)

    args = ap.parse_args(argv)
    for attr, default in (("cc", None), ("cxx", None), ("jobs", 0), ("lto", False),
                          ("reconfigure", False), ("cmake_arg", None),
                          ("force", False), ("bios", None), ("root", None),
                          ("wayland", False), ("container", None), ("json", False)):
        if not hasattr(args, attr):
            setattr(args, attr, default)
    try:
        return args.func(args)
    except OracleError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
