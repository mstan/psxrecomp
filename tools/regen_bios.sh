#!/usr/bin/env bash
# regen_bios.sh — canonical, hygiene-safe regeneration of the BIOS generated C.
#
# WHY THIS EXISTS: generated/SCPH1001_*.c is gitignored build output. The
# recompiler (psxrecomp-bios) and the runtime are built by SEPARATE CMake trees,
# so editing the BIOS emitter (full_function_emitter.cpp, strict_translator.cpp,
# the cycle headers, …) does NOT automatically refresh generated/. If you forget
# to regenerate, the runtime links a STALE BIOS that no longer matches the emitter
# (this bit us: a stale 4439-entry BIOS vs the current 4406-entry output). This
# script makes regeneration one command AND records an emitter fingerprint so the
# CMake build can warn when generated/ has drifted (see runtime/runtime.cmake).
#
# Usage:
#   tools/regen_bios.sh                 # rebuild the emitter + regenerate the BIOS
#   PSXRECOMP_BIOS_BUILD=recompiler/build tools/regen_bios.sh   # custom build dir
#
# Run from the framework root (the dir containing recompiler/ and generated/).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BIOS="${PSXRECOMP_BIOS_ROM:-bios/SCPH1001.BIN}"
SEEDS="${PSXRECOMP_BIOS_SEEDS:-recompiler/seeds/phase2_ghidra_seeds.json}"
OUT="${PSXRECOMP_BIOS_OUT:-generated}"

# Locate the recompiler build dir (where psxrecomp-bios[.exe] lives or will be built).
BUILD="${PSXRECOMP_BIOS_BUILD:-}"
if [ -z "$BUILD" ]; then
    for d in recompiler/build-t2 recompiler/build recompiler/cmake-build*; do
        if [ -f "$d/CMakeCache.txt" ]; then BUILD="$d"; break; fi
    done
fi
[ -n "$BUILD" ] || { echo "regen_bios: no recompiler build dir found (set PSXRECOMP_BIOS_BUILD)"; exit 1; }

# 1. Build the emitter FRESH so the regen always reflects current source.
echo "regen_bios: building psxrecomp-bios in $BUILD"
cmake --build "$BUILD" --target psxrecomp-bios >/dev/null

EXE="$BUILD/psxrecomp-bios.exe"; [ -f "$EXE" ] || EXE="$BUILD/psxrecomp-bios"
[ -f "$EXE" ] || { echo "regen_bios: psxrecomp-bios not found in $BUILD after build"; exit 1; }

# 2. Regenerate the BIOS C.
echo "regen_bios: emit-full $BIOS -> $OUT (seeds: $SEEDS)"
mkdir -p "$OUT"
"$EXE" "$BIOS" "$OUT" --emit-full "$SEEDS"

# 3. Record the emitter fingerprint so the build can detect future drift. MUST
#    match the file list in runtime/runtime.cmake's staleness check.
"$ROOT/tools/bios_emitter_fingerprint.sh" > "$OUT/SCPH1001.emitter.sha"
echo "regen_bios: wrote fingerprint $OUT/SCPH1001.emitter.sha"
echo "regen_bios: done ($(grep -m1 -o 'Dispatch entries: [0-9]*' "$OUT/SCPH1001_dispatch.c" || echo '?'))"
