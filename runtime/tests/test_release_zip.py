#!/usr/bin/env python3
"""Regression guard for portable release ZIP entry names."""

import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "tools" / "create_release_zip.py"

with tempfile.TemporaryDirectory() as temporary:
    base = Path(temporary)
    stage = base / "stage"
    (stage / "mods" / "package").mkdir(parents=True)
    (stage / "mods" / "space dir").mkdir(parents=True)
    (stage / ".release-meta").write_bytes(b"hidden\n")
    (stage / "Tomba Recompiled.exe").write_bytes(b"exe")
    (stage / "mods" / "package" / "manifest.toml").write_bytes(b'id = "test"\n')
    (stage / "mods" / "space dir" / "file name.txt").write_bytes(b"space\n")
    output = base / "release.zip"
    subprocess.run(
        [
            sys.executable,
            str(HELPER),
            "--source",
            str(stage),
            "--output",
            str(output),
        ],
        check=True,
    )
    with zipfile.ZipFile(output) as archive:
        names = archive.namelist()
        assert names == sorted(names)
        assert ".release-meta" in names
        assert "Tomba Recompiled.exe" in names
        assert "mods/package/manifest.toml" in names
        assert "mods/space dir/file name.txt" in names
        assert all(name not in (".", "./") for name in names)
        assert all(not name.startswith("./") for name in names)
        assert all("\\" not in name for name in names)
        assert archive.read("mods/package/manifest.toml") == b'id = "test"\n'

print("portable release ZIP guard passed")
