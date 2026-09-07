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


DS_SETFONT = 0x40

# The six predefined control classes, encoded as a SINGLE BYTE 0x80..0x85 in a
# DLGITEMTEMPLATE where an application class is a NUL-terminated string. Getting
# this wrong does not fail -- it reads the byte as the first character of a name
# and every subsequent field slides, which is why the captions below are the test.
DLG_CLASS = {0x80: "BUTTON", 0x81: "EDIT", 0x82: "STATIC",
             0x83: "LISTBOX", 0x84: "SCROLLBAR", 0x85: "COMBOBOX"}


def _sz(d, p):
    """A NUL-terminated string, returning (text, next offset)."""
    e = d.index(b"\0", p)
    return d[p:e].decode("latin-1"), e + 1


def _name_or_ord(d, p):
    """MENU/CLASS field: 0x00 = absent, 0xFF + WORD = ordinal, else a string.
    ⚠ THE 0xFF FORM IS TWO BYTES OF PAYLOAD, NOT ONE. Reading it as a byte
    ordinal leaves one byte behind and every field after it is garbage."""
    if p >= len(d):
        return None, p
    if d[p] == 0x00:
        return None, p + 1
    if d[p] == 0xFF:
        return struct.unpack_from("<H", d, p + 1)[0], p + 3
    return _sz(d, p)


def decode_dialog(d, off, ln):
    """Decode a Win16 DLGTEMPLATE into (header dict, [item dicts]).

    THE LAYOUT, and it is the 16-bit one -- every coordinate is a WORD and the
    item count is a BYTE, where Win32's DLGTEMPLATE has a WORD count and a
    different field order entirely. Mixing the two produces a plausible-looking
    dialog with the wrong number of controls.

        DWORD dtStyle;  BYTE dtItemCount;  WORD dtX, dtY, dtCX, dtCY;
        <menu name>  <class name>  <caption>
        if (dtStyle & DS_SETFONT):  WORD pointsize;  <typeface>
      then dtItemCount x:
        WORD x, y, cx, cy;  WORD id;  DWORD style;
        <class: 1 byte 0x80..0x85, or a string>   <text>   BYTE cbCreationData
    """
    b = d[off:off + ln]
    style = struct.unpack_from("<I", b, 0)[0]
    count = b[4]
    x, y, cx, cy = struct.unpack_from("<4h", b, 5)
    p = 13
    menu, p = _name_or_ord(b, p)
    cls, p = _name_or_ord(b, p)
    caption, p = _sz(b, p)
    font = None
    if style & DS_SETFONT:
        pts = struct.unpack_from("<H", b, p)[0]
        face, p = _sz(b, p + 2)
        font = (pts, face)
    hdr = dict(style=style, count=count, x=x, y=y, cx=cx, cy=cy,
               menu=menu, cls=cls, caption=caption, font=font)

    items = []
    for _ in range(count):
        ix, iy, icx, icy, iid = struct.unpack_from("<5h", b, p)
        istyle = struct.unpack_from("<I", b, p + 10)[0]
        p += 14
        if b[p] in DLG_CLASS:
            icls, p = DLG_CLASS[b[p]], p + 1
        else:
            icls, p = _sz(b, p)
        itext, p = _name_or_ord(b, p)
        extra = b[p]
        p += 1 + extra
        items.append(dict(x=ix, y=iy, cx=icx, cy=icy, id=iid & 0xFFFF,
                          style=istyle, cls=icls, text=itext, extra=extra))
    return hdr, items, p, len(b)


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
    if a[0] == "dialog":
        want = a[2] if len(a) > 2 else None
        hit = 0
        for t, rid, off, ln in resources(d):
            if t != "DIALOG":
                continue
            if want is not None and str(rid) != want:
                continue
            hit = 1
            hdr, items, used, total = decode_dialog(d, off, ln)
            print("DIALOG %s at file 0x%06x (%d bytes)" % (rid, off, ln))
            print("  style=0x%08x  %dx%d at (%d,%d)  items=%d"
                  % (hdr["style"], hdr["cx"], hdr["cy"], hdr["x"], hdr["y"],
                     hdr["count"]))
            print("  caption=%r  class=%r  menu=%r  font=%r"
                  % (hdr["caption"], hdr["cls"], hdr["menu"], hdr["font"]))
            # ⚠ THE SELF-CHECK. A correct walk consumes the resource almost
            #   exactly; a wrong one runs short or overruns. Print it rather
            #   than trusting that the captions "looked right".
            print("  consumed %d of %d bytes%s"
                  % (used, total, "" if used <= total else "  !! OVERRAN"))
            for it in items:
                print("    %-9s id=0x%04x style=0x%08x %3dx%-3d at (%3d,%3d)  %r"
                      % (it["cls"], it["id"], it["style"], it["cx"], it["cy"],
                         it["x"], it["y"], it["text"]))
        if not hit:
            print("no such DIALOG")
            return 1
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
