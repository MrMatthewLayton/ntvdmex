#!/usr/bin/env bash
#
# bmstage.sh -- lay the rig harness out on the share FROM THE REPO. (s73)
#
#   ./scripts/bmstage.sh              # harness only: debug/rig/ from scripts/bm/ + build/
#   ./scripts/bmstage.sh --host       # ALSO replace bin/ntvdmhost.exe with build/ntvdmhost.exe
#   ./scripts/bmstage.sh --check      # report what differs, change nothing
#
# THE SHARE LAYOUT (also what gets copied to a USB for a release):
#   bin/          the working host binary                 <- build/ntvdmhost.exe (--host only)
#   dist/         the installable zips                    <- scripts/package.sh, by hand
#   cfg/          everything the host READS               <- never touched here
#   debug/rig/    the harness: rt.bat runwatch.bat ...    <- scripts/bm/ (LIVE set), build/*.exe
#   debug/tests/  DOS test programs (Probe/ Argtest/ dos/) <- never touched here
#   debug/out/    everything the host WRITES              <- never touched here
#   debug/prev/   rollback host builds                    <- --host moves the old bin/ exe here
#   debug/ctl/    the watcher's control channel           <- cmd.txt watcher.txt control.txt controld.txt rigshot.txt
#   demo/msdos/   the user's games and demos              <- never touched here
#   demo/win16/   Win 3.11 apps                           <- never touched here
#
# ⛔ THE HOST IS NOT REPLACED BY DEFAULT: a harness fix must not swap it out as a side
#    effect. --host archives the displaced exe as debug/prev/ntvdmhost_<md5>.exe, then
#    deploys, then md5s BOTH sides. debug/prev/ntvdmhost_prev.exe = the last build a HUMAN
#    confirmed; promote to it by hand, never here -- an SMB copy that "succeeded" has
#    silently delivered a stale file before.
# ⚠ ONLY THE LIVE SET OF BATCH FILES IS STAGED. scripts/bm/ also holds pre-s61 scripts
#    (doomrun, menushot, setshot, sxs, zar*, stockdump ...) that copy the host to
#    C:\ntvdmex and point the IFEO key there -- exactly the litter the user wiped the
#    box over, followed by "no DOS app runs". They stay in the repo for reading only.
# ⚠ .bat files are written CRLF: cmd.exe mis-parses LF-only labels and `goto`, which
#    has taken the watcher down before.
# ⚠ rt.bat / runwatch.bat changes on the share do not take effect until the watcher
#    restarts (a `cmd /c` in flight holds the old one) -- `bmqueue.sh reboot`.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SH=/private/tmp/xpshare
MODE=harness
case "${1:-}" in
  --host)  MODE=host ;;
  --check) MODE=check ;;
  "")      ;;
  *) echo "usage: bmstage.sh [--host|--check]" >&2; exit 1 ;;
esac

[ -d "$SH/cfg" ] || { echo "share not mounted at $SH (mount_smbfs -N //guest@192.168.1.29/ntvdmex $SH)" >&2; exit 2; }

# The LIVE harness: every script here uses the s73 layout and writes nothing to C:.
LIVE_BATS=(rt.bat runwatch.bat rt_stock.bat ifeochk.bat restore.bat
           lemhp.bat lemlive.bat qbclick.bat qbopen.bat pkgtest.bat pkgsmoke.bat
           evt.bat cpuinfo.bat w16launch.bat w16watch.bat)
# Tools that are rebuilt on the Mac; staged only when a build exists.
LIVE_EXES=(vdmwatch.exe controld_v2.exe rigshot.exe vdmdump.exe present_demo.exe)

