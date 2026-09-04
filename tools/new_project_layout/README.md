# New Project Layout + Project Studio

Scaffold **new** titles and **migrate** older ones onto the setup-host layout.

**Policy:** public releases are **setup-host only** (no prebuilt generated game C).

## New project

```bash
sh tools/new_project_layout/setup_project.sh --disc /path/to/game.cue --dir ~/src
```

See [`docs/GAME_PROJECT_SETUP.md`](../../docs/GAME_PROJECT_SETUP.md).

## Project Studio (migrate / update)

Shared Python library under `project_studio/` with CLI + CustomTkinter GUI.
CLI stays stdlib-only. First GUI launch auto-creates
`tools/new_project_layout/.venv` and installs `requirements-gui.txt`
(network once); later launches are silent.

```bash
python3 tools/new_project_layout/project_studio_gui.py
```

```bash
# Audit layout gaps
python3 tools/new_project_layout/migrate_project.py audit \
  --root /path/to/ApeEscapeRecomp

# Show ordered plan
python3 tools/new_project_layout/migrate_project.py plan \
  --root /path/to/ApeEscapeRecomp

# Dry-run apply (recommended first)
python3 tools/new_project_layout/migrate_project.py apply \
  --root /path/to/ApeEscapeRecomp --dry-run

# Apply for real (rewrites CMake with .pre_migrate.bak)
python3 tools/new_project_layout/migrate_project.py apply \
  --root /path/to/ApeEscapeRecomp \
  --disc /path/to/game.cue

# GUI (auto-bootstraps .venv + customtkinter on first run)
python3 tools/new_project_layout/migrate_project.py gui
# or
python3 tools/new_project_layout/project_studio_gui.py

# Git / GitHub (also available as the GUI "Git / GitHub" tab)
python3 tools/new_project_layout/migrate_project.py git status \
  --root /path/to/ApeEscapeRecomp
python3 tools/new_project_layout/migrate_project.py git ensure-submodules \
  --root /path/to/ApeEscapeRecomp \
  --psxrecomp-branch master --recomp-ui-branch master
python3 tools/new_project_layout/migrate_project.py git set-branch \
  --root /path/to/ApeEscapeRecomp --submodule psxrecomp --branch master
python3 tools/new_project_layout/migrate_project.py git ensure-nested \
  --root /path/to/ApeEscapeRecomp \
  --recomp-net-branch main --rbengine-branch main
python3 tools/new_project_layout/migrate_project.py git update-nested \
  --root /path/to/ApeEscapeRecomp --remote
python3 tools/new_project_layout/migrate_project.py git commit-nested \
  --root /path/to/ApeEscapeRecomp -m "chore: bump nested modules"
python3 tools/new_project_layout/migrate_project.py git update-submodules \
  --root /path/to/ApeEscapeRecomp --remote
python3 tools/new_project_layout/migrate_project.py git commit \
  --root /path/to/ApeEscapeRecomp -m "chore: bump submodules"
python3 tools/new_project_layout/migrate_project.py git push \
  --root /path/to/ApeEscapeRecomp
python3 tools/new_project_layout/migrate_project.py git release \
  --root /path/to/ApeEscapeRecomp --bump patch
```

Git ops shell out to `git` / `gh` (no force-push, amend, or hook skips). Submodule **branch** in `.gitmodules` is for `update --remote`; CI builds the committed **gitlink SHAs**.

For **bulk ops across many titles / platforms**, use the sibling tool
`retcomm-studio` (`~/…/GitHub/retcomm-studio` — catalog-backed CLI + plugin API).

### Ops (subset)

| Op | Purpose |
|----|---------|
| `rename_psxrecomp_submodule` | Promote/keep `psxrecomp/`; delete leftover `psxrecomp-v4` |
| `repair_psxrecomp_submodule` | Re-clone when `psxrecomp/.git` gitdir is broken / absorbed |
| `ensure_recomp_ui_submodule` | Add `recomp-ui` |
| `emit_codegen_setup` | Thin `codegen_setup.c/.h` |
| `rewrite_cmake_setup_host` | `psxrecomp_add_game_runtime` + wizard |
| `emit_packager` | `scripts/package_setup_release.sh` |
| `emit_ci_workflow` | Setup-host `release.yml` |
| `probe_disc_refresh` | TOC / catalog / seeds (needs `--disc`) |
| `annotate_legacy_packaging` | Mark old prebuilt packagers obsolete |

Wizard + `recomp-ui` are forced on for `apply` (setup-host requirement).

Helpers reused: `probe_disc.py`, `fill_tokens.py`, `sync_symbols.py`, `templates/*`.
