#!/usr/bin/env bash
#
# build-game-iso.sh -- stage a real DOS game + the NTVDMEX host onto a CD for the
# GH #18 acceptance tests (the "playable games" bar: Doom / Skyroads / ZAR ...).
#
# A game lives under the gitignored  games/<name>/  drop directory (licensed, large:
# never committed). This packs the host, the 16-bit trigger, that game's files, and a
# generated runner batch into a fresh ISO. The runner copies everything to C:\<name>
# so the host's process CWD is the game directory -- relative file opens (e.g. a WAD)
# then resolve there (dos_int21.c uses CreateFileA on the raw guest name), and the game
# can write its own config/saves (a CD is read-only).
#
# Usage:  ./scripts/build-game-iso.sh <name> <EXE> [extra args...]
#   <name>  subdirectory under games/  (e.g. doom)
#   <EXE>   the program to launch      (e.g. DOOM.EXE)  -- DOS/4GW games also need
#           their extender file (e.g. DOS4GW.EXE) present in the same games/<name>/ dir.
#   [args]  optional command-line tail passed to the game (e.g. -warp 1 1)
#
# Example:  ./scripts/build-game-iso.sh doom DOOM.EXE
# Output:   /tmp/ntvdmex-game.iso  (+ the qmp hot-swap command)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
DT="$ROOT/tools/dostest"
ISO="/tmp/ntvdmex-game.iso"
STAGE="/tmp/ntvdmex_game_cd"
LABEL="NTVDG$(date +%H%M%S)"

NAME="${1:-}"
EXE="${2:-}"
if [[ -z "$NAME" || -z "$EXE" ]]; then
    echo "usage: $0 <name-under-games/> <EXE> [game args...]" >&2
    exit 2
fi
shift 2 || true
GAMEARGS="$*"
GAMEDIR="$ROOT/games/$NAME"
if [[ ! -d "$GAMEDIR" ]]; then
    echo "error: $GAMEDIR does not exist -- drop the game files there first." >&2
    exit 2
fi
if [[ ! -e "$GAMEDIR/$EXE" ]]; then
    echo "error: $GAMEDIR/$EXE not found (case-sensitive)." >&2
    ls "$GAMEDIR" | sed 's/^/  /' >&2
    exit 2
fi

# always rebuild the host so a game run tests the current code
cmake --build "$BUILD" --target ntvdmhost

rm -rf "$STAGE"; mkdir -p "$STAGE/$NAME"
cp "$BUILD/ntvdmhost.exe" "$STAGE/$NAME/"
cp "$BUILD/dosstub.com"   "$STAGE/$NAME/"
cp -R "$GAMEDIR"/* "$STAGE/$NAME/"

# generated runner: copy the whole game dir C:\<name>, point the IFEO Debugger at our
# host, set the target override + CWD, then fire the 16-bit trigger.
RUNNER="$STAGE/${NAME}run.bat"
{
    echo '@echo off'
    echo 'setlocal'
    echo "set G=C:\\$NAME"
    echo 'set N=C:\ntvdmex'
    echo 'if not exist %N% md %N%'
    echo 'REM kill any host from a prior run -- it locks ntvdmhost.exe so xcopy silently keeps the OLD build'
    echo 'taskkill /f /im ntvdmhost.exe >nul 2>&1'
    echo 'tskill ntvdmhost >nul 2>&1'
    echo 'if exist %G% rd /s /q %G%'
    echo 'md %G%'
    echo "xcopy /e /i /y \"%~dp0$NAME\" %G% >nul"
    echo 'reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "%G%\ntvdmhost.exe" /f >nul'
    if [[ -n "$GAMEARGS" ]]; then
        echo "echo %G%\\$EXE $GAMEARGS> %N%\\target.txt"
    else
        echo "echo %G%\\$EXE> %N%\\target.txt"
    fi
    echo 'cd /d %G%'
    echo "start /wait /d \"%G%\" \"\" \"%G%\\dosstub.com\""
    echo 'endlocal'
} > "$RUNNER"

rm -f "$ISO"
hdiutil makehybrid -iso -joliet -default-volume-name "$LABEL" -o "$ISO" "$STAGE" >/dev/null

echo "Built $ISO   (volume label $LABEL)"
echo "Staged:"; ls -R "$STAGE" | sed 's/^/  /' | head -40
echo
echo "Mount it in the running VM with:"
echo "  python3 $ROOT/scripts/qmp.py cd $ISO"
echo "Then in the XP desktop run:  D:\\${NAME}run.bat"
echo "Watch the host log (C:\\$NAME\\ntvdmhost.log) + vm/serial.log for where DOS/4GW stops."
