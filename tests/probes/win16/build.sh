#!/usr/bin/env bash
#
# build.sh -- assemble and link the Win16 probes.
#
#   ./tests/probes/win16/build.sh            # all of them -> build/wintest/
#   ./tests/probes/win16/build.sh w_kernel   # just one
#
# nasm writes the 16-bit code; tools/ne/mkne.py wraps it in the NE. There is no
# 16-bit Windows toolchain on this machine and none in the tree -- see mkne.py's
# docstring for why that is the whole reason this exists.
#
# ⚠ THE BUILT .EXE IS NOT THE TEST. Running it is: it has to be staged into
#   demo\win16\<name>\ on the test machine share and launched through the IFEO hook, which
#   is what the test machine's run script does (local-only, not in the repo).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="$ROOT/build/wintest"
mkdir -p "$OUT"

want=("$@")
built=0
for src in "$ROOT"/tests/probes/win16/w_*.asm; do
    name="$(basename "$src" .asm)"
    if [ ${#want[@]} -gt 0 ]; then
        hit=0
        for w in "${want[@]}"; do case "$name" in *"$w"*) hit=1;; esac; done
        [ $hit -eq 1 ] || continue
    fi
    # The module name is what krnl386 records; keep it 8.3-safe and uppercase.
    mod="$(printf '%s' "${name#w_}" | tr '[:lower:]' '[:upper:]' | cut -c1-8)"
    exe="$OUT/W16${mod}.EXE"
    nasm -f bin -i "$ROOT/tests/probes/win16/" "$src" -o "$OUT/$name.bin"
    python3 "$ROOT/tools/ne/mkne.py" "$OUT/$name.bin" "$exe" \
        --module "W16$mod" --desc "ntvdmex win16 probe: $name"
    # ⚠ VALIDATE WITH OUR OWN READER. nedump.py is verified against real Win16
    #   binaries, so if it cannot parse what mkne.py just wrote, krnl386 will not
    #   either -- and that is far cheaper to learn here than on the test machine.
    python3 "$ROOT/tools/ne/nedump.py" "$exe" > "$OUT/$name.nedump" 2>&1 || {
        echo "  ⛔ nedump could not parse $exe -- see $OUT/$name.nedump" >&2
        exit 1
    }
    built=$((built + 1))
done
echo "built $built probe(s) into $OUT"
