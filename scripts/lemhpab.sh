#!/usr/bin/env bash
# lemhpab.sh -- interleaved A/B of the BY-HAND Lemmings "High Performance PC" launch.
#
# WHY INTERLEAVED: a survival rate measured by running build A five times and then
# build B five times is worthless if the failure is intermittent or the box drifts --
# this project already reported a regression that did not exist from timing two builds
# minutes apart. So alternate, every round, and report per-build tallies.
#
#   ./scripts/lemhpab.sh <rounds> <exeA> <exeB>
#
# Each round: kill the host, deploy the exe, run bm\lemhp.bat, then read back whether
# ntvdmhost is STILL IN tasklist ("HOST IS GONE" if not) and the last heartbeat, which
# is where the guest was. Both builds carry livehb.flag support, so a death has a place.
set -uo pipefail
SH=/private/tmp/xpshare
ROUNDS="${1:-3}"; EXEA="${2:?usage: lemhpab.sh <rounds> <exeA> <exeB>}"; EXEB="${3:?}"
A_ALIVE=0; A_DEAD=0; B_ALIVE=0; B_DEAD=0

ctl() { printf '%s\r\n' "$1" > "$SH/control.txt"
        for _ in $(seq 1 30); do [ -f "$SH/control.txt" ] || return 0; sleep 2; done
        echo "  !! controld never consumed control.txt" >&2; return 1; }

for r in $(seq 1 "$ROUNDS"); do
  for side in A B; do
    [ "$side" = A ] && EXE="$EXEA" || EXE="$EXEB"
    TAG="${side}${r}"
    ctl "kill" >/dev/null 2>&1; sleep 3
    # ⚠ A RUNNING HOST LOCKS ITS OWN EXE, so a failed copy would silently leave the
    #   PREVIOUS build in place and the round would measure the wrong binary.
    for _ in $(seq 1 5); do cp "$EXE" "$SH/bin/ntvdmhost.exe" 2>/dev/null && break; sleep 2; done
    GOT=$(md5 -q "$SH/bin/ntvdmhost.exe"); WANT=$(md5 -q "$EXE")
    if [ "$GOT" != "$WANT" ]; then echo "round $TAG: DEPLOY FAILED ($GOT != $WANT)"; continue; fi
    rm -f "$SH/debug/out/lemhp_$TAG.txt" "$SH/debug/out/lemhp_${TAG}_host.txt"
    ctl "exec cmd /c \"\"C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\debug\\rig\\lemhp.bat\" $TAG\"" >/dev/null
    for _ in $(seq 1 45); do [ -f "$SH/debug/out/lemhp_$TAG.txt" ] && break; sleep 2; done
    if [ ! -f "$SH/debug/out/lemhp_$TAG.txt" ]; then echo "round $TAG: NO RESULT FILE"; continue; fi
    HOSTLOG="$SH/debug/out/lemhp_${TAG}_host.txt"
    if grep -q "HOST IS GONE" "$SH/debug/out/lemhp_$TAG.txt"; then
      VERDICT=DEAD
      [ "$side" = A ] && A_DEAD=$((A_DEAD+1)) || B_DEAD=$((B_DEAD+1))
    else
      VERDICT=alive
      [ "$side" = A ] && A_ALIVE=$((A_ALIVE+1)) || B_ALIVE=$((B_ALIVE+1))
    fi
    LASTHB=$(grep -a "^HB " "$HOSTLOG" 2>/dev/null | tail -1 | grep -oE "cs:ip=[0-9a-fx:]+ " | head -1)
    RELOAD=$(grep -a -oE "clocks_since_load=[0-9]+" "$HOSTLOG" 2>/dev/null | head -1)
    NHB=$(grep -a -c "^HB " "$HOSTLOG" 2>/dev/null || echo 0)
    M3=$(grep -a "MOUSEI33 ax:" "$HOSTLOG" 2>/dev/null | tail -1 | grep -oE "0003x[0-9a-f]+" | head -1)
    echo "round $TAG ($(basename "$(dirname "$(dirname "$EXE")")")): $VERDICT  hb=$NHB $LASTHB $RELOAD ${M3:-no-AX3}"
    cp "$HOSTLOG" "runs/lemref/ab_$TAG.log" 2>/dev/null
  done
done
echo "== A (OLD $EXEA): $A_ALIVE alive / $A_DEAD dead =="
echo "== B (NEW $EXEB): $B_ALIVE alive / $B_DEAD dead =="
