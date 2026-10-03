#!/usr/bin/env bash
set -u
ROOT=/Users/matthew/Development/MrMatthewLayton/ntvdmex; SH=/private/tmp/xpshare; OUT=$ROOT/runs/s89/find3; mkdir -p $OUT
R='C:\Documents and Settings\All Users\Documents\ntvdmex'
ctl() { rm -f $SH/debug/ctl/control.txt; printf '%s\r\n' "$1" > $SH/debug/ctl/control.txt; for i in $(seq 1 30); do [ -f $SH/debug/ctl/control.txt ] || break; sleep 2; done; sleep 3; }
rm -f $SH/debug/out/procs_now.txt; ctl "exec cmd /c \"$R\\debug\\rig\\hostcheck.bat\""; sleep 4
if grep -qi "ntvdmhost.exe\|ntvdm.exe" $SH/debug/out/procs_now.txt 2>/dev/null; then echo "ABORT: programs running"; cat $SH/debug/out/procs_now.txt; exit 1; fi
cp $ROOT/runs/s89/B_anchor.exe $SH/bin/ntvdmhost.exe; echo "bin=$(md5 -q $SH/bin/ntvdmhost.exe)"
awk '{printf "%s\r\n", $0}' $ROOT/scripts/bm/w16find.bat > $SH/debug/rig/w16find.bat
rm -f $SH/debug/out/w16find_done.txt
ctl "exec cmd /c \"\"$R\\debug\\rig\\w16find.bat\"\""
t=0; until [ -f $SH/debug/out/w16find_done.txt ] || [ $t -ge 180 ]; do sleep 5; t=$((t+5)); done
cp $SH/debug/out/w16find* $OUT/ 2>/dev/null; cat $OUT/w16find.txt | grep -v LDTSYNC
echo "== DONE"
