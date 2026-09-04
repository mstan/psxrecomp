#!/usr/bin/env bash
# Prefetch the pinned SDL3 source tree for FetchContent.
#
# CMake's file(DOWNLOAD) uses libcurl with HTTP/2 against github.com release
# assets and intermittently fails with REFUSED_STREAM / status 56 on GHA.
# curl --http1.1 is reliable; point cmake at the extracted tree with:
#   -DFETCHCONTENT_SOURCE_DIR_SDL3=<out-dir>
#
# Usage:
#   tools/ci/prefetch_sdl3.sh [--out-dir DIR] [--cache-dir DIR]
#
# Env overrides:
#   PSX_SDL3_VERSION   (default 3.4.10)
#   PSX_SDL3_SHA256    (default matches runtime.cmake URL_HASH)
#   PSX_SDL3_URL       (default GitHub release tarball)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT_DIR=""
CACHE_DIR=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --out-dir) OUT_DIR="${2:?}"; shift 2 ;;
    --cache-dir) CACHE_DIR="${2:?}"; shift 2 ;;
    -h|--help)
      sed -n '1,20p' "$0"
      exit 0
      ;;
    *)
      echo "usage: $0 [--out-dir DIR] [--cache-dir DIR]" >&2
      exit 2
      ;;
  esac
done

VERSION="${PSX_SDL3_VERSION:-3.4.10}"
SHA256="${PSX_SDL3_SHA256:-12b34280415ec8418c864408b93d008a20a6530687ee613d60bfbd20411f2785}"
URL="${PSX_SDL3_URL:-https://github.com/libsdl-org/SDL/releases/download/release-${VERSION}/SDL3-${VERSION}.tar.gz}"

if [[ -z "${OUT_DIR}" ]]; then
  OUT_DIR="${GITHUB_WORKSPACE:-${ROOT}}/.cache/sdl3-src"
fi
if [[ -z "${CACHE_DIR}" ]]; then
  CACHE_DIR="$(dirname "${OUT_DIR}")"
fi

mkdir -p "${CACHE_DIR}"
TARBALL="${CACHE_DIR}/SDL3-${VERSION}.tar.gz"

# Absolute path for CMake / GITHUB_ENV (Windows: prefer D:/… via cygpath -m).
abs_path() {
  local p="$1"
  mkdir -p "${p}"
  if command -v cygpath >/dev/null 2>&1; then
    cygpath -m "${p}"
  else
    (cd "${p}" && pwd)
  fi
}

# GNU tar on Windows Git Bash treats "D:\foo" as host:path ("Cannot connect to D").
# Extract with a basename -f from the archive's directory, and a POSIX -C path
# from pwd (e.g. /d/a/…), plus --force-local when GNU tar supports it.
tar_extract() {
  local archive="$1"
  local dest="$2"
  mkdir -p "${dest}"
  local force=()
  if tar --help 2>&1 | grep -q -- '--force-local'; then
    force=(--force-local)
  fi
  local arch_dir arch_base dest_posix
  arch_dir="$(cd "$(dirname "${archive}")" && pwd)"
  arch_base="$(basename "${archive}")"
  dest_posix="$(cd "${dest}" && pwd)"
  (cd "${arch_dir}" && tar "${force[@]}" -xzf "${arch_base}" -C "${dest_posix}")
}

emit_env() {
  local abs
  abs="$(abs_path "${OUT_DIR}")"
  echo "SDL3 source ready: ${abs}"
  echo "SDL3_SRC=${abs}"
  if [[ -n "${GITHUB_ENV:-}" ]]; then
    echo "PSX_SDL3_SOURCE_DIR=${abs}" >>"${GITHUB_ENV}"
  fi
}

if [[ -f "${OUT_DIR}/CMakeLists.txt" ]]; then
  echo "SDL3 source already present: ${OUT_DIR}"
  emit_env
  exit 0
fi

echo "Prefetching SDL3 ${VERSION} (HTTP/1.1)…"
TMP="$(mktemp "${CACHE_DIR}/SDL3-${VERSION}.XXXXXX.tar.gz")"
cleanup() { rm -f "${TMP}"; }
trap cleanup EXIT

# Prefer HTTP/1.1: GitHub release assets often REFUSED_STREAM under HTTP/2.
download() {
  local url="$1"
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL --http1.1 --retry 5 --retry-all-errors --retry-delay 2 \
      -o "${TMP}" "${url}"
  elif command -v wget >/dev/null 2>&1; then
    wget -q -O "${TMP}" "${url}"
  else
    echo "error: need curl or wget to download SDL3" >&2
    exit 1
  fi
}

download "${URL}"

# sha256sum -c prints the path; on Windows that may be D:\… — still OK.
if command -v sha256sum >/dev/null 2>&1; then
  echo "${SHA256}  ${TMP}" | sha256sum -c -
elif command -v shasum >/dev/null 2>&1; then
  echo "${SHA256}  ${TMP}" | shasum -a 256 -c -
else
  echo "warning: no sha256 tool; skipping verify" >&2
fi

rm -rf "${OUT_DIR}.extracting"
tar_extract "${TMP}" "${OUT_DIR}.extracting"
# Official release tarball extracts to SDL3-<version>/
INNER="$(find "${OUT_DIR}.extracting" -mindepth 1 -maxdepth 1 -type d | head -n1)"
if [[ -z "${INNER}" || ! -f "${INNER}/CMakeLists.txt" ]]; then
  echo "error: unexpected SDL3 tarball layout under ${OUT_DIR}.extracting" >&2
  ls -la "${OUT_DIR}.extracting" >&2 || true
  exit 1
fi
rm -rf "${OUT_DIR}"
mv "${INNER}" "${OUT_DIR}"
rm -rf "${OUT_DIR}.extracting"
mv -f "${TMP}" "${TARBALL}"
trap - EXIT

emit_env
