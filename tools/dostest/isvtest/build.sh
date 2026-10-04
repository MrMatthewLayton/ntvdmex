#!/usr/bin/env bash
# build.sh -- ISVTEST.DLL (an MS-ABI VDD) and p_isv.com (its DOS half). s91, #11.
set -euo pipefail
D="$(cd "$(dirname "$0")" && pwd)"
i686-w64-mingw32-dlltool -k -d "$D/ntvdm.def" -l "$D/libntvdm.a"
i686-w64-mingw32-gcc -O2 -shared -nostdlib -Wall -o "$D/ISVTEST.DLL" "$D/isvtest.c" \
    -L"$D" -lntvdm -lkernel32 -Wl,--entry,_DllMainCRTStartup@12 -Wl,--enable-stdcall-fixup
i686-w64-mingw32-objdump -p "$D/ISVTEST.DLL" | grep -A14 "DLL Name: NTVDM.EXE" | head -16
