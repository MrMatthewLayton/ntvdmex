#!/usr/bin/env python3
"""bopmap.py -- read the BOP dispatch table out of XP's ntvdm.exe.

    ./tools/ntvdm/bopmap.py guest/ntvdm/ntvdm.exe [--sub 0x54]

WHY THIS IS A SCRIPT AND NOT A NOTE IN A DOC. The table is the only authoritative
statement of which BOPs NTVDM implements and what their sub-functions are, and a
table transcribed by hand into markdown is a claim that rots. Re-run it against any
ntvdm.exe -- another service pack, another Windows version -- and the answer is
measured rather than remembered. See docs/inventory/bop.md.

⚠ The 195 unimplemented entries all share ONE stub address; that is what makes the
   implemented set exact rather than a guess about which pointers look plausible.
"""
import struct, sys

def sections(d):
    e = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, e + 6)[0]
    hdr = e + 24 + struct.unpack_from('<H', d, e + 20)[0]
    base = struct.unpack_from('<I', d, e + 24 + 28)[0]
    out = []
    for i in range(nsec):
        o = hdr + i * 40
        name = d[o:o+8].rstrip(b'\0').decode()
        vsz, va, rsz, ptr = struct.unpack_from('<IIII', d, o + 8)
        out.append((name, vsz, va, rsz, ptr))
    return base, out

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'guest/ntvdm/ntvdm.exe'
    d = open(path, 'rb').read()
    base, secs = sections(d)
    def foff(va):
        rva = va - base
        for n, vsz, sva, rsz, ptr in secs:
            if sva <= rva < sva + max(vsz, rsz):
                return ptr + (rva - sva)
        return None
    text = [s for s in secs if s[0] == '.text'][0]
    lo, hi = base + text[2], base + text[2] + text[1]
    # Find the 256-entry table: the only long run of DWORDs that are all .text VAs.
    best = None
    i = 0
    while i < len(d) - 4:
        v = struct.unpack_from('<I', d, i)[0]
        if lo <= v < hi:
            j, n = i, 0
            while j < len(d) - 4 and lo <= struct.unpack_from('<I', d, j)[0] < hi:
                j += 4; n += 1
            if n >= 256 and best is None:
                best = i
            i = j
        else:
            i += 4
    if best is None:
        print('no 256-entry table found', file=sys.stderr); return 1
    t = [struct.unpack_from('<I', d, best + 4 * k)[0] for k in range(256)]
    from collections import Counter
    stub, nstub = Counter(t).most_common(1)[0]
    print('image base 0x%08x   table file 0x%06x' % (base, best))
    print('unimplemented stub 0x%08x (%d of 256)  => %d real BOPs'
          % (stub, nstub, 256 - nstub))
    impl = [k for k, v in enumerate(t) if v != stub]
    print('implemented: ' + ' '.join('%02X' % k for k in impl))
    return 0

if __name__ == '__main__':
    sys.exit(main())
