#!/usr/bin/env bash
# Bundle imported non-system DLLs next to Windows PE executables.
#
# Title packagers (BPE/MotK setup zips) call this for Windows PEs that still
# import non-system DLLs (typically the MSYS2-built setup host + SDL2).
# Walks the exe *and* each copied DLL (zlib1.dll → libssp-0.dll, etc.).
# Emitters built with llvm-mingw + PSXRECOMP_STATIC_CLI should import none of
# the GCC runtimes.
#
# Usage:
#   bundle_mingw_dlls.sh [options] --exe <path> [--dest <dir>] [--label <name>] ...
#
# Options:
#   --runtime-bin DIR   Preferred DLL search directory (repeatable)
#   --search-dir DIR    Extra DLL search directory (repeatable)
#   --exe PATH          PE to inspect (repeatable; --dest/--label apply to it)
#   --dest DIR          Copy DLLs here (default: dirname of the preceding --exe)
#   --label NAME        Log label for the preceding --exe
#   --require DLL       After bundling, DLL must exist in every --dest that
#                       imported it (repeatable)
#   --soft-missing      Warn instead of exit 1 when objdump is unavailable
#
# Exit 1 if an imported DLL cannot be found, or a --require check fails.
set -euo pipefail

SOFT_MISSING=0
RUNTIME_BINS=()
SEARCH_DIRS=()
REQUIRE=()

# Parallel arrays for targets.
EXE_PATHS=()
DEST_DIRS=()
LABELS=()

pending_dest=""
pending_label=""

usage() {
  sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'
  exit 2
}

flush_pending_meta() {
  # Apply trailing --dest/--label to the last --exe.
  local n="${#EXE_PATHS[@]}"
  if [[ "$n" -eq 0 ]]; then
    return 0
  fi
  local i=$((n - 1))
  if [[ -n "${pending_dest}" ]]; then
    DEST_DIRS[$i]="${pending_dest}"
    pending_dest=""
  fi
  if [[ -n "${pending_label}" ]]; then
    LABELS[$i]="${pending_label}"
    pending_label=""
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage ;;
    --soft-missing) SOFT_MISSING=1; shift ;;
    --runtime-bin)
      RUNTIME_BINS+=("${2:?}"); shift 2 ;;
    --search-dir)
      SEARCH_DIRS+=("${2:?}"); shift 2 ;;
    --require)
      REQUIRE+=("${2:?}"); shift 2 ;;
    --exe)
      flush_pending_meta
      EXE_PATHS+=("${2:?}")
      DEST_DIRS+=("")
      LABELS+=("")
      shift 2
      ;;
    --dest)
      pending_dest="${2:?}"; shift 2 ;;
    --label)
      pending_label="${2:?}"; shift 2 ;;
    *)
      echo "error: unknown arg: $1" >&2
      usage
      ;;
  esac
done
flush_pending_meta