RIG="$SH/debug/rig"
changed=0
stage_text() {   # $1 = src (LF or CRLF), $2 = dst; normalises to CRLF, reports a change
  local tmp="$TMPDIR/stage.$$"
  perl -pe 's/\r?\n/\r\n/' "$1" > "$tmp"
  if [ -f "$2" ] && cmp -s "$tmp" "$2"; then rm -f "$tmp"; return 0; fi
  changed=1
  if [ "$MODE" = check ]; then echo "  differs: $(basename "$2")"; rm -f "$tmp"; return 0; fi
  cp "$tmp" "$2" && rm -f "$tmp" && echo "  staged:  $(basename "$2")"
}
stage_bin() {    # $1 = src, $2 = dst; md5 both sides
  if [ -f "$2" ] && [ "$(md5 -q "$1")" = "$(md5 -q "$2")" ]; then return 0; fi
  changed=1
  if [ "$MODE" = check ]; then echo "  differs: $(basename "$2")"; return 0; fi
  cp "$1" "$2" || { echo "FAILED copying $(basename "$2")" >&2; exit 3; }
  L=$(md5 -q "$1"); R=$(md5 -q "$2")
  [ "$L" = "$R" ] || { echo "MD5 MISMATCH after copy: $(basename "$2") local=$L remote=$R" >&2; exit 3; }
  echo "  staged:  $(basename "$2") ($L)"
}

[ "$MODE" = check ] || mkdir -p "$RIG" "$SH/debug/out" "$SH/debug/tests" "$SH/debug/prev" "$SH/debug/ctl"
echo "harness -> debug/rig/"
for b in "${LIVE_BATS[@]}"; do
  [ -f "$ROOT/scripts/bm/$b" ] || { echo "  MISSING in repo: scripts/bm/$b" >&2; continue; }
  stage_text "$ROOT/scripts/bm/$b" "$RIG/$b"
done
for e in "${LIVE_EXES[@]}"; do
  [ -f "$ROOT/build/$e" ] && stage_bin "$ROOT/build/$e" "$RIG/$e"
done
# controld_v2.exe is what runwatch.bat hot-swaps into controld.exe at the next start.
[ -f "$ROOT/build/controld.exe" ] && stage_bin "$ROOT/build/controld.exe" "$RIG/controld_v2.exe"

if [ "$MODE" = host ]; then
  HOST="$ROOT/build/ntvdmhost.exe"
  [ -f "$HOST" ] || { echo "build/ntvdmhost.exe missing" >&2; exit 1; }
  size=$(stat -f '%z' "$HOST")
  [ "$size" -gt 1000000 ] || { echo "build/ntvdmhost.exe is $size bytes: that is the LAUNCHER, not the host" >&2; exit 1; }
  mkdir -p "$SH/bin"
  # The displaced exe is kept BY HASH. debug/prev/ntvdmhost_prev.exe is the last build a
  # HUMAN confirmed and only a human promotes to it -- this script cannot know whether
  # the exe it is replacing was ever seen, and it usually was not.
  if [ -f "$SH/bin/ntvdmhost.exe" ] && [ "$(md5 -q "$HOST")" != "$(md5 -q "$SH/bin/ntvdmhost.exe")" ]; then
    old=$(md5 -q "$SH/bin/ntvdmhost.exe"); keep="$SH/debug/prev/ntvdmhost_${old:0:8}.exe"
    [ -f "$keep" ] || cp "$SH/bin/ntvdmhost.exe" "$keep"
    echo "displaced -> debug/prev/$(basename "$keep")   (ntvdmhost_prev.exe = $(md5 -q "$SH/debug/prev/ntvdmhost_prev.exe" 2>/dev/null | cut -c1-8), the confirmed one, untouched)"
  fi
  echo "host -> bin/"
  stage_bin "$HOST" "$SH/bin/ntvdmhost.exe"
else
  if [ -f "$SH/bin/ntvdmhost.exe" ]; then
    L=$(md5 -q "$ROOT/build/ntvdmhost.exe" 2>/dev/null || echo none); R=$(md5 -q "$SH/bin/ntvdmhost.exe")
    if [ "$L" = "$R" ]; then echo "host: bin/ntvdmhost.exe == build ($R)"
    else echo "host: bin/ntvdmhost.exe is $R, build is $L -- NOT replaced (use --host)"; fi
  fi
fi

# controld.exe itself is RUNNING on the box and cannot be overwritten; runwatch.bat
# copies controld_v2.exe over it at its next start. rigshot.exe is one-shot: safe.
if [ -f "$SH/wowquiet.txt" ] || [ -f "$SH/cfg/wowquiet.txt" ]; then
  echo "⚠ wowquiet.txt is on the share: the log is silenced and the guest runs FASTER than shipped" >&2
fi
[ "$MODE" = check ] && { [ $changed = 0 ] && echo "share matches the repo" || echo "share differs from the repo"; }
exit 0
