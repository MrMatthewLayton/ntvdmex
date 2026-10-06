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
# The ABI check FIRST, and it must compile: it includes the SDK header AND the
# in-tree src/vdd/ntvdd.h and fails the build if they have drifted apart. (It was
# built by tests/probes/dos/run.sh until #322 removed that script, after which
# nothing compiled it -- a guard nobody runs guards nothing.)
"$CC" -std=c99 -fsyntax-only -I "$ROOT/sdk/include" -I "$ROOT/src/vdd" -I "$ROOT/src" \
  "$ROOT/sdk/sample/abi_check.c"
echo "ABI check: the SDK header matches src/vdd/ntvdd.h"
"$CC" -std=c99 -Wall -Wextra -O2 -shared \
  -I "$ROOT/sdk/include" \
  -o "$OUT" "$ROOT/sdk/sample/portecho.c" \
  -nostdlib -Wl,--entry,0 -lkernel32
echo "Built: $OUT"
# The second sample (s91): claim_int + map_flat.
"$CC" -std=c99 -Wall -Wextra -O2 -shared \
  -I "$ROOT/sdk/include" \
  -o "$ROOT/build/intecho.dll" "$ROOT/sdk/sample/intecho.c" \
  -nostdlib -Wl,--entry,0 -lkernel32
echo "Built: $ROOT/build/intecho.dll"
