#!/usr/bin/env bash
#
# launchmatrix.sh -- run the LAUNCH SHAPES on both hosts and tabulate.  GH #140.
#
# #26's dosdiff.py compares hosts on one API call at a time. This compares them on
# one LAUNCH SHAPE at a time, which is the other axis and the one that decides
# whether NTVDMEX can be the machine's VDM: a program that runs perfectly when we
# launch it our way is still a regression if double-clicking it behaves
# differently from stock.
#
#   ./scripts/launchmatrix.sh            # run every row, write docs/research/launch-matrix.md
#   ./scripts/launchmatrix.sh --rows 1,3 # just those rows
#
# ⚠ EVERY ROW RUNS ON BOTH HOSTS. A row measured only under NTVDMEX says nothing:
#   the question is never "does it work" but "does it do what stock does". The rig
#   drops the IFEO Debugger key for the stock half and puts it back (rt_stock.bat).
#
# ⚠⚠ DO NOT ADD A "PROGRAM THAT DOES NOT EXIST" ROW. #140 lists one, and it WEDGED
#   THE RIG: stock ntvdm raises a MODAL DIALOG for a missing image, which blocks
#   rt_stock.bat in `start /wait` forever. The watcher then stops consuming cmd.txt
#   and cannot be recovered remotely (kill, reboot and shutdown all fail -- only
#   `exec cmd /c runwatch.bat` moves it, and that is not reliable).
#   ★ AND IT LEAVES THE IFEO DEBUGGER KEY ABSENT. rt_stock.bat removes it on entry
#     and restores it on every exit path -- but a wedge has no exit path, so the key
#     stays gone and EVERY LATER RUN SILENTLY MEASURES STOCK NTVDM while looking
#     entirely plausible. Check stock_state.txt and `reg query` the key after any
#     stock run that did not complete.
#   That row needs a human at the box. It is deliberately not automated here.
#
# ⚠ ABSENT IS NOT THE SAME AS EMPTY. A row whose result file never appears means
#   the run did not happen -- a wedged watcher, a host that hung -- and that is
#   reported as ERROR, never as "produced no output". Those two have been confused
#   in this project before and the stale-artefact trap is the same shape.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SH=/private/tmp/xpshare
OUT="$ROOT/docs/research/launch-matrix.md"

# row | target | description
ROWS=(
  "1|p_ver.com|DOS .COM, plain launch"
  "2|MEM.EXE|DOS .EXE, plain launch"
  "3|p_exec.com|DOS program that EXECs a child"
  "4|dpmitest.com|DPMI / protected-mode client"
  "5|p_redir.com|handle redirection inside the guest"
)

want="${2:-all}"

run_one() {   # $1 = host tag (ex|stock), $2 = target
    local tag="$1" t="$2" res
    # ⚠ rt_stock.bat writes result_stock_<target>.txt, NOT stock_<target>.txt.
    # Getting this wrong does not fail loudly -- it waits out the full timeout and
    # reports NO-RESULT for a run that in fact succeeded, which reads as "stock
    # cannot do this either" and would have made half the matrix meaningless.
    if [ "$tag" = stock ]; then res="$SH/result_stock_$t.txt"; else res="$SH/result_$t.log"; fi
    rm -f "$res"
    if [ "$tag" = stock ]; then
        printf 'stock %s\r\n' "$t" > "$SH/cmd.txt"
    else
        printf '%s\r\n' "$t" > "$SH/cmd.txt"
    fi
    local i
    for ((i=0; i<40; i++)); do [ -f "$SH/cmd.txt" ] || break; sleep 2; done
    if [ -f "$SH/cmd.txt" ]; then echo "WATCHER-DEAD"; return; fi
    for ((i=0; i<90; i++)); do [ -f "$res" ] && { sleep 3; break; }; sleep 2; done
    if [ ! -f "$res" ]; then echo "NO-RESULT"; return; fi
    echo "OK:$res"
}

