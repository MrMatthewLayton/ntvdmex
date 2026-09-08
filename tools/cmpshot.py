#!/usr/bin/env python3
"""cmpshot.py -- diff a guest's window under NTVDMEX against the same window under
STOCK ntvdm.  GH #128, session 57.

    tools/cmpshot.py <share-dir> [NAME ...]

Reads the pairs `cmp_<NAME>_ours.bmp` / `cmp_<NAME>_stock.bmp` that
scripts/bm/cmpsweep.bat leaves on the share, together with the `rigshot list`
output beside each one, and answers ONE question per guest:

    does our window look like the window Windows itself draws?

── WHY A DIFF AND NOT A LOOK ────────────────────────────────────────────────
Three user reports of "this looks wrong" have been settled this way; TWO OF THEM
WERE REFUTED -- the flat Win16 control look and the Win16 chrome are what BOTH
hosts give a Win16 program on XP.  A screenshot of ours alone cannot settle any
of it, and an eye cannot count pixels.

⚠ THE WINDOW IS CROPPED OUT OF THE DESKTOP BY ITS OWN RECTANGLE, not compared
  whole.  The two halves run at different moments with different windows behind
  them, and the desktop keeps stale pixels; comparing full screens would report
  differences that belong to the console window in the corner.

⚠ AND THE POSITION IS NOT THE MEASUREMENT.  A guest that centres itself (TASKMAN)
  or is placed by the OS lands where it lands; what has to match is the SIZE and
  the CONTENT.  A size difference is reported as the finding it is, and the pixel
  comparison is then skipped rather than being done against a shifted image and
  reported as 100% different.
"""
import os, re, struct, sys


def read_bmp(path):
    """(w, h, rows) with rows[0] = TOP row, each row a bytes() of BGR triples."""
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:2] != b"BM":
        raise ValueError("%s: not a BMP" % path)
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp != 24:
        raise ValueError("%s: %d bpp, expected 24" % (path, bpp))
    stride = ((w * 3 + 3) // 4) * 4
    flip = h > 0
    h = abs(h)
    rows = []
    for y in range(h):
        src = (h - 1 - y) if flip else y
        start = off + src * stride
        rows.append(data[start:start + w * 3])
    return w, h, rows


WIN_RE = re.compile(r"win:\s+(.*?)\s+win=\((-?\d+),(-?\d+)\)\s+(\d+)x(\d+)")


def windows(path):
    """[(caption, x, y, w, h)] from a `rigshot list` dump."""
    out = []
    try:
        with open(path, errors="replace") as fh:
            for line in fh:
                m = WIN_RE.search(line)
                if m:
                    out.append((m.group(1).strip(), int(m.group(2)), int(m.group(3)),
                                int(m.group(4)), int(m.group(5))))
    except OSError:
        pass
    return out


# Windows that belong to the harness, not to the guest.  Matched on the caption
# because that is what rigshot reports; anything else in the list is the guest's.
SKIP = ("cmd.exe", "NTVDMEX test watcher", "Program Manager")


def guest_window(listfile, name):
    """The guest's own top-level window, or None.

    ⚠ PROGMAN IS THE TRAP HERE: the Win16 Program Manager's caption is exactly
      the Win32 shell's, so a name match alone would compare our window against
      the DESKTOP.  The shell's window is full-screen and at (0,0); a guest's is
      not, so the desktop-sized one is dropped.
    """
    wins = windows(listfile)
    cands = []
    for cap, x, y, w, h in wins:
        desktop = (x == 0 and y == 0 and w >= 1280 and h >= 900)
        if "cmd.exe" in cap or "NTVDMEX test watcher" in cap:
            continue                       # the harness's own consoles
        if "Program Manager" in cap and desktop:
            continue                       # the Win32 SHELL, not the guest
        if desktop:
            continue
        cands.append((cap, x, y, w, h))
    if not cands:
        return None
    # Prefer one whose caption mentions the program; otherwise the first left.
    for c in cands:
        if name.lower() in c[0].lower().replace(" ", ""):
            return c
    return cands[0]


def crop(rows, W, H, x, y, w, h):
    x, y = max(x, 0), max(y, 0)
    w = min(w, W - x)
    h = min(h, H - y)
    if w <= 0 or h <= 0:
        return None, 0, 0
    return [r[x * 3:(x + w) * 3] for r in rows[y:y + h]], w, h


def compare(share, name):
    ours_bmp = os.path.join(share, "cmp_%s_ours.bmp" % name)
    stock_bmp = os.path.join(share, "cmp_%s_stock.bmp" % name)
    ours_lst = os.path.join(share, "cmp_%s_ours.txt" % name)
    stock_lst = os.path.join(share, "cmp_%s_stock.txt" % name)
    for p in (ours_bmp, stock_bmp):
        if not os.path.isfile(p):
            print("  %-10s -- no capture (%s missing)" % (name, os.path.basename(p)))
            return
    ow = guest_window(ours_lst, name)
    sw = guest_window(stock_lst, name)
    if not ow or not sw:
        print("  %-10s ★ NO WINDOW %s -- one host put nothing on the desktop, which is"
              " the finding" % (name, "under OURS" if not ow else "under STOCK"))
        return
    W1, H1, r1 = read_bmp(ours_bmp)
    W2, H2, r2 = read_bmp(stock_bmp)
    c1, w1, h1 = crop(r1, W1, H1, ow[1], ow[2], ow[3], ow[4])
    c2, w2, h2 = crop(r2, W2, H2, sw[1], sw[2], sw[3], sw[4])
    print("  %-10s ours %-28s %dx%d at (%d,%d)" % (name, '"%s"' % ow[0], ow[3], ow[4], ow[1], ow[2]))
    print("  %-10s stock%-28s %dx%d at (%d,%d)" % ("", '"%s"' % sw[0], sw[3], sw[4], sw[1], sw[2]))
    if (w1, h1) != (w2, h2):
        print("             ★ DIFFERENT SIZE (%dx%d vs %dx%d) -- that IS the difference;"
              " no pixel comparison" % (w1, h1, w2, h2))
        return
    diff = 0
    rowdiff = []
    for y in range(h1):
        a, b = c1[y], c2[y]
        if a == b:
            rowdiff.append(0)
            continue
        n = sum(1 for i in range(0, len(a), 3) if a[i:i+3] != b[i:i+3])
        diff += n
        rowdiff.append(n)
    total = w1 * h1
    pct = 100.0 * diff / total if total else 0.0
    print("             %d of %d pixels differ (%.2f%%)" % (diff, total, pct))
    if diff:
        # Where, in bands, so a reader can say "the display strip" rather than "somewhere".
        band = max(1, h1 // 10)
        worst = []
        for i in range(0, h1, band):
            n = sum(rowdiff[i:i + band])
            if n:
                worst.append((n, i, min(i + band, h1)))
        worst.sort(reverse=True)
        for n, a, b in worst[:3]:
            print("               rows %4d..%-4d %7d px  (%.1f%% of the window)"
                  % (a, b, n, 100.0 * n / total))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    share = sys.argv[1]
    names = sys.argv[2:]
    if not names:
        names = sorted({f[4:-9] for f in os.listdir(share)
                        if f.startswith("cmp_") and f.endswith("_ours.bmp")})
    print("== NTVDMEX vs STOCK ntvdm, window by window ==")
    for n in names:
        compare(share, n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
