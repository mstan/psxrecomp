#!/usr/bin/env bash
# Print pinned submodule revisions for CI logs.
#
# Usage (from game repo root):
#   psxrecomp/tools/ci/record_pins.sh [--root DIR]
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

pin() {
  local name="$1" path="$2"
  if [[ -d "${ROOT}/${path}/.git" ]] || [[ -f "${ROOT}/${path}/.git" ]]; then
    echo "${name}=$(git -C "${ROOT}/${path}" rev-parse --short HEAD) ($(git -C "${ROOT}/${path}" rev-parse HEAD))"
  elif [[ -d "${ROOT}/${path}" ]]; then
    echo "${name}=<present, not a git checkout>"
  else
    echo "${name}=<missing>"
  fi
}

pin psxrecomp psxrecomp
pin recomp-ui recomp-ui
pin recomp-net psxrecomp/lib/recomp-net
pin retcomm-rbengine psxrecomp/lib/retcomm-rbengine
