#!/usr/bin/env bash
# interpfuzz.sh [programs] [steps] [seed] -- the v86 interpreter, HEAD vs the working tree (#183).
#
# Builds tools/dostest/interp_fuzz.c twice -- once against HEAD's src/host/v86interp.h,
# once against the working tree's -- and runs both on the same seed. The digests must be
# IDENTICAL: that is the claim "this change did not alter what any instruction does".
# The bench lines are the speed comparison (this Mac, relative).
#   REF=<commit> compares against that commit instead of HEAD.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
T="${TMPDIR:-/tmp}/interpfuzz.$$"; mkdir -p "$T"
REF="${REF:-HEAD}"
git -C "$ROOT" show "$REF:src/host/v86interp.h" > "$T/ref.h"
cc -O2 -w -DINTERP_H="\"$T/ref.h\"" -o "$T/ref" "$ROOT/tools/dostest/interp_fuzz.c"
cc -O2 -w -o "$T/new" "$ROOT/tools/dostest/interp_fuzz.c"
args=("${1:-300000}" "${2:-64}" "${3:-0x5EEDF00D}")
echo "== $REF";     "$T/ref" "${args[@]}" | tee "$T/ref.out"
echo "== working";  "$T/new" "${args[@]}" | tee "$T/new.out"
a=$(grep digest "$T/ref.out"); b=$(grep digest "$T/new.out")
rm -rf "$T"
if [ "$a" = "$b" ]; then echo "SAME BEHAVIOUR ($a)"; else echo "⛔ DIGESTS DIFFER: $REF $a / working $b"; exit 1; fi
