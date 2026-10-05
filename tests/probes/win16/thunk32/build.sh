#!/usr/bin/env bash
# build.sh -- W16THK.DLL, the 32-bit half of tests/probes/win16/w_wcb (s91, #309).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
mkdir -p "$ROOT/build/wintest"
i686-w64-mingw32-gcc -O2 -shared -nostdlib -Wall -Wextra -Wno-cast-function-type \
    -o "$ROOT/build/wintest/W16THK.DLL" "$ROOT/tests/probes/win16/thunk32/w16thk.c" \
    -Wl,--kill-at -Wl,--entry,_DllMainCRTStartup@12 -Wl,--enable-stdcall-fixup -lkernel32
echo "built $ROOT/build/wintest/W16THK.DLL"
