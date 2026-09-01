#!/usr/bin/env python3
"""wowmap.py -- put NAMES on the 82 WOW32 function IDs, from krnl386's own tables.

GH #128. `wowthunks.py` enumerated the surface as bare integers. This names it, and
the names do not come from guesswork -- they come from krnl386.exe itself:

  DIRECT   an entry-table export points AT a stub, so the export's name in the
           resident/non-resident name table IS the function's name. No inference.
  WRAPPER  an export's body is a short prologue that calls exactly one stub before
           returning. The name is the export's, and the mapping is an inference --
           a strong one, but it is labelled so a reader can tell.
  (unnamed) reached only from internal code. These need the call site read by hand
           (tools/ne/nedis.py --wowfunc) and are the actual work list.

⚠ WHY THIS IS TRUSTWORTHY: it was cross-checked against an independent method before
  being believed. Id 0xcf was worked out from its call site alone -- the caller
  compares the return against 0x411/0x412/0x404/0x804/0xc04, which are LANGIDs -- and
  the export table then said GETSYSTEMDEFAULTLANGID. Two methods, one answer.

    tools/ne/wowmap.py guest/ne/krnl386.exe
    tools/ne/wowmap.py guest/ne/krnl386.exe --md      # the surface doc, regenerated
"""
import struct
import sys

import capstone

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from nedump import NE                                   # noqa: E402
from wowthunks import scan, IMPORTED_THUNK             # noqa: E402

WRAPPER_WINDOW = 0x40          # how far into an export body to look for the call


def stub_tables(path):
    """{(segment, thunk) -> {(seg, off) -> (id, argbytes)}} -- ONE ENTRY PER TABLE.

    ⚠ A MODULE CAN HAVE MORE THAN ONE, AND THEY DO NOT SHARE A NUMBERING. This
      used to return a single flat dict, which was right only while krnl386 seg1
      was the only table anyone had looked at. Measured (session 38):

        krnl386  seg1 -> its own thunk 0x2bb6   82 stubs   <- the documented surface
                 seg1 -> a second thunk 0xaae8   6 stubs
                 seg2 -> an imported thunk     121 stubs
        user     seg1 -> an imported thunk     457 stubs
        gdi      seg1 -> an imported thunk     367 stubs

      Pooling them reported "krnl386 has 201 WOW32 function IDs", which silently
      merges three id spaces into one and would put a name from one table on a
      function in another. That is exactly the mistake the host was making at
      run time when it answered USER's RegisterClass with GetProfileIntA."""
    _ne, hits, _t = scan(path)
    out = {}
    for s, o, f, c, tgt in hits:
        out.setdefault((s, tgt), {})[(s, o)] = (f, c)
    return out


def table_name(key):
    seg, tgt = key
    return ("seg%d -> imported thunk" % seg) if tgt == IMPORTED_THUNK \
        else ("seg%d -> own thunk 0x%04x" % (seg, tgt))


def export_names(ne):
    """ordinal -> name, resident and non-resident together.

    ⚠ Both tables, not just the resident one: krnl386 keeps 312 non-resident names
      and several of the WOW32 exports (WOWLOADMODULE, GETPROCADDRESS32W...) live
      only there. Reading one table finds a fraction of them."""
    out = {}
    for o, s in ne.resident_names() + ne.nonresident_names():
        out.setdefault(o, s)
    return out


def wrapper_target(ne, seg, off, stubs):
    """If this export body reaches exactly one stub before returning, which one?

    ⚠ `call` IS NOT THE ONLY WAY IN, and assuming it was made this function report
      "no wrappers" for a module built entirely out of them. USER.EXE's exports are
      TAIL-JUMPS:

          1dbd  push bp / mov bp,sp
          1dc0  push 0x1dc8 / pop dx      ; the return trampoline, in DX
          1dc4  pop bp
          1dc5  jmp 0x0c18                ; ★ the stub -- a JUMP, not a call
          1dc8  retf 4                    ; ...and the trampoline sits right after it

      That is `RegisterClass` (ordinal 57), and the stub at 0x0c18 is id 0x39 -- the
      call this project spent a run answering with `GetProfileIntA`. Every export in
      the module has this shape, so a call-only scan named none of them.
      (session 38; the same blind spot as nedis.py --callers, found the same day.)"""
    s = [x for x in ne.segments() if x["i"] == seg]
    if not s or s[0]["flags"] & 0x0001 or not s[0]["sector"]:
        return None
    d = ne.d[s[0]["file_off"]:s[0]["file_off"] + s[0]["length"]]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
    hit = None
    for ins in md.disasm(bytes(d[off:off + WRAPPER_WINDOW]), off):
        if ins.mnemonic in ("ret", "retf"):
            break
        if ins.mnemonic in ("call", "jmp") and ins.op_str.startswith("0x"):
            t = int(ins.op_str, 16) & 0xFFFF
            if (seg, t) not in stubs:
                # ⚠ A CALL to a non-stub is NOT disqualifying here, and treating it
                #   as such lost CreateWindow. USER's wrappers marshal their
                #   arguments through helpers before dispatching:
                #       1e4d  push bp / mov bp,sp / push 0x1e73   ; trampoline in DX
                #       1e53  ...three `call`s that convert arguments...
                #       1e70  jmp 0x038d                          ; ★ the stub
                #       1e73  retf 0x1e                           ; 30 arg bytes
                #   The TAIL-JUMP is the dispatch -- the pushed trampoline proves the
                #   body ends there -- so a preceding call is bookkeeping, not another
                #   destination. A jump elsewhere still ends the body.
                if ins.mnemonic == "jmp":
                    break
                continue
            if hit is not None:
                return None                  # more than one -- ambiguous, say nothing
            hit = t
            if ins.mnemonic == "jmp":
                break                        # a tail-jump IS the end of the body
    return hit


