#!/usr/bin/env python3
"""wowthunks.py -- extract the WOW32 call surface a 16-bit module needs.  GH #128.

krnl386 reaches its 32-bit companion through ONE thunk, and every call arrives
there from a per-function stub of a fixed shape:

    push word <arg byte count>
    push word 0
    push word <FUNCTION ID>
    nop
    push cs
    call  <the common thunk>          ; which issues BOP 0x51

Measured against the live frame on real hardware (session 30 part 14): at the BOP,
[ss:bp+6] is the ID, [ss:bp+10] is the argument byte count and [ss:bp+12...] are the
arguments -- which is exactly the order those three pushes land in.

So the set of stubs IS the API we have to implement, and it can be read off the
binary without running anything. That is what this prints.

    tools/ne/wowthunks.py guest/ne/krnl386.exe
    tools/ne/wowthunks.py guest/ne/*.exe guest/ne/*.dll

⚠ METHOD AND ITS LIMIT: this is a byte-pattern scan, not a disassembly, so it can in
  principle match data that happens to look like the sequence. The `nop / push cs /
  call` tail makes that unlikely, and every hit is verified to call the SAME target
  (the common thunk) -- a stub calling anywhere else is reported separately rather
  than silently folded in.
"""
import collections
import struct
import sys


def push_imm(d, i):
    """Decode a push-immediate at i. Returns (value, next_index) or (None, None)."""
    if d[i] == 0x6A:                                  # push imm8, sign-extended
        return d[i + 1], i + 2
    if d[i] == 0x68:                                  # push imm16
        return struct.unpack_from('<H', d, i + 1)[0], i + 3
    return None, None


def scan(path):
    from nedump import NE
    ne = NE(path)
    out, targets = [], collections.Counter()
    for s in ne.segments():
        if s['flags'] & 0x0001 or not s['sector']:
            continue
        d = ne.d[s['file_off']:s['file_off'] + s['length']]
        i = 0
        while i < len(d) - 8:
            # anchor on the distinctive tail: nop / push cs / call rel16
            if d[i] == 0x90 and d[i + 1] == 0x0E and d[i + 2] == 0xE8:
                rel = struct.unpack_from('<h', d, i + 3)[0]
                tgt = (i + 5 + rel) & 0xFFFF
                # walk back over three pushes: <count> <0> <id>
                for idw in (3, 2):                    # push imm16 / push imm8
                    ids = i - idw
                    if ids < 0:
                        continue
                    fid, nxt = push_imm(d, ids)
                    if fid is None or nxt != i:
                        continue
                    for zw in (3, 2):
                        zs = ids - zw
                        if zs < 0:
                            continue
                        zero, nxt2 = push_imm(d, zs)
                        if zero != 0 or nxt2 != ids:
                            continue
                        for cw in (3, 2):
                            cs_ = zs - cw
                            if cs_ < 0:
                                continue
                            cnt, nxt3 = push_imm(d, cs_)
                            if cnt is None or nxt3 != zs:
                                continue
                            out.append((s['i'], cs_, fid, cnt, tgt))
                            targets[tgt] += 1
                            break
                        else:
                            continue
                        break
                    else:
                        continue
                    break
            i += 1
    return ne, out, targets


def main():
    sys.path.insert(0, __file__.rsplit('/', 1)[0])
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    for path in args:
        try:
            ne, hits, targets = scan(path)
        except Exception as e:                                    # noqa: BLE001
            print(f"{path}: {e}")
            continue
        own = ne.resident_names()[0][1].upper()
        print(f"\n=== {path}  ({own})")
        if not hits:
            print("   no WOW32 thunk stubs found")
            continue
        main_tgt, main_n = targets.most_common(1)[0]
        print(f"   {len(hits)} stubs; common thunk at 0x{main_tgt:04x} "
              f"({main_n} of them)")
        odd = [h for h in hits if h[4] != main_tgt]
        if odd:
            print(f"   ⚠ {len(odd)} stub(s) call a DIFFERENT target -- listed, not folded in:")
            for seg, off, fid, cnt, tgt in odd:
                print(f"       seg{seg}:0x{off:04x} id=0x{fid:02x} args={cnt} -> 0x{tgt:04x}")
        byid = {}
        for seg, off, fid, cnt, tgt in hits:
            if tgt != main_tgt:
                continue
            byid.setdefault(fid, []).append((seg, off, cnt))
        print(f"   {len(byid)} distinct function IDs:")
        cnts = collections.Counter()
        for fid in sorted(byid):
            places = byid[fid]
            sizes = {c for _, _, c in places}
            cnts[tuple(sorted(sizes))] += 1
            where = " ".join(f"seg{s}:0x{o:04x}" for s, o, _ in places[:3])
            flag = "  ⚠ INCONSISTENT ARG SIZE" if len(sizes) > 1 else ""
            print(f"     id 0x{fid:02x}  args={sorted(sizes)[0]:3} bytes   {where}{flag}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
