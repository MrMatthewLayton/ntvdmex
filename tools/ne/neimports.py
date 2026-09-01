#!/usr/bin/env python3
"""neimports.py -- name every imported call site in a 16-bit NE, by resolving the
relocation chains rather than guessing from the code.

    tools/ne/neimports.py guest/ne/wowexec.exe            # every site, in order
    tools/ne/neimports.py guest/ne/wowexec.exe 0x08b2     # just these sites
    tools/ne/neimports.py guest/ne/wowexec.exe --seg 2

── WHY THIS EXISTS ──────────────────────────────────────────────────────────
`nedis.py` prints an imported far call as

    08b2  9affff0000    lcall 0, 0xffff

because that is genuinely what is in the file: an unlinked NE stores a CHAIN in
the operand words, and `0:0xffff` is the END of one, not a target. So the
disassembly of a program that is mostly API calls names none of them, and the
only way anyone had to say what a call site did was to read the pushes and
recognise the shape. That is inference, and this project has twice written up an
inferred name that was wrong.

The answer is in the file. Each relocation record says (module, ordinal), and
the chain it heads lists every site that takes that import. Walk the chains once
and every `lcall` in the module has a name -- `USER.41 CREATEWINDOW` -- with no
inference at all. Cross-checked against the surface tables the hard way in
session 39: `wowexec seg1:0x08b2` resolves to CREATEWINDOW, and the WOW32 stub
it reaches independently declares id 0x29 / 30 argument bytes, which is what
`wow-user-surface.md` says CREATEWINDOW is. Two methods, one answer.

⚠ THE SITE OFFSET IS THE OPERAND, NOT THE INSTRUCTION. A relocation points at
  the bytes it patches, so a `9a` far call at 0x08b2 is relocated at 0x08b3.
  This prints the CALL address (operand - 1) for the `9a`/`ea` forms and says so,
  because an off-by-one here is a lookup that silently finds nothing.

⚠ AN ADDITIVE RECORD IS NOT A CHAIN. Bit 2 of the type byte means "add to what
  is already there"; there is no chain to walk and exactly one site.
"""

import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nedump import NE, pstr                                   # noqa: E402

# Modules whose ordinals we can name, if the binary is beside the one being read.
KNOWN = {
    "KERNEL": "krnl386.exe", "USER": "user.exe", "GDI": "gdi.exe",
    "SHELL": "shell.dll", "TOOLHELP": "toolhelp.dll", "COMMDLG": "commdlg.dll",
    "KEYBOARD": "keyboard.drv", "MOUSE": "mouse.drv", "SOUND": "sound.drv",
    "SYSTEM": "system.drv", "COMM": "comm.drv", "WINNLS": "winnls.dll",
}

REL_INTERNAL, REL_ORDINAL, REL_NAME, REL_OSFIX = 0, 1, 2, 3
REL_ADDITIVE = 4


def ordinal_names(directory, module):
    """ordinal -> exported name, from the module's own tables.

    ⚠ BOTH TABLES, NOT JUST THE RESIDENT ONE. krnl386 keeps 312 exports in the
      NON-resident table, and a lookup that reads only the resident one reports
      an ordinal that plainly exists as unnamed."""
    fn = KNOWN.get(module.upper())
    if not fn:
        return {}
    path = os.path.join(directory, fn)
    if not os.path.exists(path):
        return {}
    ne = NE(path)
    return {o: s for o, s in ne.resident_names() + ne.nonresident_names()}


def sites(path, segno=1):
    """{offset of the relocated operand: label} for one segment."""
    ne = NE(path)
    seg = [s for s in ne.segments() if s["i"] == segno]
    if not seg:
        return {}, None
    seg = seg[0]
    mods = ne.modules()
    here = os.path.dirname(os.path.abspath(path))
    names = {m: ordinal_names(here, m) for m in mods}
    out = {}
    for r in ne.relocs(seg):
        kind = r["rel_type"] & 3
        if kind == REL_ORDINAL:
            mod = mods[r["a"] - 1]
            nm = names.get(mod, {}).get(r["b"])
            label = "%s.%d%s" % (mod, r["b"], (" " + nm) if nm else "")
        elif kind == REL_NAME:
            s, _ = pstr(ne.d, ne.off + ne.imp_tab + r["b"])
            label = "%s.%s" % (mods[r["a"] - 1], s)
        else:
            continue                       # internal/OS fixups name nothing useful
        if r["rel_type"] & REL_ADDITIVE:
            out[r["offset"]] = label
            continue
        off, seen = r["offset"], set()
        while off != 0xFFFF and off not in seen:
            seen.add(off)                  # a malformed chain must not hang the tool
            out[off] = label
            nxt = struct.unpack_from("<H", ne.d, seg["file_off"] + off)[0]
            off = nxt
    return out, (ne, seg)


def call_addr(ne_seg, operand):
    """The address of the INSTRUCTION, when the relocated operand belongs to one
    whose opcode we recognise. Returns None when it is a bare pointer in data."""
    ne, seg = ne_seg
    o = seg["file_off"] + operand - 1
    if operand >= 1 and ne.d[o] in (0x9A, 0xEA):     # far call / far jmp
        return operand - 1
    return None


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return 2
    path = a[0]
    segno = int(a[a.index("--seg") + 1], 0) if "--seg" in a else 1
    wanted = [int(x, 0) for x in a[1:] if not x.startswith("--")
              and a[a.index(x) - 1] != "--seg"]

    tab, ne_seg = sites(path, segno)
    if not tab:
        print("no imported relocations in seg%d of %s" % (segno, path))
        return 0

    if wanted:
        # Accept either the call address or the operand address, because a reader
        # coming from a disassembly has the first and the table is keyed on the
        # second, and being strict here just makes the tool useless at the moment
        # it is needed.
        for w in wanted:
            lab = tab.get(w + 1) or tab.get(w)
            print("0x%04x  %s" % (w, lab if lab else "-- not an imported site"))
        return 0

    for operand in sorted(tab):
        ca = call_addr(ne_seg, operand)
        where = ("call 0x%04x" % ca) if ca is not None else ("data 0x%04x" % operand)
        print("seg%d:%-16s %s" % (segno, where, tab[operand]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
