#!/usr/bin/env bash
# w16drive.sh <folder> <EXE> <stepfile.txt> -- run scripts/bm/w16drive.bat on the rig
# from the Mac (s90): stages the .bat and the step file, queues it on controld, waits
# for w16drive_done.txt, prints the report. Run under scripts/riglock.sh.
# ⚠ w16drive.bat taskkills EVERY host: run scripts/bm/hostcheck.sh first.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SH=/private/tmp/xpshare
FOLD="${1:?folder}"; EXE="${2:?exe}"; STEPS="${3:?step file}"
NAME="$(basename "$STEPS" .txt)"
[ -d "$SH/debug" ] || { echo "share not mounted at $SH" >&2; exit 2; }
awk '{printf "%s\r\n", $0}' "$ROOT/scripts/bm/w16drive.bat" > "$SH/debug/rig/w16drive.bat"
awk '{printf "%s\r\n", $0}' "$STEPS" > "$SH/debug/rig/$NAME.txt"
rm -f "$SH/debug/out/w16drive_done.txt" "$SH/debug/out/w16drive_$NAME.txt"
printf 'exec cmd /c "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\debug\\rig\\w16drive.bat" %s %s %s\r\n' \
    "$FOLD" "$EXE" "$NAME" > "$SH/debug/ctl/control.txt"
for i in $(seq 1 40); do [ -f "$SH/debug/ctl/control.txt" ] || break; sleep 2; done
for i in $(seq 1 150); do [ -f "$SH/debug/out/w16drive_done.txt" ] && break; sleep 2; done
cat "$SH/debug/out/w16drive_$NAME.txt" 2>/dev/null || echo "NO REPORT"
