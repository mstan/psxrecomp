#!/usr/bin/env bash
# Optional local check: fail if checkout SHAs disagree with framework_pins.txt.
#
# Release CI does NOT run this — submodule gitlinks are authoritative; use
# record_pins.sh to log SHAs. Keep this script for manual audits if you still
# commit a framework_pins.txt snapshot.
#
# Pins file lines (either form is accepted):
#   psxrecomp=<short> (<full40>)
#   psxrecomp=<full40>
#   recomp-ui=<not added>
#
# Usage (from game repo root):
#   psxrecomp/tools/ci/verify_pins.sh [--root DIR] [--pins PATH]
set -euo pipefail

ROOT="${PWD}"
PINS=""
while [ $# -gt 0 ]; do
  case "$1" in
    --root) ROOT="${2:?}"; shift 2 ;;
    --pins) PINS="${2:?}"; shift 2 ;;
    -h|--help)
      sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "error: unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

ROOT="$(cd "${ROOT}" && pwd)"
PINS="${PINS:-${ROOT}/framework_pins.txt}"

if [ ! -f "${PINS}" ]; then
  echo "error: missing pins file: ${PINS}" >&2
  exit 1
fi

fail=0

path_for() {
  case "$1" in
    psxrecomp) echo psxrecomp ;;
    recomp-ui) echo recomp-ui ;;
    recomp-net) echo psxrecomp/lib/recomp-net ;;
    retcomm-rbengine) echo psxrecomp/lib/retcomm-rbengine ;;
    *) echo "" ;;
  esac
}

expected_sha() {
  raw=$1
  case "$raw" in
    "<not added>"|"<missing>"*|"<present"*)
      echo ""
      return 0
      ;;
  esac
  # Prefer parenthesized full SHA from record_pins.sh: short (full40)
  inner=$(printf '%s' "$raw" | sed -n 's/.*(\([0-9a-fA-F]\{7,40\}\)).*/\1/p')
  if [ -n "$inner" ]; then
    echo "$inner"
    return 0
  fi
  echo "$raw"
}

check_pin() {
  name=$1
  path=$2
  raw=$3
  want=$(expected_sha "$raw")

  case "$raw" in
    "<not added>")
      if [ -d "${ROOT}/${path}/.git" ] || [ -f "${ROOT}/${path}/.git" ]; then
        echo "error: pins say ${name}=<not added> but ${path} is a git checkout" >&2
        fail=1
      else
        echo "ok: ${name} not added (matches pins)"
      fi
      return 0
      ;;
  esac

  if [ -z "$want" ]; then
    echo "warning: skipping unparsable pin ${name}=${raw}" >&2
    return 0
  fi

  if [ ! -d "${ROOT}/${path}/.git" ] && [ ! -f "${ROOT}/${path}/.git" ]; then
    echo "error: pin ${name}=${want} but ${path} is not a git checkout" >&2
    fail=1
    return 0
  fi

  have=$(git -C "${ROOT}/${path}" rev-parse HEAD)
  # Full SHA must match exactly; short pin must be a prefix of HEAD.
  case "$want" in
    "$have")
      echo "ok: ${name}=${have}"
      ;;
    *)
      case "$have" in
        "$want"*)
          echo "ok: ${name}=${have} (short pin ${want})"
          ;;
        *)
          echo "error: ${name} pin mismatch: pins=${want} checkout=${have}" >&2
          fail=1
          ;;
      esac
      ;;
  esac
}

while IFS= read -r line || [ -n "$line" ]; do
  case "$line" in
    ""|\#*) continue ;;
  esac
  case "$line" in
    *=*) ;;
    *) continue ;;
  esac
  name=${line%%=*}
  raw=${line#*=}
  path=$(path_for "$name")
  if [ -z "$path" ]; then
    echo "warning: unknown pin key '${name}' — ignored" >&2
    continue
  fi
  check_pin "$name" "$path" "$raw"
done <"${PINS}"

if [ "${fail}" -ne 0 ]; then
  echo "error: framework_pins.txt does not match checked-out submodules." >&2
  echo "       Bump submodules and refresh pins (psxrecomp/tools/ci/record_pins.sh)." >&2
  exit 1
fi
echo "framework pins verified."
