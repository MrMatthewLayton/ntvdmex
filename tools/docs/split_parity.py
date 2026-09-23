#!/usr/bin/env python3
"""split_parity.py -- fold docs/PARITY.md into docs/inventory/, one file per surface.

WHY
    PARITY.md and docs/inventory/ were the same method written twice: enumerate a
    surface, mark every unit, diff against an oracle. PARITY used
    missing/guessed/implemented/verified; the inventory uses IMPL/PART/STORE/MISS/N-A.
    Two vocabularies, two homes, one idea -- and the spec-first programme is built on
    the newer one while the measured data sat in the older.

    The two vocabularies are not rivals, they are two AXES, which is why neither
    could express what the other did:

        PARITY "verified"    = implemented AND compared against an oracle
        PARITY "implemented" = implemented, never compared
        PARITY "guessed"     = written from documentation or memory, never compared
        PARITY "missing"     = not implemented

    So the merged tables keep both: a coverage mark (IMPL/PART/STORE/MISS/N-A) and a
    verification mark (oracle-compared, provisional, or not asked). Nothing is lost
    and the PART state -- the dangerous half-implemented one the 4-state vocabulary
    could not say -- becomes expressible for these surfaces too.

    Content is carried over VERBATIM. Translating the state words is a judgement this
    script deliberately does NOT make automatically: it carries the original tables
    across and each surface is re-marked by hand against the code, which is the only
    way the inventory's file:line citations mean anything.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PARITY = os.path.join(ROOT, "docs", "PARITY.md")
INV = os.path.join(ROOT, "docs", "inventory")

HEADER = """# {title}

> **Carried over from `docs/PARITY.md` on 2026-09-23**, which is now retired — this is
> the same measured data in the inventory's home. See
> [`README.md`](README.md) for the method and the status vocabulary.
>
> ⚠ **The `state` column below is still PARITY's four-state vocabulary**
> (`missing` / `guessed` / `implemented` / `verified`). Re-marking it against the code
> in IMPL / PART / STORE / MISS / N-A, with a `file:line` per row, is owed — and is the
> point of doing it: a row that reads `implemented` here may well be **PART**, which is
> the state that returns a plausible wrong answer.

"""

# (outfile, title, [(first_line, last_line), ...]) -- 1-indexed, inclusive.
TARGETS = [
    ("keyboard.md", "Keyboard — INT 16h, the BDA and the 8042", [(89, 132)]),
    ("mouse.md", "Mouse — INT 33h", [(133, 207)]),
    ("video-bios.md", "Video BIOS — INT 10h and the BDA block", [(208, 270)]),
    ("pic.md", "Interrupt controller — the 8259 as a handler sees it", [(271, 297)]),
    ("bios-misc.md", "Misc BIOS — INT 11h, 12h, 1Ah, 1Ch", [(298, 343)]),
    ("dos-services.md", "DOS services — files, directories, PSP, memory, the HMA, INT 13h",
     [(344, 436), (531, 567), (612, 647)]),
    ("sweep.md", "The full probe sweep, and its triage", [(437, 530)]),
]

# Deliberately NOT carried over, each with a reason:
#   1-67    the method preamble -- already in inventory/README.md, and its probe traps
#           (POISON every output register; guard every blocking call) are folded there.
#   68-88   the references table -- becomes docs/ref/SOURCES.md, where the specs live.
#           Its "PCem is still blocked" note is stale: PCem works, it just cannot run
#           from an agent shell.
#   568-611 the score -- stale numbers, and the standing rule is re-run, never quote.
#   648-660 "Next, in order" -- superseded by STATE.md's next actions.


def main():
    with open(PARITY, encoding="utf-8") as fh:
        lines = fh.read().split("\n")

    covered = set()
    for _, _, ranges in TARGETS:
        for lo, hi in ranges:
            covered |= set(range(lo, hi + 1))
    dropped = set(range(1, 89)) | set(range(568, 612)) | set(range(648, len(lines) + 1))
    gap = set(range(1, len(lines) + 1)) - covered - dropped
    if gap:
        s = sorted(gap)
        print(f"FAIL {len(s)} lines belong to neither a target nor the drop list: "
              f"{s[:12]}{'...' if len(s) > 12 else ''}", file=sys.stderr)
        return 1

    os.makedirs(INV, exist_ok=True)
    for name, title, ranges in TARGETS:
        path = os.path.join(INV, name)
        if os.path.exists(path):
            print(f"FAIL {path} already exists — refusing to overwrite", file=sys.stderr)
            return 1
        body = []
        for lo, hi in ranges:
            body += lines[lo - 1:hi] + [""]
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(HEADER.format(title=title) + "\n".join(body).rstrip() + "\n")
        n = sum(hi - lo + 1 for lo, hi in ranges)
        print(f"  wrote inventory/{name}  ({n} lines)")

    print(f"\n{len(covered)} lines carried, {len(dropped & set(range(1, len(lines) + 1)))} "
          f"deliberately dropped (see the comment in this script).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
