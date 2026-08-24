#!/usr/bin/env python3
"""doomref.py -- pixel-exact oracle for the video path, built from DOOM1.WAD itself.

WHY THE WAD AND NOT A SCREENSHOT. Every fixed screen Doom draws is a lump in the IWAD,
stored in a documented format with a documented palette, so the bytes that SHOULD reach
the framebuffer are computable exactly. A screenshot off someone else's machine is a
photograph of an answer; the WAD is the answer. It also needs no rig run of its own --
the host already self-captures frames to shotNN.bmp -- and it localises a fault to the
pixel instead of to a feeling.

WHAT IT REPLACES. Up to now the video path was being judged by "even-column match", a
duplication detector, and that metric actively misled: when the plane-remap fix landed
the score went UP (0.55 -> 0.70) because real Doom content has many equal neighbours and
the lower number had been our own misattributed columns scoring as detail. A metric that
moves the wrong way when a bug is fixed is worse than no metric.

    ./doomref.py <DOOM1.WAD> list                       # what is in there
    ./doomref.py <DOOM1.WAD> dump TITLEPIC out.bmp      # the reference image
    ./doomref.py <DOOM1.WAD> cmp  TITLEPIC shot03.bmp   # oracle: exact diff
    ./doomref.py <DOOM1.WAD> cmp  STBAR shot09.bmp --at 0,168

Exit status is nonzero on a mismatch, so it can gate a run.
"""
import struct, sys


def read_wad(path):
    d = open(path, 'rb').read()
    sig, n, off = struct.unpack_from('<4sii', d, 0)
    if sig not in (b'IWAD', b'PWAD'):
        raise SystemExit("not a WAD: %r" % sig)
    lumps = {}
    order = []
    for i in range(n):
        lo, ls, nm = struct.unpack_from('<ii8s', d, off + i * 16)
        nm = nm.rstrip(b'\0').decode('ascii', 'replace')
        lumps[nm] = (lo, ls)
        order.append(nm)
    return d, lumps, order


def palette(d, lumps, index=0):
    lo, ls = lumps['PLAYPAL']
    base = lo + index * 768
    return [tuple(d[base + i * 3: base + i * 3 + 3]) for i in range(256)]


def decode_patch(d, lo, ls):
    """Doom's column-major patch format -> (w, h, [row bytes], transparent mask).

    Posts are (topdelta, length, pad, data[length], pad). topdelta 0xFF ends a column.
    Pixels no post covers are TRANSPARENT and must not be compared -- for a full-screen
    lump like TITLEPIC every pixel is covered, but STBAR has holes where the face and
    the numbers get drawn over it later.
    """
    w, h, _lx, _ty = struct.unpack_from('<hhhh', d, lo)
    colofs = struct.unpack_from('<%di' % w, d, lo + 8)
    px = [bytearray(w) for _ in range(h)]
    opaque = [bytearray(w) for _ in range(h)]
    for x in range(w):
        p = lo + colofs[x]
        while True:
            top = d[p]
            if top == 0xFF:
                break
            ln = d[p + 1]
            p += 3                                   # topdelta, length, leading pad
            for i in range(ln):
                y = top + i
                if 0 <= y < h:
                    px[y][x] = d[p + i]
                    opaque[y][x] = 1
            p += ln + 1                              # data + trailing pad
    return w, h, px, opaque


def write_bmp(path, w, h, rows, pal):
    stride = (w + 3) // 4 * 4
    hdr = struct.pack('<2sIHHI', b'BM', 14 + 40 + 1024 + stride * h, 0, 0, 14 + 40 + 1024)
    ih = struct.pack('<IiiHHIIiiII', 40, w, h, 1, 8, 0, stride * h, 2835, 2835, 256, 256)
    pl = b''.join(bytes((b, g, r, 0)) for (r, g, b) in pal)
    body = b''.join(bytes(rows[y]) + b'\0' * (stride - w) for y in range(h - 1, -1, -1))
    open(path, 'wb').write(hdr + ih + pl + body)


