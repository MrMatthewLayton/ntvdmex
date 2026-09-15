#!/usr/bin/env bash
#
# offvm.sh -- run the whole off-VM battery, in one command.
#
# ── WHY THIS EXISTS ──────────────────────────────────────────────────────────
# There are 30 *_test.c files under tools/dostest/ and, until now, NO RUNNER.
# Each carried its own `cc -std=c99 -I src/dos -o x_test ...` line in a comment,
# several carried none at all, and they were compiled by hand one at a time. So
# "is the battery still green?" -- the question you must be able to answer after
# touching a shared header -- had no answer short of thirty commands, and in
# practice got asked about whichever one test seemed relevant. Session 72 changed
# dos_err.h, which is included by the host and by at least two tests, and could
# not answer it.
#
# ⚠ A TEST THAT DOES NOT COMPILE IS A FAILURE, NOT A SKIP. The tempting shape
#   here is `cc ... 2>/dev/null || continue`, which turns a broken test into
#   silence and makes the battery greener the more of it rots. Anything that will
#   not build is reported in its own section and sets the exit status, exactly as
#   a failing assertion does. Same rule as the harness elsewhere in this project:
#   absent is a visible failure, stale is an invisible one that reads as a result.
#
#   ./scripts/offvm.sh              # run everything
#   ./scripts/offvm.sh pit err      # only tests whose name matches
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${TMPDIR:-/tmp}/ntvdmex-offvm.$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT

CC="${CC:-cc}"
# The union of the include roots the tests use. Harmless when unused.
INCS=(-I "$ROOT/src/dos" -I "$ROOT/src/vdd" -I "$ROOT/src" -I "$ROOT/src/host" -I "$ROOT/tools/dostest")

# ⚠ HALF THE BATTERY IS NOT HEADER-ONLY. The vdd tests exercise real device
#   models (vdd_bus_add, vdd_pic_acknowledge, ...), so the implementation has to
#   be linked in or they fail at LINK time -- which looks exactly like a broken
#   test and is how 14 of 30 first appeared here. Two sources genuinely cannot
#   build off-VM (audio_wave.c and present_ddraw.c want DirectSound/DirectDraw);
#   everything else in src/vdd is portable enough for a native build, which is
#   what makes an off-VM device battery possible at all.
VDDSRC=()
for v in "$ROOT"/src/vdd/*.c; do
    case "$(basename "$v")" in audio_wave.c|present_ddraw.c) continue;; esac
    VDDSRC+=("$v")
done

checks=0; failed=0; ran=0
declare -a BROKEN=() FAILING=()

for src in "$ROOT"/tools/dostest/*_test.c; do
    name="$(basename "$src" .c)"
    if [ $# -gt 0 ]; then
        match=0
        for pat in "$@"; do case "$name" in *"$pat"*) match=1;; esac; done
        [ $match -eq 1 ] || continue
    fi
    bin="$OUT/$name"
    # Header-only first; if it does not LINK, retry with the device models. Tried
    # in this order so a test that needs nothing is not silently given everything.
    own="$ROOT/src/vdd/vdd_${name%_test}.c"
    if ! "$CC" -std=c99 -O1 -w "${INCS[@]}" -o "$bin" "$src" 2>"$OUT/$name.cc"; then
        # ⚠ THEN ITS OWN MODEL ALONE, BEFORE ALL OF THEM. pic_test supplies its
        #   own vdd_claim_ports stub, so linking the whole device set gives a
        #   DUPLICATE SYMBOL -- a test that is perfectly fine reported as broken
        #   purely because the runner was too generous.
        if ! { [ -f "$own" ] && "$CC" -std=c99 -O1 -w "${INCS[@]}" -o "$bin" "$src" "$own" 2>"$OUT/$name.cc"; }; then
            if ! "$CC" -std=c99 -O1 -w "${INCS[@]}" -o "$bin" "$src" "${VDDSRC[@]}" 2>"$OUT/$name.cc"; then
                BROKEN+=("$name")
                continue
            fi
        fi
    fi
    # A test must terminate on its own; none of these are interactive.
    # ⚠ FROM tools/dostest, NOT THE REPO ROOT. wow_test scans the tree relative to
    #   `../..` and reported "wrong root?" as a FAIL when run from anywhere else --
    #   a green test failing for a reason that had nothing to do with the code.
    if ! out="$(cd "$ROOT/tools/dostest" && "$bin" 2>&1)"; then rc=1; else rc=0; fi
    ran=$((ran + 1))
    # ⚠ THREE SUMMARY DIALECTS, and assuming one of them under-reports the other
    #   two as "0 checks" -- which reads as a passing test that asserted nothing:
    #     "== 36 checks, 0 failed"      err_test, disk_test, ...
    #     "== 66/66 passed, 0 failed == "  mcb_test
    #     "52 checks, 0 failed"         x86len_test, xms_test, pit_test, ...
    #     "-- 36 checks, 0 failures --" sb_test, opl_test, dma_test, ...  (failureS)
    line="$(printf '%s\n' "$out" | grep -E '([0-9]+ checks, [0-9]+ (failed|failures)|[0-9]+/[0-9]+ passed, [0-9]+ failed)' | tail -1)"
    if [ -n "$line" ]; then
        if printf '%s' "$line" | grep -q 'passed,'; then
            c="$(printf '%s' "$line" | sed -E 's/.*= ([0-9]+)\/[0-9]+ passed.*/\1/')"
            f="$(printf '%s' "$line" | sed -E 's/.*passed, ([0-9]+) failed.*/\1/')"
        else
            c="$(printf '%s' "$line" | sed -E 's/.*[^0-9]([0-9]+) checks.*/\1/;s/^([0-9]+) checks.*/\1/')"
            f="$(printf '%s' "$line" | sed -E 's/.*checks, ([0-9]+) (failed|failures).*/\1/')"
        fi
        checks=$((checks + c)); failed=$((failed + f))
        if [ "$f" != "0" ]; then
            FAILING+=("$name ($f of $c)")
            printf '  %-20s %4s checks  %s FAILED\n' "$name" "$c" "$f"
            printf '%s\n' "$out" | grep -E '^\s+FAIL' | head -8 | sed 's/^/      /'
        else
            printf '  %-20s %4s checks  ok\n' "$name" "$c"
        fi
    else
        # No summary line: we cannot claim it passed. Judge by exit status and say so.
        if [ $rc -eq 0 ]; then
            printf '  %-20s    ? ran, but printed NO summary this runner knows (0 checks counted)\n' "$name"
        else
            FAILING+=("$name (no summary, exit $rc)")
            printf '  %-20s    ! no summary and NONZERO EXIT\n' "$name"
        fi
    fi
done

echo
echo "  ran $ran test binaries: $checks checks, $failed failed"
if [ ${#BROKEN[@]} -gt 0 ]; then
    echo
    echo "  ${#BROKEN[@]} test(s) DID NOT COMPILE -- these are failures, not skips:"
    for b in "${BROKEN[@]}"; do
        echo "    $b"
        head -3 "$OUT/$b.cc" | sed 's/^/        /'
    done
fi
[ ${#FAILING[@]} -eq 0 ] && [ ${#BROKEN[@]} -eq 0 ] && { echo "  BATTERY GREEN"; exit 0; }
exit 1
