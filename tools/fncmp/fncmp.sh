#!/usr/bin/env bash
#
# fncmp.sh -- prove that a refactor of the host changed no generated code.
#
# Compiles the host's translation unit (src/host/main.c and everything it includes) from
# two source trees, then compares the object code FUNCTION BY FUNCTION and the initialised
# data SECTION BY SECTION, by name. The order of functions in the file is ignored; the
# content of each one must match exactly.
#
#   ./tools/fncmp/fncmp.sh main HEAD            # two git revisions
#   ./tools/fncmp/fncmp.sh main .               # a revision and the working tree
#   ./tools/fncmp/fncmp.sh ../old ../new        # two checkouts
#
# Exit status 0 = IDENTICAL, 1 = DIFFERENT (the differing functions and sections are
# listed; FNCMP_KEEP=1 keeps the normalised disassembly for diffing).
#
# ── WHY NOT `cmp` THE TWO EXECUTABLES ─────────────────────────────────────────────
# Moving code changes where every function lands, so every call, jump and literal
# address in the image changes with it, and the PE timestamp differs on every build
# regardless. So both trees are compiled with -ffunction-sections -fdata-sections (one
# section per function and per variable, which turns references between them into named
# relocations), and the disassembly is normalised:
#   - instruction and branch-target addresses are dropped;
#   - compiler-numbered names (.L labels, name.1234 clones and local statics) lose the number;
#   - an operand relocated against .rdata (string literals, switch tables) is replaced by
#     the BYTES it points at, so a literal that merely moved compares equal and a literal
#     whose text changed does not;
#   - __LINE__ is pinned to 0, because HOST_LOCK() passes it to the lock diagnostics and
#     moving code changes it without changing behaviour.
# What survives normalisation is exactly what the code does.
#
# ⚠ CHECK THE INSTRUMENT. A comparison that cannot fail proves nothing. Before trusting a
#   new version of this script, run it against deliberate mutants: a reordering (must say
#   IDENTICAL), a one-token code change, a changed string literal and a changed table value
#   (each must say DIFFERENT, and name the function or section).
#
# FNCMP_CFLAGS adds flags to BOTH builds -- e.g. raising GCC's inlining limits, so that a
# function split into called-once static helpers is inlined back and compares equal.
#
# The compile flags are the host's from CMakeLists.txt; keep them in step.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$ROOT/tools/fncmp"
[[ $# -eq 2 ]] || { sed -n '3,16p' "${BASH_SOURCE[0]}"; exit 2; }
OUT="${TMPDIR:-/tmp}/ntvdmex-fncmp.$$"
mkdir -p "$OUT"
[[ -n "${FNCMP_KEEP:-}" ]] && echo "keeping $OUT" || trap 'rm -rf "$OUT"' EXIT
CC="${CC_XP32:-i686-w64-mingw32-gcc}"
PREFIX="${CC%gcc}"
printf '#undef __LINE__\n#define __LINE__ 0\n' > "$OUT/pin.h"

# A directory is used as it is; anything else is a git revision, exported.
tree() {
    local spec="$1" tag="$2"
    if [[ -d "$spec" ]]; then (cd "$spec" && pwd); return; fi
    mkdir -p "$OUT/$tag-src"
    git -C "$ROOT" archive "$spec" src sdk res | tar -x -C "$OUT/$tag-src"
    echo "$OUT/$tag-src"
}

build() {
    local root="$1" tag="$2"
    "$CC" -DWINVER=0x0501 -D_WIN32_WINNT=0x0501 -O3 -DNDEBUG -std=gnu99 -ffreestanding \
        -Wno-array-bounds -Wno-builtin-macro-redefined -w \
        -ffunction-sections -fdata-sections -include "$OUT/pin.h" ${FNCMP_CFLAGS:-} \
        -I"$root/src/host" -I"$root/src/vdm" -I"$root/src/dos" -I"$root/src/vdd" -I"$root/src/wow" \
        -c "$root/src/host/main.c" -o "$OUT/$tag.obj"
    "${PREFIX}objcopy" -O binary --only-section=.rdata "$OUT/$tag.obj" "$OUT/$tag.rdata.bin"
    mkdir -p "$OUT/$tag.code"
    "${PREFIX}objdump" -d -r --no-show-raw-insn "$OUT/$tag.obj" \
        | python3 "$HERE/code.py" "$OUT/$tag.code" "$OUT/$tag.rdata.bin"
    OBJDUMP="${PREFIX}objdump" python3 "$HERE/data.py" "$OUT/$tag.obj" "$OUT/$tag.data" "$OUT/$tag.rdata.bin"
}

BASE="$(tree "$1" base)"
CAND="$(tree "$2" cand)"
build "$BASE" base &
build "$CAND" cand &
wait %1 && wait %2

count() { ls "$1" | wc -l | tr -d ' '; }
echo "functions: $(count "$OUT/base.code") vs $(count "$OUT/cand.code");" \
     "data sections: $(count "$OUT/base.data") vs $(count "$OUT/cand.data")"
status=0
for part in code data; do
    if diff -rq "$OUT/base.$part" "$OUT/cand.$part" >/dev/null; then
        echo "$part IDENTICAL"
    else
        diff -rq "$OUT/base.$part" "$OUT/cand.$part" | sed -E 's|^Files .*/base\.[a-z]+/([^ ]+) and .*|  differs: \1|; s|^Only in .*/([a-z]+)\.[a-z]+: |  only in \1: |' || true
        echo "$part DIFFERENT"; status=1
    fi
done
[[ $status == 0 ]] && echo "IDENTICAL: every function and every named data section match"
exit $status
