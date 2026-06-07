#!/usr/bin/env bash
#
# stage-gate.sh -- stage everything the manual VM gate needs into build/ (the TFTP
# root), then print the guest-side steps. Run on the host before gating at the VM.
#
#   ./scripts/stage-gate.sh           # clean host (ntvdmhost.exe) -- the default
#   ./scripts/stage-gate.sh spike     # the tools/vdmhost spike instead
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WHICH="${1:-clean}"

"$ROOT/tools/dostest/make-memtest.sh" >/dev/null
cp "$ROOT/tools/dostest/memtest.com"     "$ROOT/build/"
cp "$ROOT/tools/wowprobe/dosstub.com"    "$ROOT/build/"

if [ "$WHICH" = "spike" ]; then
    host=vdmhost.exe; gate=gate.bat; log='C:\ntvdmex\vdmhost.log'
else
    host=ntvdmhost.exe; gate=gate-clean.bat; log='C:\ntvdmex\ntvdmhost.log'
fi
[ -f "$ROOT/build/$host" ] || { echo "build/$host missing -- run ./scripts/build.sh" >&2; exit 1; }
cp "$ROOT/tools/dostest/$gate" "$ROOT/build/"

echo "Staged into build/ (TFTP root): $host memtest.com dosstub.com $gate"
echo
echo "Then, in the XP VM (interactive desktop, C:\\ntvdmex must exist):"
echo "  tftp -i 10.0.2.2 GET $gate C:\\ntvdmex\\$gate"
echo "  C:\\ntvdmex\\$gate                 (or:  $gate someprog.com)"
echo
echo "It points the IFEO Debugger at $host, runs the program in V86, and prints $log."
