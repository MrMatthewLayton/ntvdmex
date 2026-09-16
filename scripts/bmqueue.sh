#!/usr/bin/env bash
#
# bmqueue.sh -- queue a target on the bare-metal XP rig via the SMB watcher and wait
# for it to finish. The watcher (runwatch.bat) polls the share for cmd.txt, runs
# rt.bat with its contents, and writes result_<target>.log back to the share.
#
#   ./scripts/bmqueue.sh selftest.com            # run one target, wait, report
#   ./scripts/bmqueue.sh doom DOOM.EXE           # rt.bat's `doom` arm -> result_doom.log
#   TIMEOUT=180 ./scripts/bmqueue.sh dpmitest.com
#
# WHY WAIT ON THE RESULT LOG'S MTIME rather than on watcher.txt: the watcher rewrites
# watcher.txt every ~3s whether or not a test is running, so its presence is a WEAK
# signal that has misled before. A result log whose mtime moved PAST the moment we
# queued is proof that this run, not a previous one, produced it.
set -uo pipefail
SH=/private/tmp/xpshare
TARGET="${1:?usage: bmqueue.sh <target> [args]}"
shift || true
ARGS="$*"
# ⚠ 240s, and it is not generous. mtime over SMB LAGS: a `setup` that demonstrably
# ran and wrote its log at 17:24:47 was still reported as "never updated" by a 90s
# wait, and that false timeout nearly bought a wrong root cause (I read "the watcher
# is not running rt.bat" off it, when rt.bat had run fine). If this fires, CHECK THE
# FILE'S MTIME BY HAND before concluding anything.
TIMEOUT="${TIMEOUT:-240}"

# s61 moved results out of the share root; s73 laid the share out for release:
#   bin/ dist/ cfg/ debug/{rig,tests,out,prev}/ demo/{msdos,win16}/ + the watcher's
#   control files. Everything the host writes lands in debug/out/. A <target> is a
#   folder under demo/msdos/ first, debug/tests/ second (rt.bat resolves it).
RESULT="$SH/debug/out/result_${TARGET}.log"

mtime() { stat -f '%m' "$1" 2>/dev/null || echo 0; }
# ── ★ REMOVE THE OLD RESULT FIRST, AND WAIT FOR A NEW FILE, NOT A NEW MTIME. (s73) ──
#   macOS's SMB client caches attributes: an mtime change on an EXISTING file can
#   take minutes to show (a whole parity sweep ran at ~4 min/probe on that alone,
#   and this script reported a false TIMEOUT for a run that had finished in 15 s).
#   A file that APPEARS is seen within seconds. So the previous result is deleted
#   here -- which also retires the stale-result hazard the mtime check existed for --
#   and phase 2 waits for the watcher to create a fresh one. If the delete fails the
#   mtime check below still applies.
rm -f "$RESULT" 2>/dev/null
BEFORE=$(mtime "$RESULT")

# The watcher reads cmd.txt with `for /f ... in ('type cmd.txt')`, so it wants a
# CRLF-terminated line. Write via printf, not a text-mode tool that would strip the CR.
# ATOMIC: the watcher polls the share and can see a freshly created, still-empty
# cmd.txt (SMB create and write are two operations). An empty read is "no target",
# the run silently becomes result_none.log, and the queue reports a timeout.
printf '%s %s\r\n' "$TARGET" "$ARGS" > "$SH/debug/ctl/cmd.tmp" && mv "$SH/debug/ctl/cmd.tmp" "$SH/debug/ctl/cmd.txt"
echo "queued: $TARGET $ARGS   (waiting up to ${TIMEOUT}s for $(basename "$RESULT"))"

# Phase 1: the watcher consumes cmd.txt. If it never does, the watcher is dead.
for ((i=0; i<30; i++)); do
  [ -f "$SH/debug/ctl/cmd.txt" ] || break
  sleep 2
done
if [ -f "$SH/debug/ctl/cmd.txt" ]; then
  echo "FAILED: watcher never consumed cmd.txt -- watcher is not running" >&2
  exit 2
fi

# Phase 2: wait for a result log NEWER than the moment we queued.
for ((i=0; i<TIMEOUT; i++)); do
  ls "$SH/debug/out" >/dev/null 2>&1      # a fresh readdir: new entries show up
  NOW=$(mtime "$RESULT")
  if [ "$NOW" != "$BEFORE" ] && [ "$NOW" != "0" ]; then
    sleep 3   # let the copy settle before anyone reads it
    echo "done: $(basename "$RESULT") $(stat -f '%z bytes, %Sm' -t '%H:%M:%S' "$RESULT")"
    # ► ARCHIVE EVERY RESULT. rt.bat/doomrun.bat write the SAME result_<target>.log
    #   every run, so the next run destroys the last one. That cost a 52MB reference
    #   run that could no longer be diffed against. Archives are local (the share is
    #   slow and small) and named by run so two runs can be compared.
    if [ -n "${ARCHIVE:-}" ]; then
      mkdir -p "$ARCHIVE"
      STAMP=$(date +%H%M%S)
      cp "$RESULT" "$ARCHIVE/$(basename "${RESULT%.log}")_$STAMP.log"
      echo "archived: $ARCHIVE/$(basename "${RESULT%.log}")_$STAMP.log"
    fi
    exit 0
  fi
  sleep 1
done
echo "TIMEOUT after ${TIMEOUT}s: $(basename "$RESULT") never updated" >&2
exit 3
