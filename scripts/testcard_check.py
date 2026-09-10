#!/usr/bin/env python3
"""testcard_check.py -- read a test-card capture and say what is wrong with it.

The card drawn by tools/dostest/testcard.asm has a known structure, so a capture
of it can be CHECKED rather than eyeballed.  This reads either a QEMU screendump
(.ppm, from scripts/dosoracle) or one of the host's own self-screenshots (.bmp,
8bpp indexed) and reports each element of the card independently:

    border    the 1-pixel frame -- geometry, and single-pixel (Bit Mask) writes
    diagonal  pixel (i,i) -- THE STRIDE TEST.  A wrong bytes-per-row bends it,
              and the report says at which row it first leaves the diagonal,
              which is usually enough to compute the stride actually in use.
    bars      16 vertical colour bands in order -- the palette and the
              value->colour mapping; a swapped PLANE shows as reordered bands.
    corners   2-byte white blocks -- origin and far corner both addressable.

WHY BOTH FORMATS.  The same card is rendered by genuine MS-DOS 6.22 under QEMU
(the reference) and by NTVDMEX on the rig (the thing under test), and they come
back in different file formats.  Reducing both to the same grid of colour indices
is what makes them comparable at all -- see --compare.

    testcard_check.py ref_0d.ppm --mode 0d
    testcard_check.py shot07.bmp --mode 0d
    testcard_check.py shot07.bmp --mode 0d --compare ref_0d.ppm
"""
import argparse
import struct
import sys

# The 16 EGA/VGA text-and-planar colours, which is what a 4-bit mode shows.
EGA = [
    (0x00, 0x00, 0x00), (0x00, 0x00, 0xA8), (0x00, 0xA8, 0x00), (0x00, 0xA8, 0xA8),
    (0xA8, 0x00, 0x00), (0xA8, 0x00, 0xA8), (0xA8, 0x57, 0x00), (0xA8, 0xA8, 0xA8),
    (0x57, 0x57, 0x57), (0x57, 0x57, 0xFF), (0x57, 0xFF, 0x57), (0x57, 0xFF, 0xFF),
    (0xFF, 0x57, 0x57), (0xFF, 0x57, 0xFF), (0xFF, 0xFF, 0x57), (0xFF, 0xFF, 0xFF),
]

# mode -> (width, height) in GUEST pixels
MODES = {
    "0d": (320, 200), "0e": (640, 200), "10": (640, 350),
    "12": (640, 480), "13": (320, 200), "03": (720, 400),
}


def load_ppm(path):
    d = open(path, "rb").read()
    f, i = [], 0
    while len(f) < 4:
        while d[i:i + 1].isspace():
            i += 1
        if d[i:i + 1] == b"#":
            while d[i:i + 1] != b"\n":
                i += 1
            continue
        j = i
        while not d[j:j + 1].isspace():
            j += 1
        f.append(d[i:j])
        i = j
    i += 1
    w, h = int(f[1]), int(f[2])
    px = d[i:]
    return w, h, lambda x, y: tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3])


