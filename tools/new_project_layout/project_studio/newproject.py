"""New-project wizard: drive setup_project.sh / .ps1 non-interactively."""

from __future__ import annotations

import os
import platform
import shutil
import subprocess
import threading
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from .gitops import CmdResult, switch_modules
from .paths import toolkit_dir


@dataclass
class NewProjectOptions:
    """Mirrors setup_project.sh / setup_project.ps1 CLI flags."""

    name: str = ""
    disc: str = ""
    parent_dir: str = ""
    bios: str = ""
    boot_exe: str = ""
    players: int = 2
    zip_prefix: str = ""
    github_owner: str = ""
    github_repo: str = ""
    description: str = ""
    publisher: str = ""
    year: str = ""
    region: str = "USA"

    enable_recomp_ui: bool = True
    enable_wizard: bool = True
    # GUI defaults: all on except GitHub; netplay follows players (≥2).
    enable_netplay: bool = False
    lobby_url: str = "netplay.retcomm.net"
    enable_ci: bool = True
    fetch_boxart: bool = True
    stage_disc: bool = True
    do_generate: bool = True
    do_build: bool = True
    create_github: bool = False
    github_visibility: str = "private"  # public | private | internal

    psxrecomp_ref: str = "master"
    recomp_ui_ref: str = "master"
    recomp_net_ref: str = ""  # empty = keep psxrecomp pin
    rbengine_ref: str = ""  # post-setup switch only (script has no flag)

    dry_run: bool = False


def setup_script_paths() -> tuple[Path, Path]:
    """Return (setup_project.sh, setup_project.ps1) under the toolkit."""
    base = toolkit_dir()
    return base / "setup_project.sh", base / "setup_project.ps1"


def is_windows() -> bool:
    return platform.system().lower().startswith("win") or os.name == "nt"


def project_folder_name(opts: NewProjectOptions) -> str:
    """Checkout folder = GitHub/catalog install_dir slug (not display name with spaces)."""
    from fill_tokens import install_dir_name, sanitize_github_name

    repo = (opts.github_repo or "").strip()
    if repo:
        return sanitize_github_name(repo)
    return install_dir_name(opts.name or "")


def project_root_for(opts: NewProjectOptions) -> Path:
    parent = Path(opts.parent_dir or ".").expanduser()
    if not parent.is_absolute():
        parent = parent.resolve()
    else:
        parent = parent.resolve()
    folder = project_folder_name(opts)
    if not folder:
        folder = (opts.name or "").strip() or "repo"
    return (parent / folder).resolve()


def validate_options(opts: NewProjectOptions) -> list[str]:
    errs: list[str] = []
    name = (opts.name or "").strip()
    disc = (opts.disc or "").strip()
    if not name:
        errs.append("Project name is required")
    if not disc:
        errs.append("Disc .cue path is required")
    else:
        p = Path(disc).expanduser()
        if not p.is_file():
            errs.append(f"Disc not found: {disc}")
    if opts.bios:
        bp = Path(opts.bios).expanduser()
        if not bp.is_file():
            errs.append(f"BIOS not found: {opts.bios}")
    if opts.players < 1 or opts.players > 8:
        errs.append("Players must be 1–8")
    if opts.do_build and not opts.do_generate:
        errs.append("Build requires Generate")
    # Wizard/netplay without UI (and 1P netplay) are auto-corrected at run time.
    vis = (opts.github_visibility or "private").strip().lower()
    if vis not in ("public", "private", "internal"):
        errs.append("GitHub visibility must be public/private/internal")
    dest = project_root_for(opts)
    if dest.exists():
        if dest.is_file() or (dest.is_dir() and any(dest.iterdir())):
            errs.append(f"Destination already exists: {dest}")
    return errs


