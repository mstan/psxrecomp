"""Git / GitHub operations for a game repository.

Uses ``git`` and (optionally) ``gh``. No force-push, amend, or hook skips.
"""

from __future__ import annotations

import configparser
import json
import re
import shutil
import subprocess
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

DEFAULT_PSXRECOMP_URL = "https://github.com/mstan/psxrecomp.git"
DEFAULT_RECOMP_UI_URL = "https://github.com/mstan/recomp-ui.git"
DEFAULT_RECOMP_NET_URL = "https://github.com/TechnicallyComputers/recomp-net.git"
DEFAULT_RBENGINE_URL = "https://github.com/TechnicallyComputers/retcomm-rbengine.git"
DEFAULT_BRANCH = "master"
DEFAULT_NESTED_BRANCH = "main"
KNOWN_SUBMODULES = ("psxrecomp", "recomp-ui")
# Nested under a psxrecomp checkout (game/psxrecomp or the engine repo itself).
KNOWN_NESTED_SUBMODULES: tuple[tuple[str, str, str], ...] = (
    ("lib/recomp-net", DEFAULT_RECOMP_NET_URL, DEFAULT_NESTED_BRANCH),
    ("lib/retcomm-rbengine", DEFAULT_RBENGINE_URL, DEFAULT_NESTED_BRANCH),
)
NESTED_PATHS = tuple(p for p, _, _ in KNOWN_NESTED_SUBMODULES)


@dataclass
class CmdResult:
    ok: bool
    message: str
    detail: str = ""

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass
class SubmoduleInfo:
    name: str
    path: str
    url: str = ""
    branch: str = ""
    sha: str = ""
    present: bool = False
    initialized: bool = False
    checkout_branch: str = ""  # actual HEAD branch; empty if detached / missing

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass
class RepoStatus:
    root: str
    is_git: bool
    branch: str = ""
    upstream: str = ""
    ahead: int = 0
    behind: int = 0
    dirty: bool = False
    staged: int = 0
    unstaged: int = 0
    untracked: int = 0
    remote_url: str = ""
    gh_available: bool = False
    gh_repo: str = ""
    short_status: str = ""
    submodules: list[SubmoduleInfo] = field(default_factory=list)
    nested_submodules: list[SubmoduleInfo] = field(default_factory=list)
    psxrecomp_root: str = ""
    notes: list[str] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        d = asdict(self)
        d["submodules"] = [s.to_dict() for s in self.submodules]
        d["nested_submodules"] = [s.to_dict() for s in self.nested_submodules]
        return d


