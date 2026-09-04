#!/usr/bin/env bash
# Assert the three baked copies of the boot-EXE name agree.
#
# A game project carries the boot-EXE name in three places, all filled from
# the same @BOOT_EXE@ token at instantiation: game.toml ([prepare_disc]
# boot_exe, or the basename of [game] exe), CMakeLists.txt (GEN_MARKER),
# and codegen_setup.c (.gen_marker_relpath). generate names its output
# after game.toml while the build and the launcher gate on the other two,
# so a skew makes every player's self-build loop generate+build forever.
# The CLI also refuses the skew at generate time; failing the release here
# is cheaper than failing the player.
#
# Usage: check_boot_exe.sh [game-repo-root]   (default: .)
# Runs on bash 3.2+ (macOS runners) — no associative arrays.
set -euo pipefail

ROOT="${1:-.}"

toml_get() { # <section> <key> — first quoted value of key inside [section]
  awk -v sec="$1" -v key="$2" '
    /^[[:space:]]*\[/ { in_sec = ($0 ~ ("^[[:space:]]*\\[" sec "\\]")) }
    in_sec && $0 ~ ("^[[:space:]]*" key "[[:space:]]*=") {
      if (match($0, /"[^"]*"/)) { print substr($0, RSTART + 1, RLENGTH - 2) }
      exit
    }' "${ROOT}/game.toml"
}

n_toml="" n_cmake="" n_setup=""

if [[ -f "${ROOT}/game.toml" ]]; then
  n_toml="$(toml_get prepare_disc boot_exe || true)"
  if [[ -z "${n_toml}" ]]; then
    exe="$(toml_get game exe || true)"
    n_toml="${exe##*/}"
  fi
fi
if [[ -f "${ROOT}/CMakeLists.txt" ]]; then
  n_cmake="$(sed -n \
    's/.*GEN_MARKER[[:space:]]*"generated\/\(.*\)_dispatch\.c".*/\1/p' \
    "${ROOT}/CMakeLists.txt" | head -n1)"
fi
if [[ -f "${ROOT}/codegen_setup.c" ]]; then
  n_setup="$(sed -n \
    's/.*\.gen_marker_relpath[[:space:]]*=[[:space:]]*"generated\/\(.*\)_dispatch\.c".*/\1/p' \
    "${ROOT}/codegen_setup.c" | head -n1)"
fi

found=0
ref=""
mismatch=0
report() { # <label> <value>
  [[ -z "$2" ]] && return 0
  echo "  $1 -> $2"
  found=$((found + 1))
  if [[ -z "${ref}" ]]; then
    ref="$2"
  elif [[ "$2" != "${ref}" ]]; then
    mismatch=1
  fi
}

echo "boot-EXE name sources:"
report "game.toml (boot_exe / game.exe)" "${n_toml}"
report "CMakeLists.txt GEN_MARKER" "${n_cmake}"
report "codegen_setup.c gen_marker_relpath" "${n_setup}"

if [[ "${found}" -lt 2 ]]; then
  echo "warning: fewer than two boot-EXE sources found under ${ROOT} —" \
    "nothing to cross-check (non-standard layout?)"
  exit 0
fi
if [[ "${mismatch}" -ne 0 ]]; then
  echo "error: the boot-EXE names above disagree. generate names its output" >&2
  echo "after game.toml, while the build (GEN_MARKER) and the launcher" >&2
  echo "(gen_marker_relpath) gate on the exact other names — players'" >&2
  echo "self-builds would loop generate+build forever. Make all three name" >&2
  echo "the same boot EXE." >&2
  exit 1
fi
echo "boot-EXE names agree: ${ref}"
