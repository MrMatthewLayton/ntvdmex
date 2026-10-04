#!/usr/bin/env bash
# w16stockdrive.sh <folder> <EXE> <stepfile.txt> -- run a w16drive step file under STOCK
# ntvdm on the rig (scripts/bm/w16stockdrive.bat; IFEO dropped and restored, checked
# here). `shot NAME` lands as debug\out\NAME_stock.bmp. Run under scripts/riglock.sh. (s92)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SH=/private/tmp/xpshare
FOLD="${1:?folder}"; EXE="${2:?exe}"; STEPS="${3:?step file}"
NAME="$(basename "$STEPS" .txt)"
[ -d "$SH/debug" ] || { echo "share not mounted at $SH" >&2; exit 2; }
awk '{printf "%s\r\n", $0}' "$ROOT/scripts/bm/w16stockdrive.bat" > "$SH/debug/rig/w16stockdrive.bat"
awk '{printf "%s\r\n", $0}' "$STEPS" > "$SH/debug/rig/$NAME.txt"
rm -f "$SH/w16stock_done.txt" "$SH/debug/out/w16sdrive_$NAME.txt" "$SH/debug/out/w16stock_state.txt"
printf 'exec cmd /c "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\debug\\rig\\w16stockdrive.bat" %s %s %s\r\n' \
    "$FOLD" "$EXE" "$NAME" > "$SH/debug/ctl/control.txt"
for i in $(seq 1 40); do [ -f "$SH/debug/ctl/control.txt" ] || break; sleep 2; done
for i in $(seq 1 150); do [ -f "$SH/w16stock_done.txt" ] && break; sleep 2; done
cat "$SH/debug/out/w16sdrive_$NAME.txt" 2>/dev/null || echo "NO REPORT"
grep -q "restored target EXISTS" "$SH/debug/out/w16stock_state.txt" || { echo "⛔ IFEO NOT PROVEN RESTORED" >&2; exit 3; }
