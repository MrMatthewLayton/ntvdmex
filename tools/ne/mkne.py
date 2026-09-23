#!/usr/bin/env python3
"""mkne.py -- link a nasm -f bin blob into a runnable Win16 NE executable.

WHY THIS EXISTS
---------------
Every Win16 check this project has ever run was "launch Notepad and look at it".
That is not a test: it needs a human, it needs a screenshot, and it cannot say
WHICH of the four hundred calls behind the window is wrong. Deterministic Win16
tests have been owed since the spec-first directive, and the thing blocking them
was not the tests -- it was that NOTHING IN THIS REPO COULD BUILD A WIN16 BINARY.
There is no OpenWatcom on this machine and no 16-bit Windows toolchain anywhere
in the tree; `tools/wowprobe/make-dosexe.sh` emits an MZ header with `printf`.

So: nasm writes the 16-bit code, and this writes the NE around it.

EVERYTHING HERE WAS READ OFF A REAL BINARY, NOT REMEMBERED
----------------------------------------------------------
`guest/win16/TASKMAN.EXE` (3744 bytes, 2 segments, KERNEL+USER) is the model, read
with this project's own tools/ne/nedump.py and nedis.py:

  * align shift 4, prog flags 0x0302 (DGROUP=MULTIPLEDATA), other flags 0x08,
    target Windows, expects 3.10, CS:IP = seg1:entry, SS:SP = seg2:0x0000.
  * Imported calls are `9A FF FF 00 00` -- an UNRELOCATED far call whose operand
    is the chain terminator 0xFFFF and segment 0 -- with a relocation record of
    addrtype 3 (FAR_ADDR 32) and reloctype 1 (IMPORTORDINAL). Verified at three
    sites: 0x04bd -> KERNEL.91, 0x04e7 -> KERNEL.30, 0x04f0 -> USER.5.

THE INTERFACE TO THE ASM: A MANIFEST AT OFFSET 0
------------------------------------------------
nasm -f bin cannot export symbols, so the .asm declares what it needs in a small
table at the start of the code segment and this reads it back out. The manifest
STAYS IN THE IMAGE as dead data -- stripping it would shift every offset nasm has
already computed, which is the kind of bookkeeping that quietly goes wrong.

  offset 0:  'NEIM'
        +4:  word entry_off      where execution starts
        +6:  word thunk_off      first import thunk
        +8:  word n_imports
       +10:  word n_modules
       +12:  word modname_off[n_modules]   NUL-terminated names, seg-relative
        ..:  word modidx, word ordinal     per import, 1-based modidx

Import i lives in slot i of an IMPORT ADDRESS TABLE at thunk_off -- a 4-byte far
pointer initialised to the unrelocated form TASKMAN.EXE also carries:

        FF FF 00 00         offset 0xFFFF (chain terminator), segment 0

so the patch site is simply thunk_off + 4*i, and the call site is

        call far [cs:slot]

⛔⛔ THE `far` IS LOad-BEARING, and the first cut of this got it wrong. The slots
were 5-byte `EA` (jmp far) stubs reached by a NEAR call, on the reasoning that the
imported function's RETF would return straight to the near caller. It does not: a
Win16 API ends in RETF, which pops TWO words, and a near call pushed one -- so the
API returned to 0000:<offset> and wandered off. An indirect far call needs no stub
at all, costs one relocation per FUNCTION rather than per call site, and cannot
get the call kind wrong.

usage: mkne.py <code.bin> <out.exe> --module NAME --desc "text"
                [--data N] [--heap N] [--stack N]
"""
import struct
import sys

ALIGN_SHIFT = 4                     # 16-byte sectors, as TASKMAN.EXE uses


def pstr(s):
    b = s.encode("ascii")
    return bytes([len(b)]) + b


