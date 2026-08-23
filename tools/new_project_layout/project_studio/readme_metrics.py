"""README download-badge + RetComM Launcher blocks for scaffold and migrate."""

from __future__ import annotations

import json
import re
import subprocess
from pathlib import Path

from fill_tokens import (
    GITHUB_ABOUT_DESCRIPTION,
    GITHUB_ABOUT_HOMEPAGE,
    sanitize_github_name,
)

METRICS_BEGIN = "<!-- retcomm-readme-metrics -->"
METRICS_END = "<!-- /retcomm-readme-metrics -->"
BOXART_BEGIN = "<!-- retcomm-readme-boxart -->"
BOXART_END = "<!-- /retcomm-readme-boxart -->"
LAUNCHER_BEGIN = "<!-- retcomm-readme-launcher -->"
LAUNCHER_END = "<!-- /retcomm-readme-launcher -->"
RAID_BEGIN = "<!-- retcomm-readme-raid -->"
RAID_END = "<!-- /retcomm-readme-raid -->"

BOXART_REL = "launcher_assets/img/boxart.png"

_METRICS_RE = re.compile(
    re.escape(METRICS_BEGIN) + r".*?" + re.escape(METRICS_END) + r"\n?",
    re.S,
)
_BOXART_RE = re.compile(
    re.escape(BOXART_BEGIN) + r".*?" + re.escape(BOXART_END) + r"\n?",
    re.S,
)
_LAUNCHER_RE = re.compile(
    re.escape(LAUNCHER_BEGIN) + r".*?" + re.escape(LAUNCHER_END) + r"\n?",
    re.S,
)
_RAID_RE = re.compile(
    re.escape(RAID_BEGIN) + r".*?" + re.escape(RAID_END) + r"\n?",
    re.S,
)
_LEGACY_LAUNCHER_RE = re.compile(
    r"^## RetComM Launcher\n.*?(?=^## |\Z)",
    re.M | re.S,
)
_LEGACY_RAID_RE = re.compile(
    r"(?:\n---\s*)?\n*<p align=\"center\">\s*\n\s*<sub><b>R\.A\.I\.D\..*\Z",
    re.S,
)
_REMOTE_RE = re.compile(
    r"github\.com[:/](?P<owner>[^/\s]+)/(?P<repo>[^/\s]+)",
    re.I,
)

DEFAULT_GITHUB_OWNER = "TechnicallyComputers"
LAUNCHER_REPO = "https://github.com/TechnicallyComputers/RetComM-Launcher"


def parse_github_remote(url: str) -> tuple[str, str] | None:
    url = (url or "").strip().rstrip("/")
    m = _REMOTE_RE.search(url)
    if not m:
        return None
    owner = m.group("owner")
    repo = m.group("repo")
    if repo.endswith(".git"):
        repo = repo[: -len(".git")]
    repo = repo.rstrip("/")
    if not owner or not repo:
        return None
    return owner, repo


