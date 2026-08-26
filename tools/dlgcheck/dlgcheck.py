#!/usr/bin/env python3
"""dlgcheck.py -- parse RT_DIALOG templates out of a built PE and check the layout.

WHY THIS EXISTS. A dialog resource that compiles is not a dialog that renders. windres
is happy to emit a control positioned off the bottom of its page, two controls sitting
on top of each other, or a duplicate control ID that makes GetDlgItem return whichever
one it finds first -- and every one of those is invisible until somebody opens the
dialog on the target machine and looks at it. On this project that means a trip to the
bare-metal box, so the cheap checks belong here.

It reads the DIALOGEX templates straight out of the linked .exe -- the actual bytes the
loader will parse, not the .rc that produced them -- and reports:

  * every control: id, window class, rectangle, caption
  * controls whose rectangle leaves the dialog
  * overlapping controls (GROUPBOX and the tab control excluded: containing things is
    what they are for)
  * duplicate control IDs, which matters here because src/host/main.c looks a control
    up by asking every page for it in turn

Usage:  tools/dlgcheck/dlgcheck.py build/ntvdmhost.exe [--ids res/settings_ids.h]

Exit status is 1 if anything was reported, so it can gate a build.
"""
import struct
import sys

RT_DIALOG = 5


# ---- PE / resource directory walking ---------------------------------------
def _sections(d):
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    off = pe + 24 + optsz
    out = []
    for i in range(nsec):
        s = off + i * 40
        name = d[s:s + 8].rstrip(b"\0").decode("latin1")
        va, rawsz, raw = struct.unpack_from("<III", d, s + 12)
        out.append((name, va, rawsz, raw))
    return out


def _rva_to_off(secs, rva):
    for _, va, rawsz, raw in secs:
        if va <= rva < va + rawsz:
            return raw + (rva - va)
    return None


def _walk(d, base, off, secs, depth=0, path=()):
    """Yield (path, data_rva, size) for every leaf of a resource directory."""
    nnamed, nid = struct.unpack_from("<HH", d, off + 12)
    for i in range(nnamed + nid):
        e = off + 16 + i * 8
        name, entry = struct.unpack_from("<II", d, e)
        key = name & 0x7FFFFFFF if name & 0x80000000 else name
        if entry & 0x80000000:
            yield from _walk(d, base, base + (entry & 0x7FFFFFFF), secs,
                             depth + 1, path + (key,))
        else:
            rva, size = struct.unpack_from("<II", d, base + entry)[:2]
            yield path + (key,), rva, size


def dialogs(path):
    d = open(path, "rb").read()
    secs = _sections(d)
    rsrc = next((s for s in secs if s[0] == ".rsrc"), None)
    if not rsrc:
        return []
    base = rsrc[3]
    out = []
    for keys, rva, size in _walk(d, base, base, secs):
        if keys and keys[0] == RT_DIALOG:
            off = _rva_to_off(secs, rva)
            out.append((keys[1], d[off:off + size]))
    return out


# ---- DIALOGEX template parsing ---------------------------------------------
ATOM = {0x80: "BUTTON", 0x81: "EDIT", 0x82: "STATIC",
        0x83: "LISTBOX", 0x84: "SCROLLBAR", 0x85: "COMBOBOX"}
BS_GROUPBOX = 0x07


def _sz(b, o):
    """Read a NUL-terminated UTF-16 string, or an ordinal, at offset o."""
    if struct.unpack_from("<H", b, o)[0] == 0xFFFF:
        return struct.unpack_from("<H", b, o + 2)[0], o + 4
    s, i = [], o
    while True:
        c = struct.unpack_from("<H", b, i)[0]
        i += 2
        if c == 0:
            break
        s.append(chr(c))
    return "".join(s), i


