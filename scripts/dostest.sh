#!/usr/bin/env bash
#
# dostest.sh -- run one in-guest M2.4 test round against the XP VM with NO reboot.
# Refreshes vdmhost.exe + the target .COM over TFTP, points target.txt at it, then
# triggers the interactive-session agent (vdmtrig.bat) via runwait.bat over a SINGLE
# telnet session and prints the verdict.
#
# Prereqs (one-time): the VM is running (scripts/xp-vm.sh run) and the agent is
# installed -- TFTP-GET install-agent.cmd + vdmtrig.bat + runwait.bat + dosstub.com
# into C:\ntvdmex, run install-agent.cmd (admin), then reboot once. See tools/dostest/.
#
#   ./scripts/dostest.sh                 # round with memtest.com
#   ./scripts/dostest.sh someprog.com    # round with another DOS program staged in build/
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
XP="$ROOT/scripts/xp.py"
PROG="${1:-memtest.com}"

# Stage everything the guest will TFTP-GET into the tftp root (build/).
[ -f "$ROOT/build/vdmhost.exe" ] || { echo "build/vdmhost.exe missing -- run ./scripts/build.sh" >&2; exit 1; }
"$ROOT/tools/dostest/make-memtest.sh" >/dev/null
cp "$ROOT/tools/dostest/memtest.com" "$ROOT/build/"
cp "$ROOT/tools/dostest/runwait.bat" "$ROOT/build/"
[ "$PROG" = memtest.com ] || cp "$ROOT/tools/dostest/$PROG" "$ROOT/build/" 2>/dev/null || true

echo ">> waiting for the guest to be ready..."
"$XP" --wait

echo ">> running round for $PROG (no reboot)..."
out=$(printf '%s\n' \
  "tftp -i 10.0.2.2 GET vdmhost.exe C:\\ntvdmex\\vdmhost.exe" \
  "tftp -i 10.0.2.2 GET $PROG C:\\ntvdmex\\$PROG" \
  "tftp -i 10.0.2.2 GET runwait.bat C:\\ntvdmex\\runwait.bat" \
  "echo C:\\ntvdmex\\$PROG>C:\\ntvdmex\\target.txt" \
  "C:\\ntvdmex\\runwait.bat" \
  | XP_CMD_TIMEOUT=220 "$XP")

echo "$out"
res=$(printf '%s' "$out" | sed -n '/===RESULT-BEGIN===/,/===RESULT-END===/p')
echo "================ VERDICT ================"
if printf '%s' "$res" | grep -q 'MEMTEST PASS'; then
    echo "PASS -- $PROG reported MEMTEST PASS through vdmhost on the real CPU"
elif printf '%s' "$res" | grep -q 'MEMTEST FAIL'; then
    echo "FAIL -- $PROG reported MEMTEST FAIL (vdmhost allocator bug)"; exit 1
else
    echo "INCONCLUSIVE -- no MEMTEST verdict in the result (see output above)"; exit 2
fi
