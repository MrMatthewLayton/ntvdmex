"""Per-row profile of the STBAR fault: how wrong is each row, and is it collapsed?

The whole-bar number (60% differ) cannot distinguish two mechanisms acting on
different rows, and the session-22 log says the write-mode-1 latch bursts only ever
touch plane offsets 0x3a1c..0x3e7f -- screen rows 186..199, i.e. BAR ROWS 18..31.
If rows 0..17 are also wrong, the latch copy is not the whole story.

grp4 collapsed = 4-pixel groups in the CAPTURE holding one value. In mode Y pixel x
lives in plane x&3, so a run of 4 equal pixels is the signature of four planes
holding the same byte -- the plane-collapse fault. Compared against the same count
in the REFERENCE, because real Doom artwork has flat runs of its own.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import doomref as D

WAD = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), 'DOOM1.WAD')
AT_Y = 168

d, lumps, _order = D.read_wad(WAD)
lo, ls = lumps['STBAR']
w, h, ref, opaque = D.decode_patch(d, lo, ls)
sw, sh, shot = D.read_bmp(sys.argv[1])

print("STBAR %dx%d at y=%d   shot %dx%d   %s" % (w, h, AT_Y, sw, sh, sys.argv[1]))
print("%5s %5s %6s %6s %7s   %-11s %-11s" %
      ("bar_y", "scr_y", "diff", "cmp", "%", "grp4 got", "grp4 ref"))


def collapse(row):
    ng = coll = 0
    for x in range(0, w - 3, 4):
        ng += 1
        if row[x] == row[x + 1] == row[x + 2] == row[x + 3]:
            coll += 1
    return coll, ng


tot_d = tot_c = 0
for y in range(h):
    dif = cmp_ = 0
    for x in range(w):
        if not opaque[y][x]:
            continue                       # face/numbers get drawn here: not comparable
        cmp_ += 1
        if ref[y][x] != shot[AT_Y + y][x]:
            dif += 1
    cg, ng = collapse(shot[AT_Y + y])
    cr, _ = collapse(ref[y])
    tot_d += dif
    tot_c += cmp_
    pct = 100.0 * dif / max(cmp_, 1)
    print("%5d %5d %6d %6d %6.1f%%   %3d/%-7d %3d/%-7d %s" %
          (y, AT_Y + y, dif, cmp_, pct, cg, ng, cr, ng, '#' * int(pct / 2.5)))
print("TOTAL %d/%d = %.2f%%" % (tot_d, tot_c, 100.0 * tot_d / tot_c))

lat = [y for y in range(h) if AT_Y + y >= 186]
oth = [y for y in range(h) if AT_Y + y < 186]


def band(rows):
    dd = cc = 0
    for y in rows:
        for x in range(w):
            if not opaque[y][x]:
                continue
            cc += 1
            if ref[y][x] != shot[AT_Y + y][x]:
                dd += 1
    return dd, cc


dd, cc = band(oth)
print("\nrows 168..185 (NOT touched by any write-mode-1 burst): %d/%d = %.1f%%" %
      (dd, cc, 100.0 * dd / max(cc, 1)))
dd, cc = band(lat)
print("rows 186..199 (the ONLY rows the bursts touch):        %d/%d = %.1f%%" %
      (dd, cc, 100.0 * dd / max(cc, 1)))
