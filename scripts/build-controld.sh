#!/usr/bin/env bash
# Build controld.exe -- the XP bare-metal control daemon (remote kill/reboot).
# XP-safe: no-CRT (reuses src/runtime.c entry + mem primitives), subsystem 5.01,
# imports only KERNEL32/USER32/ADVAPI32 (NO api-ms-win-crt UCRT stubs).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/build/controld.exe}"
i686-w64-mingw32-gcc -O2 -nostdlib -nostartfiles \
  -o "$OUT" "$ROOT/scripts/bm/controld.c" "$ROOT/src/runtime.c" \
  -Wl,--subsystem,windows -Wl,--entry,_WinMainCRTStartup \
  -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,1 \
  -Wl,--major-os-version,5 -Wl,--minor-os-version,1 \
  -lkernel32 -luser32 -ladvapi32 -lgcc
echo "Built: $OUT"