def build_command(opts: NewProjectOptions) -> tuple[list[str], dict[str, str]]:
    """Build argv + env for the OS-appropriate setup script.

    Always passes ``--yes`` / ``-Yes`` so the GUI/CLI supply every choice.
    """
    sh, ps1 = setup_script_paths()
    env = os.environ.copy()
    env["PSXRECOMP_SETUP_YES"] = "1"
    if (opts.recomp_net_ref or "").strip():
        env["RECOMP_NET_REF"] = opts.recomp_net_ref.strip()

    if is_windows():
        if not ps1.is_file():
            raise FileNotFoundError(f"Missing setup script: {ps1}")
        powershell = (
            shutil.which("pwsh")
            or shutil.which("powershell")
            or "powershell"
        )
        cmd: list[str] = [
            powershell,
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(ps1),
            "-Disc",
            str(Path(opts.disc).expanduser().resolve()),
            "-Name",
            opts.name.strip(),
            "-Yes",
            "-Players",
            str(int(opts.players)),
            "-PsxrecompRef",
            (opts.psxrecomp_ref or "master").strip(),
            "-RecompUiRef",
            (opts.recomp_ui_ref or "master").strip(),
            "-GithubVisibility",
            (opts.github_visibility or "private").strip().lower(),
        ]
        parent = (opts.parent_dir or ".").strip() or "."
        cmd.extend(["-Dir", str(Path(parent).expanduser().resolve())])
        if opts.bios:
            cmd.extend(["-Bios", str(Path(opts.bios).expanduser().resolve())])
        if opts.boot_exe:
            cmd.extend(["-BootExe", opts.boot_exe.strip()])
        if opts.zip_prefix:
            cmd.extend(["-ZipPrefix", opts.zip_prefix.strip()])
        if opts.github_owner:
            cmd.extend(["-GithubOwner", opts.github_owner.strip()])
        if opts.github_repo:
            cmd.extend(["-GithubRepo", opts.github_repo.strip()])
        if opts.description:
            cmd.extend(["-Description", opts.description.strip()])
        if opts.publisher:
            cmd.extend(["-Publisher", opts.publisher.strip()])
        if opts.year:
            cmd.extend(["-Year", opts.year.strip()])
        if opts.region:
            cmd.extend(["-Region", opts.region.strip()])
        if opts.lobby_url:
            cmd.extend(["-LobbyUrl", opts.lobby_url.strip()])

        def sw(yes: bool, on: str, off: str) -> None:
            cmd.append(on if yes else off)

        sw(opts.enable_recomp_ui, "-EnableRecompUi", "-NoRecompUi")
        sw(opts.enable_wizard, "-EnableWizard", "-NoWizard")
        sw(opts.enable_netplay, "-EnableNetplay", "-NoNetplay")
        sw(opts.enable_ci, "-EnableCi", "-NoCi")
        sw(opts.fetch_boxart, "-FetchBoxart", "-NoFetchBoxart")
        sw(opts.do_generate, "-Generate", "-NoGenerate")
        sw(opts.do_build, "-EnableBuild", "-NoBuild")
        sw(opts.create_github, "-CreateGithub", "-NoGithub")
        if opts.stage_disc:
            cmd.append("-StageDisc")
        return cmd, env

    if not sh.is_file():
        raise FileNotFoundError(f"Missing setup script: {sh}")
    cmd = [
        "sh",
        str(sh),
        "--yes",
        "--disc",
        str(Path(opts.disc).expanduser().resolve()),
        "--name",
        opts.name.strip(),
        "--players",
        str(int(opts.players)),
        "--psxrecomp-ref",
        (opts.psxrecomp_ref or "master").strip(),
        "--recomp-ui-ref",
        (opts.recomp_ui_ref or "master").strip(),
        "--github-visibility",
        (opts.github_visibility or "private").strip().lower(),
    ]
    parent = (opts.parent_dir or ".").strip() or "."
    cmd.extend(["--dir", str(Path(parent).expanduser().resolve())])
    if opts.bios:
        cmd.extend(["--bios", str(Path(opts.bios).expanduser().resolve())])
    if opts.boot_exe:
        cmd.extend(["--boot-exe", opts.boot_exe.strip()])
    if opts.zip_prefix:
        cmd.extend(["--zip-prefix", opts.zip_prefix.strip()])
    if opts.github_owner:
        cmd.extend(["--github-owner", opts.github_owner.strip()])
    if opts.github_repo:
        cmd.extend(["--github-repo", opts.github_repo.strip()])
    if opts.description:
        cmd.extend(["--description", opts.description.strip()])
    if opts.publisher:
        cmd.extend(["--publisher", opts.publisher.strip()])
    if opts.year:
        cmd.extend(["--year", opts.year.strip()])
    if opts.region:
        cmd.extend(["--region", opts.region.strip()])
    if opts.lobby_url:
        cmd.extend(["--lobby-url", opts.lobby_url.strip()])
    if (opts.recomp_net_ref or "").strip():
        cmd.extend(["--recomp-net-ref", opts.recomp_net_ref.strip()])

    def flag(yes: bool, on: str, off: str) -> None:
        cmd.append(on if yes else off)

    flag(opts.enable_recomp_ui, "--enable-recomp-ui", "--no-recomp-ui")
    flag(opts.enable_wizard, "--enable-wizard", "--no-wizard")
    flag(opts.enable_netplay, "--enable-netplay", "--no-netplay")
    flag(opts.enable_ci, "--enable-ci", "--no-ci")
    flag(opts.fetch_boxart, "--fetch-boxart", "--no-fetch-boxart")
    flag(opts.do_generate, "--generate", "--no-generate")
    flag(opts.do_build, "--enable-build", "--no-build")
    flag(opts.create_github, "--create-github", "--no-github")
    cmd.append("--stage-disc" if opts.stage_disc else "--no-stage-disc")
    return cmd, env


