#!/usr/bin/env bash
#
# check-imports.sh -- assert a built PE imports ONLY DLLs that ship with Windows
# XP SP3, so a stray modern API (esp. the UCRT/MSVCRT the mingw toolchain defaults
# to) can't silently break loading on XP. The M2.6 / build-toolchain requirement.
#
#   ./scripts/check-imports.sh build/ntvdmhost.exe build/ntvdmex.exe
#
# Exits nonzero if any import is outside the XP allowlist below.
#
set -euo pipefail
OBJDUMP="${OBJDUMP:-i686-w64-mingw32-objdump}"

# DLLs that ship with Windows XP SP3 and are legitimate to import. Extend
# deliberately (with the knowledge that the DLL exists on XP) -- never to wave
# through a CRT dependency.
ALLOW='kernel32 user32 gdi32 comctl32 advapi32 shell32 shlwapi ntdll winmm ws2_32 comdlg32 ole32 oleaut32 version winspool'

fail=0
for exe in "$@"; do
    [ -f "$exe" ] || { echo "MISSING: $exe" >&2; fail=1; continue; }
    dlls=$("$OBJDUMP" -p "$exe" | awk '/DLL Name:/ {print tolower($3)}' | sed 's/\.dll$//')
    bad=""
    for d in $dlls; do
        case " $ALLOW " in
            *" $d "*) : ;;
            *) bad="$bad $d" ;;
        esac
    done
    if [ -n "$bad" ]; then
        echo "FAIL  $exe -> non-XP imports:$bad"
        fail=1
    else
        echo "OK    $exe -> $(echo $dlls | tr '\n' ' ')"
    fi
done
[ "$fail" -eq 0 ] || { echo "import-allowlist check FAILED" >&2; exit 1; }
echo "import-allowlist check passed (all imports are XP-shipped DLLs)"
