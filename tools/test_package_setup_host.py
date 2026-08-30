import os
import subprocess
from pathlib import Path

SCRIPT = Path(__file__).with_name("package_setup_host.sh").resolve()


def _bash_path(path: Path) -> str:
    resolved = path.resolve()
    if os.name != "nt":
        return str(resolved)
    drive = resolved.drive.rstrip(":").lower()
    tail = resolved.as_posix().split(":", 1)[1]
    return f"/mnt/{drive}{tail}"


def _git(root: Path, *args: str) -> None:
    subprocess.run(
        ["git", "-C", str(root), *args],
        check=True,
        capture_output=True,
        text=True,
    )


def _init_repo(root: Path) -> None:
    _git(root, "init", "-q")
    _git(root, "config", "user.email", "setup-packager@example.invalid")
    _git(root, "config", "user.name", "Setup Packager Test")
    _git(root, "config", "core.autocrlf", "false")
    _git(root, "config", "core.filemode", "false")


def _run(root: Path, *extra: str) -> subprocess.CompletedProcess[str]:
    (root / "build").mkdir(exist_ok=True)
    return subprocess.run(
        [
            "bash",
            _bash_path(SCRIPT),
            "--root",
            _bash_path(root),
            "--build-dir",
            "build",
            "--artifact",
            "linux-x64",
            "--zip-prefix",
            "test",
            "--exe-name",
            "Host",
            *extra,
        ],
        check=False,
        capture_output=True,
        text=True,
        env={**os.environ, "RELEASE_VERSION": ""},
    )


def _write_host_fixture(root: Path, version: str = "1.2.3") -> None:
    build = root / "build"
    (build / "assets/fonts").mkdir(parents=True)
    (build / "assets/img").mkdir(parents=True)
    (build / "Host").write_bytes(b"host")
    (build / "psx_game_version.txt").write_text(version + "\n", encoding="utf-8")


def _write_windows_host_fixture(root: Path, payload: bytes = b"unstripped-host") -> Path:
    _write_host_fixture(root)
    (root / "VERSION").write_text("1.2.3\n", encoding="utf-8")
    host = root / "build/Host.exe"
    host.write_bytes(payload)
    (root / "build/Host").unlink()
    return host


def _write_strip_fixture(root: Path, body: str) -> Path:
    tool = root / "fixture-strip"
    tool.write_text("#!/bin/sh\n" + body, encoding="utf-8", newline="\n")
    tool.chmod(0o755)
    return tool


def test_rejects_path_bearing_output_slugs_before_deletion(tmp_path: Path) -> None:
    sentinel = tmp_path / "sentinel"
    sentinel.mkdir()
    for value in ("../../../sentinel", "trailing.", "CON"):
        result = _run(tmp_path, "--artifact", value)
        assert result.returncode == 2
        assert "path-free slug" in result.stderr or "device name" in result.stderr
    assert sentinel.is_dir()


def test_rejects_unsafe_project_relative_paths(tmp_path: Path) -> None:
    for option, value in (
        ("--project-file", "../secret"),
        ("--project-dir", "/absolute"),
        ("--project-file", r"C:\secret"),
        ("--project-dir", "nested//dir"),
        ("--project-file", "nested/file:stream"),
        ("--project-dir", "nested/trailing."),
        ("--project-file", "nested/NUL.txt"),
    ):
        result = _run(tmp_path, option, value)
        assert result.returncode == 2, (option, value, result.stderr)
        assert (
            "project-relative path" in result.stderr
            or "unsafe path component" in result.stderr
            or "device path component" in result.stderr
        )


def test_git_metadata_probe_failure_is_fatal(tmp_path: Path) -> None:
    (tmp_path / ".git").write_text("gitdir: missing-worktree-metadata\n", encoding="utf-8")
    result = _run(tmp_path, "--project-file", "VERSION")
    assert result.returncode == 1
    assert "cannot inspect Git metadata for project root" in result.stderr
    assert not (tmp_path / "dist").exists()


