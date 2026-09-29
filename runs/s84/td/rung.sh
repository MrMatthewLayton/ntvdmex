#!/usr/bin/env bash
# rung.sh <idx> <tag> -- Doom 1.9s timedemo demo3 at CPU-speed rung <idx> (cfg\cpuspd.txt).
SH=/private/tmp/xpshare; HERE=$(cd "$(dirname "$0")" && pwd)
printf '%s' "$1" > $SH/cfg/cpuspd.txt; printf '1200000' > $SH/cfg/headless_ms.txt
( cd "$HERE/../../.." && TIMEOUT=1300 ./scripts/bmqueue.sh doomsw DOOM.EXE -timedemo demo3 2>&1 | tail -3 )
rm -f $SH/cfg/cpuspd.txt $SH/cfg/headless_ms.txt
cp $SH/debug/out/result_doomsw.log "$HERE/$2.log" 2>/dev/null
r=$(LC_ALL=C grep -a -o "timed [0-9]* gametics in [0-9]* realtics" "$HERE/$2.log" | tail -1)
rt=$(echo "$r" | awk '{print $5}')
echo "$2 idx=$1: $r  fps=$( [ -n "$rt" ] && echo "scale=1; 74690/$rt" | bc)"