def _run(
    cmd: list[str],
    cwd: Path,
    *,
    dry_run: bool = False,
    check: bool = False,
) -> tuple[int, str, str]:
    if dry_run:
        return 0, "dry-run: " + " ".join(cmd), ""
    try:
        proc = subprocess.run(
            cmd,
            cwd=str(cwd),
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError as exc:
        if check:
            raise
        return 127, "", str(exc)
    return proc.returncode, (proc.stdout or "").strip(), (proc.stderr or "").strip()


# Git verbs / patterns that talk to a remote (DNS / TLS / HTTP).
_GIT_NETWORK_VERBS = frozenset(
    {"fetch", "pull", "push", "clone", "ls-remote"}
)

# Transient reachability failures (DNS blips, TLS resets, GitHub 5xx/429, etc.).
_TRANSIENT_GIT_NETWORK_MARKERS = (
    "could not resolve host",
    "temporary failure in name resolution",
    "name or service not known",
    "nodename nor servname provided",
    "failed to connect",
    "failed to connect to",
    "connection timed out",
    "connection refused",
    "connection reset by peer",
    "network is unreachable",
    "no route to host",
    # Note: do not match bare "unable to access" — that also covers 401/403 auth.
    "the remote end hung up unexpectedly",
    "ssl_error_syscall",
    "ssl connection timeout",
    "tls handshake timeout",
    "gnutls_handshake",
    "openssl ssl_read",
    "openssl ssl_connect",
    "empty reply from server",
    "operation timed out",
    "transfer closed with outstanding read data remaining",
    "rpc failed",
    "recv failure",
    "early eof",
    "http/2 stream",
    "http 429",
    "http 502",
    "http 503",
    "http 504",
    "error: 429",
    "error: 502",
    "error: 503",
    "error: 504",
    "server aborted the request",
)

# Default: 4 attempts with 1s / 2s / 4s backoff (covers bulk-pull DNS flakes).
_GIT_NETWORK_ATTEMPTS = 4
_GIT_NETWORK_BASE_DELAY_S = 1.0


def _git_args_need_network(args: tuple[str, ...]) -> bool:
    for i, a in enumerate(args):
        if a in _GIT_NETWORK_VERBS:
            return True
        if a == "submodule" and i + 1 < len(args) and args[i + 1] in (
            "update",
            "add",
        ):
            return True
        if a == "remote" and i + 1 < len(args) and args[i + 1] == "update":
            return True
    return False


def _is_transient_git_network_error(stdout: str, stderr: str) -> bool:
    blob = f"{stdout}\n{stderr}".lower()
    return any(m in blob for m in _TRANSIENT_GIT_NETWORK_MARKERS)


def _git(
    cwd: Path,
    *args: str,
    dry_run: bool = False,
    network_attempts: int | None = None,
) -> tuple[int, str, str]:
    """Run git. Network-touching verbs retry on transient DNS/connect errors."""
    if dry_run or not _git_args_need_network(args):
        return _run(["git", *args], cwd, dry_run=dry_run)

    attempts = (
        _GIT_NETWORK_ATTEMPTS if network_attempts is None else max(1, int(network_attempts))
    )
    code, out, err = 1, "", ""
    for attempt in range(1, attempts + 1):
        code, out, err = _run(["git", *args], cwd, dry_run=False)
        if code == 0:
            if attempt > 1:
                note = f"(git network: succeeded on attempt {attempt}/{attempts})"
                out = f"{out}\n{note}".strip() if out else note
            return code, out, err
        if attempt < attempts and _is_transient_git_network_error(out, err):
            time.sleep(_GIT_NETWORK_BASE_DELAY_S * (2 ** (attempt - 1)))
            continue
        if attempt > 1 and _is_transient_git_network_error(out, err):
            note = f"(git network: failed after {attempts} attempts)"
            err = f"{err}\n{note}".strip() if err else note
        return code, out, err
    return code, out, err


def current_branch(root: Path) -> str | None:
    """Return the checked-out branch name, or None if detached / unknown."""
    root = root.expanduser().resolve()
    if not root.is_dir():
        return None
    code, out, _ = _git(root, "rev-parse", "--abbrev-ref", "HEAD")
    if code != 0:
        return None
    name = out.strip()
    if not name or name == "HEAD":
        return None
    return name


def _which_gh() -> str | None:
    return shutil.which("gh")


def _is_git_repo(root: Path) -> bool:
    code, out, _ = _git(root, "rev-parse", "--is-inside-work-tree")
    return code == 0 and out.strip() == "true"


def _read_gitmodules(root: Path) -> configparser.ConfigParser:
    cp = configparser.ConfigParser(interpolation=None)
    gm = root / ".gitmodules"
    if gm.is_file():
        cp.read(gm, encoding="utf-8")
    return cp


def _write_gitmodules(root: Path, cp: configparser.ConfigParser, *, dry_run: bool) -> None:
    if dry_run:
        return
    buf: list[str] = []
    for section in cp.sections():
        buf.append(f"[{section}]")
        for key, value in cp.items(section):
            buf.append(f"\t{key} = {value}")
        buf.append("")
    text = "\n".join(buf).rstrip() + "\n"
    (root / ".gitmodules").write_text(text, encoding="utf-8")


def _section_for_path(cp: configparser.ConfigParser, path: str) -> str | None:
    for section in cp.sections():
        if cp.get(section, "path", fallback="") == path:
            return section
    # Common form: [submodule "psxrecomp"]
    want = f'submodule "{path}"'
    for section in cp.sections():
        if section == want or section.endswith(f'"{path}"'):
            return section
    return None


def _submodule_sha(root: Path, path: str) -> str:
    code, out, _ = _git(root, "rev-parse", f"HEAD:{path}")
    if code == 0 and re.fullmatch(r"[0-9a-f]{40}", out.strip()):
        return out.strip()[:12]
    # Fallback: ls-tree
    code, out, _ = _git(root, "ls-tree", "HEAD", path)
    if code == 0:
        parts = out.split()
        if len(parts) >= 3 and parts[0] == "160000":
            return parts[2][:12]
    # Working tree HEAD inside submodule
    sub = root / path
    if sub.is_dir():
        code, out, _ = _git(sub, "rev-parse", "HEAD")
        if code == 0:
            return out.strip()[:12]
    return ""


def _submodule_remote_url(root: Path, path: str) -> str:
    """Best-effort origin URL from a submodule working tree."""
    sub = root / path
    if not sub.is_dir():
        return ""
    code, out, _ = _git(sub, "remote", "get-url", "origin")
    if code == 0 and out.strip():
        return out.strip()
    return ""


def _default_url_for_path(path: str) -> str:
    for p, url, _ in KNOWN_NESTED_SUBMODULES:
        if p == path:
            return url
    if path == "psxrecomp":
        return DEFAULT_PSXRECOMP_URL
    if path == "recomp-ui":
        return DEFAULT_RECOMP_UI_URL
    return ""


def _list_submodules(root: Path, *, known: tuple[str, ...] = KNOWN_SUBMODULES) -> list[SubmoduleInfo]:
    cp = _read_gitmodules(root)
    found: dict[str, SubmoduleInfo] = {}
    for section in cp.sections():
        path = cp.get(section, "path", fallback="")
        if not path:
            continue
        name = path
        m = re.search(r'"([^"]+)"', section)
        if m:
            name = m.group(1)
        url = cp.get(section, "url", fallback="").strip()
        if not url:
            url = _submodule_remote_url(root, path) or _default_url_for_path(path)
        found[path] = SubmoduleInfo(
            name=name,
            path=path,
            url=url,
            branch=cp.get(section, "branch", fallback=""),
            sha=_submodule_sha(root, path),
            present=(root / path).exists(),
            initialized=(root / path / ".git").exists()
            or ((root / path).is_dir() and (root / ".git" / "modules" / path).exists()),
            checkout_branch=current_branch(root / path) or ""
            if (root / path).is_dir()
            else "",
        )
    # Ensure known slots show up even if missing from .gitmodules
    for path in known:
        if path not in found:
            url = _submodule_remote_url(root, path) or _default_url_for_path(path)
            present = (root / path).exists()
            found[path] = SubmoduleInfo(
                name=path,
                path=path,
                url=url,
                present=present,
                initialized=(root / path / ".git").exists(),
                sha=_submodule_sha(root, path) if present else "",
                checkout_branch=(current_branch(root / path) or "") if present else "",
            )
    # Stable order: known first, then others
    ordered: list[SubmoduleInfo] = []
    for path in known:
        if path in found:
            ordered.append(found.pop(path))
    ordered.extend(sorted(found.values(), key=lambda s: s.path))
    return ordered


def resolve_psxrecomp_dir(root: Path) -> Path | None:
    """Return the psxrecomp checkout: either ``root`` itself or ``root/psxrecomp``."""
    root = root.expanduser().resolve()
    if (root / "runtime" / "runtime.cmake").is_file():
        return root
    nested = root / "psxrecomp"
    if (nested / "runtime" / "runtime.cmake").is_file():
        return nested
    if nested.is_dir() and (nested / ".git").exists():
        return nested
    return None


def list_nested_modules(root: Path) -> list[SubmoduleInfo]:
    psx = resolve_psxrecomp_dir(root)
    if psx is None:
        return []
    return _list_submodules(psx, known=NESTED_PATHS)


def repo_status(root: Path) -> RepoStatus:
    root = root.expanduser().resolve()
    st = RepoStatus(root=str(root), is_git=_is_git_repo(root), gh_available=bool(_which_gh()))
    if not st.is_git:
        st.notes.append("Not a git repository.")
        return st

    code, branch, _ = _git(root, "rev-parse", "--abbrev-ref", "HEAD")
    st.branch = branch if code == 0 else ""

    code, upstream, _ = _git(root, "rev-parse", "--abbrev-ref", "@{upstream}")
    if code == 0:
        st.upstream = upstream
        code2, ab, _ = _git(root, "rev-list", "--left-right", "--count", f"{upstream}...HEAD")
        if code2 == 0:
            parts = ab.split()
            if len(parts) == 2:
                st.behind = int(parts[0])
                st.ahead = int(parts[1])

    code, porcelain, _ = _git(root, "status", "--porcelain")
    if code == 0:
        lines = [ln for ln in porcelain.splitlines() if ln.strip()]
        st.short_status = "\n".join(lines[:40])
        for ln in lines:
            xy = ln[:2]
            if ln.startswith("??"):
                st.untracked += 1
            else:
                if xy[0] not in (" ", "?"):
                    st.staged += 1
                if xy[1] not in (" ", "?"):
                    st.unstaged += 1
        st.dirty = bool(lines)

    code, url, _ = _git(root, "remote", "get-url", "origin")
    if code == 0:
        st.remote_url = url

    if st.gh_available:
        code, out, err = _run(
            ["gh", "repo", "view", "--json", "nameWithOwner", "-q", ".nameWithOwner"],
            root,
        )
        if code == 0 and out:
            st.gh_repo = out.strip()
        elif err:
            st.notes.append("gh present but not authenticated / no GitHub remote.")

    st.submodules = _list_submodules(root)
    psx = resolve_psxrecomp_dir(root)
    if psx is not None:
        st.psxrecomp_root = str(psx)
        if psx == root:
            # Engine checkout: top-level known slots are the nested libs.
            st.submodules = _list_submodules(root, known=NESTED_PATHS)
            st.nested_submodules = list(st.submodules)
            st.notes.append("Root is a psxrecomp checkout (nested modules are direct).")
        else:
            st.nested_submodules = list_nested_modules(root)
    return st


def set_submodule_url(
    root: Path,
    path: str,
    url: str,
    *,
    dry_run: bool = False,
) -> CmdResult:
    """Write ``url`` into ``.gitmodules`` for ``path`` (create section if needed)."""
    root = root.expanduser().resolve()
    url = url.strip()
    if not url:
        return CmdResult(False, "URL required")
    path = path.strip().replace("\\", "/")
    cp = _read_gitmodules(root)
    section = _section_for_path(cp, path)
    if section is None:
        if not (root / path).exists():
            return CmdResult(False, f"No .gitmodules entry for {path}")
        section = f'submodule "{path}"'
        cp.add_section(section)
        cp.set(section, "path", path)
    prev = cp.get(section, "url", fallback="").strip()
    if prev == url:
        return CmdResult(True, f"{path} url already set")
    cp.set(section, "url", url)
    _write_gitmodules(root, cp, dry_run=dry_run)
    if not dry_run:
        _git(root, "config", "-f", ".gitmodules", f"submodule.{path}.url", url)
        _git(root, "submodule", "sync", "--", path)
    return CmdResult(True, f"Set {path} url → {url}")


def ensure_submodule(
    root: Path,
    path: str,
    *,
    url: str,
    branch: str = DEFAULT_BRANCH,
    dry_run: bool = False,
) -> CmdResult:
    root = root.expanduser().resolve()
    if not _is_git_repo(root):
        return CmdResult(False, "Not a git repository")
    dest = root / path
    cp = _read_gitmodules(root)
    already_registered = _section_for_path(cp, path) is not None
    looks_present = dest.exists() and (
        (dest / ".git").exists()
        or (dest / "CMakeLists.txt").is_file()
        or (dest / "runtime" / "runtime.cmake").is_file()
        or any(dest.iterdir())
    )
    if already_registered or looks_present:
        notes: list[str] = []
        # Heal missing .gitmodules url (common when a submodule was added by hand).
        section = _section_for_path(cp, path)
        have_url = ""
        if section:
            have_url = cp.get(section, "url", fallback="").strip()
        if not have_url and url:
            url_r = set_submodule_url(root, path, url, dry_run=dry_run)
            notes.append(url_r.message)
        set_r = set_submodule_branch(root, path, branch, dry_run=dry_run)
        notes.append(set_r.message)
        return CmdResult(
            True,
            f"{path} already present",
            "; ".join(n for n in notes if n),
        )

    code, out, err = _git(
        root,
        "submodule",
        "add",
        "-b",
        branch,
        url,
        path,
        dry_run=dry_run,
    )
    if code != 0:
        return CmdResult(False, f"Failed to add {path}", err or out)
    _git(root, "submodule", "update", "--init", "--recursive", path, dry_run=dry_run)
    return CmdResult(True, f"Added submodule {path} (branch {branch})", out)


def ensure_known_submodules(
    root: Path,
    *,
    psxrecomp_branch: str = DEFAULT_BRANCH,
    recomp_ui_branch: str = DEFAULT_BRANCH,
    dry_run: bool = False,
) -> list[CmdResult]:
    return [
        ensure_submodule(
            root,
            "psxrecomp",
            url=DEFAULT_PSXRECOMP_URL,
            branch=psxrecomp_branch or DEFAULT_BRANCH,
            dry_run=dry_run,
        ),
        ensure_submodule(
            root,
            "recomp-ui",
            url=DEFAULT_RECOMP_UI_URL,
            branch=recomp_ui_branch or DEFAULT_BRANCH,
            dry_run=dry_run,
        ),
    ]


def ensure_nested_modules(
    root: Path,
    *,
    recomp_net_branch: str = DEFAULT_NESTED_BRANCH,
    rbengine_branch: str = DEFAULT_NESTED_BRANCH,
    dry_run: bool = False,
) -> list[CmdResult]:
    """Ensure ``lib/recomp-net`` + ``lib/retcomm-rbengine`` inside psxrecomp."""
    psx = resolve_psxrecomp_dir(root)
    if psx is None:
        return [CmdResult(False, "No psxrecomp checkout found (need root or root/psxrecomp)")]
    if not _is_git_repo(psx):
        return [CmdResult(False, f"psxrecomp is not a git repo: {psx}")]

    branch_by_path = {
        "lib/recomp-net": recomp_net_branch or DEFAULT_NESTED_BRANCH,
        "lib/retcomm-rbengine": rbengine_branch or DEFAULT_NESTED_BRANCH,
    }
    results: list[CmdResult] = []
    for path, url, default_branch in KNOWN_NESTED_SUBMODULES:
        results.append(
            ensure_submodule(
                psx,
                path,
                url=url,
                branch=branch_by_path.get(path, default_branch),
                dry_run=dry_run,
            )
        )
    return results


def update_nested_modules(
    root: Path,
    *,
    paths: list[str] | None = None,
    remote: bool = False,
    stage: bool = True,
    dry_run: bool = False,
) -> CmdResult:
    """Update nested modules inside psxrecomp; optionally stage gitlinks there."""
    psx = resolve_psxrecomp_dir(root)
    if psx is None:
        return CmdResult(False, "No psxrecomp checkout found")
    want = paths or list(NESTED_PATHS)
    # Allow callers to pass game-relative paths
    normalized: list[str] = []
    for p in want:
        p = p.strip().replace("\\", "/")
        if p.startswith("psxrecomp/"):
            p = p[len("psxrecomp/") :]
        normalized.append(p)

    r = update_submodules(psx, paths=normalized, remote=remote, dry_run=dry_run)
    if not r.ok:
        return r
    if stage and not dry_run:
        code, out, err = _git(psx, "add", "--", *normalized)
        if code != 0:
            return CmdResult(
                False,
                "Nested update ok but failed to stage gitlinks in psxrecomp",
                err or out,
            )
        detail = (r.detail + "\n" if r.detail else "") + "staged in psxrecomp: " + ", ".join(
            normalized
        )
        return CmdResult(
            True,
            r.message + " (staged in psxrecomp)",
            detail.strip(),
        )
    if stage and dry_run:
        return CmdResult(
            True,
            r.message + " (would stage in psxrecomp)",
            r.detail,
        )
    return r


def commit_nested(
    root: Path,
    message: str,
    *,
    dry_run: bool = False,
) -> CmdResult:
    """Commit inside the psxrecomp checkout (nested gitlink bumps)."""
    psx = resolve_psxrecomp_dir(root)
    if psx is None:
        return CmdResult(False, "No psxrecomp checkout found")
    r = commit_all(psx, message, dry_run=dry_run)
    if r.ok:
        r = CmdResult(r.ok, f"psxrecomp: {r.message}", r.detail)
    return r


def set_nested_branch(
    root: Path,
    path: str,
    branch: str,
    *,
    dry_run: bool = False,
) -> CmdResult:
    psx = resolve_psxrecomp_dir(root)
    if psx is None:
        return CmdResult(False, "No psxrecomp checkout found")
    path = path.strip().replace("\\", "/")
    if path.startswith("psxrecomp/"):
        path = path[len("psxrecomp/") :]
    return set_submodule_branch(psx, path, branch, dry_run=dry_run)


def set_submodule_branch(
    root: Path,
    path: str,
    branch: str,
    *,
    dry_run: bool = False,
) -> CmdResult:
    root = root.expanduser().resolve()
    branch = branch.strip()
    if not branch:
        return CmdResult(False, "Branch name required")
    cp = _read_gitmodules(root)
    section = _section_for_path(cp, path)
    if section is None:
        # Create section if submodule dir exists
        if not (root / path).exists():
            return CmdResult(False, f"No .gitmodules entry for {path}")
        section = f'submodule "{path}"'
        cp.add_section(section)
        cp.set(section, "path", path)
        # Try to keep existing url from git config
        code, url, _ = _git(root, "config", "-f", ".gitmodules", f"submodule.{path}.url")
        if code == 0 and url:
            cp.set(section, "url", url)
    cp.set(section, "branch", branch)
    _write_gitmodules(root, cp, dry_run=dry_run)
    if not dry_run:
        _git(root, "config", "-f", ".gitmodules", f"submodule.{path}.branch", branch)
        # Sync into local git config
        _git(root, "submodule", "sync", "--", path)
    return CmdResult(True, f"Set {path} tracking branch → {branch}")


def resolve_default_branch(root: Path, *, remote: str = "origin") -> str | None:
    """Best-effort default branch for a checkout (``origin/HEAD``, else main/master)."""
    root = root.expanduser().resolve()
    if not _is_git_repo(root):
        return None
    code, out, _err = _git(
        root,
        "symbolic-ref",
        "--quiet",
        "--short",
        f"refs/remotes/{remote}/HEAD",
    )
    if code == 0:
        ref = (out or "").strip()
        if ref:
            # origin/main → main
            if "/" in ref:
                return ref.split("/", 1)[-1]
            return ref
    for name in ("main", "master"):
        code, _o, _e = _git(
            root, "rev-parse", "--verify", "--quiet", f"refs/remotes/{remote}/{name}"
        )
        if code == 0:
            return name
        code, _o, _e = _git(
            root, "rev-parse", "--verify", "--quiet", f"refs/heads/{name}"
        )
        if code == 0:
            return name
    code, out, _err = _git(root, "rev-parse", "--abbrev-ref", "HEAD")
    if code == 0:
        head = (out or "").strip()
        if head and head != "HEAD":
            return head
    return None


def is_default_branch_token(branch: str | None) -> bool:
    """True for empty / ``(default)`` / similar UI sentinels."""
    s = (branch or "").strip().lower()
    if not s:
        return True
    if s.startswith("(") and "default" in s:
        return True
    return s in ("default", "auto")


def _git_switch(
    cwd: Path,
    *args: str,
    dry_run: bool = False,
) -> tuple[int, str, str]:
    """Run ``git switch``; fall back to ``git checkout`` on older Git."""
    code, out, err = _git(cwd, "switch", *args, dry_run=dry_run)
    if dry_run or code == 0:
        return code, out, err
    err_l = f"{err}\n{out}".lower()
    if "not a git command" not in err_l and "unknown command" not in err_l:
        return code, out, err
    checkout_args: list[str] = []
    i = 0
    a = list(args)
    while i < len(a):
        if a[i] in ("--guess", "--no-guess"):
            i += 1
            continue
        if a[i] == "-c":
            checkout_args.append("-b")
            i += 1
            continue
        if a[i] == "-C":
            checkout_args.append("-B")
            i += 1
            continue
        if a[i] == "--track" and i + 1 < len(a):
            checkout_args.extend(["--track", a[i + 1]])
            i += 2
            continue
        checkout_args.append(a[i])
        i += 1
    return _git(cwd, "checkout", *checkout_args, dry_run=False)


def switch_branch(
    root: Path,
    branch: str,
    *,
    create: bool = False,
    dry_run: bool = False,
    prefer_default: bool = True,
) -> CmdResult:
    """``git switch`` onto ``branch`` (guess remote-tracking; optional ``-c``).

    Empty / ``(default)`` resolves each repo's default branch (``origin/HEAD``,
    else main/master). When ``prefer_default`` is set and a named branch is
    missing, falls back to that default instead of failing.
    """
    root = root.expanduser().resolve()
    if not _is_git_repo(root):
        return CmdResult(False, "Not a git repository")

    requested = (branch or "").strip()
    used_default_token = is_default_branch_token(requested)
    if used_default_token:
        resolved = resolve_default_branch(root)
        if not resolved:
            return CmdResult(
                False,
                "Could not resolve default branch (fetch remotes / set origin/HEAD)",
            )
        target = resolved
        note = "default"
    else:
        target = requested
        note = ""

    current = current_branch(root)
    if current == target:
        suffix = f" ({note})" if note else ""
        return CmdResult(True, f"Already on {target}{suffix}")

    verb = "Would switch" if dry_run else "Switched"
    if dry_run:
        # Validate the ref exists so dry-run matches real switch + default fallback.
        code_v, _, _ = _git(
            root, "rev-parse", "--verify", "--quiet", f"refs/heads/{target}"
        )
        if code_v != 0:
            code_v, _, _ = _git(
                root,
                "rev-parse",
                "--verify",
                "--quiet",
                f"refs/remotes/origin/{target}",
            )
        if code_v == 0:
            suffix = f" ({note})" if note else ""
            return CmdResult(True, f"{verb} to {target}{suffix}")
        # fall through to create / prefer_default handling below
        code, out, err = 1, "", f"fatal: invalid reference: {target}"
    else:
        code, out, err = _git_switch(root, "--guess", target, dry_run=False)
        if code == 0:
            suffix = f" ({note})" if note else ""
            return CmdResult(True, f"{verb} to {target}{suffix}", out)

    if create and not used_default_token:
        code2, out2, err2 = _git_switch(root, "-c", target, dry_run=dry_run)
        if code2 == 0:
            return CmdResult(True, f"{verb} to new branch {target}", out2)
        return CmdResult(
            False,
            f"Could not create/switch to {target}",
            err2 or out2 or err or out,
        )

    # Named branch missing → prefer this checkout's default branch.
    if prefer_default and not used_default_token and not create:
        fallback = resolve_default_branch(root)
        if fallback and fallback != target:
            if current == fallback:
                return CmdResult(
                    True,
                    f"Already on {fallback} (default; {target} missing)",
                )
            if dry_run:
                return CmdResult(
                    True,
                    f"{verb} to {fallback} (default; {target} missing)",
                )
            code_f, out_f, err_f = _git_switch(
                root, "--guess", fallback, dry_run=False
            )
            if code_f == 0:
                return CmdResult(
                    True,
                    f"{verb} to {fallback} (default; {target} missing)",
                    out_f or err or out,
                )
            err = err_f or out_f or err

    hint = " (enable Create / --create to start a new branch)"
    return CmdResult(False, f"Could not switch to {target}{hint}", err or out)


def set_repo_branch(
    root: Path,
    branch: str,
    *,
    create: bool = False,
    dry_run: bool = False,
) -> CmdResult:
    """Switch the repo working tree (``git switch``). Kept for CLI compatibility."""
    return switch_branch(root, branch, create=create, dry_run=dry_run)


def list_branches(
    repo: Path,
    *,
    remotes: bool = True,
    fetch: bool = False,
) -> list[str]:
    """Return sorted local (+ remote-tracking) branch short names for a repo."""
    repo = repo.expanduser().resolve()
    if not repo.is_dir() or not _is_git_repo(repo):
        return []
    if fetch:
        _git(repo, "fetch", "--prune", "--quiet")
    names: set[str] = set()
    code, out, _ = _git(repo, "for-each-ref", "--format=%(refname:short)", "refs/heads")
    if code == 0:
        for line in out.splitlines():
            n = line.strip()
            if n and n != "HEAD":
                names.add(n)
    if remotes:
        code, out, _ = _git(
            repo, "for-each-ref", "--format=%(refname:short)", "refs/remotes"
        )
        if code == 0:
            for line in out.splitlines():
                n = line.strip()
                if not n or n.endswith("/HEAD") or "->" in n:
                    continue
                # origin/main → main
                if "/" in n:
                    n = n.split("/", 1)[1]
                if n and n != "HEAD":
                    names.add(n)
    return _sort_branch_names(names)


def list_remote_head_branches(url: str) -> list[str]:
    """``git ls-remote --heads`` when a local checkout is unavailable."""
    url = (url or "").strip()
    if not url:
        return []
    code, out, _ = _git(Path.cwd(), "ls-remote", "--heads", url)
    if code != 0:
        return []
    names: set[str] = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        ref = parts[1]
        if ref.startswith("refs/heads/"):
            names.add(ref[len("refs/heads/") :])
    return _sort_branch_names(names)


def list_module_branches(
    root: Path,
    path: str,
    *,
    nested: bool = False,
    remotes: bool = True,
    fetch: bool = False,
    url_fallback: str = "",
) -> list[str]:
    """Branches for a game submodule or a nested module inside psxrecomp."""
    root = root.expanduser().resolve()
    path = path.strip().replace("\\", "/")
    if nested:
        psx = resolve_psxrecomp_dir(root)
        if psx is None:
            return list_remote_head_branches(url_fallback) if url_fallback else []
        if path.startswith("psxrecomp/"):
            path = path[len("psxrecomp/") :]
        owner = psx
    else:
        owner = root
    sub = owner / path
    if sub.is_dir() and _is_git_repo(sub):
        return list_branches(sub, remotes=remotes, fetch=fetch)
    # Fall back to URL from .gitmodules or caller
    url = url_fallback
    if not url:
        cp = _read_gitmodules(owner)
        section = _section_for_path(cp, path)
        if section:
            url = cp.get(section, "url", fallback="")
    return list_remote_head_branches(url)


def _sort_branch_names(names: set[str] | list[str]) -> list[str]:
    preferred = ("main", "master", "develop", "development")

    def key(s: str) -> tuple:
        s_l = s.lower()
        try:
            rank = preferred.index(s_l)
        except ValueError:
            rank = len(preferred)
        return (rank, s_l)

    return sorted(set(names), key=key)


def update_submodules(
    root: Path,
    *,
    paths: list[str] | None = None,
    remote: bool = False,
    dry_run: bool = False,
) -> CmdResult:
    root = root.expanduser().resolve()
    if not _is_git_repo(root):
        return CmdResult(False, "Not a git repository")
    cmd = ["submodule", "update", "--init", "--recursive"]
    if remote:
        cmd.append("--remote")
    if paths:
        cmd.append("--")
        cmd.extend(paths)
    code, out, err = _git(root, *cmd, dry_run=dry_run)
    if code != 0:
        return CmdResult(False, "Submodule update failed", err or out)
    mode = "remote tracking tip" if remote else "pinned gitlink"
    return CmdResult(True, f"Updated submodules ({mode})", out)


# pull() strategies — used by CLI/GUI for game root, submodules, and nested libs.
PULL_MODES = ("ff-only", "rebase", "merge", "reset")
PULL_DIRTY = ("fail", "stash", "discard")


def _upstream_ref(root: Path) -> str | None:
    """Return @{upstream} rev-parse, or origin/<branch> if branch tracks nothing yet."""
    code, out, _ = _git(root, "rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{u}")
    if code == 0:
        ref = out.strip()
        if ref:
            return ref
    branch = current_branch(root)
    if branch:
        return f"origin/{branch}"
    return None


def _working_tree_dirty(root: Path) -> bool:
    code, out, _ = _git(root, "status", "--porcelain")
    return code == 0 and bool(out.strip())


def pull(
    root: Path,
    *,
    mode: str = "ff-only",
    dirty: str = "fail",
    dry_run: bool = False,
) -> CmdResult:
    """Pull (or reset-to-upstream) for a git checkout.

    ``mode``:
      - ``ff-only`` (default): ``git pull --ff-only``
      - ``rebase``: ``git pull --rebase``
      - ``merge``: ``git pull --no-rebase``
      - ``reset``: ``git fetch`` + ``git reset --hard <upstream>`` (match GH tip)

    ``dirty`` (ignored for ``reset``, which always hard-resets):
      - ``fail`` (default): leave tree alone; git aborts if it would overwrite
      - ``stash``: ``stash push -u`` before pull, ``stash pop`` after
      - ``discard``: ``git reset --hard HEAD`` before pull (drops local edits)
    """
    root = root.expanduser().resolve()
    mode = (mode or "ff-only").strip().lower().replace("_", "-")
    dirty = (dirty or "fail").strip().lower()
    if mode == "no-rebase":
        mode = "merge"
    if mode not in PULL_MODES:
        return CmdResult(
            False,
            f"Unknown pull mode {mode!r} (want: {', '.join(PULL_MODES)})",
        )
    if dirty not in PULL_DIRTY:
        return CmdResult(
            False,
            f"Unknown dirty policy {dirty!r} (want: {', '.join(PULL_DIRTY)})",
        )
    if not _is_git_repo(root):
        return CmdResult(False, "Not a git repository")

    # --- reset: make local HEAD match upstream tip (discards local commits+edits) ---
    if mode == "reset":
        code, out, err = _git(root, "fetch", "origin", dry_run=dry_run)
        if code != 0 and not dry_run:
            return CmdResult(False, "Fetch failed", err or out)
        upstream = _upstream_ref(root)
        if not upstream:
            return CmdResult(
                False,
                "No upstream branch (set upstream or checkout a branch tracking origin)",
            )
        code, out, err = _git(root, "reset", "--hard", upstream, dry_run=dry_run)
        if code != 0:
            return CmdResult(False, f"reset --hard {upstream} failed", err or out)
        return CmdResult(True, f"Reset to {upstream}", out)

    # --- dirty working tree handling before pull ---
    stashed = False
    if dirty == "discard" and (_working_tree_dirty(root) or dry_run):
        code, out, err = _git(root, "reset", "--hard", "HEAD", dry_run=dry_run)
        if code != 0:
            return CmdResult(False, "discard (reset --hard HEAD) failed", err or out)
    elif dirty == "stash" and (_working_tree_dirty(root) or dry_run):
        code, out, err = _git(
            root, "stash", "push", "-u", "-m", "project-studio pull", dry_run=dry_run
        )
        if code != 0:
            return CmdResult(False, "stash before pull failed", err or out)
        stashed = True
    elif dirty == "fail" and _working_tree_dirty(root) and not dry_run:
        return CmdResult(
            False,
            "Pull blocked: working tree dirty "
            "(commit, or pass dirty=stash|discard, or mode=reset)",
        )

    if mode == "ff-only":
        pull_args = ("pull", "--ff-only")
        label = "ff-only"
    elif mode == "rebase":
        pull_args = ("pull", "--rebase")
        label = "rebase"
    else:  # merge
        pull_args = ("pull", "--no-rebase")
        label = "merge"

    code, out, err = _git(root, *pull_args, dry_run=dry_run)
    if code != 0:
        if stashed and not dry_run:
            _git(root, "stash", "pop")
        return CmdResult(False, f"Pull failed ({label})", err or out)

    detail = out
    if stashed and not dry_run:
        sc, so, se = _git(root, "stash", "pop")
        if sc != 0:
            return CmdResult(
                False,
                f"Pulled ({label}) but stash pop failed",
                (detail + "\n" + (se or so)).strip(),
            )
        detail = (detail + "\n" + so).strip()
    return CmdResult(True, f"Pulled ({label})", detail)


def commit_all(
    root: Path,
    message: str,
    *,
    dry_run: bool = False,
) -> CmdResult:
    root = root.expanduser().resolve()
    message = message.strip()
    if not message:
        return CmdResult(False, "Commit message required")
    code, porcelain, _ = _git(root, "status", "--porcelain")
    if code != 0:
        return CmdResult(False, "git status failed", porcelain)
    if not porcelain.strip() and not dry_run:
        return CmdResult(False, "Nothing to commit")
    code, out, err = _git(root, "add", "-A", dry_run=dry_run)
    if code != 0:
        return CmdResult(False, "git add failed", err or out)
    code, out, err = _git(root, "commit", "-m", message, dry_run=dry_run)
    if code != 0:
        return CmdResult(False, "git commit failed", err or out)
    return CmdResult(True, "Committed", out)


def push(
    root: Path,
    *,
    branch: str = "",
    dry_run: bool = False,
) -> CmdResult:
    """Push current HEAD to origin.

    If HEAD is detached, ``branch`` is required and we push
    ``HEAD:refs/heads/<branch>`` so GitHub gets a real branch ref.
    """
    root = root.expanduser().resolve()
    code, _, err = _git(root, "remote", "get-url", "origin")
    if code != 0 and not dry_run:
        return CmdResult(False, "No origin remote", err)

    local = current_branch(root)
    target = (branch or "").strip()
    if target.startswith("("):
        target = ""

    if local:
        # On a branch: normal upstream push
        code, out, err = _git(root, "push", "-u", "origin", "HEAD", dry_run=dry_run)
        if code != 0:
            return CmdResult(False, "Push failed", err or out)
        return CmdResult(True, f"Pushed {local} → origin", out)

    # Detached HEAD
    if not target:
        return CmdResult(
            False,
            "Detached HEAD — select/checkout a branch before push "
            "(or Studio will push HEAD:refs/heads/<branch> if one is selected)",
        )
    refspec = f"HEAD:refs/heads/{target}"
    code, out, err = _git(root, "push", "-u", "origin", refspec, dry_run=dry_run)
    if code != 0:
        return CmdResult(False, f"Push failed (detached → {target})", err or out)
    # Best-effort: attach local checkout to that branch so later pushes are normal
    if not dry_run:
        _git_switch(root, "-C", target)
        return CmdResult(
            True,
            f"Pushed detached HEAD → origin/{target} (switched to {target})",
            out,
        )
    return CmdResult(
        True,
        f"Would push detached HEAD → origin/{target} and switch to {target}",
        out,
    )


def _normalize_module_path(path: str, *, nested: bool) -> str:
    p = path.strip().replace("\\", "/")
    if nested and p.startswith("psxrecomp/"):
        p = p[len("psxrecomp/") :]
    return p


def resolve_module_dir(
    root: Path,
    path: str,
    *,
    nested: bool = False,
) -> Path | None:
    """Resolve a game submodule or a nested module checkout under psxrecomp."""
    root = root.expanduser().resolve()
    path = _normalize_module_path(path, nested=nested)
    if not path:
        return None
    if nested:
        psx = resolve_psxrecomp_dir(root)
        if psx is None:
            return None
        # Engine repo itself: nested paths are direct.
        owner = psx
    else:
        owner = root
    sub = owner / path
    if sub.is_dir() and _is_git_repo(sub):
        return sub
    return None


def default_module_paths(*, nested: bool = False) -> tuple[str, ...]:
    return NESTED_PATHS if nested else KNOWN_SUBMODULES


def _tracking_branch_for(owner: Path, path: str) -> str:
    cp = _read_gitmodules(owner)
    section = _section_for_path(cp, path)
    if not section:
        return ""
    return cp.get(section, "branch", fallback="").strip()


def switch_modules(
    root: Path,
    *,
    paths: list[str] | None = None,
    nested: bool = False,
    branch_by_path: dict[str, str] | None = None,
    create: bool = False,
    set_tracking: bool = True,
    dry_run: bool = False,
) -> list[CmdResult]:
    """``git switch`` inside each module checkout.

    ``branch_by_path`` overrides .gitmodules tracking. When a path has no
    explicit branch, tracking is used, then the checkout's default branch.
    ``(default)`` always means each module's own default. ``set_tracking``
    also writes ``branch =`` in .gitmodules so ``update --remote`` stays aligned.
    """
    want = paths or list(default_module_paths(nested=nested))
    branches = {
        _normalize_module_path(k, nested=nested): (v or "").strip()
        for k, v in (branch_by_path or {}).items()
    }
    results: list[CmdResult] = []
    owner: Path | None = None
    if nested:
        owner = resolve_psxrecomp_dir(root)
    else:
        owner = root.expanduser().resolve()
    for path in want:
        path = _normalize_module_path(path, nested=nested)
        branch = (branches.get(path) or "").strip()
        if is_default_branch_token(branch):
            # Blank override → prefer .gitmodules tracking, else default.
            if not branch and owner is not None:
                tracked = _tracking_branch_for(owner, path)
                if tracked and not is_default_branch_token(tracked):
                    branch = tracked
                else:
                    branch = "(default)"
            else:
                branch = "(default)"
        if not branch:
            results.append(CmdResult(False, f"{path}: no branch to switch to"))
            continue
        sub = resolve_module_dir(root, path, nested=nested)
        if sub is None:
            results.append(CmdResult(False, f"{path}: checkout missing"))
            continue
        r = switch_branch(sub, branch, create=create, dry_run=dry_run)
        msg = f"{path}: {r.message}"
        if r.ok and set_tracking:
            # Track the branch we actually landed on (may be default fallback).
            actual = ""
            if not dry_run:
                actual = current_branch(sub) or ""
            if not actual:
                # dry-run: derive from switch message when possible
                for token in ("Switched to ", "Would switch to ", "Already on "):
                    if token in (r.message or ""):
                        rest = (r.message or "").split(token, 1)[1]
                        actual = rest.split()[0] if rest else ""
                        break
            if not actual:
                actual = (
                    resolve_default_branch(sub)
                    if is_default_branch_token(branch)
                    else branch
                )
            if nested:
                tr = set_nested_branch(root, path, actual, dry_run=dry_run)
            else:
                tr = set_submodule_branch(root, path, actual, dry_run=dry_run)
            detail = "\n".join(x for x in (r.detail, tr.message) if x)
            results.append(CmdResult(tr.ok, f"{msg}; {tr.message}", detail))
        else:
            results.append(CmdResult(r.ok, msg, r.detail))
    return results


def switch_psxrecomp(
    root: Path,
    branch: str,
    *,
    create: bool = False,
    dry_run: bool = False,
) -> CmdResult:
    psx = resolve_psxrecomp_dir(root)
    if psx is None:
        return CmdResult(False, "No psxrecomp checkout found")
    r = switch_branch(psx, branch, create=create, dry_run=dry_run)
    return CmdResult(r.ok, f"psxrecomp: {r.message}", r.detail)


def pull_modules(
    root: Path,
    *,
    paths: list[str] | None = None,
    nested: bool = False,
    mode: str = "ff-only",
    dirty: str = "fail",
    dry_run: bool = False,
) -> list[CmdResult]:
    """Pull inside each module checkout (see ``pull`` for mode/dirty)."""
    want = paths or list(default_module_paths(nested=nested))
    results: list[CmdResult] = []
    for path in want:
        path = _normalize_module_path(path, nested=nested)
        sub = resolve_module_dir(root, path, nested=nested)
        if sub is None:
            results.append(CmdResult(False, f"{path}: checkout missing"))
            continue
        r = pull(sub, mode=mode, dirty=dirty, dry_run=dry_run)
        results.append(CmdResult(r.ok, f"{path}: {r.message}", r.detail))
    return results


def push_modules(
    root: Path,
    *,
    paths: list[str] | None = None,
    nested: bool = False,
    branch_by_path: dict[str, str] | None = None,
    dry_run: bool = False,
) -> list[CmdResult]:
    """Push each module checkout to origin (handles detached HEAD via branch map)."""
    want = paths or list(default_module_paths(nested=nested))
    branches = branch_by_path or {}
    results: list[CmdResult] = []
    for path in want:
        path = _normalize_module_path(path, nested=nested)
        sub = resolve_module_dir(root, path, nested=nested)
        if sub is None:
            results.append(CmdResult(False, f"{path}: checkout missing"))
            continue
        r = push(sub, branch=branches.get(path, ""), dry_run=dry_run)
        results.append(CmdResult(r.ok, f"{path}: {r.message}", r.detail))
    return results


def commit_modules(
    root: Path,
    message: str,
    *,
    paths: list[str] | None = None,
    nested: bool = False,
    dry_run: bool = False,
) -> list[CmdResult]:
    """``git add -A && git commit`` inside each module checkout."""
    message = message.strip()
    if not message:
        return [CmdResult(False, "Commit message required")]
    want = paths or list(default_module_paths(nested=nested))
    results: list[CmdResult] = []
    for path in want:
        path = _normalize_module_path(path, nested=nested)
        sub = resolve_module_dir(root, path, nested=nested)
        if sub is None:
            results.append(CmdResult(False, f"{path}: checkout missing"))
            continue
        r = commit_all(sub, message, dry_run=dry_run)
        results.append(CmdResult(r.ok, f"{path}: {r.message}", r.detail))
    return results


def pull_psxrecomp(
    root: Path,
    *,
    mode: str = "ff-only",
    dirty: str = "fail",
    dry_run: bool = False,
) -> CmdResult:
    psx = resolve_psxrecomp_dir(root)
    if psx is None:
        return CmdResult(False, "No psxrecomp checkout found")
    r = pull(psx, mode=mode, dirty=dirty, dry_run=dry_run)
    return CmdResult(r.ok, f"psxrecomp: {r.message}", r.detail)


def push_psxrecomp(
    root: Path,
    *,
    branch: str = "",
    dry_run: bool = False,
) -> CmdResult:
    psx = resolve_psxrecomp_dir(root)
    if psx is None:
        return CmdResult(False, "No psxrecomp checkout found")
    r = push(psx, branch=branch, dry_run=dry_run)
    return CmdResult(r.ok, f"psxrecomp: {r.message}", r.detail)


def install_and_push_release_ci(
    root: Path,
    *,
    zip_prefix: str = "",
    force: bool = False,
    push_remote: bool = True,
    dry_run: bool = False,
) -> CmdResult:
    """Write psxrecomp setup-release.yml (+ packager), commit, and push.

    Uses the same template as ``setup_project --enable-ci`` /
    ``op_emit_ci_workflow``. After push, best-effort checks that Actions has
    registered ``release.yml`` (nudge commit if needed).
    """
    from .models import MigrateOptions
    from .ops import op_emit_ci_workflow, op_emit_packager

    root = root.expanduser().resolve()
    if not _is_git_repo(root):
        return CmdResult(False, "Not a git repository")

    opts = MigrateOptions(
        zip_prefix=zip_prefix.strip() or None,
        enable_ci=True,
        force=force,
        dry_run=dry_run,
    )
    pack = op_emit_packager(root, opts)
    if not pack.ok:
        return CmdResult(False, f"Packager: {pack.message}")
    ci = op_emit_ci_workflow(root, opts)
    if not ci.ok:
        return CmdResult(False, f"CI workflow: {ci.message}")

    paths = sorted({*pack.changed_paths, *ci.changed_paths})
    if not paths and not dry_run:
        # Already present — still allow push of any uncommitted workflow edits.
        wf = root / ".github" / "workflows" / "release.yml"
        pkg = root / "scripts" / "package_setup_release.sh"
        for p in (wf, pkg):
            if p.is_file():
                rel = str(p.relative_to(root)).replace("\\", "/")
                if rel not in paths:
                    paths.append(rel)

    if dry_run:
        return CmdResult(
            True,
            "dry-run: would install CI + commit + push\n"
            + "\n".join(f"  {p}" for p in paths),
        )

    if not paths:
        return CmdResult(False, "No CI files to commit")

    code, out, err = _git(root, "add", "--", *paths)
    if code != 0:
        return CmdResult(False, "git add failed", err or out)

    code, porcelain, _ = _git(root, "diff", "--cached", "--name-only")
    staged = [ln.strip() for ln in porcelain.splitlines() if ln.strip()]
    if not staged:
        # Nothing new staged — still try push so remote gets existing commits.
        detail = f"{pack.op_id}: {pack.message}; {ci.op_id}: {ci.message}"
        if not push_remote:
            return CmdResult(True, "CI already up to date (nothing to commit)", detail)
        push_r = push(root, dry_run=False)
        if not push_r.ok:
            return CmdResult(
                True,
                "CI already up to date locally; push skipped/failed",
                (detail + "\n" + push_r.message).strip(),
            )
        nudge = ensure_actions_registers_release_yml(root)
        msg = "CI already up to date; pushed existing commits"
        if nudge.message:
            msg += f"\n{nudge.message}"
        return CmdResult(True, msg, detail)

    code, out, err = _git(
        root,
        "commit",
        "-m",
        "ci: add setup-host release.yml (psxrecomp template)",
    )
    if code != 0:
        return CmdResult(False, "git commit failed", err or out)

    if not push_remote:
        return CmdResult(True, "Installed + committed release CI (not pushed)", out)

    push_r = push(root, dry_run=False)
    if not push_r.ok:
        return CmdResult(False, f"Committed CI but push failed: {push_r.message}", push_r.detail)

    nudge = ensure_actions_registers_release_yml(root)
    msg = f"Installed + pushed release CI\n{push_r.message}"
    if nudge.message:
        msg += f"\n{nudge.message}"
    return CmdResult(True, msg, out)


def ensure_actions_registers_release_yml(root: Path) -> CmdResult:
    """Poll Actions for release.yml; nudge with an empty commit if missing.

    Mirrors setup_project.sh ensure_actions_registers_release_yml.
    """
    root = root.expanduser().resolve()
    if not _which_gh():
        return CmdResult(False, "gh CLI not found — skip Actions registration check")

    wf_path = ".github/workflows/release.yml"
    if not (root / wf_path).is_file():
        return CmdResult(False, "release.yml missing locally")

    import time

    for _ in range(6):
        name = release_workflow_name(root)
        if name:
            return CmdResult(
                True,
                f"Actions workflow registered: {name}\n"
                "(runs on workflow_dispatch or push of v* tags)",
            )
        time.sleep(2)

    # Nudge: empty commit touching the workflow mtime via amend-free empty commit
    code, out, err = _git(
        root,
        "commit",
        "--allow-empty",
        "-m",
        "ci: nudge Actions to register release.yml",
    )
    if code != 0:
        return CmdResult(
            False,
            "Actions has not listed release.yml yet; nudge commit failed",
            err or out,
        )
    push_r = push(root, dry_run=False)
    if not push_r.ok:
        return CmdResult(
            False,
            "Nudge committed but push failed",
            push_r.detail or push_r.message,
        )

    for _ in range(8):
        time.sleep(2)
        name = release_workflow_name(root)
        if name:
            return CmdResult(
                True,
                f"Actions workflow registered after nudge: {name}\n"
                "(runs on workflow_dispatch or push of v* tags)",
            )
    return CmdResult(
        False,
        "release.yml nudged but Actions still has not listed it; "
        "open the repo Actions tab or re-run Install & push CI later",
    )


def release_workflow_name(root: Path) -> str | None:
    """Return registered Actions workflow name for release.yml, if any."""
    if not _which_gh():
        return None
    code, out, _ = _run(
        [
            "gh",
            "api",
            "repos/{owner}/{repo}/actions/workflows",
            "--jq",
            '.workflows[] | select(.path|endswith("release.yml")) | .name',
        ],
        root,
    )
    if code != 0:
        return None
    for line in out.splitlines():
        name = line.strip()
        if name:
            return name
    return None


def run_release_workflow(
    root: Path,
    *,
    version: str = "",
    bump: str = "patch",
    publish: bool = True,
    reuse_cached_emitters: bool = True,
    dry_run: bool = False,
) -> CmdResult:
    root = root.expanduser().resolve()
    if not _which_gh():
        return CmdResult(False, "gh CLI not found — install GitHub CLI and auth login")
    if bump not in ("patch", "minor", "major"):
        return CmdResult(False, f"Invalid bump: {bump}")

    wf = root / ".github" / "workflows" / "release.yml"
    if not wf.is_file() and not dry_run:
        return CmdResult(False, "Missing .github/workflows/release.yml")

    # Prefer workflow file path; gh accepts it.
    cmd = [
        "gh",
        "workflow",
        "run",
        "release.yml",
        "-f",
        f"version={version}",
        "-f",
        f"bump={bump}",
        "-f",
        f"publish={'true' if publish else 'false'}",
        "-f",
        f"reuse_cached_emitters={'true' if reuse_cached_emitters else 'false'}",
    ]
    if dry_run:
        return CmdResult(True, "dry-run: " + " ".join(cmd))

    code, out, err = _run(cmd, root)
    if code != 0:
        return CmdResult(False, "Failed to dispatch release workflow", err or out)

    # Best-effort: fetch latest run URL
    run_url = ""
    code2, runs_json, _ = _run(
        [
            "gh",
            "run",
            "list",
            "--workflow",
            "release.yml",
            "--limit",
            "1",
            "--json",
            "url,databaseId,status",
        ],
        root,
    )
    if code2 == 0 and runs_json:
        try:
            runs = json.loads(runs_json)
            if runs:
                run_url = runs[0].get("url") or ""
        except json.JSONDecodeError:
            pass

    msg = "Dispatched Release builds workflow"
    if run_url:
        msg += f"\n{run_url}"
    return CmdResult(True, msg, out)
