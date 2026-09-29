#!/usr/bin/env python3
"""wipecheck.py -- judge a capture taken DURING Doom's screen melt (#58).

    ./wipecheck.py DOOM1.WAD TITLEPIC shot07.bmp [shot08.bmp ...]

WHY THIS CAN BE EXACT. The melt's start screen is whatever I_ReadScreen copied out of
video memory -- on the attract loop, TITLEPIC. During the melt every 2-pixel column
slides down by its own offset d, so for y >= d the frame must hold TITLEPIC[y - d] in
that column, byte for byte; above d it shows the incoming screen, which this does not
judge. The defect #58 asks about is I_ReadScreen reading one plane four times (fixed in
8648f41): a remnant would put the SAME byte in every 4-pixel group of the sliding part,
so the shifted region would stop matching the WAD while d itself still looked right.

Per frame it prints, for the columns whose best offset leaves at least MIN_ROWS rows of
start screen to compare:
    moving   columns with d > 0 (0 = a settled title, not a melt frame)
    exact    of those, columns whose shifted region matches TITLEPIC exactly
    worst    the worst column's mismatching pixels / compared pixels
and a VERDICT: PASS (every judged column exact), FAIL, or NOT-A-MELT.
Captures must be the host's 8bpp palette-index BMPs (capture.flag), as for doomref.py.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from doomref import read_wad, decode_patch, read_bmp   # noqa: E402

MIN_ROWS = 8


def best_offset(ref, shot, x, h):
    """(d, mismatches, compared) minimising mismatches of the 2-pixel column at x."""
    best = None
    for d in range(0, h - MIN_ROWS + 1):
        n = h - d
        bad = 0
        for y in range(d, h):
            r = ref[y - d]
            s = shot[y]
            if r[x] != s[x] or r[x + 1] != s[x + 1]:
                bad += 1
                if best and bad >= best[1]:
                    break
        if best is None or bad < best[1] or (bad == best[1] and d < best[0]):
            best = (d, bad, n * 2)
        if bad == 0 and d == 0:
            break
    return best


def judge(ref, path):
    w, h, rows = read_bmp(path)
    if (w, h) != (320, 200):
        return "%s: %dx%d is not a 320x200 frame" % (path, w, h), None
    moving = exact = 0
    worst = (0, 1, -1)
    for x in range(0, 320, 2):
        d, bad, n = best_offset(ref, rows, x, h)
        if d == 0:
            continue
        moving += 1
        if bad == 0:
            exact += 1
        elif bad / n > worst[0] / worst[1]:
            worst = (bad, n, x)
    if moving == 0:
        return "%s: no column has moved -- NOT-A-MELT" % path, None
    ok = (exact == moving)
    msg = "%s: moving=%d exact=%d" % (path, moving, exact)
    if not ok:
        msg += " worst=%d/%d at x=%d" % worst
    return msg + (" -- PASS" if ok else " -- FAIL"), ok


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    d, lumps, _ = read_wad(sys.argv[1])
    lo, ls = lumps[sys.argv[2]]
    _w, _h, px, _op = decode_patch(d, lo, ls)
    ref = [bytes(r) for r in px]
    verdicts = []
    for p in sys.argv[3:]:
        msg, ok = judge(ref, p)
        print(msg)
        if ok is not None:
            verdicts.append(ok)
    if not verdicts:
        print("VERDICT: no melt frame among the captures")
        sys.exit(2)
    print("VERDICT: %s (%d melt frame(s))" % ("PASS" if all(verdicts) else "FAIL", len(verdicts)))
    sys.exit(0 if all(verdicts) else 1)


if __name__ == "__main__":
    main()
