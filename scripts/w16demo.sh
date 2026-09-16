#!/usr/bin/env bash
#
# w16demo.sh -- lay demo/win16/ on the share out of guest/win16/ (the Windows 3.11
# applets expanded off the install disks in s45; guest/ is .gitignored, Microsoft's).
#
#   ./scripts/w16demo.sh            # write demo/win16/<app>/ for every app below
#   ./scripts/w16demo.sh --check    # report what differs, change nothing
#
# ONE FOLDER PER APP, named after the EXE, with ONLY what that app brings of its own.
# The module tables say which DLLs are app-private: PBRUSH.DLL (Paint) and RECORDER.DLL
# (Recorder). Everything else -- COMMDLG, SHELL, VER, LZEXPAND, OLECLI/OLESVR, DDEML,
# MMSYSTEM, TOOLHELP, WINNLS -- the guest loads from XP's OWN system32 (measured s49:
# same sizes as the 3.11 copies, different md5, and disassembling the wrong one cost
# part of a session). So the 3.11 krnl386/user/gdi/*.drv and shared DLLs in guest/win16
# are deliberately NOT shipped; a copy beside the EXE would be ignored at best.
#
# ⚠ WINFILE.EXE is left out on purpose: it is the Windows for Workgroups build and
#   statically imports SCONFIG.DLL, which is not on any box we have (s56 diagnosis:
#   "Cannot find file ... or one of its components"). An asset gap, not a code one.
# ⚠ No .HLP files: only *.EXE and *.DLL were copied back from the rig's expansion, so
#   Help > Contents in these apps will say the help file is missing. The install disks
#   would supply them (NOTEPAD.HL_ etc. + WINHELP.EX_).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/guest/win16"
SH=/private/tmp/xpshare
DST="$SH/demo/win16"
MODE=write; [ "${1:-}" = --check ] && MODE=check
[ -d "$SRC" ] || { echo "guest/win16 missing (it is .gitignored; fetch it off the rig)" >&2; exit 1; }
[ -d "$SH/cfg" ] || { echo "share not mounted at $SH" >&2; exit 2; }

# app folder : files it brings.  Status per tools/score/model.json `guests` (s72).
APPS=(
  "notepad:NOTEPAD.EXE"                 # done    -- edits and saves, byte-verified
  "pbrush:PBRUSH.EXE PBRUSH.DLL"        # done    -- draws in colour, saves a valid BMP
  "sol:SOL.EXE"                         # done    -- plays; Options/Deck dialogs do not open
  "winmine:WINMINE.EXE"                 # done    -- plays; Preferences dialog does not open
  "charmap:CHARMAP.EXE"                 # done    -- selects and copies
  "calc:CALC.EXE"                       # partial -- calculates correctly, not user-confirmed
  "write:WRITE.EXE"                     # partial -- window + caret; nothing typed/saved yet
  "cardfile:CARDFILE.EXE"               # partial -- card drawn; nothing typed/saved yet
  "clock:CLOCK.EXE"                     # partial -- window up, face blank (paint loop)
  "sysedit:SYSEDIT.EXE"                 # partial -- MDI frame, reads the 4 files
  "taskman:TASKMAN.EXE"                 # partial -- dialog up, list empty (GetWindow)
  "recorder:RECORDER.EXE RECORDER.DLL"  # partial -- window up; nothing recorded
  "mplayer:MPLAYER.EXE"                 # partial -- window up; nothing played
  "soundrec:SOUNDREC.EXE"               # partial -- window up; own controls unpainted
  "packager:PACKAGER.EXE"               # partial -- window up; nothing packaged
  "progman:PROGMAN.EXE"                 # partial -- shell frame up; no groups
  "terminal:TERMINAL.EXE"               # partial -- port dialog up, listbox empty
)

changed=0
for entry in "${APPS[@]}"; do
  app="${entry%%:*}"; files="${entry#*:}"
  for f in $files; do
    [ -f "$SRC/$f" ] || { echo "MISSING guest/win16/$f" >&2; continue; }
    if [ -f "$DST/$app/$f" ] && cmp -s "$SRC/$f" "$DST/$app/$f"; then continue; fi
    changed=1
    if [ $MODE = check ]; then echo "  differs: $app/$f"; continue; fi
    mkdir -p "$DST/$app" && cp "$SRC/$f" "$DST/$app/$f" && echo "  staged:  $app/$f"
  done
done
# Anything in demo/win16 that is not one of ours is the user's; only report it.
for d in "$DST"/*/; do
  [ -d "$d" ] || continue
  n=$(basename "$d"); case " ${APPS[*]} " in *" $n:"*) ;; *) echo "  (not ours, left alone: $n/)";; esac
done
if [ $MODE = check ]; then [ $changed = 0 ] && echo "demo/win16 matches guest/win16" || echo "demo/win16 differs"; fi
exit 0
