#!/usr/bin/env bash
# Set up the DuckStation oracle build:
#   1. Ensure duckstation/ submodule is initialized and at the pinned upstream commit
#   2. Fetch + extract Windows prebuilt deps if missing
#   3. Rewrite stale absolute paths inside the extracted deps (artefact of how
#      upstream packages them — the prefix is baked in at pack time)
#   4. Apply the PSXRecomp oracle patch (psxrecomp_oracle.patch) if not applied
#
# After setup, run `tools/duckstation/build.sh` (or the commands in its tail) to
# compile duckstation-qt.exe into duckstation/build/bin/.
#
# Safe to run repeatedly — each step checks for "already done" and skips.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DUCK="$REPO_ROOT/duckstation"
PATCH="$REPO_ROOT/tools/duckstation/psxrecomp_oracle.patch"
# The pin lives in pin.json so this script and duckstation_oracle.py (the
# Linux/macOS path) can never drift onto different upstream bases.
UPSTREAM_BASE="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["upstream_base"])' "$REPO_ROOT/tools/duckstation/pin.json")"

log() { echo "[duckstation-setup] $*"; }

# ---- Step 1: submodule init + checkout upstream base ----
if [ ! -f "$DUCK/CMakeLists.txt" ]; then
    log "initializing duckstation submodule..."
    cd "$REPO_ROOT"
    git submodule update --init --recursive duckstation
fi

cd "$DUCK"
CUR_SHA="$(git rev-parse HEAD)"
if [ "$CUR_SHA" != "$UPSTREAM_BASE" ]; then
    log "checking out upstream base $UPSTREAM_BASE (was $CUR_SHA)..."
    git fetch origin "$UPSTREAM_BASE" 2>/dev/null || git fetch origin
    git checkout --detach "$UPSTREAM_BASE"
fi

# ---- Step 2: prebuilt deps ----
DEPS_ARCHIVE="$DUCK/dep/prebuilt/deps-windows-x64.7z"
DEPS_DIR="$DUCK/dep/prebuilt/windows-x64"
PREBUILT_SHA256="f3473f18504b8a2d3ae32f007a575839288380503584d7867135da894ac8c129"
PREBUILT_VERSION="$(cat "$DUCK/dep/PREBUILT-VERSION")"
PREBUILT_URL="https://github.com/duckstation/dependencies/releases/download/${PREBUILT_VERSION}/deps-windows-x64.7z"

if [ ! -f "$DEPS_ARCHIVE" ]; then
    log "downloading prebuilt deps ($PREBUILT_VERSION) from $PREBUILT_URL..."
    mkdir -p "$(dirname "$DEPS_ARCHIVE")"
    if command -v curl >/dev/null 2>&1; then
        curl -L --fail -o "$DEPS_ARCHIVE" "$PREBUILT_URL"
    elif command -v wget >/dev/null 2>&1; then
        wget -O "$DEPS_ARCHIVE" "$PREBUILT_URL"
    else
        log "ERROR: neither curl nor wget found — fetch $PREBUILT_URL manually and place at $DEPS_ARCHIVE"
        exit 1
    fi
fi

if [ -f "$DEPS_ARCHIVE" ]; then
    ACTUAL_SHA="$(sha256sum "$DEPS_ARCHIVE" | awk '{print $1}')"
    if [ "$ACTUAL_SHA" != "$PREBUILT_SHA256" ]; then
        log "ERROR: prebuilt archive sha256 mismatch"
        log "  expected: $PREBUILT_SHA256"
        log "  actual:   $ACTUAL_SHA"
        exit 1
    fi
fi

if [ ! -d "$DEPS_DIR" ] || [ -z "$(ls -A "$DEPS_DIR" 2>/dev/null || true)" ]; then
    log "extracting prebuilt deps..."
    cd "$DUCK/dep/prebuilt"
    if command -v 7z >/dev/null 2>&1; then
        7z x -y deps-windows-x64.7z >/dev/null
    elif [ -x "/c/Program Files/7-Zip/7z.exe" ]; then
        "/c/Program Files/7-Zip/7z.exe" x -y deps-windows-x64.7z >/dev/null
    else
        log "ERROR: 7z not found — extract $DEPS_ARCHIVE manually"
        exit 1
    fi
fi

# ---- Step 3: rewrite stale absolute paths in extracted CMake/pkg-config metadata ----
# Upstream bakes in whatever prefix the archive was built at. Replace with this
# checkout's real path. Idempotent: no-op once rewritten.
log "normalizing absolute paths in prebuilt deps metadata..."
python3 "$REPO_ROOT/tools/fix_duckstation_deps_paths.py" "$DEPS_DIR" >/dev/null

# ---- Step 4: apply PSXRecomp oracle patch ----
cd "$DUCK"
if git apply --check "$PATCH" >/dev/null 2>&1; then
    log "applying PSXRecomp oracle patch..."
    git apply "$PATCH"
else
    # Either already applied, or conflicts. Distinguish:
    if git apply --reverse --check "$PATCH" >/dev/null 2>&1; then
        log "oracle patch already applied"
    else
        log "ERROR: oracle patch does not apply cleanly and is not already applied."
        log "  - upstream commit may have moved past the pinned base"
        log "  - or the patch itself was regenerated against a different base"
        log "  patch: $PATCH"
        log "  duck HEAD: $(git rev-parse HEAD)"
        exit 1
    fi
fi

log "setup complete — run tools/duckstation/build.sh to compile"
