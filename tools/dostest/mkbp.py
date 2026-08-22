#!/usr/bin/env python3
"""mkbp.py -- generate a pmbp.txt breakpoint sweep from a disassembly.

WHY. Bisecting a silent VDM death means asking "how far did it get?" over and over,
and hand-picking a dozen addresses per round -- converting file offsets to linear,
checking each site is at least two bytes so the breakpoint footprint does not eat its
neighbour -- is where the time actually goes. This does that mechanically.

    ./mkbp.py <carve.bin> <start-offset> [--count N] [--base LINEAR] [--carve-off OFF]

`--base` is the linear address the carve's offset 0 maps to. For DOS/4GW's aliased
code window (selector 0x67, base 0xd9b0) the carve offset is guest offset + 0x7d10,
so the mapping is linear = 0xd9b0 + (fileoff - 0x7d10), i.e. --base 0xd9b0
--carve-off 0x7d10. Defaults are exactly that case.

Sites of one byte are SKIPPED, not emitted: a breakpoint displaces the byte after the
one you name, and putting one on a single-byte instruction whose neighbour is reachable
some other way corrupts the program -- that trap cost session 17 a wrong conclusion.
"""
import re
import subprocess
import sys

OBJDUMP = "i686-w64-mingw32-objdump"


def disasm(binf, start, length):
    out = subprocess.run(
        [OBJDUMP, "-D", "-b", "binary", "-m", "i8086",
         "--start-address=0x%x" % start, "--stop-address=0x%x" % (start + length), binf],
        capture_output=True, text=True, check=True).stdout
    rows = []
    for line in out.splitlines():
        m = re.match(r"\s+([0-9a-f]+):\t((?:[0-9a-f]{2} )+)\s*\t?(.*)", line)
        if m:
            off = int(m.group(1), 16)
            nbytes = len(m.group(2).split())
            rows.append((off, nbytes, m.group(3).strip()))
    return rows


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    binf, start = sys.argv[1], int(sys.argv[2], 0)
    count, base, carve_off = 32, 0xd9b0, 0x7d10
    args = sys.argv[3:]
    for i, a in enumerate(args):
        if a == "--count":
            count = int(args[i + 1], 0)
        elif a == "--base":
            base = int(args[i + 1], 0)
        elif a == "--carve-off":
            carve_off = int(args[i + 1], 0)

    rows = [r for r in disasm(binf, start, 0x400) if r[1] >= 2]
    if not rows:
        print("no multi-byte instructions found", file=sys.stderr)
        return 1
    # Spread the budget across the span rather than clustering at the start: the
    # question is "how far", so even coverage answers it in one run.
    step = max(1, len(rows) // count)
    picked = rows[::step][:count]

    lines = []
    for off, nb, txt in picked:
        lin = base + (off - carve_off)
        lines.append("%08x   # carve %04x (%d B)  %s" % (lin, off, nb, txt[:40]))
    open("pmbp.txt", "wb").write(("\r\n".join(lines) + "\r\n").encode())
    print("\n".join(lines))
    print("\n%d breakpoints -> pmbp.txt" % len(lines), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
