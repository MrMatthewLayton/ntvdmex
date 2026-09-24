#!/usr/bin/env python3
"""vdmdis.py -- disassemble a function in XP's ntvdm.exe with its calls NAMED.

    ./tools/ntvdm/vdmdis.py 0x0f0156ec [bytes] [--exe guest/ntvdm/ntvdm.exe]

WHY: ntvdm.exe exports getAX/getDS/setCF/... -- the whole register-accessor set a
BOP handler is built from -- and it imports GetNextVDMCommand, GetKeyboardType and
the rest by name. Raw `call 0xf00556a` is unreadable; `call getDX` is a
specification. Every name printed here comes from the binary's own export or import
table, so nothing in the output is remembered or inferred.

⚠ Indirect calls `FF 15 <imm32>` are resolved through the IAT; direct `E8 rel32`
   through the export table. A call to a private (unexported) helper stays numeric --
   that is honest, not a gap to fill in by guessing.
"""
import struct, subprocess, sys, os, re

def pe(d):
    e = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, e + 6)[0]
    hdr = e + 24 + struct.unpack_from('<H', d, e + 20)[0]
    base = struct.unpack_from('<I', d, e + 24 + 28)[0]
    secs = [struct.unpack_from('<IIII', d, hdr + i * 40 + 8) for i in range(nsec)]
    return e, base, secs

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    exe = 'guest/ntvdm/ntvdm.exe'
    if '--exe' in sys.argv:
        exe = sys.argv[sys.argv.index('--exe') + 1]
    va = int(args[0], 0)
    n = int(args[1], 0) if len(args) > 1 else 0x100
    d = open(exe, 'rb').read()
    e, base, secs = pe(d)
    def foff(v):
        rva = v - base
        for vsz, sva, rsz, ptr in secs:
            if sva <= rva < sva + max(vsz, rsz):
                return ptr + (rva - sva)
        return None
    names = {}
    # exports
    expva = struct.unpack_from('<I', d, e + 24 + 96)[0]
    o = foff(base + expva)
    _, _, _, _, ob, na, nn, av, npv, ov = struct.unpack_from('<IIIIIIIIII', d, o)
    apo, npo, opo = foff(base + av), foff(base + npv), foff(base + ov)
    for i in range(nn):
        nv = struct.unpack_from('<I', d, npo + i * 4)[0]
        p = foff(base + nv)
        nm = d[p:d.index(b'\0', p)].decode()
        oi = struct.unpack_from('<H', d, opo + i * 2)[0]
        names[base + struct.unpack_from('<I', d, apo + oi * 4)[0]] = nm
    # imports, keyed by IAT slot
    iat = {}
    impva = struct.unpack_from('<I', d, e + 24 + 96 + 8)[0]
    o = foff(base + impva)
    while True:
        oft, ts, fc, nameva, fthunk = struct.unpack_from('<IIIII', d, o)
        if not nameva:
            break
        t = foff(base + (oft or fthunk)); k = 0
        while True:
            v = struct.unpack_from('<I', d, t + 4 * k)[0]
            if not v:
                break
            if not (v & 0x80000000):
                q = foff(base + v + 2)
                iat[base + fthunk + 4 * k] = d[q:d.index(b'\0', q)].decode()
            k += 1
        o += 20
    tf = os.path.join(os.environ.get('TMPDIR', '/tmp'), 'vdmdis.bin')
    open(tf, 'wb').write(d[foff(va):foff(va) + n])
    out = subprocess.run(['ndisasm', '-b', '32', '-o', hex(va), tf],
                         capture_output=True).stdout.decode()
    for line in out.rstrip().split('\n'):
        m = re.search(r'call (?:dword near )?\[?(0x[0-9a-f]+)\]?', line)
        if m:
            t = int(m.group(1), 16)
            nm = iat.get(t) or names.get(t)
            if nm:
                line += '        ; ' + nm
        print(line)

if __name__ == '__main__':
    sys.exit(main())
