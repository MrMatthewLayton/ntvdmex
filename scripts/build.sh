#!/usr/bin/env bash
#
# Convenience wrapper: configure + build the XP-32 cross build.
# Equivalent to running the two cmake commands by hand.
#
#   ./scripts/build.sh           # configure (if needed) and build
#   ./scripts/build.sh clean     # remove the build directory first
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"

if [[ "${1:-}" == "clean" ]]; then
    rm -rf "$BUILD"
fi

cmake -S "$ROOT" -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-xp32-mingw.cmake" \
    -DCMAKE_BUILD_TYPE=Release

cmake --build "$BUILD" --parallel

echo
echo "Built: $BUILD/ntvdmex.exe"
echo "Copy it to a Windows XP SP3 (32-bit) machine or VM and run it."
