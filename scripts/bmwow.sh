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

# ── DEPLOY THE WOW32 ANSWER OVERRIDES.
#   `wow32ret.txt` changes what an UNIMPLEMENTED WOW32 id answers, and every call
#   it changes is logged as an EXPERIMENT rather than a service.
# ★ IT IS NO LONGER LOAD-BEARING. It used to carry `7d 00000001`, which was the
#   difference between WOWEXEC.EXE running and krnl386 spinning 1884 times in
#   seg2:0x2a08 until its stack was gone. 0x7d is now a real service (it echoes
#   the task selector it is offered -- see wow32.h), so the file ships EMPTY and a
#   deploy line reading "0 override(s)" is the correct state. WOW32RET=0 skips it.
if [ "${WOW32RET:-1}" = "1" ] && [ -f scripts/bm/wow32ret.txt ]; then
  cp scripts/bm/wow32ret.txt "$SH/wow32ret.txt" || exit 2
  echo "deployed wow32ret.txt ($(grep -cv '^#' scripts/bm/wow32ret.txt) override(s))"
else
  rm -f "$SH/wow32ret.txt"
  echo "wow32ret.txt NOT deployed -- unimplemented ids answer the plain sentinel"
fi

# Disarm the PM breakpoint list unless the caller explicitly wants it. A stale
# pmbp.txt halts the guest at addresses from a previous investigation, and the
# resulting log looks like a new frontier rather than an old breakpoint.
if [ "${PMBP:-0}" != "1" ] && [ -f "$SH/pmbp.txt" ]; then
  mv "$SH/pmbp.txt" "$SH/pmbp.txt.disarmed"
  echo "disarmed pmbp.txt (PMBP=1 to keep it)"
fi

# ⚠⚠ MOVE THE TWO SWITCHES ASIDE, BECAUSE THE BASELINE IS MEASURED WITHOUT THEM.
#   `wowsched.txt` and `wowcall.txt` turn on the task scheduler and 16-bit
#   callbacks. wowlive.bat CREATES both -- it has to, a guest cannot launch
#   without them -- so any gate run that follows a live session silently
#   measures a DIFFERENT CONFIGURATION: the guest runs much further and the
#   counters come back several times the baseline, which reads exactly like
#   catastrophic drift and is nothing of the kind.
#   This was written down as a standing hazard and left to be remembered by
#   hand, and session 55 duly forgot it and got 280/291/74 against a documented
#   85/113/57. A script that knows the rule cannot forget it. SWITCHES=1 keeps
#   them, for deliberately measuring the other configuration.
if [ "${SWITCHES:-0}" != "1" ]; then
  for sw in wowsched wowcall; do
    if [ -f "$SH/$sw.txt" ]; then
      mv "$SH/$sw.txt" "$SH/$sw.txt.gateaside"
      echo "moved $sw.txt aside for the gate (SWITCHES=1 to keep it)"
    fi
  done
fi

rm -f "$SH/wow_done.txt" "$SH/wow_host.txt" "$SH/wow_ldt.txt" "$SH/wow_wd.txt" "$SH/wow_alive.txt" "$SH/alive.txt"
# The Win16 program to run. Empty = wowrun.bat's default (SYSEDIT.EXE), so the
# baseline invocation is unchanged; TARGET=C:\\WINDOWS\\winhelp.exe runs another.
printf 'exec cmd /c "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\wowrun.bat" %s\r\n' \
  "${TARGET:-} ${WOWWAIT:-}" > "$SH/control.txt"
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
    # ── PRINT THE GATE'S OWN SIGNATURE. It is `serviced / declined / unimpl`
    #   plus the address the guest stops at, and it has been counted BY HAND out
    #   of the log every session it has ever been quoted -- which is why it is
    #   quoted inconsistently. LC_ALL=C because the log is not valid UTF-8 and
    #   grep will otherwise refuse to match in it.
    S=$(LC_ALL=C grep -ac -- "-> SERVICED"      "$SH/wow_host.txt" 2>/dev/null || echo 0)
    D=$(LC_ALL=C grep -ac -- "-> DECLINED"      "$SH/wow_host.txt" 2>/dev/null || echo 0)
    U=$(LC_ALL=C grep -ac -- "-> UNIMPLEMENTED" "$SH/wow_host.txt" 2>/dev/null || echo 0)
    W=$(LC_ALL=C grep -ao "0001:229[Cc]" "$SH/wow_host.txt" 2>/dev/null | tail -1)
    echo "GATE: ${S} serviced / ${D} declined / ${U} unimpl${W:+ · $W}"
    echo "      baseline (session 50): 85 / 113 / 57 · 0001:229C"
    echo "      ⚠ only comparable with wowsched.txt/wowcall.txt MOVED ASIDE"
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
