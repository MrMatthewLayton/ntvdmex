#!/usr/bin/env bash
# Build the OPL comparison harness. DEV ONLY -- links the out-of-tree reference core.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$here/../.."
dst="$root/build/oplref"
[ -f "$dst/opl3.c" ] || "$here/fetch.sh"
core=("$root/src/vdd/vdd_opl.c" "$root/src/vdd/vdd_opl_synth.c" "$root/src/vdd/vdd_bus.c" "$dst/opl3.c")

# oplcmp  -- replay a game trace through both cores and score it (the regression).
cc -O2 -I"$dst" -I"$root/src/vdd" "$here/oplcmp.c" "${core[@]}" -lm -o "$dst/oplcmp"
echo "built $dst/oplcmp"

# oplprobe -- the single-note experiment rig (one register moving, one number out).
cc -O2 -I"$dst" -I"$root/src/vdd" "$here/oplprobe.c" "${core[@]}" -lm -o "$dst/oplprobe"
echo "built $dst/oplprobe"