def run_new_project(
    opts: NewProjectOptions,
    *,
    on_line: Callable[[str], None] | None = None,
    cancel_event: threading.Event | None = None,
) -> CmdResult:
    """Run setup_project end-to-end; stream lines via ``on_line``."""
    errs = validate_options(opts)
    if errs:
        return CmdResult(False, "Invalid new-project options", "\n".join(errs))

    # Match script policy: no UI ⇒ no wizard/netplay; 1P ⇒ no netplay.
    if not opts.enable_recomp_ui:
        opts.enable_wizard = False
        opts.enable_netplay = False
    if opts.players < 2:
        opts.enable_netplay = False

    try:
        cmd, env = build_command(opts)
    except FileNotFoundError as exc:
        return CmdResult(False, str(exc))

    if opts.dry_run:
        preview = " ".join(cmd)
        if on_line:
            on_line(f"[dry-run] {preview}")
        return CmdResult(True, "dry-run: would run setup_project", preview)

    if on_line:
        on_line(f"$ {' '.join(cmd)}")

    try:
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=env,
            cwd=str(Path(opts.parent_dir or ".").expanduser().resolve()),
        )
    except OSError as exc:
        return CmdResult(False, f"Failed to start setup: {exc}")

    assert proc.stdout is not None
    for line in proc.stdout:
        if cancel_event is not None and cancel_event.is_set():
            proc.terminate()
            return CmdResult(False, "New project cancelled")
        text = line.rstrip("\n")
        if on_line:
            on_line(text)
    code = proc.wait()
    root = project_root_for(opts)
    if code != 0:
        return CmdResult(
            False,
            f"setup_project failed (exit {code})",
            str(root),
        )

    # Optional nested rbengine (and net on Windows, where script has no ref flag)
    post_notes: list[str] = []
    if root.is_dir():
        nested_branches: dict[str, str] = {}
        net = (opts.recomp_net_ref or "").strip()
        rb = (opts.rbengine_ref or "").strip()
        if net and is_windows():
            nested_branches["lib/recomp-net"] = net
        if rb:
            nested_branches["lib/retcomm-rbengine"] = rb
        if nested_branches:
            if on_line:
                on_line("== Post-setup nested lib branch switch ==")
            for r in switch_modules(
                root,
                nested=True,
                branch_by_path=nested_branches,
                paths=list(nested_branches.keys()),
                set_tracking=True,
                dry_run=False,
            ):
                post_notes.append(r.message)
                if on_line:
                    on_line(f"  [{'OK' if r.ok else 'FAIL'}] {r.message}")

    detail = str(root)
    if post_notes:
        detail += "\n" + "\n".join(post_notes)
    return CmdResult(True, f"Created project at {root}", detail)


def index_new_project(
    root: Path,
    *,
    name: str = "",
    cue: str = "",
) -> CmdResult:
    """Add/update the Studio repo index for a freshly created project."""
    from .bulkops import (
        catalog_title_id_for_root,
        upsert_studio_toml_title,
    )
    from .repo_index import add_repo, load_index

    if not root.is_dir():
        return CmdResult(False, f"Project root missing: {root}")
    idx = load_index()
    entry = add_repo(idx, root, name=name or root.name, cue=cue)
    notes: list[str] = [entry.path]
    tid = catalog_title_id_for_root(root)
    if tid:
        note = upsert_studio_toml_title(tid, root)
        if note:
            notes.append(note)
    return CmdResult(True, f"Indexed {entry.label()}", "\n".join(notes))
