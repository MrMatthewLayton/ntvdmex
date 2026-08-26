#!/usr/bin/env python3
"""neints.py -- what services does an NE module ask the HOST for?

GH #128. Before krnl386 can initialise, the host has to answer whatever it calls.
Guessing that list is how sessions get spent; this reads it off the binary.

    tools/ne/neints.py guest/ne/krnl386.exe

⚠ METHOD, AND ITS LIMIT. This is a LINEAR SWEEP disassembly (ndisasm) of each code
  segment. Code segments contain data -- jump tables, strings -- so a sweep will
  decode some of it as instructions and invent `int` sites that are not there. This
  project has been burned by exactly that twice: the session-21 INT-site patcher
  matched `CD nn` as a byte pair and rewrote a `jle` displacement, and a session-18
  instrument reported a 30x-over-chance count measured on the wrong bytes.
  So treat the numbers as an UPPER BOUND and a to-do list, not a census. The
  cross-check printed at the end is the count of raw `CD nn` byte pairs: the sweep
  should find FEWER than that, and a sweep finding MORE means the sweep is desynced.
"""
import collections
import re
import subprocess
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from nedump import NE                                             # noqa: E402

# INT numbers whose "function" lives in AH, vs in the whole of AX.
AH_SELECTED = {0x21, 0x10, 0x13, 0x16, 0x1A, 0x33}
LINE = re.compile(r"^([0-9A-F]{8})\s+([0-9A-F]+)\s+(.*)$")


def sweep(data, org):
    p = subprocess.run(["ndisasm", "-b", "16", "-o", hex(org), "-"],
                       input=data, capture_output=True)
    out = []
    for ln in p.stdout.decode("latin1").splitlines():
        m = LINE.match(ln)
        if m:
            out.append((int(m.group(1), 16), m.group(3).strip()))
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    for path in sys.argv[1:]:
        ne = NE(path)
        print(f"\n=== {path}")
        tally = collections.Counter()
        raw = 0
        for s in ne.segments():
            if s["flags"] & 0x0001 or not s["sector"]:      # DATA, or no file bytes
                continue
            body = ne.d[s["file_off"]:s["file_off"] + s["length"]]
            raw += sum(1 for i in range(len(body) - 1) if body[i] == 0xCD)
            ax = ah = None
            for _, txt in sweep(body, 0):
                mv = re.match(r"mov (ax|ah),0x([0-9a-f]+)$", txt)
                if mv:
                    if mv.group(1) == "ax":
                        ax, ah = int(mv.group(2), 16), None
                    else:
                        ah, ax = int(mv.group(2), 16), None
                    continue
                mi = re.match(r"int (?:byte )?0x([0-9a-f]+)$", txt)
                if mi:
                    n = int(mi.group(1), 16)
                    if ah is not None:
                        fn = f"AH={ah:02x}"
                    elif ax is not None:
                        fn = f"AX={ax:04x}" if n not in AH_SELECTED else f"AH={ax >> 8:02x}"
                    else:
                        fn = "AX=?"
                    tally[(n, fn)] += 1
                    ax = ah = None
                    continue
                if re.match(r"(call|jmp|ret|retf|iret|push|pop)", txt):
                    continue                      # these do not clobber AX/AH
                if re.search(r"\b(ax|ah|al|eax)\b", txt):
                    ax = ah = None                # anything else that touches it does
        for n in sorted({k[0] for k in tally}):
            fns = sorted((k[1], v) for k, v in tally.items() if k[0] == n)
            total = sum(v for _, v in fns)
            named = ", ".join(f"{f}x{v}" for f, v in fns if f != "AX=?")
            unk = sum(v for f, v in fns if f == "AX=?")
            print(f"  INT {n:02X}h  {total:4} sites   {named}"
                  + (f"   [+{unk} with AX not statically known]" if unk else ""))
        print(f"  -- sweep found {sum(tally.values())} int sites; "
              f"{raw} raw 0xCD bytes exist (upper bound). "
              f"{'OK' if sum(tally.values()) <= raw else 'SWEEP DESYNCED -- distrust'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
