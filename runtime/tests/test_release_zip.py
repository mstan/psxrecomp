#!/usr/bin/env python3
"""Regression guard for portable release ZIP entry names."""

from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "tools" / "create_release_zip.py"

with tempfile.TemporaryDirectory() as temporary:
    base = Path(temporary)
    stage = base / "stage"
    (stage / "mods" / "package").mkdir(parents=True)
    (stage / "Tomba Recompiled.exe").write_bytes(b"exe")
    (stage / "mods" / "package" / "manifest.toml").write_text(
        'id = "test"\n', encoding="utf-8"
    )
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
        assert "Tomba Recompiled.exe" in names
        assert "mods/package/manifest.toml" in names
        assert all("\\" not in name for name in names)
        assert archive.read("mods/package/manifest.toml") == b'id = "test"\n'

print("portable release ZIP guard passed")
