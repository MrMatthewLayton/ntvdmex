#!/usr/bin/env python3
"""neres.py -- list, extract and decode the RESOURCES in an NE module.

    tools/ne/neres.py list <exe>
    tools/ne/neres.py menu <exe> <id>      # decode a MENU and print it as a tree

WHY THIS EXISTS BEFORE ANY HOST CODE. The host is about to turn a Win16 program's
own MENU resource into a real Win32 menu, and both the resource table's layout and
the menu template's are things this project would otherwise be taking from memory.
Decoding them HERE first makes the reading self-checking: if the offsets are wrong
the strings do not come out as "&File" and "&Edit", and a wrong reading of a menu
does not accidentally spell the menu.
"""
import struct
import sys

RT = {1: "CURSOR", 2: "BITMAP", 3: "ICON", 4: "MENU", 5: "DIALOG", 6: "STRING",
      7: "FONTDIR", 8: "FONT", 9: "ACCELERATOR", 10: "RCDATA",
      12: "GROUP_CURSOR", 14: "GROUP_ICON", 16: "VERSION"}

MF_POPUP = 0x0010
MF_END = 0x0080


def ne_off(d):
    if d[:2] != b"MZ":
        raise SystemExit("not an MZ file")
    return struct.unpack_from("<I", d, 0x3C)[0]


def resources(d):
    """Yield (type_name, res_id, file_offset, length). The table is a list of
    TYPEINFO records, each followed by its NAMEINFOs, terminated by a zero type;
    names for non-integer ids live in a length-prefixed string pool after it."""
    h = ne_off(d)
    rt = h + struct.unpack_from("<H", d, h + 0x24)[0]
    shift = struct.unpack_from("<H", d, rt)[0]
    p = rt + 2
    out = []
    while True:
        tid = struct.unpack_from("<H", d, p)[0]
        if tid == 0:
            break
        cnt = struct.unpack_from("<H", d, p + 2)[0]
        p += 8
        for _ in range(cnt):
            off, ln, flags, rid = struct.unpack_from("<HHHH", d, p)
            out.append((tid, rid, off << shift, ln << shift, p))
            p += 12
    # Resolve the names of anything whose high bit is clear -- an offset into the
    # pool that begins where the TYPEINFO list ended.
    def nm(v, isname_base):
        if v & 0x8000:
            return v & 0x7FFF
        q = rt + v
        n = d[q]
        return d[q + 1:q + 1 + n].decode("latin1")
    res = []
    for tid, rid, off, ln, _p in out:
        t = nm(tid, rt)
        res.append((RT.get(t, t) if isinstance(t, int) else t, nm(rid, rt), off, ln))
    return res


def decode_menu(d, off, ln):
    """Win16 MENU template: WORD version, WORD headerSize, then items.
       item = WORD flags [, WORD id if not POPUP] , ASCIIZ text ; MF_END ends a level."""
    p = off
    ver, hdr = struct.unpack_from("<HH", d, p)
    p += 4 + hdr
    end = off + ln
    lines = []

    def level(p, depth):
        while p < end:
            flags = struct.unpack_from("<H", d, p)[0]
            p += 2
            mid = None
            if not (flags & MF_POPUP):
                mid = struct.unpack_from("<H", d, p)[0]
                p += 2
            s = bytearray()
            while p < end and d[p]:
                s.append(d[p]); p += 1
            p += 1
            txt = bytes(s).decode("latin1")
            lines.append("%s%-24s %s" % ("    " * depth,
                                         txt if txt else "(separator)",
                                         "" if mid is None else "id=0x%04x" % mid))
            if flags & MF_POPUP:
                p = level(p, depth + 1)
            if flags & MF_END:
                return p
        return p

    level(p, 0)
    return ver, hdr, lines


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        print(__doc__)
        return 2
    d = open(a[1], "rb").read()
    if a[0] == "list":
        for t, rid, off, ln in resources(d):
            print("  %-14s %-10s file 0x%06x  %6d bytes" % (t, rid, off, ln))
        return 0
    if a[0] == "menu":
        want = int(a[2], 0)
        for t, rid, off, ln in resources(d):
            if t == "MENU" and rid == want:
                ver, hdr, lines = decode_menu(d, off, ln)
                print("MENU %s at file 0x%06x (%d bytes) version=%d header=%d"
                      % (rid, off, ln, ver, hdr))
                for l in lines:
                    print("   " + l)
                return 0
        print("no MENU %d" % want)
        return 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
