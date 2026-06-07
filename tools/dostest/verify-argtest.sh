#!/usr/bin/env bash
#
# verify-argtest.sh -- prove argtest.com off the VM under dosbox-x: run it with an
# argument, confirm it echoes the command tail AND exits with errorlevel = tail
# length. Validates the .COM + the args/errorlevel contract independently of the host.
#
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
command -v dosbox-x >/dev/null || { echo "dosbox-x not found on PATH" >&2; exit 2; }

"$DIR/make-argtest.sh"

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
cp "$DIR/argtest.com" "$work/"
conf="$work/dbx.conf"
cat > "$conf" <<EOF
[sdl]
autolock=false
[dosbox]
memsize=16
[autoexec]
mount c $work
c:
argtest.com HELLO > out.txt
if errorlevel 6 if not errorlevel 7 echo RC=PASS>> out.txt
exit
EOF

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy dosbox-x -conf "$conf" -nogui >/dev/null 2>&1 &
dbx=$!
for _ in $(seq 1 30); do kill -0 "$dbx" 2>/dev/null || break; sleep 1; done
kill "$dbx" 2>/dev/null || true; wait "$dbx" 2>/dev/null || true

echo "--- out.txt ---"; cat "$work/out.txt" 2>/dev/null || echo "(none)"; echo "---------------"
# "argtest.com HELLO" -> tail " HELLO" (6 chars): echoes HELLO, errorlevel exactly 6.
if grep -q 'RC=PASS' "$work/out.txt" 2>/dev/null && grep -q 'HELLO' "$work/out.txt" 2>/dev/null; then
    echo "VERIFY: PASS -- argtest echoed the command tail; errorlevel = tail length (6) under dosbox-x"
else
    echo "VERIFY: FAIL -- argtest did not behave as expected" >&2
    exit 1
fi
