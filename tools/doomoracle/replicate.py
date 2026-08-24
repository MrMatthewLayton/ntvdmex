"""Is the captured bar exactly ONE PLANE replicated four times?

If every captured pixel equals the reference pixel at the same group but phase p,
then the bar we display is plane p's column set smeared over all four -- a total
description of the fault rather than a correlation. Scored per phase so the winner
is compared against its three rivals, and against the trivial "captured == reference"
baseline so a high score cannot come from the artwork being flat.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import doomref as D

WAD = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'DOOM1.WAD')
AT_Y = 168

d, lumps, _ = D.read_wad(WAD)
lo, ls = lumps['STBAR']
w, h, ref, opaque = D.decode_patch(d, lo, ls)

for path in sys.argv[1:]:
    sw, sh, shot = D.read_bmp(path)
    tot = 0
    hit = [0, 0, 0, 0]
    ident = 0
    per_row = [[0] * 4 for _ in range(h)]
    for y in range(h):
        for x in range(w):
            if not opaque[y][x]:
                continue
            g0 = x - (x % 4)
            if g0 + 3 >= w or not all(opaque[y][g0 + i] for i in range(4)):
                continue
            tot += 1
            got = shot[AT_Y + y][x]
            if got == ref[y][x]:
                ident += 1
            for p in range(4):
                if got == ref[y][g0 + p]:
                    hit[p] += 1
                    per_row[y][p] += 1
    print("%s  (%d comparable pixels)" % (os.path.basename(path), tot))
    for p in range(4):
        print("    got == ref[group + %d]  (plane %d replicated): %6d  %5.1f%%"
              % (p, p, hit[p], 100.0 * hit[p] / max(tot, 1)))
    print("    got == ref[x]          (correct)              : %6d  %5.1f%%"
          % (ident, 100.0 * ident / max(tot, 1)))
    best = max(range(4), key=lambda p: hit[p])
    rows = ["%d:%.0f%%" % (y, 100.0 * per_row[y][best] / max(sum(1 for x in range(w)
            if opaque[y][x]), 1)) for y in range(0, h, 4)]
    print("    plane %d match by row: %s" % (best, "  ".join(rows)))
    print()