if [[ ${#EXE_PATHS[@]} -eq 0 ]]; then
  echo "error: at least one --exe is required" >&2
  usage
fi

OBJDUMP=""
if command -v x86_64-w64-mingw32-objdump >/dev/null 2>&1; then
  OBJDUMP="x86_64-w64-mingw32-objdump"
elif command -v objdump >/dev/null 2>&1; then
  OBJDUMP="objdump"
else
  if [[ "${SOFT_MISSING}" -eq 1 ]]; then
    echo "warning: no objdump; skipping MinGW DLL bundling" >&2
    exit 0
  fi
  echo "error: no objdump; cannot bundle MinGW DLLs" >&2
  exit 1
fi

SYSTEM_DLL_RE='^(KERNEL32|USER32|GDI32|ADVAPI32|SHELL32|OLE32|OLEAUT32|WS2_32|WINMM|IMM32|SETUPAPI|VERSION|OPENGL32|COMCTL32|COMDLG32|RPCRT4|SHLWAPI|CRYPT32|BCRYPT|IPHLPAPI|NSI|DNSAPI|MSVCRT|UCRTBASE|VCRUNTIME|DBGHELP|API-MS-).*\.DLL$'

PROBE_DLLS=(
  SDL2.dll
  zlib1.dll
  libgcc_s_seh-1.dll
  libstdc++-6.dll
  libc++.dll
  libunwind.dll
  libwinpthread-1.dll
  libssp-0.dll
)

# Companion GCC/MinGW runtimes. Copied when the PE or any already-copied DLL
# imports them (direct or transitive). libssp-0 is pulled in by zlib1.dll even
# when the host exe itself is -static-libgcc.
ALWAYS_RUNTIME_DLLS=(
  libgcc_s_seh-1.dll
  libstdc++-6.dll
  libwinpthread-1.dll
  libssp-0.dll
)

dll_name_eq() {
  local a b
  a="$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')"
  b="$(printf '%s' "$2" | tr '[:upper:]' '[:lower:]')"
  [[ "${a}" == "${b}" ]]
}

is_system_dll() {
  local name="$1"
  printf '%s' "${name}" | grep -qiE "${SYSTEM_DLL_RE}"
}

is_always_runtime_dll() {
  local dll="$1" cand
  for cand in "${ALWAYS_RUNTIME_DLLS[@]}"; do
    if dll_name_eq "${cand}" "${dll}"; then
      return 0
    fi
  done
  return 1
}

# Avoid `objdump | grep -q` under pipefail: grep -q exits early → objdump SIGPIPE
# → pipeline fails even when the DLL Name line matched.
list_exe_imports() {
  local exe="$1"
  "${OBJDUMP}" -p "${exe}" 2>/dev/null \
    | awk '/DLL Name:/{print $3}' \
    || true
}

exe_imports_dll() {
  local exe="$1"
  local dll="$2"
  local name
  while IFS= read -r name; do
    [[ -n "${name}" ]] || continue
    if dll_name_eq "${name}" "${dll}"; then
      return 0
    fi
  done < <(list_exe_imports "${exe}")
  return 1
}

find_dll_src() {
  local dll="$1"
  local exe="$2"
  local dest_dir="$3"
  local cand d name
  local -a names=("${dll}")
  local -a candidates=()
  local lower
  lower="$(printf '%s' "${dll}" | tr '[:upper:]' '[:lower:]')"
  # MSVC/vcpkg: zlib1.dll; some MinGW layouts: z.dll / zlib.dll.
  case "${lower}" in
    z.dll) names+=("zlib1.dll" "zlib.dll") ;;
    zlib1.dll) names+=("z.dll" "zlib.dll") ;;
    zlib.dll) names+=("zlib1.dll" "z.dll") ;;
  esac
  for name in "${names[@]}"; do
    candidates+=(
      "$(dirname "${exe}")/${name}"
      "${dest_dir}/${name}"
      "/mingw64/bin/${name}"
      "/usr/x86_64-w64-mingw32/bin/${name}"
    )
    for d in "${SEARCH_DIRS[@]+"${SEARCH_DIRS[@]}"}"; do
      candidates+=("${d}/${name}")
    done
    for d in "${RUNTIME_BINS[@]+"${RUNTIME_BINS[@]}"}"; do
      candidates+=("${d}/${name}")
    done
  done
  for cand in "${candidates[@]}"; do
    [[ -n "${cand}" ]] || continue
    if [[ -f "${cand}" ]]; then
      printf '%s' "${cand}"
      return 0
    fi
  done
  return 1
}

