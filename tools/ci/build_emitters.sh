#!/usr/bin/env bash
# Configure and build psxrecomp-game + psxrecomp-bios.
#
# On Windows CI, prefer the portable cmake-clang-v1 (llvm-mingw) pack so
# emitters statically link libc++ and do not need MSYS2 GCC DLLs.
#
# Usage (from game repo root):
#   psxrecomp/tools/ci/build_emitters.sh \
#     [--framework psxrecomp] [--build-dir build-recompiler] \
#     [--toolchain-dir DIR] [--jobs N]
#
# Env:
#   PSXRECOMP_TOOLCHAIN_DIR | TOOLCHAIN_DIR | BPE_TOOLCHAIN_DIR
set -euo pipefail

FRAMEWORK="psxrecomp"
BUILD_DIR="build-recompiler"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
TOOLCHAIN_DIR="${PSXRECOMP_TOOLCHAIN_DIR:-${TOOLCHAIN_DIR:-${BPE_TOOLCHAIN_DIR:-}}}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --framework) FRAMEWORK="${2:?}"; shift 2 ;;
    --build-dir) BUILD_DIR="${2:?}"; shift 2 ;;
    --toolchain-dir) TOOLCHAIN_DIR="${2:?}"; shift 2 ;;
    --jobs) JOBS="${2:?}"; shift 2 ;;
    -h|--help)
      sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "error: unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

if [[ ! -d "${FRAMEWORK}/recompiler" ]]; then
  echo "error: ${FRAMEWORK}/recompiler missing" >&2
  exit 1
fi

CMAKE_ARGS=(
  -S "${FRAMEWORK}/recompiler"
  -B "${BUILD_DIR}"
  -G Ninja
  -DCMAKE_BUILD_TYPE=Release
)

# Prefer portable llvm-mingw pack when present (Windows release CI).
if [[ -n "${TOOLCHAIN_DIR}" && -d "${TOOLCHAIN_DIR}/bin" ]]; then
  TOOLCHAIN_DIR="$(cd "${TOOLCHAIN_DIR}" && pwd)"
  export PATH="${TOOLCHAIN_DIR}/bin:${PATH}"
  if [[ -f "${TOOLCHAIN_DIR}/env.sh" ]]; then
    # shellcheck disable=SC1091
    source "${TOOLCHAIN_DIR}/env.sh"
  fi

  pick_tool() {
    local name="$1"
    if [[ -f "${TOOLCHAIN_DIR}/bin/${name}.exe" ]]; then
      echo "${TOOLCHAIN_DIR}/bin/${name}.exe"
    elif [[ -f "${TOOLCHAIN_DIR}/bin/${name}" ]]; then
      echo "${TOOLCHAIN_DIR}/bin/${name}"
    else
      return 1
    fi
  }

  if CC_BIN="$(pick_tool clang)"; then
    CMAKE_ARGS+=("-DCMAKE_C_COMPILER=${CC_BIN}")
  fi
  if CXX_BIN="$(pick_tool clang++)"; then
    CMAKE_ARGS+=("-DCMAKE_CXX_COMPILER=${CXX_BIN}")
  fi
  if CMAKE_BIN="$(pick_tool cmake)"; then
    CMAKE="${CMAKE_BIN}"
  else
    CMAKE="cmake"
  fi
  # Static CRT/libc++ so staged emitters need no MSYS2 GCC DLLs.
  CMAKE_ARGS+=(-DPSXRECOMP_STATIC_CLI=ON)
  echo "build_emitters: using portable toolchain at ${TOOLCHAIN_DIR}"
else
  CMAKE="cmake"
  if [[ -d /mingw64/bin ]]; then
    export PATH="/mingw64/bin:${PATH}"
    echo "build_emitters: no portable toolchain; using MSYS2 /mingw64"
  fi
fi

"${CMAKE}" "${CMAKE_ARGS[@]}"
"${CMAKE}" --build "${BUILD_DIR}" --target psxrecomp-game psxrecomp-bios -j"${JOBS}"

for bin in psxrecomp-bios psxrecomp-game; do
  if [[ -f "${BUILD_DIR}/${bin}.exe" ]]; then
    chmod +x "${BUILD_DIR}/${bin}.exe"
  elif [[ -f "${BUILD_DIR}/${bin}" ]]; then
    chmod +x "${BUILD_DIR}/${bin}"
  else
    echo "error: ${bin} missing after recompiler build" >&2
    exit 1
  fi
done

echo "emitters ready under ${BUILD_DIR}"
