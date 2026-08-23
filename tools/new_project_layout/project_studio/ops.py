"""Apply migration plan steps (setup-host exclusively)."""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

from fill_tokens import derive_zip_prefix

from .models import ApplyResult, MigrateOptions, Plan
from .naming import (
    boot_exe_from_game_toml,
    build_token_map,
    entry_pc_from_game_toml,
    game_name_from_project,
    infer_project_name,
    normalize_lobby_url,
    players_from_cmake,
    window_title_from_cmake,
    window_title_from_name,
)
from .paths import ci_setup_release_template, psxrecomp_root_from_toolkit, templates_dir, toolkit_dir


def _run(cmd: list[str], cwd: Path, dry_run: bool) -> tuple[bool, str]:
    if dry_run:
        return True, "dry-run: " + " ".join(cmd)
    try:
        proc = subprocess.run(
            cmd,
            cwd=str(cwd),
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError as exc:
        return False, str(exc)
    if proc.returncode != 0:
        err = (proc.stderr or proc.stdout or "").strip()
        return False, err or f"exit {proc.returncode}"
    return True, (proc.stdout or "").strip()


def _write(path: Path, text: str, dry_run: bool) -> None:
    if dry_run:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def _fill(src: Path, dst: Path, tokens: dict[str, str], dry_run: bool, *, ci: bool = False) -> None:
    text = src.read_text(encoding="utf-8")
    for k, v in tokens.items():
        text = text.replace(f"@{k}@", v)
    if ci:
        zp = tokens.get("ZIP_PREFIX", "game")
        title = tokens.get("GAME_TITLE") or tokens.get("WINDOW_TITLE") or "Game"
        title = " ".join(title.split())
        title_yaml = (
            title.replace("\\", "\\\\").replace('"', '\\"').replace("\n", " ").replace("\r", "")
        )
        text = text.replace("YOUR_ZIP_PREFIX", zp)
        text = text.replace("YOUR_GAME_TITLE", title_yaml)
        text = text.replace("yourgame-release", f"{zp}-release")
    _write(dst, text, dry_run)


def _is_real_psxrecomp(path: Path) -> bool:
    return (path / "runtime" / "runtime.cmake").is_file()


def _strip_gitmodules_submodule(text: str, name: str) -> str:
    """Remove a ``[submodule "name"]`` block (and its indented keys) from .gitmodules."""
    lines = text.splitlines(keepends=True)
    out: list[str] = []
    skipping = False
    header = f'[submodule "{name}"]'
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("[submodule ") and stripped.endswith("]"):
            skipping = stripped == header
            if skipping:
                continue
        if skipping:
            # Indented key = value lines belong to the skipped block.
            if line[:1] in (" ", "\t") or stripped == "":
                continue
            skipping = False
        if not skipping:
            out.append(line)
    return "".join(out)


def _remove_psxrecomp_v4(root: Path, options: MigrateOptions) -> tuple[bool, str, list[str]]:
    """Drop legacy ``psxrecomp-v4`` (submodule or plain tree). Prefer keeping ``psxrecomp/``."""
    v4 = root / "psxrecomp-v4"
    changed: list[str] = []
    if not v4.exists():
        return True, "psxrecomp-v4 already absent", changed

    notes: list[str] = []
    if options.dry_run:
        return True, "dry-run: remove psxrecomp-v4 (deinit/rm or rmtree)", ["psxrecomp-v4"]

    # Registered submodule: deinit + git rm cleans the gitlink and worktree.
    gm = root / ".gitmodules"
    gm_text = gm.read_text(encoding="utf-8") if gm.is_file() else ""
    registered = 'path = psxrecomp-v4' in gm_text or '[submodule "psxrecomp-v4"]' in gm_text

    if registered or (v4 / ".git").exists():
        ok, out = _run(
            ["git", "submodule", "deinit", "-f", "psxrecomp-v4"], root, False
        )
        if out:
            notes.append(out if ok else f"deinit: {out}")
        ok_rm, out_rm = _run(["git", "rm", "-f", "psxrecomp-v4"], root, False)
        if ok_rm:
            notes.append(out_rm or "git rm psxrecomp-v4")
            changed.append("psxrecomp-v4")
        else:
            # Fallback: force-remove the directory if git rm refused (untracked tree).
            notes.append(f"git rm: {out_rm}")
            try:
                if v4.is_symlink() or v4.is_file():
                    v4.unlink()
                elif v4.is_dir():
                    shutil.rmtree(v4)
                changed.append("psxrecomp-v4")
                notes.append("removed psxrecomp-v4 via rmtree")
            except OSError as exc:
                return False, f"could not remove psxrecomp-v4: {exc}", changed
    else:
        try:
            if v4.is_symlink() or v4.is_file():
                v4.unlink()
            elif v4.is_dir():
                shutil.rmtree(v4)
            changed.append("psxrecomp-v4")
            notes.append("removed untracked psxrecomp-v4/")
        except OSError as exc:
            return False, f"could not remove psxrecomp-v4: {exc}", changed

    # Ensure .gitmodules no longer lists the legacy path (git rm usually does this).
    if gm.is_file():
        text = gm.read_text(encoding="utf-8")
        new = _strip_gitmodules_submodule(text, "psxrecomp-v4")
        new = new.replace("path = psxrecomp-v4\n", "")
        if new != text:
            _write(gm, new, False)
            if ".gitmodules" not in changed:
                changed.append(".gitmodules")
            notes.append("stripped psxrecomp-v4 from .gitmodules")

    # Drop stale git modules metadata if present.
    mod_meta = root / ".git" / "modules" / "psxrecomp-v4"
    if mod_meta.is_dir():
        try:
            shutil.rmtree(mod_meta)
            notes.append("removed .git/modules/psxrecomp-v4")
        except OSError:
            pass

    return True, "; ".join(n for n in notes if n) or "removed psxrecomp-v4", changed


def _resolve_tokens(root: Path, options: MigrateOptions) -> dict[str, str]:
    name = options.project_name or infer_project_name(root)
    boot = (
        options.boot_exe
        or boot_exe_from_game_toml(root / "game.toml")
        or "SLUS_01234"
    )
    players = options.players
    if players == 2:
        detected = players_from_cmake(root / "CMakeLists.txt")
        if detected is not None:
            players = detected
        else:
            from .naming import players_from_game_toml

            detected = players_from_game_toml(root / "game.toml")
            if detected is not None:
                players = detected
    title = options.window_title or window_title_from_cmake(root / "CMakeLists.txt")
    if not title:
        # Prefer game.toml window_title
        gt = (root / "game.toml").read_text(encoding="utf-8", errors="replace") if (
            root / "game.toml"
        ).is_file() else ""
        for line in gt.splitlines():
            if line.strip().startswith("window_title"):
                _, _, v = line.partition("=")
                title = v.strip().strip('"').strip("'")
                break
    if not title:
        title = window_title_from_name(name)

    has_boxart = (root / "launcher_assets" / "img" / "boxart.tga").is_file()
    # Also treat pending relocate as has_boxart for cmake wiring after relocate runs first
    if not has_boxart:
        for p in (
            root / "recomp" / "launcher" / "boxart.tga",
            root / "recomp" / "launcher" / "boxart.png",
        ):
            if p.is_file():
                has_boxart = options.relocate_boxart
                break

    enable_netplay = options.enable_netplay and players >= 2
    return build_token_map(
        name=name,
        boot_exe=boot,
        players=players,
        zip_prefix=options.zip_prefix or derive_zip_prefix(name),
        window_title=title,
        entry_pc=entry_pc_from_game_toml(root / "game.toml"),
        lobby_url=normalize_lobby_url(options.lobby_url),
        enable_recomp_ui=options.enable_recomp_ui,
        enable_wizard=options.enable_wizard if options.enable_recomp_ui else False,
        enable_netplay=enable_netplay if options.enable_recomp_ui else False,
        has_boxart=has_boxart,
        github_owner=options.github_owner,
        github_repo=options.github_repo,
    )


def op_rename_psxrecomp_submodule(root: Path, options: MigrateOptions) -> ApplyResult:
    """Consolidate on ``psxrecomp/``: promote v4 if needed, always delete leftover v4."""
    v4 = root / "psxrecomp-v4"
    dest = root / "psxrecomp"
    changed: list[str] = []
    parts: list[str] = []

    if not v4.exists() and _is_real_psxrecomp(dest):
        # Still scrub orphan .gitmodules / CMake references to the old name.
        scrubbed: list[str] = []
        gm = root / ".gitmodules"
        if gm.is_file():
            text = gm.read_text(encoding="utf-8")
            new = _strip_gitmodules_submodule(text, "psxrecomp-v4")
            if new != text:
                _write(gm, new, options.dry_run)
                scrubbed.append(".gitmodules")
        cmake = root / "CMakeLists.txt"
        if cmake.is_file():
            text = cmake.read_text(encoding="utf-8")
            new = text.replace("psxrecomp-v4", "psxrecomp")
            if new != text:
                _write(cmake, new, options.dry_run)
                scrubbed.append("CMakeLists.txt")
        if scrubbed:
            return ApplyResult(
                "rename_psxrecomp_submodule",
                True,
                "Already using psxrecomp/; scrubbed leftover psxrecomp-v4 refs",
                scrubbed,
            )
        return ApplyResult("rename_psxrecomp_submodule", True, "Already using psxrecomp/", [])

    if not v4.exists():
        return ApplyResult(
            "rename_psxrecomp_submodule", False, "psxrecomp-v4 not found", []
        )

    # Case A: real psxrecomp/ already — keep it, remove legacy v4 entirely.
    if dest.exists() and _is_real_psxrecomp(dest):
        ok, out, ch = _remove_psxrecomp_v4(root, options)
        changed.extend(ch)
        parts.append(out)
        if not ok:
            return ApplyResult("rename_psxrecomp_submodule", False, out, changed)
        # Removing v4 often deletes .git/modules/psxrecomp-v4 while psxrecomp/.git
        # still points there — repair into a real submodule checkout.
        if not options.dry_run and not _psxrecomp_git_ok(dest):
            fix = op_repair_psxrecomp_submodule(root, options)
            changed.extend(fix.changed_paths)
            parts.append(fix.message)
            if not fix.ok:
                return ApplyResult(
                    "rename_psxrecomp_submodule", False, "; ".join(parts), changed
                )
    else:
        # Case B: stub or missing psxrecomp/ — promote v4 into place.
        if dest.exists() and not _is_real_psxrecomp(dest):
            stub_bak = root / "psxrecomp.stub.bak"
            if options.dry_run:
                parts.append(f"dry-run: move stub {dest} → {stub_bak}")
            else:
                if stub_bak.exists():
                    shutil.rmtree(stub_bak)
                dest.rename(stub_bak)
                changed.append(str(stub_bak.relative_to(root)))
                parts.append(f"Moved stub psxrecomp/ → {stub_bak.name}")

        if not dest.exists() or not _is_real_psxrecomp(dest):
            ok, out = _run(
                ["git", "mv", "psxrecomp-v4", "psxrecomp"], root, options.dry_run
            )
            if not ok:
                if options.dry_run:
                    out = "dry-run: shutil.move psxrecomp-v4 → psxrecomp"
                else:
                    shutil.move(str(v4), str(dest))
                    out = "Moved psxrecomp-v4 → psxrecomp (without git mv)"
            changed.append("psxrecomp")
            parts.append(out)

        # After promote, any leftover v4 path must still go (rare dual gitlink).
        if v4.exists():
            ok, out, ch = _remove_psxrecomp_v4(root, options)
            changed.extend(ch)
            parts.append(out)
            if not ok:
                return ApplyResult("rename_psxrecomp_submodule", False, out, changed)

    # Rewrite .gitmodules: rename v4 → psxrecomp if still named v4, then strip orphans.
    gm = root / ".gitmodules"
    if gm.is_file():
        text = gm.read_text(encoding="utf-8")
        new = text.replace('path = psxrecomp-v4', "path = psxrecomp")
        new = new.replace('[submodule "psxrecomp-v4"]', '[submodule "psxrecomp"]')
        # If both names somehow remain, drop the legacy block.
        if '[submodule "psxrecomp-v4"]' in new or "path = psxrecomp-v4" in new:
            new = _strip_gitmodules_submodule(new, "psxrecomp-v4")
        if new != text:
            _write(gm, new, options.dry_run)
            if ".gitmodules" not in changed:
                changed.append(".gitmodules")
            parts.append("updated .gitmodules")

    # Patch CMakeLists PSXRECOMP_ROOT path if present
    cmake = root / "CMakeLists.txt"
    if cmake.is_file():
        text = cmake.read_text(encoding="utf-8")
        new = text.replace("psxrecomp-v4", "psxrecomp")
        if new != text:
            _write(cmake, new, options.dry_run)
            changed.append("CMakeLists.txt")
            parts.append("rewrote CMakeLists psxrecomp-v4 → psxrecomp")

    msg = "; ".join(p for p in parts if p) or "Consolidated on psxrecomp/"
    return ApplyResult("rename_psxrecomp_submodule", True, msg, changed)


def _gitmodules_psxrecomp_url_branch(root: Path) -> tuple[str, str]:
    """Return (url, branch) for the game's psxrecomp submodule entry."""
    url = "https://github.com/mstan/psxrecomp.git"
    branch = "master"
    gm = root / ".gitmodules"
    if not gm.is_file():
        return url, branch
    text = gm.read_text(encoding="utf-8")
    for block in re.split(r"(?=\[submodule)", text):
        if not re.search(r"^\s*path\s*=\s*psxrecomp\s*$", block, re.M):
            continue
        m = re.search(r"^\s*url\s*=\s*(\S+)", block, re.M)
        if m:
            url = m.group(1)
        m = re.search(r"^\s*branch\s*=\s*(\S+)", block, re.M)
        if m:
            branch = m.group(1)
        break
    return url, branch


def _psxrecomp_git_ok(path: Path) -> bool:
    if not path.is_dir():
        return False
    ok, out = _run(
        ["git", "-C", str(path), "rev-parse", "--is-inside-work-tree"],
        path.parent,
        False,
    )
    return ok and out.strip() == "true"


def op_ensure_psxrecomp_submodule(root: Path, options: MigrateOptions) -> ApplyResult:
    dest = root / "psxrecomp"
    if _is_real_psxrecomp(dest) and _psxrecomp_git_ok(dest):
        return ApplyResult("ensure_psxrecomp_submodule", True, "Already present", [])
    if _is_real_psxrecomp(dest) and not _psxrecomp_git_ok(dest):
        # Tree on disk but broken gitdir — repair, don't no-op.
        return op_repair_psxrecomp_submodule(root, options)
    url = "https://github.com/mstan/psxrecomp.git"
    ok, out = _run(
        ["git", "submodule", "add", "-b", "master", url, "psxrecomp"],
        root,
        options.dry_run,
    )
    if not ok:
        return ApplyResult("ensure_psxrecomp_submodule", False, out, [])
    ok2, out2 = _run(
        ["git", "submodule", "update", "--init", "--recursive", "psxrecomp"],
        root,
        options.dry_run,
    )
    return ApplyResult(
        "ensure_psxrecomp_submodule",
        ok2,
        out2 or out or "Added psxrecomp submodule",
        ["psxrecomp", ".gitmodules"],
    )


def op_repair_psxrecomp_submodule(root: Path, options: MigrateOptions) -> ApplyResult:
    """Re-establish ``psxrecomp/`` as a real git submodule after a broken gitdir.

    Moves the on-disk tree aside (``psxrecomp.broken.bak``), clears absorbed
    index entries, then ``submodule update --init`` / ``submodule add``.
    """
    dest = root / "psxrecomp"
    changed: list[str] = []
    parts: list[str] = []

    if not dest.is_dir():
        return ApplyResult(
            "repair_psxrecomp_submodule",
            False,
            "psxrecomp/ missing — use ensure_psxrecomp_submodule instead",
            [],
        )

    if _is_real_psxrecomp(dest) and _psxrecomp_git_ok(dest):
        return ApplyResult(
            "repair_psxrecomp_submodule",
            True,
            "psxrecomp/ checkout already healthy",
            [],
        )

    url, branch = _gitmodules_psxrecomp_url_branch(root)
    if options.dry_run:
        return ApplyResult(
            "repair_psxrecomp_submodule",
            True,
            f"dry-run: move psxrecomp/ aside, re-init submodule from {url}@{branch}",
            ["psxrecomp", ".gitmodules"],
        )

    # Ensure .gitmodules lists the canonical submodule before re-init.
    gm = root / ".gitmodules"
    gm_text = gm.read_text(encoding="utf-8") if gm.is_file() else ""
    if 'path = psxrecomp' not in gm_text and 'path=psxrecomp' not in gm_text:
        block = (
            f'[submodule "psxrecomp"]\n'
            f"\tpath = psxrecomp\n"
            f"\turl = {url}\n"
            f"\tbranch = {branch}\n"
        )
        _write(gm, (gm_text.rstrip() + "\n\n" if gm_text.strip() else "") + block, False)
        changed.append(".gitmodules")
        parts.append("wrote .gitmodules psxrecomp entry")
    else:
        # Make sure url/branch are present on the existing block (best-effort).
        if url and url not in gm_text:
            parts.append(f"using url {url}")

    bak = root / "psxrecomp.broken.bak"
    if bak.exists():
        n = 1
        while (root / f"psxrecomp.broken.bak.{n}").exists():
            n += 1
        bak = root / f"psxrecomp.broken.bak.{n}"

    # Drop absorbed tree from the index (mode 100644 flood) so a gitlink can land.
    ok_rm, out_rm = _run(["git", "rm", "-rf", "--cached", "psxrecomp"], root, False)
    if ok_rm:
        parts.append(out_rm or "git rm --cached psxrecomp")
    elif out_rm:
        parts.append(f"git rm --cached: {out_rm}")

    try:
        dest.rename(bak)
        changed.append(bak.name)
        parts.append(f"moved broken tree → {bak.name}")
    except OSError as exc:
        return ApplyResult(
            "repair_psxrecomp_submodule",
            False,
            f"could not move broken psxrecomp/: {exc}",
            changed,
        )

    # Prefer update --init when .gitmodules already declares the path.
    _run(["git", "submodule", "sync", "--", "psxrecomp"], root, False)
    ok_up, out_up = _run(
        [
            "git",
            "submodule",
            "update",
            "--init",
            "--force",
            "--recursive",
            "psxrecomp",
        ],
        root,
        False,
    )
    if ok_up and _psxrecomp_git_ok(dest) and _is_real_psxrecomp(dest):
        changed.append("psxrecomp")
        parts.append(out_up or "submodule update --init psxrecomp")
        return ApplyResult(
            "repair_psxrecomp_submodule",
            True,
            "; ".join(parts),
            changed,
        )

    # Fresh add (works when no gitlink was ever recorded).
    if dest.exists():
        try:
            shutil.rmtree(dest)
        except OSError:
            pass
    ok_add, out_add = _run(
        ["git", "submodule", "add", "-b", branch, "--force", url, "psxrecomp"],
        root,
        False,
    )
    if not ok_add:
        # Restore bak so the user keeps a buildable tree.
        if bak.exists() and not dest.exists():
            try:
                bak.rename(dest)
                parts.append("restored broken tree after failed add")
            except OSError:
                pass
        return ApplyResult(
            "repair_psxrecomp_submodule",
            False,
            f"submodule add failed: {out_add}; update: {out_up}",
            changed,
        )

    _run(
        ["git", "submodule", "update", "--init", "--recursive", "psxrecomp"],
        root,
        False,
    )
    if not (_psxrecomp_git_ok(dest) and _is_real_psxrecomp(dest)):
        return ApplyResult(
            "repair_psxrecomp_submodule",
            False,
            "re-init finished but psxrecomp/ still not a healthy checkout",
            changed,
        )

    changed.append("psxrecomp")
    if ".gitmodules" not in changed:
        changed.append(".gitmodules")
    parts.append(out_add or f"submodule add {url}")
    parts.append(f"old tree kept at {bak.name} (delete when satisfied)")
    return ApplyResult(
        "repair_psxrecomp_submodule",
        True,
        "; ".join(parts),
        changed,
    )


def op_ensure_recomp_ui_submodule(root: Path, options: MigrateOptions) -> ApplyResult:
    if (root / "recomp-ui").is_dir() and (
        (root / "recomp-ui" / "CMakeLists.txt").is_file()
        or (root / "recomp-ui" / ".git").exists()
    ):
        return ApplyResult("ensure_recomp_ui_submodule", True, "Already present", [])
    url = "https://github.com/mstan/recomp-ui.git"
    ok, out = _run(
        ["git", "submodule", "add", "-b", "master", url, "recomp-ui"],
        root,
        options.dry_run,
    )
    if not ok:
        return ApplyResult("ensure_recomp_ui_submodule", False, out, [])
    _run(
        ["git", "submodule", "update", "--init", "--recursive", "recomp-ui"],
        root,
        options.dry_run,
    )
    return ApplyResult(
        "ensure_recomp_ui_submodule",
        True,
        out or "Added recomp-ui submodule",
        ["recomp-ui", ".gitmodules"],
    )


def op_emit_codegen_setup(root: Path, options: MigrateOptions) -> ApplyResult:
    tokens = _resolve_tokens(root, options)
    tdir = templates_dir()
    changed = []
    for name in ("codegen_setup.c", "codegen_setup.h"):
        src = tdir / f"{name}.in"
        dst = root / name
        if dst.is_file() and not options.force:
            # Refresh if missing forward helper
            if name.endswith(".c") and "psx_game_codegen_forward_if_built" in dst.read_text(
                encoding="utf-8", errors="replace"
            ):
                continue
        _fill(src, dst, tokens, options.dry_run)
        changed.append(name)
    if not changed:
        return ApplyResult("emit_codegen_setup", True, "Already up to date", [])
    return ApplyResult("emit_codegen_setup", True, "Wrote codegen_setup", changed)


def op_emit_version(root: Path, options: MigrateOptions) -> ApplyResult:
    """Write starter VERSION only when missing.

    Never bump VERSION here — a newer VERSION without a rebuild leaves
    PSX_GAME_VERSION sticky in CMakeCache / an old binary, which breaks
    netplay lobby list filters. Releases must pin VERSION then rebuild with
    -DPSX_GAME_VERSION=<pin> (CI / package_setup_host enforce the stamp).
    """
    dst = root / "VERSION"
    if dst.is_file() and not options.force:
        return ApplyResult("emit_version", True, "VERSION already exists", [])
    src = templates_dir() / "VERSION.in"
    text = "0.1.0\n" if not src.is_file() else src.read_text(encoding="utf-8")
    # VERSION.in may have tokens — keep simple
    text = text.replace("@VERSION@", "0.1.0")
    if not text.endswith("\n"):
        text += "\n"
    _write(dst, text if "@" not in text else "0.1.0\n", options.dry_run)
    return ApplyResult("emit_version", True, "Wrote VERSION", ["VERSION"])


def op_emit_symbols_toml(root: Path, options: MigrateOptions) -> ApplyResult:
    dst = root / "symbols.toml"
    if dst.is_file() and not options.force:
        return ApplyResult("emit_symbols_toml", True, "symbols.toml already exists", [])
    tokens = _resolve_tokens(root, options)
    _fill(templates_dir() / "symbols.toml.in", dst, tokens, options.dry_run)
    return ApplyResult("emit_symbols_toml", True, "Wrote symbols.toml", ["symbols.toml"])


def op_emit_sync_symbols(root: Path, options: MigrateOptions) -> ApplyResult:
    src = toolkit_dir() / "sync_symbols.py"
    dst = root / "tools" / "sync_symbols.py"
    if dst.is_file() and not options.force:
        return ApplyResult("emit_sync_symbols", True, "Already present", [])
    if options.dry_run:
        return ApplyResult("emit_sync_symbols", True, "dry-run: copy sync_symbols.py", ["tools/sync_symbols.py"])
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    return ApplyResult("emit_sync_symbols", True, "Installed tools/sync_symbols.py", ["tools/sync_symbols.py"])


def op_merge_gitignore(root: Path, options: MigrateOptions) -> ApplyResult:
    src = templates_dir() / "gitignore.in"
    template = src.read_text(encoding="utf-8")
    tokens = _resolve_tokens(root, options)
    for k, v in tokens.items():
        template = template.replace(f"@{k}@", v)

    dst = root / ".gitignore"
    existing = dst.read_text(encoding="utf-8") if dst.is_file() else ""
    required = [
        "/generated/",
        "/disc/",
        "/bios/",
        "/dist/",
        "/build/",
        "/build-*/",
        "/saves/",
        "*.bin",
        "*.cue",
    ]
    additions = []
    for pat in required:
        if pat not in existing:
            additions.append(pat)

    if not existing:
        _write(dst, template if not template.startswith("@") else "\n".join(required) + "\n", options.dry_run)
        return ApplyResult("merge_gitignore", True, "Wrote .gitignore from template", [".gitignore"])

    if not additions:
        return ApplyResult("merge_gitignore", True, "No gitignore changes needed", [])

    block = (
        "\n# --- PSXRecomp setup-host (project studio) ---\n"
        + "\n".join(additions)
        + "\n"
    )
    _write(dst, existing.rstrip() + "\n" + block, options.dry_run)
    return ApplyResult(
        "merge_gitignore",
        True,
        f"Appended {len(additions)} ignore rules",
        [".gitignore"],
    )


def op_emit_mods_preloaded(root: Path, options: MigrateOptions) -> ApplyResult:
    base = root / "mods" / "preloaded"
    readme = base / "README.md"
    packages = base / "packages" / ".gitkeep"
    if readme.is_file() and (base / "packages").is_dir() and not options.force:
        return ApplyResult("emit_mods_preloaded", True, "Already present", [])
    if options.dry_run:
        return ApplyResult(
            "emit_mods_preloaded",
            True,
            "dry-run: stub mods/preloaded",
            ["mods/preloaded/README.md"],
        )
    packages.parent.mkdir(parents=True, exist_ok=True)
    packages.write_text("", encoding="utf-8")
    readme.write_text(
        "# Preloaded mods\n\n"
        "Ship default-off `.psxmod` packages under `packages/<id>/<version>/`.\n"
        "Setup-host releases copy this tree beside the executable when present.\n",
        encoding="utf-8",
    )
    return ApplyResult(
        "emit_mods_preloaded",
        True,
        "Created mods/preloaded stub",
        ["mods/preloaded/README.md", "mods/preloaded/packages/.gitkeep"],
    )


def op_relocate_boxart(root: Path, options: MigrateOptions) -> ApplyResult:
    dest_dir = root / "launcher_assets" / "img"
    dest = dest_dir / "boxart.tga"
    if dest.is_file() and not options.force:
        return ApplyResult("relocate_boxart", True, "Modern boxart already present", [])

    sources = [
        root / "recomp" / "launcher" / "boxart.tga",
        root / "recomp" / "launcher" / "boxart.png",
    ]
    src = next((p for p in sources if p.is_file()), None)
    if src is None:
        return ApplyResult("relocate_boxart", False, "No legacy boxart found", [])

    if options.dry_run:
        return ApplyResult(
            "relocate_boxart",
            True,
            f"dry-run: copy {src.name} → launcher_assets/img/boxart.tga",
            ["launcher_assets/img/boxart.tga"],
        )

    dest_dir.mkdir(parents=True, exist_ok=True)
    if src.suffix.lower() == ".tga":
        shutil.copy2(src, dest)
    else:
        # Keep png beside expected path with note — TGA preferred by launcher
        shutil.copy2(src, dest_dir / src.name)
        (dest_dir / "BOXART_SOURCE.txt").write_text(
            f"Copied from {src.relative_to(root)}; convert to boxart.tga if needed.\n",
            encoding="utf-8",
        )
        return ApplyResult(
            "relocate_boxart",
            True,
            f"Copied {src.name} (not TGA — convert for LAUNCHER_BOXART)",
            [str((dest_dir / src.name).relative_to(root))],
        )

    (dest_dir / "BOXART_SOURCE.txt").write_text(
        f"Relocated from {src.relative_to(root)} by project studio.\n",
        encoding="utf-8",
    )
    return ApplyResult(
        "relocate_boxart",
        True,
        "Relocated boxart to launcher_assets/img/boxart.tga",
        ["launcher_assets/img/boxart.tga", "launcher_assets/img/BOXART_SOURCE.txt"],
    )


def op_emit_boxart_stub(root: Path, options: MigrateOptions) -> ApplyResult:
    dest_dir = root / "launcher_assets" / "img"
    if dest_dir.is_dir() and not options.force:
        return ApplyResult("emit_boxart_stub", True, "launcher_assets already exists", [])
    if options.dry_run:
        return ApplyResult("emit_boxart_stub", True, "dry-run: mkdir launcher_assets/img", [])
    dest_dir.mkdir(parents=True, exist_ok=True)
    (dest_dir / ".gitkeep").write_text("", encoding="utf-8")
    return ApplyResult(
        "emit_boxart_stub",
        True,
        "Created launcher_assets/img (add boxart.tga later)",
        ["launcher_assets/img/.gitkeep"],
    )


def _app_icon_source_dir(root: Path) -> Path | None:
    """Prefer game submodule assets, then toolkit-adjacent psxrecomp checkout."""
    for cand in (
        root / "psxrecomp" / "assets",
        root / "psxrecomp-v4" / "assets",
    ):
        if (cand / "psxrecomp.ico").is_file() or (cand / "psxrecomp.svg").is_file():
            return cand
    pr = psxrecomp_root_from_toolkit()
    if pr is not None:
        cand = pr / "assets"
        if (cand / "psxrecomp.ico").is_file() or (cand / "psxrecomp.svg").is_file():
            return cand
    return None


def op_ensure_app_icon(root: Path, options: MigrateOptions) -> ApplyResult:
    dest_dir = root / "assets"
    dest_ico = dest_dir / "psxrecomp.ico"
    if dest_ico.is_file() and not options.force:
        return ApplyResult("ensure_app_icon", True, "assets/psxrecomp.ico already present", [])
    src_dir = _app_icon_source_dir(root)
    if src_dir is None:
        return ApplyResult(
            "ensure_app_icon",
            False,
            "No psxrecomp/assets icon source (add psxrecomp submodule or update toolkit)",
            [],
        )
    names = [n for n in ("psxrecomp.svg", "psxrecomp.png", "psxrecomp.ico") if (src_dir / n).is_file()]
    if not names:
        return ApplyResult("ensure_app_icon", False, f"Empty icon source: {src_dir}", [])
    if options.dry_run:
        return ApplyResult(
            "ensure_app_icon",
            True,
            f"dry-run: copy {', '.join(names)} → assets/",
            [f"assets/{n}" for n in names],
        )
    dest_dir.mkdir(parents=True, exist_ok=True)
    changed: list[str] = []
    for name in names:
        shutil.copy2(src_dir / name, dest_dir / name)
        changed.append(f"assets/{name}")
    return ApplyResult(
        "ensure_app_icon",
        True,
        f"Installed app icon from {src_dir.name}/ ({', '.join(names)})",
        changed,
    )


def op_rewrite_cmake_setup_host(root: Path, options: MigrateOptions) -> ApplyResult:
    tokens = _resolve_tokens(root, options)
    # Force wizard for setup-host policy
    if not options.enable_wizard and options.enable_recomp_ui:
        # Still rewrite with wizard ON — exclusive setup-host
        options = MigrateOptions(**{**options.to_dict(), "enable_wizard": True})
        tokens = _resolve_tokens(root, options)

    cmake = root / "CMakeLists.txt"
    changed = ["CMakeLists.txt"]
    extras_note = root / "CMakeLists.migrate_extras.txt"

    old = cmake.read_text(encoding="utf-8") if cmake.is_file() else ""
    # Capture extras for the user
    extras_bits = []
    if "EXTRAS_SOURCES" in old:
        extras_bits.append("Old CMakeLists used EXTRAS_SOURCES — re-attach mod/plugin sources.")
    if "add_test(" in old or "BUILD_TESTING" in old:
        extras_bits.append("Old CMakeLists had tests — restore from CMakeLists.txt.pre_migrate.bak.")
    if "POST_BUILD" in old and "mods/preloaded" in old:
        extras_bits.append("Old CMakeLists copied mods/preloaded POST_BUILD — re-add if needed.")

    bak = root / "CMakeLists.txt.pre_migrate.bak"
    if cmake.is_file() and not options.dry_run:
        if not bak.is_file() or options.force:
            bak.write_text(old, encoding="utf-8")
            changed.append("CMakeLists.txt.pre_migrate.bak")

    _fill(templates_dir() / "CMakeLists.txt.in", cmake, tokens, options.dry_run)

    go = root / "game_options.toml"
    if not go.is_file():
        _fill(templates_dir() / "game_options.toml.in", go, tokens, options.dry_run)
        changed.append("game_options.toml")

    if extras_bits and not options.dry_run:
        extras_note.write_text(
            "Preserved notes after setup-host CMake rewrite:\n\n"
            + "\n".join(f"- {b}" for b in extras_bits)
            + "\n\nFull previous file: CMakeLists.txt.pre_migrate.bak\n",
            encoding="utf-8",
        )
        changed.append("CMakeLists.migrate_extras.txt")

    return ApplyResult(
        "rewrite_cmake_setup_host",
        True,
        "Wrote setup-host CMakeLists.txt (backup .pre_migrate.bak)",
        changed,
    )


def op_emit_packager(root: Path, options: MigrateOptions) -> ApplyResult:
    tokens = _resolve_tokens(root, options)
    dst = root / "scripts" / "package_setup_release.sh"
    if dst.is_file() and not options.force:
        return ApplyResult("emit_packager", True, "Packager already present", [])
    _fill(templates_dir() / "package_setup_release.sh.in", dst, tokens, options.dry_run)
    if not options.dry_run:
        dst.chmod(dst.stat().st_mode | 0o111)
    return ApplyResult(
        "emit_packager",
        True,
        "Wrote scripts/package_setup_release.sh",
        ["scripts/package_setup_release.sh"],
    )


def op_emit_ci_workflow(root: Path, options: MigrateOptions) -> ApplyResult:
    tokens = _resolve_tokens(root, options)
    src = ci_setup_release_template(root)
    if src is None:
        return ApplyResult(
            "emit_ci_workflow",
            False,
            "Cannot find docs/ci/templates/setup-release.yml (need psxrecomp submodule)",
            [],
        )
    dst = root / ".github" / "workflows" / "release.yml"
    if dst.is_file() and not options.force and "YOUR_ZIP_PREFIX" not in dst.read_text(
        encoding="utf-8", errors="replace"
    ):
        return ApplyResult("emit_ci_workflow", True, "release.yml already filled", [])
    _fill(src, dst, tokens, options.dry_run, ci=True)
    return ApplyResult(
        "emit_ci_workflow",
        True,
        "Wrote .github/workflows/release.yml (setup-host)",
        [".github/workflows/release.yml"],
    )


def op_annotate_legacy_packaging(root: Path, options: MigrateOptions) -> ApplyResult:
    note = root / "packaging" / "SETUP_HOST_MIGRATION.txt"
    text = (
        "This title is migrating to setup-host releases only.\n"
        "Do not ship prebuilt generated game C.\n"
        "Use scripts/package_setup_release.sh + .github/workflows/release.yml\n"
        "(see psxrecomp/docs/GAME_PROJECT_SETUP.md and docs/ci/HOST_ONLY_RELEASES.md).\n"
        "Legacy package_release scripts in this folder are obsolete for public releases.\n"
    )
    if (root / "packaging").is_dir():
        _write(note, text, options.dry_run)
        return ApplyResult(
            "annotate_legacy_packaging",
            True,
            "Wrote packaging/SETUP_HOST_MIGRATION.txt",
            ["packaging/SETUP_HOST_MIGRATION.txt"],
        )
    # tools-only legacy
    tools_note = root / "tools" / "SETUP_HOST_MIGRATION.txt"
    _write(tools_note, text, options.dry_run)
    return ApplyResult(
        "annotate_legacy_packaging",
        True,
        "Wrote tools/SETUP_HOST_MIGRATION.txt",
        ["tools/SETUP_HOST_MIGRATION.txt"],
    )


def op_probe_disc_refresh(root: Path, options: MigrateOptions) -> ApplyResult:
    if not options.disc:
        return ApplyResult(
            "probe_disc_refresh",
            False,
            "Pass --disc path/to/game.cue to refresh identity",
            [],
        )
    disc = Path(options.disc).expanduser().resolve()
    if not disc.is_file():
        return ApplyResult("probe_disc_refresh", False, f"Disc not found: {disc}", [])

    probe = toolkit_dir() / "probe_disc.py"
    if not probe.is_file():
        return ApplyResult("probe_disc_refresh", False, "probe_disc.py missing from toolkit", [])

    name = options.project_name or infer_project_name(root)
    players = options.players
    cmd = [
        sys.executable,
        str(probe),
        str(disc),
        "--write-game-toml",
        str(root / "game.toml"),
        "--write-catalog",
        str(root / "catalog_identity.json"),
        "--write-seeds",
        str(root / "seeds" / "ghidra_funcs.txt"),
        "--out-dir",
        "disc",
        "--disc-rel",
        "disc/" + disc.name,
        "--players",
        str(players),
        "--display-name",
        name,
    ]
    if options.dry_run:
        return ApplyResult(
            "probe_disc_refresh",
            True,
            "dry-run: " + " ".join(cmd),
            ["game.toml", "catalog_identity.json", "seeds/ghidra_funcs.txt"],
        )

    (root / "seeds").mkdir(parents=True, exist_ok=True)
    ok, out = _run(cmd, root, dry_run=False)
    if not ok:
        return ApplyResult("probe_disc_refresh", False, out, [])
    return ApplyResult(
        "probe_disc_refresh",
        True,
        out or "probe_disc completed",
        ["game.toml", "catalog_identity.json", "seeds/ghidra_funcs.txt"],
    )


def op_record_framework_pins(root: Path, options: MigrateOptions) -> ApplyResult:
    record = None
    for base in (root / "psxrecomp", root / "psxrecomp-v4"):
        cand = base / "tools" / "ci" / "record_pins.sh"
        if cand.is_file():
            record = cand
            break
    dst = root / "framework_pins.txt"
    if record is None:
        # Best-effort: git rev-parse in submodules
        lines = []
        for name in ("psxrecomp", "recomp-ui"):
            sub = root / name
            if not sub.is_dir():
                continue
            ok, out = _run(["git", "rev-parse", "HEAD"], sub, options.dry_run)
            if ok and out:
                short = out[:7] if not options.dry_run else "dryrun"
                lines.append(f"{name}={short} ({out})")
        if not lines:
            return ApplyResult(
                "record_framework_pins",
                False,
                "No psxrecomp submodule to record pins from",
                [],
            )
        text = "\n".join(lines) + "\n"
        _write(dst, text, options.dry_run)
        return ApplyResult("record_framework_pins", True, "Wrote framework_pins.txt", ["framework_pins.txt"])

    if options.dry_run:
        return ApplyResult(
            "record_framework_pins",
            True,
            f"dry-run: {record} --root {root}",
            ["framework_pins.txt"],
        )
    proc = subprocess.run(
        ["bash", str(record), "--root", str(root)],
        capture_output=True,
        text=True,
        check=False,
        cwd=str(root),
    )
    body = (proc.stdout or "").strip()
    if proc.returncode != 0 and not body:
        return ApplyResult(
            "record_framework_pins",
            False,
            (proc.stderr or f"exit {proc.returncode}").strip(),
            [],
        )
    if body:
        dst.write_text(body + "\n", encoding="utf-8")
    return ApplyResult(
        "record_framework_pins",
        True,
        "Wrote framework_pins.txt",
        ["framework_pins.txt"],
    )


def _boxart_hints(root: Path, options: MigrateOptions) -> tuple[str, str]:
    cue = ""
    display = ""
    if options.disc:
        cue = Path(options.disc).name
    cat = root / "catalog_identity.json"
    if cat.is_file():
        try:
            data = json.loads(cat.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            data = {}
        if isinstance(data, dict):
            rom = data.get("rom_identity") if isinstance(data.get("rom_identity"), dict) else {}
            cue = cue or str(rom.get("cue_name") or "")
            game = data.get("game") if isinstance(data.get("game"), dict) else {}
            display = str(game.get("name") or "")
    name = options.project_name or infer_project_name(root)
    display = options.window_title or display or game_name_from_project(name)
    return cue, display


def _ensure_boxart_png(root: Path, options: MigrateOptions) -> list[str]:
    """Fetch libretro boxart PNG/TGA when the README PNG is missing. Never raises."""
    png = root / "launcher_assets" / "img" / "boxart.png"
    tga = root / "launcher_assets" / "img" / "boxart.tga"
    if png.is_file():
        return []
    if options.dry_run:
        return ["launcher_assets/img/boxart.png"]
    cue, display = _boxart_hints(root, options)
    if not cue and not display:
        return []
    try:
        sys.path.insert(0, str(toolkit_dir()))
        from fetch_boxart import fetch_to_paths  # type: ignore
    except ImportError:
        return []
    try:
        fetch_to_paths(tga, cue_stem=cue, display_name=display)
    except Exception:
        return []
    changed: list[str] = []
    if png.is_file():
        changed.append("launcher_assets/img/boxart.png")
    if tga.is_file():
        changed.append("launcher_assets/img/boxart.tga")
    src = tga.parent / "BOXART_SOURCE.txt"
    if src.is_file():
        changed.append("launcher_assets/img/BOXART_SOURCE.txt")
    return changed


def op_patch_readme_metrics(root: Path, options: MigrateOptions) -> ApplyResult:
    """Upsert download badges, libretro boxart, RetComM Launcher, and R.A.I.D. footer."""
    from .readme_metrics import (
        apply_github_about,
        boxart_png_present,
        render_boxart_block,
        render_launcher_block,
        render_metrics_block,
        render_raid_block,
        resolve_github_slug,
        upsert_readme_blocks,
    )

    name = options.project_name or infer_project_name(root)
    owner, repo = resolve_github_slug(root, options.github_owner, options.github_repo)
    zp = options.zip_prefix or derive_zip_prefix(name)
    fetched = _ensure_boxart_png(root, options)
    display = options.window_title or game_name_from_project(name)
    boxart_block = None
    if boxart_png_present(root) or (
        options.dry_run and "launcher_assets/img/boxart.png" in fetched
    ):
        boxart_block = render_boxart_block(display)

    path = root / "README.md"
    if path.is_file():
        old = path.read_text(encoding="utf-8", errors="replace")
    else:
        title = options.window_title or window_title_from_name(name)
        old = f"# {title}\n"
    new = upsert_readme_blocks(
        old,
        render_metrics_block(owner, repo, zp),
        render_launcher_block(),
        render_raid_block(),
        boxart=boxart_block,
    )
    changed: list[str] = list(fetched)
    if new != old:
        _write(path, new, options.dry_run)
        if "README.md" not in changed:
            changed.append("README.md")

    src_img = templates_dir() / "raid-discord.png"
    dst_img = root / ".github" / "raid-discord.png"
    if src_img.is_file() and not dst_img.is_file():
        if not options.dry_run:
            dst_img.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src_img, dst_img)
        changed.append(".github/raid-discord.png")

    about_ok, about_msg = apply_github_about(owner, repo, dry_run=options.dry_run)
    parts: list[str] = []
    if changed:
        parts.append(
            f"Patched README badges, boxart, RetComM Launcher, and R.A.I.D. footer ({owner}/{repo})"
        )
    if fetched:
        parts.append("fetched libretro boxart")
    if about_ok:
        parts.append(about_msg)
    else:
        parts.append("GitHub About skipped: " + about_msg)
    if not changed and not about_ok:
        return ApplyResult(
            "patch_readme_metrics",
            True,
            f"README metrics already current ({owner}/{repo}); {about_msg}",
            [],
        )
    return ApplyResult(
        "patch_readme_metrics",
        True,
        "; ".join(parts),
        changed,
    )


_OPS = {
    "rename_psxrecomp_submodule": op_rename_psxrecomp_submodule,
    "ensure_psxrecomp_submodule": op_ensure_psxrecomp_submodule,
    "repair_psxrecomp_submodule": op_repair_psxrecomp_submodule,
    "ensure_recomp_ui_submodule": op_ensure_recomp_ui_submodule,
    "emit_codegen_setup": op_emit_codegen_setup,
    "emit_version": op_emit_version,
    "emit_symbols_toml": op_emit_symbols_toml,
    "emit_sync_symbols": op_emit_sync_symbols,
    "merge_gitignore": op_merge_gitignore,
    "emit_mods_preloaded": op_emit_mods_preloaded,
    "relocate_boxart": op_relocate_boxart,
    "emit_boxart_stub": op_emit_boxart_stub,
    "ensure_app_icon": op_ensure_app_icon,
    "rewrite_cmake_setup_host": op_rewrite_cmake_setup_host,
    "emit_packager": op_emit_packager,
    "emit_ci_workflow": op_emit_ci_workflow,
    "annotate_legacy_packaging": op_annotate_legacy_packaging,
    "probe_disc_refresh": op_probe_disc_refresh,
    "record_framework_pins": op_record_framework_pins,
    "patch_readme_metrics": op_patch_readme_metrics,
}


def apply_plan(plan: Plan, *, selected: list[str] | None = None) -> list[ApplyResult]:
    root = Path(plan.root)
    options = plan.options
    results: list[ApplyResult] = []

    # Setup-host exclusive: never leave wizard off when CI/packager are applied
    if options.enable_ci or any(
        s.op_id in ("emit_ci_workflow", "emit_packager", "rewrite_cmake_setup_host")
        and s.selected
        for s in plan.steps
    ):
        options.enable_wizard = True
        options.enable_recomp_ui = True

    for step in plan.steps:
        if not step.selected:
            continue
        if selected is not None and step.op_id not in selected:
            continue
        fn = _OPS.get(step.op_id)
        if fn is None:
            results.append(
                ApplyResult(step.op_id, False, f"Unknown op: {step.op_id}", [])
            )
            continue
        try:
            results.append(fn(root, options))
        except Exception as exc:  # noqa: BLE001 — surface to CLI/GUI
            results.append(ApplyResult(step.op_id, False, f"{type(exc).__name__}: {exc}", []))
    return results


def list_ops() -> list[str]:
    return list(_OPS.keys())
