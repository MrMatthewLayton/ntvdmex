#!/usr/bin/env bash
#
# gate.sh -- run Win16 probes on the rig and grade each against its STOCK baseline
#            (tools/wintest/stock/<probe>.txt). (s92)
#
#   ./tools/wintest/gate.sh                 # every probe that has a stock baseline
#   ./tools/wintest/gate.sh w_mdi w_msgs    # just these
#
# Prints one line per probe: "w_mdi 13/13" or "w_mdi 7/13 <first differing CASE>".
# A CASE is compared as its whole line; a case missing from our run is a difference.
# Run under scripts/riglock.sh, outside the command sandbox (it writes to the share).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/runs/wgate"
mkdir -p "$OUT"
if [ $# -gt 0 ]; then PROBES="$*"; else
    PROBES=$(cd "$ROOT/tools/wintest/stock" && ls w_*.txt | sed 's/\.txt$//'); fi
for p in $PROBES; do
    st="$ROOT/tools/wintest/stock/$p.txt"
    [ -f "$st" ] || { echo "$p -- no stock baseline"; continue; }
    "$ROOT/tools/wintest/run.sh" "$p" > "$OUT/$p.txt" 2>&1
    python3 - "$st" "$OUT/$p.txt" "$p" <<'EOF'
import sys
st, ours, name = sys.argv[1:4]
def cases(path):
    d = {}
    for ln in open(path, errors="replace"):
        ln = ln.strip()
        if ln.startswith("CASE="):
            d[ln.split()[0]] = ln
    return d
s, o = cases(st), cases(ours)
bad = [k for k in s if o.get(k) != s[k]]
first = (" first: " + bad[0] + " stock[" + s[bad[0]] + "] ours[" + o.get(bad[0], "MISSING") + "]") if bad else ""
print("%s %d/%d%s" % (name, len(s) - len(bad), len(s), first))
EOF
done