def test_dirty_or_untracked_title_inputs_are_rejected(tmp_path: Path) -> None:
    _init_repo(tmp_path)
    source = tmp_path / "src"
    source.mkdir()
    (source / "tracked.c").write_text("tracked\n", encoding="utf-8")
    _git(tmp_path, "add", "src/tracked.c")
    _git(tmp_path, "commit", "-qm", "fixture")

    (source / "tracked.c").write_text("dirty\n", encoding="utf-8")
    result = _run(tmp_path, "--project-dir", "src")
    assert result.returncode == 1
    assert "dirty or untracked title rebuild inputs" in result.stderr

    (source / "tracked.c").write_text("tracked\n", encoding="utf-8")
    (source / "untracked.c").write_text("untracked\n", encoding="utf-8")
    result = _run(tmp_path, "--project-dir", "src")
    assert result.returncode == 1
    assert "dirty or untracked title rebuild inputs" in result.stderr


def test_only_tracked_title_directory_files_are_staged(tmp_path: Path) -> None:
    _init_repo(tmp_path)
    source = tmp_path / "src"
    source.mkdir()
    (source / "tracked.c").write_text("tracked\n", encoding="utf-8")
    (source / "ignored.secret").write_text("secret\n", encoding="utf-8")
    (tmp_path / ".gitignore").write_text("*.secret\n", encoding="utf-8")
    _git(tmp_path, "add", ".gitignore", "src/tracked.c")
    _git(tmp_path, "commit", "-qm", "fixture")
    _write_host_fixture(tmp_path)

    result = _run(tmp_path, "--project-dir", "src")
    assert result.returncode == 1
    assert "psxrecomp missing" in result.stderr
    stage = tmp_path / "dist/stage-setup-linux-x64/src"
    assert (stage / "tracked.c").read_text(encoding="utf-8") == "tracked\n"
    assert not (stage / "ignored.secret").exists()


def test_packaging_does_not_rewrite_source_version(tmp_path: Path) -> None:
    _init_repo(tmp_path)
    version = tmp_path / "VERSION"
    version.write_text("1.0.0\n", encoding="utf-8")
    _git(tmp_path, "add", "VERSION")
    _git(tmp_path, "commit", "-qm", "fixture")
    version.write_text("v1.2.3\n", encoding="utf-8")
    _write_host_fixture(tmp_path)

    result = _run(tmp_path, "--project-file", "VERSION")
    assert result.returncode == 1
    assert "psxrecomp missing" in result.stderr
    assert version.read_text(encoding="utf-8") == "v1.2.3\n"
    status = subprocess.run(
        ["git", "-C", str(tmp_path), "status", "--short", "--", "VERSION"],
        check=True,
        capture_output=True,
        text=True,
    )
    assert status.stdout.strip() == "M VERSION"


def test_windows_host_is_stripped_only_after_copy(
    tmp_path: Path,
) -> None:
    source = _write_windows_host_fixture(tmp_path)
    source_payload = source.read_bytes()
    strip_tool = _write_strip_fixture(
        tmp_path,
        '[ "$1" = "--strip-all" ] || exit 9\nprintf "stripped-host" >"$2"\n',
    )

    result = _run(
        tmp_path,
        "--strip-tool",
        _bash_path(strip_tool),
        "--project-file",
        "VERSION",
    )

    assert result.returncode == 1
    assert "stripped staged release host" in result.stdout
    assert "psxrecomp missing" in result.stderr
    assert source.read_bytes() == source_payload
    staged = tmp_path / "dist/stage-setup-linux-x64/Host.exe"
    assert staged.read_bytes() == b"stripped-host"


def test_windows_host_packaging_fails_when_stripping_fails(
    tmp_path: Path,
) -> None:
    source = _write_windows_host_fixture(tmp_path)
    source_payload = source.read_bytes()
    strip_tool = _write_strip_fixture(tmp_path, "exit 7\n")

    result = _run(tmp_path, "--strip-tool", _bash_path(strip_tool))

    assert result.returncode == 1
    assert "failed to strip staged release host" in result.stderr
    assert "psxrecomp missing" not in result.stderr
    assert source.read_bytes() == source_payload


def test_explicit_missing_strip_tool_is_fatal(tmp_path: Path) -> None:
    _write_windows_host_fixture(tmp_path)

    result = _run(tmp_path, "--strip-tool", "/definitely/missing/strip")

    assert result.returncode == 1
    assert "configured strip tool is not executable" in result.stderr
    assert "psxrecomp missing" not in result.stderr
