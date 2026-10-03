#!/usr/bin/env bash
SH=/private/tmp/xpshare
rm -f "$SH/debug/out/procs_now.txt"
printf 'exec cmd /c "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\debug\\rig\\hostcheck.bat"\r\n' > "$SH/debug/ctl/control.txt"
for i in $(seq 1 30); do [ -f "$SH/debug/out/procs_now.txt" ] && break; sleep 2; done
sleep 2; cat "$SH/debug/out/procs_now.txt"
