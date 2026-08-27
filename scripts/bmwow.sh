#!/usr/bin/env bash
#
# bmwow.sh -- deploy this build to the bare-metal XP rig and run the WOW bootstrap.
# GH #128.
#
#   ./scripts/bmwow.sh                 # deploy, run, collect
#   ./scripts/bmwow.sh --no-deploy     # re-run whatever is already on the box
#
# The normal test path (bmqueue.sh -> runwatch.bat -> rt.bat) cannot drive this:
# rt.bat runs a DOS target out of bm\tests, and a WOW run is "launch a 16-bit
# WINDOWS program and let the IFEO hook route it to us". So this goes through
# controld's `exec` lever instead, which is independent of the watcher and cannot
# be wedged by a hung guest.
#
# ⚠ DELETE THE DESTINATIONS BEFORE THE RUN. A "before" and an "after" once
#   analysed the same hours-old file and produced byte-identical histograms,
#   because nothing on the box had actually written a new one. An absent artefact
#   is a loud failure; a stale one is a silent wrong answer.
# ⚠ CHECKSUM WHAT IS DEPLOYED. build/ produces ntvdmex.exe AND ntvdmhost.exe and
#   the host is the second one; copying the wrong one has cost more than one
#   session, with runs that kept "succeeding" against a stale log.
set -uo pipefail
SH=/private/tmp/xpshare
HOST=build/ntvdmhost.exe
TIMEOUT="${TIMEOUT:-180}"

[ -f "$HOST" ] || { echo "no $HOST -- run ./scripts/build.sh first" >&2; exit 2; }
[ -d "$SH" ]   || { echo "share not mounted at $SH" >&2; exit 2; }

if [ "${1:-}" != "--no-deploy" ]; then
  cp "$HOST" "$SH/bm/ntvdmhost.exe" || exit 2
  L=$(md5 -q "$HOST"); R=$(md5 -q "$SH/bm/ntvdmhost.exe")
  [ "$L" = "$R" ] || { echo "DEPLOY MISMATCH local=$L share=$R" >&2; exit 2; }
  echo "deployed ntvdmhost.exe  md5=$L"
fi

# Disarm the PM breakpoint list unless the caller explicitly wants it. A stale
# pmbp.txt halts the guest at addresses from a previous investigation, and the
# resulting log looks like a new frontier rather than an old breakpoint.
if [ "${PMBP:-0}" != "1" ] && [ -f "$SH/pmbp.txt" ]; then
  mv "$SH/pmbp.txt" "$SH/pmbp.txt.disarmed"
  echo "disarmed pmbp.txt (PMBP=1 to keep it)"
fi

rm -f "$SH/wow_done.txt" "$SH/wow_host.txt" "$SH/wow_ldt.txt" "$SH/wow_wd.txt" "$SH/wow_alive.txt" "$SH/alive.txt"
printf 'exec cmd /c "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\wowrun.bat"\r\n' \
  > "$SH/control.txt"
echo "queued via controld; waiting up to ${TIMEOUT}s for wow_done.txt"

for ((i=0; i<30; i++)); do [ -f "$SH/control.txt" ] || break; sleep 2; done
if [ -f "$SH/control.txt" ]; then
  echo "FAILED: controld never consumed control.txt -- daemon is not running" >&2
  echo "  heartbeat: $(cat "$SH/controld.txt" 2>/dev/null)" >&2
  exit 2
fi

for ((i=0; i<TIMEOUT; i++)); do
  if [ -f "$SH/wow_done.txt" ]; then
    sleep 3                                     # let the copies settle
    echo "done: wow_host.txt $(stat -f '%z bytes, %Sm' -t '%H:%M:%S' "$SH/wow_host.txt" 2>/dev/null)"
    if [ -n "${ARCHIVE:-}" ]; then
      mkdir -p "$ARCHIVE"; S=$(date +%H%M%S)
      cp "$SH/wow_host.txt" "$ARCHIVE/wow_host_$S.log" 2>/dev/null
      echo "archived: $ARCHIVE/wow_host_$S.log"
    fi
    exit 0
  fi
  sleep 1
done
echo "TIMEOUT after ${TIMEOUT}s: wow_done.txt never appeared" >&2
exit 3
