#!/usr/bin/env bash
set -u
ROOT=/Users/matthew/Development/MrMatthewLayton/ntvdmex; SH=/private/tmp/xpshare
R='C:\Documents and Settings\All Users\Documents\ntvdmex'
rm -f $SH/debug/out/procs_now.txt; rm -f $SH/debug/ctl/control.txt
printf '%s\r\n' "exec cmd /c \"$R\\debug\\rig\\hostcheck.bat\"" > $SH/debug/ctl/control.txt; sleep 10
if grep -qi "ntvdmhost.exe\|ntvdm.exe" $SH/debug/out/procs_now.txt 2>/dev/null; then echo "ABORT: programs running"; exit 1; fi
cp $ROOT/runs/s89/B_294.exe $SH/bin/ntvdmhost.exe; echo "bin=$(md5 -q $SH/bin/ntvdmhost.exe)"
cd $ROOT && ./tools/wintest/stock.sh w_kprof
echo "== DONE"
