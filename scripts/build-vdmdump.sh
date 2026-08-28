#!/usr/bin/env bash
# Build vdmdump.exe -- read a live VDM's memory from outside it (GH #128).
# Same XP-safe recipe as rigshot/controld: no-CRT, subsystem 5.01, imports only
# KERNEL32 (+ntdll, resolved at runtime) so it loads on XP, which has no UCRT.
#
# ⚠ -ffreestanding -fno-builtin are load-bearing: without them GCC turns the
#   hand-rolled loops back into calls to memset/strlen, which do not exist in a
#   -nostdlib link. Console subsystem on purpose -- the .bat that drives this has
#   to WAIT for it (it runs between "IFEO removed" and "IFEO restored").
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/build/vdmdump.exe}"
mkdir -p "$(dirname "$OUT")"
i686-w64-mingw32-gcc -O2 -nostdlib -nostartfiles -Wall -Wextra \
  -ffreestanding -fno-builtin \
  -o "$OUT" "$ROOT/tools/vdmdump/vdmdump.c" \
  -Wl,--subsystem,console -Wl,--entry,_mainCRTStartup \
  -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,1 \
  -Wl,--major-os-version,5 -Wl,--minor-os-version,1 \
  -lkernel32 -lgcc
echo "Built: $OUT"
