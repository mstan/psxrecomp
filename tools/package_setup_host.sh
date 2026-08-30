#!/usr/bin/env bash
# Universal setup-host zip packager for PSXRecomp game repos.
#
# Stages the host exe, title sources, filtered psxrecomp/ + recomp-ui/, then
# finishes with stage_setup_sdk.sh (emitters, OpenBIOS, MinGW DLLs).
# Portable cmake/clang is NOT embedded by default — RetComM / the setup wizard
# download cmake-clang-v1 from retcomm-toolchains (or accept an offline zip).
#
# Usage (from game repo root):
#   psxrecomp/tools/package_setup_host.sh \
#     --build-dir build-ci \
#     --artifact linux-x64 \
#     --zip-prefix bpe \
#     --exe-name Bomberman_Party_Edition_Recompiled \
#     --display-name "Bomberman Party Edition Recompiled" \
#     --recompiler-build build-recompiler \
#     [--project-file REL]... [--project-dir REL]... \
#     [--disc-hint "your legally owned disc"] \
#     [--version-env BPE_RELEASE_VERSION] \
#     [--embed-toolchain]   # optional: copy PSXRECOMP_TOOLCHAIN_DIR into zip
#
# Env:
#   RELEASE_VERSION / <version-env> / VERSION file  (must match binary stamp)
#   PSXRECOMP_TOOLCHAIN_DIR | TOOLCHAIN_DIR | BPE_TOOLCHAIN_DIR  (only with --embed-toolchain)
#   PSXRECOMP_EMBED_TOOLCHAIN=1  same as --embed-toolchain
#   PSXRECOMP_RUNTIME_BIN_DIR | BPE_RUNTIME_BIN_DIR  (Windows MinGW DLL search)
#
# Lobby pin: the host exe must have been built with current runtime.cmake so
# $<TARGET_FILE_DIR>/psx_game_version.txt exists. This script refuses to ship a
# VERSION file that disagrees with that stamp (netplay list filter bug).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(pwd)"

BUILD_DIR=""
ARTIFACT=""
ZIP_PREFIX=""
EXE_NAME=""
DISPLAY_NAME=""
RECOMPILER_BUILD="build-recompiler"
VERSION_ENV="RELEASE_VERSION"
DISC_HINT="your legally owned game disc"
PROJECT_FILES=()
PROJECT_DIRS=()
RUNTIME_BIN_DIR="${PSXRECOMP_RUNTIME_BIN_DIR:-${BPE_RUNTIME_BIN_DIR:-/usr/x86_64-w64-mingw32/bin}}"
EMBED_TOOLCHAIN=0
if [[ "${PSXRECOMP_EMBED_TOOLCHAIN:-0}" == "1" ]]; then
  EMBED_TOOLCHAIN=1
fi

usage() {
  sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
  exit 2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage ;;
    --build-dir) BUILD_DIR="${2:?}"; shift 2 ;;
    --artifact) ARTIFACT="${2:?}"; shift 2 ;;
    --zip-prefix) ZIP_PREFIX="${2:?}"; shift 2 ;;
    --exe-name) EXE_NAME="${2:?}"; shift 2 ;;
    --display-name) DISPLAY_NAME="${2:?}"; shift 2 ;;
    --recompiler-build) RECOMPILER_BUILD="${2:?}"; shift 2 ;;
    --version-env) VERSION_ENV="${2:?}"; shift 2 ;;
    --disc-hint) DISC_HINT="${2:?}"; shift 2 ;;
    --project-file) PROJECT_FILES+=("${2:?}"); shift 2 ;;
    --project-dir) PROJECT_DIRS+=("${2:?}"); shift 2 ;;
    --runtime-bin) RUNTIME_BIN_DIR="${2:?}"; shift 2 ;;
    --root) ROOT="${2:?}"; shift 2 ;;
    --embed-toolchain) EMBED_TOOLCHAIN=1; shift ;;
    --no-embed-toolchain) EMBED_TOOLCHAIN=0; shift ;;
    *)
      echo "error: unknown arg: $1" >&2
      usage
      ;;
  esac
done

if [[ -z "${BUILD_DIR}" || -z "${ARTIFACT}" || -z "${ZIP_PREFIX}" || -z "${EXE_NAME}" ]]; then
  echo "error: --build-dir, --artifact, --zip-prefix, and --exe-name are required" >&2
  usage
fi
if [[ -z "${DISPLAY_NAME}" ]]; then
  DISPLAY_NAME="${EXE_NAME}"
