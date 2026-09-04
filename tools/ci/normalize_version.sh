#!/usr/bin/env bash
# Normalize a release version string to X.Y.Z (strip leading v / whitespace),
# or compute the next semver from git tags / VERSION.
#
# Usage:
#   normalize_version.sh <raw>                      # prints VERSION=… TAG=…
#   normalize_version.sh --write VERSION <raw>      # also writes VERSION file
#   normalize_version.sh --next [patch|minor|major] # bump from latest vX.Y.Z tag
#   normalize_version.sh --write VERSION --next patch
#
# --next looks at (in order):
#   1) latest git tag matching vMAJOR.MINOR.PATCH*
#   2) VERSION file in cwd (if present)
#   3) 0.0.0
# then increments the chosen component (default: patch).
#
# When GITHUB_OUTPUT is set, appends version= and tag=.
set -euo pipefail

WRITE=""
RAW=""
NEXT=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --write)
      WRITE="${2:?}"
      shift 2
      ;;
    --next)
      if [[ $# -ge 2 && "$2" != -* ]]; then
        NEXT="$2"
        shift 2
      else
        NEXT="patch"
        shift
      fi
      ;;
    -h|--help)
      sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      RAW="$1"
      shift
      ;;
  esac
done

bump_semver() {
  local base="$1" which="${2:-patch}"
  local ma mi pa
  base="${base#v}"
  base="${base%%[-+]*}"
  if [[ ! "${base}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "error: cannot bump non-semver base '${base}'" >&2
    exit 1
  fi
  IFS=. read -r ma mi pa <<<"${base}"
  ma="${ma:-0}"
  mi="${mi:-0}"
  pa="${pa:-0}"
  case "${which}" in
    major) ma=$((ma + 1)); mi=0; pa=0 ;;
    minor) mi=$((mi + 1)); pa=0 ;;
    patch) pa=$((pa + 1)) ;;
    *)
      echo "error: unknown bump '${which}' (want patch|minor|major)" >&2
      exit 1
      ;;
  esac
  printf '%s.%s.%s' "${ma}" "${mi}" "${pa}"
}

latest_semver_base() {
  local tag=""
  if git rev-parse --git-dir >/dev/null 2>&1; then
    tag="$(git tag -l 'v[0-9]*.[0-9]*.[0-9]*' --sort=-v:refname 2>/dev/null | head -1 || true)"
  fi
  if [[ -n "${tag}" ]]; then
    printf '%s\n' "${tag#v}"
    return 0
  fi
  if [[ -f VERSION ]]; then
    local v
    v="$(tr -d '[:space:]' <VERSION)"
    v="${v#v}"
    if [[ -n "${v}" ]]; then
      printf '%s\n' "${v}"
      return 0
    fi
  fi
  printf '0.0.0\n'
}

if [[ -n "${NEXT}" ]]; then
  if [[ -n "${RAW}" ]]; then
    echo "error: pass either <raw> or --next, not both" >&2
    exit 2
  fi
  case "${NEXT}" in
    patch|minor|major) ;;
    *)
      echo "error: --next wants patch|minor|major (got '${NEXT}')" >&2
      exit 1
      ;;
  esac
  BASE="$(latest_semver_base)"
  VER="$(bump_semver "${BASE}" "${NEXT}")"
  echo "Auto-bumped ${NEXT}: ${BASE} → ${VER}" >&2
elif [[ -n "${RAW}" ]]; then
  RAW="$(printf '%s' "${RAW}" | tr -d '[:space:]')"
  VER="${RAW#v}"
  if [[ ! "${VER}" =~ ^[0-9]+\.[0-9]+\.[0-9]+([.+-][A-Za-z0-9.+-]*)?$ ]]; then
    echo "error: invalid version '${RAW}' (want X.Y.Z or vX.Y.Z)" >&2
    exit 1
  fi
else
  echo "usage: $0 [--write VERSION] <raw-version>" >&2
  echo "       $0 [--write VERSION] --next [patch|minor|major]" >&2
  exit 2
fi

TAG="v${VER}"

echo "VERSION=${VER}"
echo "TAG=${TAG}"

if [[ -n "${WRITE}" ]]; then
  printf '%s\n' "${VER}" >"${WRITE}"
fi
if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
  echo "version=${VER}" >>"${GITHUB_OUTPUT}"
  echo "tag=${TAG}" >>"${GITHUB_OUTPUT}"
fi
