#!/usr/bin/env bash
#
# package.sh -- assemble the portable NTVDMEX zip (the 17th deliverable).
#
#   ./scripts/package.sh            # uses build/ntvdmhost.exe as built
#   ./scripts/package.sh --build    # rebuild first
#
# Produces dist/ntvdmex-<date>-<sha>.zip -- or, for a release, dist/ntvdmex-<version>.zip
# when NTVDMEX_VERSION is set (the release workflow sets it from the tag) -- laid out so
# that extracting it ANYWHERE
# on an XP box and running install.bat is the whole install:
#
#   install.bat uninstall.bat status.bat smoke.bat README.txt
#   bin\ntvdmhost.exe  bin\ntvdmex.exe (the manager, #281)  bin\selftest.com
#   cfg\  debug\out\        (empty; the host creates them anyway)
#
# No Win16 system files are bundled: the WOW half runs XP's OWN krnl386/gdi/user
# from system32 (the -a argument of the WOW launch names them), which every XP
# has. guest/wow in the repo is a copy of those for reading, not for shipping.
#
# Text files are written CRLF: cmd.exe mis-parses LF-only batch files (labels and
# goto break), which took the rig's watcher down once. The host binary is checked
# to be the HOST (~1.7 MB), not the 20 KB stale launcher build/ntvdmex.exe.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [[ "${1:-}" == "--build" ]]; then ./scripts/build.sh; fi

HOST="$ROOT/build/ntvdmhost.exe"
[ -f "$HOST" ] || { echo "build/ntvdmhost.exe missing -- run ./scripts/build.sh" >&2; exit 1; }
size=$(wc -c < "$HOST" | tr -d ' ')
[ "$size" -gt 1000000 ] || { echo "build/ntvdmhost.exe is $size bytes: that is the launcher, not the host" >&2; exit 1; }

sha=$(git rev-parse --short HEAD)
date=$(date +%Y%m%d)
build_id="ntvdmex-$date-$sha"
name="${NTVDMEX_VERSION:+ntvdmex-$NTVDMEX_VERSION}"
name="${name:-$build_id}"

# md5 on macOS, md5sum on Linux.
md5_of() { if command -v md5 >/dev/null 2>&1; then md5 -q "$1"; else md5sum "$1" | cut -d' ' -f1; fi; }
stage="$ROOT/dist/$name"
rm -rf "$stage"
mkdir -p "$stage/bin" "$stage/cfg" "$stage/debug/out"

cp "$HOST" "$stage/bin/ntvdmhost.exe"
# GH #281: the manager -- one tray icon for every program. It must BE the manager:
# build/ntvdmex.exe used to be the retired milestone-0 shell preview, same name.
MGR="$ROOT/build/ntvdmex.exe"
grep -q "NTVDMEX_Manager" "$MGR" 2>/dev/null || { echo "build/ntvdmex.exe is not the manager (stale M0 preview?) -- rebuild" >&2; exit 1; }
cp "$MGR" "$stage/bin/ntvdmex.exe"
SELFTEST="$ROOT/build/probes/selftest.com"   # built by CMake from tests/probes/dos/selftest.asm
[ -f "$SELFTEST" ] || { echo "build/probes/selftest.com missing -- nasm needed; run ./scripts/build.sh" >&2; exit 1; }
cp "$SELFTEST" "$stage/bin/selftest.com"
# s90 (#278): the WOW32.DLL / NTVDM.EXE stand-ins a 32-bit thunk DLL looks up by name
# (winmm does: Sound Recorder and Media Player need them). The host loads them from
# bin\wowshim\ by full path; without them Win16 multimedia reports no devices.
mkdir -p "$stage/bin/wowshim"
for f in WOW32.DLL NTVDM.EXE; do
    [ -f "$ROOT/build/wowshim/$f" ] || { echo "build/wowshim/$f missing -- rebuild" >&2; exit 1; }
    cp "$ROOT/build/wowshim/$f" "$stage/bin/wowshim/$f"
done
for f in install.bat uninstall.bat status.bat smoke.bat diag.bat README.txt; do
    perl -pe 's/\r?\n/\r\n/' "$ROOT/package/$f" > "$stage/$f"
done
printf 'settings files go here; empty = defaults\r\n' > "$stage/cfg/README.txt"
# ⚠⚠ THE WIN16 HALF IS FOUR cfg\ FILES, AND EVERY CONFIRMED RESULT HAD ALL FOUR.
#   They were filed as opt-in experiments in s38-s43 and never promoted; the s61 wipe
#   lost them and s58 + s73 both rediscovered the symptom -- a "16-bit Windows not
#   supported" box, or a guest that quits after ~6 s in GetMessage. A zip that
#   promises Notepad and Paint must ship them (existence-gated except wowidle):
#     wowtry.flag    the WOW opt-in -- absent = every Win16 launch is REFUSED
#     wowsched.txt   the host-side Win16 task scheduler (src/wow/wowsched.h)
#     wowcall.txt    the host calls 16-bit code (CreateWindow -> WM_CREATE etc.)
#     wowidle.txt=0  a task blocked in GetMessage waits FOREVER, as real Windows does;
#                    absent = WOWMSG_WAIT_MS, a harness bound that kills an interactive app
printf 'WOW opt-in: with this file present NTVDMEX runs 16-bit Windows programs; without it they are refused.\r\n' > "$stage/cfg/wowtry.flag"
printf 'Win16 task scheduler ON (existence-gated). Needed for any Win16 program.\r\n' > "$stage/cfg/wowsched.txt"
printf 'Host calls 16-bit code ON (existence-gated). Needed for any Win16 program.\r\n'  > "$stage/cfg/wowcall.txt"
printf '0' > "$stage/cfg/wowidle.txt"
printf 'the last run''s log (ntvdmhost.log) and screenshots land here\r\n' > "$stage/debug/out/README.txt"
{
    echo "NTVDMEX ${NTVDMEX_VERSION:-(development build)}"
    echo "build:    $build_id"
    echo "host md5: $(md5_of "$HOST")"
    echo "built:    $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "commit:   $(git rev-parse HEAD)"
} | perl -pe 's/\r?\n/\r\n/' > "$stage/VERSION.txt"

mkdir -p "$ROOT/dist"
rm -f "$ROOT/dist/$name.zip"
( cd "$ROOT/dist" && zip -q -r "$name.zip" "$name" )
echo "wrote dist/$name.zip"
( cd "$ROOT/dist" && unzip -l "$name.zip" | tail -n +4 | sed -e '$d' -e '$d' )