fi

ROOT="$(cd "${ROOT}" && pwd)"

# Defaults for a typical title if caller passed none.
# Codegen host sources live in psxrecomp/host/ (copied with the framework tree).
if [[ ${#PROJECT_FILES[@]} -eq 0 && ${#PROJECT_DIRS[@]} -eq 0 ]]; then
  PROJECT_FILES=(CMakeLists.txt game.toml VERSION)
  for d in seeds launcher_assets; do
    if [[ -e "${ROOT}/${d}" ]]; then
      PROJECT_DIRS+=("${d}")
    fi
  done
  for f in codegen_setup.c codegen_setup.h DISC.md README.md; do
    if [[ -e "${ROOT}/${f}" ]]; then
      PROJECT_FILES+=("${f}")
    fi
  done
fi

validate_slug() {
  local label="$1" value="$2"
  if [[ -z "${value}" || "${value}" == "." || "${value}" == ".." ||
        "${value}" == *'.' || ! "${value}" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]]; then
    echo "error: ${label} must be a path-free slug: '${value}'" >&2
    exit 2
  fi
  local stem="${value%%.*}"
  stem="${stem,,}"
  case "${stem}" in
    aux|con|nul|prn|com[1-9]|lpt[1-9])
      echo "error: ${label} must not use a Windows device name: '${value}'" >&2
      exit 2
      ;;
  esac
}

validate_project_relpath() {
  local rel="$1" label="$2"
  if [[ -z "${rel}" || "${rel}" == /* || "${rel}" == \\* ||
        "${rel}" == *:* || "${rel}" == *\\* ]]; then
    echo "error: ${label} must be a project-relative path: '${rel}'" >&2
    exit 2
  fi
  local component component_stem
  local -a components
  IFS='/' read -r -a components <<<"${rel}"
  for component in "${components[@]}"; do
    if [[ -z "${component}" || "${component}" == "." || "${component}" == ".." ||
          "${component}" =~ [[:cntrl:]] || "${component}" == *[[:space:]] ||
          "${component}" == *'.' ]]; then
      echo "error: ${label} has an unsafe path component: '${rel}'" >&2
      exit 2
    fi
    component_stem="${component%%.*}"
    component_stem="${component_stem,,}"
    case "${component_stem}" in
      aux|con|nul|prn|com[1-9]|lpt[1-9])
        echo "error: ${label} has a Windows device path component: '${rel}'" >&2
        exit 2
        ;;
    esac
  done
}

validate_slug "--artifact" "${ARTIFACT}"
validate_slug "--zip-prefix" "${ZIP_PREFIX}"
for f in "${PROJECT_FILES[@]}"; do
  validate_project_relpath "${f}" "--project-file"
done
for d in "${PROJECT_DIRS[@]}"; do
  validate_project_relpath "${d}" "--project-dir"
done

ROOT_IS_GIT=0
if git -C "${ROOT}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  ROOT_TOP="$(git -C "${ROOT}" rev-parse --show-toplevel)"
  ROOT_TOP="$(cd "${ROOT_TOP}" && pwd)"
  if [[ "${ROOT_TOP}" != "${ROOT}" ]]; then
    echo "error: --root must be the Git worktree root: ${ROOT}" >&2
    exit 1
  fi
  ROOT_IS_GIT=1
elif [[ -e "${ROOT}/.git" || -L "${ROOT}/.git" ]]; then
  echo "error: cannot inspect Git metadata for project root ${ROOT}" >&2
  exit 1
fi

if [[ "${ROOT_IS_GIT}" -eq 1 ]]; then
  PROJECT_PATHS=("${PROJECT_FILES[@]}" "${PROJECT_DIRS[@]}")
  CLEAN_PROJECT_PATHS=("${PROJECT_DIRS[@]}")
  for f in "${PROJECT_FILES[@]}"; do
    # Release jobs intentionally stamp VERSION before the build. Its content is
    # checked against the baked binary stamp below; every other input is clean.
    if [[ "${f}" != "VERSION" ]]; then
      CLEAN_PROJECT_PATHS+=("${f}")
    fi
  done
  if [[ ${#CLEAN_PROJECT_PATHS[@]} -gt 0 ]]; then
    if ! git -C "${ROOT}" diff --quiet -- "${CLEAN_PROJECT_PATHS[@]}" ||
       ! git -C "${ROOT}" diff --cached --quiet -- "${CLEAN_PROJECT_PATHS[@]}"; then
      echo "error: refusing to package dirty or untracked title rebuild inputs" >&2
      exit 1
    fi
  fi
  if [[ ${#PROJECT_PATHS[@]} -gt 0 ]]; then
    PROJECT_UNTRACKED="$(git -C "${ROOT}" ls-files --others --exclude-standard -- "${PROJECT_PATHS[@]}")"
    if [[ -n "${PROJECT_UNTRACKED}" ]]; then
      echo "error: refusing to package dirty or untracked title rebuild inputs" >&2
      printf '%s\n' "${PROJECT_UNTRACKED}" >&2
      exit 1
    fi
  fi
  for f in "${PROJECT_FILES[@]}"; do
    if [[ ! -f "${ROOT}/${f}" && ! -L "${ROOT}/${f}" ]]; then
      echo "error: title rebuild file is missing or is not a file: ${f}" >&2
      exit 1
    elif ! git -C "${ROOT}" ls-files --error-unmatch -- "${f}" >/dev/null 2>&1; then
      echo "error: title rebuild file is not tracked: ${f}" >&2
      exit 1
    fi
  done
  for d in "${PROJECT_DIRS[@]}"; do
    if [[ ! -d "${ROOT}/${d}" ]]; then
      echo "error: title rebuild directory is missing or is not a directory: ${d}" >&2
      exit 1
    fi
    TRACKED_IN_DIR="$(git -C "${ROOT}" ls-files -- "${d}")"
    if [[ -z "${TRACKED_IN_DIR}" ]]; then
      echo "error: title rebuild directory has no tracked files: ${d}" >&2
      exit 1
    fi
  done
fi

if [[ -d "${ROOT}/${BUILD_DIR}" ]]; then
  BUILD_DIR="$(cd "${ROOT}/${BUILD_DIR}" && pwd)"
elif [[ -d "${BUILD_DIR}" ]]; then
  BUILD_DIR="$(cd "${BUILD_DIR}" && pwd)"
else
  echo "error: build dir not found: ${BUILD_DIR}" >&2
  exit 1
fi

# Resolve *requested* version from env / VERSION file (may be empty until stamp).
REQUESTED=""
if [[ -n "${VERSION_ENV}" ]]; then
  REQUESTED="${!VERSION_ENV:-}"
fi
if [[ -z "${REQUESTED}" && -n "${RELEASE_VERSION:-}" ]]; then
  REQUESTED="${RELEASE_VERSION}"
fi
if [[ -z "${REQUESTED}" && -n "${BPE_RELEASE_VERSION:-}" ]]; then
  REQUESTED="${BPE_RELEASE_VERSION}"
fi
if [[ -z "${REQUESTED}" && -f "${ROOT}/VERSION" ]]; then
  REQUESTED="$(tr -d '[:space:]' <"${ROOT}/VERSION")"
fi
REQUESTED="$(printf '%s' "${REQUESTED}" | tr -d '[:space:]')"
REQUESTED="${REQUESTED#v}"

DIST="${ROOT}/dist"
STAGE="${DIST}/stage-setup-${ARTIFACT}"

EXE=""
for cand in \
  "${BUILD_DIR}/${EXE_NAME}" \
  "${BUILD_DIR}/${EXE_NAME}.exe" \
  "${BUILD_DIR}/Release/${EXE_NAME}.exe"
do
  if [[ -f "${cand}" ]]; then
    EXE="${cand}"
    break
  fi
done
if [[ -z "${EXE}" ]]; then
  echo "error: setup host executable '${EXE_NAME}' not found under ${BUILD_DIR}" >&2
  ls -la "${BUILD_DIR}" >&2 || true
  exit 1
fi

# Authoritative lobby pin = stamp written at compile time next to the exe
# (see runtime.cmake file(GENERATE) psx_game_version.txt). Never invent a
# newer VERSION for the zip than what was baked into PSX_GAME_VERSION.
normalize_ver() {
  local v
  v="$(printf '%s' "${1:-}" | tr -d '[:space:]')"
  v="${v#v}"
  printf '%s' "${v}"
}
BUILT=""
for stamp in \
  "$(dirname "${EXE}")/psx_game_version.txt" \
  "${BUILD_DIR}/psx_game_version.txt" \
  "${BUILD_DIR}/Release/psx_game_version.txt"
do
  if [[ -f "${stamp}" ]]; then
    BUILT="$(normalize_ver "$(cat "${stamp}")")"
    echo "lobby pin stamp: ${stamp} -> ${BUILT}"
    break
  fi
done
if [[ -z "${BUILT}" ]]; then
  echo "error: missing psx_game_version.txt next to ${EXE}" >&2
  echo "  Rebuild with current psxrecomp runtime.cmake so the lobby pin is stamped." >&2
  echo "  Refusing to package (VERSION file alone can drift from PSX_GAME_VERSION)." >&2
  exit 1
fi
if [[ -n "${REQUESTED}" && "${REQUESTED}" != "${BUILT}" ]]; then
  echo "error: RELEASE_VERSION/VERSION=${REQUESTED} but binary stamp=${BUILT}" >&2
  echo "  Rebuild with -DPSX_GAME_VERSION=${REQUESTED} (or pin VERSION then reconfigure)," >&2
  echo "  then re-run this packager. Shipping mismatched pins breaks netplay lobby lists." >&2
  exit 1
fi
VERSION="${BUILT}"
validate_slug "release version" "${VERSION}"
for f in "${PROJECT_FILES[@]}"; do
  if [[ "${f}" == "VERSION" ]]; then
    SOURCE_VERSION="$(normalize_ver "$(cat "${ROOT}/VERSION")")"
    if [[ "${SOURCE_VERSION}" != "${VERSION}" ]]; then
      echo "error: source VERSION=${SOURCE_VERSION} but binary stamp=${VERSION}" >&2
      exit 1
    fi
  fi
done
ZIP_NAME="${ZIP_PREFIX}-${VERSION}-${ARTIFACT}.zip"
rm -rf "${STAGE}"
mkdir -p "${STAGE}" "${DIST}"
rm -f "${DIST}/${ZIP_NAME}"

cp -a "${EXE}" "${STAGE}/"
# Ship the stamp beside the exe so installers / RetComM can prefer it over VERSION.
if [[ -f "$(dirname "${EXE}")/psx_game_version.txt" ]]; then
  cp -a "$(dirname "${EXE}")/psx_game_version.txt" "${STAGE}/psx_game_version.txt"
else
  printf '%s\n' "${VERSION}" >"${STAGE}/psx_game_version.txt"
fi
EXE_BASENAME="$(basename "${EXE}")"
EXE_DIR="$(dirname "${EXE}")"

# Windows: CMake may already have placed imported runtime DLLs (zlib1.dll)
# next to the host. Copy siblings into the stage before MinGW bundling —
# this packager used to copy only the .exe, so a missed PE-import walk
# shipped hosts that die on clean machines with "zlib1.dll was not found".
if [[ "${EXE_BASENAME}" == *.exe ]]; then
  shopt -s nullglob
  for dll in "${EXE_DIR}"/*.dll "${EXE_DIR}"/*.DLL; do
    [[ -f "${dll}" ]] || continue
    cp -a "${dll}" "${STAGE}/"
    echo "staged sibling DLL $(basename "${dll}")"
  done
  shopt -u nullglob
fi

if [[ ! -d "${EXE_DIR}/assets/fonts" || ! -d "${EXE_DIR}/assets/img" ]]; then
  echo "error: ${EXE_DIR}/assets/{fonts,img} missing — rebuild psx-runtime" >&2
  exit 1
fi
mkdir -p "${STAGE}/assets"
cp -a "${EXE_DIR}/assets/fonts" "${STAGE}/assets/"
cp -a "${EXE_DIR}/assets/img" "${STAGE}/assets/"
if [[ ! -f "${STAGE}/assets/img/boxart.tga" && -f "${ROOT}/launcher_assets/img/boxart.tga" ]]; then
  cp -a "${ROOT}/launcher_assets/img/boxart.tga" "${STAGE}/assets/img/boxart.tga"
fi

# Product runtime resources are generated/staged beside the executable by
# CMake. They are intentionally copied from the build output, not from the
# repository's development tree: the latter can contain retired packages and
# other authoring leftovers. Never copy settings.toml, bios.cfg, disc.cfg,
# saves, or other machine/user state into a portable release.
if [[ -d "${EXE_DIR}/bezels" ]]; then
  cp -a "${EXE_DIR}/bezels" "${STAGE}/"
fi
if [[ -d "${EXE_DIR}/mods/packages" ]]; then
  mkdir -p "${STAGE}/mods"
  if [[ -f "${EXE_DIR}/mods/state.toml" ]]; then
    mkdir -p "${STAGE}/mods/packages"
    # The build directory can retain disabled catalog entries from an older
    # configure. The portable release should contain only packages named by
    # the authoritative state file, so the launcher cannot show stale mods.
    while IFS= read -r package_id; do
      [[ -n "${package_id}" && -d "${EXE_DIR}/mods/packages/${package_id}" ]] || continue
      cp -a "${EXE_DIR}/mods/packages/${package_id}" "${STAGE}/mods/packages/"
    done < <(awk -F'"' '/^\[\[package\]\]/{in_pkg=1; next} in_pkg && /^id[[:space:]]*=/ {print $2; in_pkg=0}' "${EXE_DIR}/mods/state.toml")
    cp -a "${EXE_DIR}/mods/state.toml" "${STAGE}/mods/"
  else
    cp -a "${EXE_DIR}/mods/packages" "${STAGE}/mods/"
  fi
  if [[ -f "${EXE_DIR}/mods/README.md" ]]; then
    cp -a "${EXE_DIR}/mods/README.md" "${STAGE}/mods/"
  fi
fi
if [[ -f "${EXE_DIR}/game_options.toml" ]]; then
  cp -a "${EXE_DIR}/game_options.toml" "${STAGE}/"
fi

copy_proj() {
  local rel="$1"
  if [[ -e "${ROOT}/${rel}" ]]; then
    mkdir -p "$(dirname "${STAGE}/${rel}")"
    cp -a "${ROOT}/${rel}" "${STAGE}/${rel}"
  else
    echo "error: missing ${rel}" >&2
    exit 1
  fi
}

if [[ "${ROOT_IS_GIT}" -eq 1 ]]; then
  TRACKED_PROJECT_INPUTS="$(mktemp)"
  git -C "${ROOT}" ls-files -z -- "${PROJECT_FILES[@]}" "${PROJECT_DIRS[@]}" \
    >"${TRACKED_PROJECT_INPUTS}"
  while IFS= read -r -d '' rel; do
    if [[ ! -e "${ROOT}/${rel}" && ! -L "${ROOT}/${rel}" ]]; then
      echo "error: tracked title rebuild input disappeared: ${rel}" >&2
      rm -f "${TRACKED_PROJECT_INPUTS}"
      exit 1
    fi
    mkdir -p "$(dirname "${STAGE}/${rel}")"
    cp -a "${ROOT}/${rel}" "${STAGE}/${rel}"
  done <"${TRACKED_PROJECT_INPUTS}"
  rm -f "${TRACKED_PROJECT_INPUTS}"
else
  for f in "${PROJECT_FILES[@]}"; do
    copy_proj "${f}"
  done
  for d in "${PROJECT_DIRS[@]}"; do
    copy_proj "${d}"
  done
fi

copy_tree_filtered() {
  local src="$1" dest="$2"
  shift 2
  mkdir -p "${dest}"
  if git -C "${src}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    if ! git -C "${src}" diff --quiet --ignore-submodules=none -- ||
       ! git -C "${src}" diff --cached --quiet --ignore-submodules=none --; then
      echo "error: refusing to package dirty tracked framework tree ${src}" >&2
      exit 1
    fi
    local tracked
    tracked="$(mktemp)"
    git -C "${src}" ls-files -z --recurse-submodules >"${tracked}"
    if command -v rsync >/dev/null 2>&1; then
      rsync -a --from0 --files-from="${tracked}" "${src}/" "${dest}/"
    else
      local rel
      while IFS= read -r -d '' rel; do
        if [[ ! -e "${src}/${rel}" && ! -L "${src}/${rel}" ]]; then
          echo "error: tracked framework input disappeared: ${src}/${rel}" >&2
          rm -f "${tracked}"
          exit 1
        fi
        mkdir -p "$(dirname "${dest}/${rel}")"
        cp -a "${src}/${rel}" "${dest}/${rel}"
      done <"${tracked}"
    fi
    rm -f "${tracked}"
    return
  fi
  if [[ -e "${src}/.git" || -L "${src}/.git" ]]; then
    echo "error: cannot inspect Git metadata for framework tree ${src}" >&2
    exit 1
  fi
  if command -v rsync >/dev/null 2>&1; then
    rsync -a "$@" "${src}/" "${dest}/"
  else
    cp -a "${src}/." "${dest}/"
    rm -rf "${dest}/.git" "${dest}/recompiler/build" "${dest}/generated" 2>/dev/null || true
    find "${dest}" -type d -name '__pycache__' -prune -exec rm -rf {} + 2>/dev/null || true
    find "${dest}" -type d \( -name 'build' -o -name 'build-*' \) -prune -exec rm -rf {} + 2>/dev/null || true
  fi
}

if [[ ! -d "${ROOT}/psxrecomp" ]]; then
  echo "error: ${ROOT}/psxrecomp missing (expected framework submodule)" >&2
  exit 1
fi
if [[ ! -d "${ROOT}/recomp-ui" ]]; then
  echo "error: ${ROOT}/recomp-ui missing (expected UI submodule)" >&2
  exit 1
fi

copy_tree_filtered "${ROOT}/psxrecomp" "${STAGE}/psxrecomp" \
  --exclude '.git' \
  --exclude '/recompiler/build' \
  --exclude '/generated' \
  --exclude '__pycache__' \
  --exclude 'build' \
  --exclude 'build-*'

copy_tree_filtered "${ROOT}/recomp-ui" "${STAGE}/recomp-ui" \
  --exclude '.git' \
  --exclude 'build' \
  --exclude '__pycache__'

# Never ship game generated C or common disc working trees.
rm -rf "${STAGE}/generated" "${STAGE}/bpe" "${STAGE}/motk" "${STAGE}/disc"

STAGE_SDK="${SCRIPT_DIR}/stage_setup_sdk.sh"
if [[ ! -f "${STAGE_SDK}" ]]; then
  echo "error: missing ${STAGE_SDK}" >&2
  exit 1
fi
chmod +x "${STAGE_SDK}" 2>/dev/null || true

stage_args=(
  --stage "${STAGE}"
  --framework "${ROOT}/psxrecomp"
  --search-dir "${EXE_DIR}"
  --search-dir "${BUILD_DIR}"
  --runtime-bin "${RUNTIME_BIN_DIR}"
  --host-exe "${STAGE}/${EXE_BASENAME}"
  --recompiler-build "${RECOMPILER_BUILD}"
)
if [[ "${EMBED_TOOLCHAIN}" -eq 1 ]]; then
  if [[ -z "${PSXRECOMP_TOOLCHAIN_DIR:-${TOOLCHAIN_DIR:-${BPE_TOOLCHAIN_DIR:-}}}" ]]; then
    echo "error: --embed-toolchain requires PSXRECOMP_TOOLCHAIN_DIR (or TOOLCHAIN_DIR)" >&2
    exit 1
  fi
  stage_args+=(--toolchain-dir "${PSXRECOMP_TOOLCHAIN_DIR:-${TOOLCHAIN_DIR:-${BPE_TOOLCHAIN_DIR}}}")
else
  stage_args+=(--allow-no-toolchain)
fi

bash "${STAGE_SDK}" "${stage_args[@]}"

cat >"${STAGE}/README-SETUP.txt" <<EOF
${DISPLAY_NAME} ${VERSION} — setup package
Platform: ${ARTIFACT}

One zip for first install and updates. Does NOT include disc images, retail
BIOS dumps, pre-generated game C, or a portable cmake/clang pack. Emitters
(psxrecomp-game / psxrecomp-bios) and the CLI are inside psxrecomp/.

Standalone:
1. Install Python 3.
2. Run ${EXE_BASENAME}.
3. Provide ${DISC_HINT} (and optional retail SCPH-1001 BIOS; otherwise
   OpenBIOS is regenerated locally).
4. Follow the Generate & rebuild wizard. On first rebuild the host downloads
   cmake-clang-v1 from TechnicallyComputers/retcomm-toolchains (or you can
   pick a local cmake-clang-v1-*.zip for offline builds). System cmake/ninja
   also works if already on PATH.

RetComM uses this same zip: it harvests emitters into a shared SDK cache,
downloads the toolchain pack (or uses RETCOMM_TOOLCHAIN_DIR), and preserves
saves/user config across updates.
EOF

find "${STAGE}" -exec touch -c {} + 2>/dev/null || find "${STAGE}" -exec touch {} +

(
  cd "${STAGE}"
  if command -v zip >/dev/null 2>&1; then
    zip -r -q "${DIST}/${ZIP_NAME}" .
  elif command -v tar >/dev/null 2>&1; then
    # Windows installations commonly have bsdtar but no zip.exe.  Its
    # archive-format auto-detection keeps the published artifact a normal
    # .zip without requiring an installer or an extra SDK package.
    tar -a -c -f "${DIST}/${ZIP_NAME}" .
  else
    echo "error: neither zip nor tar is available" >&2
    exit 1
  fi
)

echo "Wrote ${DIST}/${ZIP_NAME}"
du -h "${DIST}/${ZIP_NAME}"
