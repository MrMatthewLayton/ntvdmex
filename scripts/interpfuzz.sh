#!/usr/bin/env bash
# interpfuzz.sh [programs] [steps] [seed] -- the v86 interpreter, HEAD vs the working tree (#183).
#
# Builds tools/dostest/interp_fuzz.c twice -- once against HEAD's src/host/v86interp.h,
# once against the working tree's -- and runs both on the same seed. The digests must be
# IDENTICAL: that is the claim "this change did not alter what any instruction does".
# The bench lines are the speed comparison (this Mac, relative).
#   REF=<commit> compares against that commit instead of HEAD.
#
#   MODE=superset [ALLOW="66:8C ..."] ./scripts/interpfuzz.sh [programs] [steps] [seed] [pm]
#     (#194) For a change that ADDS instructions, where the digest must differ: runs both
#     interpreters in lockstep (tools/dostest/interp_superset.c) and requires the working
#     tree to do exactly what REF did wherever REF executed. Steps only the new one takes
#     are counted by opcode. ALLOW names intended behaviour changes. pm=1 runs with a fake
#     protected-mode descriptor table (g_seg2lin/g_sel_desc set).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
T="${TMPDIR:-/tmp}/interpfuzz.$$"; mkdir -p "$T"
REF="${REF:-HEAD}"
git -C "$ROOT" show "$REF:src/host/v86interp.h" > "$T/ref.h"
if [ "${MODE:-}" = "superset" ]; then
    D="$ROOT/tools/dostest"
    cc -O2 -w -DSIDE=ref_ -DINTERP_H="\"$T/ref.h\"" -I "$D" -c "$D/interp_side.c" -o "$T/ref.o"
    cc -O2 -w -DSIDE=new_ -I "$D" -c "$D/interp_side.c" -o "$T/new.o"
    cc -O2 -w -I "$D" -o "$T/sup" "$D/interp_superset.c" "$T/ref.o" "$T/new.o"
    rc=0; "$T/sup" "${1:-300000}" "${2:-64}" "${3:-0x5EEDF00D}" "${4:-0}" || rc=$?
    rm -rf "$T"; exit $rc
fi
cc -O2 -w -DINTERP_H="\"$T/ref.h\"" -o "$T/ref" "$ROOT/tools/dostest/interp_fuzz.c"
cc -O2 -w -o "$T/new" "$ROOT/tools/dostest/interp_fuzz.c"
args=("${1:-300000}" "${2:-64}" "${3:-0x5EEDF00D}")
echo "== $REF";     "$T/ref" "${args[@]}" | tee "$T/ref.out"
echo "== working";  "$T/new" "${args[@]}" | tee "$T/new.out"
a=$(grep digest "$T/ref.out"); b=$(grep digest "$T/new.out")
rm -rf "$T"
if [ "$a" = "$b" ]; then echo "SAME BEHAVIOUR ($a)"; else echo "⛔ DIGESTS DIFFER: $REF $a / working $b"; exit 1; fi
