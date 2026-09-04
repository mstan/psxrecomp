#!/usr/bin/env bash
# Stage psxrecomp emitters + OpenBIOS surface + optional portable toolchain
# into an existing setup-host release stage directory.
#
# Title packagers copy the host exe, game sources, and framework tree first,
# then call this to finish the RetComM/wizard-complete zip layout.
#
# Usage:
#   stage_setup_sdk.sh --stage <stage-dir> [options]
#
# Options:
#   --framework DIR         psxrecomp source tree (default: <cwd>/psxrecomp)
#   --sdk-overlay DIR       Legacy optional overlay onto stage/psxrecomp
#                           (prefer shipping CLI/tools inside the submodule)
#   --recompiler-build DIR  Where psxrecomp-game/bios were built (repeatable)
#   --toolchain-dir DIR     Pack root with bin/; embedded as stage/toolchain/
#   --allow-no-toolchain    Warn instead of failing when toolchain unset
#   --host-exe PATH         Host PE for Windows DLL bundling (optional)
#   --runtime-bin DIR       MinGW runtime DLL search dir (repeatable)
#   --search-dir DIR        Extra DLL search dir (repeatable)
#   --require-cli           Require psxrecomp_cli.py after overlays (default on)
#
# Env aliases for toolchain dir (first wins):
#   PSXRECOMP_TOOLCHAIN_DIR, TOOLCHAIN_DIR, BPE_TOOLCHAIN_DIR
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FW_TOOLS="${SCRIPT_DIR}"

STAGE=""
FRAMEWORK=""
SDK_OVERLAY=""
RECOMPILER_BUILDS=()
# Empty until --toolchain-dir or (when not allow-no) env aliases.
TOOLCHAIN_DIR=""
TOOLCHAIN_DIR_SET=0
ALLOW_NO_TOOLCHAIN=0
HOST_EXE=""
RUNTIME_BINS=()
SEARCH_DIRS=()
REQUIRE_CLI=1

usage() {
  sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'
  exit 2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage ;;
    --stage) STAGE="${2:?}"; shift 2 ;;
    --framework) FRAMEWORK="${2:?}"; shift 2 ;;
    --sdk-overlay) SDK_OVERLAY="${2:?}"; shift 2 ;;
    --recompiler-build) RECOMPILER_BUILDS+=("${2:?}"); shift 2 ;;
    --toolchain-dir) TOOLCHAIN_DIR="${2:?}"; TOOLCHAIN_DIR_SET=1; shift 2 ;;
    --allow-no-toolchain) ALLOW_NO_TOOLCHAIN=1; shift ;;
    --host-exe) HOST_EXE="${2:?}"; shift 2 ;;
    --runtime-bin) RUNTIME_BINS+=("${2:?}"); shift 2 ;;
    --search-dir) SEARCH_DIRS+=("${2:?}"); shift 2 ;;
    --require-cli) REQUIRE_CLI=1; shift ;;
    --no-require-cli) REQUIRE_CLI=0; shift ;;
    *)
      echo "error: unknown arg: $1" >&2
      usage
      ;;
  esac
done

if [[ -z "${STAGE}" ]]; then
  echo "error: --stage is required" >&2
  usage
fi
STAGE="$(cd "${STAGE}" && pwd)"

# Env aliases only when embedding is expected (not --allow-no-toolchain alone).
if [[ "${TOOLCHAIN_DIR_SET}" -eq 0 && "${ALLOW_NO_TOOLCHAIN}" -eq 0 ]]; then
  TOOLCHAIN_DIR="${PSXRECOMP_TOOLCHAIN_DIR:-${TOOLCHAIN_DIR:-${BPE_TOOLCHAIN_DIR:-}}}"
fi

if [[ -z "${FRAMEWORK}" ]]; then
  if [[ -d "${PWD}/psxrecomp" ]]; then
    FRAMEWORK="$(cd "${PWD}/psxrecomp" && pwd)"
  else
    FRAMEWORK="$(cd "${SCRIPT_DIR}/.." && pwd)"
  fi
else
  FRAMEWORK="$(cd "${FRAMEWORK}" && pwd)"
fi

mkdir -p "${STAGE}/psxrecomp"

# Optional SDK overlay (CLI / prepare_disc / progress helpers).
if [[ -n "${SDK_OVERLAY}" ]]; then
  if [[ ! -d "${SDK_OVERLAY}" ]]; then
    echo "error: --sdk-overlay not a directory: ${SDK_OVERLAY}" >&2
    exit 1
  fi
  if command -v rsync >/dev/null 2>&1; then
    rsync -a "${SDK_OVERLAY}/" "${STAGE}/psxrecomp/"
  else
    cp -a "${SDK_OVERLAY}/." "${STAGE}/psxrecomp/"
  fi
  echo "overlaid SDK from ${SDK_OVERLAY}"
fi

