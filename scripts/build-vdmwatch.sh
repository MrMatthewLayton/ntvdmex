#!/usr/bin/env bash
# Build vdmwatch.exe -- attach-and-log debugger for the XP test box: sees the
# exception the kernel could not deliver (and the exit code) when a VDM is killed
# outright. Same XP-safe recipe as rigshot: no-CRT (src/runtime.c entry + mem
# primitives), subsystem 5.01, imports only KERNEL32. Console subsystem so the
# lines are visible live in the window the user leaves open.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/build/vdmwatch.exe}"
# -ffreestanding -fno-builtin: without them GCC turns the hand-rolled slen/sput
# loops into strlen/strcpy calls that do not exist in a -nostdlib link.
i686-w64-mingw32-gcc -O2 -nostdlib -nostartfiles -Wall -Wextra \
  -ffreestanding -fno-builtin \
  -o "$OUT" "$ROOT/scripts/bm/vdmwatch.c" "$ROOT/src/runtime.c" \
  -Wl,--subsystem,console -Wl,--entry,_WinMainCRTStartup \
  -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,1 \
  -Wl,--major-os-version,5 -Wl,--minor-os-version,1 \
  -lkernel32 -lgcc
echo "Built: $OUT"
