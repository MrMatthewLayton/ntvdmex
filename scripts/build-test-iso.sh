#!/usr/bin/env bash
#
# build-test-iso.sh -- build a complete NTVDMEX test CD for the XP VM.
#
# Bundles the host (ntvdmhost.exe) plus everything needed to run the full test
# suite on the real CPU in one disc:
#   selftest.bat / selftest.com  -- the one-shot regression suite (all subsystems,
#                                   PASS/FAIL report; this is the regression gate)
#   runtest-demos.bat            -- the per-program menu (graphics demos + tests)
#   dosstub.com                  -- the 16-bit trigger that brings up the host
#   *.com test programs          -- xmstest/emstest/timertst/mousetst/blitfast
#   demos/*.EXE                  -- the QuickBASIC visual demos
#
# The volume label is made unique each build (NTVDM<HHMMSS>) because XP caches a
# CD's contents by label -- a stale label silently serves the previous disc.
#
# Usage:  ./scripts/build-test-iso.sh            # build host + assemble + stage ISO
#         ./scripts/build-test-iso.sh --no-build # skip the cmake build (reuse build/)
# Output: /tmp/ntvdmex-test.iso  (+ the qmp hot-swap command to mount it)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
DT="$ROOT/tools/dostest"
ISO="/tmp/ntvdmex-test.iso"
STAGE="/tmp/ntvdmex_cd"
LABEL="NTVDM$(date +%H%M%S)"

if [[ "${1:-}" != "--no-build" ]]; then
    cmake --build "$BUILD" --target ntvdmhost
fi
"$DT/make-demos.sh"

rm -rf "$STAGE"; mkdir -p "$STAGE"

# host + trigger + runners
cp "$BUILD/ntvdmhost.exe" "$STAGE/"
cp "$BUILD/dosstub.com"   "$STAGE/"
cp "$DT/selftest.bat" "$DT/selftest.com" "$STAGE/"
cp "$DT/modeswitch.bat" "$DT/modeswitch.com" "$STAGE/"   # GH#14 mode-switch regression test
cp "$DT/dpmitest.bat" "$DT/dpmitest.com" "$STAGE/"       # GH#1 DPMI 16-bit switch spike
cp "$DT/dpmiexe.exe" "$STAGE/"                           # GH#2 multi-segment MZ .EXE DPMI client
cp "$DT/i310102.exe" "$STAGE/"                           # GH#2 C-runtime client (host PM interpreter, runs 51+)
cp "$DT/dpmiback.com" "$STAGE/"                          # GH#2 third-party DPMI client (Japheth, JWasm-built)
cp "$DT/dpmiauto.bat" "$DT/dpmiinstall.bat" "$STAGE/"    # headless DPMI autorun harness
cp "$DT/i31run.bat" "$STAGE/"                            # one-shot i310102 runner (run 56 confirm)
cp "$DT/dpbrun.bat" "$STAGE/"                            # one-shot dpmiback runner (interpreter-path probe)
cp "$DT/rawjmp7.exe" "$DT/rjrun.bat" "$STAGE/"          # HX Regress16 raw-mode-switch client + runner
cp "$DT/runtest-demos.bat" "$STAGE/"
# individual test programs
cp "$DT/xmstest.com" "$DT/emstest.com" "$DT/timertst.com" \
   "$DT/mousetst.com" "$DT/blitfast.com" "$STAGE/"
# the QuickBASIC visual demos (corpus is untracked; skip if absent)
if compgen -G "$ROOT/demos/*.EXE" >/dev/null; then
    cp "$ROOT"/demos/*.EXE "$STAGE/"
fi

rm -f "$ISO"
hdiutil makehybrid -iso -joliet -default-volume-name "$LABEL" -o "$ISO" "$STAGE" >/dev/null

echo "Built $ISO   (volume label $LABEL)"
echo "Staged:"; ls "$STAGE" | sed 's/^/  /'
echo
echo "Mount it in the running VM with:"
echo "  python3 $ROOT/scripts/qmp.py cd $ISO"
echo "Then in the XP desktop run:  D:\\selftest.bat   (the regression gate)"
