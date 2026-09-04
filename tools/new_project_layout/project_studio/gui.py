"""CustomTkinter GUI for Project Studio.

CLI audit/plan/apply stay stdlib-only. The GUI auto-bootstraps a local
``.venv`` and installs ``requirements-gui.txt`` on first launch.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox

_LOG_OK = "#3dd68c"
_LOG_WARN = "#e6b450"
_LOG_ERROR = "#f07178"
_LOG_INFO = "#8a9199"
_DEFAULT_LOG_HEIGHT = 160

from .detect import audit_project
from .models import CheckStatus, MigrateOptions
from .ops import apply_plan
from .plan import build_plan

_STATUS_COLORS = {
    CheckStatus.PASS: "#3dd68c",
    CheckStatus.FAIL: "#f07178",
    CheckStatus.WARN: "#e6b450",
    CheckStatus.SKIP: "#8a9199",
}

_TOOLKIT = Path(__file__).resolve().parent.parent
_VENV_DIR = _TOOLKIT / ".venv"
_REQS = _TOOLKIT / "requirements-gui.txt"
_STAMP = _VENV_DIR / ".project_studio_gui_deps"
_BOOTSTRAP_ENV = "PROJECT_STUDIO_GUI_BOOTSTRAPPED"


def _is_frozen() -> bool:
    return bool(getattr(sys, "frozen", False)) or os.environ.get(
        "RETCOMM_STUDIO_FROZEN", ""
    ).strip() in ("1", "true", "yes")


def _toolkit_root() -> Path:
    from .paths import toolkit_dir

    return toolkit_dir()


def _pick_directory(*, title: str, parent=None, initialdir: str | None = None) -> str:
    """Native directory picker when available; Tk fallback otherwise."""
    start = initialdir or os.path.expanduser("~")
    if sys.platform == "darwin":
        picked = _macos_pick(directory=True, title=title, initial=start)
        if picked is not None:
            return picked
    elif sys.platform != "win32":
        picked = _linux_pick(directory=True, title=title, initial=start, filetypes=None)
        if picked is not None:
            return picked
    # Windows (and fallbacks): Tk uses the OS common dialog.
    return filedialog.askdirectory(parent=parent, title=title, initialdir=start) or ""


def _pick_open_file(
    *,
    title: str,
    parent=None,
    initialdir: str | None = None,
    filetypes: list[tuple[str, str]] | None = None,
) -> str:
    """Native open-file picker when available; Tk fallback otherwise."""
    start = initialdir or os.path.expanduser("~")
    ftypes = filetypes or [("All", "*.*")]
    if sys.platform == "darwin":
        picked = _macos_pick(
            directory=False, title=title, initial=start, filetypes=ftypes
        )
        if picked is not None:
            return picked
    elif sys.platform != "win32":
        picked = _linux_pick(
            directory=False, title=title, initial=start, filetypes=ftypes
        )
        if picked is not None:
            return picked
    return (
        filedialog.askopenfilename(
            parent=parent, title=title, initialdir=start, filetypes=ftypes
        )
        or ""
    )


def _linux_pick(
    *,
    directory: bool,
    title: str,
    initial: str,
    filetypes: list[tuple[str, str]] | None,
) -> str | None:
    """Prefer Zenity/KDialog (real desktop dialogs). None → caller should fall back."""
    desk = (
        os.environ.get("XDG_CURRENT_DESKTOP")
        or os.environ.get("DESKTOP_SESSION")
        or ""
    ).lower()
    prefer_kde = "kde" in desk or "plasma" in desk
    order = ("kdialog", "zenity") if prefer_kde else ("zenity", "kdialog")

    for tool in order:
        if not shutil.which(tool):
            continue
        try:
            if tool == "zenity":
                cmd = [
                    "zenity",
                    "--file-selection",
                    f"--title={title}",
                    f"--filename={initial.rstrip('/')}/",
                ]
                if directory:
                    cmd.append("--directory")
                else:
                    for label, pattern in filetypes or []:
                        cmd.append(f"--file-filter={label} | {pattern}")
            else:
                cmd = ["kdialog", "--title", title]
                if directory:
                    cmd.extend(["--getexistingdirectory", initial])
                else:
                    # kdialog filter: "Cue sheet (*.cue)|*.cue\nAll (*)|*"
                    filt = "\n".join(
                        f"{label} ({pattern})|{pattern}"
                        for label, pattern in (filetypes or [("All", "*")])
                    )
                    cmd.extend(["--getopenfilename", initial, filt])
            r = subprocess.run(cmd, capture_output=True, text=True, check=False)
        except OSError:
            continue
        if r.returncode == 0:
            return (r.stdout or "").strip()
        if r.returncode in (1, 5):  # cancel / no input
            return ""
        # Other codes: try next tool / fall back
    return None


def _macos_pick(
    *,
    directory: bool,
    title: str,
    initial: str,
    filetypes: list[tuple[str, str]] | None = None,
) -> str | None:
    """NSOpenPanel via osascript. None → Tk fallback."""
    if not shutil.which("osascript"):
        return None
    # Escape for AppleScript strings
    def esc(s: str) -> str:
        return s.replace("\\", "\\\\").replace('"', '\\"')

    choose = "choose folder" if directory else "choose file"
    props = [f'with prompt "{esc(title)}"']
    if initial and Path(initial).is_dir():
        props.append(f'default location POSIX file "{esc(initial)}"')
    if not directory and filetypes:
        exts: list[str] = []
        for _, pattern in filetypes:
            for part in pattern.replace(";", " ").split():
                part = part.strip()
                if part.startswith("*.") and part != "*.*":
                    exts.append(part[2:])
        if exts:
            listed = ", ".join(f'"{e}"' for e in dict.fromkeys(exts))
            props.append(f"of type {{{listed}}}")
    script = f'set p to {choose} {" ".join(props)}\nPOSIX path of p'
    try:
        r = subprocess.run(
            ["osascript", "-e", script],
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError:
        return None
    if r.returncode == 0:
        return (r.stdout or "").strip().rstrip("/")
    if r.returncode == 1:  # user cancel
        return ""
    return None


def run_gui(*, initial_root: Path | None = None) -> int:
    err = _ensure_gui_deps()
    if err is not None:
        print(err, file=sys.stderr)
        return 2

    try:
        import customtkinter as ctk
    except ImportError:
        print(
            "Project Studio GUI still cannot import customtkinter after bootstrap.\n"
            "CLI still works: migrate_project.py audit|plan|apply",
            file=sys.stderr,
        )
        return 2

    ctk.set_appearance_mode("dark")
    ctk.set_default_color_theme("dark-blue")
    app = ProjectStudioApp(ctk, initial_root=initial_root)
    app.mainloop()
    return 0


def _venv_python(venv_dir: Path = _VENV_DIR) -> Path:
    if os.name == "nt":
        return venv_dir / "Scripts" / "python.exe"
    return venv_dir / "bin" / "python"


def _reqs_stamp() -> str:
    reqs = _toolkit_root() / "requirements-gui.txt"
    raw = reqs.read_bytes() if reqs.is_file() else b"customtkinter>=5.2\n"
    return hashlib.sha256(raw).hexdigest()


def _running_in_venv(venv_dir: Path = _VENV_DIR) -> bool:
    """True when this process is the managed GUI venv (not the system python).

    Do not compare ``sys.executable.resolve()`` to the venv launcher — on Linux
    the venv ``bin/python`` often symlinks to the system interpreter, so
    resolve() falsely reports they are the same.
    """
    try:
        return Path(sys.prefix).resolve() == venv_dir.resolve()
    except OSError:
        return False


def _ctk_importable() -> bool:
    try:
        import customtkinter  # noqa: F401

        return True
    except ImportError:
        return False


def _venv_can_import_ctk(venv_py: Path) -> bool:
    try:
        r = subprocess.run(
            [str(venv_py), "-c", "import customtkinter"],
            check=False,
            capture_output=True,
            text=True,
        )
        return r.returncode == 0
    except OSError:
        return False


def _run(cmd: list[str], *, label: str) -> str | None:
    try:
        subprocess.run(cmd, check=True)
        return None
    except subprocess.CalledProcessError as e:
        return (
            f"Project Studio GUI bootstrap failed ({label}, exit {e.returncode}).\n"
            f"  cmd: {' '.join(cmd)}\n"
            "CLI still works: migrate_project.py audit|plan|apply"
        )
    except OSError as e:
        return (
            f"Project Studio GUI bootstrap failed ({label}): {e}\n"
            "CLI still works: migrate_project.py audit|plan|apply"
        )


def _ensure_gui_deps() -> str | None:
    """Create/fill ``.venv`` if needed, then re-exec into it.

    Returns an error message on failure, or None when customtkinter is ready
    in the current interpreter (possibly after re-exec).
    """
    if _is_frozen():
        if _ctk_importable():
            return None
        return (
            "Frozen RetComM Studio build is missing customtkinter.\n"
            "Reinstall from a release package, or run from source."
        )

    toolkit = _toolkit_root()
    venv_dir = toolkit / ".venv"
    reqs = toolkit / "requirements-gui.txt"
    stamp_path = venv_dir / ".project_studio_gui_deps"

    in_venv = _running_in_venv(venv_dir)
    if _ctk_importable():
        # Refresh managed venv when the stamp drifts and we are already in it.
        if in_venv:
            if stamp_path.is_file() and stamp_path.read_text(encoding="utf-8").strip() == _reqs_stamp():
                return None
        else:
            return None

    if not reqs.is_file():
        return (
            f"Missing {reqs} — cannot bootstrap GUI deps.\n"
            "CLI still works: migrate_project.py audit|plan|apply"
        )

    venv_py = _venv_python(venv_dir)
    stamp = _reqs_stamp()
    need_venv = not venv_py.is_file()
    stamp_ok = stamp_path.is_file() and stamp_path.read_text(encoding="utf-8").strip() == stamp
    need_install = need_venv or not stamp_ok or not _venv_can_import_ctk(venv_py)

    if need_venv:
        print(
            "Project Studio GUI: creating local .venv (first run)…",
            file=sys.stderr,
        )
        err = _run([sys.executable, "-m", "venv", str(venv_dir)], label="python -m venv")
        if err:
            return err
        venv_py = _venv_python(venv_dir)
        if not venv_py.is_file():
            return (
                f"venv created but interpreter missing: {venv_py}\n"
                "CLI still works: migrate_project.py audit|plan|apply"
            )

    if need_install:
        print(
            "Project Studio GUI: installing deps from requirements-gui.txt…",
            file=sys.stderr,
        )
        err = _run(
            [str(venv_py), "-m", "pip", "install", "-r", str(reqs)],
            label="pip install",
        )
        if err:
            return err
        stamp_path.parent.mkdir(parents=True, exist_ok=True)
        stamp_path.write_text(stamp + "\n", encoding="utf-8")

    if _running_in_venv(venv_dir):
        if _ctk_importable():
            return None
        return (
            "Installed into .venv but customtkinter still missing.\n"
            "CLI still works: migrate_project.py audit|plan|apply"
        )

    if os.environ.get(_BOOTSTRAP_ENV) == "1":
        return (
            "Re-exec into .venv did not pick up customtkinter.\n"
            "CLI still works: migrate_project.py audit|plan|apply"
        )

    os.environ[_BOOTSTRAP_ENV] = "1"
    os.execv(str(venv_py), [str(venv_py), *sys.argv])
    return "exec failed"  # pragma: no cover


class ProjectStudioApp:
    def __init__(self, ctk, *, initial_root: Path | None = None) -> None:
        self.ctk = ctk
        self.root = ctk.CTk()
        self.root.title("RetComM Studio")
        self.root.geometry("1100x820")
        self.root.minsize(900, 640)
        # Maximize after the first map so WMs honor zoomed/fullscreen hints.
        self.root.after(0, self._maximize_window)
        self.root.after(50, self._apply_app_icon)

        self.root_var = tk.StringVar(value=str(initial_root) if initial_root else "")
        self.repo_label_var = tk.StringVar(value="(add a repo…)")
        self.disc_var = tk.StringVar()
        self.players_var = tk.StringVar(value="2")
        self.zip_var = tk.StringVar()
        self.netplay_var = tk.BooleanVar(value=False)
        self.ci_var = tk.BooleanVar(value=True)
        self.probe_var = tk.BooleanVar(value=False)
        self.dry_run_var = tk.BooleanVar(value=True)
        self.force_var = tk.BooleanVar(value=False)
        self.catalog_only_var = tk.BooleanVar(value=False)
        self.status_var = tk.StringVar(value="Open a game repo and Audit.")
        self._repo_menu_entries: list = []
        self._netplay_detected = False

        self.git_branch_var = tk.StringVar()
        self.git_psx_branch_var = tk.StringVar(value="master")
        self.git_ui_branch_var = tk.StringVar(value="master")
        self.git_net_branch_var = tk.StringVar(value="main")
        self.git_rb_branch_var = tk.StringVar(value="main")
        self.git_msg_var = tk.StringVar()
        self.git_sub_msg_var = tk.StringVar(value="chore: update submodule")
        self.git_nested_msg_var = tk.StringVar(
            value="chore: bump recomp-net + retcomm-rbengine"
        )
        self.git_libs_msg_var = tk.StringVar(value="chore: update nested lib")
        self.git_remote_update_var = tk.BooleanVar(value=False)
        self.git_create_branch_var = tk.BooleanVar(value=False)
        self.git_pull_mode_var = tk.StringVar(value="ff-only")
        self.git_pull_dirty_var = tk.StringVar(value="fail")
        self.bulk_tgt_game_var = tk.BooleanVar(value=True)
        self.bulk_tgt_modules_var = tk.BooleanVar(value=False)
        self.bulk_tgt_psx_var = tk.BooleanVar(value=False)
        self.bulk_tgt_nested_var = tk.BooleanVar(value=False)
        self.bulk_msg_var = tk.StringVar(value="chore: sync")
        self.bulk_set_tracking_var = tk.BooleanVar(value=True)
        self.bulk_jobs_var = tk.StringVar(value="2")
        self._bulk_busy = False
        # Dedicated Bulk branch picks (do not share Git-tab vars — refresh
        # there used to overwrite bulk switch selections).
        self.bulk_game_branch_var = tk.StringVar(value="(default)")
        self.bulk_psx_branch_var = tk.StringVar(value="(default)")
        self.bulk_ui_branch_var = tk.StringVar(value="(default)")
        self.bulk_net_branch_var = tk.StringVar(value="(default)")
        self.bulk_rb_branch_var = tk.StringVar(value="(default)")
        self.bulk_branch_status_var = tk.StringVar(value="")
        self._bulk_branch_fetch_busy = False
        self._bulk_vars: dict[str, tk.BooleanVar] = {}
        self._newproj_busy = False
        self.np_name_var = tk.StringVar()
        self.np_parent_var = tk.StringVar(
            value=str(Path.home() / "Documents" / "GitHub")
        )
        self.np_disc_var = tk.StringVar()
        self.np_bios_var = tk.StringVar()
        self.np_players_var = tk.StringVar(value="2")
        self.np_zip_var = tk.StringVar()
        self.np_desc_var = tk.StringVar()
        self.np_publisher_var = tk.StringVar()
        self.np_year_var = tk.StringVar()
        self.np_region_var = tk.StringVar(value="USA")
        self.np_lobby_var = tk.StringVar(value="netplay.retcomm.net")
        self.np_ui_var = tk.BooleanVar(value=True)
        self.np_wizard_var = tk.BooleanVar(value=True)
        # Netplay follows players (≥2 on); GitHub stays off. All other flags on.
        self.np_netplay_var = tk.BooleanVar(value=True)  # players default = 2
        self.np_ci_var = tk.BooleanVar(value=True)
        self.np_boxart_var = tk.BooleanVar(value=True)
        # Off by default — see NewProjectOptions.stage_disc. The project reads
        # the dump from where the user keeps it instead of duplicating it.
        self.np_stage_var = tk.BooleanVar(value=False)
        self.np_generate_var = tk.BooleanVar(value=True)
        self.np_build_var = tk.BooleanVar(value=True)
        self.np_github_var = tk.BooleanVar(value=False)
        self.np_gh_vis_var = tk.StringVar(value="private")
        self.np_psx_ref_var = tk.StringVar(value="master")
        self.np_ui_ref_var = tk.StringVar(value="master")
        self.np_net_ref_var = tk.StringVar(value="(default)")
        self.np_rb_ref_var = tk.StringVar(value="(default)")
        self.np_status_var = tk.StringVar(
            value="Fill disc + name, then Create project."
        )
        self.np_branch_status_var = tk.StringVar(value="")
        self._np_autofill_busy = False
        self._np_branch_refresh_busy = False
        self.release_version_var = tk.StringVar()
        self.release_bump_var = tk.StringVar(value="patch")
        self.release_publish_var = tk.BooleanVar(value=True)
        self.release_reuse_var = tk.BooleanVar(value=True)

        self.build_dir_var = tk.StringVar(value="build-release")
        self.build_type_var = tk.StringVar(value="Release")
        self.build_target_var = tk.StringVar(value="psx-runtime")
        self.build_generator_var = tk.StringVar(value="")
        self.build_jobs_var = tk.StringVar(value="")
        self.build_extra_cmake_var = tk.StringVar()
        self.build_exe_var = tk.StringVar()
        self.build_launch_args_var = tk.StringVar()
        self.build_status_var = tk.StringVar(value="Open a game repo to build.")
        self._build_busy = False
        self._build_settings_root = ""  # path whose Build fields are loaded
        self._build_env_default = (
            "# KEY=VALUE pairs (space or newline separated)\n"
            "# Example:\n"
            "# RBE_CROSS_OS_PACING_DIAG=1 PSX_RB_ZERO_DELAY=0\n"
        )

        self._report = None
        self._plan = None
        self._step_vars: dict[str, tk.BooleanVar] = {}
        self._git_status = None
        self._repo_index = None
        self._log_pane = None
        self._log_height = _DEFAULT_LOG_HEIGHT
        self._log_sash_job = None

        self._build()
        self._repo_index_load(initial_root=initial_root)
        if self.root_var.get().strip():
            self.refresh_audit()
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        # Startup update check (studio app + shared retcomm-toolchain).
        self.root.after(1200, self._startup_update_check)

    def mainloop(self) -> None:
        self.root.mainloop()

    def _build(self) -> None:
        ctk = self.ctk
        root = self.root

        header = ctk.CTkFrame(root, fg_color="transparent")
        header.pack(fill="x", padx=16, pady=(16, 8))
        ctk.CTkLabel(
            header,
            text="RetComM Studio",
            font=ctk.CTkFont(size=22, weight="bold"),
        ).pack(side="left")
        ctk.CTkLabel(
            header,
            text="Migrate, audit, and GitHub ops for setup-host game repos",
            text_color=("gray40", "gray65"),
            font=ctk.CTkFont(size=13),
        ).pack(side="left", padx=(12, 0), pady=(6, 0))
        ctk.CTkButton(
            header,
            text="Check for updates",
            width=140,
            height=32,
            command=self._on_check_updates,
        ).pack(side="right")
        self._update_status_var = tk.StringVar(value="")
        ctk.CTkLabel(
            header,
            textvariable=self._update_status_var,
            text_color=("gray40", "gray65"),
            font=ctk.CTkFont(size=11),
            anchor="e",
        ).pack(side="right", padx=(0, 10))

        path_row = ctk.CTkFrame(root, fg_color="transparent")
        path_row.pack(fill="x", padx=16, pady=4)
        ctk.CTkLabel(path_row, text="Game repo", width=88, anchor="w").pack(side="left")
        self.repo_menu = ctk.CTkOptionMenu(
            path_row,
            variable=self.repo_label_var,
            values=["(add a repo…)"],
            width=320,
            height=34,
            command=self._on_repo_selected,
        )
        self.repo_menu.pack(side="left", padx=(0, 8))
        ctk.CTkButton(
            path_row, text="Add…", width=70, height=34, command=self._repo_add
        ).pack(side="left")
        ctk.CTkButton(
            path_row, text="Remove", width=80, height=34, command=self._repo_remove
        ).pack(side="left", padx=(6, 0))
        ctk.CTkButton(
            path_row,
            text="Audit",
            width=80,
            height=34,
            fg_color=("#3a7ebf", "#1f538d"),
            command=self.refresh_audit,
        ).pack(side="left", padx=(8, 0))
        ctk.CTkSwitch(
            path_row,
            text="Catalog only",
            variable=self.catalog_only_var,
            command=self._on_catalog_only_toggle,
            width=120,
        ).pack(side="left", padx=(12, 0))

        self.repo_path_label = ctk.CTkLabel(
            root,
            textvariable=self.root_var,
            text_color=("gray40", "gray65"),
            font=ctk.CTkFont(size=12),
            anchor="w",
        )
        self.repo_path_label.pack(fill="x", padx=16 + 88, pady=(0, 2))

        mode = ""
        try:
            mode = str(ctk.get_appearance_mode() or "")
        except Exception:
            mode = ""
        sash_bg = "#3d3d3d" if mode.lower() == "dark" else "#c4c4c4"
        pane = tk.PanedWindow(
            root,
            orient=tk.VERTICAL,
            sashwidth=8,
            sashrelief=tk.FLAT,
            bd=0,
            bg=sash_bg,
            opaqueresize=True,
        )
        pane.pack(fill="both", expand=True, padx=16, pady=(8, 16))
        self._log_pane = pane

        tabs_host = ctk.CTkFrame(pane, fg_color="transparent", corner_radius=0)
        tabs = ctk.CTkTabview(tabs_host, corner_radius=10)
        tabs.pack(fill="both", expand=True)
        tab_migrate = tabs.add("Migrate")
        tab_new = tabs.add("New Project")
        tab_git = tabs.add("Git / GitHub")
        tab_bulk = tabs.add("Bulk")
        tab_build = tabs.add("Build")
        self._tabs = tabs

        self._build_migrate_tab(self._scrollable_tab(tab_migrate))
        self._build_new_project_tab(self._scrollable_tab(tab_new))
        self._build_git_tab(self._scrollable_tab(tab_git))
        self._build_bulk_tab(self._scrollable_tab(tab_bulk))
        self._build_build_tab(self._scrollable_tab(tab_build))

        log_wrap = ctk.CTkFrame(pane, corner_radius=10)
        head = ctk.CTkFrame(log_wrap, fg_color="transparent")
        head.pack(fill="x", padx=12, pady=(8, 2))
        ctk.CTkLabel(
            head,
            text="Log",
            font=ctk.CTkFont(size=13, weight="bold"),
        ).pack(side="left")
        ctk.CTkLabel(
            head,
            text="drag the bar above to resize",
            text_color=("gray45", "gray60"),
            font=ctk.CTkFont(size=11),
        ).pack(side="left", padx=(10, 0), pady=(2, 0))
        self.log = ctk.CTkTextbox(log_wrap, height=_DEFAULT_LOG_HEIGHT, font=ctk.CTkFont(size=12))
        self.log.pack(fill="both", expand=True, padx=10, pady=(0, 10))
        self._setup_log_tags()

        pane.add(tabs_host, minsize=220, stretch="always")
        pane.add(log_wrap, minsize=100, stretch="never")
        pane.bind("<ButtonRelease-1>", self._on_log_sash, add="+")
        self.root.after(80, self._apply_log_sash)

    def _scrollable_tab(self, tab):
        """Fill a Tabview page with a vertical scroller so long tabs stay reachable."""
        ctk = self.ctk
        body = ctk.CTkScrollableFrame(tab, fg_color="transparent")
        body.pack(fill="both", expand=True, padx=0, pady=0)
        return body

    def _build_migrate_tab(self, tab) -> None:
        ctk = self.ctk

        opts = ctk.CTkFrame(tab, corner_radius=10)
        opts.pack(fill="x", padx=4, pady=4)
        ctk.CTkLabel(
            opts,
            text="Options  ·  setup-host only",
            font=ctk.CTkFont(size=13, weight="bold"),
        ).pack(anchor="w", padx=12, pady=(10, 4))

        disc_row = ctk.CTkFrame(opts, fg_color="transparent")
        disc_row.pack(fill="x", padx=12, pady=4)
        ctk.CTkLabel(disc_row, text="Disc .cue", width=88, anchor="w").pack(side="left")
        ctk.CTkEntry(disc_row, textvariable=self.disc_var, height=32).pack(
            side="left", fill="x", expand=True, padx=(0, 8)
        )
        ctk.CTkButton(
            disc_row, text="Browse…", width=90, height=32, command=self._browse_disc
        ).pack(side="left")
        ctk.CTkButton(
            disc_row, text="Clear", width=70, height=32, command=self._clear_disc
        ).pack(side="left", padx=(6, 0))

        row2 = ctk.CTkFrame(opts, fg_color="transparent")
        row2.pack(fill="x", padx=12, pady=4)
        ctk.CTkLabel(row2, text="Players", width=88, anchor="w").pack(side="left")
        ctk.CTkOptionMenu(
            row2,
            values=[str(i) for i in range(1, 9)],
            variable=self.players_var,
            width=72,
            height=32,
            command=self._on_players_changed,
        ).pack(side="left")
        ctk.CTkLabel(row2, text="Zip prefix", width=80, anchor="e").pack(
            side="left", padx=(16, 8)
        )
        ctk.CTkEntry(row2, textvariable=self.zip_var, width=140, height=32).pack(
            side="left"
        )

        toggles = ctk.CTkFrame(opts, fg_color="transparent")
        toggles.pack(fill="x", padx=12, pady=(4, 4))
        for text, var in (
            ("Netplay", self.netplay_var),
            ("CI workflow", self.ci_var),
            ("Probe disc", self.probe_var),
            ("Dry-run", self.dry_run_var),
            ("Force", self.force_var),
        ):
            ctk.CTkSwitch(toggles, text=text, variable=var, width=120).pack(
                side="left", padx=(0, 16)
            )

        ctk.CTkLabel(
            opts,
            text="Wizard + recomp-ui are always enabled. Releases are setup-host only (no prebuilt game C).",
            text_color=("gray40", "gray60"),
            font=ctk.CTkFont(size=12),
            wraplength=900,
            justify="left",
        ).pack(anchor="w", padx=12, pady=(2, 10))

        mid = ctk.CTkFrame(tab, fg_color="transparent", height=320)
        mid.pack(fill="x", padx=4, pady=4)
        mid.pack_propagate(False)
        mid.grid_columnconfigure(0, weight=1)
        mid.grid_columnconfigure(1, weight=1)
        mid.grid_rowconfigure(0, weight=1)

        audit_wrap = ctk.CTkFrame(mid, corner_radius=10)
        audit_wrap.grid(row=0, column=0, sticky="nsew", padx=(0, 6))
        ctk.CTkLabel(
            audit_wrap,
            text="Audit",
            font=ctk.CTkFont(size=13, weight="bold"),
        ).pack(anchor="w", padx=12, pady=(10, 4))
        self.audit_list = ctk.CTkScrollableFrame(audit_wrap, fg_color="transparent")
        self.audit_list.pack(fill="both", expand=True, padx=8, pady=(0, 10))

        plan_wrap = ctk.CTkFrame(mid, corner_radius=10)
        plan_wrap.grid(row=0, column=1, sticky="nsew", padx=(6, 0))
        ctk.CTkLabel(
            plan_wrap,
            text="Plan  ·  uncheck to skip",
            font=ctk.CTkFont(size=13, weight="bold"),
        ).pack(anchor="w", padx=12, pady=(10, 4))
        self.plan_checks = ctk.CTkScrollableFrame(plan_wrap, fg_color="transparent")
        self.plan_checks.pack(fill="both", expand=True, padx=8, pady=(0, 10))

        bottom = ctk.CTkFrame(tab, fg_color="transparent")
        bottom.pack(fill="x", padx=4, pady=8)
        ctk.CTkButton(
            bottom, text="Build plan", width=120, height=36, command=self.refresh_plan
        ).pack(side="left")
        ctk.CTkButton(
            bottom,
            text="Apply selected",
            width=140,
            height=36,
            fg_color=("#2ecc71", "#1e8449"),
            hover_color=("#27ae60", "#196f3d"),
            command=self.apply_selected,
        ).pack(side="left", padx=10)
        ctk.CTkLabel(
            bottom,
            textvariable=self.status_var,
            text_color=("gray30", "gray70"),
            anchor="w",
        ).pack(side="left", fill="x", expand=True)

    def _build_new_project_tab(self, tab) -> None:
        """Wizard that drives setup_project.sh / .ps1 then indexes the result."""
        ctk = self.ctk
        from .newproject import is_windows, setup_script_paths

        sh, ps1 = setup_script_paths()
        script = ps1.name if is_windows() else sh.name

        head = ctk.CTkFrame(tab, corner_radius=10)
        head.pack(fill="x", padx=4, pady=4)
        ctk.CTkLabel(
            head,
            text="New Project  ·  end-to-end setup_project wizard",
            font=ctk.CTkFont(size=13, weight="bold"),
        ).pack(anchor="w", padx=12, pady=(10, 2))
        ctk.CTkLabel(
            head,
            text=f"Routes to {script} on this OS with --yes / -Yes (no TTY prompts).",
            text_color=("gray40", "gray65"),
            anchor="w",
        ).pack(fill="x", padx=12, pady=(0, 8))

        def row(parent, label: str, widget_fn) -> None:
            r = ctk.CTkFrame(parent, fg_color="transparent")
            r.pack(fill="x", padx=12, pady=3)
            ctk.CTkLabel(r, text=label, width=110, anchor="w").pack(side="left")
            widget_fn(r)

        paths = ctk.CTkFrame(tab, corner_radius=10)
        paths.pack(fill="x", padx=4, pady=4)
        ctk.CTkLabel(
            paths, text="Paths", font=ctk.CTkFont(size=13, weight="bold")
        ).pack(anchor="w", padx=12, pady=(10, 4))

        def parent_widgets(r):
            ctk.CTkEntry(r, textvariable=self.np_parent_var, height=30).pack(
                side="left", fill="x", expand=True, padx=(0, 8)
            )
            ctk.CTkButton(
                r, text="Browse…", width=80, height=30, command=self._np_browse_parent
            ).pack(side="left")

        def name_widgets(r):
            ctk.CTkEntry(
                r,
                textvariable=self.np_name_var,
                placeholder_text="MyGameRecomp",
                height=30,
            ).pack(side="left", fill="x", expand=True)

        def disc_widgets(r):
            ctk.CTkEntry(r, textvariable=self.np_disc_var, height=30).pack(
                side="left", fill="x", expand=True, padx=(0, 8)
            )
            ctk.CTkButton(
                r, text="Browse…", width=80, height=30, command=self._np_browse_disc
            ).pack(side="left", padx=(0, 6))
            ctk.CTkButton(
                r,
                text="Autofill meta",
                width=110,
                height=30,
                command=self._np_autofill_meta,
            ).pack(side="left")

        def bios_widgets(r):
            ctk.CTkEntry(
                r,
                textvariable=self.np_bios_var,
                placeholder_text="optional SCPH1001.BIN",
                height=30,
            ).pack(side="left", fill="x", expand=True, padx=(0, 8))
            ctk.CTkButton(
                r, text="Browse…", width=80, height=30, command=self._np_browse_bios
            ).pack(side="left")

        row(paths, "Parent dir", parent_widgets)
        row(paths, "Name", name_widgets)
        row(paths, "Disc (.cue)", disc_widgets)
        row(paths, "BIOS", bios_widgets)
        ctk.CTkLabel(
            paths,
            text="Autofill uses Track-01 CRC/MD5/SHA1 → Redump + libretro "
            "(romhacks often miss). Runs automatically after Browse.",
            text_color=("gray40", "gray65"),
            anchor="w",
            wraplength=960,
        ).pack(fill="x", padx=12, pady=(0, 8))
        ctk.CTkFrame(paths, fg_color="transparent", height=2).pack()

        meta = ctk.CTkFrame(tab, corner_radius=10)
        meta.pack(fill="x", padx=4, pady=4)
        ctk.CTkLabel(
            meta, text="Game", font=ctk.CTkFont(size=13, weight="bold")
        ).pack(anchor="w", padx=12, pady=(10, 4))

        def players_widgets(r):
            ctk.CTkOptionMenu(
                r,
                values=[str(i) for i in range(1, 9)],
                variable=self.np_players_var,
                width=72,
                height=30,
                command=self._np_on_players_changed,
            ).pack(side="left")
            ctk.CTkLabel(r, text="Zip prefix", width=80, anchor="e").pack(
                side="left", padx=(16, 8)
            )
            ctk.CTkEntry(r, textvariable=self.np_zip_var, width=140, height=30).pack(
                side="left"
            )
            ctk.CTkLabel(r, text="Region", width=60, anchor="e").pack(
                side="left", padx=(12, 8)
            )
            ctk.CTkEntry(r, textvariable=self.np_region_var, width=80, height=30).pack(
                side="left"
            )

        def desc_widgets(r):
            ctk.CTkEntry(r, textvariable=self.np_desc_var, height=30).pack(
                side="left", fill="x", expand=True
            )

        def pub_widgets(r):
            ctk.CTkEntry(r, textvariable=self.np_publisher_var, height=30).pack(
                side="left", fill="x", expand=True, padx=(0, 8)
            )
            ctk.CTkLabel(r, text="Year", width=40, anchor="e").pack(side="left")
            ctk.CTkEntry(r, textvariable=self.np_year_var, width=80, height=30).pack(
                side="left", padx=(8, 0)
            )

        row(meta, "Players", players_widgets)
        row(meta, "Description", desc_widgets)
        row(meta, "Publisher", pub_widgets)
        ctk.CTkFrame(meta, fg_color="transparent", height=6).pack()

        feats = ctk.CTkFrame(tab, corner_radius=10)
        feats.pack(fill="x", padx=4, pady=4)
        ctk.CTkLabel(
            feats, text="Features", font=ctk.CTkFont(size=13, weight="bold")
        ).pack(anchor="w", padx=12, pady=(10, 4))
        toggles = ctk.CTkFrame(feats, fg_color="transparent")
        toggles.pack(fill="x", padx=12, pady=(0, 4))
        for text, var in (
            ("recomp-ui", self.np_ui_var),
            ("Wizard", self.np_wizard_var),
            ("Netplay", self.np_netplay_var),
            ("CI", self.np_ci_var),
            ("Boxart", self.np_boxart_var),
            ("Copy disc into project", self.np_stage_var),
            ("Generate", self.np_generate_var),
            ("Build", self.np_build_var),
            ("GitHub", self.np_github_var),
        ):
            ctk.CTkCheckBox(toggles, text=text, variable=var, width=100).pack(
                side="left", padx=(0, 6), pady=2
            )

        def lobby_widgets(r):
            ctk.CTkEntry(r, textvariable=self.np_lobby_var, height=30).pack(
                side="left", fill="x", expand=True, padx=(0, 8)
            )
            ctk.CTkLabel(r, text="GH vis", width=50, anchor="e").pack(side="left")
            ctk.CTkOptionMenu(
                r,
                values=["private", "public", "internal"],
                variable=self.np_gh_vis_var,
                width=100,
                height=30,
            ).pack(side="left", padx=(8, 0))

        row(feats, "Lobby URL", lobby_widgets)
        ctk.CTkFrame(feats, fg_color="transparent", height=6).pack()

        refs = ctk.CTkFrame(tab, corner_radius=10)
        refs.pack(fill="x", padx=4, pady=4)
        ctk.CTkLabel(
            refs,
            text="Submodule / nested branches",
            font=ctk.CTkFont(size=13, weight="bold"),
        ).pack(anchor="w", padx=12, pady=(10, 4))
        ctk.CTkLabel(
            refs,
            text="Pick a public remote head (git ls-remote). "
            "net/rb “(default)” keeps the script’s pin.",
            text_color=("gray40", "gray65"),
            anchor="w",
            wraplength=960,
        ).pack(fill="x", padx=12, pady=(0, 4))

        # Compact select-only menus in a left→right wrapping row.
        wrap = ctk.CTkFrame(refs, fg_color="transparent")
        wrap.pack(fill="x", padx=12, pady=2)
        self._np_ref_wrap = wrap
        self._np_ref_cells: list = []

        def ref_menu(label: str, var: tk.StringVar, menu_attr: str, values: list[str]):
            cell = ctk.CTkFrame(wrap, fg_color="transparent")
            ctk.CTkLabel(cell, text=label, anchor="w").pack(anchor="w")
            menu = ctk.CTkOptionMenu(
                cell,
                variable=var,
                values=values,
                width=150,
                height=28,
            )
            menu.pack(anchor="w", pady=(2, 0))
            setattr(self, menu_attr, menu)
            self._np_ref_cells.append(cell)
            return menu

        ref_menu("psxrecomp", self.np_psx_ref_var, "np_psx_ref_menu", ["master"])
        ref_menu("recomp-ui", self.np_ui_ref_var, "np_ui_ref_menu", ["master"])
        ref_menu(
            "recomp-net",
            self.np_net_ref_var,
            "np_net_ref_menu",
            ["(default)"],
        )
        ref_menu(
            "rbengine",
            self.np_rb_ref_var,
            "np_rb_ref_menu",
            ["(default)"],
        )
        wrap.bind("<Configure>", self._np_reflow_ref_menus)
        self.root.after(50, self._np_reflow_ref_menus)

        bref = ctk.CTkFrame(refs, fg_color="transparent")
        bref.pack(fill="x", padx=12, pady=(4, 8))
        ctk.CTkButton(
            bref,
            text="Refresh remote branches",
            width=180,
            height=30,
            command=self._np_refresh_remote_branches,
        ).pack(side="left")
        ctk.CTkLabel(
            bref,
            textvariable=self.np_branch_status_var,
            text_color=("gray40", "gray65"),
            anchor="w",
        ).pack(side="left", padx=12)
        self.root.after(200, self._np_refresh_remote_branches)

        actions = ctk.CTkFrame(tab, corner_radius=10)
        actions.pack(fill="x", padx=4, pady=4)
        brow = ctk.CTkFrame(actions, fg_color="transparent")
        brow.pack(fill="x", padx=12, pady=12)
        ctk.CTkButton(
            brow,
            text="Create project",
            width=160,
            height=36,
            fg_color=("#2ecc71", "#1e8449"),
            hover_color=("#27ae60", "#196f3d"),
            command=self._np_create,
        ).pack(side="left")
        ctk.CTkSwitch(
            brow, text="Dry-run", variable=self.dry_run_var, width=100
        ).pack(side="left", padx=16)
        ctk.CTkLabel(
            brow,
            textvariable=self.np_status_var,
            text_color=("gray30", "gray70"),
            anchor="w",
        ).pack(side="left", fill="x", expand=True)

    def _np_on_players_changed(self, value: str) -> None:
        """Auto netplay from player count; keep the checkbox always clickable."""
        try:
            n = int(value or self.np_players_var.get() or "2")
        except ValueError:
            n = 2
        if n >= 2:
            self.np_netplay_var.set(True)
        else:
            # 1P → turn off, but do not grey out (user may still toggle on).
            self.np_netplay_var.set(False)

    def _maximize_window(self) -> None:
        """Maximize the main window (cross-platform best-effort)."""
        try:
            self.root.state("zoomed")
            return
        except tk.TclError:
            pass
        try:
            self.root.attributes("-zoomed", True)
            return
        except tk.TclError:
            pass
        try:
            sw = int(self.root.winfo_screenwidth())
            sh = int(self.root.winfo_screenheight())
            self.root.geometry(f"{sw}x{sh}+0+0")
        except tk.TclError:
            pass

    def _apply_app_icon(self) -> None:
        """Window / taskbar icon from assets (PNG / ICO)."""
        from .paths import assets_dir

        base = assets_dir()
        if base is None:
            return
        ico = base / "retcomm-studio.ico"
        png = base / "retcomm-studio.png"
        try:
            if sys.platform == "win32" and ico.is_file():
                self.root.iconbitmap(default=str(ico))
                return
        except tk.TclError:
            pass
        try:
            if png.is_file():
                img = tk.PhotoImage(file=str(png))
                self.root.iconphoto(True, img)
                self._app_icon_image = img  # keep ref
        except tk.TclError:
            pass

    def _startup_update_check(self) -> None:
        from .updater import check_updates_on_startup_enabled

        if not check_updates_on_startup_enabled():
            return
        self._run_update_check(prompt_if_available=True, silent_if_current=True)

    def _on_check_updates(self) -> None:
        self._run_update_check(prompt_if_available=True, silent_if_current=False)

    def _run_update_check(
        self, *, prompt_if_available: bool, silent_if_current: bool
    ) -> None:
        if getattr(self, "_update_check_busy", False):
            return
        self._update_check_busy = True
        if hasattr(self, "_update_status_var"):
            self._update_status_var.set("Checking…")
        self._log("--- Check for updates (Studio + toolchain) ---")

        def on_progress(msg: str) -> None:
            self.root.after(0, lambda m=msg: self._update_status_var.set(m[:80]))

        def on_done(result) -> None:
            def apply() -> None:
                self._update_check_busy = False
                self._log(result.studio.message)
                self._log(result.toolchain.message)
                if hasattr(self, "_update_status_var"):
                    if result.studio.available or result.toolchain.available:
                        self._update_status_var.set("Updates available")
                    else:
                        self._update_status_var.set("Up to date")
                any_avail = result.studio.available or result.toolchain.available
                if any_avail and prompt_if_available:
                    self._prompt_apply_updates(result)
                elif not silent_if_current:
                    messagebox.showinfo(
                        "RetComM Studio",
                        result.message or "Everything is up to date.",
                        parent=self.root,
                    )

            self.root.after(0, apply)

        from .updater import check_updates_async

        check_updates_async(on_done, on_progress=on_progress)

    def _prompt_apply_updates(self, result) -> None:
        lines = []
        if result.studio.available:
            lines.append(f"• Studio: {result.studio.current} → {result.studio.latest}")
        if result.toolchain.available:
            lines.append(
                f"• Toolchain: {result.toolchain.current} → {result.toolchain.latest}"
            )
        body = (
            "Updates are available:\n\n"
            + "\n".join(lines)
            + "\n\nApply now?\n"
            "(Toolchain installs into the shared RetComM cache used by the "
            "launcher and game apps.)"
        )
        if not messagebox.askyesno("RetComM Studio", body, parent=self.root):
            return

        update_studio = bool(result.studio.available)
        update_toolchain = bool(result.toolchain.available)
        # If both, ask whether to do both or just one — keep it simple: both.
        self._update_status_var.set("Updating…")
        self._log("--- Applying updates ---")

        def worker() -> None:
            from .updater import apply_updates

            def prog(msg: str) -> None:
                self.root.after(0, lambda m=msg: self._update_status_var.set(m[:80]))
                self.root.after(0, lambda m=msg: self._log(m))

            summary, should_exit = apply_updates(
                result,
                update_studio=update_studio,
                update_toolchain=update_toolchain,
                on_progress=prog,
            )

            def done() -> None:
                self._log(summary)
                self._update_status_var.set("Update done" if not should_exit else "Restarting…")
                if should_exit:
                    messagebox.showinfo(
                        "RetComM Studio",
                        summary + "\n\nStudio will exit to finish the update.",
                        parent=self.root,
                    )
                    self.root.after(300, self.root.destroy)
                else:
                    messagebox.showinfo(
                        "RetComM Studio", summary, parent=self.root
                    )

            self.root.after(0, done)

        threading.Thread(target=worker, daemon=True).start()

    def _np_reflow_ref_menus(self, _event=None) -> None:
        """Lay out New Project branch menus left→right, wrapping when needed."""
        wrap = getattr(self, "_np_ref_wrap", None)
        cells = getattr(self, "_np_ref_cells", None)
        if wrap is None or not cells:
            return
        width = max(int(wrap.winfo_width()), 1)
        # Each cell ≈ OptionMenu 150 + padding; keep at least one column.
        cell_w = 170
        cols = max(1, width // cell_w)
        if getattr(self, "_np_ref_cols", None) == cols:
            return
        self._np_ref_cols = cols
        for i, cell in enumerate(cells):
            cell.grid(row=i // cols, column=i % cols, sticky="nw", padx=(0, 16), pady=4)

    @staticmethod
    def _np_ref_choice(raw: str) -> str:
        """Map OptionMenu sentinel labels to empty / concrete branch names."""
        v = (raw or "").strip()
        if not v or v.startswith("("):
            return ""
        return v

    def _np_browse_parent(self) -> None:
        path = _pick_directory(
            title="Parent directory for new project",
            parent=self.root,
            initialdir=self.np_parent_var.get().strip() or None,
        )
        if path:
            self.np_parent_var.set(path)

    def _np_browse_disc(self) -> None:
        start = self.np_disc_var.get().strip() or self.np_parent_var.get().strip() or None
        if start and Path(start).is_file():
            start = str(Path(start).parent)
        path = _pick_open_file(
            title="Select Redump .cue",
            parent=self.root,
            initialdir=start,
            filetypes=[("Cue sheet", "*.cue"), ("All files", "*.*")],
        )
        if not path:
            return
        cue = str(Path(path).expanduser().resolve())
        self.np_disc_var.set(cue)
        self._np_populate_name_from_disc(cue, only_empty=True)
        self._np_autofill_meta(only_empty=True)

    def _np_populate_name_from_disc(self, cue: str, *, only_empty: bool = True) -> None:
        """Set project name from cue stem (and later Redump title via autofill)."""
        if only_empty and self.np_name_var.get().strip():
            return
        from .discmeta import suggest_project_name

        guess = suggest_project_name(Path(cue).stem)
        if guess:
            self.np_name_var.set(guess)
            self._np_name_from_disc = True

    def _np_browse_bios(self) -> None:
        start = self.np_bios_var.get().strip() or self.np_parent_var.get().strip() or None
        if start and Path(start).is_file():
            start = str(Path(start).parent)
        path = _pick_open_file(
            title="Select SCPH1001 BIOS",
            parent=self.root,
            initialdir=start,
            filetypes=[("BIOS", "*.bin *.BIN"), ("All files", "*.*")],
        )
        if path:
            self.np_bios_var.set(str(Path(path).expanduser().resolve()))

    def _np_refresh_remote_branches(self) -> None:
        """Populate New Project branch ComboBoxes via git ls-remote."""
        if getattr(self, "_np_branch_refresh_busy", False):
            return
        self._np_branch_refresh_busy = True
        self.np_branch_status_var.set("Fetching remote heads…")

        def worker() -> None:
            from .gitops import (
                DEFAULT_PSXRECOMP_URL,
                DEFAULT_RECOMP_NET_URL,
                DEFAULT_RECOMP_UI_URL,
                DEFAULT_RBENGINE_URL,
                list_remote_head_branches,
            )

            try:
                results = {
                    "psx": list_remote_head_branches(DEFAULT_PSXRECOMP_URL),
                    "ui": list_remote_head_branches(DEFAULT_RECOMP_UI_URL),
                    "net": list_remote_head_branches(DEFAULT_RECOMP_NET_URL),
                    "rb": list_remote_head_branches(DEFAULT_RBENGINE_URL),
                }
                err = ""
            except Exception as exc:
                results = {"psx": [], "ui": [], "net": [], "rb": []}
                err = str(exc)

            def apply() -> None:
                self._np_branch_refresh_busy = False
                required = (
                    ("psx", self.np_psx_ref_var, getattr(self, "np_psx_ref_menu", None)),
                    ("ui", self.np_ui_ref_var, getattr(self, "np_ui_ref_menu", None)),
                )
                optional = (
                    ("net", self.np_net_ref_var, getattr(self, "np_net_ref_menu", None)),
                    ("rb", self.np_rb_ref_var, getattr(self, "np_rb_ref_menu", None)),
                )
                counts = []
                for key, var, menu in required:
                    branches = results.get(key) or []
                    if menu is None:
                        continue
                    self._set_branch_menu(menu, var, branches)
                    counts.append(f"{key}:{len(branches)}")
                for key, var, menu in optional:
                    branches = results.get(key) or []
                    if menu is None:
                        continue
                    current = self._np_ref_choice(var.get())
                    values = ["(default)"] + [b for b in branches if b]
                    if current and current not in values:
                        values = ["(default)", current, *[b for b in branches if b and b != current]]
                    menu.configure(values=values)
                    target = current if current in values else "(default)"
                    var.set(target)
                    try:
                        menu.set(target)
                    except Exception:
                        pass
                    counts.append(f"{key}:{len(branches)}")
                if err:
                    self.np_branch_status_var.set(f"Branch fetch error: {err}")
                    self._log(f"New Project branch refresh failed: {err}")
                else:
                    self.np_branch_status_var.set(
                        "Remote heads: " + ", ".join(counts)
                    )

            self.root.after(0, apply)

        threading.Thread(target=worker, daemon=True).start()

    def _np_autofill_meta(self, only_empty: bool = False) -> None:
        """Cross-reference disc digests with Redump / libretro / catalog."""
        cue = self.np_disc_var.get().strip()
        if not cue:
            messagebox.showinfo(
                "Project Studio",
                "Select a disc .cue first.",
                parent=self.root,
            )
            return
        if not Path(cue).expanduser().is_file():
            messagebox.showerror(
                "Project Studio",
                f"Disc not found:\n{cue}",
                parent=self.root,
            )
            return
        if getattr(self, "_np_autofill_busy", False):
            return
        self._np_autofill_busy = True
        self.np_status_var.set("Looking up disc metadata…")
        self._log(f"Autofill meta for {cue}")

        def worker() -> None:
            from .discmeta import lookup_cue, suggest_project_name

            try:
                hit = lookup_cue(cue)
                err = ""
            except Exception as exc:
                hit = None
                err = str(exc)

            def apply() -> None:
                self._np_autofill_busy = False
                if hit is None:
                    self.np_status_var.set(f"Autofill failed: {err}")
                    self._log(f"Autofill failed: {err}")
                    messagebox.showerror(
                        "Project Studio",
                        f"Metadata lookup failed:\n{err}",
                        parent=self.root,
                    )
                    return
                for note in hit.notes:
                    self._log(f"  meta: {note}")

                def set_if(var: tk.StringVar, value: str) -> bool:
                    if not value:
                        return False
                    if only_empty and var.get().strip():
                        return False
                    var.set(value)
                    return True

                filled: list[str] = []
                if hit.players is not None:
                    cur = (self.np_players_var.get() or "").strip()
                    if (not only_empty) or cur in ("", "2"):
                        self.np_players_var.set(str(hit.players))
                        filled.append(f"players={hit.players}")
                if set_if(self.np_desc_var, hit.description):
                    filled.append("description")
                if set_if(self.np_publisher_var, hit.publisher):
                    filled.append("publisher")
                if set_if(self.np_year_var, str(hit.year or "")):
                    filled.append(f"year={hit.year}")
                if set_if(self.np_region_var, hit.region):
                    filled.append(f"region={hit.region}")
                if hit.name:
                    guess = suggest_project_name(hit.name)
                    cur = self.np_name_var.get().strip()
                    # Prefer Redump/catalog title over a cue-stem autosuggest.
                    if guess and (
                        not only_empty
                        or not cur
                        or getattr(self, "_np_name_from_disc", False)
                    ):
                        self.np_name_var.set(guess)
                        self._np_name_from_disc = True
                        filled.append("name")
                elif not self.np_name_var.get().strip():
                    cue_guess = suggest_project_name(Path(cue).stem)
                    if cue_guess:
                        self.np_name_var.set(cue_guess)
                        self._np_name_from_disc = True
                        filled.append("name")

                # Keep netplay in sync when players were autofilled.
                self._np_on_players_changed(self.np_players_var.get())

                src = hit.source or "none"
                if filled:
                    msg = f"Autofill ({src}): " + ", ".join(filled)
                elif src == "none":
                    msg = "Autofill: no match (romhack / non-Redump?)"
                else:
                    msg = f"Autofill ({src}): fields already set"
                self.np_status_var.set(msg)
                self._log(msg)

            self.root.after(0, apply)

        threading.Thread(target=worker, daemon=True).start()

    def _np_options(self):
        from .newproject import NewProjectOptions

        try:
            players = int(self.np_players_var.get() or "2")
        except ValueError:
            players = 2
        return NewProjectOptions(
            name=self.np_name_var.get().strip(),
            disc=self.np_disc_var.get().strip(),
            parent_dir=self.np_parent_var.get().strip() or ".",
            bios=self.np_bios_var.get().strip(),
            players=players,
            zip_prefix=self.np_zip_var.get().strip(),
            description=self.np_desc_var.get().strip(),
            publisher=self.np_publisher_var.get().strip(),
            year=self.np_year_var.get().strip(),
            region=self.np_region_var.get().strip() or "USA",
            enable_recomp_ui=bool(self.np_ui_var.get()),
            enable_wizard=bool(self.np_wizard_var.get()),
            enable_netplay=bool(self.np_netplay_var.get()),
            lobby_url=self.np_lobby_var.get().strip() or "netplay.retcomm.net",
            enable_ci=bool(self.np_ci_var.get()),
            fetch_boxart=bool(self.np_boxart_var.get()),
            stage_disc=bool(self.np_stage_var.get()),
            do_generate=bool(self.np_generate_var.get()),
            do_build=bool(self.np_build_var.get()),
            create_github=bool(self.np_github_var.get()),
            github_visibility=self.np_gh_vis_var.get().strip() or "private",
            psxrecomp_ref=self._np_ref_choice(self.np_psx_ref_var.get()) or "master",
            recomp_ui_ref=self._np_ref_choice(self.np_ui_ref_var.get()) or "master",
            recomp_net_ref=self._np_ref_choice(self.np_net_ref_var.get()),
            rbengine_ref=self._np_ref_choice(self.np_rb_ref_var.get()),
            dry_run=self._git_dry(),
        )

    def _np_create(self) -> None:
        from .newproject import (
            index_new_project,
            project_root_for,
            run_new_project,
            validate_options,
        )

        if self._newproj_busy:
            messagebox.showinfo(
                "Project Studio",
                "A new-project run is already in progress.",
                parent=self.root,
            )
            return
        opts = self._np_options()
        errs = validate_options(opts)
        if errs:
            messagebox.showerror(
                "Project Studio",
                "Fix these before creating:\n\n• " + "\n• ".join(errs),
                parent=self.root,
            )
            return
        dest = project_root_for(opts)
        if not opts.dry_run:
            if not messagebox.askyesno(
                "Project Studio",
                f"Create new project?\n\n{dest}\n\n"
                f"disc={opts.disc}\n"
                f"players={opts.players}  ui={opts.enable_recomp_ui}  "
                f"wizard={opts.enable_wizard}  netplay={opts.enable_netplay}\n"
                f"generate={opts.do_generate}  build={opts.do_build}  "
                f"github={opts.create_github}",
                parent=self.root,
            ):
                return

        def worker() -> None:
            self._newproj_busy = True

            def log_line(msg: str) -> None:
                self.root.after(0, lambda m=msg: self._log(m))

            try:
                self.root.after(
                    0, lambda: self.np_status_var.set("Running setup_project…")
                )
                self.root.after(
                    0, lambda: self._log("--- New project setup ---")
                )
                r = run_new_project(opts, on_line=log_line)
                self.root.after(0, lambda: self._log_cmd(r))
                if r.ok and not opts.dry_run:
                    root = project_root_for(opts)
                    ir = index_new_project(
                        root, name=opts.name, cue=opts.disc
                    )
                    self.root.after(0, lambda: self._log_cmd(ir))

                    def finish() -> None:
                        # Reload index from disk (add_repo already saved)
                        self._repo_index_load(initial_root=root)
                        self.np_status_var.set(f"Created {root.name}")
                        messagebox.showinfo(
                            "Project Studio",
                            f"Project ready:\n{root}\n\nIndexed in Studio.",
                            parent=self.root,
                        )

                    self.root.after(0, finish)
                else:
                    self.root.after(
                        0,
                        lambda: self.np_status_var.set(
                            f"{'OK' if r.ok else 'FAIL'}: {r.message}"
                        ),
                    )
            finally:
                self._newproj_busy = False

        threading.Thread(target=worker, daemon=True).start()

    def _build_git_tab(self, tab) -> None:
        ctk = self.ctk

        top = ctk.CTkFrame(tab, corner_radius=10)
        top.pack(fill="x", padx=4, pady=4)
        head = ctk.CTkFrame(top, fg_color="transparent")
        head.pack(fill="x", padx=12, pady=(10, 4))
        ctk.CTkLabel(
            head, text="Repository", font=ctk.CTkFont(size=13, weight="bold")
        ).pack(side="left")
        ctk.CTkButton(
            head, text="Refresh", width=90, height=30, command=self.refresh_git
        ).pack(side="right")
        ctk.CTkButton(
            head,
            text="Fetch branches",
            width=120,
            height=30,
            command=self._git_fetch_branches,
        ).pack(side="right", padx=(0, 8))
        ctk.CTkSwitch(
            head, text="Dry-run", variable=self.dry_run_var, width=100
        ).pack(side="right", padx=12)

        pull_opts = ctk.CTkFrame(top, fg_color="transparent")
        pull_opts.pack(fill="x", padx=12, pady=(0, 6))
        ctk.CTkLabel(pull_opts, text="Pull mode", width=80, anchor="w").pack(
            side="left"
        )
        self.git_pull_mode_menu = ctk.CTkComboBox(
            pull_opts,
            variable=self.git_pull_mode_var,
            values=["ff-only", "rebase", "merge", "reset"],
            width=120,
            height=28,
        )
        self.git_pull_mode_menu.pack(side="left", padx=(0, 12))
        ctk.CTkLabel(pull_opts, text="If dirty", width=70, anchor="w").pack(
            side="left"
        )
        self.git_pull_dirty_menu = ctk.CTkComboBox(
            pull_opts,
            variable=self.git_pull_dirty_var,
            values=["fail", "stash", "discard"],
            width=110,
            height=28,
        )
        self.git_pull_dirty_menu.pack(side="left")
        ctk.CTkLabel(
            pull_opts,
            text="reset = match origin  ·  discard drops local edits",
            text_color=("gray40", "gray60"),
            anchor="w",
        ).pack(side="left", padx=(12, 0))

        self.git_summary_var = tk.StringVar(value="Open a game repo, then Refresh.")
        ctk.CTkLabel(
            top,
            textvariable=self.git_summary_var,
            text_color=("gray30", "gray70"),
            anchor="w",
            justify="left",
            wraplength=980,
        ).pack(fill="x", padx=12, pady=(0, 8))

        branch_row = ctk.CTkFrame(top, fg_color="transparent")
        branch_row.pack(fill="x", padx=12, pady=(0, 10))
        ctk.CTkLabel(branch_row, text="Game branch", width=100, anchor="w").pack(
            side="left"
        )
        self.git_branch_menu = ctk.CTkComboBox(
            branch_row,
            variable=self.git_branch_var,
            values=["(refresh)"],
            width=200,
            height=30,
        )
        self.git_branch_menu.pack(side="left", padx=(0, 8))
        ctk.CTkButton(
            branch_row,
            text="Switch",
            width=80,
            height=30,
            command=self._git_switch_branch,
        ).pack(side="left")
        ctk.CTkButton(
            branch_row, text="Pull", width=70, height=30, command=self._git_pull
        ).pack(side="left", padx=6)
        ctk.CTkButton(
            branch_row, text="Push", width=70, height=30, command=self._git_push
        ).pack(side="left")
        ctk.CTkSwitch(
            branch_row,
            text="Create branch",
            variable=self.git_create_branch_var,
            width=130,
        ).pack(side="left", padx=(12, 0))

        game_commit = ctk.CTkFrame(top, fg_color="transparent")
        game_commit.pack(fill="x", padx=12, pady=(0, 10))
        ctk.CTkEntry(
            game_commit,
            textvariable=self.git_msg_var,
            placeholder_text="Commit message for game repo",
            height=30,
        ).pack(side="left", fill="x", expand=True, padx=(0, 8))
        ctk.CTkButton(
            game_commit,
            text="Commit",
            width=90,
            height=30,
            command=self._git_commit,
        ).pack(side="left")

        sub_wrap = ctk.CTkFrame(tab, corner_radius=10)
        sub_wrap.pack(fill="x", padx=4, pady=4)
        ctk.CTkLabel(
            sub_wrap,
            text="Submodules  ·  Switch moves HEAD; Save only writes .gitmodules tracking",
            font=ctk.CTkFont(size=13, weight="bold"),
        ).pack(anchor="w", padx=12, pady=(10, 4))

        cfg = ctk.CTkFrame(sub_wrap, fg_color="transparent")
        cfg.pack(fill="x", padx=12, pady=4)
        ctk.CTkLabel(cfg, text="psxrecomp", width=90, anchor="w").pack(side="left")
        self.git_psx_branch_menu = ctk.CTkComboBox(
            cfg,
            variable=self.git_psx_branch_var,
            values=["master"],
            width=150,
            height=28,
        )
        self.git_psx_branch_menu.pack(side="left", padx=(0, 12))
        ctk.CTkLabel(cfg, text="recomp-ui", width=80, anchor="w").pack(side="left")
        self.git_ui_branch_menu = ctk.CTkComboBox(
            cfg,
            variable=self.git_ui_branch_var,
            values=["master"],
            width=150,
            height=28,
        )
        self.git_ui_branch_menu.pack(side="left", padx=(0, 12))
        ctk.CTkButton(
            cfg,
            text="Ensure both",
            width=110,
            height=28,
            command=self._git_ensure_submodules,
        ).pack(side="left")
        ctk.CTkButton(
            cfg,
            text="Switch modules",
            width=130,
            height=28,
            command=self._git_switch_modules,
        ).pack(side="left", padx=8)
        ctk.CTkButton(
            cfg,
            text="Save tracking",
            width=120,
            height=28,
            command=self._git_save_submodule_branches,
        ).pack(side="left")

        actions = ctk.CTkFrame(sub_wrap, fg_color="transparent")
        actions.pack(fill="x", padx=12, pady=(0, 4))
        ctk.CTkSwitch(
            actions,
            text="Update to remote tip",
            variable=self.git_remote_update_var,
            width=160,
        ).pack(side="left")
        ctk.CTkButton(
            actions,
            text="Update submodules",
            width=150,
            height=30,
            command=self._git_update_submodules,
        ).pack(side="left", padx=12)
        ctk.CTkButton(
            actions, text="Pull", width=70, height=30, command=self._git_pull_modules
        ).pack(side="left")
        ctk.CTkButton(
            actions, text="Push", width=70, height=30, command=self._git_push_modules
        ).pack(side="left", padx=6)

        sub_commit = ctk.CTkFrame(sub_wrap, fg_color="transparent")
        sub_commit.pack(fill="x", padx=12, pady=(0, 4))
        ctk.CTkEntry(
            sub_commit,
            textvariable=self.git_sub_msg_var,
            placeholder_text="Commit inside psxrecomp + recomp-ui",
            height=30,
        ).pack(side="left", fill="x", expand=True, padx=(0, 8))
        ctk.CTkButton(
            sub_commit,
            text="Commit modules",
            width=140,
            height=30,
            command=self._git_commit_modules,
        ).pack(side="left")

        # Plain frame (not ScrollableFrame): grows with module rows so the
        # tab-level scroller owns the wheel, not nested section scrollbars.
        self.git_sub_list = ctk.CTkFrame(sub_wrap, fg_color="transparent")
        self.git_sub_list.pack(fill="x", padx=8, pady=(0, 6))

        nested_wrap = ctk.CTkFrame(tab, corner_radius=10)
        nested_wrap.pack(fill="x", padx=4, pady=4)
        ctk.CTkLabel(
            nested_wrap,
            text="Nested in psxrecomp  ·  recomp-net + retcomm-rbengine",
            font=ctk.CTkFont(size=13, weight="bold"),
        ).pack(anchor="w", padx=12, pady=(10, 4))

        ncfg = ctk.CTkFrame(nested_wrap, fg_color="transparent")
        ncfg.pack(fill="x", padx=12, pady=4)
        ctk.CTkLabel(ncfg, text="recomp-net", width=100, anchor="w").pack(side="left")
        self.git_net_branch_menu = ctk.CTkComboBox(
            ncfg,
            variable=self.git_net_branch_var,
            values=["main"],
            width=150,
            height=28,
        )
        self.git_net_branch_menu.pack(side="left", padx=(0, 12))
        ctk.CTkLabel(ncfg, text="rbengine", width=80, anchor="w").pack(side="left")
        self.git_rb_branch_menu = ctk.CTkComboBox(
            ncfg,
            variable=self.git_rb_branch_var,
            values=["main"],
            width=150,
            height=28,
        )
        self.git_rb_branch_menu.pack(side="left", padx=(0, 12))
        ctk.CTkButton(
            ncfg,
            text="Ensure nested",
            width=120,
            height=28,
            command=self._git_ensure_nested,
        ).pack(side="left")
        ctk.CTkButton(
            ncfg,
            text="Switch nested",
            width=130,
            height=28,
            command=self._git_switch_nested,
        ).pack(side="left", padx=8)
        ctk.CTkButton(
            ncfg,
            text="Save tracking",
            width=120,
            height=28,
            command=self._git_save_nested_branches,
        ).pack(side="left")

        nact = ctk.CTkFrame(nested_wrap, fg_color="transparent")
        nact.pack(fill="x", padx=12, pady=(0, 4))
        ctk.CTkButton(
            nact,
            text="Update nested",
            width=130,
            height=30,
            command=self._git_update_nested,
        ).pack(side="left")
        ctk.CTkButton(
            nact, text="Pull libs", width=90, height=30, command=self._git_pull_nested
        ).pack(side="left", padx=6)
        ctk.CTkButton(
            nact, text="Push libs", width=90, height=30, command=self._git_push_nested
        ).pack(side="left")

        libs_commit = ctk.CTkFrame(nested_wrap, fg_color="transparent")
        libs_commit.pack(fill="x", padx=12, pady=(0, 4))
        ctk.CTkEntry(
            libs_commit,
            textvariable=self.git_libs_msg_var,
            placeholder_text="Commit inside recomp-net + rbengine",
            height=30,
        ).pack(side="left", fill="x", expand=True, padx=(0, 8))
        ctk.CTkButton(
            libs_commit,
            text="Commit libs",
            width=120,
            height=30,
            command=self._git_commit_nested_libs,
        ).pack(side="left")

        nact2 = ctk.CTkFrame(nested_wrap, fg_color="transparent")
        nact2.pack(fill="x", padx=12, pady=(0, 4))
        ctk.CTkEntry(
            nact2,
            textvariable=self.git_nested_msg_var,
            placeholder_text="psxrecomp commit message (gitlinks)",
            height=30,
        ).pack(side="left", fill="x", expand=True, padx=(0, 8))
        ctk.CTkButton(
            nact2,
            text="Commit in psxrecomp",
            width=150,
            height=30,
            command=self._git_commit_nested,
        ).pack(side="left")
        ctk.CTkButton(
            nact2,
            text="Pull psxrecomp",
            width=120,
            height=30,
            command=self._git_pull_psxrecomp,
        ).pack(side="left", padx=6)
        ctk.CTkButton(
            nact2,
            text="Push psxrecomp",
            width=120,
            height=30,
            command=self._git_push_psxrecomp,
        ).pack(side="left")

        self.git_nested_list = ctk.CTkFrame(nested_wrap, fg_color="transparent")
        self.git_nested_list.pack(fill="x", padx=8, pady=(0, 10))

        rel = ctk.CTkFrame(tab, corner_radius=10)
        rel.pack(fill="x", padx=4, pady=(4, 8))
        ctk.CTkLabel(
            rel,
            text="Release CI  ·  workflow_dispatch release.yml",
            font=ctk.CTkFont(size=13, weight="bold"),
        ).pack(anchor="w", padx=12, pady=(10, 4))
        rel_row = ctk.CTkFrame(rel, fg_color="transparent")
        rel_row.pack(fill="x", padx=12, pady=(0, 4))
        ctk.CTkLabel(rel_row, text="Version", width=70, anchor="w").pack(side="left")
        ctk.CTkEntry(
            rel_row,
            textvariable=self.release_version_var,
            placeholder_text="empty = auto-bump",
            width=140,
            height=28,
        ).pack(side="left", padx=(0, 10))
        ctk.CTkLabel(rel_row, text="Bump", width=50, anchor="w").pack(side="left")
        ctk.CTkOptionMenu(
            rel_row,
            values=["patch", "minor", "major"],
            variable=self.release_bump_var,
            width=90,
            height=28,
        ).pack(side="left", padx=(0, 12))
        ctk.CTkSwitch(
            rel_row, text="Publish", variable=self.release_publish_var, width=100
        ).pack(side="left", padx=(0, 10))
        ctk.CTkSwitch(
            rel_row, text="Reuse emitters", variable=self.release_reuse_var, width=130
        ).pack(side="left")
        rel_btns = ctk.CTkFrame(rel, fg_color="transparent")
        rel_btns.pack(fill="x", padx=12, pady=(4, 4))
        ctk.CTkButton(
            rel_btns,
            text="Run release workflow",
            width=180,
            height=34,
            fg_color=("#c0392b", "#922b21"),
            hover_color=("#a93226", "#7b241c"),
            command=self._git_run_release,
        ).pack(side="left", padx=(0, 8))
        ctk.CTkButton(
            rel_btns,
            text="Install & push CI",
            width=160,
            height=34,
            command=self._git_install_push_ci,
        ).pack(side="left")
        ctk.CTkLabel(
            rel,
            text="Install & push CI writes psxrecomp setup-release.yml → "
            ".github/workflows/release.yml, commits, pushes, and registers Actions.",
            text_color=("gray30", "gray70"),
            anchor="w",
            wraplength=980,
        ).pack(fill="x", padx=12, pady=(0, 10))

    def _build_bulk_tab(self, tab) -> None:
        """Multi-repo Git/GitHub ops against the indexed game list."""
        ctk = self.ctk

        head = ctk.CTkFrame(tab, corner_radius=10)
        head.pack(fill="x", padx=4, pady=4)
        ctk.CTkLabel(
            head,
            text="Bulk  ·  run Git/GitHub ops on selected indexed repos (parallel 1–4)",
            font=ctk.CTkFont(size=13, weight="bold"),
        ).pack(anchor="w", padx=12, pady=(10, 4))
        ctk.CTkLabel(
            head,
            text="Uses the same Pull mode / If dirty / Dry-run settings as the Git tab.",
            text_color=("gray40", "gray65"),
            anchor="w",
        ).pack(fill="x", padx=12, pady=(0, 8))

        opts = ctk.CTkFrame(head, fg_color="transparent")
        opts.pack(fill="x", padx=12, pady=(0, 10))
        ctk.CTkLabel(opts, text="Pull mode", width=80, anchor="w").pack(side="left")
        ctk.CTkComboBox(
            opts,
            variable=self.git_pull_mode_var,
            values=["ff-only", "rebase", "merge", "reset"],
            width=120,
            height=28,
        ).pack(side="left", padx=(0, 12))
        ctk.CTkLabel(opts, text="If dirty", width=70, anchor="w").pack(side="left")
        ctk.CTkComboBox(
            opts,
            variable=self.git_pull_dirty_var,
            values=["fail", "stash", "discard"],
            width=110,
            height=28,
        ).pack(side="left", padx=(0, 12))
        ctk.CTkSwitch(
            opts, text="Dry-run", variable=self.dry_run_var, width=100
        ).pack(side="left")
        ctk.CTkLabel(opts, text="Parallel", width=70, anchor="e").pack(
            side="left", padx=(12, 4)
        )
        ctk.CTkOptionMenu(
            opts,
            variable=self.bulk_jobs_var,
            values=["1", "2", "3", "4"],
            width=64,
            height=28,
            command=self._on_bulk_jobs_changed,
        ).pack(side="left")

        tgt = ctk.CTkFrame(head, fg_color="transparent")
        tgt.pack(fill="x", padx=12, pady=(0, 10))
        ctk.CTkLabel(tgt, text="Targets", width=80, anchor="w").pack(side="left")
        ctk.CTkCheckBox(
            tgt, text="Game root", variable=self.bulk_tgt_game_var, width=100
        ).pack(side="left")
        ctk.CTkCheckBox(
            tgt, text="Modules", variable=self.bulk_tgt_modules_var, width=90
        ).pack(side="left")
        ctk.CTkCheckBox(
            tgt, text="psxrecomp", variable=self.bulk_tgt_psx_var, width=100
        ).pack(side="left")
        ctk.CTkCheckBox(
            tgt, text="Nested libs", variable=self.bulk_tgt_nested_var, width=110
        ).pack(side="left")
        ctk.CTkButton(
            tgt,
            text="Game only",
            width=90,
            height=26,
            command=self._bulk_targets_game_only,
        ).pack(side="left", padx=(16, 4))
        ctk.CTkButton(
            tgt,
            text="Engine libs",
            width=100,
            height=26,
            command=self._bulk_targets_engine,
        ).pack(side="left")

        br = ctk.CTkFrame(head, fg_color="transparent")
        br.pack(fill="x", padx=12, pady=(0, 4))
        ctk.CTkLabel(br, text="Branches", width=80, anchor="w").pack(side="left")
        ctk.CTkLabel(br, text="game", width=40, anchor="e").pack(side="left")
        self.bulk_game_branch_menu = ctk.CTkComboBox(
            br,
            variable=self.bulk_game_branch_var,
            values=["(default)", "main", "master"],
            width=120,
            height=28,
        )
        self.bulk_game_branch_menu.pack(side="left", padx=(4, 8))
        ctk.CTkLabel(br, text="psx", width=28, anchor="e").pack(side="left")
        self.bulk_psx_branch_menu = ctk.CTkComboBox(
            br,
            variable=self.bulk_psx_branch_var,
            values=["(default)", "master", "feat/rbengine"],
            width=130,
            height=28,
        )
        self.bulk_psx_branch_menu.pack(side="left", padx=(4, 8))
        ctk.CTkLabel(br, text="ui", width=22, anchor="e").pack(side="left")
        self.bulk_ui_branch_menu = ctk.CTkComboBox(
            br,
            variable=self.bulk_ui_branch_var,
            values=["(default)", "master"],
            width=120,
            height=28,
        )
        self.bulk_ui_branch_menu.pack(side="left", padx=(4, 0))

        br2 = ctk.CTkFrame(head, fg_color="transparent")
        br2.pack(fill="x", padx=12, pady=(0, 10))
        ctk.CTkLabel(br2, text="", width=80, anchor="w").pack(side="left")
        ctk.CTkLabel(br2, text="net", width=40, anchor="e").pack(side="left")
        self.bulk_net_branch_menu = ctk.CTkComboBox(
            br2,
            variable=self.bulk_net_branch_var,
            values=["(default)", "main"],
            width=120,
            height=28,
        )
        self.bulk_net_branch_menu.pack(side="left", padx=(4, 8))
        ctk.CTkLabel(br2, text="rb", width=28, anchor="e").pack(side="left")
        self.bulk_rb_branch_menu = ctk.CTkComboBox(
            br2,
            variable=self.bulk_rb_branch_var,
            values=["(default)", "main"],
            width=130,
            height=28,
        )
        self.bulk_rb_branch_menu.pack(side="left", padx=(4, 8))
        ctk.CTkSwitch(
            br2,
            text="Create branch",
            variable=self.git_create_branch_var,
            width=120,
        ).pack(side="left", padx=(8, 0))
        ctk.CTkSwitch(
            br2,
            text="Set tracking",
            variable=self.bulk_set_tracking_var,
            width=120,
        ).pack(side="left", padx=(8, 0))
        ctk.CTkButton(
            br2,
            text="Fetch branches",
            width=120,
            height=28,
            command=self._bulk_fetch_branches,
        ).pack(side="left", padx=(12, 0))

        br_help = ctk.CTkFrame(head, fg_color="transparent")
        br_help.pack(fill="x", padx=12, pady=(0, 10))
        ctk.CTkLabel(
            br_help,
            text="Branch switch: enable Targets (Game / Modules / Nested), pick "
            "branches, then Switch branches. Fetch loads remote heads + "
            "union of selected game branches.",
            text_color=("gray40", "gray65"),
            anchor="w",
            wraplength=980,
        ).pack(side="left", fill="x", expand=True)
        ctk.CTkLabel(
            br_help,
            textvariable=self.bulk_branch_status_var,
            text_color=("gray40", "gray65"),
            anchor="e",
        ).pack(side="right", padx=(8, 0))

        list_wrap = ctk.CTkFrame(tab, corner_radius=10)
        list_wrap.pack(fill="both", expand=True, padx=4, pady=4)
        list_head = ctk.CTkFrame(list_wrap, fg_color="transparent")
        list_head.pack(fill="x", padx=12, pady=(10, 4))
        ctk.CTkLabel(
            list_head,
            text="Indexed repos",
            font=ctk.CTkFont(size=13, weight="bold"),
        ).pack(side="left")
        ctk.CTkButton(
            list_head, text="All", width=60, height=28, command=self._bulk_select_all
        ).pack(side="right")
        ctk.CTkButton(
            list_head,
            text="None",
            width=60,
            height=28,
            command=self._bulk_select_none,
        ).pack(side="right", padx=(0, 6))
        ctk.CTkButton(
            list_head,
            text="Catalog",
            width=80,
            height=28,
            command=self._bulk_select_catalog,
        ).pack(side="right", padx=(0, 6))
        ctk.CTkButton(
            list_head,
            text="Cat+contrib",
            width=110,
            height=28,
            command=self._bulk_select_catalog_contributor,
        ).pack(side="right", padx=(0, 6))
        ctk.CTkButton(
            list_head,
            text="Contributor",
            width=100,
            height=28,
            command=self._bulk_select_contributor,
        ).pack(side="right", padx=(0, 6))
        ctk.CTkButton(
            list_head,
            text="Refresh",
            width=80,
            height=28,
            command=self._bulk_refresh_list,
        ).pack(side="right", padx=(0, 6))

        self.bulk_repo_list = ctk.CTkScrollableFrame(
            list_wrap, fg_color="transparent", height=220
        )
        self.bulk_repo_list.pack(fill="both", expand=True, padx=8, pady=(0, 10))

        actions = ctk.CTkFrame(tab, corner_radius=10)
        actions.pack(fill="x", padx=4, pady=4)
        row = ctk.CTkFrame(actions, fg_color="transparent")
        row.pack(fill="x", padx=12, pady=(10, 6))
        ctk.CTkButton(
            row, text="Status", width=90, height=34, command=self._bulk_status
        ).pack(side="left")
        ctk.CTkButton(
            row, text="Pull", width=90, height=34, command=self._bulk_pull
        ).pack(side="left", padx=8)
        ctk.CTkButton(
            row, text="Push", width=90, height=34, command=self._bulk_push
        ).pack(side="left")
        ctk.CTkButton(
            row,
            text="Switch branches",
            width=140,
            height=34,
            command=self._bulk_switch,
        ).pack(side="left", padx=8)

        crow = ctk.CTkFrame(actions, fg_color="transparent")
        crow.pack(fill="x", padx=12, pady=(0, 8))
        ctk.CTkEntry(
            crow,
            textvariable=self.bulk_msg_var,
            placeholder_text="Commit message for selected targets",
            height=34,
        ).pack(side="left", fill="x", expand=True, padx=(0, 8))
        ctk.CTkButton(
            crow, text="Commit", width=100, height=34, command=self._bulk_commit
        ).pack(side="left")

        rel = ctk.CTkFrame(actions, fg_color="transparent")
        rel.pack(fill="x", padx=12, pady=(0, 4))
        ctk.CTkLabel(rel, text="Release CI", width=80, anchor="w").pack(side="left")
        ctk.CTkEntry(
            rel,
            textvariable=self.release_version_var,
            placeholder_text="version (empty = auto-bump each repo)",
            width=220,
            height=30,
        ).pack(side="left", padx=(0, 8))
        ctk.CTkOptionMenu(
            rel,
            values=["patch", "minor", "major"],
            variable=self.release_bump_var,
            width=90,
            height=30,
        ).pack(side="left", padx=(0, 8))
        ctk.CTkSwitch(
            rel, text="Publish", variable=self.release_publish_var, width=90
        ).pack(side="left", padx=(0, 6))
        ctk.CTkSwitch(
            rel, text="Reuse emitters", variable=self.release_reuse_var, width=120
        ).pack(side="left")

        relb = ctk.CTkFrame(actions, fg_color="transparent")
        relb.pack(fill="x", padx=12, pady=(0, 12))
        ctk.CTkButton(
            relb,
            text="Run release CI",
            width=140,
            height=34,
            fg_color=("#c0392b", "#922b21"),
            hover_color=("#a93226", "#7b241c"),
            command=self._bulk_release,
        ).pack(side="left")
        ctk.CTkButton(
            relb,
            text="Install & push CI",
            width=150,
            height=34,
            command=self._bulk_install_ci,
        ).pack(side="left", padx=8)
        ctk.CTkLabel(
            relb,
            text="Dispatches release.yml per selected game repo (gh). "
            "Empty version auto-bumps independently.",
            text_color=("gray40", "gray65"),
            anchor="w",
        ).pack(side="left", fill="x", expand=True)

    def _build_build_tab(self, tab) -> None:
        from .buildops import (
            default_generator,
            detect_host,
            find_runtime_exe,
            resolve_build_dir,
        )

        ctk = self.ctk
        host = detect_host()
        if not self.build_generator_var.get().strip():
            self.build_generator_var.set(default_generator(host))
        if not self.build_jobs_var.get().strip():
            self.build_jobs_var.set(str(host.jobs))

        top = ctk.CTkFrame(tab, corner_radius=10)
        top.pack(fill="x", padx=4, pady=4)
        head = ctk.CTkFrame(top, fg_color="transparent")
        head.pack(fill="x", padx=12, pady=(10, 4))
        ctk.CTkLabel(
            head,
            text=f"Local CMake build  ·  host OS: {host.label} ({host.system})",
            font=ctk.CTkFont(size=13, weight="bold"),
        ).pack(side="left")
        ctk.CTkButton(
            head, text="Refresh exe", width=110, height=30, command=self._build_refresh_exe
        ).pack(side="right")

        cmake_note = "cmake: OK" if host.cmake else "cmake: MISSING"
        ninja_note = "ninja: OK" if host.ninja else "ninja: (optional)"
        ctk.CTkLabel(
            top,
            text=f"{cmake_note}  ·  {ninja_note}  ·  default jobs={host.jobs}",
            text_color=("gray30", "gray70"),
            anchor="w",
        ).pack(fill="x", padx=12, pady=(0, 6))
        ctk.CTkLabel(
            top,
            textvariable=self.build_status_var,
            text_color=("gray30", "gray70"),
            anchor="w",
            wraplength=980,
        ).pack(fill="x", padx=12, pady=(0, 8))

        cfg = ctk.CTkFrame(tab, corner_radius=10)
        cfg.pack(fill="x", padx=4, pady=4)
        ctk.CTkLabel(
            cfg,
            text="Configure  ·  cmake -S . -B <dir>",
            font=ctk.CTkFont(size=13, weight="bold"),
        ).pack(anchor="w", padx=12, pady=(10, 4))

        row1 = ctk.CTkFrame(cfg, fg_color="transparent")
        row1.pack(fill="x", padx=12, pady=4)
        ctk.CTkLabel(row1, text="Build dir", width=90, anchor="w").pack(side="left")
        ctk.CTkEntry(row1, textvariable=self.build_dir_var, width=160, height=30).pack(
            side="left", padx=(0, 12)
        )
        ctk.CTkLabel(row1, text="Type", width=50, anchor="w").pack(side="left")
        ctk.CTkOptionMenu(
            row1,
            values=["Release", "RelWithDebInfo", "Debug", "MinSizeRel"],
            variable=self.build_type_var,
            width=140,
            height=30,
        ).pack(side="left", padx=(0, 12))
        ctk.CTkLabel(row1, text="Generator", width=80, anchor="w").pack(side="left")
        gens: list[str] = []
        if host.ninja:
            gens.append("Ninja")
        if host.label == "windows":
            gens.extend(["", "Ninja", "Visual Studio 17 2022", "NMake Makefiles"])
        else:
            gens.extend(["Ninja", "Unix Makefiles", ""])
        seen: set[str] = set()
        gen_values: list[str] = []
        for g in gens:
            key = g if g else "(default)"
            if key in seen:
                continue
            seen.add(key)
            gen_values.append(g if g else "(default)")
        display_gen = self.build_generator_var.get() or "(default)"
        if display_gen not in gen_values and display_gen != "(default)":
            gen_values.insert(0, display_gen)
        self._build_gen_display = tk.StringVar(
            value=display_gen if display_gen in gen_values else gen_values[0]
        )
        ctk.CTkOptionMenu(
            row1,
            values=gen_values or ["(default)"],
            variable=self._build_gen_display,
            width=180,
            height=30,
        ).pack(side="left")

        row2 = ctk.CTkFrame(cfg, fg_color="transparent")
        row2.pack(fill="x", padx=12, pady=4)
        ctk.CTkLabel(row2, text="Extra cmake", width=90, anchor="w").pack(side="left")
        ctk.CTkEntry(
            row2,
            textvariable=self.build_extra_cmake_var,
            placeholder_text="-DMOTK_NATIVE=ON -DPSX_NETPLAY=ON …",
            height=30,
        ).pack(side="left", fill="x", expand=True, padx=(0, 8))
        ctk.CTkButton(
            row2, text="Configure", width=110, height=30, command=self._build_configure
        ).pack(side="left", padx=(0, 8))
        ctk.CTkButton(
            row2,
            text="Ensure OpenBIOS",
            width=140,
            height=30,
            command=self._build_ensure_bios,
        ).pack(side="left")

        ctk.CTkLabel(
            cfg,
            text=(
                "Configure auto-regens missing OpenBIOS under psxrecomp/generated "
                "(bundled MIT). Use Ensure OpenBIOS to force regen; SCPH1001 only "
                "if bios/SCPH1001.BIN is present."
            ),
            text_color=("gray30", "gray70"),
            anchor="w",
            wraplength=980,
            justify="left",
        ).pack(fill="x", padx=12, pady=(0, 10))

        build_box = ctk.CTkFrame(tab, corner_radius=10)
        build_box.pack(fill="x", padx=4, pady=4)
        ctk.CTkLabel(
            build_box,
            text="Build  ·  cmake --build",
            font=ctk.CTkFont(size=13, weight="bold"),
        ).pack(anchor="w", padx=12, pady=(10, 4))
        brow = ctk.CTkFrame(build_box, fg_color="transparent")
        brow.pack(fill="x", padx=12, pady=(0, 10))
        ctk.CTkLabel(brow, text="Target", width=90, anchor="w").pack(side="left")
        ctk.CTkEntry(brow, textvariable=self.build_target_var, width=140, height=30).pack(
            side="left", padx=(0, 12)
        )
        ctk.CTkLabel(brow, text="Jobs", width=50, anchor="w").pack(side="left")
        ctk.CTkEntry(brow, textvariable=self.build_jobs_var, width=70, height=30).pack(
            side="left", padx=(0, 12)
        )
        ctk.CTkButton(
            brow, text="Build", width=100, height=30, command=self._build_run_build
        ).pack(side="left")
        ctk.CTkButton(
            brow,
            text="Configure + Build",
            width=150,
            height=30,
            command=self._build_configure_and_build,
        ).pack(side="left", padx=8)
        # Local export runs the SAME wrapper CI runs, so a zip made here and a
        # released one are the same artifact by construction. It exists because
        # "build it and look inside the package" had no button: the only route
        # to a distributable was commit-and-wait-for-CI, which is how releases
        # shipped with an empty Mods page without anyone noticing.
        ctk.CTkButton(
            brow,
            text="Export release zip",
            width=160,
            height=30,
            command=self._build_export,
        ).pack(side="left")

        launch_box = ctk.CTkFrame(tab, corner_radius=10)
        launch_box.pack(fill="x", padx=4, pady=4)
        ctk.CTkLabel(
            launch_box,
            text="Launch  ·  local product binary + env",
            font=ctk.CTkFont(size=13, weight="bold"),
        ).pack(anchor="w", padx=12, pady=(10, 4))

        erow = ctk.CTkFrame(launch_box, fg_color="transparent")
        erow.pack(fill="x", padx=12, pady=4)
        ctk.CTkLabel(erow, text="Executable", width=90, anchor="w").pack(side="left")
        ctk.CTkEntry(
            erow,
            textvariable=self.build_exe_var,
            placeholder_text="(auto-detect after build)",
            height=30,
        ).pack(side="left", fill="x", expand=True, padx=(0, 8))

        arow = ctk.CTkFrame(launch_box, fg_color="transparent")
        arow.pack(fill="x", padx=12, pady=4)
        ctk.CTkLabel(arow, text="Args", width=90, anchor="w").pack(side="left")
        ctk.CTkEntry(
            arow,
            textvariable=self.build_launch_args_var,
            placeholder_text="optional CLI args for the game",
            height=30,
        ).pack(side="left", fill="x", expand=True)

        ctk.CTkLabel(
            launch_box,
            text="Environment variables (KEY=VALUE …)",
            anchor="w",
        ).pack(fill="x", padx=12, pady=(6, 2))
        self.build_env_box = ctk.CTkTextbox(launch_box, height=100, font=ctk.CTkFont(size=12))
        self.build_env_box.pack(fill="x", padx=12, pady=(0, 6))
        self.build_env_box.insert("1.0", self._build_env_default)

        lrow = ctk.CTkFrame(launch_box, fg_color="transparent")
        lrow.pack(fill="x", padx=12, pady=(0, 12))
        ctk.CTkButton(
            lrow,
            text="Launch",
            width=110,
            height=34,
            fg_color=("#2e7d32", "#1b5e20"),
            hover_color=("#256628", "#14401a"),
            command=self._build_launch,
        ).pack(side="left")
        ctk.CTkButton(
            lrow, text="Stop", width=90, height=34, command=self._build_stop
        ).pack(side="left", padx=8)
        ctk.CTkSwitch(
            lrow, text="Dry-run", variable=self.dry_run_var, width=100
        ).pack(side="left", padx=12)

        try:
            root_s = self.root_var.get().strip()
            if root_s:
                bdir = resolve_build_dir(
                    Path(root_s), self.build_dir_var.get().strip() or "build-release"
                )
                exe = find_runtime_exe(bdir)
                if exe:
                    self.build_exe_var.set(str(exe))
                    self.build_status_var.set(f"Found {exe.name} under {bdir.name}")
        except Exception:
            pass

    def _build_generator_value(self) -> str:
        raw = ""
        if hasattr(self, "_build_gen_display"):
            raw = self._build_gen_display.get().strip()
        else:
            raw = self.build_generator_var.get().strip()
        if not raw or raw == "(default)":
            return ""
        self.build_generator_var.set(raw)
        return raw

    def _build_extra_args(self) -> list[str]:
        import shlex

        raw = self.build_extra_cmake_var.get().strip()
        if not raw:
            return []
        try:
            return shlex.split(raw, posix=os.name != "nt")
        except ValueError:
            return raw.split()

    def _build_jobs(self) -> int | None:
        raw = self.build_jobs_var.get().strip()
        if not raw:
            return None
        try:
            n = int(raw)
            return n if n > 0 else None
        except ValueError:
            return None

    def _build_env_text(self) -> str:
        if hasattr(self, "build_env_box") and self.build_env_box is not None:
            try:
                return self.build_env_box.get("1.0", "end")
            except tk.TclError:
                return ""
        return ""

    def _build_run_bg(self, label: str, fn) -> None:
        if self._build_busy:
            messagebox.showinfo(
                "Project Studio",
                "A build operation is already running.",
                parent=self.root,
            )
            return

        def worker() -> None:
            self._build_busy = True
            try:
                self.root.after(0, lambda: self.build_status_var.set(f"{label}…"))
                r = fn()
                self.root.after(0, lambda: self._log_cmd(r))
                self.root.after(
                    0,
                    lambda: self.build_status_var.set(
                        f"{'OK' if r.ok else 'FAIL'}: {r.message}"
                    ),
                )
                if r.ok:
                    self.root.after(0, self._build_refresh_exe)
            finally:
                self._build_busy = False

        threading.Thread(target=worker, daemon=True).start()

    def _build_configure(self) -> None:
        from .buildops import configure

        root = self._game_root()
        if root is None:
            return

        def go():
            return configure(
                root,
                build_dir=self.build_dir_var.get().strip() or "build-release",
                build_type=self.build_type_var.get().strip() or "Release",
                generator=self._build_generator_value(),
                extra_args=self._build_extra_args(),
                dry_run=self._git_dry(),
                log=lambda m: self.root.after(0, lambda line=m: self._log(line)),
            )

        self._build_run_bg("Configure", go)

    def _build_ensure_bios(self) -> None:
        from .buildops import ensure_bios_backends

        root = self._game_root()
        if root is None:
            return

        def go():
            return ensure_bios_backends(
                root,
                force=True,
                dry_run=self._git_dry(),
                log=lambda m: self.root.after(0, lambda line=m: self._log(line)),
            )

        self._build_run_bg("Ensure OpenBIOS", go)

    def _build_run_build(self) -> None:
        from .buildops import build

        root = self._game_root()
        if root is None:
            return

        def go():
            return build(
                root,
                build_dir=self.build_dir_var.get().strip() or "build-release",
                target=self.build_target_var.get().strip() or "psx-runtime",
                jobs=self._build_jobs(),
                dry_run=self._git_dry(),
                log=lambda m: self.root.after(0, lambda line=m: self._log(line)),
            )

        self._build_run_bg("Build", go)

    def _build_export(self) -> None:
        """Bundle a distributable zip from the current build (mods included)."""
        from .buildops import export_release

        root = self._game_root()
        if root is None:
            return

        def go():
            return export_release(
                root,
                build_dir=self.build_dir_var.get().strip() or "build-release",
                # Local export keeps developer-channel mods: the reason to
                # export locally is to test what you are working on. CI drops
                # them; `build export --exclude-dev-mods` reproduces that.
                exclude_dev_mods=False,
                log=lambda m: self.root.after(0, lambda line=m: self._log(line)),
            )

        self._build_run_bg("Export release", go)

    def _build_configure_and_build(self) -> None:
        from .buildops import build, configure

        root = self._game_root()
        if root is None:
            return

        def go():
            r = configure(
                root,
                build_dir=self.build_dir_var.get().strip() or "build-release",
                build_type=self.build_type_var.get().strip() or "Release",
                generator=self._build_generator_value(),
                extra_args=self._build_extra_args(),
                dry_run=self._git_dry(),
                log=lambda m: self.root.after(0, lambda line=m: self._log(line)),
            )
            if not r.ok:
                return r
            self.root.after(0, lambda: self._log_cmd(r))
            return build(
                root,
                build_dir=self.build_dir_var.get().strip() or "build-release",
                target=self.build_target_var.get().strip() or "psx-runtime",
                jobs=self._build_jobs(),
                dry_run=self._git_dry(),
                log=lambda m: self.root.after(0, lambda line=m: self._log(line)),
            )

        self._build_run_bg("Configure + Build", go)

    def _build_refresh_exe(self) -> None:
        from .buildops import find_runtime_exe, launch_status, resolve_build_dir

        root_s = self.root_var.get().strip()
        if not root_s:
            return
        root = Path(root_s).expanduser().resolve()
        bdir = resolve_build_dir(
            root, self.build_dir_var.get().strip() or "build-release"
        )
        exe = find_runtime_exe(bdir)
        if exe:
            self.build_exe_var.set(str(exe))
            self.build_status_var.set(f"{exe}  ·  {launch_status()}")
            self._log(f"Runtime exe: {exe}")
            self._save_build_settings_for_current()
        else:
            self.build_exe_var.set("")
            self.build_status_var.set(f"No exe under {bdir}  ·  {launch_status()}")
            self._save_build_settings_for_current()

    def _build_launch(self) -> None:
        from .buildops import launch

        root = self._game_root()
        if root is None:
            return
        import shlex

        args_raw = self.build_launch_args_var.get().strip()
        try:
            extra = shlex.split(args_raw, posix=os.name != "nt") if args_raw else []
        except ValueError:
            extra = args_raw.split()
        exe_s = self.build_exe_var.get().strip()
        r = launch(
            root,
            build_dir=self.build_dir_var.get().strip() or "build-release",
            exe=Path(exe_s) if exe_s else None,
            env_text=self._build_env_text(),
            extra_args=extra,
            dry_run=self._git_dry(),
            log=self._log,
        )
        self._log_cmd(r)
        self.build_status_var.set(r.message)
        self._save_build_settings_for_current()

    def _build_stop(self) -> None:
        from .buildops import stop_launch

        r = stop_launch()
        self._log_cmd(r)
        self.build_status_var.set(r.message)

    def _save_build_settings_for_current(self) -> None:
        """Persist Build-tab fields into the repo index for the active project."""
        from .repo_index import set_repo_build

        idx = self._repo_index
        root_s = (self._build_settings_root or self.root_var.get()).strip()
        if idx is None or not root_s:
            return
        if idx.find(root_s) is None:
            return
        gen = ""
        if hasattr(self, "_build_gen_display") and self._build_gen_display is not None:
            gen = self._build_gen_display.get().strip()
            if gen == "(default)":
                gen = ""
        else:
            gen = self.build_generator_var.get().strip()
        settings = {
            "build_dir": self.build_dir_var.get().strip() or "build-release",
            "build_type": self.build_type_var.get().strip() or "Release",
            "generator": gen,
            "target": self.build_target_var.get().strip() or "psx-runtime",
            "jobs": self.build_jobs_var.get().strip(),
            "extra_cmake": self.build_extra_cmake_var.get().strip(),
            "exe": self.build_exe_var.get().strip(),
            "launch_args": self.build_launch_args_var.get().strip(),
            "env": self._build_env_text().strip(),
        }
        set_repo_build(idx, root_s, settings)

    def _clear_build_fields(self) -> None:
        self.build_dir_var.set("build-release")
        self.build_type_var.set("Release")
        self.build_target_var.set("psx-runtime")
        self.build_generator_var.set("")
        if hasattr(self, "_build_gen_display") and self._build_gen_display is not None:
            self._build_gen_display.set("(default)")
        self.build_jobs_var.set("")
        self.build_extra_cmake_var.set("")
        self.build_exe_var.set("")
        self.build_launch_args_var.set("")
        self.build_status_var.set("Open a game repo to build.")
        if hasattr(self, "build_env_box") and self.build_env_box is not None:
            try:
                self.build_env_box.delete("1.0", "end")
                self.build_env_box.insert("1.0", self._build_env_default)
            except tk.TclError:
                pass

    def _load_build_settings_for(self, root_path: str) -> None:
        """Restore Build-tab fields for ``root_path`` (or clear + discover exe)."""
        from .buildops import find_runtime_exe, resolve_build_dir
        from .repo_index import get_repo_build

        self._save_build_settings_for_current()
        self._clear_build_fields()
        self._build_settings_root = ""
        path = (root_path or "").strip()
        if not path:
            return
        self._build_settings_root = path
        idx = self._repo_index
        settings = get_repo_build(idx, path) if idx is not None else {}
        if settings.get("build_dir"):
            self.build_dir_var.set(str(settings["build_dir"]))
        if settings.get("build_type"):
            self.build_type_var.set(str(settings["build_type"]))
        if settings.get("target"):
            self.build_target_var.set(str(settings["target"]))
        if "jobs" in settings and settings["jobs"] is not None:
            self.build_jobs_var.set(str(settings["jobs"]))
        if settings.get("extra_cmake"):
            self.build_extra_cmake_var.set(str(settings["extra_cmake"]))
        gen = str(settings.get("generator") or "").strip()
        self.build_generator_var.set(gen)
        if hasattr(self, "_build_gen_display") and self._build_gen_display is not None:
            self._build_gen_display.set(gen if gen else "(default)")
        if settings.get("launch_args"):
            self.build_launch_args_var.set(str(settings["launch_args"]))
        env = str(settings.get("env") or "").strip()
        if hasattr(self, "build_env_box") and self.build_env_box is not None:
            try:
                self.build_env_box.delete("1.0", "end")
                self.build_env_box.insert("1.0", env if env else self._build_env_default)
            except tk.TclError:
                pass
        exe_s = str(settings.get("exe") or "").strip()
        if exe_s and Path(exe_s).is_file():
            self.build_exe_var.set(exe_s)
            self.build_status_var.set(f"Restored exe for this project: {Path(exe_s).name}")
        else:
            try:
                root = Path(path).expanduser().resolve()
                bdir = resolve_build_dir(
                    root, self.build_dir_var.get().strip() or "build-release"
                )
                exe = find_runtime_exe(bdir)
                if exe:
                    self.build_exe_var.set(str(exe))
                    self.build_status_var.set(f"Found {exe.name} under {bdir.name}")
                else:
                    self.build_exe_var.set("")
                    self.build_status_var.set(f"No exe under {bdir.name} yet")
            except OSError:
                self.build_exe_var.set("")
                self.build_status_var.set("Select a project to build.")
        self._save_build_settings_for_current()

    def _game_root(self) -> Path | None:
        root_s = self.root_var.get().strip()
        if not root_s:
            messagebox.showerror("Project Studio", "Choose a game repo root.", parent=self.root)
            return None
        root = Path(root_s).expanduser().resolve()
        if not root.is_dir():
            messagebox.showerror(
                "Project Studio", f"Not a directory:\n{root}", parent=self.root
            )
            return None
        return root

    def _git_dry(self) -> bool:
        return bool(self.dry_run_var.get())

    def _log_textbox(self):
        return getattr(self.log, "_textbox", self.log)

    def _setup_log_tags(self) -> None:
        tb = self._log_textbox()
        try:
            tb.tag_configure("ok", foreground=_LOG_OK)
            tb.tag_configure("warn", foreground=_LOG_WARN)
            tb.tag_configure("error", foreground=_LOG_ERROR)
            tb.tag_configure("info", foreground=_LOG_INFO)
        except tk.TclError:
            pass

    @staticmethod
    def _log_tag_for(msg: str) -> str:
        s = (msg or "").strip()
        low = s.lower()
        if s.startswith("[FAIL]") or low.startswith("error:") or " error:" in low:
            return "error"
        if s.startswith("[OK]"):
            return "ok"
        if (
            low.startswith("warning")
            or low.startswith("warn:")
            or "warning:" in low
            or low.startswith("[warn]")
        ):
            return "warn"
        return "info"

    def _log_cmd(self, r) -> None:
        tag = "ok" if getattr(r, "ok", False) else "error"
        self._log(f"[{'OK' if r.ok else 'FAIL'}] {r.message}", tag=tag)
        if r.detail:
            for line in str(r.detail).splitlines()[:20]:
                self._log(f"  {line}", tag=tag)

    def _browse_root(self) -> None:
        """Legacy alias — Add uses the native picker and indexes the path."""
        self._repo_add()

    def _repo_index_load(self, *, initial_root: Path | None = None) -> None:
        from .repo_index import add_repo, load_index, looks_like_game_repo

        idx = load_index()
        self._repo_index = idx
        self._log_height = int(getattr(idx, "log_height", 0) or _DEFAULT_LOG_HEIGHT)
        self.catalog_only_var.set(bool(getattr(idx, "catalog_only", False)))
        jobs = int(getattr(idx, "bulk_jobs", 2) or 2)
        if jobs < 1:
            jobs = 1
        if jobs > 4:
            jobs = 4
        self.bulk_jobs_var.set(str(jobs))
        self.root.after(120, self._apply_log_sash)
        chosen = ""
        if initial_root is not None:
            root = initial_root.expanduser().resolve()
            if looks_like_game_repo(root) or root.is_dir():
                add_repo(idx, root)
                chosen = str(root)
        if not chosen and idx.last and any(r.path == idx.last for r in idx.repos):
            chosen = idx.last
        if not chosen and idx.repos:
            chosen = idx.repos[0].path
        self._repo_refresh_menu(select_path=chosen or None)

    def _visible_repo_entries(self):
        """Indexed repos for the Game-repo dropdown (optional catalog filter)."""
        from .bulkops import repo_has_catalog_entry, resolve_catalog_root, find_studio_toml
        from .repo_index import RepoEntry

        idx = self._repo_index
        if idx is None:
            return []
        entries: list[RepoEntry] = list(idx.repos)
        if not self.catalog_only_var.get():
            return entries
        studio = find_studio_toml()
        cat = resolve_catalog_root(studio)
        return [
            e
            for e in entries
            if repo_has_catalog_entry(e, catalog_root=cat, studio_toml=studio)
        ]

    def _on_catalog_only_toggle(self) -> None:
        from .repo_index import save_index

        idx = self._repo_index
        if idx is not None:
            idx.catalog_only = bool(self.catalog_only_var.get())
            save_index(idx)
        self._repo_refresh_menu(select_path=self.root_var.get().strip() or None)
        on = self.catalog_only_var.get()
        visible = self._visible_repo_entries()
        total = len(idx.repos) if idx is not None else 0
        if on:
            self._log(f"Catalog only: showing {len(visible)} of {total} indexed repos")
        else:
            self._log(f"Showing all {total} indexed repos")

    def _apply_repo_cue(self, root_path: str | None = None) -> None:
        """Load indexed / discovered .cue into the Migrate disc field."""
        from .repo_index import discover_cue

        idx = self._repo_index
        path = (root_path or self.root_var.get()).strip()
        if not path:
            self.disc_var.set("")
            return
        cue = ""
        if idx is not None:
            entry = idx.find(path)
            if entry is not None:
                if entry.cue:
                    cue = entry.cue
                else:
                    cue = discover_cue(Path(path))
                    if cue:
                        from .repo_index import set_repo_cue

                        set_repo_cue(idx, path, cue)
                        self._log(f"Indexed disc .cue: {cue}")
        if not cue:
            cue = discover_cue(Path(path))
        self.disc_var.set(cue if cue and Path(cue).is_file() else (cue or ""))
        if self.disc_var.get().strip():
            self.probe_var.set(True)

    def _apply_repo_players(self, root_path: str | None = None) -> None:
        """Load Migrate players + toggles from the selected game repo."""
        self._apply_repo_migrate_settings(root_path)

    def _on_players_changed(self, value: str) -> None:
        """When netplay isn't already in the project, default it from player count."""
        if self._netplay_detected:
            return
        try:
            n = int(value or "2")
        except ValueError:
            n = 2
        self.netplay_var.set(n >= 2)

    def _apply_repo_migrate_settings(self, root_path: str | None = None) -> None:
        """Set Players / Netplay / CI / Probe from existing project settings."""
        from .naming import (
            ci_workflow_present,
            configured_players,
            disc_probe_configured,
            netplay_configured,
        )

        path = (root_path or self.root_var.get()).strip()
        if not path:
            self.players_var.set("2")
            self._netplay_detected = False
            self.netplay_var.set(True)  # default players=2 → netplay on
            self.ci_var.set(True)
            self.probe_var.set(False)
            return

        root = Path(path)
        n = configured_players(root, default=2)
        self.players_var.set(str(n))

        detected_netplay = netplay_configured(root)
        self._netplay_detected = detected_netplay
        if detected_netplay:
            self.netplay_var.set(True)
        else:
            # Not configured yet: enable by default for multiplayer titles.
            self.netplay_var.set(n >= 2)

        # Mirror existing release.yml. Turn on manually if you want migrate to emit one.
        self.ci_var.set(ci_workflow_present(root))

        # Probe: on when cue is selected or disc identity already exists.
        has_cue = bool(self.disc_var.get().strip())
        self.probe_var.set(has_cue or disc_probe_configured(root))

    def _repo_refresh_menu(self, *, select_path: str | None = None) -> None:
        from .repo_index import labels_for_repos, path_for_label, set_last

        idx = self._repo_index
        if idx is None:
            return
        entries = self._visible_repo_entries()
        self._repo_menu_entries = entries
        labels = labels_for_repos(entries)
        if not labels:
            empty = (
                "(no catalog matches…)"
                if self.catalog_only_var.get() and idx.repos
                else "(add a repo…)"
            )
            labels = [empty]
            self.repo_menu.configure(values=labels)
            self.repo_label_var.set(labels[0])
            self.root_var.set("")
            self.disc_var.set("")
            self._apply_repo_migrate_settings("")
            self._load_build_settings_for("")
            self._bulk_refresh_list()
            return
        self.repo_menu.configure(values=labels)
        pick = select_path or self.root_var.get().strip() or idx.last
        label = labels[0]
        if pick:
            try:
                pick_res = str(Path(pick).expanduser().resolve())
            except OSError:
                pick_res = pick
            for lab, entry in zip(labels, entries):
                if entry.path == pick or entry.path == pick_res:
                    label = lab
                    break
        self.repo_label_var.set(label)
        path = path_for_label(idx, label, repos=entries)
        if path:
            self.root_var.set(path)
            set_last(idx, path)
            self._apply_repo_cue(path)
            self._apply_repo_players(path)
            self._load_build_settings_for(path)
        self._bulk_refresh_list()

    def _on_repo_selected(self, label: str) -> None:
        from .repo_index import path_for_label, set_last

        idx = self._repo_index
        if idx is None:
            return
        if label.startswith("("):
            return
        path = path_for_label(idx, label, repos=self._repo_menu_entries or None)
        if not path:
            return
        self.root_var.set(path)
        set_last(idx, path)
        self._apply_repo_cue(path)
        self._apply_repo_players(path)
        self._load_build_settings_for(path)
        self._log(f"Selected repo: {path}")
        self.refresh_audit()

    def _repo_add(self) -> None:
        from .repo_index import add_repo, load_index, looks_like_game_repo

        idx = self._repo_index
        if idx is None:
            idx = load_index()
            self._repo_index = idx
        start = self.root_var.get().strip() or None
        path = _pick_directory(
            title="Add game repository root",
            parent=self.root,
            initialdir=start,
        )
        if not path:
            return
        root = Path(path).expanduser().resolve()
        if not root.is_dir():
            messagebox.showerror(
                "Project Studio", f"Not a directory:\n{root}", parent=self.root
            )
            return
        if not looks_like_game_repo(root):
            if not messagebox.askyesno(
                "Project Studio",
                f"This folder does not look like a game repo "
                f"(no game.toml / CMakeLists+psxrecomp):\n{root}\n\n"
                "Add it anyway?",
                parent=self.root,
            ):
                return
        entry = add_repo(idx, root)
        self._log(f"Indexed repo: {entry.name} → {entry.path}")
        self._repo_refresh_menu(select_path=entry.path)
        self.refresh_audit()

    def _repo_remove(self) -> None:
        from .repo_index import path_for_label, remove_repo

        idx = self._repo_index
        if idx is None or not idx.repos:
            messagebox.showinfo(
                "Project Studio", "No indexed repos to remove.", parent=self.root
            )
            return
        label = self.repo_label_var.get().strip()
        path = path_for_label(idx, label) or self.root_var.get().strip()
        if not path or label.startswith("("):
            messagebox.showinfo(
                "Project Studio", "Select a repo to remove.", parent=self.root
            )
            return
        if not messagebox.askyesno(
            "Project Studio",
            f"Remove from index (does not delete files)?\n\n{path}",
            parent=self.root,
        ):
            return
        remove_repo(idx, path)
        self._log(f"Removed from index: {path}")
        next_path = idx.last or (idx.repos[0].path if idx.repos else None)
        self._repo_refresh_menu(select_path=next_path)
        if self.root_var.get().strip():
            self.refresh_audit()
        else:
            self.status_var.set("Add a game repo to begin.")

    def _browse_disc(self) -> None:
        from .repo_index import set_repo_cue

        start = self.disc_var.get().strip() or self.root_var.get().strip() or None
        if start and Path(start).is_file():
            start = str(Path(start).parent)
        path = _pick_open_file(
            title="Select Redump .cue",
            parent=self.root,
            initialdir=start,
            filetypes=[("Cue sheet", "*.cue"), ("All", "*.*")],
        )
        if path:
            cue = str(Path(path).expanduser().resolve())
            self.disc_var.set(cue)
            self.probe_var.set(True)
            root = self.root_var.get().strip()
            idx = self._repo_index
            if root and idx is not None:
                if idx.find(root) is None:
                    from .repo_index import add_repo

                    add_repo(idx, Path(root), cue=cue)
                else:
                    set_repo_cue(idx, root, cue)
                self._log(f"Indexed disc .cue for repo: {cue}")

    def _clear_disc(self) -> None:
        from .naming import disc_probe_configured
        from .repo_index import clear_repo_cue

        self.disc_var.set("")
        root = self.root_var.get().strip()
        if root and disc_probe_configured(Path(root)):
            self.probe_var.set(True)
        else:
            self.probe_var.set(False)
        idx = self._repo_index
        if root and idx is not None and clear_repo_cue(idx, root):
            self._log("Cleared indexed disc .cue")

    def _migrate_options(self) -> MigrateOptions:
        return MigrateOptions(
            disc=self.disc_var.get().strip() or None,
            players=int(self.players_var.get() or "2"),
            zip_prefix=self.zip_var.get().strip() or None,
            enable_recomp_ui=True,
            enable_wizard=True,
            enable_netplay=bool(self.netplay_var.get()),
            enable_ci=bool(self.ci_var.get()),
            probe_disc=bool(self.probe_var.get()) and bool(self.disc_var.get().strip()),
            record_pins=True,
            dry_run=bool(self.dry_run_var.get()),
            force=bool(self.force_var.get()),
        )

    def _log(self, msg: str, *, tag: str | None = None) -> None:
        level = tag or self._log_tag_for(msg)
        try:
            self.log.insert("end", msg + "\n", level)
        except TypeError:
            self.log.insert("end", msg + "\n")
            tb = self._log_textbox()
            try:
                tb.tag_add(level, "end-2l", "end-1c")
            except tk.TclError:
                pass
        try:
            self.log.see("end")
        except tk.TclError:
            pass

    def _desired_log_height(self) -> int:
        h = int(self._log_height or _DEFAULT_LOG_HEIGHT)
        if h < 100:
            return 100
        if h > 800:
            return 800
        return h

    def _apply_log_sash(self) -> None:
        pane = self._log_pane
        if pane is None:
            return
        try:
            pane.update_idletasks()
            total = int(pane.winfo_height())
        except tk.TclError:
            return
        if total <= 1:
            self.root.after(120, self._apply_log_sash)
            return
        log_h = self._desired_log_height()
        sash_y = total - log_h - 8
        if sash_y < 220:
            sash_y = 220
        try:
            pane.sash_place(0, 0, sash_y)
        except tk.TclError:
            pass

    def _read_log_sash_height(self) -> int | None:
        pane = self._log_pane
        if pane is None:
            return None
        try:
            total = int(pane.winfo_height())
            sash_y = int(pane.sash_coord(0)[1])
        except (tk.TclError, IndexError, TypeError):
            return None
        if total <= 1:
            return None
        h = total - sash_y
        if h < 100:
            h = 100
        if h > 800:
            h = 800
        return h

    def _persist_log_height(self) -> None:
        h = self._read_log_sash_height()
        if h is None:
            return
        self._log_height = h
        idx = self._repo_index
        if idx is None:
            return
        if int(getattr(idx, "log_height", 0) or 0) == h:
            return
        idx.log_height = h
        from .repo_index import save_index

        save_index(idx)

    def _on_log_sash(self, _event=None) -> None:
        if self._log_sash_job is not None:
            try:
                self.root.after_cancel(self._log_sash_job)
            except tk.TclError:
                pass
        self._log_sash_job = self.root.after(200, self._persist_log_height)

    def _on_close(self) -> None:
        try:
            self._persist_log_height()
            self._save_build_settings_for_current()
        except Exception:
            pass
        self.root.destroy()

    def _clear_children(self, frame) -> None:
        for child in frame.winfo_children():
            child.destroy()

    def refresh_audit(self) -> None:
        ctk = self.ctk
        root = self._game_root()
        if root is None:
            return
        # Persist typed/browsed .cue into the repo index when present.
        cue = self.disc_var.get().strip()
        idx = self._repo_index
        if cue and idx is not None and Path(cue).is_file():
            from .repo_index import add_repo, set_repo_cue

            if idx.find(root) is None:
                add_repo(idx, root, cue=cue)
            else:
                entry = idx.find(root)
                if entry is not None and entry.cue != str(Path(cue).resolve()):
                    set_repo_cue(idx, root, cue)
        self._report = audit_project(root)
        self._clear_children(self.audit_list)
        for c in self._report.checks:
            color = _STATUS_COLORS.get(c.status, "#8a9199")
            row = ctk.CTkFrame(self.audit_list, fg_color=("gray90", "gray17"), corner_radius=8)
            row.pack(fill="x", pady=3, padx=2)
            badge = ctk.CTkLabel(
                row,
                text=c.status.value.upper(),
                text_color=color,
                font=ctk.CTkFont(size=11, weight="bold"),
                width=52,
            )
            badge.pack(side="left", padx=(10, 6), pady=8)
            body = ctk.CTkFrame(row, fg_color="transparent")
            body.pack(side="left", fill="x", expand=True, padx=(0, 10), pady=6)
            ctk.CTkLabel(
                body,
                text=c.title,
                font=ctk.CTkFont(size=13, weight="bold"),
                anchor="w",
            ).pack(fill="x")
            detail = c.detail or c.severity.value
            ctk.CTkLabel(
                body,
                text=detail,
                text_color=("gray40", "gray65"),
                font=ctk.CTkFont(size=11),
                anchor="w",
                wraplength=420,
                justify="left",
            ).pack(fill="x")
        self.status_var.set(
            f"Layout: {self._report.layout.value} · boot={self._report.boot_exe or '?'}"
        )
        self._log(f"Audited {root} → {self._report.layout.value}")
        self.refresh_plan()
        self.refresh_git(quiet=True)

    def refresh_plan(self) -> None:
        ctk = self.ctk
        root_s = self.root_var.get().strip()
        if not root_s:
            return
        root = Path(root_s).expanduser().resolve()
        opts = self._migrate_options()
        self._plan = build_plan(root, opts, self._report)
        self._clear_children(self.plan_checks)
        self._step_vars.clear()
        if not self._plan.steps:
            ctk.CTkLabel(
                self.plan_checks,
                text="No migration steps needed.",
                text_color=("gray40", "gray65"),
            ).pack(anchor="w", padx=4, pady=8)
            return
        for step in self._plan.steps:
            var = tk.BooleanVar(value=step.selected)
            self._step_vars[step.op_id] = var
            row = ctk.CTkFrame(self.plan_checks, fg_color=("gray90", "gray17"), corner_radius=8)
            row.pack(fill="x", pady=3, padx=2)
            ctk.CTkCheckBox(row, text="", variable=var, width=28).pack(
                side="left", padx=(10, 4), pady=10
            )
            body = ctk.CTkFrame(row, fg_color="transparent")
            body.pack(side="left", fill="x", expand=True, padx=(0, 10), pady=6)
            ctk.CTkLabel(
                body,
                text=step.title,
                font=ctk.CTkFont(size=13, weight="bold"),
                anchor="w",
            ).pack(fill="x")
            sub = step.op_id if not step.detail else f"{step.op_id}  ·  {step.detail}"
            ctk.CTkLabel(
                body,
                text=sub,
                text_color=("gray40", "gray65"),
                font=ctk.CTkFont(size=11),
                anchor="w",
                wraplength=420,
                justify="left",
            ).pack(fill="x")

    def apply_selected(self) -> None:
        if self._plan is None:
            self.refresh_plan()
        if self._plan is None or not self._plan.steps:
            messagebox.showinfo("Project Studio", "Nothing to apply.", parent=self.root)
            return
        opts = self._migrate_options()
        opts.enable_wizard = True
        opts.enable_recomp_ui = True
        self._plan.options = opts
        selected = [op for op, var in self._step_vars.items() if var.get()]
        if not selected:
            messagebox.showinfo("Project Studio", "No steps selected.", parent=self.root)
            return
        for step in self._plan.steps:
            step.selected = step.op_id in selected

        mode = "DRY-RUN" if opts.dry_run else "APPLY"
        if not opts.dry_run:
            if not messagebox.askyesno(
                "Project Studio",
                f"Apply {len(selected)} step(s) to:\n{self._plan.root}\n\n"
                "A backup CMakeLists.txt.pre_migrate.bak is written when rewriting CMake.",
                parent=self.root,
            ):
                return

        self._log(f"--- {mode} ({len(selected)} ops) ---")
        results = apply_plan(self._plan, selected=selected)
        failed = 0
        for r in results:
            self._log(f"[{'OK' if r.ok else 'FAIL'}] {r.op_id}: {r.message}")
            for p in r.changed_paths:
                self._log(f"  · {p}")
            if not r.ok:
                failed += 1
        self.status_var.set(f"{mode} done — {failed} failed, {len(results) - failed} ok")
        if not opts.dry_run:
            self.refresh_audit()
        if failed:
            messagebox.showwarning(
                "Project Studio",
                f"{failed} step(s) failed — see log.",
                parent=self.root,
            )
        else:
            messagebox.showinfo(
                "Project Studio",
                f"{mode} completed successfully.",
                parent=self.root,
            )

    def refresh_git(self, *, quiet: bool = False) -> None:
        from .gitops import repo_status

        ctk = self.ctk
        root = self._game_root()
        if root is None:
            return
        st = repo_status(root)
        self._git_status = st
        if not st.is_git:
            self.git_summary_var.set("Not a git repository.")
            self._clear_children(self.git_sub_list)
            self._clear_children(self.git_nested_list)
            ctk.CTkLabel(
                self.git_sub_list,
                text="Initialize git in this folder first.",
                text_color=("gray40", "gray65"),
            ).pack(anchor="w", padx=4, pady=8)
            return

        self.git_branch_var.set(st.branch)
        dirty = "dirty" if st.dirty else "clean"
        parts = [
            f"{st.branch or '?'}",
            f"{dirty} (staged={st.staged} unstaged={st.unstaged} untracked={st.untracked})",
            f"ahead={st.ahead} behind={st.behind}",
        ]
        if st.upstream:
            parts.insert(1, f"→ {st.upstream}")
        if st.gh_repo:
            parts.append(f"gh:{st.gh_repo}")
        elif st.remote_url:
            parts.append(st.remote_url)
        if not st.gh_available:
            parts.append("gh CLI missing")
        self.git_summary_var.set("  ·  ".join(parts))

        for s in st.submodules:
            if s.path == "psxrecomp" and s.branch:
                self.git_psx_branch_var.set(s.branch)
            if s.path == "recomp-ui" and s.branch:
                self.git_ui_branch_var.set(s.branch)

        self._clear_children(self.git_sub_list)
        self._clear_children(self.git_nested_list)
        for s in st.submodules:
            self._git_module_row(self.git_sub_list, s)
        for s in st.nested_submodules:
            if s.path == "lib/recomp-net" and s.branch:
                self.git_net_branch_var.set(s.branch)
            if s.path == "lib/retcomm-rbengine" and s.branch:
                self.git_rb_branch_var.set(s.branch)
            self._git_module_row(self.git_nested_list, s)
        if not st.nested_submodules:
            self.ctk.CTkLabel(
                self.git_nested_list,
                text="No psxrecomp checkout / nested modules yet.",
                text_color=("gray40", "gray65"),
            ).pack(anchor="w", padx=4, pady=6)
        self._refresh_branch_menus(root, st, fetch=False)
        if not quiet:
            self._log(f"Git status: {st.branch} ({dirty})")

    def _set_branch_menu(self, menu, var: tk.StringVar, branches: list[str]) -> None:
        current = (var.get() or "").strip()
        try:
            typed = (menu.get() or "").strip()
            if typed:
                current = typed
        except Exception:
            pass
        keep_default = current.lower() == "(default)" or (
            current.startswith("(") and "default" in current.lower()
        )
        values = [b for b in branches if b and not b.startswith("(")]
        if keep_default:
            values = ["(default)", *values]
        elif current and current not in values and not current.startswith("("):
            values = [current, *values]
        if not values:
            values = ["(default)", "(none)"]
        menu.configure(values=values)
        if keep_default:
            target = "(default)"
        else:
            target = current if current in values else values[0]
        var.set(target)
        try:
            menu.set(target)
        except Exception:
            pass

    def _refresh_branch_menus(self, root: Path, st, *, fetch: bool) -> None:
        from .gitops import (
            DEFAULT_PSXRECOMP_URL,
            DEFAULT_RECOMP_NET_URL,
            DEFAULT_RECOMP_UI_URL,
            DEFAULT_RBENGINE_URL,
            list_branches,
            list_module_branches,
        )

        self._set_branch_menu(
            self.git_branch_menu,
            self.git_branch_var,
            list_branches(root, remotes=True, fetch=fetch),
        )
        url_by_path = {s.path: s.url for s in st.submodules}
        nested_url = {s.path: s.url for s in st.nested_submodules}
        self._set_branch_menu(
            self.git_psx_branch_menu,
            self.git_psx_branch_var,
            list_module_branches(
                root,
                "psxrecomp",
                fetch=fetch,
                url_fallback=url_by_path.get("psxrecomp") or DEFAULT_PSXRECOMP_URL,
            ),
        )
        self._set_branch_menu(
            self.git_ui_branch_menu,
            self.git_ui_branch_var,
            list_module_branches(
                root,
                "recomp-ui",
                fetch=fetch,
                url_fallback=url_by_path.get("recomp-ui") or DEFAULT_RECOMP_UI_URL,
            ),
        )
        self._set_branch_menu(
            self.git_net_branch_menu,
            self.git_net_branch_var,
            list_module_branches(
                root,
                "lib/recomp-net",
                nested=True,
                fetch=fetch,
                url_fallback=nested_url.get("lib/recomp-net") or DEFAULT_RECOMP_NET_URL,
            ),
        )
        self._set_branch_menu(
            self.git_rb_branch_menu,
            self.git_rb_branch_var,
            list_module_branches(
                root,
                "lib/retcomm-rbengine",
                nested=True,
                fetch=fetch,
                url_fallback=nested_url.get("lib/retcomm-rbengine")
                or DEFAULT_RBENGINE_URL,
            ),
        )
        # Merge Git-tab branch names into Bulk dropdown lists without
        # overwriting the Bulk tab's independent branch selections.
        for src, dst, bvar in (
            (
                self.git_branch_menu,
                getattr(self, "bulk_game_branch_menu", None),
                getattr(self, "bulk_game_branch_var", None),
            ),
            (
                self.git_psx_branch_menu,
                getattr(self, "bulk_psx_branch_menu", None),
                getattr(self, "bulk_psx_branch_var", None),
            ),
            (
                self.git_ui_branch_menu,
                getattr(self, "bulk_ui_branch_menu", None),
                getattr(self, "bulk_ui_branch_var", None),
            ),
            (
                self.git_net_branch_menu,
                getattr(self, "bulk_net_branch_menu", None),
                getattr(self, "bulk_net_branch_var", None),
            ),
            (
                self.git_rb_branch_menu,
                getattr(self, "bulk_rb_branch_menu", None),
                getattr(self, "bulk_rb_branch_var", None),
            ),
        ):
            if dst is None:
                continue
            try:
                incoming = [b for b in (src.cget("values") or []) if b and not str(b).startswith("(")]
                if not incoming:
                    continue
                existing = [b for b in (dst.cget("values") or []) if b and not str(b).startswith("(")]
                merged = list(dict.fromkeys([*existing, *incoming]))
                keep = ""
                if bvar is not None:
                    keep = (bvar.get() or "").strip()
                if not keep:
                    try:
                        keep = (dst.get() or "").strip()
                    except Exception:
                        keep = ""
                if keep and keep not in merged:
                    merged = [keep, *merged]
                dst.configure(values=merged or ["(none)"])
                if keep:
                    if bvar is not None:
                        bvar.set(keep)
                    dst.set(keep)
            except Exception:
                pass

    def _git_fetch_branches(self) -> None:
        root = self._game_root()
        if root is None:
            return
        from .gitops import repo_status

        self._log("Fetching branch lists (git fetch / ls-remote)…")
        st = repo_status(root)
        self._git_status = st
        if not st.is_git:
            messagebox.showerror("Project Studio", "Not a git repository.", parent=self.root)
            return
        # Keep current tracking selections from status
        for s in st.submodules:
            if s.path == "psxrecomp" and s.branch:
                self.git_psx_branch_var.set(s.branch)
            if s.path == "recomp-ui" and s.branch:
                self.git_ui_branch_var.set(s.branch)
        for s in st.nested_submodules:
            if s.path == "lib/recomp-net" and s.branch:
                self.git_net_branch_var.set(s.branch)
            if s.path == "lib/retcomm-rbengine" and s.branch:
                self.git_rb_branch_var.set(s.branch)
        if st.branch:
            self.git_branch_var.set(st.branch)
        self._refresh_branch_menus(root, st, fetch=True)
        self._log("Branch menus updated.")

    def _valid_branch_selection(self, value: str) -> str | None:
        v = (value or "").strip()
        if not v:
            return None
        # Allow Bulk "(default)" sentinel through to switch_branch.
        low = v.lower()
        if low == "(default)" or (v.startswith("(") and "default" in low):
            return "(default)"
        if v.startswith("("):
            return None
        return v

    def _combo_branch(self, menu, var: tk.StringVar) -> str | None:
        try:
            typed = (menu.get() or "").strip()
        except Exception:
            typed = ""
        return self._valid_branch_selection(typed or var.get())

    def _git_create(self) -> bool:
        return bool(self.git_create_branch_var.get())

    def _git_module_row(self, parent, s) -> None:
        ctk = self.ctk
        row = ctk.CTkFrame(parent, fg_color=("gray90", "gray17"), corner_radius=8)
        row.pack(fill="x", pady=3, padx=2)
        mark = "OK" if s.present else "MISS"
        color = "#3dd68c" if s.present else "#f07178"
        ctk.CTkLabel(
            row,
            text=mark,
            text_color=color,
            font=ctk.CTkFont(size=11, weight="bold"),
            width=48,
        ).pack(side="left", padx=(10, 6), pady=8)
        body = ctk.CTkFrame(row, fg_color="transparent")
        body.pack(side="left", fill="x", expand=True, padx=(0, 10), pady=6)
        ctk.CTkLabel(
            body,
            text=s.path,
            font=ctk.CTkFont(size=13, weight="bold"),
            anchor="w",
        ).pack(fill="x")
        head = s.checkout_branch or ("detached" if s.present else "-")
        detail = (
            f"HEAD={head}  track={s.branch or '-'}  sha={s.sha or '-'}  "
            f"{s.url or '(no url)'}"
        )
        ctk.CTkLabel(
            body,
            text=detail,
            text_color=("gray40", "gray65"),
            font=ctk.CTkFont(size=11),
            anchor="w",
            wraplength=900,
            justify="left",
        ).pack(fill="x")

    def _git_switch_branch(self) -> None:
        from .gitops import switch_branch

        root = self._game_root()
        if root is None:
            return
        branch = self._combo_branch(self.git_branch_menu, self.git_branch_var)
        if not branch:
            messagebox.showerror(
                "Project Studio",
                "Pick or type a branch name, then Switch.",
                parent=self.root,
            )
            return
        r = switch_branch(
            root,
            branch,
            create=self._git_create(),
            dry_run=self._git_dry(),
        )
        self._log_cmd(r)
        self.refresh_git()

    def _git_switch_modules(self) -> None:
        from .gitops import switch_modules

        root = self._game_root()
        if root is None:
            return
        branches = {
            "psxrecomp": self._combo_branch(
                self.git_psx_branch_menu, self.git_psx_branch_var
            )
            or "",
            "recomp-ui": self._combo_branch(
                self.git_ui_branch_menu, self.git_ui_branch_var
            )
            or "",
        }
        if not any(branches.values()):
            messagebox.showerror(
                "Project Studio",
                "Pick or type a branch for psxrecomp / recomp-ui.",
                parent=self.root,
            )
            return
        self._log_module_results(
            switch_modules(
                root,
                nested=False,
                branch_by_path=branches,
                create=self._git_create(),
                set_tracking=True,
                dry_run=self._git_dry(),
            )
        )
        self.refresh_git()

    def _git_switch_nested(self) -> None:
        from .gitops import switch_modules

        root = self._game_root()
        if root is None:
            return
        branches = {
            "lib/recomp-net": self._combo_branch(
                self.git_net_branch_menu, self.git_net_branch_var
            )
            or "",
            "lib/retcomm-rbengine": self._combo_branch(
                self.git_rb_branch_menu, self.git_rb_branch_var
            )
            or "",
        }
        if not any(branches.values()):
            messagebox.showerror(
                "Project Studio",
                "Pick or type a branch for recomp-net / rbengine.",
                parent=self.root,
            )
            return
        self._log_module_results(
            switch_modules(
                root,
                nested=True,
                branch_by_path=branches,
                create=self._git_create(),
                set_tracking=True,
                dry_run=self._git_dry(),
            )
        )
        self.refresh_git()

    def _git_ensure_submodules(self) -> None:
        from .gitops import ensure_known_submodules

        root = self._game_root()
        if root is None:
            return
        results = ensure_known_submodules(
            root,
            psxrecomp_branch=self._combo_branch(
                self.git_psx_branch_menu, self.git_psx_branch_var
            )
            or "master",
            recomp_ui_branch=self._combo_branch(
                self.git_ui_branch_menu, self.git_ui_branch_var
            )
            or "master",
            dry_run=self._git_dry(),
        )
        for r in results:
            self._log_cmd(r)
        self.refresh_git()

    def _git_save_submodule_branches(self) -> None:
        from .gitops import set_submodule_branch

        root = self._game_root()
        if root is None:
            return
        for path, menu, var in (
            ("psxrecomp", self.git_psx_branch_menu, self.git_psx_branch_var),
            ("recomp-ui", self.git_ui_branch_menu, self.git_ui_branch_var),
        ):
            branch = self._combo_branch(menu, var)
            if not branch:
                continue
            r = set_submodule_branch(root, path, branch, dry_run=self._git_dry())
            self._log_cmd(r)
        self.refresh_git()

    def _git_update_submodules(self) -> None:
        from .gitops import update_submodules

        root = self._game_root()
        if root is None:
            return
        remote = bool(self.git_remote_update_var.get())
        if remote and not self._git_dry():
            if not messagebox.askyesno(
                "Project Studio",
                "Update submodules to their remote tracking tips?\n\n"
                "This moves working trees; commit the gitlink changes afterward "
                "so CI picks up the new SHAs.",
                parent=self.root,
            ):
                return
        r = update_submodules(
            root,
            paths=["psxrecomp", "recomp-ui"],
            remote=remote,
            dry_run=self._git_dry(),
        )
        self._log_cmd(r)
        self.refresh_git()

    def _log_module_results(self, results) -> bool:
        ok = True
        for r in results:
            self._log_cmd(r)
            if not r.ok:
                ok = False
        return ok

    def _git_pull_kwargs(self) -> dict:
        mode = (self.git_pull_mode_var.get() or "ff-only").strip()
        dirty = (self.git_pull_dirty_var.get() or "fail").strip()
        return {"mode": mode, "dirty": dirty, "dry_run": self._git_dry()}

    def _bulk_targets(self) -> dict:
        return {
            "game": bool(self.bulk_tgt_game_var.get()),
            "modules": bool(self.bulk_tgt_modules_var.get()),
            "psxrecomp": bool(self.bulk_tgt_psx_var.get()),
            "nested": bool(self.bulk_tgt_nested_var.get()),
        }

    def _bulk_targets_game_only(self) -> None:
        self.bulk_tgt_game_var.set(True)
        self.bulk_tgt_modules_var.set(False)
        self.bulk_tgt_psx_var.set(False)
        self.bulk_tgt_nested_var.set(False)

    def _bulk_targets_engine(self) -> None:
        """Modules + nested libs (psxrecomp / recomp-ui / net / rbengine)."""
        self.bulk_tgt_game_var.set(False)
        self.bulk_tgt_modules_var.set(True)
        self.bulk_tgt_psx_var.set(False)
        self.bulk_tgt_nested_var.set(True)

    def _bulk_refresh_list(self) -> None:
        ctk = self.ctk
        frame = getattr(self, "bulk_repo_list", None)
        if frame is None:
            return
        prev = {
            path: bool(var.get())
            for path, var in getattr(self, "_bulk_vars", {}).items()
        }
        self._clear_children(frame)
        self._bulk_vars = {}
        idx = self._repo_index
        if idx is None or not idx.repos:
            ctk.CTkLabel(
                frame,
                text="No indexed repos — use Add… in the header.",
                text_color=("gray40", "gray65"),
            ).pack(anchor="w", padx=4, pady=8)
            return
        for entry in idx.repos:
            path = entry.path
            checked = prev.get(path, True)
            var = tk.BooleanVar(value=checked)
            self._bulk_vars[path] = var
            row = ctk.CTkFrame(frame, fg_color="transparent")
            row.pack(fill="x", padx=2, pady=2)
            ctk.CTkCheckBox(
                row,
                text=entry.label(),
                variable=var,
            ).pack(side="left", fill="x", expand=True)
            ctk.CTkLabel(
                row,
                text=path,
                text_color=("gray45", "gray60"),
                font=ctk.CTkFont(size=11),
                anchor="e",
            ).pack(side="right", padx=(8, 0))

    def _bulk_select_all(self) -> None:
        for var in self._bulk_vars.values():
            var.set(True)

    def _bulk_select_none(self) -> None:
        for var in self._bulk_vars.values():
            var.set(False)

    def _bulk_apply_path_selection(self, paths: set[str]) -> int:
        """Check only repos whose index path is in ``paths``. Returns count selected."""
        n = 0
        for path, var in self._bulk_vars.items():
            on = path in paths
            var.set(on)
            if on:
                n += 1
        return n

    def _bulk_select_catalog(self) -> None:
        from .bulkops import filter_indexed_catalog

        hits, note = filter_indexed_catalog(self._repo_index)
        paths = {e.path for e in hits}
        n = self._bulk_apply_path_selection(paths)
        self._log(f"--- Bulk select catalog ({n} of {len(self._bulk_vars)}) · {note} ---")
        if n == 0:
            messagebox.showinfo(
                "Project Studio",
                "No indexed repos matched a retcomm-catalog entry.\n"
                f"({note})",
                parent=self.root,
            )

    def _bulk_select_contributor(self) -> None:
        from .bulkops import filter_indexed_contributors

        self._log("--- Bulk select contributor (querying gh viewerPermission) ---")
        self.root.update_idletasks()
        hits, logs = filter_indexed_contributors(self._repo_index)
        for line in logs:
            self._log(f"  {line}")
        paths = {e.path for e in hits}
        n = self._bulk_apply_path_selection(paths)
        self._log(f"Selected {n} repo(s) with WRITE/MAINTAIN/ADMIN")
        if n == 0:
            messagebox.showinfo(
                "Project Studio",
                "No indexed repos reported contributor (WRITE+) privilege.\n"
                "Requires authenticated `gh` and a GitHub remote.",
                parent=self.root,
            )

    def _bulk_select_catalog_contributor(self) -> None:
        """Select repos that are both catalog-backed and WRITE+ (drops catalog-only)."""
        from .bulkops import filter_indexed_catalog_contributors

        self._log(
            "--- Bulk select catalog ∩ contributor "
            "(catalog filter, then gh viewerPermission) ---"
        )
        self.root.update_idletasks()
        hits, note, logs = filter_indexed_catalog_contributors(self._repo_index)
        for line in logs:
            self._log(f"  {line}")
        paths = {e.path for e in hits}
        n = self._bulk_apply_path_selection(paths)
        self._log(
            f"Selected {n} of {len(self._bulk_vars)} "
            f"(catalog ∩ WRITE/MAINTAIN/ADMIN) · {note}"
        )
        if n == 0:
            messagebox.showinfo(
                "Project Studio",
                "No indexed repos matched both a catalog entry and "
                "contributor (WRITE+) privilege.\n"
                f"({note})\n"
                "Requires authenticated `gh` and a GitHub remote.",
                parent=self.root,
            )

    def _bulk_selected_repos(self) -> list[tuple[str, Path]]:
        idx = self._repo_index
        if idx is None:
            return []
        out: list[tuple[str, Path]] = []
        for entry in idx.repos:
            var = self._bulk_vars.get(entry.path)
            if var is None or not var.get():
                continue
            try:
                root = entry.resolved()
            except OSError:
                continue
            if root.is_dir():
                out.append((entry.label(), root))
        return out

    def _bulk_require_selection(self) -> list[tuple[str, Path]] | None:
        repos = self._bulk_selected_repos()
        if not repos:
            messagebox.showinfo(
                "Project Studio",
                "Select at least one indexed repo on the Bulk tab.",
                parent=self.root,
            )
            return None
        return repos

    def _bulk_require_targets(self, *, need_commit: bool = False) -> dict | None:
        t = self._bulk_targets()
        if need_commit:
            if not (t["game"] or t["modules"] or t["nested"]):
                messagebox.showinfo(
                    "Project Studio",
                    "Enable at least one commit target: Game root, Modules, "
                    "or Nested libs.",
                    parent=self.root,
                )
                return None
        elif not any(t.values()):
            messagebox.showinfo(
                "Project Studio",
                "Enable at least one target: Game root, Modules, "
                "psxrecomp, or Nested libs.",
                parent=self.root,
            )
            return None
        return t

    def _bulk_jobs(self) -> int:
        from .bulkops import clamp_bulk_jobs

        return clamp_bulk_jobs(self.bulk_jobs_var.get())

    def _on_bulk_jobs_changed(self, _value: str | None = None) -> None:
        from .bulkops import clamp_bulk_jobs
        from .repo_index import save_index

        jobs = clamp_bulk_jobs(self.bulk_jobs_var.get())
        self.bulk_jobs_var.set(str(jobs))
        idx = self._repo_index
        if idx is not None:
            idx.bulk_jobs = jobs
            save_index(idx)

    def _bulk_stream_repo(self, results) -> None:
        """Marshal per-repo CmdResult batches onto the UI activity log."""

        def go(rs=results) -> None:
            self._log_module_results(rs)

        self.root.after(0, go)

    def _bulk_run_bg(
        self,
        title: str,
        fn,
        *,
        refresh_git: bool = False,
        done_info: str | None = None,
        done_warn_prefix: str | None = None,
        status_var_set=None,
    ) -> None:
        """Run a bulkops call off the UI thread with parallel workers + live log."""
        if self._bulk_busy:
            messagebox.showinfo(
                "Project Studio",
                "A bulk operation is already running.",
                parent=self.root,
            )
            return
        jobs = self._bulk_jobs()
        self._log(f"--- {title} (parallel={jobs}) ---")
        self._bulk_busy = True

        def worker() -> None:
            results = []
            err: str | None = None
            try:
                results = fn(jobs=jobs, on_repo=self._bulk_stream_repo) or []
            except Exception as exc:  # noqa: BLE001
                err = str(exc)
            finally:
                self._bulk_busy = False

            def done() -> None:
                if err:
                    self._log(f"[FAIL] Bulk error: {err}", tag="error")
                    messagebox.showerror(
                        "Project Studio",
                        f"Bulk operation failed:\n{err}",
                        parent=self.root,
                    )
                    return
                ok = sum(1 for r in results if getattr(r, "ok", False))
                fail = len(results) - ok
                self._log(f"--- done: {ok} ok, {fail} fail ---")
                if callable(status_var_set):
                    status_var_set(ok, fail, results)
                if refresh_git:
                    self.refresh_git(quiet=True)
                if fail and done_warn_prefix:
                    messagebox.showwarning(
                        "Project Studio",
                        f"{done_warn_prefix}\n{fail} failed — see activity log.",
                        parent=self.root,
                    )
                elif done_info and not self._git_dry() and fail == 0:
                    messagebox.showinfo(
                        "Project Studio", done_info, parent=self.root
                    )

            self.root.after(0, done)

        threading.Thread(target=worker, daemon=True).start()

    def _bulk_status(self) -> None:
        from .bulkops import bulk_status

        repos = self._bulk_require_selection()
        if repos is None:
            return

        def run(*, jobs: int, on_repo) -> list:
            return bulk_status(repos, jobs=jobs, on_repo=on_repo)

        self._bulk_run_bg(f"Bulk status ({len(repos)} repos)", run)

    def _bulk_pull(self) -> None:
        from .bulkops import bulk_pull

        repos = self._bulk_require_selection()
        if repos is None:
            return
        targets = self._bulk_require_targets()
        if targets is None:
            return
        names = ", ".join(lab for lab, _ in repos)
        if not self._git_confirm_pull(f"{len(repos)} repo(s):\n{names}"):
            return
        pull_kw = self._git_pull_kwargs()

        def run(*, jobs: int, on_repo) -> list:
            return bulk_pull(
                repos, **targets, **pull_kw, jobs=jobs, on_repo=on_repo
            )

        self._bulk_run_bg(
            f"Bulk pull ({len(repos)} repos)", run, refresh_git=True
        )

    def _bulk_push(self) -> None:
        from .bulkops import bulk_push

        repos = self._bulk_require_selection()
        if repos is None:
            return
        targets = self._bulk_require_targets()
        if targets is None:
            return
        if not self._git_dry():
            names = "\n".join(f"• {lab}" for lab, _ in repos)
            if not messagebox.askyesno(
                "Project Studio",
                f"Push selected targets for {len(repos)} repo(s)?\n\n"
                f"{names}\n\n(no force-push)",
                parent=self.root,
            ):
                return
        dry = self._git_dry()

        def run(*, jobs: int, on_repo) -> list:
            return bulk_push(
                repos, **targets, dry_run=dry, jobs=jobs, on_repo=on_repo
            )

        self._bulk_run_bg(
            f"Bulk push ({len(repos)} repos)", run, refresh_git=True
        )

    def _bulk_commit(self) -> None:
        from .bulkops import bulk_commit

        repos = self._bulk_require_selection()
        if repos is None:
            return
        targets = self._bulk_require_targets(need_commit=True)
        if targets is None:
            return
        msg = self.bulk_msg_var.get().strip()
        if not msg:
            messagebox.showerror(
                "Project Studio",
                "Enter a commit message.",
                parent=self.root,
            )
            return
        # psxrecomp-only is not a commit target in bulkops
        commit_targets = {
            "game": targets["game"],
            "modules": targets["modules"],
            "nested": targets["nested"],
        }
        if not self._git_dry():
            names = "\n".join(f"• {lab}" for lab, _ in repos)
            if not messagebox.askyesno(
                "Project Studio",
                f"Commit in {len(repos)} repo(s)?\n\n{names}\n\n{msg}",
                parent=self.root,
            ):
                return
        dry = self._git_dry()

        def run(*, jobs: int, on_repo) -> list:
            return bulk_commit(
                repos,
                msg,
                **commit_targets,
                dry_run=dry,
                jobs=jobs,
                on_repo=on_repo,
            )

        self._bulk_run_bg(
            f"Bulk commit ({len(repos)} repos)", run, refresh_git=True
        )

    def _bulk_switch(self) -> None:
        from .bulkops import bulk_switch

        repos = self._bulk_require_selection()
        if repos is None:
            return
        targets = self._bulk_require_targets()
        if targets is None:
            return
        game_br = (
            self._combo_branch(self.bulk_game_branch_menu, self.bulk_game_branch_var)
            or ""
        )
        psx_br = (
            self._combo_branch(self.bulk_psx_branch_menu, self.bulk_psx_branch_var)
            or ""
        )
        ui_br = (
            self._combo_branch(self.bulk_ui_branch_menu, self.bulk_ui_branch_var)
            or ""
        )
        net_br = (
            self._combo_branch(self.bulk_net_branch_menu, self.bulk_net_branch_var)
            or ""
        )
        rb_br = (
            self._combo_branch(self.bulk_rb_branch_menu, self.bulk_rb_branch_var)
            or ""
        )
        # Empty / (default) is OK — each checkout uses its own default branch.
        if targets["game"] and not game_br:
            game_br = "(default)"
        if targets["modules"] and not (psx_br or ui_br):
            psx_br = psx_br or "(default)"
            ui_br = ui_br or "(default)"
        if targets["psxrecomp"] and not targets["modules"] and not psx_br:
            psx_br = "(default)"
        if targets["nested"] and not (net_br or rb_br):
            net_br = net_br or "(default)"
            rb_br = rb_br or "(default)"
        if not self._git_dry():
            names = "\n".join(f"• {lab}" for lab, _ in repos)
            bits = []
            if targets["game"]:
                bits.append(f"game→{game_br}")
            if targets["modules"]:
                bits.append(f"psx→{psx_br or '(track)'} ui→{ui_br or '(track)'}")
            elif targets["psxrecomp"]:
                bits.append(f"psx→{psx_br}")
            if targets["nested"]:
                bits.append(f"net→{net_br or '(track)'} rb→{rb_br or '(track)'}")
            if not messagebox.askyesno(
                "Project Studio",
                f"git switch on {len(repos)} repo(s)?\n\n{names}\n\n"
                + " · ".join(bits),
                parent=self.root,
            ):
                return
        create = self._git_create()
        set_tracking = bool(self.bulk_set_tracking_var.get())
        dry = self._git_dry()
        bits = []
        if targets["game"]:
            bits.append(f"game->{game_br}")
        if targets["modules"]:
            bits.append(f"psx->{psx_br or '(skip)'} ui->{ui_br or '(skip)'}")
        elif targets["psxrecomp"]:
            bits.append(f"psx->{psx_br}")
        if targets["nested"]:
            bits.append(f"net->{net_br or '(skip)'} rb->{rb_br or '(skip)'}")
        self._log("  " + " · ".join(bits))

        def run(*, jobs: int, on_repo) -> list:
            return bulk_switch(
                repos,
                game=targets["game"],
                modules=targets["modules"],
                psxrecomp=targets["psxrecomp"],
                nested=targets["nested"],
                game_branch=game_br,
                psxrecomp_branch=psx_br,
                recomp_ui_branch=ui_br,
                recomp_net_branch=net_br,
                rbengine_branch=rb_br,
                create=create,
                set_tracking=set_tracking,
                dry_run=dry,
                jobs=jobs,
                on_repo=on_repo,
            )

        def status(ok: int, fail: int, results) -> None:
            self.bulk_branch_status_var.set(
                f"Switch done: {ok}/{ok + fail} ok"
            )

        self._bulk_run_bg(
            f"Bulk switch ({len(repos)} repos)",
            run,
            refresh_git=True,
            status_var_set=status,
        )

    def _bulk_fetch_branches(self) -> None:
        """Populate Bulk branch ComboBoxes from remotes + selected game repos."""
        if getattr(self, "_bulk_branch_fetch_busy", False):
            return
        self._bulk_branch_fetch_busy = True
        self.bulk_branch_status_var.set("Fetching branches…")
        selected = self._bulk_selected_repos()
        # If nothing checked, use all indexed rows for game-branch union.
        game_roots = [root for _, root in selected] if selected else []
        if not game_roots:
            from .bulkops import indexed_repos
            from .repo_index import load_index

            idx = getattr(self, "_repo_index", None) or load_index()
            game_roots = [root for _, root in indexed_repos(index=idx)]

        def worker() -> None:
            from .gitops import (
                DEFAULT_PSXRECOMP_URL,
                DEFAULT_RECOMP_NET_URL,
                DEFAULT_RECOMP_UI_URL,
                DEFAULT_RBENGINE_URL,
                list_branches,
                list_remote_head_branches,
            )

            game_names: list[str] = []
            seen: set[str] = set()
            for root in game_roots:
                try:
                    for b in list_branches(root, remotes=True, fetch=False):
                        if b and b not in seen:
                            seen.add(b)
                            game_names.append(b)
                except Exception:
                    continue
            try:
                results = {
                    "game": game_names,
                    "psx": list_remote_head_branches(DEFAULT_PSXRECOMP_URL),
                    "ui": list_remote_head_branches(DEFAULT_RECOMP_UI_URL),
                    "net": list_remote_head_branches(DEFAULT_RECOMP_NET_URL),
                    "rb": list_remote_head_branches(DEFAULT_RBENGINE_URL),
                }
                err = ""
            except Exception as exc:
                results = {
                    "game": game_names,
                    "psx": [],
                    "ui": [],
                    "net": [],
                    "rb": [],
                }
                err = str(exc)

            def apply() -> None:
                self._bulk_branch_fetch_busy = False
                pairs = (
                    ("game", self.bulk_game_branch_var, getattr(self, "bulk_game_branch_menu", None)),
                    ("psx", self.bulk_psx_branch_var, getattr(self, "bulk_psx_branch_menu", None)),
                    ("ui", self.bulk_ui_branch_var, getattr(self, "bulk_ui_branch_menu", None)),
                    ("net", self.bulk_net_branch_var, getattr(self, "bulk_net_branch_menu", None)),
                    ("rb", self.bulk_rb_branch_var, getattr(self, "bulk_rb_branch_menu", None)),
                )
                counts = []
                for key, var, menu in pairs:
                    branches = results.get(key) or []
                    if menu is None:
                        continue
                    self._set_branch_menu(menu, var, branches)
                    counts.append(f"{key}:{len(branches)}")
                if err:
                    self.bulk_branch_status_var.set(f"Fetch error: {err}")
                    self._log(f"Bulk branch fetch failed: {err}")
                else:
                    msg = "Branches: " + ", ".join(counts)
                    self.bulk_branch_status_var.set(msg)
                    self._log(f"--- Bulk fetch branches ---\n  {msg}")

            self.root.after(0, apply)

        threading.Thread(target=worker, daemon=True).start()

    def _bulk_release(self) -> None:
        from .bulkops import bulk_release

        repos = self._bulk_require_selection()
        if repos is None:
            return
        version = self.release_version_var.get().strip()
        bump = self.release_bump_var.get().strip() or "patch"
        publish = bool(self.release_publish_var.get())
        reuse = bool(self.release_reuse_var.get())
        detail = (
            f"version={version or '(auto per repo)'} bump={bump} "
            f"publish={publish} reuse_cached_emitters={reuse}"
        )
        if not self._git_dry():
            names = "\n".join(f"* {lab}" for lab, _ in repos)
            if not messagebox.askyesno(
                "Project Studio",
                f"Dispatch release.yml on {len(repos)} repo(s)?\n\n"
                f"{names}\n\n{detail}",
                parent=self.root,
            ):
                return
        self._log(detail)
        dry = self._git_dry()

        def run(*, jobs: int, on_repo) -> list:
            return bulk_release(
                repos,
                version=version,
                bump=bump,
                publish=publish,
                reuse_cached_emitters=reuse,
                dry_run=dry,
                jobs=jobs,
                on_repo=on_repo,
            )

        self._bulk_run_bg(
            f"Bulk release CI ({len(repos)} repos)",
            run,
            done_info="Release workflows dispatched — see activity log.",
            done_warn_prefix="Release dispatch finished with failures.",
        )

    def _bulk_install_ci(self) -> None:
        from .bulkops import bulk_install_ci

        repos = self._bulk_require_selection()
        if repos is None:
            return
        existing = [
            lab
            for lab, root in repos
            if (root / ".github" / "workflows" / "release.yml").is_file()
        ]
        force = False
        if existing and not self._git_dry():
            preview = "\n".join(f"* {n}" for n in existing[:12])
            extra = "" if len(existing) <= 12 else f"\n... +{len(existing) - 12} more"
            if not messagebox.askyesno(
                "Project Studio",
                f"{len(existing)} selected repo(s) already have release.yml.\n"
                f"Overwrite from the current psxrecomp template, commit, and push?\n\n"
                f"{preview}{extra}",
                parent=self.root,
            ):
                return
            force = True
        elif not self._git_dry():
            names = "\n".join(f"* {lab}" for lab, _ in repos)
            if not messagebox.askyesno(
                "Project Studio",
                f"Install release.yml + package script, commit, and push for "
                f"{len(repos)} repo(s)?\n\n{names}",
                parent=self.root,
            ):
                return

        dry = self._git_dry()

        def run(*, jobs: int, on_repo) -> list:
            return bulk_install_ci(
                repos,
                force=force,
                push_remote=True,
                dry_run=dry,
                jobs=jobs,
                on_repo=on_repo,
            )

        self._bulk_run_bg(
            f"Bulk install CI ({len(repos)} repos) force={force}",
            run,
            refresh_git=True,
            done_info="Install CI finished — see activity log.",
            done_warn_prefix="Install CI finished with failures.",
        )

    def _git_confirm_pull(self, scope: str) -> bool:
        """Confirm destructive pull modes. Returns False if user cancels."""
        if self._git_dry():
            return True
        mode = (self.git_pull_mode_var.get() or "ff-only").strip()
        dirty = (self.git_pull_dirty_var.get() or "fail").strip()
        if mode == "reset":
            return bool(
                messagebox.askyesno(
                    "Project Studio",
                    f"Reset {scope} to match origin (discard local commits "
                    f"and edits)?\n\nmode=reset",
                    parent=self.root,
                )
            )
        if dirty == "discard":
            return bool(
                messagebox.askyesno(
                    "Project Studio",
                    f"Discard uncommitted edits in {scope}, then pull "
                    f"({mode})?",
                    parent=self.root,
                )
            )
        return True

    def _git_pull_modules(self) -> None:
        from .gitops import pull_modules

        root = self._game_root()
        if root is None:
            return
        if not self._git_confirm_pull("psxrecomp + recomp-ui"):
            return
        self._log_module_results(
            pull_modules(root, nested=False, **self._git_pull_kwargs())
        )
        self.refresh_git()

    def _git_push_modules(self) -> None:
        from .gitops import push_modules

        root = self._game_root()
        if root is None:
            return
        if not self._git_dry():
            if not messagebox.askyesno(
                "Project Studio",
                "Push HEAD → origin for psxrecomp and recomp-ui?\n\n"
                "If a checkout is detached, uses the branch selected above.\n"
                "(no force-push)",
                parent=self.root,
            ):
                return
        branches = {
            "psxrecomp": self._combo_branch(
                self.git_psx_branch_menu, self.git_psx_branch_var
            )
            or "",
            "recomp-ui": self._combo_branch(
                self.git_ui_branch_menu, self.git_ui_branch_var
            )
            or "",
        }
        self._log_module_results(
            push_modules(
                root,
                nested=False,
                branch_by_path=branches,
                dry_run=self._git_dry(),
            )
        )
        self.refresh_git()

    def _git_commit_modules(self) -> None:
        from .gitops import commit_modules

        root = self._game_root()
        if root is None:
            return
        msg = self.git_sub_msg_var.get().strip()
        if not msg:
            messagebox.showerror(
                "Project Studio",
                "Enter a commit message for psxrecomp / recomp-ui.",
                parent=self.root,
            )
            return
        if not self._git_dry():
            if not messagebox.askyesno(
                "Project Studio",
                f"Commit inside psxrecomp and recomp-ui?\n\n{msg}",
                parent=self.root,
            ):
                return
        self._log_module_results(
            commit_modules(root, msg, nested=False, dry_run=self._git_dry())
        )
        self.refresh_git()

    def _git_ensure_nested(self) -> None:
        from .gitops import ensure_nested_modules

        root = self._game_root()
        if root is None:
            return
        results = ensure_nested_modules(
            root,
            recomp_net_branch=self._combo_branch(
                self.git_net_branch_menu, self.git_net_branch_var
            )
            or "main",
            rbengine_branch=self._combo_branch(
                self.git_rb_branch_menu, self.git_rb_branch_var
            )
            or "main",
            dry_run=self._git_dry(),
        )
        for r in results:
            self._log_cmd(r)
        self.refresh_git()

    def _git_save_nested_branches(self) -> None:
        from .gitops import set_nested_branch

        root = self._game_root()
        if root is None:
            return
        for path, menu, var in (
            ("lib/recomp-net", self.git_net_branch_menu, self.git_net_branch_var),
            ("lib/retcomm-rbengine", self.git_rb_branch_menu, self.git_rb_branch_var),
        ):
            branch = self._combo_branch(menu, var)
            if not branch:
                continue
            r = set_nested_branch(root, path, branch, dry_run=self._git_dry())
            self._log_cmd(r)
        self.refresh_git()

    def _git_update_nested(self) -> None:
        from .gitops import update_nested_modules

        root = self._game_root()
        if root is None:
            return
        remote = bool(self.git_remote_update_var.get())
        if remote and not self._git_dry():
            if not messagebox.askyesno(
                "Project Studio",
                "Update nested modules inside psxrecomp to remote tips?\n\n"
                "Stages gitlinks in psxrecomp — use Commit in psxrecomp, then "
                "bump the game's psxrecomp gitlink.",
                parent=self.root,
            ):
                return
        r = update_nested_modules(
            root, remote=remote, stage=True, dry_run=self._git_dry()
        )
        self._log_cmd(r)
        self.refresh_git()

    def _git_pull_nested(self) -> None:
        from .gitops import pull_modules

        root = self._game_root()
        if root is None:
            return
        if not self._git_confirm_pull("nested libs (recomp-net, rbengine)"):
            return
        self._log_module_results(
            pull_modules(root, nested=True, **self._git_pull_kwargs())
        )
        self.refresh_git()

    def _git_push_nested(self) -> None:
        from .gitops import push_modules

        root = self._game_root()
        if root is None:
            return
        if not self._git_dry():
            if not messagebox.askyesno(
                "Project Studio",
                "Push HEAD → origin for recomp-net and retcomm-rbengine?\n\n"
                "If a checkout is detached, uses the branch selected above.\n"
                "(no force-push)",
                parent=self.root,
            ):
                return
        branches = {
            "lib/recomp-net": self._combo_branch(
                self.git_net_branch_menu, self.git_net_branch_var
            )
            or "",
            "lib/retcomm-rbengine": self._combo_branch(
                self.git_rb_branch_menu, self.git_rb_branch_var
            )
            or "",
        }
        self._log_module_results(
            push_modules(
                root,
                nested=True,
                branch_by_path=branches,
                dry_run=self._git_dry(),
            )
        )
        self.refresh_git()

    def _git_commit_nested_libs(self) -> None:
        from .gitops import commit_modules

        root = self._game_root()
        if root is None:
            return
        msg = self.git_libs_msg_var.get().strip()
        if not msg:
            messagebox.showerror(
                "Project Studio",
                "Enter a commit message for nested libs.",
                parent=self.root,
            )
            return
        if not self._git_dry():
            if not messagebox.askyesno(
                "Project Studio",
                f"Commit inside recomp-net and retcomm-rbengine?\n\n{msg}",
                parent=self.root,
            ):
                return
        self._log_module_results(
            commit_modules(root, msg, nested=True, dry_run=self._git_dry())
        )
        self.refresh_git()

    def _git_pull_psxrecomp(self) -> None:
        from .gitops import pull_psxrecomp

        root = self._game_root()
        if root is None:
            return
        if not self._git_confirm_pull("psxrecomp"):
            return
        self._log_cmd(pull_psxrecomp(root, **self._git_pull_kwargs()))
        self.refresh_git()

    def _git_push_psxrecomp(self) -> None:
        from .gitops import push_psxrecomp

        root = self._game_root()
        if root is None:
            return
        branch = (
            self._combo_branch(self.git_psx_branch_menu, self.git_psx_branch_var) or ""
        )
        if not self._git_dry():
            extra = (
                f"\nDetached HEAD will push to origin/{branch}."
                if branch
                else "\nDetached HEAD needs a branch selected above."
            )
            if not messagebox.askyesno(
                "Project Studio",
                "Push the psxrecomp checkout to origin?\n\n"
                f"(no force-push){extra}",
                parent=self.root,
            ):
                return
        self._log_cmd(
            push_psxrecomp(root, branch=branch, dry_run=self._git_dry())
        )
        self.refresh_git()

    def _git_commit_nested(self) -> None:
        from .gitops import commit_nested

        root = self._game_root()
        if root is None:
            return
        msg = self.git_nested_msg_var.get().strip()
        if not msg:
            messagebox.showerror(
                "Project Studio", "Enter a psxrecomp commit message.", parent=self.root
            )
            return
        if not self._git_dry():
            if not messagebox.askyesno(
                "Project Studio",
                f"Commit inside psxrecomp checkout?\n\n{msg}",
                parent=self.root,
            ):
                return
        r = commit_nested(root, msg, dry_run=self._git_dry())
        self._log_cmd(r)
        self.refresh_git()

    def _git_pull(self) -> None:
        from .gitops import pull

        root = self._game_root()
        if root is None:
            return
        if not self._git_confirm_pull("game repo"):
            return
        r = pull(root, **self._git_pull_kwargs())
        self._log_cmd(r)
        self.refresh_git()

    def _git_commit(self) -> None:
        from .gitops import commit_all

        root = self._game_root()
        if root is None:
            return
        msg = self.git_msg_var.get().strip()
        if not msg:
            messagebox.showerror("Project Studio", "Enter a commit message.", parent=self.root)
            return
        if not self._git_dry():
            if not messagebox.askyesno(
                "Project Studio",
                f"Commit all changes in the game repo?\n{root}\n\n{msg}",
                parent=self.root,
            ):
                return
        r = commit_all(root, msg, dry_run=self._git_dry())
        self._log_cmd(r)
        if r.ok and not self._git_dry():
            self.git_msg_var.set("")
        self.refresh_git()

    def _git_push(self) -> None:
        from .gitops import push

        root = self._game_root()
        if root is None:
            return
        branch = self._combo_branch(self.git_branch_menu, self.git_branch_var) or ""
        if not self._git_dry():
            if not messagebox.askyesno(
                "Project Studio",
                f"Push game repo to origin?\n{root}\n\n"
                "(no force-push; detached HEAD uses selected game branch)",
                parent=self.root,
            ):
                return
        r = push(root, branch=branch, dry_run=self._git_dry())
        self._log_cmd(r)
        self.refresh_git()

    def _git_run_release(self) -> None:
        from .gitops import run_release_workflow

        root = self._game_root()
        if root is None:
            return
        version = self.release_version_var.get().strip()
        bump = self.release_bump_var.get().strip() or "patch"
        publish = bool(self.release_publish_var.get())
        reuse = bool(self.release_reuse_var.get())
        if not self._git_dry():
            detail = (
                f"version={version or '(auto)'} bump={bump} "
                f"publish={publish} reuse_cached_emitters={reuse}"
            )
            if not messagebox.askyesno(
                "Project Studio",
                f"Dispatch release.yml on GitHub?\n\n{detail}",
                parent=self.root,
            ):
                return
        r = run_release_workflow(
            root,
            version=version,
            bump=bump,
            publish=publish,
            reuse_cached_emitters=reuse,
            dry_run=self._git_dry(),
        )
        self._log_cmd(r)
        if r.ok and not self._git_dry():
            messagebox.showinfo("Project Studio", r.message, parent=self.root)
        else:
            messagebox.showwarning("Project Studio", r.message, parent=self.root)

    def _git_install_push_ci(self) -> None:
        from .gitops import install_and_push_release_ci

        root = self._game_root()
        if root is None:
            return
        zip_prefix = self.zip_var.get().strip()
        force = False
        wf = root / ".github" / "workflows" / "release.yml"
        if wf.is_file():
            if not messagebox.askyesno(
                "Project Studio",
                f"release.yml already exists:\n{wf}\n\n"
                "Overwrite from the current psxrecomp template, commit, and push?",
                parent=self.root,
            ):
                return
            force = True
        elif not self._git_dry():
            if not messagebox.askyesno(
                "Project Studio",
                "Install psxrecomp setup-release.yml as .github/workflows/release.yml,\n"
                "commit, push to origin, and register Actions?\n\n"
                f"{root}",
                parent=self.root,
            ):
                return
        r = install_and_push_release_ci(
            root,
            zip_prefix=zip_prefix,
            force=force,
            push_remote=True,
            dry_run=self._git_dry(),
        )
        self._log_cmd(r)
        self.refresh_git()
        if r.ok:
            messagebox.showinfo("Project Studio", r.message, parent=self.root)
        else:
            messagebox.showwarning("Project Studio", r.message, parent=self.root)
