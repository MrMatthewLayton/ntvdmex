#!/usr/bin/env bash
#
# stage-gate.sh -- stage everything the manual VM gate needs into build/ (the TFTP
# root), then print the guest-side steps. Run on the host before gating at the VM.
#
#   ./scripts/stage-gate.sh           # stage the clean host (ntvdmhost.exe)
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

"$ROOT/tools/dostest/make-memtest.sh" >/dev/null
"$ROOT/tools/dostest/make-argtest.sh" >/dev/null
"$ROOT/tools/dostest/make-ioprobe.sh" >/dev/null
cp "$ROOT/tools/dostest/memtest.com"     "$ROOT/build/"
cp "$ROOT/tools/dostest/argtest.com"     "$ROOT/build/"
cp "$ROOT/tools/dostest/ioprobe.com"     "$ROOT/build/"
cp "$ROOT/tools/wowprobe/dosstub.com"    "$ROOT/build/"

host=ntvdmhost.exe; gate=gate-clean.bat; log='C:\ntvdmex\ntvdmhost.log'
[ -f "$ROOT/build/$host" ] || { echo "build/$host missing -- run ./scripts/build.sh" >&2; exit 1; }
cp "$ROOT/tools/dostest/$gate" "$ROOT/build/"

echo "Staged into build/ (TFTP root): $host memtest.com argtest.com ioprobe.com dosstub.com $gate"
echo
echo "Then, in the XP VM (interactive desktop, C:\\ntvdmex must exist):"
echo "  tftp -i 10.0.2.2 GET $gate C:\\ntvdmex\\$gate"
echo "  C:\\ntvdmex\\$gate                       (memtest -> MEMTEST PASS)"
echo "  C:\\ntvdmex\\$gate argtest.com HELLO     (args/env/errorlevel: echoes ' HELLO', exit 6)"
echo "  C:\\ntvdmex\\$gate ioprobe.com           (M3 slice-1b I/O trap: log shows 'IO out/in' lines,"
echo "                                          exit 0x34; if it stops with event=2 paste the dump)"
echo
echo "It points the IFEO Debugger at $host, runs the program in V86, and prints $log."
