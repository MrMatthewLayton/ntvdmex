#!/usr/bin/env python3
"""neneeds.py -- THE COMPLETE 32-BIT SURFACE ONE Win16 PROGRAM NEEDS.  GH #128.

    tools/ne/neneeds.py guest/ne/notepad.exe
    tools/ne/neneeds.py guest/ne/notepad.exe --todo      # only what is missing
    tools/ne/neneeds.py guest/ne/*.exe --todo            # a whole shelf of guests

── WHY THIS EXISTS ──────────────────────────────────────────────────────────
Until now the next thing to implement was found by RUNNING the guest and reading
which call it stopped on. That works -- every service in this host was named that
way -- but it finds exactly one wall per deploy/run/read cycle, and it cannot see
the calls that fail QUIETLY. Notepad's File > Save did nothing at all and File >
Open killed it: neither announced a missing service, because the missing piece is
only reached after something else has already returned a wrong answer.

An import list does not have that problem. It is in the file, it is finite, and
every entry is a call the program really can make. So this enumerates it, and
that turns "what is next?" into "what is left?" -- a list with an end.

── ★ AN IMPORT IS NOT NECESSARILY WORK, AND THAT IS THE WHOLE POINT ─────────
Most of what a Win16 program imports is implemented IN 16-BIT CODE inside the
module it imports from, and runs on the real CPU without ever asking us. The
host only has to implement an import that reaches a WOW32 thunk. Both halves are
in the binaries, so the distinction is computed, not guessed:

    import (module, ordinal)                  -- from the program's relocations
      -> that module's OWN entry table        -- ordinal to segment:offset
        -> the bytes there                    -- `6a AA 68 00 00 68 II II 9a`?
             yes -> a WOW32 stub: id II, AA argument bytes.  ★ OUR JOB.
             no  -> 16-bit code in the module.               FREE.

That test is exact and it has already earned itself twice: `USER.107
DEFWINDOWPROC` and `USER.87 DIALOGBOX` both look like obvious work and are both
16-bit code, so neither needs a line from us.

⚠⚠ "native16" DOES NOT MEAN FREE, AND THE COUNTEREXAMPLE IS ALREADY IN THIS HOST.
  `USER.174 LOADICON` classifies as native16 -- correctly, its entry point is
  16-bit code -- and it is the whole reason id `0xad` exists: USER does the
  resource lookup itself and THEN asks the 32-bit side to build the object. That
  internal call is not in the program's import table and this tool cannot see it.
  ⇒ The number below is a LOWER BOUND on the work and an EXACT list of the
    directly-reachable part. What a module asks for on the program's behalf still
    comes from a run, so this bounds the job; it does not replace the log.
⚠ AND AN IMPLEMENTED ID IS NOT A CORRECT ONE. "serviced" here means this host has
  a `case` for it, nothing more.

── WHAT IT SAYS TODAY (session 44) ─────────────────────────────────────────
    NOTEPAD.EXE alone:  118 imports | 34 reach us | 9 serviced | 25 TO DO
    19 Win3.11 guests:  distinct ids left -- USER 84, GDI 46, SHELL 17,
                        SOUND 6, KEYBOARD 5
So the whole shelf is ~158 distinct services, against the ~1000 thunked entry
points those modules define between them. That gap -- 158 against 1000 -- is the
argument for enumerating per PROGRAM rather than per API.
"""

import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nedump import NE, pstr                                    # noqa: E402
from neimports import KNOWN, REL_ORDINAL, REL_NAME, REL_ADDITIVE   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Which source file dispatches which module's id space. Used only to say whether
# an id is already answered -- see the warning above about what that means.
DISPATCHERS = {
    "KERNEL":  "src/wow/wow32.h",
    "USER":    "src/wow/wowuser.h",
    "SHELL":   "src/wow/wowshell.h",
    "COMMDLG": "src/wow/wowcommdlg.h",
    "KEYBOARD": "src/wow/wowkbd.h",
    "GDI":     "src/wow/wowgdi.h",
    # ⚠ A module with no entry here reports 0 SERVICED whatever the host does.
    #   Add the file when its dispatcher appears, or the tool quietly overstates
    #   the work -- which is the same class of lie it exists to prevent.
}


