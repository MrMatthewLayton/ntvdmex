#!/usr/bin/env bash
# riglock.sh -- run a command while holding THE rig/oracle lock. (s87)
#
# One thing at a time may touch the rig (bin\, the watcher, controld) or run the
# oracles (dosdiff.py: msdos622 / dosbox-x / pcem / ntvdmex). Two dosdiffs at once
# cross-contaminated PCem and the cache kept the wrong answer (s84), and the rig's
# `ntvdmex` host runs whatever is in bin\ at that moment -- so deploying a build and
# running the probe against it must happen under ONE lock, in ONE call:
#
#   scripts/riglock.sh bash -c 'cp build/ntvdmhost.exe /private/tmp/xpshare/bin/ && \
#                                python3 scripts/dosdiff.py ... --host ntvdmex'
#
# A lock older than RIGLOCK_STALE seconds (default 45 min) is taken over.
set -u
LOCK=${RIGLOCK_DIR:-/private/tmp/claude/ntvdmex-rig.lock}
STALE=${RIGLOCK_STALE:-2700}
mkdir -p "$(dirname "$LOCK")"
waited=0
while ! mkdir "$LOCK" 2>/dev/null; do
    if [ -f "$LOCK/since" ]; then
        age=$(( $(date +%s) - $(cat "$LOCK/since" 2>/dev/null || echo 0) ))
        if [ "$age" -gt "$STALE" ]; then
            echo "riglock: taking over a stale lock (${age}s, held by: $(cat "$LOCK/who" 2>/dev/null))" >&2
            rm -rf "$LOCK"; continue
        fi
    fi
    if [ $((waited % 60)) -eq 0 ]; then
        echo "riglock: waiting (held by: $(cat "$LOCK/who" 2>/dev/null))" >&2
    fi
    sleep 5; waited=$((waited + 5))
done
date +%s > "$LOCK/since"
echo "${RIGLOCK_WHO:-pid $$} :: $*" > "$LOCK/who"
trap 'rm -rf "$LOCK"' EXIT INT TERM
"$@"
