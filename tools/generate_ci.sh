#!/usr/bin/env sh
# Write the setup-host release workflow into an existing PSX project (Linux /
# macOS / WSL / Git Bash). The logic is tools/generate_ci.py; this finds Python.
#
#   sh psxrecomp/tools/generate_ci.sh                 # cwd is the project
#   sh psxrecomp/tools/generate_ci.sh ~/src/MyGame    # or name the root
#   sh psxrecomp/tools/generate_ci.sh --check         # stale? (exit 1)
#   sh psxrecomp/tools/generate_ci.sh --force         # overwrite
#
# Every flag passes through: --zip-prefix, --title, --dry-run. `--help` lists them.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PYTHON=${PYTHON:-$(command -v python3 || command -v python || true)}
[ -n "$PYTHON" ] || { echo "generate_ci: python3 not found on PATH" >&2; exit 1; }
exec "$PYTHON" "$SCRIPT_DIR/generate_ci.py" "$@"
