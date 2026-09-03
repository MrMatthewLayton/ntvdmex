#!/usr/bin/env bash
#
# wowtriage.sh -- run a set of Win16 guests one after another and say, in one line
# each, HOW FAR each one got. GH #128, session 43.
#
#   ./scripts/wowtriage.sh                      # the default app list
#   ./scripts/wowtriage.sh NOTEPAD PBRUSH       # just these
#
# WHY A TRIAGE PASS RATHER THAN A RUN. "Can I test the Windows 3.x apps yet" is not
# a question about one guest, and answering it by reasoning about which APIs each
# one probably needs is exactly the kind of guess this project keeps being wrong
# about. Every app is cheap to launch; the honest answer is the table.
#
# WHAT EACH COLUMN MEANS, and none of them is a verdict on its own:
#   bop      WOW32 calls the guest made -- how much work happened at all
#   uni      calls answered with the harness sentinel: the size of the gap
#   cls/win  classes it registered / windows it created (its OWN, not WOWEXEC's)
#   shown    ShowWindow reached a real HWND -- something was on the desktop
#   end      how the run finished
#
# A guest that registers no class of its own gave up before its WinMain got going;
# one that creates windows and then quits is waiting for input it never gets.
set -uo pipefail
cd "$(dirname "$0")/.."
SH=/private/tmp/xpshare
DIR='C:\WIN16'
ARCHIVE="${ARCHIVE:-build/wowruns}"

APPS=("$@")
if [ ${#APPS[@]} -eq 0 ]; then
  APPS=(NOTEPAD WINMINE CALC CLOCK SOL CARDFILE CHARMAP PBRUSH WRITE TERMINAL PROGMAN WINFILE)
fi

# The frontier needs both switches; a triage pass with them off measures the
# bootstrap and nothing else.
touch "$SH/wowsched.txt" "$SH/wowcall.txt" || exit 2

# ⚠ DEPLOY ONCE, HERE. Every run in the loop below is --no-deploy, which is right
# -- copying the same binary a dozen times is waste -- but the first cut had no
# deploy at ALL, so a whole triage pass measured the PREVIOUS build and reported
# "no change" about a fix that had never reached the box. That is this project's
# oldest trap and it caught this script on its second use.
./scripts/bmwow.sh --deploy-only >/dev/null 2>&1 || {
    cp build/ntvdmhost.exe "$SH/bm/ntvdmhost.exe" || exit 2
    L=$(md5 -q build/ntvdmhost.exe); R=$(md5 -q "$SH/bm/ntvdmhost.exe")
    [ "$L" = "$R" ] || { echo "DEPLOY MISMATCH local=$L share=$R" >&2; exit 2; }
    echo "deployed ntvdmhost.exe  md5=$L"
}

printf '%-10s %5s %5s %5s %5s %6s  %s\n' APP bop uni cls win shown end
printf '%-10s %5s %5s %5s %5s %6s  %s\n' ---------- ----- ----- ----- ----- ------ ---
for a in "${APPS[@]}"; do
  # W1=8s to the screenshot, W2=0 to skip the second: a guest that is going to
  # fail has done it by then, and GetMessage's own wait is 6s.
  TARGET="$DIR\\$a.EXE" WOWWAIT="8 0" TIMEOUT=120 ARCHIVE="$ARCHIVE" \
      ./scripts/bmwow.sh --no-deploy >/dev/null 2>&1
  L=$(ls -t "$ARCHIVE"/*.log | head -1)
  bop=$(grep -c 'WOWBOP 0x51' "$L")
  uni=$(grep -c 'UNIMPLEMENTED, STEPPED OVER' "$L")
  # ITS OWN classes and windows: WOWEXEC always registers two and creates two, so
  # counting all of them would report a dead guest as a working one.
  cls=$(grep -c 'RegisterClass "' "$L")
  win=$(grep -c 'CreateWindow "' "$L")
  cls=$(( cls > 2 ? cls - 2 : 0 ))
  win=$(( win > 2 ? win - 2 : 0 ))
  shown=$(grep -c "the OS's ShowWindow" "$L")
  if   grep -q 'ExitKernelThunk(0x00000000)' "$L"; then end=clean
  elif grep -q '0001:229C'                   "$L"; then end='GP 229C'
  else                                              end='no exit'
  fi
  printf '%-10s %5s %5s %5s %5s %6s  %-8s %s\n' \
         "$a" "$bop" "$uni" "$cls" "$win" "$shown" "$end" "$(basename "$L")"
done
