#!/usr/bin/env python3
"""Replace @TOKEN@ placeholders (and CI YOUR_* tokens) in scaffold files."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

GITHUB_ABOUT_DESCRIPTION = (
    "Made with PSXrecomp, a Sony PlayStation game static recompiler ecosystem · "
    "Part of the R.A.I.D. community"
)
GITHUB_ABOUT_HOMEPAGE = "https://discord.gg/Ad9BwSzctP"


def derive_zip_prefix(name: str) -> str:
    base = re.sub(r"(?i)recomp(iled)?$", "", name).strip()
    caps = re.findall(r"[A-Z][a-z0-9]*|[0-9]+", base)
    if len(caps) >= 3:
        acr = "".join(w[0] for w in caps if w and w[0].isalpha()).lower()
        if 3 <= len(acr) <= 8:
            return acr
    slug = re.sub(r"[^a-z0-9]+", "", base.lower())
    return (slug or "game")[:20]


def sanitize_github_name(name: str) -> str:
    """GitHub owner/repo slug: spaces → '-', drop illegal chars, keep [A-Za-z0-9._-]."""
    s = (name or "").strip()
    if not s:
        return ""
    s = re.sub(r"\s+", "-", s)
    s = re.sub(r"[^A-Za-z0-9._-]+", "", s)
    s = re.sub(r"-{2,}", "-", s)
    s = s.strip(".-")
    return s[:100] or "repo"


def install_dir_name(name: str) -> str:
    """Local checkout / launcher ``apps/`` folder — same slug as the GitHub repo name.

    New-project wizard must create this folder (not the display name with spaces)
    so Studio catalog matching and RetComM ``install_dir_name`` stay consistent.
    """
    return sanitize_github_name(name)


def normalize_repo_key(name: str) -> str:
    """Casefold + collapse whitespace/underscores to hyphens for folder/catalog match.

    Lets ``Wipeout 3 Special Edition Recomp`` match catalog
    ``Wipeout-3-Special-Edition-Recomp`` / github short name without renaming.
    """
    s = (name or "").strip().lower()
    if not s:
        return ""
    s = re.sub(r"[\s_]+", "-", s)
    s = re.sub(r"[^a-z0-9._-]+", "", s)
    s = re.sub(r"-{2,}", "-", s)
    return s.strip(".-")


def repo_match_keys(name: str) -> set[str]:
    """Match keys for a folder / install_dir / github short name.

    Includes the normalized form and a form with a trailing ``-recomp`` /
    ``-recompiled`` stripped so local ``… Recomp`` checkouts still match
    catalog ``install_dir_name`` / github slugs that omit that suffix
    (e.g. Klonoa).
    """
    k = normalize_repo_key(name)
    if not k:
        return set()
    out = {k}
    stripped = re.sub(r"(-recomp(iled)?)+$", "", k).strip("-")
    if stripped:
        out.add(stripped)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--set", action="append", default=[], metavar="KEY=VALUE")
    ap.add_argument(
        "--set-file",
        action="append",
        default=[],
        metavar="KEY=PATH",
        help="Read VALUE from a file (for multiline CMake blocks)",
    )
    ap.add_argument(
        "--ci-placeholders",
        action="store_true",
        help="Also replace YOUR_ZIP_PREFIX / YOUR_GAME_TITLE / yourgame-release",
    )
    args = ap.parse_args()

    repl: dict[str, str] = {}
    for item in args.set:
        if "=" not in item:
            print(f"bad --set {item!r} (want KEY=VALUE)", file=sys.stderr)
            return 2
        k, v = item.split("=", 1)
        repl[k] = v
    for item in args.set_file:
        if "=" not in item:
            print(f"bad --set-file {item!r} (want KEY=PATH)", file=sys.stderr)
            return 2
        k, path = item.split("=", 1)
        repl[k] = Path(path).read_text(encoding="utf-8").rstrip("\n")

    text = Path(args.src).read_text(encoding="utf-8")
    for k, v in repl.items():
        text = text.replace(f"@{k}@", v)

    if args.ci_placeholders:
        zp = repl.get("ZIP_PREFIX", "game")
        title = repl.get("GAME_TITLE") or repl.get("WINDOW_TITLE") or "Game"
        # Collapse runs of whitespace; escape for YAML double-quoted release names
        # (template uses name: "YOUR_GAME_TITLE ${{ … }}").
        title = re.sub(r"\s+", " ", title.strip())
        title_yaml = (
            title.replace("\\", "\\\\")
            .replace('"', '\\"')
            .replace("\n", " ")
            .replace("\r", "")
        )
        text = text.replace("YOUR_ZIP_PREFIX", zp)
        text = text.replace("YOUR_GAME_TITLE", title_yaml)
        text = text.replace("yourgame-release", f"{zp}-release")

    Path(args.dst).parent.mkdir(parents=True, exist_ok=True)
    Path(args.dst).write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
