#!/usr/bin/env bash
# Download and unpack a retcomm-toolchains cmake-clang-v1 pack.
#
# Usage:
#   fetch_toolchain.sh --artifact <linux-x64|windows-x64|macos-arm64|macos-x64> \
#                      [--dl-dir DIR] [--out-dir DIR] [--repo OWNER/NAME]
#
# Prints: TOOLCHAIN_DIR=<absolute path with bin/>
# When GITHUB_ENV is set (Actions), also appends:
#   TOOLCHAIN_DIR=...
#   PSXRECOMP_TOOLCHAIN_DIR=...
#   BPE_TOOLCHAIN_DIR=...   (compat alias for existing title workflows)
#
# Env:
#   GH_TOKEN / GITHUB_TOKEN — optional; used for gh or Authorization on curl
set -euo pipefail

ARTIFACT=""
DL_DIR=".cache/toolchain-dl"
OUT_DIR=".cache/toolchain-pack"
REPO="TechnicallyComputers/retcomm-toolchains"

usage() {
  sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
  exit 2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage ;;
    --artifact) ARTIFACT="${2:?}"; shift 2 ;;
    --dl-dir) DL_DIR="${2:?}"; shift 2 ;;
    --out-dir) OUT_DIR="${2:?}"; shift 2 ;;
    --repo) REPO="${2:?}"; shift 2 ;;
    *)
      echo "error: unknown arg: $1" >&2
      usage
      ;;
  esac
done

if [[ -z "${ARTIFACT}" ]]; then
  echo "error: --artifact is required" >&2
  usage
fi

case "${ARTIFACT}" in
  linux-x64)
    PATTERN='cmake-clang-v1*linux*.zip'
    ASSET_NAME='cmake-clang-v1-linux-x64.zip'
    ;;
  windows-x64)
    PATTERN='cmake-clang-v1*windows*.zip'
    ASSET_NAME='cmake-clang-v1-windows-x64.zip'
    ;;
  macos-arm64|macos-x64)
    PATTERN='cmake-clang-v1*macos*.zip'
    ASSET_NAME='cmake-clang-v1-macos-universal.zip'
    ;;
  *)
    echo "error: unknown artifact '${ARTIFACT}' (want linux-x64|windows-x64|macos-arm64|macos-x64)" >&2
    exit 1
    ;;
esac

TOKEN="${GH_TOKEN:-${GITHUB_TOKEN:-}}"

rm -rf "${DL_DIR}" "${OUT_DIR}"
mkdir -p "${DL_DIR}" "${OUT_DIR}"

if command -v gh >/dev/null 2>&1; then
  if [[ -n "${TOKEN}" ]]; then
    export GH_TOKEN="${TOKEN}"
  fi
  gh release download -R "${REPO}" -p "${PATTERN}" -D "${DL_DIR}" --clobber
elif command -v curl >/dev/null 2>&1; then
  ASSET_URL="https://github.com/${REPO}/releases/latest/download/${ASSET_NAME}"
  echo "gh not on PATH; curling ${ASSET_URL}"
  curl_args=(-fsSL -o "${DL_DIR}/${ASSET_NAME}")
  if [[ -n "${TOKEN}" ]]; then
    curl_args+=(-H "Authorization: Bearer ${TOKEN}")
  fi
  curl "${curl_args[@]}" "${ASSET_URL}"
else
  echo "error: neither gh nor curl available to fetch toolchain pack" >&2
  exit 1
fi

ZIP="$(find "${DL_DIR}" -maxdepth 1 -type f -name '*.zip' | head -n1)"
if [[ -z "${ZIP}" ]]; then
  echo "error: no toolchain zip matching ${PATTERN}" >&2
  ls -la "${DL_DIR}" >&2 || true
  exit 1
fi

echo "Unpacking toolchain from ${ZIP}"
if command -v unzip >/dev/null 2>&1; then
  unzip -q "${ZIP}" -d "${OUT_DIR}"
else
  python3 -c '
import pathlib, zipfile, sys
z = next(pathlib.Path(sys.argv[1]).glob("*.zip"))
zipfile.ZipFile(z).extractall(sys.argv[2])
' "${DL_DIR}" "${OUT_DIR}"
fi

# Unwrap single top-level directory if present.
shopt -s nullglob
entries=("${OUT_DIR}"/*)
shopt -u nullglob
if [[ ${#entries[@]} -eq 1 && -d "${entries[0]}" ]]; then
  TOOLCHAIN_DIR="$(cd "${entries[0]}" && pwd)"
else
  TOOLCHAIN_DIR="$(cd "${OUT_DIR}" && pwd)"
fi

if [[ ! -d "${TOOLCHAIN_DIR}/bin" ]]; then
  echo "error: toolchain pack missing bin/ under ${TOOLCHAIN_DIR}" >&2
  find "${OUT_DIR}" -maxdepth 3 >&2 || true
  exit 1
fi

echo "TOOLCHAIN_DIR=${TOOLCHAIN_DIR}"
if [[ -n "${GITHUB_ENV:-}" ]]; then
  {
    echo "TOOLCHAIN_DIR=${TOOLCHAIN_DIR}"
    echo "PSXRECOMP_TOOLCHAIN_DIR=${TOOLCHAIN_DIR}"
    echo "BPE_TOOLCHAIN_DIR=${TOOLCHAIN_DIR}"
  } >>"${GITHUB_ENV}"
fi
if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
  echo "toolchain-dir=${TOOLCHAIN_DIR}" >>"${GITHUB_OUTPUT}"
fi
