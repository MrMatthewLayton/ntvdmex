"""Split the status bar into the two BANDS that behave differently, and score each
plane against every reference PHASE within each.

Session 23 built its plane-vs-phase matrix over the WHOLE bar and found every plane
peaking at phase 1. But rows 184-199 receive write-mode-1 bursts and rows 168-183
receive none -- session 24 confirmed that split across all 120 bursts and showed the
two bands are separately damaged (62.2% vs 57.6% wrong). A matrix over their union
describes neither. If the bands have different signatures they have different causes,
and the whole-bar figure has been averaging them together.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import doomref as D

LOG = sys.argv[1]
WAD = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'DOOM1.WAD')
d, lumps, _ = D.read_wad(WAD)
lo, ls = lumps['STBAR']
w, h, ref, opaque = D.decode_patch(d, lo, ls)

planes = {}
for line in open(LOG, 'r', errors='replace'):
    if not line.startswith('MODEYBAR pg'):
        continue
    f = line.split()
    pg = int(f[1][2:], 16); pl = int(f[2][2:], 16); row = int(f[3][1:], 16)
    planes.setdefault(pg, {}).setdefault(pl, {})[row] = bytes.fromhex(f[4].strip())

BANDS = [("A rows168-183 (NO bursts)", 168, 184), ("B rows184-199 (all bursts)", 184, 200)]
pg = 0
for name, r0, r1 in BANDS:
    print("=== page %d, band %s ===" % (pg, name))
    print("        " + "".join("  q=%d  " % q for q in range(4)) + "   <- reference phase")
    for pl in range(4):
        cells = []
        for q in range(4):
            ok = tot = 0
            for row in range(r0, r1):
                b = planes[pg][pl].get(row)
                if not b:
                    continue
                for i in range(80):
                    x = 4 * i + q          # score plane pl's byte against phase q's pixel
                    y = row - 168
                    if x >= w or not opaque[y][x]:
                        continue
                    tot += 1
                    if b[i] == ref[y][x]:
                        ok += 1
            cells.append("%5.1f%%" % (100.0 * ok / max(tot, 1)))
        star = "  <- own phase is %s" % cells[pl]
        print("  pl%d  %s%s" % (pl, " ".join(cells), star))
    # how uniform is each band, ours vs the reference
    same = tot = 0
    for row in range(r0, r1):
        ps = [planes[pg][p].get(row) for p in range(4)]
        if not all(ps):
            continue
        for i in range(80):
            tot += 1
            if ps[0][i] == ps[1][i] == ps[2][i] == ps[3][i]:
                same += 1
    rsame = rtot = 0
    for row in range(r0, r1):
        y = row - 168
        for i in range(80):
            xs = [4 * i + q for q in range(4)]
            if any(x >= w or not opaque[y][x] for x in xs):
                continue
            rtot += 1
            v = [ref[y][x] for x in xs]
            if v[0] == v[1] == v[2] == v[3]:
                rsame += 1
    print("  four-way uniform: ours %5.1f%%   reference %5.1f%%" %
          (100.0 * same / max(tot, 1), 100.0 * rsame / max(rtot, 1)))
    print()