find_tool_bin() {
  local name="$1"
  local prefer_exe="${2:-0}"
  local dir cand
  local -a roots=()
  local r
  for r in "${RECOMPILER_BUILDS[@]+"${RECOMPILER_BUILDS[@]}"}"; do
    [[ -n "${r}" ]] || continue
    if [[ ! -d "${r}" ]]; then
      echo "error: --recompiler-build not a directory: ${r}" >&2
      exit 1
    fi
    roots+=("$(cd -- "${r}" && pwd)")
  done
  roots+=(
    "${STAGE}/psxrecomp/recompiler/build"
    "${FRAMEWORK}/recompiler/build"
    "${PWD}/build-recompiler"
  )
  for dir in "${roots[@]}"; do
    [[ -d "${dir}" ]] || continue
    if [[ "${prefer_exe}" -eq 1 ]]; then
      for cand in \
        "${dir}/${name}.exe" \
        "${dir}/Release/${name}.exe" \
        "${dir}/${name}"
      do
        if [[ -f "${cand}" ]]; then
          echo "${cand}"
          return 0
        fi
      done
    else
      for cand in \
        "${dir}/${name}" \
        "${dir}/${name}.exe" \
        "${dir}/Release/${name}.exe"
      do
        if [[ -f "${cand}" ]]; then
          echo "${cand}"
          return 0
        fi
      done
    fi
  done
  return 1
}

WANT_WIN_EMITTERS=0
if [[ -n "${HOST_EXE}" && "${HOST_EXE}" == *.exe ]]; then
  WANT_WIN_EMITTERS=1
fi

GAME_BIN="$(find_tool_bin psxrecomp-game "${WANT_WIN_EMITTERS}" || true)"
BIOS_BIN="$(find_tool_bin psxrecomp-bios "${WANT_WIN_EMITTERS}" || true)"
if [[ -z "${GAME_BIN}" ]]; then
  echo "error: psxrecomp-game not found (pass --recompiler-build)" >&2
  if [[ "${WANT_WIN_EMITTERS}" -eq 1 ]]; then
    echo "  Windows setup zip needs psxrecomp-game.exe (MinGW cross-build)," >&2
    echo "  not a Linux ELF from build-recompiler." >&2
  fi
  exit 1
fi
if [[ -z "${BIOS_BIN}" ]]; then
  echo "error: psxrecomp-bios not found (required for OpenBIOS regen)" >&2
  exit 1
fi
if [[ "${WANT_WIN_EMITTERS}" -eq 1 ]]; then
  if [[ "${GAME_BIN}" != *.exe || "${BIOS_BIN}" != *.exe ]]; then
    echo "error: Windows setup host requires MinGW emitter .exes, found:" >&2
    echo "  game: ${GAME_BIN}" >&2
    echo "  bios: ${BIOS_BIN}" >&2
    echo "  Build with: cmake -S psxrecomp/recompiler -B build-recompiler-mingw \\" >&2
    echo "    -DCMAKE_TOOLCHAIN_FILE=psxrecomp/cmake/toolchain-mingw-w64.cmake \\" >&2
    echo "    -DPSXRECOMP_STATIC_CLI=ON && cmake --build build-recompiler-mingw \\" >&2
    echo "    --target psxrecomp-game psxrecomp-bios" >&2
    exit 1
  fi
fi

mkdir -p "${STAGE}/psxrecomp/recompiler/build"
cp -a "${GAME_BIN}" \
  "${STAGE}/psxrecomp/recompiler/build/$(basename "${GAME_BIN}")"
cp -a "${BIOS_BIN}" \
  "${STAGE}/psxrecomp/recompiler/build/$(basename "${BIOS_BIN}")"
chmod +x "${STAGE}/psxrecomp/recompiler/build/$(basename "${GAME_BIN}")" 2>/dev/null || true
chmod +x "${STAGE}/psxrecomp/recompiler/build/$(basename "${BIOS_BIN}")" 2>/dev/null || true
echo "staged emitters from ${GAME_BIN%/*}"

cat >"${STAGE}/psxrecomp/retcomm-sdk.json" <<'EOF'
{
  "cli": "psxrecomp_cli.py",
  "id": "psxrecomp-tools",
  "game_bin": "recompiler/build/psxrecomp-game",
  "bios_bin": "recompiler/build/psxrecomp-bios"
}
EOF

for f in OpenBIOS.toml openbios.bin OpenBIOS.LICENSE SCPH1001.toml; do
  if [[ ! -f "${STAGE}/psxrecomp/bios/${f}" ]]; then
    echo "error: missing psxrecomp/bios/${f} in staged tree" >&2
    exit 1
  fi
done

if [[ "${REQUIRE_CLI}" -eq 1 && ! -f "${STAGE}/psxrecomp/psxrecomp_cli.py" ]]; then
  echo "error: missing psxrecomp/psxrecomp_cli.py in staged tree" >&2
  exit 1
fi

# Never ship retail BIOS dumps from the stage.
rm -f "${STAGE}/psxrecomp/bios/SCPH1001.BIN" \
      "${STAGE}/psxrecomp/bios/SCPH1001.bin" 2>/dev/null || true

