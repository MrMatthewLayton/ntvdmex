#!/usr/bin/env bash
# gtype.sh -- type a string into the guest SLOWLY, one ATOMIC key-chord per character with a big
# inter-key gap, to beat this XP VM's laggy/interleaving QMP send-key. Fast multi-key type() calls
# overlap in QEMU's send-key queue and drop/reorder chars (observed 2026-08-11: "D:\pfrun.bat" came
# out "pfrun.batD:\pfrun.bat"). One chord at a time, ordered, lands reliably.
# Usage:  scripts/gtype.sh 'D:\pfrun.bat'      (env GTYPE_GAP overrides the 0.6s gap)
# NB: UK layout -- '\' is qcode "less"; ':' is shift+semicolon (matches scripts/qmp.py).
cd "$(dirname "$0")/.."
GAP="${GTYPE_GAP:-0.6}"
send(){ python3 scripts/qmp.py key "$@" >/dev/null 2>&1; sleep "$GAP"; }
s="$1"; i=0
while [ $i -lt ${#s} ]; do
  c="${s:$i:1}"
  case "$c" in
    [A-Z]) send shift "$(printf '%s' "$c" | tr 'A-Z' 'a-z')";;
    [a-z0-9]) send "$c";;
    ':') send shift semicolon;;
    '\') send less;;
    '.') send dot;;
    '-') send minus;;
    '_') send shift minus;;
    '/') send slash;;
    ' ') send spc;;
    *) echo "gtype: unmapped char '$c'" >&2;;
  esac
  i=$((i+1))
done
echo "gtype done: $s"
