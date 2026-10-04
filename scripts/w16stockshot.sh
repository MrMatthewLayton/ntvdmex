#!/usr/bin/env bash
# w16stockshot.sh <folder> <EXE> <shotname> -- photograph a Win16 program under STOCK
# ntvdm on the rig (scripts/bm/w16stockshot.bat; IFEO dropped and restored, checked
# here). Then the same program under OURS (scripts/bm/w16drive.bat, one `shot`).
# Both BMPs land in runs/stockshot/. Run under scripts/riglock.sh. (s91)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SH=/private/tmp/xpshare
F="${1:?folder}"; E="${2:?exe}"; N="${3:?shot name}"
mkdir -p "$ROOT/runs/stockshot"
awk '{printf "%s\r\n", $0}' "$ROOT/scripts/bm/w16stockshot.bat" > "$SH/debug/rig/w16stockshot.bat"
rm -f "$SH/w16stock_done.txt" "$SH/debug/out/${N}_stock.bmp"
printf 'exec cmd /c "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\debug\\rig\\w16stockshot.bat" %s %s %s_stock\r\n' "$F" "$E" "$N" > "$SH/debug/ctl/control.txt"
for i in $(seq 1 60); do [ -f "$SH/w16stock_done.txt" ] && break; sleep 2; done
grep -q "restored target EXISTS" "$SH/debug/out/w16stock_state.txt" || { echo "⛔ IFEO NOT PROVEN RESTORED" >&2; exit 3; }
cp "$SH/debug/out/${N}_stock.bmp" "$ROOT/runs/stockshot/" 2>/dev/null
printf 'wait 8\nlist\nshot %s_ours\n' "$N" > "$ROOT/runs/stockshot/drv_shot.txt"
bash "$ROOT/scripts/bm/hostcheck.sh" >/dev/null 2>&1
"$ROOT/scripts/w16drive.sh" "$F" "$E" "$ROOT/runs/stockshot/drv_shot.txt" | grep "win:" | head -4
cp "$SH/debug/out/${N}_ours.bmp" "$ROOT/runs/stockshot/" 2>/dev/null
ls -la "$ROOT/runs/stockshot/${N}"_*.bmp
