#!/usr/bin/env bash
#
# bmsxs.sh -- put a batch of Win16 guests on the rig's desktop under BOTH hosts,
# side by side, and leave them there for a human to judge. GH #128.
#
#   ./scripts/bmsxs.sh                    # the default confirmation batch
#   ./scripts/bmsxs.sh CALC WRITE         # just those
#   ./scripts/bmsxs.sh --no-deploy CALC   # re-run with whatever is on the box
#
# WHY THIS EXISTS. `guests` is the heaviest item in the score model and its rule is
# that `done` means USER-CONFIRMED -- so the only thing that moves it is a person
# looking at a running program. The user's condition for doing that is fixed and
# reasonable:
#
#   "if you want me to manually test, you need to run each app side-by-side,
#    one in stock NTVDM and one in NTVDMEX"
#
# So this does not screenshot anything and does not decide anything. It leaves a
# desktop in the state where the judgement can be made: ours down the LEFT half,
# stock's down the RIGHT, same programs, same moment.
#
# It goes through controld's `exec` lever rather than the watcher, for the same
# reason bmwow.sh does: rt.bat runs a DOS target out of bm\tests, and this is
# "launch 16-bit WINDOWS programs and let the IFEO hook route them".
set -uo pipefail
SH=/private/tmp/xpshare
HOST=build/ntvdmhost.exe
SHOT=build/rigshot.exe
TIMEOUT="${TIMEOUT:-420}"

[ -d "$SH" ] || { echo "share not mounted at $SH" >&2; exit 2; }

if [ "${1:-}" = "--no-deploy" ]; then
  shift
else
  [ -f "$HOST" ] || { echo "no $HOST -- run ./scripts/build.sh first" >&2; exit 2; }
  # ⚠ CHECKSUM WHAT IS DEPLOYED. build/ produces ntvdmex.exe AND ntvdmhost.exe and
  #   the host is the second one; copying the wrong one has cost more than one
  #   session, with runs that kept "succeeding" against a stale log.
  cp "$HOST" "$SH/bin/ntvdmhost.exe" || exit 2
  L=$(md5 -q "$HOST"); R=$(md5 -q "$SH/bin/ntvdmhost.exe")
  [ "$L" = "$R" ] || { echo "DEPLOY MISMATCH local=$L share=$R" >&2; exit 2; }
  echo "deployed ntvdmhost.exe  md5=$L"
  if [ -f "$SHOT" ]; then
    cp "$SHOT" "$SH/debug/rig/rigshot.exe" || exit 2
    echo "deployed rigshot.exe    md5=$(md5 -q "$SHOT")"
  fi
fi
cp scripts/bm/sxs.bat "$SH/debug/rig/sxs.bat" || exit 2

GUESTS="$*"
[ -n "$GUESTS" ] || GUESTS="CALC WRITE CARDFILE SYSEDIT"

# ⚠ DELETE THE DESTINATIONS BEFORE THE RUN. An absent artefact is a loud failure;
#   a stale one is a silent wrong answer, and this project has paid for that once.
rm -f "$SH/sxs_done.txt" "$SH/sxs.txt" "$SH/sxs.bmp"

printf 'exec cmd /c "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\debug\\rig\\sxs.bat" %s\r\n' \
  "$GUESTS" > "$SH/debug/ctl/control.txt"
echo "queued via controld: $GUESTS   (up to ${TIMEOUT}s for sxs_done.txt)"

for ((i=0; i<30; i++)); do [ -f "$SH/debug/ctl/control.txt" ] || break; sleep 2; done
if [ -f "$SH/debug/ctl/control.txt" ]; then
  echo "FAILED: controld never consumed control.txt -- daemon is not running" >&2
  echo "  heartbeat: $(cat "$SH/debug/ctl/controld.txt" 2>/dev/null)" >&2
  exit 2
fi

for ((i=0; i<TIMEOUT; i++)); do
  if [ -f "$SH/sxs_done.txt" ]; then
    sleep 3
    echo
    sed -n '/what is on the desktop now/,$p' "$SH/sxs.txt" 2>/dev/null | head -60
    echo
    # ⚠ THE IFEO LINE IS THE ONE THAT MATTERS MOST. A run that left the Debugger
    #   value absent makes every later test on this box silently measure stock.
    grep -E '\[sxs\] (IFEO|!!)' "$SH/sxs.txt" 2>/dev/null
    grep -E 'NOT RUNNING' "$SH/sxs.txt" 2>/dev/null
    echo "sxs.bmp: $(stat -f '%z bytes, %Sm' -t '%H:%M:%S' "$SH/sxs.bmp" 2>/dev/null || echo MISSING)"
    exit 0
  fi
  sleep 1
done
echo "TIMEOUT after ${TIMEOUT}s: sxs_done.txt never appeared" >&2
echo "  last log lines:" >&2
tail -20 "$SH/sxs.txt" 2>/dev/null >&2
exit 3
