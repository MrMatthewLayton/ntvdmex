#!/usr/bin/env python3
"""wowdecline.py -- which WOW32 calls does krnl386 let us DECLINE?  GH #128.

krnl386 hooks INT 21h in protected mode and routes some functions to its 32-bit
companion. What makes that tractable is what it does when the companion says no:

    5507  cmp ax, 0xffff
    550a  je  0x55a1          -->  55a1  pop ax / pop bx / pop dx
                                   55a4  jmp 0x56c8
                                   56c8  ... lcall cs:[0x3c]     <-- PREVIOUS INT 21h

`cs:[0x3c]` is the vector krnl386 saved before hooking, so declining a call hands
it to REAL DOS -- which, in this host, is our own working INT 21h layer. So a
sentinel return is not a stub: it is a true statement ("the 32-bit side did not
service this") for which the guest has a correct path already written.

⚠ BUT ONLY WHERE THE CALL SITE SAYS SO. Some sites treat 0xFFFF as a plain error
  and report failure to the app instead of chaining. Declining there would turn
  "not implemented" into "the file does not exist", which is worse -- a wrong
  answer instead of a missing one. So this checks each site rather than assuming
  the family is uniform, and prints exactly which register is tested.

    tools/ne/wowdecline.py guest/ne/krnl386.exe
"""
import struct
import sys

import capstone

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from nedump import NE                                   # noqa: E402
from wowthunks import scan                              # noqa: E402

CHAIN_WINDOW = 0x30        # how far to follow before giving up
MAX_HOPS     = 6           # jmp chains: 55a1 -> 55a4 -> 56c8


def load(path):
    ne = NE(path)
    s = [x for x in ne.segments() if x["i"] == 1][0]
    return ne, bytes(ne.d[s["file_off"]:s["file_off"] + s["length"]])


def reaches_dos_chain(d, md, start, seen=None, depth=0):
    """Does control from `start` reach the `lcall cs:[0x3c]` that re-enters DOS?"""
    if depth > MAX_HOPS:
        return False
    seen = seen if seen is not None else set()
    if start in seen:
        return False
    seen.add(start)
    for ins in md.disasm(d[start:start + CHAIN_WINDOW], start):
        # 2e ff 1e 3c 00 -- lcall cs:[0x3c], the saved previous INT 21h vector
        if ins.bytes[:5] == b"\x2e\xff\x1e\x3c\x00":
            return True
        if ins.mnemonic == "jmp" and ins.op_str.startswith("0x"):
            return reaches_dos_chain(d, md, int(ins.op_str, 16) & 0xFFFF,
                                     seen, depth + 1)
        if ins.mnemonic in ("ret", "retf", "call", "lcall"):
            return False
    return False


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    ne, d = load(path)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
    _n, hits, _t = scan(path)
    stubs = {o: (f, c) for s, o, f, c, _g in hits if s == 1}

    print("%-6s %-6s %-9s %s" % ("ID", "args", "call site", "verdict"))
    ok = 0
    for i in range(len(d) - 3):
        if d[i] != 0xE8:
            continue
        tgt = (i + 3 + struct.unpack_from("<h", d, i + 1)[0]) & 0xFFFF
        if tgt not in stubs:
            continue
        fid, nby = stubs[tgt]
        # Walk forward from the return point looking for a conditional branch on
        # 0xFFFF, then ask whether ITS target reaches the DOS chain.
        verdict, reg = None, None
        after = i + 3
        for ins in md.disasm(d[after:after + CHAIN_WINDOW], after):
            if ins.mnemonic == "cmp" and ins.op_str.endswith("0xffff"):
                reg = ins.op_str.split(",")[0].strip()
            elif ins.mnemonic == "inc":                 # inc dx / jne == test dx==0xffff
                reg = ins.op_str.strip()
            elif ins.mnemonic in ("je", "jz") and ins.op_str.startswith("0x") and reg:
                t = int(ins.op_str, 16) & 0xFFFF
                verdict = ("DECLINE -> real DOS" if reaches_dos_chain(d, md, t)
                           else "0xffff is an ERROR here, not a decline")
                break
            elif ins.mnemonic in ("call", "lcall", "ret", "retf", "jmp"):
                break
        if verdict:
            print("0x%02x   %-6d seg1:0x%04x  %-40s (tests %s)"
                  % (fid, nby, i, verdict, reg))
            if verdict.startswith("DECLINE"):
                ok += 1
    print("\n%d call site(s) where declining hands the call to real DOS." % ok)
    return 0


if __name__ == "__main__":
    sys.exit(main())