# One line of evidence per run: what the guest actually printed, squeezed.
summarise() {  # $1 = result path
    # ⚠ THE TWO HOSTS PRODUCE DIFFERENT FILE SHAPES. Ours is the host log with the
    # guest's output embedded after "DOS OUTPUT"; stock has no host log at all, so
    # rt_stock.bat redirects the program's own stdout and the file IS the output.
    # One summariser that assumed our shape would report every stock row as empty.
    local f="$1"
    if [ ! -f "$f" ]; then echo "(absent)"; return; fi
    if LC_ALL=C grep -aq "DOS OUTPUT" "$f" 2>/dev/null; then
        LC_ALL=C grep -a -A4 "DOS OUTPUT" "$f" 2>/dev/null | LC_ALL=C tr -d '\r' \
          | grep -avE "^ *==> DOS OUTPUT|^\]|^--$" | head -3 | LC_ALL=C tr '\n' ' ' \
          | sed 's/  */ /g;s/^ //;s/ $//' | cut -c1-110
    else
        LC_ALL=C tr -d '\r' < "$f" | grep -av "^\[stock\]" | head -3 \
          | LC_ALL=C tr '\n' ' ' | sed 's/  */ /g;s/^ //;s/ $//' | cut -c1-110
    fi
    return 0
}

# ⚠⚠ DID THIS RESULT COME FROM THE PROGRAM WE ASKED FOR? `C:\ntvdmex\target.txt`
#   is an UNCONDITIONAL OVERRIDE, so a run whose target file was not rewritten
#   executes whatever it last named -- and the result file still appears, still
#   looks plausible, and is about a different program entirely. Caught here: a
#   probe's output must carry its own `#PROBE <name>` banner. Without this check
#   the stock column of this table was reporting DPMI output under `p_ver.com`.
identity_ok() {   # $1 = result path, $2 = target
    local f="$1" t="$2" stem
    case "$t" in p_*.com) stem="${t%.com}"; stem="${stem#p_}" ;; *) return 0 ;; esac
    LC_ALL=C grep -aq "#PROBE" "$f" || return 0        # not a probe run at all
    LC_ALL=C grep -aq "#PROBE .*$stem" "$f"
}

echo "running the launch matrix -- $(date '+%H:%M:%S')"
declare -a RESULTS
for spec in "${ROWS[@]}"; do
    IFS='|' read -r n target desc <<< "$spec"
    if [ "$want" != all ] && [[ ",$want," != *",$n,"* ]]; then continue; fi
    echo "  row $n: $target ($desc)"
    a=$(run_one ex "$target");    echo "     ntvdmex: $a"
    b=$(run_one stock "$target"); echo "     stock  : $b"
    RESULTS+=("$n|$target|$desc|$a|$b")
done

{
  echo "# The launch matrix (GH #140)"
  echo
  echo "Generated by \`scripts/launchmatrix.sh\` on $(date '+%Y-%m-%d %H:%M')."
  echo
  echo "One row per LAUNCH SHAPE, run on BOTH hosts on the bare-metal rig. The"
  echo "question a row answers is never \"does it work\" but \"does it do what stock"
  echo "ntvdm does\" -- NTVDMEX replaces the machine's VDM, so a difference is a"
  echo "regression for every program on the box, not just the one under test."
  echo
  echo "| # | Shape | Target | NTVDMEX | stock ntvdm |"
  echo "|---|---|---|---|---|"
  for r in "${RESULTS[@]}"; do
      IFS='|' read -r n target desc a b <<< "$r"
      af="${a#OK:}"; bf="${b#OK:}"
      # if/else, NOT `test && x=$(...) || y=...`: an assignment takes the exit
      # status of the command substitution, so a summariser whose last pipeline
      # stage returns non-zero silently falls into the `||` branch. That is how
      # this table first printed raw file paths in the stock column.
      if [ "${a%%:*}" = OK ]; then
          as=$(summarise "$af")
          identity_ok "$af" "$target" || as="**WRONG TARGET RAN** -- $as"
      else as="**$a**"; fi
      if [ "${b%%:*}" = OK ]; then
          bs=$(summarise "$bf")
          identity_ok "$bf" "$target" || bs="**WRONG TARGET RAN** -- $bs"
      else bs="**$b**"; fi
      echo "| $n | $desc | \`$target\` | $as | $bs |"
  done
} > "$OUT"
echo "wrote $OUT"
