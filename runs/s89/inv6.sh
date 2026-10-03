#!/usr/bin/env bash
# s89: B_305 (WM_DESTROY sent; COMMDLG; BM_; focus) -- shelf regression, Charmap's saved font, Notepad Find.
set -u
ROOT=/Users/matthew/Development/MrMatthewLayton/ntvdmex; SH=/private/tmp/xpshare; OUT=$ROOT/runs/s89/inv6; mkdir -p $OUT
R='C:\Documents and Settings\All Users\Documents\ntvdmex'
ctl() { rm -f $SH/debug/ctl/control.txt; printf '%s\r\n' "$1" > $SH/debug/ctl/control.txt; for i in $(seq 1 30); do [ -f $SH/debug/ctl/control.txt ] || break; sleep 2; done; sleep 3; }
rm -f $SH/debug/out/procs_now.txt; ctl "exec cmd /c \"$R\\debug\\rig\\hostcheck.bat\""; sleep 4
if grep -qi "ntvdmhost.exe\|ntvdm.exe" $SH/debug/out/procs_now.txt 2>/dev/null; then echo "ABORT: programs running"; exit 1; fi
cp $ROOT/runs/s89/B_init.exe $SH/bin/ntvdmhost.exe; echo "bin=$(md5 -q $SH/bin/ntvdmhost.exe)"
(cd $ROOT && ./scripts/w16shelf.sh > $OUT/shelf.txt 2>&1); cat $OUT/shelf.txt
cp $SH/debug/out/w16close_*_host.log $SH/debug/out/w16close_*.txt $SH/debug/out/w16close_*.bmp $OUT/ 2>/dev/null
echo "== DONE"
