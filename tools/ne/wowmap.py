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
from wowthunks import scan                              # noqa: E402

WRAPPER_WINDOW = 0x40          # how far into an export body to look for the call


def stub_table(path):
    """{(seg, off) -> (id, argbytes)} for every WOW32 stub in the module."""
    _ne, hits, _t = scan(path)
    return {(s, o): (f, c) for s, o, f, c, _tgt in hits}


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
    """If this export body calls exactly one stub before returning, which one?"""
    s = [x for x in ne.segments() if x["i"] == seg]
    if not s or s[0]["flags"] & 0x0001 or not s[0]["sector"]:
        return None
    d = ne.d[s[0]["file_off"]:s[0]["file_off"] + s[0]["length"]]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
    hit = None
    for ins in md.disasm(bytes(d[off:off + WRAPPER_WINDOW]), off):
        if ins.mnemonic in ("ret", "retf"):
            break
        if ins.mnemonic == "call" and ins.op_str.startswith("0x"):
            t = int(ins.op_str, 16) & 0xFFFF
            if (seg, t) not in stubs:
                return None                  # calls something else -- not a thin wrapper
            if hit is not None:
                return None                  # more than one -- ambiguous, say nothing
            hit = t
    return hit


def build(path):
    ne = NE(path)
    stubs = stub_table(path)
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
    ne, byid = build(a[0])
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
    print("%s: %d WOW32 function IDs -- %d NAMED (%d direct, %d wrapper), %d unnamed"
          % (a[0], len(byid), len(named), len(direct), len(named) - len(direct),
             len(byid) - len(named)))
    for fid in sorted(byid):
        e = byid[fid]
        print("  id 0x%02x  args=%2d  seg%d:0x%04x  %-9s %s"
              % (fid, e["args"], e["seg"], e["off"], e["how"] or "-", e["name"] or ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