def serviced_ids(relpath):
    """The ids a dispatcher has a `case` for: {id}. Resolves `case NAME:` through
    the `#define NAME 0x..` in the same file, because every id in this host is
    named rather than written as a bare number."""
    if not relpath:
        return set()
    path = os.path.join(ROOT, relpath)
    if not os.path.isfile(path):
        return set()
    src = open(path, errors="replace").read()
    defs = {m.group(1): int(m.group(2), 16)
            for m in re.finditer(r"#define\s+(\w+)\s+(0x[0-9a-fA-F]+)", src)}
    out = set()
    for m in re.finditer(r"^\s*case\s+(\w+)\s*:", src, re.M):
        if m.group(1) in defs:
            out.add(defs[m.group(1)])
    for m in re.finditer(r"^\s*case\s+(0x[0-9a-fA-F]+)\s*:", src, re.M):
        out.add(int(m.group(1), 16))
    return out


def classify(ne, segs, seg, off, depth):
    """(kind, id, argbytes) for the code an exported ordinal lands on.

    ⚠⚠ AN EXPORT DOES NOT POINT AT ITS STUB. Two prologue conventions sit between
      them, and a classifier that reads only the first bytes gets BOTH modules
      wrong -- the first cut of this tool reported COMMDLG as "0 need us" and
      the whole of USER as native 16-bit code, which is the opposite of true.

      * COMMDLG: `commdlg.1 GETOPENFILENAME` -> seg1:0x0000 = `9a c2 00 ff ff`,
        an (unlinked) far call, and the stub follows it at +5. wowthunks.py
        independently puts that id's stub at seg1:0x0005.
      * USER: `user.56 MOVEWINDOW` -> seg1:0x1f8e =
        `55 8b ec 68 99 1f 5a 5d e9 e3 eb` -- push bp / mov bp,sp / push imm16 /
        pop dx / pop bp / JMP. This is the TAIL-JUMP this project already
        documented ("USER's exports reach their stubs by tail-jump, not by
        call"), and following it lands on seg1:0x0b7c, which is exactly where
        wow-user-surface.md says MOVEWINDOW's stub is. Two methods, one answer.

    ⚠ THE TAIL-JUMP PATTERN IS MATCHED EXACTLY, not by scanning for a `e9`.
      `user.107 DEFWINDOWPROC` begins with the same four bytes
      (`55 8b ec 68 86 1d`) and then pushes arguments -- it is genuinely 16-bit
      code, and a looser match would follow a jump further down its body and
      report it as a thunk we have to write. It is not; we already proved that
      by never seeing one as a BOP in a whole run."""
    if depth > 3 or not (1 <= seg <= len(segs)):
        return ("native16", None, None)
    fo = segs[seg - 1]["file_off"]
    ln = segs[seg - 1]["length"]
    if off >= ln:
        return ("native16", None, None)
    b = ne.d[fo + off: fo + off + 16]

    # push <argbytes> / push 0 / push <id> / lcall <the common thunk>
    if len(b) >= 9 and b[0] == 0x6A and b[2] == 0x68 and b[5] == 0x68 and b[8] == 0x9A:
        return ("wow32", b[6] | (b[7] << 8), b[1])
    if len(b) >= 11 and b[0] == 0x68 and b[3] == 0x68 and b[6] == 0x68 and b[9] == 0x9A:
        return ("wow32", b[7] | (b[8] << 8), b[1] | (b[2] << 8))

    # COMMDLG's export prologue: a far call, then the stub.
    if len(b) >= 6 and b[0] == 0x9A:
        return classify(ne, segs, seg, off + 5, depth + 1)

    # USER's export thunk: push bp / mov bp,sp / push imm16 / pop dx / pop bp / jmp
    if (len(b) >= 11 and b[0] == 0x55 and b[1] == 0x8B and b[2] == 0xEC
            and b[3] == 0x68 and b[6] == 0x5A and b[7] == 0x5D and b[8] == 0xE9):
        rel = struct.unpack_from("<h", b, 9)[0]
        return classify(ne, segs, seg, (off + 8 + 3 + rel) & 0xFFFF, depth + 1)

    return ("native16", None, None)


_MIDX = {}


