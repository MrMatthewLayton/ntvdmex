#!/usr/bin/env python3
"""bmp2png.py -- turn a rig screenshot into something that can actually be looked at.

    tools/bmp2png.py <in.bmp> <out.png> [--scale N] [--crop X,Y,W,H]

WHY THIS EXISTS. The bare-metal rig has no VNC, so `rigshot shot` writing a BMP to
the SMB share is the only eye this project has on what a run DREW -- and a 1680x1050
24-bit BMP is neither viewable here nor small enough to carry around. `sips` refuses
these files, so the conversion is done by hand: BMP is bottom-up BGR rows padded to
4 bytes, PNG is top-down RGB with a filter byte per row, and zlib is in the standard
library. No dependency, nothing to install on a fresh machine.

`--crop` is the part that matters in practice. A full desktop shot is mostly wallpaper;
the question is always about one window, and cropping to it BEFORE scaling is what keeps
the pixels the question is about at their original size.
"""
import struct
import sys
import zlib


def read_bmp(path):
    d = open(path, "rb").read()
    if d[:2] != b"BM":
        raise SystemExit("%s: not a BMP" % path)
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    if bpp not in (24, 32):
        raise SystemExit("%s: %d bpp not handled" % (path, bpp))
    stride = ((w * bpp // 8) + 3) // 4 * 4
    return d, off, w, abs(h), bpp, stride, h > 0


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        print(__doc__)
        return 2
    src, dst = a[0], a[1]
    scale = 1
    zoom = 1
    crop = None
    if "--scale" in a:
        scale = int(a[a.index("--scale") + 1])
    if "--zoom" in a:
        # Nearest-neighbour magnification. A 16x16 tray icon is unreadable at 1:1
        # in any viewer, and blurring it to read it would be inventing pixels.
        zoom = int(a[a.index("--zoom") + 1])
    if "--crop" in a:
        crop = [int(v) for v in a[a.index("--crop") + 1].split(",")]

    d, off, w, h, bpp, stride, bottom_up = read_bmp(src)
    px = bpp // 8
    cx, cy, cw, ch = crop if crop else (0, 0, w, h)
    cw = min(cw, w - cx)
    ch = min(ch, h - cy)
    ow, oh = cw // scale, ch // scale
    sys.stderr.write("%s: %dx%d %dbpp -> %dx%d\n" % (src, w, h, bpp, ow, oh))

    rows = []
    for y in range(oh):
        sy = cy + y * scale
        if bottom_up:
            sy = h - 1 - sy
        base = off + sy * stride
        line = bytearray()
        for x in range(ow):
            p = base + (cx + x * scale) * px
            line += bytes((d[p + 2], d[p + 1], d[p])) * zoom     # BGR -> RGB
        for _ in range(zoom):
            rows.append(b"\x00" + bytes(line))
    ow *= zoom
    oh *= zoom

    def chunk(t, data):
        c = t + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", ow, oh, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 6))
    png += chunk(b"IEND", b"")
    open(dst, "wb").write(png)
    return 0


if __name__ == "__main__":
    sys.exit(main())