def parse_manifest(code):
    if code[:4] != b"NEIM":
        raise SystemExit("mkne: code blob does not start with the 'NEIM' manifest "
                         "-- see the module docstring for its layout")
    entry, thunk, nimp, nmod = struct.unpack_from("<HHHH", code, 4)
    o = 12
    mods = []
    for _ in range(nmod):
        noff = struct.unpack_from("<H", code, o)[0]; o += 2
        end = code.index(b"\0", noff)
        mods.append(code[noff:end].decode("ascii"))
    imports = []
    for _ in range(nimp):
        midx, ordv = struct.unpack_from("<HH", code, o); o += 4
        if not 1 <= midx <= nmod:
            raise SystemExit("mkne: import names module %d, only %d declared"
                             % (midx, nmod))
        imports.append((midx, ordv))
    return entry, thunk, mods, imports


def build(code, module, desc, data_len, heap, stack):
    entry, thunk_off, mods, imports = parse_manifest(code)

    # ── relocations: one record per thunk, each its own single-site chain ─────
    #    The site holds FF FF 00 00 already (nasm wrote it); 0xFFFF in the offset
    #    word is the END-OF-CHAIN marker, which is exactly what TASKMAN has.
    relocs = b""
    for i, (midx, ordv) in enumerate(imports):
        site = thunk_off + 4 * i
        if code[site:site + 4] != b"\xff\xff\x00\x00":
            raise SystemExit("mkne: IAT slot %d at 0x%04x is not FF FF 00 00 -- "
                             "the manifest's table offset disagrees with the code"
                             % (i, site))
        relocs += struct.pack("<BBHHH", 3, 1, site, midx, ordv)
    if relocs:
        relocs = struct.pack("<H", len(imports)) + relocs

    # ── the tables, laid out after a 64-byte NE header ───────────────────────
    seg_tab = 0x40
    off = seg_tab + 2 * 8

    res_tab = off                       # no resources: zero-length, as linkers do
    resident_tab = off
    resident = pstr(module) + struct.pack("<H", 0) + b"\0"
    off += len(resident)

    mod_tab = off
    off += 2 * len(mods)

    imp_tab = off
    imp_names = b"\0"                   # offset 0 is the empty name
    mod_name_off = []
    for m in mods:
        mod_name_off.append(len(imp_names))
        imp_names += pstr(m)
    off += len(imp_names)

    entry_tab = off
    entry_bytes = b"\0\0"               # a zero bundle count terminates it
    off += len(entry_bytes)

    tables = (resident
              + struct.pack("<%dH" % len(mods), *mod_name_off)
              + imp_names + entry_bytes)

    ne_len = 0x40 + len(tables) + 2 * 8

    # ── the file ─────────────────────────────────────────────────────────────
    #    MZ stub first. e_lfanew must land the NE header 16-byte aligned.
    stub_code = bytes([0x0E, 0x1F,                      # push cs / pop ds
                       0xBA, 0x0E, 0x00,                # mov dx, msg
                       0xB4, 0x09, 0xCD, 0x21,          # ah=9 / int 21h
                       0xB8, 0x01, 0x4C, 0xCD, 0x21])   # ax=4C01 / int 21h
    msg = b"This program requires Microsoft Windows.\r\n$"
    stub = stub_code + msg
    stub += b"\0" * (-len(stub) % 16)

    mz = bytearray(64)
    mz[0:2] = b"MZ"
    total = 64 + len(stub)
    struct.pack_into("<H", mz, 2, total % 512)          # cblp
    struct.pack_into("<H", mz, 4, (total + 511) // 512)  # cp
    struct.pack_into("<H", mz, 6, 0)                    # crlc
    struct.pack_into("<H", mz, 8, 4)                    # cparhdr
    struct.pack_into("<H", mz, 10, 0)                   # minalloc
    struct.pack_into("<H", mz, 12, 0xFFFF)              # maxalloc
    struct.pack_into("<H", mz, 16, 0x00B8)              # sp
    struct.pack_into("<H", mz, 24, 0x0040)              # lfarlc
    ne_off = 64 + len(stub)
    struct.pack_into("<I", mz, 0x3C, ne_off)

    # Segment 1 (code, with relocs) then segment 2 (data), both sector aligned.
    body_start = ne_off + ne_len
    def align(n):
        return n + (-n % (1 << ALIGN_SHIFT))

    seg1_off = align(body_start)
    seg2_off = align(seg1_off + len(code) + len(relocs))
    data = b"\0" * data_len
    nonres_off = align(seg2_off + len(data))
    nonres = pstr(desc) + struct.pack("<H", 0) + b"\0"

    segs = [
        # sector, length, flags, minalloc
        (seg1_off >> ALIGN_SHIFT, len(code), 0x0040 | (0x0100 if relocs else 0),
         len(code)),
        (seg2_off >> ALIGN_SHIFT, len(data), 0x0001 | 0x0040, data_len),
    ]

    h = bytearray(0x40)
    h[0:2] = b"NE"
    h[2] = 5; h[3] = 20                                   # linker 5.20
    struct.pack_into("<H", h, 0x04, entry_tab)
    struct.pack_into("<H", h, 0x06, len(entry_bytes))
    struct.pack_into("<I", h, 0x08, 0)                    # crc: linkers leave 0
    struct.pack_into("<H", h, 0x0C, 0x0302)               # DGROUP=MULTIPLEDATA
    struct.pack_into("<H", h, 0x0E, 2)                    # auto data segment
    struct.pack_into("<H", h, 0x10, heap)
    struct.pack_into("<H", h, 0x12, stack)
    struct.pack_into("<I", h, 0x14, (1 << 16) | entry)    # CS:IP
    struct.pack_into("<I", h, 0x18, (2 << 16) | 0)        # SS:SP -- SP from stack
    struct.pack_into("<H", h, 0x1C, 2)                    # segment count
    struct.pack_into("<H", h, 0x1E, len(mods))
    struct.pack_into("<H", h, 0x20, len(nonres))
    struct.pack_into("<H", h, 0x22, seg_tab)
    struct.pack_into("<H", h, 0x24, res_tab)
    struct.pack_into("<H", h, 0x26, resident_tab)
    struct.pack_into("<H", h, 0x28, mod_tab)
    struct.pack_into("<H", h, 0x2A, imp_tab)
    struct.pack_into("<I", h, 0x2C, nonres_off)           # ⚠ ABSOLUTE, and a DWORD
    struct.pack_into("<H", h, 0x30, 0)                    # movable entries
    struct.pack_into("<H", h, 0x32, ALIGN_SHIFT)
    struct.pack_into("<H", h, 0x34, 0)                    # resource count
    h[0x36] = 2                                           # target OS = Windows
    h[0x37] = 0x08                                        # other flags, as TASKMAN
    struct.pack_into("<H", h, 0x3E, 0x030A)               # expects Windows 3.10

    out = bytearray()
    out += mz + stub
    out += h
    out += struct.pack("<8H", *[x for s in segs for x in s])
    out += tables
    out += b"\0" * (seg1_off - len(out))
    out += code + relocs
    out += b"\0" * (seg2_off - len(out))
    out += data
    out += b"\0" * (nonres_off - len(out))
    out += nonres
    return bytes(out)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    src, dst = argv[1], argv[2]
    def opt(name, default):
        return argv[argv.index(name) + 1] if name in argv else default
    code = open(src, "rb").read()
    blob = build(code,
                 module=opt("--module", "W16TEST"),
                 desc=opt("--desc", "ntvdmex win16 probe"),
                 data_len=int(opt("--data", "0x200"), 0),
                 heap=int(opt("--heap", "0x200"), 0),
                 stack=int(opt("--stack", "0xc00"), 0))
    open(dst, "wb").write(blob)
    print("wrote %s (%d bytes)" % (dst, len(blob)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
