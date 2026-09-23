#!/usr/bin/env python3
"""doomdetail.py -- measure the mode-Y doubling defect objectively, by region.

    ./tools/doomdetail.py 'runs/.../*.bmp'

WHY THE STATUS BAR IS THE INSTRUMENT, AND THE 3D VIEW IS NOT.
    "Even-column match" -- of the pixel pairs (2k, 2k+1), how many are equal --
    was the project's mode-Y instrument, calibrated on Doom's 3D view where 0.08
    means a real 320-wide picture and 1.000 means every even column equals the
    next. That calibration is only valid at HIGH detail.

    ⚠ AT LOW DETAIL DOOM DOUBLES PIXELS ON PURPOSE, so a high even-column match
      in the 3D view is CORRECT there and measures nothing. Every earlier attempt
      to grade low detail this way was reading Doom's own drawing as our defect.

    THE STATUS BAR IS DRAWN AT FULL RESOLUTION REGARDLESS OF DETAIL LEVEL. So it
    is an internal control: same build, same scene, same everything, and the only
    variable is a setting that should not touch it. If its even-column match
    moves when detaillevel moves, the movement is OURS.

MEASURED 2026-09-23, host 77b9b0bd, Doom E1M1, screenblocks 10:

        region        high detail    low detail
        3D view       0.68 - 0.72    0.99 - 1.00   (low is Doom's own doubling)
        status bar    0.28 - 0.29    0.75 - 0.77   <-- THE DEFECT, 2.6x

    ⇒ The target for step 4 is the status bar reading ~0.28 at BOTH detail levels.
"""
import glob
import os
import struct
import sys


def load_bmp(path):
    d = open(path, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    if bpp != 8:
        return None, w, abs(h), bpp
    stride = ((w * bpp // 8) + 3) & ~3
    rows = [list(d[off + y * stride: off + y * stride + w]) for y in range(abs(h))]
    if h > 0:
        rows.reverse()                      # bottom-up DIB
    return rows, w, abs(h), bpp


def even_col_match(rows, y0, y1, w):
    same = tot = 0
    for r in rows[y0:y1]:
        for x in range(0, w - 1, 2):
            tot += 1
            same += (r[x] == r[x + 1])
    return same / tot if tot else float("nan")


def main():
    pats = sys.argv[1:] or ["runs/**/*.bmp"]
    files = sorted(f for p in pats for f in glob.glob(p, recursive=True))
    if not files:
        sys.exit("no .bmp matched")
    print("  %-28s %10s  %8s %11s" % ("shot", "geom", "3D view", "status bar"))
    for p in files:
        rows, w, h, bpp = load_bmp(p)
        if rows is None:
            print("  %-28s %dx%dx%d  (not 8bpp)" % (os.path.basename(p), w, h, bpp))
            continue
        sb = 32 * h // 200                  # Doom's status bar is 32 of 200 lines
        print("  %-28s %5dx%-4d  %8.3f %11.3f"
              % (os.path.basename(p), w, h,
                 even_col_match(rows, 0, h - sb, w),
                 even_col_match(rows, h - sb, h, w)))
    print("\n  status bar ~0.28 = correct (what high detail achieves);"
          " ~0.75 = the defect.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
