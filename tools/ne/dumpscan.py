#!/usr/bin/env python3
"""dumpscan.py -- find an NE's loaded segments inside a vdmdump dump.

vdmdump.exe brings back a VDM's low megabyte (<prefix>.bin, based at linear 0)
and a set of 64KB blocks from anywhere in the process (<prefix>.blk). This reads
both as one sparse address space, locates every segment image of an NE file in
it, and prints -- per candidate -- the base, how well it matches the file, and
exactly which bytes differ.

The bytes that differ ARE the loader's work: a correct loader's relocations. That
is the thing session 32 could not see and had to guess at.

    python3 tools/ne/dumpscan.py guest/ne/krnl386.exe build/stockdumps/NNN/stockdump

Reads <prefix>.bin and <prefix>.blk if present.
"""
import sys
import struct
from nedump import NE

PROBE = 512             # bytes of segment head used to propose a candidate base
MIN_SCORE = 0.55        # a genuine image matches far better than this


class Space:
    """A sparse address space assembled from a low dump plus loose blocks."""

    def __init__(self):
        self.chunks = []                        # (base, bytes)

    def add(self, base, data):
        if data:
            self.chunks.append((base, data))

    def load(self, prefix):
        try:
            with open(prefix + ".bin", "rb") as f:
                self.add(0, f.read())
        except FileNotFoundError:
            pass
        try:
            blk = open(prefix + ".blk", "rb").read()
        except FileNotFoundError:
            return self
        o = 0
        while o + 12 <= len(blk):
            if blk[o:o + 4] != b"BLK1":
                break
            addr, ln = struct.unpack_from("<II", blk, o + 4)
            self.add(addr, blk[o + 12:o + 12 + ln])
            o += 12 + ln
        return self

    def read(self, addr, n):
        """Bytes at addr, or None if no single chunk covers the whole range."""
        for base, data in self.chunks:
            if base <= addr and addr + n <= base + len(data):
                return data[addr - base:addr - base + n]
        return None

    def find(self, needle):
        out = []
        for base, data in self.chunks:
            i = data.find(needle)
            while i >= 0:
                out.append(base + i)
                i = data.find(needle, i + 1)
        return out


def score(img, mem):
    same = sum(1 for a, b in zip(img, mem) if a == b)
    return same / max(1, len(img))


def diff_runs(img, mem, gap=4):
    """Differing byte ranges, coalescing runs closer together than `gap`."""
    runs = []
    i = 0
    n = min(len(img), len(mem))
    while i < n:
        if img[i] != mem[i]:
            s = i
            last = i
            while i < n and i - last <= gap:
                if img[i] != mem[i]:
                    last = i
                i += 1
            runs.append((s, last + 1))
            i = last + 1
        else:
            i += 1
    return runs


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    ne = NE(sys.argv[1])
    sp = Space().load(sys.argv[2])
    total = sum(len(d) for _, d in sp.chunks)
    print("dump: %d chunks, %d bytes, bases %s"
          % (len(sp.chunks), total,
             " ".join("0x%08x" % b for b, _ in sp.chunks)))

    for seg in ne.segments():
        img = ne.d[seg["file_off"]:seg["file_off"] + seg["length"]]
        print("\n== segment %d  file 0x%06x  len %d (0x%x)  flags 0x%04x"
              % (seg["i"], seg["file_off"], seg["length"], seg["length"],
                 seg["flags"]))

        # Propose bases from unique-enough windows taken across the WHOLE image,
        # not just its head: a relocated copy differs most densely at the start
        # (krnl386's segment 1 has 160 patched bytes in its first 0x100), and
        # probing only the head loses exactly the copies we care about.
        cands = set()
        for off in range(0, max(1, len(img) - 24), 0x80):
            probe = img[off:off + 24]
            if len(set(probe)) < 8:
                continue
            for a in sp.find(probe):
                cands.add(a - off)
        if not cands:
            print("   not found")
            continue

        scored = []
        for base in cands:
            mem = sp.read(base, len(img))
            if mem is None:
                continue
            scored.append((score(img, mem), base, mem))
        scored.sort(reverse=True, key=lambda t: (t[0], -t[1]))

        for sc, base, mem in scored:
            if sc < MIN_SCORE:
                continue
            runs = diff_runs(img, mem)
            nd = sum(e - s for s, e in runs)
            print("   base 0x%08x  match %.4f  %d differing bytes in %d runs"
                  % (base, sc, nd, len(runs)))
            for s, e in runs[:24]:
                print("      +0x%04x  file %s  mem %s"
                      % (s, img[s:e].hex(), mem[s:e].hex()))
            if len(runs) > 24:
                print("      ... %d more runs" % (len(runs) - 24))
    return 0


if __name__ == "__main__":
    sys.exit(main())
