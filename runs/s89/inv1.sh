#!/usr/bin/env bash
# s89: the inventory's ground truth -- whole shelf on the current host, every host log kept.
set -u
ROOT=/Users/matthew/Development/MrMatthewLayton/ntvdmex; SH=/private/tmp/xpshare; OUT=$ROOT/runs/s89/inv1; mkdir -p $OUT
R='C:\Documents and Settings\All Users\Documents\ntvdmex'
echo "bin=$(md5 -q $SH/bin/ntvdmhost.exe)"
rm -f $SH/debug/out/procs_now.txt; rm -f $SH/debug/ctl/control.txt
printf '%s\r\n' "exec cmd /c \"$R\\debug\\rig\\hostcheck.bat\"" > $SH/debug/ctl/control.txt; sleep 8
if grep -qi "ntvdmhost.exe\|ntvdm.exe" $SH/debug/out/procs_now.txt 2>/dev/null; then echo "ABORT: programs running"; cat $SH/debug/out/procs_now.txt; exit 1; fi
(cd $ROOT && ./scripts/w16shelf.sh > $OUT/shelf.txt 2>&1); cat $OUT/shelf.txt
cp $SH/debug/out/w16close_*_host.log $SH/debug/out/w16close_*.txt $OUT/ 2>/dev/null
echo "== DONE"
