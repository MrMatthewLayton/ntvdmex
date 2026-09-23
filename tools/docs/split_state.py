#!/usr/bin/env python3
"""split_state.py -- carve the session archive out of docs/STATE.md.

WHY THIS EXISTS
    STATE.md is the canonical resume point and had grown to 5,155 lines, of which
    ~240 were state and ~4,500 were session blocks for s54-s75 nested INSIDE a list
    item (156 headings indented three spaces, invisible to every heading-level tool).

    This project has already fixed this exact problem once: docs/log/sessions/README.md
    records splitting `return-ntvdm.md` -- "4,000 lines of reverse-chronological
    narrative, a fine scratchpad and an impossible thing to hand to anyone" -- into
    per-session files. The replacement regrew into the same shape. This is that fix,
    applied again, as a script so it is repeatable rather than a one-off act of typing.

DISCIPLINE
    The archive's rule is that content is VERBATIM: "Nothing has been edited or
    corrected, including conclusions that a later session refuted." So this script
    only ever does two things to a line: strips the three-space list indentation that
    nested it, and promotes the headings that indentation was hiding. It never
    rewrites prose. Run with --check to verify that round-trip before writing.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
STATE = os.path.join(ROOT, "docs", "STATE.md")
SESSIONS = os.path.join(ROOT, "docs", "log", "sessions")

# (session, first_line, last_line, headline) -- 1-indexed, inclusive, from the
# indented-heading map. Sessions run newest-first in the file, so the ranges descend.
BLOCKS = [
    (75, 253, 453, "Doom's low detail is genuinely broken; `detaillevel 1` cost a day; the zip in the field"),
    (74, 454, 1004, "Duke3D runs, ZAR's VESA modes render, heaven7 renders, Heretic runs"),
    (73, 1005, 1387, "The share is laid out for release; Win16 comes back; Hexen and Doom run"),
    (72, 1388, 1694, "The by-hand pass; Doom's E1M1 crash is ours; QBasic's drive list"),
    (71, 1695, 1779, "The text-mode application class -- QBasic's three symptoms were six host defects"),
    (70, 1780, 1889, "Lemmings closed for real: the timer restarts per the datasheet, IRQ0 held in service"),
    (69, 1890, 1934, "Lemmings: music fix reverted (it blanked the screen); capture refined"),
    (68, 1935, 1976, "The rig was silently stock; Lemmings reaches gameplay by hand"),
    (61, 1977, 2120, "Skyroads perfect: the crystal was on the wrong lock"),
    (60, 2121, 2305, "CPU speed, honestly -- then the Skyroads wobble, root-caused"),
    (59, 2306, 2804, "ZAR renders: its attract demo is on screen, in colour"),
    (58, 2805, 3004, "86.9% unchanged, and four real defects were fixed anyway"),
    (57, 3005, 3104, "TASKMAN puts up its task list, and Cancel closes it"),
    (56, 3105, 3478, "Calc calculates; we never let the CPU fault for us; the trace was the problem"),
    (55, 3479, 3627, "Win16 dialogs -- the unlock of the day (USER thunk 0xEF)"),
    (54, 3628, 3738, "The clock: a stepped-over call, for the fifth time"),
]

# Everything from here to the end of the nested region is already labelled background:
# session 53's handoff, the session 41-50 pointers, and the WOW/krnl386 reference blocks.
TAIL = (3739, 4774, "Session 53 and older -- handoff pointers and the WOW/krnl386 reference blocks")

NEST = re.compile(r"^ {3}(?=\S)")


def deindent(lines):
    """Strip the three-space list nesting. Leaves deeper indentation (code, quotes) alone."""
    return [NEST.sub("", ln) for ln in lines]


def load():
    with open(STATE, encoding="utf-8") as fh:
        return fh.read().split("\n")


def check(lines):
    """Every line we are about to move must be inside the nested region and nothing
    outside it may be touched. A silent off-by-one here would drop a session."""
    ok = True
    covered = set()
    for _, lo, hi, _ in [(b[0], b[1], b[2], b[3]) for b in BLOCKS] + [(0, TAIL[0], TAIL[1], "")]:
        for n in range(lo, hi + 1):
            if n in covered:
                print(f"FAIL overlapping line {n}", file=sys.stderr)
                ok = False
            covered.add(n)
    span = range(BLOCKS[0][1], TAIL[1] + 1)
    missing = [n for n in span if n not in covered]
    if missing:
        print(f"FAIL {len(missing)} lines in the region belong to no block: "
              f"{missing[:10]}{'...' if len(missing) > 10 else ''}", file=sys.stderr)
        ok = False
    print(f"{'PASS' if ok else 'FAIL'}  {len(covered)} lines mapped, "
          f"{BLOCKS[0][1]}..{TAIL[1]}, {len(BLOCKS)} sessions + tail")
    return ok


def write_block(path, title, body, provenance):
    out = [f"# {title}", "", f"> {provenance}", "",
           "> Verbatim from `docs/STATE.md`. Nothing has been edited or corrected,",
           "> including conclusions a later session refuted — same rule as the rest of",
           "> this archive. See [`README.md`](README.md).", ""]
    out += deindent(body)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(out).rstrip() + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="verify the line map, write nothing")
    args = ap.parse_args()

    lines = load()
    if len(lines) < TAIL[1]:
        print(f"FAIL STATE.md is {len(lines)} lines, expected at least {TAIL[1]} — "
              "the line map is stale, re-derive it before running this.", file=sys.stderr)
        return 1

    if not check(lines):
        return 1
    if args.check:
        return 0

    os.makedirs(SESSIONS, exist_ok=True)
    for n, lo, hi, headline in BLOCKS:
        path = os.path.join(SESSIONS, f"session-{n}.md")
        if os.path.exists(path):
            print(f"FAIL {path} already exists — refusing to overwrite", file=sys.stderr)
            return 1
        write_block(path, f"Session {n} — {headline}", lines[lo - 1:hi],
                    f"Session {n}. Split out of `docs/STATE.md` on 2026-09-23.")
        print(f"  wrote session-{n}.md  ({hi - lo + 1} lines)")

    lo, hi, headline = TAIL
    write_block(os.path.join(SESSIONS, "session-53-and-older.md"),
                f"Sessions 53 and older — {headline}", lines[lo - 1:hi],
                "Split out of `docs/STATE.md` on 2026-09-23. This region was already "
                "marked *background only* in STATE.md itself.")
    print(f"  wrote session-53-and-older.md  ({hi - lo + 1} lines)")
    print("\nSTATE.md itself is NOT modified by this script — rewrite it by hand.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
