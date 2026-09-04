#!/usr/bin/env python3
"""Drop developer-channel mod features from a staged catalog tree.

Developer-channel work does not ship. A contributor reaches it by cloning the
repo and building locally; a release must not carry it at all -- absent, not
hidden behind a toggle.

Channels are per FEATURE, so pruning by package directory is no longer right:
a catalog may hold a stable feature and an instrument side by side, and dropping
the directory would take the player-facing feature with it. That is exactly what
a line-anchored `grep '^channel = "developer"'` does, because it cannot tell a
package-level key from one inside a [[feature]] block.

What this does instead:

  * package-level channel = "developer", or every feature developer
        -> remove the version directory (nothing in it ships)
  * some features developer
        -> emit a manifest without those features, and without the
           [[option]] / [[patch]] / [[overlay]] / [[plugin]] / [[resource]] /
           [[constraint]] entries that only served them

Emitting is not rewriting someone's archive. A staged catalog is build output
generated from the title's mods/preloaded/ source (or the framework's
mods/builtin/); the author's manifest is the one in the repo and is never
touched. Installed .psxmod archives are a different matter -- those are never
modified, and the runtime declines to surface developer features from them
instead (see PSX_MOD_DEVELOPER_CHANNEL).

Usage:
    mod_channel_filter.py <catalog-dir> [<catalog-dir> ...] [--quiet]

A catalog dir directly contains <package-id>/<version>/manifest.toml. Missing
directories are skipped. Exits non-zero if any manifest could not be parsed or
if a filtered manifest failed to re-parse, so packaging fails closed rather
than publishing unfinished work.
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:  # pragma: no cover - Python < 3.11
    tomllib = None
    try:
        import tomli as tomllib  # type: ignore[no-redef]
    except ModuleNotFoundError:
        pass

DEVELOPER = "developer"

# A top-level array-of-tables header starts a block. A dotted header
# ([[option.choice]]) belongs to the block above it and rides along with it.
_TABLE = re.compile(r"^\s*\[\[\s*([A-Za-z0-9_.]+)\s*\]\]\s*(?:#.*)?$")
# Only used on lines inside one block, so it cannot pick up a package-level key.
_FEATURE_KEY = re.compile(r"""^\s*feature\s*=\s*["']([^"']*)["']\s*(?:#.*)?$""")
_ID_KEY = re.compile(r"""^\s*id\s*=\s*["']([^"']*)["']\s*(?:#.*)?$""")

# Entries that name the feature they serve and are meaningless without it.
DEPENDENT_TABLES = {
    "option", "patch", "overlay", "plugin", "resource", "constraint",
}


class ManifestError(Exception):
    pass


def _split_blocks(text: str) -> tuple[str, list[tuple[str, str]]]:
    """Return (preamble, [(table-name, block-text), ...]).

    The preamble is everything before the first top-level [[table]] -- the
    package keys. Each block keeps its own trailing blank lines and any comment
    lines that precede its header, so removing one leaves the rest untouched.
    """
    lines = text.splitlines(keepends=True)
    preamble: list[str] = []
    blocks: list[tuple[str, list[str]]] = []
    pending: list[str] = []  # comments/blanks that belong to the NEXT header

    for line in lines:
        match = _TABLE.match(line)
        name = match.group(1) if match else None
        if name is not None and "." not in name:
            if blocks:
                blocks[-1][1].extend([])
            blocks.append((name, pending + [line]))
            pending = []
            continue
        if not blocks:
            if line.strip().startswith("#") or not line.strip():
                pending.append(line)
            else:
                preamble.extend(pending)
                pending = []
                preamble.append(line)
            continue
        if line.strip().startswith("#") or not line.strip():
            pending.append(line)
            continue
        blocks[-1][1].extend(pending)
        pending = []
        blocks[-1][1].append(line)

    if blocks:
        blocks[-1][1].extend(pending)
    else:
        preamble.extend(pending)
    return "".join(preamble), [(n, "".join(b)) for n, b in blocks]


def _block_value(block: str, pattern: re.Pattern[str]) -> str | None:
    for line in block.splitlines():
        if _TABLE.match(line):
            continue
        match = pattern.match(line)
        if match:
            return match.group(1)
    return None


def _block_channel(block: str) -> str | None:
    for line in block.splitlines():
        if _TABLE.match(line):
            continue
        match = re.match(r"""^\s*channel\s*=\s*["']([^"']*)["']\s*(?:#.*)?$""", line)
        if match:
            return match.group(1)
    return None


def filter_manifest(text: str) -> tuple[str | None, list[str]]:
    """Return (filtered text or None to drop the package, dropped feature ids).

    None means nothing in this package ships: the package itself is
    developer-channel, or every feature it defines is.
    """
    if tomllib is None:
        raise ManifestError(
            "no TOML parser available (need Python 3.11+, or tomli installed)")
    try:
        parsed = tomllib.loads(text)
    except Exception as exc:  # tomllib.TOMLDecodeError, and anything it raises
        raise ManifestError(f"unparseable manifest: {exc}") from exc

    package_channel = parsed.get("channel")
    features = parsed.get("feature") or []
    if package_channel == DEVELOPER:
        return None, [str(f.get("id", "")) for f in features]

    dropped = [
        str(f.get("id", ""))
        for f in features
        if f.get("channel", package_channel) == DEVELOPER
    ]
    if not dropped:
        return text, []
    if len(dropped) == len(features):
        return None, dropped

    preamble, blocks = _split_blocks(text)
    kept: list[str] = []
    dropped_option_ids: set[tuple[str, str]] = set()
    for name, block in blocks:
        if name == "feature":
            if _block_value(block, _ID_KEY) in dropped:
                continue
        elif name in DEPENDENT_TABLES:
            owner = _block_value(block, _FEATURE_KEY)
            if owner in dropped:
                if name == "option":
                    dropped_option_ids.add((owner, _block_value(block, _ID_KEY) or ""))
                continue
        kept.append(block)

    filtered = preamble + "".join(kept)
    try:
        reparsed = tomllib.loads(filtered)
    except Exception as exc:
        raise ManifestError(f"filtered manifest does not parse: {exc}") from exc
    surviving = {str(f.get("id", "")) for f in (reparsed.get("feature") or [])}
    if surviving & set(dropped):
        raise ManifestError(
            "developer feature survived filtering: "
            + ", ".join(sorted(surviving & set(dropped))))
    if not surviving:
        raise ManifestError("filtering left a package with no features")
    return filtered, dropped


def filter_catalog(catalog: Path, quiet: bool = False) -> tuple[int, int]:
    """Filter every package under one catalog dir. Returns (removed, edited)."""
    removed = edited = 0
    if not catalog.is_dir():
        return 0, 0
    for manifest in sorted(catalog.glob("*/*/manifest.toml")):
        version_dir = manifest.parent
        package_dir = version_dir.parent
        try:
            filtered, dropped = filter_manifest(
                manifest.read_text(encoding="utf-8"))
        except ManifestError as exc:
            raise ManifestError(f"{manifest}: {exc}") from exc
        if filtered is None:
            shutil.rmtree(version_dir)
            try:
                package_dir.rmdir()
            except OSError:
                pass
            removed += 1
            if not quiet:
                print(f"  excluded developer package: "
                      f"{package_dir.name}/{version_dir.name}")
            continue
        if dropped:
            manifest.write_text(filtered, encoding="utf-8")
            edited += 1
            if not quiet:
                print(f"  excluded developer feature(s) from "
                      f"{package_dir.name}/{version_dir.name}: "
                      f"{', '.join(sorted(dropped))}")
    return removed, edited


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Drop developer-channel mod features from staged catalogs.")
    parser.add_argument("catalog", type=Path, nargs="+",
                        help="directory holding <id>/<version>/manifest.toml")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    removed = edited = 0
    try:
        for catalog in args.catalog:
            r, e = filter_catalog(catalog, args.quiet)
            removed += r
            edited += e
    except ManifestError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    if not args.quiet:
        print(f"developer-channel filter: {removed} package(s) removed, "
              f"{edited} manifest(s) filtered")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
