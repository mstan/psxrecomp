#!/bin/bash
# Build the psx-dsoracle target (MSVC, links DuckStation oracle).
#
# Prerequisites:
#   - DuckStation built via: bash tools/duckstation/build.sh
#   - MSVC 2022 installed (detected via vswhere)
#   - CMake in PATH
#
# Usage:
#   bash tools/build_oracle.sh
#   bash tools/build_oracle.sh clean    # clean + rebuild

set -e
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

echo "=== psxrecomp-v4: build oracle ==="

# Check DuckStation is built.
if [ ! -f "duckstation/build/src/core/Release/core.lib" ]; then
    echo "ERROR: DuckStation not built. Run: bash tools/duckstation/build.sh"
    exit 1
fi

# Download SDL2 MSVC dev package if not present.
if [ ! -d sdl2-msvc/SDL2-* ] 2>/dev/null; then
    echo "Downloading SDL2 MSVC development package..."
    curl -L -o sdl2-dev.zip \
        "https://github.com/libsdl-org/SDL/releases/download/release-2.30.12/SDL2-devel-2.30.12-VC.zip"
    mkdir -p sdl2-msvc
    unzip -q -o sdl2-dev.zip -d sdl2-msvc/
    rm -f sdl2-dev.zip
fi

# Configure with MSVC generator.
BUILD_DIR="runtime/build-msvc"

if [ "$1" = "clean" ]; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

if [ ! -d "$BUILD_DIR" ]; then
    echo "Configuring CMake (MSVC x64)..."
    cmake -S runtime -B "$BUILD_DIR" \
        -G "Visual Studio 17 2022" -A x64
fi

# Build psx-dsoracle target.
echo "Building psx-dsoracle..."
cmake --build "$BUILD_DIR" --target psx-dsoracle --config Release -- /m

echo ""
echo "=== Build complete ==="
echo "Binary: $BUILD_DIR/Release/psx-dsoracle.exe"

# Copy SDL2.dll if not already there.
if [ -f "$BUILD_DIR/Release/psx-dsoracle.exe" ]; then
    echo "Oracle binary ready."
fi
