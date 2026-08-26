#!/usr/bin/env bash
# Build rigshot.exe -- desktop capture + remote window poking for the XP test box.
# Same XP-safe recipe as controld: no-CRT (reuses src/runtime.c entry + mem
# primitives), subsystem 5.01, imports only KERNEL32/USER32/GDI32 (NO UCRT stubs,
# which are absent on XP).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/build/rigshot.exe}"
# ⚠ -ffreestanding -fno-builtin are load-bearing, not tidiness. Without them GCC
#   recognises the hand-rolled `slen` loop as strlen and emits a CALL to strlen --
#   which does not exist in a -nostdlib link. Same trap CMakeLists.txt documents for
#   src/runtime.c's memset.
i686-w64-mingw32-gcc -O2 -nostdlib -nostartfiles -Wall -Wextra \
  -ffreestanding -fno-builtin \
  -o "$OUT" "$ROOT/scripts/bm/rigshot.c" "$ROOT/src/runtime.c" \
  -Wl,--subsystem,windows -Wl,--entry,_WinMainCRTStartup \
  -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,1 \
  -Wl,--major-os-version,5 -Wl,--minor-os-version,1 \
  -lkernel32 -luser32 -lgdi32 -lgcc
echo "Built: $OUT"
