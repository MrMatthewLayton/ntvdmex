#!/usr/bin/env bash
#
# paritysweep.sh -- run EVERY probe against the oracle and tally the result.
#
# The companion to scripts/offvm.sh: that one scores our own algorithms off-VM,
# this one scores us against genuine MS-DOS 6.22. Together they are the two
# tiers of the suite, and each now answers in one command.
#
#   ./scripts/paritysweep.sh                 # all probes
#   ./scripts/paritysweep.sh p_xms p_err     # just these
#
# ⚠ IT GRADES WHATEVER IS ON THE RIG. `bm\ntvdmhost.exe` is the subject, and it
#   is frequently NOT the build you just made -- the rig is deliberately parked
#   on the last USER-CONFIRMED binary between by-hand passes. Deploy first, or
#   the tally describes an older host. The header line says which md5 answered.
#
# ⚠ A CLEAN PROBE IS NOT A VERIFIED SURFACE. Several probes emit fewer functions
#   than their own headers claim, and many rows compare only CF. This counts
#   ROWS, which is a measure of what was ASKED, not of what is covered.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
D="$ROOT/tools/dostest"
SHARE=/private/tmp/xpshare

echo "  subject on the rig: $(md5 -q "$SHARE/bin/ntvdmhost.exe" 2>/dev/null || echo '?')"
echo

