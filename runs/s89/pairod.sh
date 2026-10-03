#!/usr/bin/env bash
# s89 #302: owner-draw string lists no longer stripped -- Charmap ours vs stock.
set -u
ROOT=/Users/matthew/Development/MrMatthewLayton/ntvdmex; SH=/private/tmp/xpshare; OUT=$ROOT/runs/s89/pairod; mkdir -p $OUT
R='C:\Documents and Settings\All Users\Documents\ntvdmex'
ctl() { rm -f $SH/debug/ctl/control.txt; printf '%s\r\n' "$1" > $SH/debug/ctl/control.txt; for i in $(seq 1 30); do [ -f $SH/debug/ctl/control.txt ] || break; sleep 2; done; }
rm -f $SH/debug/out/procs_now.txt; ctl "exec cmd /c \"$R\\debug\\rig\\hostcheck.bat\""; sleep 8
if grep -qi "ntvdmhost.exe\|ntvdm.exe" $SH/debug/out/procs_now.txt; then echo "ABORT: programs running"; exit 1; fi
cp $ROOT/runs/s89/B_od.exe $SH/bin/ntvdmhost.exe; echo "bin=$(md5 -q $SH/bin/ntvdmhost.exe)"
awk '{printf "%s\r\n", $0}' $ROOT/scripts/bm/w16pair.bat > $SH/debug/rig/w16pair.bat
for e in "charmap|Character Map"; do
  a=${e%%|*}; c=${e#*|}; rm -f $SH/debug/out/pair_done.txt
  ctl "exec cmd /c \"\"$R\\debug\\rig\\w16pair.bat\" $a \"$c\"\""
  t=0; until [ -f $SH/debug/out/pair_done.txt ] || [ $t -ge 150 ]; do sleep 5; t=$((t+5)); done
  cp $SH/debug/out/pair_$a* $OUT/ 2>/dev/null
  echo "$a:"; grep -a "win:\|restored target\|MISSING" $OUT/pair_$a.txt | grep -v "NTVDMEX\|Program Manager\|cmd.exe"
done
echo "== DONE"
