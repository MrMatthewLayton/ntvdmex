#!/usr/bin/env python3
"""nedis.py -- disassemble a segment of a 16-bit NE, with WOW32 stubs named.  GH #128.

The WOW32 surface is 82 undocumented function IDs (docs/research/wow32-call-surface.md)
and the ONLY documentation that exists is krnl386's own code. Every one of them has to
be worked out the same way: find the stub, find who calls it, read what the caller
pushes, and name the arguments from what the caller did to build them.

That is a disassembly job, done ~82 times, so it gets a tool rather than a throwaway.

    tools/ne/nedis.py guest/ne/krnl386.exe 1 0x65c0 0x60      # a window of seg 1
    tools/ne/nedis.py guest/ne/krnl386.exe --callers 0xb48e   # who calls this stub
    tools/ne/nedis.py guest/ne/krnl386.exe --wowfunc 0xb8     # ★ the whole story for one ID

⚠ THE BYTES ARE THE UNRELOCATED FILE IMAGE. Every `mov ax,<seg>` / far pointer in the
  output is a relocation SITE holding a chain link, not a value -- the loader overwrites
  it. Do not read segment values out of this output; offsets within the segment are real.
"""
import struct
import sys

import capstone

sys.path.insert(0, __file__.rsplit("/", 1)[0])          # runnable from anywhere
from nedump import NE                                   # noqa: E402

# The per-function stub shape, from wowthunks.py. Repeated here so a disassembly can
# label `call 0xb48e` as "WOW32 id 0xb8" inline -- which is most of the value.
try:
    from wowthunks import scan as _wowscan
except ImportError:                                            # pragma: no cover
    _wowscan = None


def wow_stubs(path):
    """{stub offset -> (id, arg bytes)} for segment 1, or {} if unavailable."""
    if not _wowscan:
        return {}
    out = {}
    try:
        _ne, hits, _t = _wowscan(path)
        for seg, off, fid, cnt, _tgt in hits:
            if seg == 1:
                out[off] = (fid, cnt)
    except Exception:                                          # noqa: BLE001
        return {}
    return out


def seg_bytes(ne, n):
    s = [x for x in ne.segments() if x["i"] == n][0]
    return s, ne.d[s["file_off"]:s["file_off"] + s["length"]]


def _disasm_resync(md, d, start, end, lines):
    """Yield instructions from `start` to `end`, stepping over undecodable bytes.

    capstone stops dead at the first byte it cannot decode. This walks the window,
    restarting one byte past every stall and recording a `db` line (into `lines`, in
    order -- the consumer appends each instruction as it is yielded, so laziness keeps
    the interleaving right) so a stall is VISIBLE rather than being an empty window.
    """
    # Feed capstone up to 15 bytes PAST the window so the instruction straddling the end
    # still decodes; stop yielding at `end`. Without the overrun the last instruction is
    # truncated, fails to decode, and gets reported as `db ... not decodable` -- a window
    # edge masquerading as a data byte, which is the same lie one level down.
    lim = min(end + 15, len(d))
    pos = start
    while pos < end:
        progressed = False
        for ins in md.disasm(bytes(d[pos:lim]), pos):
            if ins.address >= end:
                return
            progressed = True
            yield ins
            pos = ins.address + ins.size
        if not progressed:
            b = d[pos]
            ch = chr(b) if 0x20 <= b < 0x7F else "."
            lines.append("  %04x  %-20s %-8s 0x%02x%s"
                         % (pos, "%02x" % b, "db", b,
                            "   ; '%s' -- not decodable, resyncing" % ch))
            pos += 1


def disasm(ne, segno, start, count, stubs=None, out=None):
    """Print `count` bytes of segment `segno` from `start`, annotated."""
    s, d = seg_bytes(ne, segno)
    stubs = stubs or {}
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
    md.detail = False
    end = min(start + count, len(d))
    lines = []
    # ⚠ RESYNC ON UNDECODABLE BYTES, AND SAY SO. capstone's disasm() STOPS at the first
    #   byte it cannot decode and returns what it had. So a start offset that lands mid
    #   instruction -- or a segment with strings embedded in it, which krnl386's seg2 has
    #   at offset 0 -- produced SILENCE, and silence from a disassembler reads as "there
    #   is no code here" rather than "I gave up on byte one". Session 35 lost time to
    #   exactly that on seg2:0x0f30 before dumping the bytes by hand. Emit `db` and step
    #   one byte on instead, so the output always spans the window it was asked for.
    for ins in _disasm_resync(md, d, start, end, lines):
        note = ""
        # ⚠ MASK BRANCH TARGETS TO 16 BITS. capstone computes target = address + rel
        #   and does not wrap, so disassembling from a high offset prints
        #   `call 0x11493` for what the CPU executes as `call 0x1493` -- an address
        #   that does not exist in the segment, in a tool whose entire job is to name
        #   addresses. Session 32 lost a few minutes to it; session 33 fixed it.
        op_str = ins.op_str
        if (ins.mnemonic[0] == "j" or ins.mnemonic.startswith(("call", "loop"))) \
                and op_str.startswith("0x"):
            try:
                op_str = "0x%04x" % (int(op_str, 16) & 0xFFFF)
            except ValueError:
                pass
        # ★ Name the WOW32 call in place. `call 0xb48e` means nothing; "WOW32 id 0xb8
        #   (16 arg bytes)" is the thing you are actually looking at.
        if ins.mnemonic == "call" and op_str.startswith("0x"):
            t = int(op_str, 16)
            if t in stubs:
                note = "   ; ★ WOW32 id 0x%02x (%d arg bytes)" % stubs[t]
        # A native BOP -- krnl386's own 16->32 escape. Same reason.
        if ins.bytes[:2] == b"\xc4\xc4":
            note = "   ; ★ NATIVE BOP 0x%02x" % ins.bytes[2] if len(ins.bytes) > 2 else ""
        lines.append("  %04x  %-20s %-8s %s%s"
                     % (ins.address, ins.bytes.hex(), ins.mnemonic, op_str, note))
    text = "\n".join(lines)
    if out is None:
        print(text)
    return text