def build(path, stubs=None, ne=None):
    ne = ne or NE(path)
    stubs = stubs if stubs is not None else \
        max(stub_tables(path).values(), key=len)
    names = export_names(ne)
    byid = {}                                # id -> dict(args, off, name, how)
    for (seg, off), (fid, cnt) in stubs.items():
        byid.setdefault(fid, dict(args=cnt, seg=seg, off=off, name=None, how=""))
    for ordv, (_fl, kind, seg, val) in sorted(ne.entries().items()):
        if seg is None:
            continue
        nm = names.get(ordv)
        if not nm:
            continue
        if (seg, val) in stubs:              # DIRECT: the export IS the stub
            fid = stubs[(seg, val)][0]
            byid[fid]["name"], byid[fid]["how"], byid[fid]["ord"] = nm, "DIRECT", ordv
            continue
        t = wrapper_target(ne, seg, val, stubs)
        if t is not None:
            fid = stubs[(seg, t)][0]
            if byid[fid]["how"] != "DIRECT":  # never let an inference beat a fact
                byid[fid]["name"], byid[fid]["how"], byid[fid]["ord"] = nm, "WRAPPER", ordv
    return ne, byid


def main():
    a = [x for x in sys.argv[1:] if not x.startswith("--")]
    if not a:
        print(__doc__)
        return 2
    tables = stub_tables(a[0])
    ne0 = NE(a[0])
    if "--md" not in sys.argv and len(tables) > 1:
        print("%s has %d SEPARATE stub tables; each has its own id space:" % (a[0], len(tables)))
        for k in sorted(tables, key=lambda k: -len(tables[k])):
            print("   %-28s %3d stubs" % (table_name(k), len(tables[k])))
        print()
    # ★ DEFAULT: the table this module's OWN thunk serves, if it has one. krnl386
    #   owns the common thunk, so its native surface is `seg1 -> own thunk 0x2bb6`
    #   -- the 82 ids wow32.h implements -- and picking "the biggest" instead would
    #   silently hand back seg2's 121 and redefine the documented surface under a
    #   reader who did not ask. Modules that only IMPORT the thunk have one table
    #   and get it. `--table=...` overrides.
    own = [k for k in tables if k[1] != IMPORTED_THUNK]
    pool = own or list(tables)
    key = max(pool, key=lambda k: len(tables[k])) if pool else None
    for opt in sys.argv[1:]:
        if opt.startswith("--table="):
            sel = opt.split("=", 1)[1]
            for k in tables:
                if table_name(k).replace(" ", "") == sel.replace(" ", ""):
                    key = k
    if key is None:
        print("%s: no WOW32 stub tables" % a[0]); return 1
    ne, byid = build(a[0], tables[key], ne0)
    named = [f for f in byid if byid[f]["name"]]
    direct = [f for f in named if byid[f]["how"] == "DIRECT"]
    if "--md" in sys.argv:
        print("| ID | args | stub | name | evidence |")
        print("|---|---|---|---|---|")
        for fid in sorted(byid):
            e = byid[fid]
            print("| `0x%02x` | %d | `seg%d:0x%04x` | %s | %s |"
                  % (fid, e["args"], e["seg"], e["off"],
                     "**%s**" % e["name"] if e["name"] else "—",
                     e["how"] or "internal only — read the call site"))
        return 0
    print("%s [%s]: %d WOW32 function IDs -- %d NAMED (%d direct, %d wrapper), %d unnamed"
          % (a[0], table_name(key), len(byid), len(named), len(direct),
             len(named) - len(direct), len(byid) - len(named)))
    for fid in sorted(byid):
        e = byid[fid]
        print("  id 0x%02x  args=%2d  seg%d:0x%04x  %-9s %s"
              % (fid, e["args"], e["seg"], e["off"], e["how"] or "-", e["name"] or ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
