#!/usr/bin/env bash
# dospair.sh <probe.com> -- one DOS probe under STOCK ntvdm and under OURS on the rig,
# stdout redirected the same way on both (scripts/bm/dosstock.bat / dosours.bat), and
# the CASE rows side by side. (s91) Run under scripts/riglock.sh; needs the share.
# ⚠ dosstock.bat drops the IFEO key and restores it -- the state file is checked here.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SH=/private/tmp/xpshare
P="${1:?probe .com}"; B="$(basename "$P" | tr '[:lower:]' '[:upper:]')"
OUT="$ROOT/build/dospair"; mkdir -p "$OUT" "$SH/debug/tests/dos"
cp "$P" "$SH/debug/tests/dos/$B"
for b in dosstock dosours; do awk '{printf "%s\r\n", $0}' "$ROOT/scripts/bm/$b.bat" > "$SH/debug/rig/$b.bat"; done
run() {   # run <bat> -> copies <bat>_out.txt
    rm -f "$SH/debug/out/$1_done.txt"
    printf 'exec cmd /c "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\debug\\rig\\%s.bat" %s\r\n' "$1" "$B" > "$SH/debug/ctl/control.txt"
    for i in $(seq 1 90); do [ -f "$SH/debug/out/$1_done.txt" ] && break; sleep 2; done
    # one output file per probe (s91): a stuck stock run held dosstock_out.txt open and
    # every later stock "result" was the stale file
    cp "$SH/debug/out/$1_${B%.*}.txt" "$OUT/${B%.COM}.$1.txt" 2>/dev/null || : > "$OUT/${B%.COM}.$1.txt"
}
run dosstock
grep -q "restored target EXISTS" "$SH/debug/out/dosstock_state.txt" || { echo "⛔ IFEO NOT PROVEN RESTORED -- see dosstock_state.txt" >&2; cat "$SH/debug/out/dosstock_state.txt"; exit 3; }
run dosours
python3 - "$OUT/${B%.COM}.dosstock.txt" "$OUT/${B%.COM}.dosours.txt" <<'PY'
import sys, re
def rows(p):
    d = {}
    for l in open(p, errors="replace"):
        m = re.match(r"CASE=(\S+) SIG=(\S+) (.*)", l.strip())
        if m:
            regs = dict(kv.split("=") for kv in m.group(3).split() if "=" in kv)
            d[m.group(1)] = " ".join(regs.get(r, "?") for r in m.group(2).split(","))
    return d
s, o = rows(sys.argv[1]), rows(sys.argv[2])
print("%-32s %-10s %-10s %s" % ("case", "stock", "ours", "verdict"))
for k in list(s) + [k for k in o if k not in s]:
    a, b = s.get(k, "-"), o.get(k, "-")
    print("%-32s %-10s %-10s %s" % (k, a, b, "AGREE" if a == b else "MISMATCH"))
PY
