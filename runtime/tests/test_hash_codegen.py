#!/usr/bin/env python3
"""Focused contract tests for hash_codegen.cmake."""

from __future__ import annotations

import re
import shutil
import subprocess
import tempfile
from pathlib import Path


RUNTIME = Path(__file__).resolve().parents[1]
REPO = RUNTIME.parent
SCRIPT = RUNTIME / "hash_codegen.cmake"
SOURCES = RUNTIME / "codegen_hash_sources.cmake"


def run_hash(root: Path, sources: list[Path], out: Path) -> subprocess.CompletedProcess[str]:
    source_arg = ";".join(str(source) for source in sources)
    return subprocess.run(
        [
            "cmake",
            f"-DOUT={out}",
            f"-DROOT={root}",
            f"-DSRCS={source_arg}",
            "-P",
            str(SCRIPT),
        ],
        check=False,
        capture_output=True,
        text=True,
    )


def read_hash(header: Path) -> str:
    match = re.search(r"PSX_OVERLAY_CODEGEN_HASH 0x([0-9a-f]{8})u", header.read_text())
    assert match, f"missing codegen hash in {header}"
    return match.group(1)


def write_fixture(root: Path, first: str, second: str) -> list[Path]:
    source_dir = root / "recompiler" / "src"
    source_dir.mkdir(parents=True, exist_ok=True)
    sources = [source_dir / "first.cpp", source_dir / "second.cpp"]
    sources[0].write_text(first)
    sources[1].write_text(second)
    return sources


def expect_failure(root: Path, sources: list[Path], needle: str) -> None:
    out = root / "failed.h"
    result = run_hash(root, sources, out)
    assert result.returncode != 0, result.stdout
    assert needle in result.stderr, result.stderr
    assert not out.exists(), "failed hashing must not publish a header"


def run_raw(arguments: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["cmake", *arguments, "-P", str(SCRIPT)],
        check=False,
        capture_output=True,
        text=True,
    )


def main() -> None:
    cmake = shutil.which("cmake")
    assert cmake, "cmake is required"

    with tempfile.TemporaryDirectory() as temp:
        workspace = Path(temp)
        checkout_a = workspace / "checkout-a"
        checkout_b = workspace / "elsewhere" / "checkout-b"
        sources_a = write_fixture(checkout_a, "alpha", "beta")
        sources_b = write_fixture(checkout_b, "alpha", "beta")
        out_a = workspace / "a.h"
        out_b = workspace / "b.h"
        assert run_hash(checkout_a, sources_a, out_a).returncode == 0
        assert run_hash(checkout_b, sources_b, out_b).returncode == 0
        assert read_hash(out_a) == read_hash(out_b), "hash depends on checkout path"

        # Raw concatenation makes both sequences spell "abc". Framing must keep
        # the distinct per-file contents distinct.
        boundary_root = workspace / "boundary"
        boundary_sources = write_fixture(boundary_root, "ab", "c")
        boundary_a = workspace / "boundary-a.h"
        assert run_hash(boundary_root, boundary_sources, boundary_a).returncode == 0
        boundary_sources = write_fixture(boundary_root, "a", "bc")
        boundary_b = workspace / "boundary-b.h"
        assert run_hash(boundary_root, boundary_sources, boundary_b).returncode == 0
        assert read_hash(boundary_a) != read_hash(boundary_b), "file boundaries are ambiguous"

        expect_failure(checkout_a, [checkout_a / "missing.cpp"], "missing or not a file")
        empty = checkout_a / "empty.cpp"
        empty.touch()
        expect_failure(checkout_a, [empty], "source is empty")
        expect_failure(checkout_a, [], "at least one source")
        outside = workspace / "outside.cpp"
        outside.write_text("outside")
        expect_failure(checkout_a, [outside], "outside ROOT")

        source_arg = f"-DSRCS={sources_a[0]}"
        missing_root = run_raw([f"-DOUT={workspace / 'missing-root.h'}", source_arg])
        assert missing_root.returncode != 0
        assert "non-empty ROOT" in missing_root.stderr
        missing_out = run_raw([f"-DROOT={checkout_a}", source_arg])
        assert missing_out.returncode != 0
        assert "non-empty OUT" in missing_out.stderr

    source_list = SOURCES.read_text()
    assert "recompiler/src/main_psx.cpp" in source_list
    relative_sources = re.findall(
        r"\$\{PSXRECOMP_CODEGEN_HASH_ROOT\}/([^\s\)]+)", source_list
    )
    assert relative_sources, "canonical codegen source list is empty"
    canonical_sources = [REPO / relative for relative in relative_sources]
    for source in canonical_sources:
        assert source.is_file(), f"canonical codegen source is missing: {source}"
        assert source.stat().st_size > 0, f"canonical codegen source is empty: {source}"
    with tempfile.TemporaryDirectory() as temp:
        canonical_out = Path(temp) / "canonical.h"
        canonical_result = run_hash(REPO, canonical_sources, canonical_out)
        assert canonical_result.returncode == 0, canonical_result.stderr
        read_hash(canonical_out)

    runtime_cmake = (RUNTIME / "runtime.cmake").read_text()
    recompiler_cmake = (REPO / "recompiler" / "CMakeLists.txt").read_text()
    assert "-DROOT=${PSXRECOMP_ROOT}" in runtime_cmake
    assert "-DROOT=${PSXRECOMP_CODEGEN_HASH_ROOT}" in recompiler_cmake
    print("hash_codegen contract checks passed")


if __name__ == "__main__":
    main()