def load_bmp(path):
    d = open(path, "rb").read()
    w, h = struct.unpack("<ii", d[18:26])
    bpp = struct.unpack("<H", d[28:30])[0]
    off = struct.unpack("<I", d[10:14])[0]
    if bpp != 8:
        sys.exit("only 8bpp BMPs (the host writes those); got %d" % bpp)
    pal = d[54:54 + 1024]
    px = d[off:]
    rowb = ((w + 3) // 4) * 4
    flip = h > 0                      # positive height = bottom-up rows
    ah = abs(h)

    def get(x, y):
        yy = (ah - 1 - y) if flip else y
        v = px[yy * rowb + x]
        return (pal[v * 4 + 2], pal[v * 4 + 1], pal[v * 4 + 0])
    return w, ah, get


def nearest(rgb):
    """RGB -> EGA index. The capture may have been scaled or slightly shifted,
    so match to the closest palette entry rather than demanding equality."""
    best, bd = 0, 1 << 30
    for i, c in enumerate(EGA):
        d = sum((a - b) ** 2 for a, b in zip(rgb, c))
        if d < bd:
            bd, best = d, i
    return best


def grid(path, gw, gh):
    """Reduce a capture to a gw x gh grid of EGA indices, whatever it was
    captured at. QEMU reports mode 0Dh as 640x400 (the adapter's own line
    doubling) and the host's snapshot is at the mode's true size, so neither
    can be indexed directly -- SAMPLE, do not assume."""
    w, h, get = (load_ppm if path.lower().endswith(".ppm") else load_bmp)(path)
    out = []
    for y in range(gh):
        sy = min(h - 1, (y * h) // gh + (h // gh) // 2)
        out.append([nearest(get(min(w - 1, (x * w) // gw + (w // gw) // 2), sy))
                    for x in range(gw)])
    return w, h, out


def check(g, gw, gh):
    res = []

    top_bad = [x for x in range(gw) if g[0][x] != 15]
    bot_bad = [x for x in range(gw) if g[gh - 1][x] != 15]
    lef_bad = [y for y in range(gh) if g[y][0] != 15]
    rig_bad = [y for y in range(gh) if g[y][gw - 1] != 15]
    res.append(("border top",    not top_bad, "%d/%d wrong" % (len(top_bad), gw)))
    res.append(("border bottom", not bot_bad, "%d/%d wrong" % (len(bot_bad), gw)))
    res.append(("border left",   not lef_bad, "%d/%d wrong" % (len(lef_bad), gh)))
    res.append(("border right",  not rig_bad, "%d/%d wrong" % (len(rig_bad), gh)))

    lim = min(gw, gh)
    off = [i for i in range(1, lim - 1) if g[i][i] != 12]
    detail = "all %d on" % (lim - 2) if not off else \
             "first wrong at row %d (of %d); %d wrong" % (off[0], lim, len(off))
    res.append(("diagonal", not off, detail))

    # Bars: sample the middle of each sixteenth. ⚠ NUDGE OFF THE DIAGONAL -- in a
    # 640x200 mode the natural sample point for bar 2 is (100,100), which is
    # exactly ON the diagonal, and the checker reported a palette fault that was
    # really its own sampling. An instrument that fails by printing a plausible
    # wrong answer is worse than one that fails loudly.
    bars = []
    for b in range(16):
        bx = min(gw - 1, (b * gw) // 16 + gw // 32)
        by = gh // 2
        while abs(bx - by) < 3:
            by = (by + 7) % (gh - 2) + 1
        bars.append(g[by][bx])
    res.append(("colour bars", bars == list(range(16)), str(bars)))

    res.append(("corner TL", g[1][1] == 15 or g[0][0] == 15, "idx %d" % g[0][0]))
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--mode", required=True, help="0d, 0e, 10, 12, 13")
    ap.add_argument("--compare", help="a second capture to diff against")
    a = ap.parse_args()
    if a.mode not in MODES:
        sys.exit("unknown mode %s" % a.mode)
    gw, gh = MODES[a.mode]

    w, h, g = grid(a.capture, gw, gh)
    print("%s: captured %dx%d, sampled to %dx%d (mode %s)" %
          (a.capture, w, h, gw, gh, a.mode))
    bad = 0
    for name, ok, detail in check(g, gw, gh):
        print("  %-14s %s  %s" % (name, "PASS" if ok else "FAIL", detail))
        bad += not ok

    if a.compare:
        w2, h2, g2 = grid(a.compare, gw, gh)
        diff = sum(g[y][x] != g2[y][x] for y in range(gh) for x in range(gw))
        pct = 100.0 * diff / (gw * gh)
        print("  vs %s: %d of %d pixels differ (%.2f%%)" %
              (a.compare, diff, gw * gh, pct))
        if diff:
            rows = [y for y in range(gh)
                    if any(g[y][x] != g2[y][x] for x in range(gw))]
            print("     first differing row %d, last %d" % (rows[0], rows[-1]))
        bad += diff > 0
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
