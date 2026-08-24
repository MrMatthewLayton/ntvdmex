"""Judge the four mode-Y planes against the IWAD, plane by plane.

Every previous measurement judged the SCREEN, which is planes plus a render, so it
could not say which plane held the right byte. The MODEYBAR dump is the planes
themselves; STBAR is what they should contain. Plane p's byte i is pixel x = 4i + p.

The question this exists to answer: are planes 0/2/3 a LITERAL COPY of plane 1 (one
writer smearing one plane over the rest), or their own separately-damaged content
that merely resembles it? Those need completely different fixes, and no screen
capture can tell them apart.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import doomref as D

LOG = sys.argv[1]
WAD = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'DOOM1.WAD')
d, lumps, _ = D.read_wad(WAD)
lo, ls = lumps['STBAR']
w, h, ref, opaque = D.decode_patch(d, lo, ls)

# planes[pg][pl][row] = 80 bytes
planes = {}
for line in open(LOG, 'r', errors='replace'):
    if not line.startswith('MODEYBAR pg'):
        continue
    f = line.split()
    pg = int(f[1][2:], 16); pl = int(f[2][2:], 16); row = int(f[3][1:], 16)
    planes.setdefault(pg, {}).setdefault(pl, {})[row] = bytes.fromhex(f[4].strip())

for pg in sorted(planes):
    print("=== page %d ===" % pg)
    for pl in range(4):
        ok = tot = 0
        for row in range(168, 200):
            b = planes[pg][pl].get(row)
            if not b:
                continue
            for i in range(80):
                x = 4 * i + pl
                y = row - 168
                if x >= w or not opaque[y][x]:
                    continue
                tot += 1
                if b[i] == ref[y][x]:
                    ok += 1
        print("  plane %d vs STBAR: %5d/%5d correct = %5.1f%%" % (pl, ok, tot, 100.0 * ok / max(tot, 1)))

    print("  --- plane-to-plane identity (bytes equal at the same offset) ---")
    for a in range(4):
        row_out = []
        for b_ in range(4):
            same = tot = 0
            for row in range(168, 200):
                pa, pb = planes[pg][a].get(row), planes[pg][b_].get(row)
                if not pa or not pb:
                    continue
                for i in range(80):
                    tot += 1
                    if pa[i] == pb[i]:
                        same += 1
            row_out.append("%5.1f%%" % (100.0 * same / max(tot, 1)))
        print("    plane %d vs 0/1/2/3: %s" % (a, "  ".join(row_out)))
    print()
