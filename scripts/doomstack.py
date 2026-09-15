#!/usr/bin/env python3
"""doomstack.py -- read a vdmwatch stack dump against Doom's LE image.

vdmwatch (scripts/bm/vdmwatch.c) logs, at the first-chance exception, the stack
as dwords from ESP-0x100 to ESP+0x600 and writes a core of the DPMI region
(out\\vdmwatch_core_dpmi.bin, base 0x03ff0000). This script walks that dump:
every dword that lands in the LE code object is checked for a CALL instruction
ending just before it -- the mark of a return address -- and disassembled in
context, so the call chain at the death can be read off. Values in the data
object, the zone and the stack object are named too.

    scripts/doomstack.py runs/vdmwatch_s72_crash7.txt \
        --core /private/tmp/xpshare/out/vdmwatch_core_dpmi.bin \
        --exe  /private/tmp/xpshare/games/Doom/DOOM.EXE

The object bases are the ones the host handed DOS/4GW on this run (from the
INT 31h 0501 lines in ntvdmhost.log); pass --code/--data if they differ.
"""
import argparse, re, struct, subprocess, sys

def le_objects(exe):
    f = open(exe, 'rb').read()
    off = f.find(b'LE\x00\x00')
    h = f[off:off + 0xC4]
    u32 = lambda o: struct.unpack_from('<I', h, o)[0]
    objtab, nobj, datapages = off + u32(0x40), u32(0x44), u32(0x80)
    objs = []
    for i in range(nobj):
        vsize, base, flags, pmi, pmc, _ = struct.unpack_from('<IIIIII', f, objtab + i * 24)
        objs.append(dict(vsize=vsize, base=base, flags=flags, page=pmi, npages=pmc,
                         fileoff=datapages + (pmi - 1) * 0x1000))
    return f, objs

def disasm(buf, origin, want_end=None):
    p = subprocess.run(['ndisasm', '-b', '32', '-o', hex(origin), '-'], input=buf,
                       capture_output=True, text=True)
    return p.stdout.splitlines()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--core', help='vdmwatch_core_dpmi.bin (relocated bytes; preferred)')
    ap.add_argument('--core-base', type=lambda s: int(s, 0), default=0x03ff0000)
    ap.add_argument('--exe', default='/private/tmp/xpshare/games/Doom/DOOM.EXE')
    ap.add_argument('--code', type=lambda s: int(s, 0), default=0x04140000)
    ap.add_argument('--data', type=lambda s: int(s, 0), default=0x041b0000)
    ap.add_argument('--zone', type=lambda s: int(s, 0), default=0x04260000)
    ap.add_argument('--zone-size', type=lambda s: int(s, 0), default=0x801000)
    ap.add_argument('--context', type=int, default=6, help='instructions before each call site')
    a = ap.parse_args()

    f, objs = le_objects(a.exe)
    code_size = objs[0]['vsize']; data_size = objs[2]['vsize'] if len(objs) > 2 else 0
    core = open(a.core, 'rb').read() if a.core else None

    def code_bytes(lin, n):
        if core is not None:
            o = lin - a.core_base
            if 0 <= o and o + n <= len(core): return core[o:o + n]
        o = objs[0]['fileoff'] + (lin - a.code)
        return f[o:o + n]

    # the LAST exception's stack lines in the log
    lines = open(a.log, errors='replace').read().splitlines()
    starts = [i for i, l in enumerate(lines) if 'EXCEPTION first-chance' in l]
    if not starts: sys.exit('no first-chance exception in the log')
    i0 = starts[-1]
    hdr = [l for l in lines[i0:i0 + 8]]
    for l in hdr: print(l)
    print()
    m = re.search(r'ss:esp=([0-9a-f]{4}):([0-9a-f]{8})', ' '.join(hdr))
    esp = int(m.group(2), 16) if m else None
    st = []
    for l in lines[i0:]:
        mm = re.match(r'\S+\s+st ([0-9a-f]{8}) [ >] ((?:[0-9a-f]{8} ?)+)$', l)
        if mm:
            base = int(mm.group(1), 16)
            for k, v in enumerate(mm.group(2).split()):
                st.append((base + 4 * k, int(v, 16)))
        elif st and 'EXCEPTION' in l:
            break
    if not st: sys.exit('no stack lines after the exception')

    def classify(v):
        if a.code <= v < a.code + code_size: return 'CODE'
        if a.data <= v < a.data + data_size: return 'data' if v < a.data + data_size - 0x8000 else 'stack'
        if a.zone <= v < a.zone + a.zone_size: return 'zone'
        return ''

    print(f'{"addr":>10} {"value":>10}  note')
    for addr, v in st:
        mark = '<== ESP' if addr == esp else ''
        cls = classify(v)
        note = ''
        if cls == 'CODE':
            # a CALL rel32 (E8) ends at v if bytes v-5 == E8; CALL r/m32 (FF /2) is 2-7 bytes
            pre = code_bytes(v - 8, 8)
            kind = ''
            if pre[3] == 0xE8:
                tgt = (v + struct.unpack('<i', pre[4:8])[0]) & 0xffffffff
                kind = f'RET after call {tgt:#010x}'
            elif pre[6] == 0xFF and (pre[7] >> 3) & 7 == 2:
                kind = 'RET after call r/m (2-byte)'
            elif pre[2] == 0xFF and (pre[3] >> 3) & 7 == 2:
                kind = 'RET after call r/m (6-byte)'
            note = f'code {kind}'.rstrip()
        elif cls:
            note = cls
        if v == 0x04c4c4fa: note += '  *** the EIP that died'
        as_fixed = v / 65536.0
        if 0x00100000 <= v <= 0x20000000 and not cls: note += f'  (fixed {as_fixed:.2f})'
        print(f'{addr:#010x} {v:#010x}  {note} {mark}')

    # the call sites, in context
    print('\n--- call sites (return addresses on the stack, innermost first) ---')
    for addr, v in st:
        if classify(v) != 'CODE': continue
        pre = code_bytes(v - 8, 8)
        if pre[3] != 0xE8 and not (pre[6] == 0xFF and (pre[7] >> 3) & 7 == 2): continue
        lo = v - 40
        out = disasm(code_bytes(lo, 56), lo)
        # keep the last `context` instructions before v and 2 after
        idx = [i for i, l in enumerate(out) if l.startswith(f'{v:08X}')]
        if not idx: continue
        k = idx[0]
        print(f'\n[{addr:#010x}] -> {v:#010x}')
        for l in out[max(0, k - a.context):k + 2]:
            print('   ', l)

if __name__ == '__main__':
    main()
