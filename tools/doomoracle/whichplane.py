"""For every COLLAPSED 4-pixel group in the bar, ask which plane's byte survived.

In mode Y pixel x lives in plane x&3. A group at x0 (x0 % 4 == 0) that holds one
value V in the capture is four planes holding the same byte. If V equals the
REFERENCE pixel at x0+i, then plane i's data is what got smeared over the other
three -- which names the code path, because each candidate mechanism seeds from a
different plane:

    the scratch seed        g_yview[sel[0]] = the LOWEST selected plane = plane 0
    a latch copy            would preserve each plane's own byte (no collapse)
    the present path        re-interleaves, no single plane preferred

"none" means the surviving byte is not any of the four reference bytes at that
group -- the data is not merely smeared, it came from somewhere else entirely.
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
    win = [0, 0, 0, 0]
    none = amb = groups = notcoll = 0
    none_by_row = {}
    for y in range(h):
        for x0 in range(0, w - 3, 4):
            if not all(opaque[y][x0 + i] for i in range(4)):
                continue
            g = [shot[AT_Y + y][x0 + i] for i in range(4)]
            if not (g[0] == g[1] == g[2] == g[3]):
                notcoll += 1
                continue
            groups += 1
            r = [ref[y][x0 + i] for i in range(4)]
            hits = [i for i in range(4) if r[i] == g[0]]
            if not hits:
                none += 1
                none_by_row[y] = none_by_row.get(y, 0) + 1
            elif len(hits) > 1:
                amb += 1                      # reference itself flat here: uninformative
            else:
                win[hits[0]] += 1
    print("%s" % os.path.basename(path))
    print("  collapsed groups %d   (not collapsed %d)" % (groups, notcoll))
    print("  survivor is plane 0:%d  1:%d  2:%d  3:%d   ambiguous(ref flat):%d"
          % (win[0], win[1], win[2], win[3], amb))
    print("  NONE of the four reference bytes: %d  (%.1f%% of collapsed)"
          % (none, 100.0 * none / max(groups, 1)))
    if none_by_row:
        worst = sorted(none_by_row.items(), key=lambda kv: -kv[1])[:6]
        print("  worst 'none' rows (bar_y:count): "
              + ", ".join("%d:%d" % kv for kv in worst))
    print()
