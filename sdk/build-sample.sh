#!/usr/bin/env bash
# Build the SDK's sample VDD as an XP-compatible 32-bit DLL. (GH #11)
#
# Deliberately ONE compiler line with no project headers on the include path
# beyond sdk/include: if this ever needs something from src/, the SDK header has
# stopped being self-contained and the ABI check in sdk/sample/abi_check.c is the
# thing to look at, not this script.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC="${CC:-i686-w64-mingw32-gcc}"
OUT="$ROOT/build/portecho.dll"
mkdir -p "$ROOT/build"
"$CC" -std=c99 -Wall -Wextra -O2 -shared \
  -I "$ROOT/sdk/include" \
  -o "$OUT" "$ROOT/sdk/sample/portecho.c" \
  -nostdlib -Wl,--entry,0 -lkernel32
echo "Built: $OUT"
