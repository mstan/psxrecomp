#!/usr/bin/env bash
# setup_recompiler.sh — Bootstrap the psxrecomp environment from scratch.
#
# Usage:
#   ./setup_recompiler.sh
#
# Does:
#   1. Checks for required system packages
#   2. Initializes git submodules (RmlUi, FreeType)
#   3. Builds the recompiler (psxrecomp-game + psxrecomp-bios)
#   4. Checks for bios/SCPH1001.BIN
#   5. Regenerates BIOS C code from your bios/SCPH1001.BIN
#
# After this completes, run ./setup_game.sh for each new game.

set -euo pipefail

cd "$(dirname "$0")"
REPO_ROOT="$PWD"

echo "========================================"
echo "  psxrecomp — Setup Recompiler"
echo "========================================"
echo ""

# ---- Step 1: Check prerequisites ----
echo "=== Step 1: Checking prerequisites ==="

MISSING=""
check_cmd() {
    if ! command -v "$1" &>/dev/null; then
        echo "  [MISSING] $1"
        MISSING="$MISSING $1"
    else
        echo "  [OK]      $1 ($(command -v "$1"))"
    fi
}

check_cmd cmake
check_cmd ninja
check_cmd pkg-config
check_cmd python3

# Check for C/C++ compiler
if command -v gcc &>/dev/null; then
    echo "  [OK]      gcc ($(gcc --version | head -1))"
elif command -v clang &>/dev/null; then
    echo "  [OK]      clang ($(clang --version | head -1))"
else
    echo "  [MISSING] C compiler (gcc or clang)"
    MISSING="$MISSING c-compiler"
fi

# Check for SDL2
if pkg-config --exists sdl2 2>/dev/null; then
    SDL2_VER=$(pkg-config --modversion sdl2)
    echo "  [OK]      SDL2 (v$SDL2_VER)"
else
    echo "  [MISSING] SDL2 development library"
    echo "            Install: sudo apt install libsdl2-dev"
    MISSING="$MISSING sdl2"
fi

if [ -n "$MISSING" ]; then
    echo ""
    echo "ERROR: Missing prerequisites:$MISSING"
    echo ""
    echo "On Debian/Ubuntu:"
    echo "  sudo apt install build-essential cmake ninja-build pkg-config libsdl2-dev python3"
    echo ""
    echo "On Arch:"
    echo "  sudo pacman -S base-devel cmake ninja pkgconf sdl2 python"
    echo ""
    echo "On macOS (Homebrew):"
    echo "  brew install cmake ninja pkg-config sdl2 python3"
    exit 1
fi
echo ""

# ---- Step 2: Init submodules ----
echo "=== Step 2: Initializing git submodules ==="
if [ -f ".gitmodules" ]; then
    git submodule update --init --recursive
    echo "  ✓ Submodules initialized"
else
    echo "  No .gitmodules found (not a git repo or no submodules configured)"
fi
echo ""

# ---- Step 3: Build the recompiler ----
echo "=== Step 3: Building the recompiler ==="
if [ -d "recompiler" ]; then
    cmake -S recompiler -B recompiler/build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build recompiler/build
    echo "  ✓ Recompiler built"
else
    echo "  WARNING: recompiler/ directory not found. Is this a psxrecomp repo?"
fi
echo ""

# ---- Step 4: Check for BIOS ----
echo "=== Step 4: Checking for BIOS ==="
if [ -f "bios/SCPH1001.BIN" ]; then
    BIOS_SIZE=$(stat -c%s "bios/SCPH1001.BIN" 2>/dev/null || stat -f%z "bios/SCPH1001.BIN" 2>/dev/null)
    echo "  ✓ Found bios/SCPH1001.BIN ($BIOS_SIZE bytes)"
else
    echo "  [MISSING] bios/SCPH1001.BIN"
    echo ""
    echo "  You need a Sony SCPH-1001 BIOS file. Place it at:"
    echo "    bios/SCPH1001.BIN"
    echo ""
    echo "  Without it, the runtime will prompt for a BIOS file at launch."
    echo "  The BIOS C code cannot be regenerated until the file is present."
fi
echo ""

# ---- Step 5: Regenerate BIOS C code ----
echo "=== Step 5: Regenerating BIOS C code ==="
if [ -f "bios/SCPH1001.BIN" ] && [ -x "recompiler/build/psxrecomp-bios" ] && [ -f "tools/regen_bios.sh" ]; then
    echo "  Running tools/regen_bios.sh ..."
    bash tools/regen_bios.sh 2>&1 | tail -3
    echo "  ✓ BIOS C code regenerated"
elif [ -f "generated/SCPH1001_full.c" ]; then
    echo "  ✓ BIOS C code already present at generated/SCPH1001_full.c"
    echo "    (To regenerate: bash tools/regen_bios.sh)"
else
    echo "  WARNING: Could not generate BIOS C code."
    echo "  If you have bios/SCPH1001.BIN and the recompiler, run:"
    echo "    bash tools/regen_bios.sh"
    echo ""
    echo "  Alternatively, generated/SCPH1001_full.c must exist from a prior build."
fi
echo ""

# ---- Done ----
echo "========================================"
echo "  Setup complete!"
echo "========================================"
echo ""
echo "  Next:"
echo "    Place a game disc (.bin/.cue) in a folder, then run:"
echo "      ./setup_game.sh \"Game Name\" path/to/game.bin path/to/game.cue"
echo ""
echo "  Or build and run an already-configured game:"
echo "    ./build_digimon.sh"
echo "    ./run_digimon.sh"
echo ""
