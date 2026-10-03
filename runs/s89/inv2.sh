#!/usr/bin/env bash
# s89 #293: deploy the profile-write build, set WinMine/Clock .ini aside, run the whole shelf,
# collect every host log (the inventory's ground truth) and the .ini files the run wrote.
set -u
ROOT=/Users/matthew/Development/MrMatthewLayton/ntvdmex; SH=/private/tmp/xpshare; OUT=$ROOT/runs/s89/inv2; mkdir -p $OUT
R='C:\Documents and Settings\All Users\Documents\ntvdmex'
ctl() { rm -f $SH/debug/ctl/control.txt; printf '%s\r\n' "$1" > $SH/debug/ctl/control.txt; for i in $(seq 1 30); do [ -f $SH/debug/ctl/control.txt ] || break; sleep 2; done; sleep 3; }
rm -f $SH/debug/out/procs_now.txt; ctl "exec cmd /c \"$R\\debug\\rig\\hostcheck.bat\""; sleep 4
if grep -qi "ntvdmhost.exe\|ntvdm.exe" $SH/debug/out/procs_now.txt 2>/dev/null; then echo "ABORT: programs running"; cat $SH/debug/out/procs_now.txt; exit 1; fi
cp $ROOT/runs/s89/B_293.exe $SH/bin/ntvdmhost.exe; echo "bin=$(md5 -q $SH/bin/ntvdmhost.exe)"
mkdir -p $SH/debug/out/ini_before $SH/debug/out/ini_after
ctl "exec cmd /c copy /y C:\\WINDOWS\\winmine.ini \"$R\\debug\\out\\ini_before\\\" & copy /y C:\\WINDOWS\\clock.ini \"$R\\debug\\out\\ini_before\\\" & copy /y C:\\WINDOWS\\win.ini \"$R\\debug\\out\\ini_before\\\" & del /q C:\\WINDOWS\\winmine.ini C:\\WINDOWS\\clock.ini"
ls -la $SH/debug/out/ini_before/
(cd $ROOT && ./scripts/w16shelf.sh > $OUT/shelf.txt 2>&1); cat $OUT/shelf.txt
ctl "exec cmd /c copy /y C:\\WINDOWS\\winmine.ini \"$R\\debug\\out\\ini_after\\\" & copy /y C:\\WINDOWS\\clock.ini \"$R\\debug\\out\\ini_after\\\" & copy /y C:\\WINDOWS\\win.ini \"$R\\debug\\out\\ini_after\\\""
cp -r $SH/debug/out/ini_before $SH/debug/out/ini_after $OUT/; ls -la $OUT/ini_after/
cp $SH/debug/out/w16close_*_host.log $SH/debug/out/w16close_*.txt $SH/debug/out/w16close_*.bmp $OUT/ 2>/dev/null
echo "== DONE"
