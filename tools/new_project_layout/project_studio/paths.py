"""Locate toolkit roots (templates, CI helpers) relative to this package."""

from __future__ import annotations

from pathlib import Path


def toolkit_dir() -> Path:
    """tools/new_project_layout/"""
    return Path(__file__).resolve().parent.parent


def templates_dir() -> Path:
    return toolkit_dir() / "templates"


def psxrecomp_root_from_toolkit() -> Path | None:
    """If this toolkit lives inside a psxrecomp checkout, return that root."""
    # …/psxrecomp/tools/new_project_layout/project_studio
    candidate = toolkit_dir().parent.parent
    if (candidate / "runtime" / "runtime.cmake").is_file():
        return candidate
    return None


def ci_setup_release_template(game_root: Path | None = None) -> Path | None:
    """Prefer game submodule copy; fall back to toolkit's parent psxrecomp."""
    if game_root is not None:
        p = game_root / "psxrecomp" / "docs" / "ci" / "templates" / "setup-release.yml"
        if p.is_file():
            return p
        p = game_root / "psxrecomp-v4" / "docs" / "ci" / "templates" / "setup-release.yml"
        if p.is_file():
            return p
    root = psxrecomp_root_from_toolkit()
    if root is None:
        return None
    p = root / "docs" / "ci" / "templates" / "setup-release.yml"
    return p if p.is_file() else None
