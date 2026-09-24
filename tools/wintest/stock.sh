#!/usr/bin/env bash
#
# stock.sh -- run a Win16 probe under STOCK ntvdm and diff it against ours.
#
#   ./tools/wintest/stock.sh w_kernel
#
# A value tools/wintest/run.sh produces is OURS. This asks the documented
# authority for everything WOW -- stock ntvdm on the same box -- and prints the
# two side by side. Only after this is a probe row a verdict rather than a
# measurement.
#
# ⛔⛔⛔ THIS DROPS THE IFEO DEBUGGER VALUE so the launch routes to stock. The
#   batch restores it on every exit path, and THIS SCRIPT REFUSES TO EXIT QUIETLY
#   unless the collected state file proves it came back and points at a binary
#   that exists. A key left absent turns every later test into a stock run with
#   entirely plausible-looking logs; a key pointing at a missing binary bricks
#   the box for DOS and Win16 alike.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SH=/private/tmp/xpshare
NAME="${1:?usage: stock.sh <probe name, e.g. w_kernel>}"
mod="$(printf '%s' "${NAME#w_}" | tr '[:lower:]' '[:upper:]' | cut -c1-8)"
FOLD="w16$(printf '%s' "$mod" | tr '[:upper:]' '[:lower:]')"
EXE="$ROOT/build/wintest/W16${mod}.EXE"
OURS="$ROOT/build/wintest/${NAME}.ours.txt"
STOCK="$ROOT/build/wintest/${NAME}.stock.txt"

[ -f "$EXE" ] || { echo "no $EXE -- run tools/wintest/build.sh first" >&2; exit 2; }
[ -d "$SH" ]  || { echo "share not mounted at $SH" >&2; exit 2; }

# ── 1. ours, through the IFEO hook ──────────────────────────────────────────
"$ROOT/tools/wintest/run.sh" "$NAME" > "$OURS" 2>&1 || {
    echo "our own run failed -- see $OURS" >&2; exit 2; }

# ── 2. stock, with the key dropped ──────────────────────────────────────────
awk '{printf "%s\r\n", $0}' "$ROOT/scripts/bm/w16stock.bat" > "$SH/debug/rig/w16stock.bat"
rm -f "$SH/w16stock_done.txt" "$SH/debug/out/w16stock_state.txt" "$SH/debug/out/W16OUT.TXT"

printf 'exec cmd /c "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\debug\\rig\\w16stock.bat" %s W16%s.EXE\r\n' \
    "$FOLD" "$mod" > "$SH/debug/ctl/control.txt"
echo "queued the STOCK run"
for i in $(seq 1 40); do [ -f "$SH/debug/ctl/control.txt" ] || break; sleep 2; done
for i in $(seq 1 40); do [ -f "$SH/w16stock_done.txt" ] && break; sleep 3; done

# ── 3. the key, before anything else is believed ────────────────────────────
state="$SH/debug/out/w16stock_state.txt"
if ! grep -q "IFEO after" "$state" 2>/dev/null; then
    echo >&2
    echo "⛔⛔⛔ THE STOCK RUN DID NOT REACH ITS RESTORE. The IFEO Debugger value" >&2
    echo "     may be ABSENT, which makes every later test on this box a STOCK run" >&2
    echo "     with plausible-looking logs. Repair it by hand:" >&2
    echo >&2
    echo '     reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "\"C:\Documents and Settings\All Users\Documents\ntvdmex\bin\ntvdmhost.exe\"" /f' >&2
    exit 2
fi
if grep -q "RESTORED TARGET IS MISSING" "$state"; then
    echo "⛔⛔⛔ the IFEO key was restored but points at a binary that does not exist" >&2
    exit 2
fi
echo "IFEO key restored and its target exists."

cp "$SH/debug/out/W16OUT.TXT" "$STOCK" 2>/dev/null || {
    echo "stock produced no output -- see $state" >&2; exit 1; }

# ── 4. the comparison ───────────────────────────────────────────────────────
echo
printf '%-34s %-10s %-10s %s\n' "case" "ours" "stock" "verdict"
printf '%-34s %-10s %-10s %s\n' "----" "----" "-----" "-------"
grep '^CASE=' "$OURS" | while read -r line; do
    case_name="$(printf '%s' "$line" | sed 's/^CASE=\([^ ]*\).*/\1/')"
    ours_v="$(printf  '%s' "$line" | sed 's/.*AX=//')"
    stock_line="$(grep "^CASE=$case_name " "$STOCK" 2>/dev/null | head -1)"
    if [ -z "$stock_line" ]; then
        printf '%-34s %-10s %-10s %s\n' "$case_name" "$ours_v" "--" "NO-DATA"
        continue
    fi
    stock_v="$(printf '%s' "$stock_line" | sed 's/.*AX=//')"
    if [ "$ours_v" = "$stock_v" ]; then v=AGREE; else v=MISMATCH; fi
    printf '%-34s %-10s %-10s %s\n' "$case_name" "$ours_v" "$stock_v" "$v"
done
echo
echo "ours:  $OURS"
echo "stock: $STOCK"
