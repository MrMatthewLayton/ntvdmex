#!/usr/bin/env bash
#
# run.sh -- stage a Win16 probe on the bare-metal rig, launch it through the
#           IFEO hook, and print what it wrote.
#
#   ./tools/wintest/run.sh w_kernel
#
# ⚠ WRITES TO THE SHARE, so it needs to run outside the command sandbox.
# ⚠ DELETES THE OUTPUT FIRST. An absent artefact is a loud failure; a stale one
#   is a silent wrong answer, and this harness has produced exactly that shape
#   before (see scripts/bmwow.sh's note).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SH=/private/tmp/xpshare
NAME="${1:?usage: run.sh <probe name, e.g. w_kernel>}"
mod="$(printf '%s' "${NAME#w_}" | tr '[:lower:]' '[:upper:]' | cut -c1-8)"
EXE="$ROOT/build/wintest/W16${mod}.EXE"
# ⚠ NOT ${mod,,} -- macOS ships bash 3.2 and that is a bash 4 feature. It fails
#   as "bad substitution" and then `set -u` reports the NEXT line instead, which
#   points at the wrong place entirely.
FOLD="w16$(printf '%s' "$mod" | tr '[:upper:]' '[:lower:]')"

[ -f "$EXE" ] || { echo "no $EXE -- run tools/wintest/build.sh first" >&2; exit 2; }
[ -d "$SH" ]  || { echo "share not mounted at $SH" >&2; exit 2; }

# The probe writes here. Its path is baked into the .asm -- see the note there on
# why a RELATIVE name does not land in the launch directory.
OUTFILE="$SH/debug/out/W16OUT.TXT"
rm -f "$OUTFILE" "$SH/debug/out/result_w16_${FOLD}.log"

mkdir -p "$SH/demo/win16/$FOLD"
cp "$EXE" "$SH/demo/win16/$FOLD/W16${mod}.EXE"

printf 'exec cmd /c "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\debug\\rig\\w16launch.bat" %s W16%s.EXE\r\n' \
    "$FOLD" "$mod" > "$SH/debug/ctl/control.txt"
echo "queued $FOLD/W16${mod}.EXE"

for i in $(seq 1 40); do [ -f "$SH/debug/ctl/control.txt" ] || break; sleep 2; done
if [ -f "$SH/debug/ctl/control.txt" ]; then
    echo "FAILED: controld never consumed control.txt -- is the daemon running?" >&2
    echo "  heartbeat: $(cat "$SH/debug/ctl/controld.txt" 2>/dev/null)" >&2
    exit 2
fi
for i in $(seq 1 40); do
    grep -q "stopped at\|left running" "$SH/debug/out/result_w16_${FOLD}.log" 2>/dev/null && break
    sleep 3
done

echo
if [ -f "$OUTFILE" ]; then
    cat "$OUTFILE"
else
    # ⛔ An absence means nothing on its own -- say where to look next.
    echo "NO OUTPUT FILE."
    echo "  The probe either never reached its INT 21h AH=3Ch, or the create failed."
    echo "  The host log names which: grep for 'INT21h AH=0000003c' in"
    echo "  $SH/debug/out/result_w16_${FOLD}_host.log"
    exit 1
fi
