#!/usr/bin/env python3
"""Guard the staged mod-catalog layout (<exe-dir>/mods/bundled).

WHY THIS TEST EXISTS
--------------------
Framework commit 4cc04be3 renamed the staged mod catalog from mods/packages to
mods/bundled. The framework's own staging followed, and the shared release
module (tools/release_overlay_stage.ps1, Add-ModCatalog) followed. Five title
CMakeLists did not, because the only thing coupling them to the framework was a
`cmake -E copy_directory` DESTINATION STRING -- no symbol, no header, no link
dependency. Nothing failed to configure, compile or link. The five titles kept
writing their packages into mods/packages, where nothing reads them, and the
defect surfaced only when a release packager ran, in a different repository, on
a later day (bead beads-eio.3.101).

runtime/psx_check_mod_catalog.cmake is the tripwire that makes that state a
build failure. This test exercises it against on-disk fixtures, including a
byte-for-byte reproduction of the broken state as it actually appeared in Ape
Escape's build output: mods/bundled holding the four framework psx.* packages
and mods/packages holding the four game-owned ape.* ones.

It also pins two source invariants in runtime.cmake, so the layout cannot drift
back into per-title copies:
  * the framework declares PRELOADED_MODS_DIR and stages the title's catalog,
  * the build-time guard is registered through cmake_language(DEFER), which is
    what makes it run AFTER any POST_BUILD command a title registered later.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
GUARD = REPO / "runtime" / "psx_check_mod_catalog.cmake"
RUNTIME_CMAKE = REPO / "runtime" / "runtime.cmake"

FW_IDS = [
    "psx.enhancement.cd-speed",
    "psx.enhancement.fast-loading",
    "psx.enhancement.pgxp",
    "psx.presentation.bezel",
]
GAME_IDS = [
    "ape.enhancement.frame-smoothing",
    "ape.enhancement.widescreen",
    "ape.experimental.interpolated-frame-rate",
    "ape.presentation.crt",
]

_failures: list[str] = []


def check(cond: bool, what: str) -> None:
    if cond:
        print(f"  ok   {what}")
    else:
        print(f"  FAIL {what}")
        _failures.append(what)


def make_package(root: Path, pkg_id: str, version: str = "1.0.0") -> None:
    d = root / pkg_id / version
    d.mkdir(parents=True, exist_ok=True)
    (d / "manifest.toml").write_text(
        f'format_version = 5\nid = "{pkg_id}"\nversion = "{version}"\n',
        encoding="utf-8",
    )


def write_manifest(path: Path, ids: list[str]) -> Path:
    path.write_text("\n".join(ids) + "\n", encoding="utf-8")
    return path


def run_guard(
    cmake: str,
    mods_dir: Path,
    manifest: Path,
    *,
    require_staged: int = 1,
    alt_manifests: list[Path] | None = None,
    label: str = "psx-runtime",
) -> subprocess.CompletedProcess[str]:
    argv = [
        cmake,
        f"-DPSX_MODS_DIR={mods_dir.as_posix()}",
        f"-DPSX_CATALOG_MANIFEST={manifest.as_posix()}",
        f"-DPSX_REQUIRE_STAGED={require_staged}",
        f"-DPSX_LABEL={label}",
    ]
    if alt_manifests:
        joined = "|".join(p.as_posix() for p in alt_manifests)
        argv.append(f"-DPSX_CATALOG_ALT_MANIFESTS={joined}")
    argv += ["-P", str(GUARD)]
    return subprocess.run(argv, capture_output=True, text=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmake", default=shutil.which("cmake") or "cmake")
    args = ap.parse_args()

    if not GUARD.is_file():
        print(f"FAIL: guard script missing: {GUARD}")
        return 1
    if shutil.which(args.cmake) is None and not Path(args.cmake).is_file():
        print(f"SKIP: no cmake executable available ({args.cmake})")
        return 0

    with tempfile.TemporaryDirectory(prefix="psx_mod_catalog_") as tmpdir:
        tmp = Path(tmpdir)
        all_ids = sorted(FW_IDS + GAME_IDS)
        manifest = write_manifest(tmp / "expected.txt", all_ids)

        # ---- 1. correct layout: everything under mods/bundled -------------
        print("[1] correct layout (framework + game under mods/bundled)")
        mods = tmp / "good" / "mods"
        for pid in all_ids:
            make_package(mods / "bundled", pid)
        (mods / "README.md").write_text("mods\n", encoding="utf-8")
        r = run_guard(args.cmake, mods, manifest)
        check(r.returncode == 0, "accepts a complete mods/bundled catalog")
        check(
            "staged mod catalog OK: 8 package(s) = 4 game-owned + 4 framework-owned"
            in (r.stdout + r.stderr),
            "reports the derived 4 game-owned + 4 framework-owned breakdown",
        )

        # ---- 2. THE BUG: game packages under the legacy mods/packages ----
        # This is exactly what a framework-master build of Ape Escape produced
        # before the fix: the framework staged its own four into mods/bundled,
        # and the title's own copy_directory put the game's four into
        # mods/packages, where Add-ModCatalog cannot see them.
        print("[2] the regression: game packages under legacy mods/packages")
        mods = tmp / "regression" / "mods"
        for pid in FW_IDS:
            make_package(mods / "bundled", pid)
        for pid in GAME_IDS:
            make_package(mods / "packages", pid)
        r = run_guard(args.cmake, mods, manifest)
        out = r.stdout + r.stderr
        check(r.returncode != 0, "FAILS the build when packages land in mods/packages")
        check("LEGACY mods/packages" in out, "names the legacy layout as the cause")
        for pid in GAME_IDS:
            check(pid in out, f"names the stranded package {pid}")
        check(
            "PRELOADED_MODS_DIR" in out,
            "tells the reader the fix is to declare PRELOADED_MODS_DIR",
        )

        # ---- 3. a declared package simply never staged --------------------
        print("[3] a declared package never reached mods/bundled")
        mods = tmp / "missing" / "mods"
        for pid in FW_IDS + GAME_IDS[:-1]:
            make_package(mods / "bundled", pid)
        r = run_guard(args.cmake, mods, manifest)
        out = r.stdout + r.stderr
        check(r.returncode != 0, "FAILS when a declared package is absent")
        check(GAME_IDS[-1] in out, "names the absent package")
        check(
            "never reached the staged catalog" in out,
            "says the package never reached the catalog",
        )

        # ---- 4. a package directory with no manifest ----------------------
        print("[4] staged package directory without a manifest.toml")
        mods = tmp / "manifestless" / "mods"
        for pid in all_ids:
            make_package(mods / "bundled", pid)
        broken = mods / "bundled" / GAME_IDS[0] / "1.0.0" / "manifest.toml"
        broken.unlink()
        r = run_guard(args.cmake, mods, manifest)
        out = r.stdout + r.stderr
        check(r.returncode != 0, "FAILS when a staged package has no manifest.toml")
        check(GAME_IDS[0] in out, "names the manifest-less package")

        # ---- 5. player-installed legacy packages must NOT fail -----------
        # mods/packages can legitimately hold packages a player installed under
        # the pre-split layout; the runtime's migrate_legacy_root() relocates
        # those into mods/installed. Failing a build over them would be wrong.
        print("[5] foreign (player-installed) packages under mods/packages")
        mods = tmp / "foreign" / "mods"
        for pid in all_ids:
            make_package(mods / "bundled", pid)
        make_package(mods / "packages", "someone.else.mod")
        r = run_guard(args.cmake, mods, manifest)
        out = r.stdout + r.stderr
        check(r.returncode == 0, "does NOT fail over a package this build never staged")
        check("someone.else.mod" in out, "still reports the legacy package it found")
        check("mods/installed" in out, "points at the runtime's own migration path")

        # ---- 6. nothing staged: require_staged distinguishes the callers --
        print("[6] nothing staged at all")
        mods = tmp / "empty" / "mods"
        r = run_guard(args.cmake, mods, manifest, require_staged=0)
        check(r.returncode == 0, "ctest invocation SKIPS an unbuilt tree")
        check("skipped" in (r.stdout + r.stderr), "says it skipped")
        r = run_guard(args.cmake, mods, manifest, require_staged=1)
        check(
            r.returncode != 0,
            "POST_BUILD invocation FAILS when staging produced nothing",
        )

        # ---- 7. sibling runtime target sharing one output directory ------
        # Tomba 2's US and Italian runtimes both land in the build root, so
        # mods/bundled holds whichever linked last. A ctest run after a full
        # build must accept either, not demand one.
        print("[7] sibling target's catalog in a shared output directory")
        ita_ids = sorted(
            [
                "psx.enhancement.cd-speed",
                "psx.enhancement.fast-loading",
                "psx.enhancement.pgxp",
                "psx.presentation.bezel",
                "tombi2.enhancement.skip-fmvs",
                "tombi2.enhancement.widescreen",
                "tombi2.experimental.interpolated-frame-rate",
            ]
        )
        ita_manifest = write_manifest(tmp / "expected_ita.txt", ita_ids)
        mods = tmp / "sibling" / "mods"
        for pid in ita_ids:
            make_package(mods / "bundled", pid)
        r = run_guard(args.cmake, mods, manifest, alt_manifests=[ita_manifest])
        out = r.stdout + r.stderr
        check(r.returncode == 0, "accepts a sibling target's complete catalog")
        check("sibling runtime target" in out, "says which case it took")
        # ...but an INCOMPLETE sibling catalog is still a failure.
        (mods / "bundled" / ita_ids[-1]).rename(
            mods / "bundled" / "unexpected.leftover"
        )
        r = run_guard(args.cmake, mods, manifest, alt_manifests=[ita_manifest])
        check(
            r.returncode != 0,
            "does NOT accept a catalog matching neither target's id set",
        )

    # ---- 8. source invariants in runtime.cmake ---------------------------
    print("[8] runtime.cmake source invariants")
    text = RUNTIME_CMAKE.read_text(encoding="utf-8", errors="replace")
    check(
        "PRELOADED_MODS_DIR" in text,
        "runtime.cmake accepts PRELOADED_MODS_DIR so titles never name the layout",
    )
    check(
        "cmake_language(DEFER CALL _psxrt_finalize_mod_catalog_guards" in text,
        "the build-time guard is deferred, so it runs after a title's POST_BUILD",
    )
    check(
        "psx_check_mod_catalog.cmake" in text,
        "runtime.cmake wires the guard script into the build",
    )
    # The only mods/packages reference left in the staging code must be the
    # legacy purge -- never a copy destination.
    bad = [
        line.strip()
        for line in text.splitlines()
        if "mods/bundled" not in line
        and "mods/packages" in line
        and "copy_directory" in line
    ]
    check(not bad, f"no copy_directory targets mods/packages (found {bad})")

    print()
    if _failures:
        print(f"FAILED: {len(_failures)} check(s)")
        for f in _failures:
            print(f"  - {f}")
        return 1
    print("mod catalog layout guard: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
