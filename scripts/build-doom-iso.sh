#!/usr/bin/env bash
#
# build-doom-iso.sh -- bootstrap Doom (id's DEICE installer) for the GH #18 DOS/4GW
# acceptance test. The shareware set ships as DEICE.EXE + DOOMS_19.1/.2 (a two-stage
# DOS installer) which can only be unpacked by running it -- and it must run under XP's
# STOCK ntvdm, not our host, so extraction and execution are two separate steps:
#
#   1. D:\doominstall.bat  -- REMOVES our IFEO redirect, copies the installer to
#      C:\DINST, and runs INSTALL.BAT so XP's real ntvdm extracts Doom to C:\DOOMS
#      (per DOOMS_19.DAT: PATH=\DOOMS). Follow SETUP's prompts; choose *No Sound* for
#      the first bring-up (we have no SB16/OPL yet -- epics #20/#21). One-time.
#
#   2. D:\doomrun.bat  -- copies our host into C:\DOOMS, points the IFEO Debugger at it,
#      and launches DOOM.EXE through it (CWD = C:\DOOMS so DOOM.WAD resolves). This is
#      the actual test: DOS/4GW detects our DPMI host, switches to 32-bit PM, loads the
#      LE image. Watch C:\DOOMS\ntvdmhost.log + vm/serial.log for where it stops.
#
# Usage:  ./scripts/build-doom-iso.sh
# Output: /tmp/ntvdmex-doom.iso  (+ the qmp hot-swap command)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
DT="$ROOT/tools/dostest"
GAMEDIR="$ROOT/games/doom"
ISO="/tmp/ntvdmex-doom.iso"
STAGE="/tmp/ntvdmex_doom_cd"
LABEL="NTVDM$(date +%H%M%S)"
EXE="${1:-DOOM.EXE}"          # extracted program name (override if your set differs)
ARGS="${2:--nosound}"         # first-run args; -nosound skips the SB probe

for f in DEICE.EXE DOOMS_19.1 DOOMS_19.2 INSTALL.BAT; do
    [[ -e "$GAMEDIR/$f" ]] || { echo "error: $GAMEDIR/$f missing" >&2; exit 2; }
done

cmake --build "$BUILD" --target ntvdmhost

rm -rf "$STAGE"; mkdir -p "$STAGE/inst"
cp "$GAMEDIR"/* "$STAGE/inst/"
cp "$BUILD/ntvdmhost.exe" "$STAGE/"
cp "$BUILD/dosstub.com"   "$STAGE/"

IFEO='HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe'

# step 1: extract under STOCK ntvdm (IFEO redirect removed)
{
    echo '@echo off'
    echo 'setlocal'
    echo "reg delete \"$IFEO\" /v Debugger /f >nul 2>&1"
    echo 'if exist C:\DINST rd /s /q C:\DINST'
    echo 'md C:\DINST'
    echo 'xcopy /e /i /y "%~dp0inst" C:\DINST >nul'
    echo 'cd /d C:\DINST'
    echo 'echo === Doom will extract to C:\DOOMS -- follow SETUP (choose No Sound) ==='
    echo 'call INSTALL.BAT'
    echo 'endlocal'
} > "$STAGE/doominstall.bat"

# step 2: run the extracted DOOM.EXE through OUR host
{
    echo '@echo off'
    echo 'setlocal'
    echo 'set G=C:\DOOMS'
    echo 'set N=C:\ntvdmex'
    echo 'if not exist %N% md %N%'
    echo 'if not exist %G%\'"$EXE"' echo NOT FOUND: %G%\'"$EXE"' -- run doominstall.bat first & pause & goto :eof'
    echo 'taskkill /f /im ntvdmhost.exe >nul 2>&1'
    echo 'tskill ntvdmhost >nul 2>&1'
    echo 'copy /y "%~dp0ntvdmhost.exe" %G%\ >nul'
    echo 'copy /y "%~dp0dosstub.com" %G%\ >nul'
    echo "reg add \"$IFEO\" /v Debugger /t REG_SZ /d \"%G%\\ntvdmhost.exe\" /f >nul"
    echo "echo %G%\\$EXE $ARGS> %N%\\target.txt"
    echo 'cd /d %G%'
    echo 'start /wait /d "%G%" "" "%G%\dosstub.com"'
    echo 'endlocal'
} > "$STAGE/doomrun.bat"

rm -f "$ISO"
hdiutil makehybrid -iso -joliet -default-volume-name "$LABEL" -o "$ISO" "$STAGE" >/dev/null

echo "Built $ISO   (volume label $LABEL)"
echo "Staged:"; ls -R "$STAGE" | sed 's/^/  /'
echo
echo "Mount in the running VM:  python3 $ROOT/scripts/qmp.py cd $ISO"
echo "  1) D:\\doominstall.bat   (one-time: extract Doom to C:\\DOOMS under stock ntvdm; pick No Sound)"
echo "  2) D:\\doomrun.bat       (run DOOM.EXE through our host -- the DOS/4GW test)"
echo "Then read  C:\\DOOMS\\ntvdmhost.log  +  vm/serial.log  for the stop point."
