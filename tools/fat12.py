#!/usr/bin/env python3
"""fat12.py -- list or extract files from a raw FAT12 floppy image.

    tools/fat12.py list  <img> [<img> ...]
    tools/fat12.py get   <outdir> <img> [<img> ...]           # everything
    tools/fat12.py get   <outdir> <img> --only NOTEPAD,CALC   # by stem

WHY NOT JUST MOUNT IT. `hdiutil attach` needs privileges, is not available
everywhere this repo gets worked on, and silently does its own filename mangling.
FAT12 is a documented on-disk layout and reading it is sixty lines, so the
extraction is exact, needs nothing installed, and produces the same bytes on any
machine.

⚠ THE GEOMETRY IS READ FROM THE BPB, NOT ASSUMED. Every field this needs --
  bytes per sector, sectors per cluster, reserved sectors, FAT count, root entry
  count, sectors per FAT -- is in the boot sector of the image in front of us.
  Hardcoding 1.44 MB numbers would work for these eight disks and quietly corrupt
  the first 720 KB image anyone tried afterwards.

⚠ THIS DOES NOT DECOMPRESS. Windows 3.1x distribution files are stored compressed
  (NOTEPAD.EX_), and the decompressor that matters is EXPAND.EXE on the target
  machine, which is where the file has to end up anyway. Extract here, expand
  there -- see scripts/bm/w16grab.bat.
"""
import os
import struct
import sys


class Fat12:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        d = self.d
        self.bps = struct.unpack_from("<H", d, 0x0B)[0]
        self.spc = d[0x0D]
        self.res = struct.unpack_from("<H", d, 0x0E)[0]
        self.nfat = d[0x10]
        self.nroot = struct.unpack_from("<H", d, 0x11)[0]
        self.spf = struct.unpack_from("<H", d, 0x16)[0]
        if not self.bps or not self.spc or not self.spf:
            raise SystemExit("%s: not a FAT12 image (BPB is empty)" % path)
        self.fat_off = self.res * self.bps
        self.root_off = self.fat_off + self.nfat * self.spf * self.bps
        self.data_off = self.root_off + self.nroot * 32

    def fat_next(self, cl):
        """The FAT12 entry for cluster `cl` -- 12 bits, packed two per three bytes."""
        i = self.fat_off + (cl * 3) // 2
        v = self.d[i] | (self.d[i + 1] << 8)
        return (v >> 4) if (cl & 1) else (v & 0xFFF)

    def read(self, first, size):
        out = bytearray()
        cl = first
        while 2 <= cl < 0xFF0 and len(out) < size:
            off = self.data_off + (cl - 2) * self.spc * self.bps
            out += self.d[off:off + self.spc * self.bps]
            cl = self.fat_next(cl)
        return bytes(out[:size])

    def entries(self):
        for i in range(self.nroot):
            e = self.d[self.root_off + i * 32: self.root_off + i * 32 + 32]
            if not e or e[0] in (0x00,):
                break
            if e[0] == 0xE5 or (e[11] & 0x08):        # deleted, or the volume label
                continue
            if e[11] & 0x10:                          # a directory
                continue
            name = e[0:8].decode("latin1").rstrip()
            ext = e[8:11].decode("latin1").rstrip()
            first = struct.unpack_from("<H", e, 26)[0]
            size = struct.unpack_from("<I", e, 28)[0]
            yield (name + ("." + ext if ext else ""), first, size)


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        print(__doc__)
        return 2
    cmd = a[0]
    only = None
    if "--only" in a:
        only = {s.strip().upper() for s in a[a.index("--only") + 1].split(",")}
        i = a.index("--only")
        a = a[:i] + a[i + 2:]

    if cmd == "list":
        for img in a[1:]:
            f = Fat12(img)
            print("== %s  (%d B/sec, %d sec/clus, %d root entries)"
                  % (os.path.basename(img), f.bps, f.spc, f.nroot))
            for name, _first, size in f.entries():
                print("   %-14s %8d" % (name, size))
        return 0

    if cmd == "get":
        outdir = a[1]
        os.makedirs(outdir, exist_ok=True)
        n = 0
        for img in a[2:]:
            f = Fat12(img)
            for name, first, size in f.entries():
                stem = name.split(".")[0].upper()
                if only is not None and stem not in only:
                    continue
                open(os.path.join(outdir, name), "wb").write(f.read(first, size))
                n += 1
                print("   %-14s %8d  <- %s" % (name, size, os.path.basename(img)))
        print("%d file(s) -> %s" % (n, outdir))
        return 0

    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