probes=()
if [ $# -gt 0 ]; then for p in "$@"; do probes+=("$p"); done
else for f in "$D"/p_*.asm; do probes+=("$(basename "$f" .asm)"); done; fi

tot_rows=0; tot_bad=0; tot_abs=0; clean=0; dirty=0; failed=0
declare -a DIRTY=() FAILED=()

for p in "${probes[@]}"; do
    [ -f "$D/$p.asm" ] || continue
    # ⛔ A FILE MATCHING p_*.asm IS NOT NECESSARILY A PROBE, and the glob above
    #   cannot tell. p_tsrc.asm is the RESIDENT HALF of p_tsr -- it is incbin'd
    #   into p_tsr.com, emits no canonical dump, and is not meant to be launched.
    #   The sweep ran it anyway on every pass, which means every pass installed a
    #   TSR hooking INT 60h on the rig and then reported the probe as "unusable"
    #   for ever. A probe is DEFINED by including the scaffolding that emits the
    #   dump, so ask that rather than trusting the filename.
    if ! grep -q 'include *"probe.inc"' "$D/$p.asm"; then
        printf '  %-10s skipped -- not a probe (no probe.inc; a helper or a payload)\n' "$p"
        continue
    fi
    # Assemble FROM the probe directory -- probe.inc is resolved relative to it.
    if ! ( cd "$D" && nasm -f bin "$p.asm" -o "$p.com" ) 2>/dev/null; then
        FAILED+=("$p (assembly)"); failed=$((failed+1))
        printf '  %-10s ASSEMBLY FAILED\n' "$p"; continue
    fi
    # A probe may name EXTRA oracles in a header comment, "; ORACLE-ALSO: pcem-vesa".
    # p_vesa does: a VESA BIOS is not MS-DOS, and Bochs's VBE and a real Tseng ROM
    # disagree on a few fields -- with both voting, a split is DISPUTED (not graded)
    # instead of a false mismatch against whichever answered alone. Costs ~2.5 min
    # per probe that asks (PCem boots for each), so it is opt-in per probe.
    # ⛔⛔⛔ TAKE THE FIRST WORD ONLY, AND THAT IS NOT A TIDINESS FIX. This used to
    #   be `s/^; *ORACLE-ALSO: *//p`, which returns THE REST OF THE LINE -- so a
    #   probe whose header reads
    #       ; ORACLE-ALSO: pcem   (a real AMI 486 BIOS -- see scripts/pcemoracle.py)
    #   word-split into `--host pcem --host (a --host real --host AMI ...`, dosdiff
    #   rejected it, and the probe reported NO ROWS and LEFT THE SWEEP ENTIRELY.
    #   Measured 2026-09-23: TEN of 47 probes -- p_dma p_fdc p_kbc p_rtc p_uart
    #   p_vgaext p_vgamem p_vgareg p_video, i.e. essentially EVERY hardware-device
    #   probe this project has written -- were excluded from the parity number for
    #   as long as they have existed. p_vesa and p_pit survived only because their
    #   lines happen to be a bare hostname.
    #   ⚠ The trailing comment is the NATURAL thing to write, and it is contagious:
    #     p_fdc.asm got its line by copying p_uart.asm's. A parser that punishes a
    #     comment must not do it silently.
    extra=""
    for o in $(sed -n 's/^; *ORACLE-ALSO: *\([^ ]*\).*/\1/p' "$D/$p.asm"); do
        extra="$extra --host $o"
    done
    out="$(/usr/bin/python3 "$ROOT/scripts/dosdiff.py" "$D/$p.com" --host msdos622 $extra --host ntvdmex 2>&1)"
    rows="$(printf '%s\n' "$out" | grep -cE '  (AGREE|MISMATCH|ABSTAINED|DISPUTED|NO-DATA)( \[[0-9]+\])?$')"
    bad="$(printf  '%s\n' "$out" | grep -cE '  MISMATCH( \[[0-9]+\])?$')"
    abs="$(printf  '%s\n' "$out" | grep -cE '  ABSTAINED( \[[0-9]+\])?$')"
    nod="$(printf  '%s\n' "$out" | grep -cE '  NO-DATA( \[[0-9]+\])?$')"
    if [ "$rows" -eq 0 ]; then
        # ⚠ AND SAY WHY. "NO ROWS" on its own is an absence, and an absence in a
        #   report means nothing -- it reads identically for "the probe crashed",
        #   "the host was silent" and "the runner built a nonsense command line".
        #   The last of those hid the ten probes above for three sessions.
        why="$(printf '%s\n' "$out" | grep -viE '^ *$' | tail -1 | cut -c1-90)"
        FAILED+=("$p (no rows)"); failed=$((failed+1))
        printf '  %-10s NO ROWS  <- %s\n' "$p" "${why:-no output at all}"; continue
    fi
    tot_rows=$((tot_rows+rows)); tot_bad=$((tot_bad+bad)); tot_abs=$((tot_abs+abs))
    note=""
    [ "$nod" -gt 0 ] && note=" ${nod} no-data"
    if [ "$bad" -eq 0 ]; then
        clean=$((clean+1)); printf '  %-10s %3d rows  clean%s%s\n' "$p" "$rows" \
            "$( [ "$abs" -gt 0 ] && printf ' (%d abstained)' "$abs")" "$note"
    else
        dirty=$((dirty+1)); DIRTY+=("$p:$bad")
        printf '  %-10s %3d rows  %d MISMATCH%s\n' "$p" "$rows" "$bad" "$note"
    fi
done

agree=$((tot_rows - tot_bad - tot_abs))
echo
printf '  %d probes: %d clean, %d with mismatches, %d unusable\n' \
       "$((clean+dirty+failed))" "$clean" "$dirty" "$failed"
printf '  %d rows: %d agree, %d mismatch, %d abstained\n' \
       "$tot_rows" "$agree" "$tot_bad" "$tot_abs"
if [ "$((agree+tot_bad))" -gt 0 ]; then
    printf '  PARITY %.1f%% of comparable rows (%d of %d; abstentions excluded, not counted as passes)\n' \
        "$(echo "scale=4; 100*$agree/($agree+$tot_bad)" | bc)" "$agree" "$((agree+tot_bad))"
fi
# ⛔⛔ A PROBE THAT DID NOT RUN AND A PROBE THAT DOES NOT EXIST LOOK THE SAME IN A
#   PERCENTAGE. The number above is computed over the rows that ANSWERED; every
#   unusable probe silently leaves both halves of the fraction, so the score can
#   only ever go UP when a probe breaks. For three sessions ten device probes were
#   excluded this way and the headline read 99.3%.
#   ⇒ The score is not allowed to be quoted on its own while any probe is
#     unusable. This is the same rule the VGA parity number had to learn: a score
#     is only about the questions that were actually asked.
if [ "$failed" -gt 0 ]; then
    echo
    echo "  ⛔ THE SCORE ABOVE EXCLUDES $failed UNUSABLE PROBE(S) ENTIRELY -- it is"
    echo "     not a parity figure for this suite until they run. Each is listed"
    echo "     above with the reason; fix or explain them before quoting a number."
    printf '     unusable: %s\n' "${FAILED[*]}"
fi
[ ${#DIRTY[@]} -gt 0 ] && { echo; echo "  still disagreeing: ${DIRTY[*]}"; }
exit 0