if [[ -n "${TOOLCHAIN_DIR}" && -d "${TOOLCHAIN_DIR}" ]]; then
  if [[ ! -d "${TOOLCHAIN_DIR}/bin" ]]; then
    echo "error: toolchain dir missing bin/: ${TOOLCHAIN_DIR}" >&2
    exit 1
  fi
  mkdir -p "${STAGE}/toolchain"
  if command -v rsync >/dev/null 2>&1; then
    rsync -a --delete "${TOOLCHAIN_DIR}/" "${STAGE}/toolchain/"
  else
    rm -rf "${STAGE}/toolchain"
    mkdir -p "${STAGE}/toolchain"
    cp -a "${TOOLCHAIN_DIR}/." "${STAGE}/toolchain/"
  fi
  echo "bundled toolchain from ${TOOLCHAIN_DIR}"
elif [[ "${ALLOW_NO_TOOLCHAIN}" -eq 1 ]]; then
  echo "note: no embedded toolchain/ — RetComM/wizard will download cmake-clang-v1" \
       "(or accept an offline zip / system cmake)" >&2
else
  echo "error: toolchain dir required (pass --toolchain-dir or set PSXRECOMP_TOOLCHAIN_DIR)" >&2
  exit 1
fi

# Windows MinGW DLL bundling for host + emitters.
# Studio MinGW package exports PSXRECOMP_BUNDLE_MINGW_DLLS so a stale
# game-submodule copy is not used.
BUNDLE="${PSXRECOMP_BUNDLE_MINGW_DLLS:-}"
if [[ -z "${BUNDLE}" || ! -f "${BUNDLE}" ]]; then
  BUNDLE="${FW_TOOLS}/bundle_mingw_dlls.sh"
fi
if [[ ! -f "${BUNDLE}" ]]; then
  # Staged copy (after sdk overlay) may own the helper.
  if [[ -f "${STAGE}/psxrecomp/tools/bundle_mingw_dlls.sh" ]]; then
    BUNDLE="${STAGE}/psxrecomp/tools/bundle_mingw_dlls.sh"
  fi
fi

need_dlls=0
if [[ -n "${HOST_EXE}" && "${HOST_EXE}" == *.exe ]]; then
  need_dlls=1
fi
if [[ -f "${STAGE}/psxrecomp/recompiler/build/psxrecomp-game.exe" ]]; then
  need_dlls=1
fi

if [[ "${need_dlls}" -eq 1 ]]; then
  if [[ ! -f "${BUNDLE}" ]]; then
    echo "error: bundle_mingw_dlls.sh not found next to stage_setup_sdk.sh" >&2
    exit 1
  fi
  chmod +x "${BUNDLE}" 2>/dev/null || true

  for emitter in psxrecomp-game.exe psxrecomp-bios.exe; do
    if [[ ! -f "${STAGE}/psxrecomp/recompiler/build/${emitter}" ]]; then
      echo "error: missing ${emitter} in staged tree" >&2
      exit 1
    fi
  done

  args=()
  for d in "${RUNTIME_BINS[@]+"${RUNTIME_BINS[@]}"}"; do
    args+=(--runtime-bin "${d}")
  done
  for d in "${SEARCH_DIRS[@]+"${SEARCH_DIRS[@]}"}"; do
    args+=(--search-dir "${d}")
  done
  if [[ -n "${HOST_EXE}" && -f "${HOST_EXE}" ]]; then
    args+=(
      --exe "${HOST_EXE}"
      --dest "${STAGE}"
      --label "$(basename "${HOST_EXE}")"
    )
  fi
  args+=(
    --exe "${STAGE}/psxrecomp/recompiler/build/psxrecomp-game.exe"
    --dest "${STAGE}/psxrecomp/recompiler/build"
    --label "psxrecomp-game.exe"
    --exe "${STAGE}/psxrecomp/recompiler/build/psxrecomp-bios.exe"
    --dest "${STAGE}/psxrecomp/recompiler/build"
    --label "psxrecomp-bios.exe"
  )
  # Host (MSYS2 GCC) still needs these when imported. llvm-mingw static
  # emitters typically import neither — --require is skipped per-exe then.
  # zlib1.dll is a savestate (ZLIB::ZLIB) import; PSX_STATIC_RUNTIME does not
  # fold it, and --require of only GCC runtimes was a no-op on static-libgcc
  # hosts so CI shipped zips that died on clean Windows ("zlib1.dll not found").
  if [[ -n "${HOST_EXE}" && -f "${HOST_EXE}" ]]; then
    args+=(
      --require libgcc_s_seh-1.dll
      --require libstdc++-6.dll
      --require libwinpthread-1.dll
      --require libssp-0.dll
      --require zlib1.dll
      --require z.dll
    )
  fi
  bash "${BUNDLE}" "${args[@]}"
fi

echo "stage_setup_sdk: ready under ${STAGE}/psxrecomp"
