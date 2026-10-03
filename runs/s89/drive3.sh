#!/usr/bin/env bash
set -u
ROOT=/Users/matthew/Development/MrMatthewLayton/ntvdmex; SH=/private/tmp/xpshare; OUT=$ROOT/runs/s89/drive3; mkdir -p $OUT
R='C:\Documents and Settings\All Users\Documents\ntvdmex'
ctl() { rm -f $SH/debug/ctl/control.txt; printf '%s\r\n' "$1" > $SH/debug/ctl/control.txt; for i in $(seq 1 30); do [ -f $SH/debug/ctl/control.txt ] || break; sleep 2; done; sleep 3; }
rm -f $SH/debug/out/procs_now.txt; ctl "exec cmd /c \"$R\\debug\\rig\\hostcheck.bat\""; sleep 4
if grep -qi "ntvdmhost.exe\|ntvdm.exe" $SH/debug/out/procs_now.txt 2>/dev/null; then echo "ABORT: programs running"; exit 1; fi
cp $ROOT/runs/s89/B_gww.exe $SH/bin/ntvdmhost.exe; echo "bin=$(md5 -q $SH/bin/ntvdmhost.exe)"
cp $ROOT/build/rigshot.exe $SH/debug/rig/rigshot.exe
for f in w16drive.bat drv_sr.txt; do awk '{printf "%s\r\n", $0}' $ROOT/scripts/bm/$f > $SH/debug/rig/$f; done
drive() { rm -f $SH/debug/out/w16drive_done.txt; ctl "exec cmd /c \"\"$R\\debug\\rig\\w16drive.bat\" $1 $2 $3\""
  t=0; until [ -f $SH/debug/out/w16drive_done.txt ] || [ $t -ge 200 ]; do sleep 5; t=$((t+5)); done
  cp $SH/debug/out/w16drive_$3* $SH/debug/out/$3_*.bmp $OUT/ 2>/dev/null; echo "######## $3"; cat $OUT/w16drive_$3.txt | grep -v "NTVDMEX test watcher\|Program Manager\|cmd.exe"; }
drive soundrec SOUNDREC.EXE drv_sr
echo "== DONE"