def module_index(directory, module):
    """For one imported-from module: ordinal -> (name, kind, id, argbytes).

    `kind` is 'wow32' (a thunk we must implement) or 'native16' (its own code)
    or 'absent' (the module's file is not beside the program)."""
    key = (directory, module.upper())
    if key in _MIDX:
        return _MIDX[key]
    fn = KNOWN.get(module.upper())
    if not fn:
        _MIDX[key] = None
        return None
    path = os.path.join(directory, fn)
    if not os.path.exists(path):
        _MIDX[key] = None
        return None
    ne = NE(path)
    names = {o: s for o, s in ne.resident_names() + ne.nonresident_names()}
    segs = ne.segments() if callable(ne.segments) else ne.segments
    ents = ne.entries() if callable(ne.entries) else ne.entries
    out = {}
    for o, ent in ents.items():
        if not o:
            continue
        seg, off = ent[2], ent[3]
        # ⚠ An entry-table bundle can be a GAP (unused ordinals), and those come
        #   back with no segment at all. Skipping them is not a shortcut: an
        #   ordinal nothing exports cannot be the thing an import resolves to.
        if not isinstance(seg, int) or not isinstance(off, int):
            continue
        if seg < 1 or seg > len(segs):
            continue
        kind, fid, argb = classify(ne, segs, seg, off, 0)
        out[o] = (names.get(o, "?"), kind, fid, argb, seg, off)
    _MIDX[key] = out
    return out


def imports_of(path):
    """Every distinct (MODULE, ordinal) the program imports, across all segments.
    ⚠ `relocs()` takes the segment DICT, and the module index lives in `a` with
      the ordinal (or the imported-name offset) in `b` -- same reading neimports
      uses, so the two tools cannot drift."""
    ne = NE(path)
    mods = ne.modules()
    want, byname = set(), {}
    for s in ne.segments():
        for r in ne.relocs(s):
            kind = r["rel_type"] & 3
            if not 1 <= r["a"] <= len(mods):
                continue
            m = mods[r["a"] - 1].upper()
            if kind == REL_ORDINAL:
                want.add((m, r["b"]))
            elif kind == REL_NAME:
                nm, _ = pstr(ne.d, ne.off + ne.imp_tab + r["b"])
                byname.setdefault(m, set()).add(nm)
    return want, byname


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    todo_only = "--todo" in sys.argv
    if not args:
        print(__doc__)
        return 2
    grand = {}
    for path in args:
        directory = os.path.dirname(os.path.abspath(path))
        # ⚠ NAME THE FILE IT COULD NOT READ. This is meant to be pointed at a
        #   whole directory, and a directory of Windows 3.x programs contains
        #   plain-DOS MZ images (EXPAND.EXE) and the odd PE. Dying on the first
        #   one loses the answer for every file after it, and skipping one
        #   silently would under-report the work -- which is the one thing this
        #   tool exists not to do.
        try:
            want, byname = imports_of(path)
        except Exception as exc:                                  # noqa: BLE001
            print("\n=== %s ===\n  NOT READ: %s"
                  % (os.path.basename(path), exc))
            continue
        # by-name imports resolved back to ordinals through the module's tables
        for m, names in byname.items():
            idx = module_index(directory, m)
            if not idx:
                continue
            rev = {v[0].upper(): o for o, v in idx.items()}
            for n in names:
                if n.upper() in rev:
                    want.add((m, rev[n.upper()]))

        print("\n=== %s ===" % os.path.basename(path))
        per = {}
        for m, o in sorted(want):
            idx = module_index(directory, m)
            if idx is None:
                per.setdefault(m, []).append((o, "?", "absent", None, None))
                continue
            if o not in idx:
                per.setdefault(m, []).append((o, "?", "no-entry", None, None))
                continue
            nm, kind, fid, argb = idx[o][0], idx[o][1], idx[o][2], idx[o][3]
            per.setdefault(m, []).append((o, nm, kind, fid, argb))

        for m in sorted(per):
            done = serviced_ids(DISPATCHERS.get(m, ""))
            rows = sorted(per[m])
            w32 = [r for r in rows if r[2] == "wow32"]
            nat = [r for r in rows if r[2] == "native16"]
            miss = [r for r in w32 if r[3] not in done]
            print("  %-8s %3d imported | %3d need us | %3d free (16-bit) | "
                  "%3d SERVICED | %3d TO DO"
                  % (m, len(rows), len(w32), len(nat), len(w32) - len(miss), len(miss)))
            for o, nm, kind, fid, argb in (miss if todo_only else w32):
                mark = " " if fid in done else "*"
                print("      %s %-24s ord %-4d id 0x%03x  %2d args"
                      % (mark, nm, o, fid, argb))
                grand.setdefault(m, set()).add((fid, nm, argb))
    if len(args) > 1:
        print("\n=== union across %d programs ===" % len(args))
        for m in sorted(grand):
            print("  %-8s %d distinct ids still to do" % (m, len(grand[m])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
