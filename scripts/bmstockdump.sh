#!/usr/bin/env bash
#
# bmstockdump.sh -- dump a live STOCK ntvdm VDM on the bare-metal rig.  GH #128.
#
#   ./scripts/bmstockdump.sh
#
# Builds vdmdump.exe, cuts fresh needles from guest/ne/krnl386.exe, deploys both,
# and drives scripts/bm/stockdump.bat through controld. Comes back with
# build/stockdumps/<time>/{stockdump.txt,.bin,.blk,con,state}.
#
# ⚠ THE HAZARD IS THE IFEO KEY. stockdump.bat removes it so SYSEDIT routes to
#   stock ntvdm, and puts it back at the end. If it is left absent, every later
#   test on this box silently measures stock ntvdm and the logs look fine. This
#   script therefore FAILS LOUDLY unless the collected state file proves the key
#   is back, and tells you the one command that repairs it.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SH=/private/tmp/xpshare
TIMEOUT="${TIMEOUT:-120}"
KRNL="${KRNL:-$ROOT/guest/ne/krnl386.exe}"

[ -d "$SH" ] || { echo "share not mounted at $SH" >&2; exit 2; }

"$ROOT/scripts/build-vdmdump.sh" >/dev/null || exit 2
"$ROOT/scripts/check-imports.sh" "$ROOT/build/vdmdump.exe" >/dev/null || exit 2
python3 "$ROOT/tools/ne/needles.py" "$KRNL" > "$ROOT/build/needles.txt" || exit 2
echo "needles: $(grep -vc '^#' "$ROOT/build/needles.txt") from $(basename "$KRNL")"

# Deploy, checksummed -- copying the wrong file has cost more than one session.
cp "$ROOT/build/vdmdump.exe" "$SH/bm/vdmdump.exe" || exit 2
L=$(md5 -q "$ROOT/build/vdmdump.exe"); R=$(md5 -q "$SH/bm/vdmdump.exe")
[ "$L" = "$R" ] || { echo "DEPLOY MISMATCH local=$L share=$R" >&2; exit 2; }
echo "deployed vdmdump.exe  md5=$L"
awk '{printf "%s\r\n", $0}' "$ROOT/build/needles.txt"        > "$SH/bm/needles.txt"
awk '{printf "%s\r\n", $0}' "$ROOT/scripts/bm/stockdump.bat" > "$SH/stockdump.bat"

# Delete the destinations BEFORE the run: an absent artefact is a loud failure,
# a stale one is a silent wrong answer.
rm -f "$SH"/stockdump.txt "$SH"/stockdump.bin "$SH"/stockdump.blk \
      "$SH"/stockdump_con.txt "$SH"/stockdump_state.txt "$SH"/stockdump_done.txt

printf 'exec cmd /c "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\stockdump.bat"\r\n' \
  > "$SH/control.txt"
echo "queued via controld; waiting up to ${TIMEOUT}s"

for ((i=0; i<30; i++)); do [ -f "$SH/control.txt" ] || break; sleep 2; done
if [ -f "$SH/control.txt" ]; then
  echo "FAILED: controld never consumed control.txt -- daemon is not running" >&2
  echo "  heartbeat: $(cat "$SH/controld.txt" 2>/dev/null)" >&2
  exit 2
fi

got=0
for ((i=0; i<TIMEOUT; i++)); do
  [ -f "$SH/stockdump_done.txt" ] && { got=1; break; }
  sleep 1
done
sleep 3                                            # let the copies settle

STAMP=$(date +%H%M%S)
DEST="$ROOT/build/stockdumps/$STAMP"
mkdir -p "$DEST"
for f in stockdump.txt stockdump.bin stockdump.blk stockdump_con.txt stockdump_state.txt; do
  [ -f "$SH/$f" ] && cp "$SH/$f" "$DEST/$f"
done
echo "collected into $DEST:"
ls -l "$DEST" | tail -n +2

# --- the IFEO check. Do this LAST and loudly, whatever else happened. ---------
STATE="$DEST/stockdump_state.txt"
if [ -f "$STATE" ] && awk '/IFEO after/{a=1} a && /ntvdmhost\.exe/{f=1} END{exit !f}' "$STATE"; then
  echo "IFEO Debugger verified RESTORED"
else
  echo >&2
  if [ ! -f "$STATE" ]; then
    echo "*** NO STATE FILE -- the batch may never have started (key probably untouched)," >&2
    echo "    but this cannot be proven from here. VERIFY THE KEY BEFORE THE NEXT RUN. ***" >&2
  else
    echo "*** IFEO DEBUGGER NOT PROVEN RESTORED -- every later test would measure STOCK ***" >&2
  fi
  echo "    repair it on the rig with:" >&2
  echo '    reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f' >&2
  exit 4
fi

[ "$got" = 1 ] || { echo "TIMEOUT after ${TIMEOUT}s: stockdump_done.txt never appeared" >&2; exit 3; }
exit 0
