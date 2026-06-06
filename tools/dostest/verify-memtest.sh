#!/usr/bin/env bash
#
# verify-memtest.sh -- prove memtest.com is correct OFF the XP VM, by running it
# under dosbox-x (a correct DOS allocator) and asserting errorlevel 0 + the PASS
# banner. This validates the hand-assembled .COM and its self-checks independently
# of vdmhost, so a later vdmhost run isolates *our* allocator behaviour.
#
# Requires dosbox-x on PATH. Exits nonzero if the verdict isn't a clean PASS.
#
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

command -v dosbox-x >/dev/null || { echo "dosbox-x not found on PATH" >&2; exit 2; }

"$DIR/make-memtest.sh"

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
cp "$DIR/memtest.com" "$work/"
conf="$work/dbx.conf"
cat > "$conf" <<EOF
[sdl]
autolock=false
[dosbox]
memsize=16
[autoexec]
mount c $work
c:
memtest.com > out.txt
if errorlevel 1 echo RESULT=FAIL>> out.txt
if not errorlevel 1 echo RESULT=PASS>> out.txt
exit
EOF

# Run headless; dosbox-x exits itself via the autoexec 'exit'. Guard against a
# hang (no 'timeout' on stock macOS) by killing it if it overstays.
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy dosbox-x -conf "$conf" -nogui >/dev/null 2>&1 &
dbx=$!
for _ in $(seq 1 30); do kill -0 "$dbx" 2>/dev/null || break; sleep 1; done
kill "$dbx" 2>/dev/null || true; wait "$dbx" 2>/dev/null || true

echo "--- dosbox-x out.txt ---"
cat "$work/out.txt" 2>/dev/null || echo "(no out.txt produced)"
echo "------------------------"
if grep -q 'RESULT=PASS' "$work/out.txt" 2>/dev/null \
   && grep -q 'MEMTEST PASS' "$work/out.txt" 2>/dev/null; then
    echo "VERIFY: PASS -- memtest.com prints PASS and exits errorlevel 0 under dosbox-x"
else
    echo "VERIFY: FAIL -- memtest.com did not report a clean pass" >&2
    exit 1
fi
