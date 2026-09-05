#!/usr/bin/env python3
"""generate_ci.py -- write the setup-host release workflow into an EXISTING
PSX project. Nothing else: no commit, no push, no migration plan.

    python3 psxrecomp/tools/generate_ci.py                 # cwd is the project
    python3 psxrecomp/tools/generate_ci.py ~/src/MyGameRecomp
    python3 psxrecomp/tools/generate_ci.py --check         # stale? (exit 1)
    python3 psxrecomp/tools/generate_ci.py --force         # overwrite

This is the thin, single-purpose face of Project Studio's emit_ci_workflow
operation (tools/new_project_layout/project_studio/ops.py), which is also
what `migrate_project.py apply` and `git install-ci` run. The template is
docs/ci/templates/setup-release.yml from the project's own psxrecomp
submodule when it has one, so the workflow matches the framework the project
pins; this checkout is the fallback.

Where the values come from:
  ZIP_PREFIX   scripts/package_setup_release.sh --zip-prefix <x> (the
               packager's prefix, so the zips CI uploads are the zips it
               built), else derived from the project name, else --zip-prefix
  GAME_TITLE   CMakeLists.txt / game.toml window_title, else --title

An existing, filled release.yml is not overwritten silently: its step names
are compared with the template's, and a step the template defines that the
file lacks means the file predates a framework change (reported as stale).
--force overwrites; a customised workflow is replaced wholesale, so diff it.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "new_project_layout"))

WORKFLOW_REL = pathlib.Path(".github") / "workflows" / "release.yml"


def zip_prefix_from_packager(root: pathlib.Path) -> str:
    try:
        text = (root / "scripts" / "package_setup_release.sh").read_text(
            encoding="utf-8", errors="replace")
    except OSError:
        return ""
    m = re.search(r"--zip-prefix\s+([^\s\\'\"]+)", text)
    return m.group(1) if m else ""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog="\n".join(__doc__.split("\n\n")[1:]))
    ap.add_argument("root", nargs="?", default=".", help="project root (default: cwd)")
    ap.add_argument("--zip-prefix", default="", help="override the release zip prefix")
    ap.add_argument("--title", default="", help="override the game title in release names")
    ap.add_argument("--force", action="store_true", help="overwrite an existing release.yml")
    ap.add_argument("--check", action="store_true",
                    help="report whether release.yml is present and current; exit 1 if not")
    ap.add_argument("--dry-run", action="store_true", help="resolve and report, write nothing")
    args = ap.parse_args()

    root = pathlib.Path(args.root).resolve()
    if not (root / "CMakeLists.txt").is_file():
        print(f"generate_ci: {root} has no CMakeLists.txt -- not a project root", file=sys.stderr)
        return 2

    try:
        from project_studio.models import MigrateOptions
        from project_studio.ops import op_emit_ci_workflow, _ci_steps_missing_from
        from project_studio.paths import ci_setup_release_template
    except ImportError as exc:
        print(f"generate_ci: cannot import Project Studio from {HERE / 'new_project_layout'}: {exc}",
              file=sys.stderr)
        return 2

    template = ci_setup_release_template(root)
    if template is None:
        print("generate_ci: no docs/ci/templates/setup-release.yml found (neither the "
              "project's psxrecomp submodule nor this checkout)", file=sys.stderr)
        return 2

    dst = root / WORKFLOW_REL
    if args.check:
        if not dst.is_file():
            print(f"{WORKFLOW_REL}: missing")
            return 1
        text = dst.read_text(encoding="utf-8", errors="replace")
        if "YOUR_ZIP_PREFIX" in text or "YOUR_GAME_TITLE" in text:
            print(f"{WORKFLOW_REL}: still has YOUR_* placeholders")
            return 1
        stale = _ci_steps_missing_from(dst, template)
        if stale:
            print(f"{WORKFLOW_REL}: stale -- missing step(s): {', '.join(stale)}")
            return 1
        print(f"{WORKFLOW_REL}: present and current")
        return 0

    options = MigrateOptions(
        zip_prefix=args.zip_prefix or zip_prefix_from_packager(root) or None,
        window_title=args.title or None,
        force=args.force,
        dry_run=args.dry_run,
    )
    result = op_emit_ci_workflow(root, options)
    print(f"template:   {template}")
    print(f"zip prefix: {options.zip_prefix or '(derived from the project name)'}")
    print(result.message)
    return 0 if result.ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