def callers(ne, segno, target):
    """Every near/far call OR JUMP to `target` within a segment. Returns offsets.

    ⚠ THIS USED TO COUNT `call` ONLY, AND THAT MADE IT LIE. WOW32 id 0x74 is
      reached from `seg1:0x9822 jmp 0xb1d0`, preceded by `push cs / push 0x985c` --
      a hand-built far call whose return address is deliberately NOT the next
      instruction. The tool reported "0 caller(s)", which reads as "nothing calls
      this", and the truth was "something calls this in the one way I did not
      look for". A tail-jump into a thunk is the normal shape here, not an oddity.
    """
    s, d = seg_bytes(ne, segno)
    hits = []
    for i in range(len(d) - 3):
        if d[i] == 0xE8 or d[i] == 0xE9:                     # near call / near jmp
            rel = struct.unpack_from("<h", d, i + 1)[0]
            if ((i + 3 + rel) & 0xFFFF) == target:
                hits.append(i)
        elif d[i] == 0xEB:                                   # short jmp
            rel = struct.unpack_from("<b", d, i + 1)[0]
            if ((i + 2 + rel) & 0xFFFF) == target:
                hits.append(i)
        elif d[i] == 0x9A or d[i] == 0xEA:                   # far call / far jmp
            if struct.unpack_from("<H", d, i + 1)[0] == target:
                hits.append(i)
    return hits


def wowfunc(ne, path, fid, back=0x50):
    """★ The whole story for one WOW32 function ID: stub, callers, and the code that
    builds the arguments. This is the unit of work for #128, so it is one command."""
    stubs = wow_stubs(path)
    at = [o for o, (i, _) in stubs.items() if i == fid]
    if not at:
        print("no stub for id 0x%02x in %s" % (fid, path))
        return 1
    for a in at:
        nby = stubs[a][1]
        print("=== WOW32 id 0x%02x -- stub at seg1:0x%04x, %d arg bytes (%d words)"
              % (fid, a, nby, nby // 2))
        print("--- the stub itself")
        disasm(ne, 1, a, 0x14, stubs)
        # ⚠ SCAN EVERY SEGMENT, NOT JUST SEGMENT 1. The stubs all live in seg1, but the
        #   code that CALLS them does not: WowLoadModule (0x2d) is called once, from
        #   seg2:0x0f6d, and scanning seg1 alone reported "0 caller(s)" -- a tool saying
        #   an ID is never used when it is the very call the run stops on. Session 34
        #   already lost time to a call site printed without its segment; this is the
        #   same mistake one level up.
        total = 0
        for seg in ne.segments():
            n = seg["i"]
            for c in callers(ne, n, a):
                total += 1
                # Show the run-up: the pushes ARE the argument list, and how each pushed
                # value was computed is the only thing that says what it means.
                print("  -- caller at seg%d:0x%04x (0x%x bytes before through 0x10 after)"
                      % (n, c, back))
                disasm(ne, n, max(0, c - back), back + 0x10, stubs)
        print("--- %d caller(s)" % total)
    return 0


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return 2
    path = a[0]
    ne = NE(path)
    if "--callers" in a:
        t = int(a[a.index("--callers") + 1], 0)
        for c in callers(ne, 1, t):
            print("seg1:0x%04x" % c)
        return 0
    if "--wowfunc" in a:
        return wowfunc(ne, path, int(a[a.index("--wowfunc") + 1], 0))
    segno = int(a[1], 0) if len(a) > 1 else 1
    start = int(a[2], 0) if len(a) > 2 else 0
    count = int(a[3], 0) if len(a) > 3 else 0x80
    disasm(ne, segno, start, count, wow_stubs(path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