def infer_github_slug(root: Path) -> tuple[str, str] | None:
    """Best-effort owner/repo from origin, then catalog homepage."""
    try:
        proc = subprocess.run(
            ["git", "-C", str(root), "remote", "get-url", "origin"],
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError:
        proc = None
    if proc is not None and proc.returncode == 0:
        parsed = parse_github_remote(proc.stdout.strip())
        if parsed:
            return parsed
    cat = root / "catalog_identity.json"
    if cat.is_file():
        try:
            data = json.loads(cat.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            data = {}
        if isinstance(data, dict):
            for key in ("homepage", "github", "repository", "url"):
                val = data.get(key)
                if isinstance(val, str):
                    parsed = parse_github_remote(val)
                    if parsed:
                        return parsed
    return None


def resolve_github_slug(
    root: Path,
    owner: str | None = None,
    repo: str | None = None,
) -> tuple[str, str]:
    o = sanitize_github_name(owner or "")
    r = sanitize_github_name(repo or "")
    if o and r:
        return o, r
    inferred = infer_github_slug(root)
    if inferred:
        return (
            o or sanitize_github_name(inferred[0]),
            r or sanitize_github_name(inferred[1]),
        )
    return (o or DEFAULT_GITHUB_OWNER, r or sanitize_github_name(root.name))


def apply_github_about(
    owner: str,
    repo: str,
    *,
    dry_run: bool = False,
) -> tuple[bool, str]:
    """Set GitHub About description + Discord homepage via ``gh repo edit``."""
    slug = f"{sanitize_github_name(owner) or DEFAULT_GITHUB_OWNER}/{sanitize_github_name(repo) or 'repo'}"
    cmd = [
        "gh",
        "repo",
        "edit",
        slug,
        "--description",
        GITHUB_ABOUT_DESCRIPTION,
        "--homepage",
        GITHUB_ABOUT_HOMEPAGE,
    ]
    if dry_run:
        return True, "dry-run: " + " ".join(cmd)
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    except OSError as exc:
        return False, f"gh not available ({exc})"
    if proc.returncode != 0:
        err = (proc.stderr or proc.stdout or "").strip()
        return False, err or f"gh repo edit exit {proc.returncode}"
    return True, f"GitHub About set on {slug}"


def render_metrics_block(owner: str, repo: str, zip_prefix: str = "") -> str:
    # Per-OS latest badges need an exact asset name. Title zips are
    # `{prefix}-{version}-{platform}.zip`, so globs always show 0 on shields.io.
    # Keep all-time / latest totals + version; zip_prefix is unused here.
    del zip_prefix
    owner = sanitize_github_name(owner) or DEFAULT_GITHUB_OWNER
    repo = sanitize_github_name(repo) or "repo"
    base = f"https://github.com/{owner}/{repo}"
    img = "https://img.shields.io/github"
    lines = [
        METRICS_BEGIN,
        f"[![GitHub downloads (all assets, all releases)]({img}/downloads/{owner}/{repo}/total)]({base}/releases)",
        f"[![GitHub downloads (latest release)]({img}/downloads/{owner}/{repo}/latest/total)]({base}/releases/latest)",
        f"[![GitHub release]({img}/v/release/{owner}/{repo})]({base}/releases/latest)",
        METRICS_END,
    ]
    return "\n".join(lines)


def render_boxart_block(game_name: str) -> str:
    alt = (game_name or "Game").strip() or "Game"
    alt = alt.replace('"', "'")
    return "\n".join(
        [
            BOXART_BEGIN,
            '<p align="center">',
            f'  <img src="{BOXART_REL}" alt="{alt} box art" width="280">',
            "</p>",
            BOXART_END,
        ]
    )


def render_launcher_block() -> str:
    shots = (
        "https://raw.githubusercontent.com/TechnicallyComputers/"
        "RetComM-Launcher/main/docs/screenshots"
    )
    return "\n".join(
        [
            LAUNCHER_BEGIN,
            "## RetComM Launcher",
            "",
            "You can run this title **standalone** (release zip + the built-in recomp-ui",
            "Generate & Build flow), or manage installs, updates, ROM/BIOS wiring, and queued",
            "builds more intuitively with",
            f"**[RetComM Launcher]({LAUNCHER_REPO})** —",
            "the Retro Compilation Manager hub for self-compiling recomps.",
            "",
            f"[Downloads]({LAUNCHER_REPO}/releases) ·",
            f"[Full README & features]({LAUNCHER_REPO}#readme)",
            "",
            "<p align=\"center\">",
            f'  <img src="{shots}/hub-and-game-launcher.png" alt="RetComM hub with a background build, next to a title’s recomp-ui launcher" width="720">',
            "</p>",
            "",
            "<p align=\"center\">",
            f'  <img src="{shots}/queue-and-background-build.png" alt="Background cmake build with titles queued" width="720">',
            "</p>",
            "",
            "RetComM checks for updates, rebuilds with existing build data when possible,",
            "shares the portable toolchain used by per-title launchers, and automates",
            "BIOS/ROM/save plumbing so you are not stuck repeating each game’s wizard by hand.",
            LAUNCHER_END,
        ]
    )


def render_raid_block() -> str:
    return "\n".join(
        [
            RAID_BEGIN,
            "---",
            "",
            "<p align=\"center\">",
            "  <sub><b>R.A.I.D. — Retro AI Development</b> · a Discord for AI-assisted retro reverse-engineering, decomp &amp; recomp</sub>",
            "</p>",
            "",
            "<p align=\"center\">",
            '  <a href="https://discord.gg/Ad9BwSzctP"><img src=".github/raid-discord.png" alt="Join the Retro AI Development (R.A.I.D.) Discord" width="200"></a>',
            "</p>",
            RAID_END,
        ]
    )


def readme_has_metrics(text: str) -> bool:
    return "img.shields.io/github/downloads/" in text


def readme_has_boxart(text: str) -> bool:
    return BOXART_BEGIN in text or BOXART_REL in text


def boxart_png_present(root: Path) -> bool:
    return (root / BOXART_REL).is_file()


def readme_has_launcher(text: str) -> bool:
    return "TechnicallyComputers/RetComM-Launcher" in text


def readme_has_raid(text: str) -> bool:
    return "discord.gg/Ad9BwSzctP" in text or "R.A.I.D. — Retro AI Development" in text


def _apply_boxart(text: str, boxart_block: str) -> str:
    boxart_block = boxart_block.strip() + "\n"
    if _BOXART_RE.search(text):
        return _BOXART_RE.sub(boxart_block, text, count=1)
    m = _METRICS_RE.search(text)
    if m:
        rest = text[m.end() :].lstrip("\n")
        return text[: m.end()] + "\n" + boxart_block + "\n" + rest
    hm = re.match(r"(#[^\n]+\n)", text)
    if hm:
        rest = text[hm.end() :].lstrip("\n")
        return text[: hm.end()] + "\n" + boxart_block + "\n" + rest
    return boxart_block + "\n" + text


def upsert_boxart_block(text: str, game_name: str) -> str:
    if not text.endswith("\n"):
        text += "\n"
    return _apply_boxart(text, render_boxart_block(game_name))


def inject_readme_boxart(readme: Path, game_name: str) -> bool:
    """Insert/replace the README boxart block if ``boxart.png`` exists."""
    readme = Path(readme)
    if not readme.is_file() or not boxart_png_present(readme.parent):
        return False
    old = readme.read_text(encoding="utf-8", errors="replace")
    new = upsert_boxart_block(old, game_name)
    if new == old:
        return False
    readme.write_text(new, encoding="utf-8")
    return True


def upsert_readme_blocks(
    text: str,
    metrics: str,
    launcher: str,
    raid: str | None = None,
    boxart: str | None = None,
) -> str:
    """Insert or replace marked metrics, boxart, launcher, and RAID footer; keep custom body."""
    if not text.endswith("\n"):
        text += "\n"
    metrics_block = metrics.strip() + "\n"
    launcher_block = launcher.strip() + "\n"
    raid_block = (raid if raid is not None else render_raid_block()).strip() + "\n"

    if _METRICS_RE.search(text):
        text = _METRICS_RE.sub(metrics_block, text, count=1)
    elif not readme_has_metrics(text):
        m = re.match(r"(#[^\n]+\n)", text)
        if m:
            text = (
                text[: m.end()]
                + "\n"
                + metrics_block
                + "\n"
                + text[m.end() :].lstrip("\n")
            )
        else:
            text = metrics_block + "\n" + text

    if boxart:
        text = _apply_boxart(text, boxart)

    if _LAUNCHER_RE.search(text):
        text = _LAUNCHER_RE.sub(launcher_block, text, count=1)
    elif _LEGACY_LAUNCHER_RE.search(text):
        text = _LEGACY_LAUNCHER_RE.sub(launcher_block + "\n", text, count=1)
    elif not readme_has_launcher(text):
        insert_at = None
        for heading in ("## Legal", "## Quick start", "## Layout", "## Symbols"):
            idx = text.find("\n" + heading)
            if idx >= 0:
                insert_at = idx + 1
                break
        if insert_at is None:
            text = text.rstrip() + "\n\n" + launcher_block
        else:
            text = text[:insert_at] + launcher_block + "\n" + text[insert_at:]

    if _RAID_RE.search(text):
        text = _RAID_RE.sub(raid_block, text, count=1)
    elif _LEGACY_RAID_RE.search(text):
        text = _LEGACY_RAID_RE.sub("\n" + raid_block, text, count=1)
    elif not readme_has_raid(text):
        text = text.rstrip() + "\n\n" + raid_block
    return text if text.endswith("\n") else text + "\n"
