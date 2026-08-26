#!/usr/bin/env python3
"""Pull a file out of one of the MS-DOS 6.22 floppy images in msdos-622/.

    python3 tools/dostest/extract-dos-file.py COMMAND.COM guest/

The images are already vendored; the files inside them are Microsoft's, so they
are extracted on demand rather than committed a second time in unpacked form.
Plain FAT12: read the BPB, walk the 12-bit cluster chain, honour the directory
entry's size.  Disk1 has COMMAND.COM, ATTRIB.EXE, CHKDSK.EXE, FORMAT.COM, ...
"""
import struct, sys, os

def read_dir(img):
    d = open(img, 'rb').read()
    bps = struct.unpack('<H', d[11:13])[0]
    res = struct.unpack('<H', d[14:16])[0]
    nfat = d[16]
    nroot = struct.unpack('<H', d[17:19])[0]
    spf = struct.unpack('<H', d[22:24])[0]
    fat = d[res * bps:(res + spf) * bps]
    root = (res + nfat * spf) * bps
    data = root + nroot * 32
    return d, fat, root, data, nroot, bps

def chain(fat, n):
    off = n + n // 2
    v = struct.unpack('<H', fat[off:off + 2])[0]
    return (v >> 4) if n & 1 else (v & 0xFFF)

def extract(img, want):
    d, fat, root, data, nroot, bps = read_dir(img)
    for i in range(nroot):
        e = d[root + i * 32:root + i * 32 + 32]
        if not e or e[0] in (0, 0xE5) or (e[11] & 0x08):
            continue
        nm = e[0:8].decode('latin1').strip() + '.' + e[8:11].decode('latin1').strip()
        if nm.upper() != want.upper():
            continue
        start = struct.unpack('<H', e[26:28])[0]
        size = struct.unpack('<I', e[28:32])[0]
        out, c = bytearray(), start
        while 2 <= c < 0xFF0:
            off = data + (c - 2) * bps
            out += d[off:off + bps]
            c = chain(fat, c)
        return bytes(out[:size])
    return None

if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    name, dest = sys.argv[1], sys.argv[2]
    here = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    for n in (1, 2, 3):
        img = os.path.join(here, 'msdos-622', f'Disk{n}.img')
        if not os.path.exists(img):
            continue
        blob = extract(img, name)
        if blob:
            os.makedirs(dest, exist_ok=True)
            path = os.path.join(dest, name.upper())
            open(path, 'wb').write(blob)
            print(f'{name} <- Disk{n}.img  ({len(blob)} bytes) -> {path}')
            break
    else:
        sys.exit(f'{name}: not found in any of the three images')
