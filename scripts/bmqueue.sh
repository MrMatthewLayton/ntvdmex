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
TIMEOUT="${TIMEOUT:-240}"

# rt.bat's `doom` arm writes result_doom.log; every other target writes result_<name>.log
case "$TARGET" in
  doom) RESULT="$SH/result_doom.log" ;;
  *)    RESULT="$SH/result_${TARGET}.log" ;;
esac

mtime() { stat -f '%m' "$1" 2>/dev/null || echo 0; }
BEFORE=$(mtime "$RESULT")

# The watcher reads cmd.txt with `for /f ... in ('type cmd.txt')`, so it wants a
# CRLF-terminated line. Write via printf, not a text-mode tool that would strip the CR.
printf '%s %s\r\n' "$TARGET" "$ARGS" > "$SH/cmd.txt"
echo "queued: $TARGET $ARGS   (waiting up to ${TIMEOUT}s for $(basename "$RESULT"))"

# Phase 1: the watcher consumes cmd.txt. If it never does, the watcher is dead.
for ((i=0; i<30; i++)); do
  [ -f "$SH/cmd.txt" ] || break
  sleep 2
done
if [ -f "$SH/cmd.txt" ]; then
  echo "FAILED: watcher never consumed cmd.txt -- watcher is not running" >&2
  exit 2
fi

# Phase 2: wait for a result log NEWER than the moment we queued.
for ((i=0; i<TIMEOUT; i++)); do
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