def read_bmp(path):
    d = open(path, 'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]
    w, h = struct.unpack_from('<ii', d, 18)
    bpp = struct.unpack_from('<H', d, 28)[0]
    if bpp != 8:
        raise SystemExit("%s is %d bpp; the oracle compares PALETTE INDICES, so it "
                         "needs the 8bpp capture" % (path, bpp))
    stride = (w * bpp // 8 + 3) // 4 * 4
    rows = [d[off + y * stride: off + y * stride + w] for y in range(abs(h))]
    if h > 0:
        rows.reverse()
    return w, abs(h), rows


def cmp_lump(d, lumps, name, shot, at):
    lo, ls = lumps[name]
    rw, rh, ref, opaque = decode_patch(d, lo, ls)
    sw, sh, got = read_bmp(shot)
    ax, ay = at
    bad = tested = 0
    firstbad = None
    badrows = {}
    for y in range(rh):
        gy = ay + y
        if gy >= sh:
            break
        for x in range(rw):
            gx = ax + x
            if gx >= sw or not opaque[y][x]:
                continue
            tested += 1
            if got[gy][gx] != ref[y][x]:
                bad += 1
                badrows[y] = badrows.get(y, 0) + 1
                if firstbad is None:
                    firstbad = (x, y, got[gy][gx], ref[y][x])
    print("%s vs %s at %d,%d : %d of %d compared pixels differ (%.3f%%)"
          % (name, shot, ax, ay, bad, tested, 100.0 * bad / tested if tested else 0))
    if bad:
        x, y, g, r = firstbad
        print("   first at x=%d y=%d  got index %d, want %d" % (x, y, g, r))
        worst = sorted(badrows.items(), key=lambda kv: -kv[1])[:6]
        print("   worst rows: " + ", ".join("y=%d:%d" % (k, v) for k, v in worst))
        # A column-duplication signature is worth naming outright, because it is the
        # failure this project keeps producing.
        dup = sum(1 for y in range(rh) for x in range(0, rw - 1, 2)
                  if opaque[y][x] and opaque[y][x + 1]
                  and ay + y < sh and ax + x + 1 < sw
                  and got[ay + y][ax + x] == got[ay + y][ax + x + 1])
        tot = sum(1 for y in range(rh) for x in range(0, rw - 1, 2)
                  if opaque[y][x] and opaque[y][x + 1] and ay + y < sh and ax + x + 1 < sw)
        print("   even-column duplication in the CAPTURE: %.3f (reference: %.3f)"
              % (dup / tot if tot else 0,
                 sum(1 for y in range(rh) for x in range(0, rw - 1, 2)
                     if opaque[y][x] and opaque[y][x + 1] and ref[y][x] == ref[y][x + 1])
                 / tot if tot else 0))
    return bad


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    wad = sys.argv[1]
    cmd = sys.argv[2]
    d, lumps, order = read_wad(wad)
    if cmd == 'list':
        for nm in order:
            lo, ls = lumps[nm]
            print("%-10s %8d %8d" % (nm, lo, ls))
        return 0
    if cmd == 'dump':
        name, out = sys.argv[3], sys.argv[4]
        lo, ls = lumps[name]
        w, h, px, _ = decode_patch(d, lo, ls)
        write_bmp(out, w, h, px, palette(d, lumps))
        print("wrote %s (%dx%d)" % (out, w, h))
        return 0
    if cmd == 'cmp':
        name, shot = sys.argv[3], sys.argv[4]
        at = (0, 0)
        if '--at' in sys.argv:
            at = tuple(int(v) for v in sys.argv[sys.argv.index('--at') + 1].split(','))
        return 1 if cmp_lump(d, lumps, name, shot, at) else 0
    raise SystemExit(__doc__)


if __name__ == '__main__':
    sys.exit(main())