# True if exe or any PE already in dest imports dll (direct).
tree_imports_dll() {
  local exe="$1"
  local dest_dir="$2"
  local dll="$3"
  local pe
  if exe_imports_dll "${exe}" "${dll}"; then
    return 0
  fi
  shopt -s nullglob
  for pe in "${dest_dir}"/*.dll "${dest_dir}"/*.DLL "${dest_dir}"/*.exe "${dest_dir}"/*.EXE; do
    [[ -f "${pe}" ]] || continue
    if exe_imports_dll "${pe}" "${dll}"; then
      shopt -u nullglob
      return 0
    fi
  done
  shopt -u nullglob
  return 1
}

bundle_one() {
  local exe="$1"
  local dest_dir="$2"
  local label="$3"
  local dll src key name dest_file src_res dest_res
  local -a queue=()
  local -A queued=()

  if [[ ! -f "${exe}" ]]; then
    echo "error: cannot bundle DLLs; missing ${exe}" >&2
    exit 1
  fi
  if [[ -z "${dest_dir}" ]]; then
    dest_dir="$(dirname "${exe}")"
  fi
  if [[ -z "${label}" ]]; then
    label="$(basename "${exe}")"
  fi
  mkdir -p "${dest_dir}"

  enqueue() {
    local n="$1"
    local k
    [[ -n "${n}" ]] || return 0
    if is_system_dll "${n}"; then
      return 0
    fi
    k="$(printf '%s' "${n}" | tr '[:upper:]' '[:lower:]')"
    if [[ -n "${queued[$k]:-}" ]]; then
      return 0
    fi
    queued[$k]=1
    queue+=("${n}")
  }

  while IFS= read -r name; do
    enqueue "${name}"
  done < <(list_exe_imports "${exe}")

  # Probe names the exe actually imports (covers case / alias mismatches).
  for dll in "${PROBE_DLLS[@]}" "${ALWAYS_RUNTIME_DLLS[@]}"; do
    if exe_imports_dll "${exe}" "${dll}"; then
      enqueue "${dll}"
    fi
  done

  local i=0
  while [[ "${i}" -lt "${#queue[@]}" ]]; do
    dll="${queue[$i]}"
    i=$((i + 1))
    src="$(find_dll_src "${dll}" "${exe}" "${dest_dir}" || true)"
    if [[ -z "${src}" ]]; then
      echo "error: required DLL missing for ${label}: ${dll}" >&2
      echo "  exe: ${exe}" >&2
      echo "  dest: ${dest_dir}" >&2
      if [[ ${#SEARCH_DIRS[@]} -gt 0 ]]; then
        echo "  search-dirs: ${SEARCH_DIRS[*]}" >&2
      fi
      if [[ ${#RUNTIME_BINS[@]} -gt 0 ]]; then
        echo "  runtime-bins: ${RUNTIME_BINS[*]}" >&2
      fi
      exit 1
    fi
    dest_file="${dest_dir}/${dll}"
    if [[ -f "${dest_file}" ]] && [[ "${src}" -ef "${dest_file}" ]]; then
      echo "bundled ${dll} → ${dest_dir}/ (${label}; already present)"
    else
      src_res="$(cd "$(dirname "${src}")" && pwd)/$(basename "${src}")"
      dest_res="$(cd "${dest_dir}" && pwd)/${dll}"
      if [[ "${src_res}" == "${dest_res}" ]]; then
        echo "bundled ${dll} → ${dest_dir}/ (${label}; already present)"
      else
        # Copy to the PE import name (zlib1.dll source may satisfy z.dll).
        cp -f "${src}" "${dest_file}"
        echo "bundled ${dll} → ${dest_dir}/ (${label})"
      fi
    fi
    # Recurse: zlib1.dll imports libssp-0.dll even when the host does not.
    if [[ -f "${dest_file}" ]]; then
      while IFS= read -r name; do
        enqueue "${name}"
      done < <(list_exe_imports "${dest_file}")
    elif [[ -f "${src}" ]]; then
      while IFS= read -r name; do
        enqueue "${name}"
      done < <(list_exe_imports "${src}")
    fi
  done
}

for i in "${!EXE_PATHS[@]}"; do
  bundle_one "${EXE_PATHS[$i]}" "${DEST_DIRS[$i]}" "${LABELS[$i]}"
done

for dll in "${REQUIRE[@]}"; do
  [[ -n "${dll}" ]] || continue
  for i in "${!EXE_PATHS[@]}"; do
    exe="${EXE_PATHS[$i]}"
    dest="${DEST_DIRS[$i]:-}"
    if [[ -z "${dest}" ]]; then
      dest="$(dirname "${exe}")"
    fi
    if ! tree_imports_dll "${exe}" "${dest}" "${dll}"; then
      continue
    fi
    if [[ ! -f "${dest}/${dll}" ]]; then
      echo "error: $(basename "${exe}") needs ${dll} but it is missing under ${dest}" >&2
      exit 1
    fi
  done
done
