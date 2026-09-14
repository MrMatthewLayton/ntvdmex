#!/usr/bin/env bash
#
# package.sh -- assemble the portable NTVDMEX zip (the 17th deliverable).
#
#   ./scripts/package.sh            # uses build/ntvdmhost.exe as built
#   ./scripts/package.sh --build    # rebuild first
#
# Produces dist/ntvdmex-<date>-<sha>.zip laid out so that extracting it ANYWHERE
# on an XP box and running install.bat is the whole install:
#
#   install.bat uninstall.bat status.bat smoke.bat README.txt
#   bm\ntvdmhost.exe  bm\selftest.com
#   cfg\  out\                 (empty; the host creates them anyway)
#   guest\wow\*                (the Win16 system files; private copies)
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
size=$(stat -f '%z' "$HOST")
[ "$size" -gt 1000000 ] || { echo "build/ntvdmhost.exe is $size bytes: that is the launcher, not the host" >&2; exit 1; }

sha=$(git rev-parse --short HEAD)
date=$(date +%Y%m%d)
name="ntvdmex-$date-$sha"
stage="$ROOT/dist/$name"
rm -rf "$stage"
mkdir -p "$stage/bm" "$stage/cfg" "$stage/out" "$stage/guest/wow"

cp "$HOST" "$stage/bm/ntvdmhost.exe"
cp "$ROOT/tools/dostest/selftest.com" "$stage/bm/selftest.com"
if ls "$ROOT/guest/wow"/* >/dev/null 2>&1; then
    cp "$ROOT/guest/wow"/* "$stage/guest/wow/"
else
    echo "WARNING: guest/wow is empty -- Win16 programs will use XP's own system files" >&2
fi
for f in install.bat uninstall.bat status.bat smoke.bat README.txt; do
    perl -pe 's/\r?\n/\r\n/' "$ROOT/package/$f" > "$stage/$f"
done
printf 'settings files go here; empty = defaults\r\n' > "$stage/cfg/README.txt"
printf 'the last run''s log (ntvdmhost.log) and screenshots land here\r\n' > "$stage/out/README.txt"
{
    echo "NTVDMEX $name"
    echo "host md5: $(md5 -q "$HOST")"
    echo "built:    $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "commit:   $(git rev-parse HEAD)"
} | perl -pe 's/\r?\n/\r\n/' > "$stage/VERSION.txt"

mkdir -p "$ROOT/dist"
rm -f "$ROOT/dist/$name.zip"
( cd "$ROOT/dist" && zip -q -r "$name.zip" "$name" )
echo "wrote dist/$name.zip"
( cd "$ROOT/dist" && unzip -l "$name.zip" | tail -n +4 | sed -e '$d' -e '$d' )
