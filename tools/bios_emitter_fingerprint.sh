#!/usr/bin/env bash
# bios_emitter_fingerprint.sh — print a single content hash of every source that
# affects the BIOS generated C (the psxrecomp-bios emitter + the cycle model it
# bakes in + the seeds). regen_bios.sh writes this into generated/SCPH1001.emitter.sha
# after a regen; runtime/runtime.cmake recomputes it at configure time and warns if
# it differs — i.e. "you changed the emitter but didn't regenerate the BIOS".
#
# Keep this file list in sync with what actually feeds psxrecomp-bios --emit-full.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

FILES=(
    recompiler/src/full_function_emitter.cpp
    recompiler/src/full_function_emitter.h
    recompiler/src/strict_translator.cpp
    recompiler/src/main_bios.cpp
    recompiler/src/function_discovery.cpp
    recompiler/src/control_flow.cpp
    recompiler/src/function_analysis.cpp
    recompiler/src/mips_decoder.cpp
    recompiler/src/bios_slice_walker.cpp
    recompiler/src/basic_block.cpp
    runtime/include/psx_cyc.h
    runtime/include/psx_instr_cost.h
    recompiler/seeds/phase2_ghidra_seeds.json
)

# Hash only files that exist (list may evolve). sha256sum is in Git Bash/coreutils.
present=()
for f in "${FILES[@]}"; do [ -f "$f" ] && present+=("$f"); done
# Stable order, single combined digest.
LC_ALL=C sort <<<"$(printf '%s\n' "${present[@]}")" | while IFS= read -r f; do
    sha256sum "$f"
done | sha256sum | awk '{print $1}'
