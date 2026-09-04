#!/usr/bin/env python3
"""Create a portable release ZIP from a staged directory.

PowerShell's Compress-Archive writes Windows backslashes into entry names.
They are tolerated by Explorer, but are not valid ZIP path separators and
break several Unix extractors. This helper always emits sorted POSIX names.
"""

from __future__ import annotations

import argparse
import datetime as dt
import os
from pathlib import Path
import tempfile
import zipfile


def _zip_datetime(path: Path) -> tuple[int, int, int, int, int, int]:
    stamp = dt.datetime.fromtimestamp(path.stat().st_mtime)
    if stamp.year < 1980:
        stamp = stamp.replace(year=1980, month=1, day=1, hour=0, minute=0, second=0)
    return stamp.year, stamp.month, stamp.day, stamp.hour, stamp.minute, stamp.second


def create_release_zip(source: Path, output: Path) -> None:
    source = source.resolve()
    output = output.resolve()
    if not source.is_dir():
        raise ValueError(f"source is not a directory: {source}")
    try:
        output.relative_to(source)
    except ValueError:
        pass
    else:
        raise ValueError("output ZIP must not be inside the staged source tree")

    output.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(
        prefix=f".{output.name}.", suffix=".tmp", dir=output.parent
    )
    os.close(fd)
    temporary = Path(temporary_name)
    try:
        with zipfile.ZipFile(
            temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
        ) as archive:
            files = sorted(
                (path for path in source.rglob("*") if path.is_file()),
                key=lambda path: path.relative_to(source).as_posix(),
            )
            for path in files:
                entry_name = path.relative_to(source).as_posix()
                if "\\" in entry_name or entry_name.startswith("/"):
                    raise ValueError(f"invalid ZIP entry name: {entry_name!r}")
                info = zipfile.ZipInfo(entry_name, _zip_datetime(path))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.create_system = 3
                info.external_attr = (path.stat().st_mode & 0xFFFF) << 16
                with path.open("rb") as source_file, archive.open(info, "w") as target:
                    while chunk := source_file.read(1024 * 1024):
                        target.write(chunk)
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    create_release_zip(args.source, args.output)
    print(args.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
