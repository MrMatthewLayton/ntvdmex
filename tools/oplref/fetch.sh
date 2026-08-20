#!/usr/bin/env bash
# Fetch the reference OPL core. DEV-TIME ONLY -- it is NEVER linked into the host.
#
# Nuked OPL3 is a cycle-accurate YMF262 reconstructed from a die shot, and it is the
# only honest way to answer "does our instrument sound right". It is LGPL-2.1 and our
# synth is deliberately written from documented behaviour so it stays MIT-clean, so
# this is kept OUT OF THE TREE: fetched on demand into build/oplref/ (gitignored) and
# used only by the offline comparison harness.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
dst="$here/../../build/oplref"
mkdir -p "$dst"
base="https://raw.githubusercontent.com/nukeykt/Nuked-OPL3/master"
for f in opl3.c opl3.h; do
    if [ -f "$dst/$f" ]; then echo "have $f"; continue; fi
    echo "fetching $f"
    curl -fsSL "$base/$f" -o "$dst/$f"
done
echo "reference core in $dst (gitignored, dev-only)"