def parse(b):
    """Return (w, h, caption, [controls]) for a DLGTEMPLATEEX."""
    sig, ver = struct.unpack_from("<HH", b, 0)
    if not (sig == 1 and ver == 0xFFFF):
        raise ValueError("not a DIALOGEX template")
    style = struct.unpack_from("<I", b, 12)[0]
    ccount, x, y, w, h = struct.unpack_from("<Hhhhh", b, 16)
    o = 26
    _menu, o = _sz(b, o)
    _cls, o = _sz(b, o)
    cap, o = _sz(b, o)
    if style & 0x40:                          # DS_SETFONT: point size, weight, ...
        o += 6
        _face, o = _sz(b, o)
    ctrls = []
    for _ in range(ccount):
        o = (o + 3) & ~3                      # each control is DWORD-aligned
        cstyle = struct.unpack_from("<I", b, o + 8)[0]
        cx, cy, cw, ch, cid = struct.unpack_from("<hhhhI", b, o + 12)
        o += 24
        cls, o = _sz(b, o)
        txt, o = _sz(b, o)
        extra = struct.unpack_from("<H", b, o)[0]
        o += 2 + extra
        if isinstance(cls, int):
            cls = ATOM.get(cls, "atom:%#x" % cls)
        ctrls.append(dict(id=cid, cls=cls, style=cstyle,
                          x=cx, y=cy, w=cw, h=ch,
                          txt=txt if isinstance(txt, str) else ""))
    return w, h, cap if isinstance(cap, str) else "", ctrls


def is_container(c):
    """Group boxes and the tab control legitimately enclose other controls."""
    if c["cls"] == "BUTTON" and (c["style"] & 0x0F) == BS_GROUPBOX:
        return True
    return "TAB" in c["cls"].upper()


# ⚠ A COMBOBOX's height in a dialog template is the height of its DROPPED LIST, not
#   of the closed control. Taking the template number literally makes every combo look
#   like it swallows the two rows beneath it and hangs off the bottom of the page --
#   the first run of this script reported 47 "problems" and 45 of them were this. What
#   occupies space in the resting layout is the closed control: one text line plus the
#   frame, which is ~12 dlu at 8pt MS Shell Dlg.
COMBO_CLOSED_H = 12


def rect(c):
    """The rectangle the control actually occupies when the dialog is at rest."""
    h = c["h"]
    if c["cls"] == "COMBOBOX":
        h = min(h, COMBO_CLOSED_H)
    return c["x"], c["y"], c["w"], h


def overlap(a, b):
    ax, ay, aw, ah = rect(a)
    bx, by, bw, bh = rect(b)
    return not (ax + aw <= bx or bx + bw <= ax or
                ay + ah <= by or by + bh <= ay)


def main():
    argv = sys.argv[1:]
    if not argv:
        print(__doc__)
        return 2
    exe = argv[0]
    idsfile = None
    if "--ids" in argv:
        idsfile = argv[argv.index("--ids") + 1]

    names = {}
    if idsfile:
        for line in open(idsfile):
            p = line.split()
            if len(p) >= 3 and p[0] == "#define" and p[2].isdigit():
                names[int(p[2])] = p[1]

    problems = []
    seen = {}
    for rid, blob in sorted(dialogs(exe)):
        try:
            w, h, cap, ctrls = parse(blob)
        except Exception as e:                                  # noqa: BLE001
            problems.append("dialog %d: %s" % (rid, e))
            continue
        print("\n== dialog %s (%d)  %dx%d dlu  %r  -- %d controls"
              % (names.get(rid, rid), rid, w, h, cap, len(ctrls)))
        for c in ctrls:
            tag = names.get(c["id"], "" if c["id"] in (0xFFFFFFFF, 0) else str(c["id"]))
            print("   %-18s %-14s (%3d,%3d %3dx%-3d) %s"
                  % (tag, c["cls"], c["x"], c["y"], c["w"], c["h"], c["txt"][:44]))
            cx, cy, cw, ch = rect(c)
            if cx < 0 or cy < 0 or cx + cw > w or cy + ch > h:
                problems.append("dialog %s: %s leaves the dialog (%d,%d %dx%d in %dx%d)"
                                % (names.get(rid, rid), tag or c["cls"],
                                   cx, cy, cw, ch, w, h))
            if c["id"] not in (0xFFFFFFFF, 0):
                if c["id"] in seen:
                    problems.append("control id %s used twice: dialogs %s and %s"
                                    % (tag, seen[c["id"]], names.get(rid, rid)))
                seen[c["id"]] = names.get(rid, rid)
        real = [c for c in ctrls if not is_container(c)]
        for i in range(len(real)):
            for j in range(i + 1, len(real)):
                if overlap(real[i], real[j]):
                    problems.append("dialog %s: %s overlaps %s"
                                    % (names.get(rid, rid),
                                       names.get(real[i]["id"], real[i]["txt"][:20]),
                                       names.get(real[j]["id"], real[j]["txt"][:20])))
    print()
    if problems:
        for p in problems:
            print("PROBLEM: " + p)
        print("\n%d problem(s)" % len(problems))
        return 1
    print("no layout problems found")
    return 0


if __name__ == "__main__":
    sys.exit(main())
