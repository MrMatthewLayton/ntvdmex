#!/usr/bin/env bash
# w16shelf.sh [tag] -- the Win16 shelf through w16close.bat, one line per program, for
# whatever host is in bin\ right now. Run it on two hosts and diff the tables (s82).
# Per program: did its window appear (caption seen before close), did WM_CLOSE find it,
# and did the host exit within 10 s of the X.
# ⛔ PROGMAN IS NOT IN THE LIST: `rigshot close "Program Manager"` finds XP's OWN
#    shell window by that caption, and WM_CLOSE to it raises the shutdown dialog.
set -uo pipefail
SH=/private/tmp/xpshare
TAG="${1:-$(md5 -q "$SH/bin/ntvdmhost.exe" | cut -c1-8)}"
APPS=(
"calc|Calculator" "cardfile|Cardfile - (Untitled)" "charmap|Character Map" "clock|Clock"
"mplayer|Media Player" "notepad|Notepad - (Untitled)" "packager|Object Packager - Package"
"pbrush|Paintbrush - (Untitled)" "recorder|Recorder - (Untitled)" "sol|Solitaire"
"soundrec|Sound Recorder" "sysedit|System Configuration Editor"
"taskman|Task List" "terminal|Terminal - (Untitled)" "winmine|Minesweeper" "write|Write - (Untitled)"
)
echo "== shelf on host $TAG"
for e in "${APPS[@]}"; do
  a=${e%%|*}; c=${e#*|}; R="$SH/debug/out/w16close_$a.txt"
  rm -f "$SH/debug/out/w16close_done.txt" "$R"
  printf 'exec cmd /c ""C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\debug\\rig\\w16close.bat" %s "%s""\r\n' "$a" "$c" > "$SH/debug/ctl/control.txt"
  t=0; until grep -q "== done" "$R" 2>/dev/null || [ $t -ge 120 ]; do sleep 5; t=$((t+5)); ls "$SH/debug/out" >/dev/null; done
  if ! grep -q "== done" "$R" 2>/dev/null; then printf '%-10s TIMEOUT\n' "$a"; continue; fi
  before=$(sed -n '/windows BEFORE/,/sending WM_CLOSE/p' "$R" | grep -c "win: $c")
  found=$(grep -c "close: WM_CLOSE" "$R")
  after=$(sed -n '/windows AFTER/,/host process/p' "$R" | grep -c "win: $c")
  alive=$(sed -n '/host process/,/what the guest/p' "$R" | grep -c "ntvdmhost.exe")
  printf '%-10s window=%s close_found=%s window_after=%s host_alive_after=%s\n' "$a" "$before" "$found" "$after" "$alive"
  mkdir -p "runs/s82/shelf_$TAG"; cp "$R" "runs/s82/shelf_$TAG/" 2>/dev/null
done
