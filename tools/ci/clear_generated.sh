#!/usr/bin/env bash
# Clear generated game/BIOS C so CI can build a BIOS-free setup host.
#
# Usage (from game repo root):
#   psxrecomp/tools/ci/clear_generated.sh [--root DIR]
set -euo pipefail

ROOT="${PWD}"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --root) ROOT="${2:?}"; shift 2 ;;
    -h|--help)
      sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "error: unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

ROOT="$(cd "${ROOT}" && pwd)"
rm -rf "${ROOT}/generated" "${ROOT}/psxrecomp/generated"
echo "cleared generated/ under ${ROOT} (setup-host mode)"
