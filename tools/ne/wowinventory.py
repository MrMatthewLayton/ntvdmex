#!/usr/bin/env python3
"""wowinventory.py -- THE WHOLE Win16 CALL SURFACE, LIBRARY BY LIBRARY.  GH #128.

    tools/ne/wowinventory.py                      # summary + the gap list
    tools/ne/wowinventory.py --md  > docs/inventory/win16-surface.md
    tools/ne/wowinventory.py --json > build/win16-surface.json

── WHY THIS EXISTS (session 89) ─────────────────────────────────────────────
`neneeds.py` answers "what does THIS program need?". That is app-first, and it
finds one program's walls. The user's question was the other one: *what does
Windows 3.1 itself expose, and how much of it do we answer?* -- so that the work
is a list with an end, filled library by library, and a program nobody has tried
yet finds its calls already there.

The surface is not the export list. Most exports are 16-bit code inside the
module and run on the real CPU without asking us. What reaches THIS host is the
numbered 16->32 call each WOW stub makes (`wowthunks.py`), and every module's
stub table can be read straight out of the binary. So one row here is:

    (module, stub table, id)  argbytes  export name(s)  Wine's argument types
                              handled? (a `case` in the module's dispatcher)
                              how many shelf programs reach it, and which

The shelf count is TRANSITIVE through the shelf's own 16-bit DLLs: a program
that imports VER.DLL is charged with every thunked call VER.DLL makes. That is
an over-count (the program may not reach all of VER), and it is labelled so --
but the alternative, ignoring DLLs, under-counts calls like COMMDLG's, which no
program imports from USER directly and every Open dialog makes.

── WHAT A COLUMN DOES *NOT* SAY ─────────────────────────────────────────────
⚠ "handled" = the dispatcher has a `case` for the id. NOT that the answer is
  right -- this project's own rule: an implemented id is not a correct one. Each
  batch filled from this list is verified against stock (tools/wintest or a
  same-desktop screenshot) before it is called done.
⚠ "kind" comes from Wine's .spec argument types, fetched once into
  build/winespec/ (LGPL data, so it is cached, not vendored):
      values    words/longs only -- still needs HANDLE mapping, but no memory
      pointer   a ptr/str: the host must read or write guest memory
      callback  the function takes or installs 16-bit code (Enum*, hooks,
                dialog procs, timers...) -- structural: needs wow_call16_sync()
  A row with no Wine entry says "?" rather than guessing.
⚠ An export classified native16 can STILL reach us internally (USER's LoadIcon
  -> id 0xad). Those ids appear here as "(internal)" rows -- the table is the
  stubs, not the exports -- but their shelf count is only what an export maps
  to, so an internal id with 0 users may well be in use. Unnamed internal ids
  are the place to read call sites (`nedis.py --wowfunc`).
⚠ MMSYSTEM does not use the WOW stub shape at all (#5): its rows come from the
  export table, and none is "handled" because no 32-bit half exists yet.
"""
import json
import os
import re
import struct
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from nedump import NE, pstr                                     # noqa: E402
from neimports import REL_ORDINAL, REL_NAME                     # noqa: E402
from neneeds import classify                      # noqa: E402
from wowthunks import IMPORTED_THUNK                            # noqa: E402
from wowmap import stub_tables, build, table_name               # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
SHELF = os.path.join(ROOT, "guest", "win16")

# ★ WHICH COPY. A Win16 program on XP gets SYSTEM32's modules, not the 3.1 ones
#   beside it: guest/win16/COMMDLG.DLL is 3.1's 97 KB all-16-bit COMMDLG, and
#   XP's is a 32 KB thunk module with two stub tables. Inventorying the wrong
#   one reports "COMMDLG needs nothing from us", which is false.
#   module -> (binary, dispatcher, Wine spec)
SYSMODS = {
    "KERNEL":   ("guest/ne/krnl386.exe",  "src/wow/wow32.h",      "krnl386.exe16"),
    "USER":     ("guest/ne/user.exe",     "src/wow/wowuser.h",    "user.exe16"),
    "GDI":      ("guest/ne/gdi.exe",      "src/wow/wowgdi.h",     "gdi.exe16"),
    "KEYBOARD": ("guest/ne/keyboard.drv", "src/wow/wowkbd.h",     "keyboard.drv16"),
    "SHELL":    ("guest/ne/shell.dll",    "src/wow/wowshell.h",   "shell.dll16"),
    "COMMDLG":  ("guest/ne/commdlg.dll",  "src/wow/wowcommdlg.h", "commdlg.dll16"),
    "SOUND":    ("guest/ne/sound.drv",    "src/wow/wowsound.h",   "sound.drv16"),
    "SYSTEM":   ("guest/ne/system.drv",   None,                   "system.drv16"),
    "MOUSE":    ("guest/ne/mouse.drv",    None,                   None),
    "COMM":     ("guest/ne/comm.drv",     None,                   None),
    "TOOLHELP": ("guest/ne/toolhelp.dll", None,                   None),
    "WINNLS":   ("guest/ne/winnls.dll",   None,                   None),
    "MMSYSTEM": ("guest/win16/mmsystem.dll", None,                "mmsystem.dll16"),
}
NOSTUBS = {"MMSYSTEM"}                     # see the docstring: not WOW-stub shaped

WINE = "https://gitlab.winehq.org/wine/wine/-/raw/master/dlls/%s/%s.spec"
SPECDIR = os.path.join(ROOT, "build", "winespec")

CALLBACK_RE = re.compile(r"Enum|Hook|Proc\b|ProcInstance|Callback|SetTimer|KillTimer|"
                         r"DialogBox|CreateDialog|LineDDA|GrayString|SetAbortProc|"
                         r"SetWindowLong|SetClassLong|CallWindowProc|DispatchMessage|"
                         r"SendMessage|SendDlgItemMessage|GetOpenFileName|GetSaveFileName|"
                         r"ChooseColor|ChooseFont|PrintDlg|FindText|ReplaceText", re.I)


# ── Wine's specs ───────────────────────────────────────────────────────────
def wine_spec(name):
    """ordinal -> (name, [argtypes]) from Wine's .spec, cached in build/."""
    if not name:
        return {}
    os.makedirs(SPECDIR, exist_ok=True)
    path = os.path.join(SPECDIR, name + ".spec")
    if not os.path.exists(path):
        try:
            with urllib.request.urlopen(WINE % (name, name), timeout=20) as r:
                open(path, "wb").write(r.read())
        except Exception as exc:                                # noqa: BLE001
            sys.stderr.write("wine spec %s not fetched (%s); kinds will be '?'\n"
                             % (name, exc))
            return {}
    out = {}
    for line in open(path, errors="replace"):
        line = line.split("#", 1)[0].strip()
        m = re.match(r"(\d+)\s+(\S+)\s+((?:-\S+\s+)*)(\w+)(?:\(([^)]*)\))?", line)
        if not m:
            continue
        out[int(m.group(1))] = (m.group(4), (m.group(5) or "").split(),
                                m.group(2) == "stub")
    return out


def kind_of(names, args):
    if args is None:
        return "?"
    if any(CALLBACK_RE.search(n) for n in names):
        return "callback"
    if any(a in ("ptr", "str", "segptr", "segstr") for a in args):
        return "pointer"
    return "values"


# ── one system module ─────────────────────────────────────────────────────
def host_names(dispatcher):
    """id -> the name the HOST gave it (`#define WOWUSER_FOO 0x..` used in a
    `case`). Covers ids the export tables cannot name -- the internal ones the
    project already worked out from call sites."""
    if not dispatcher:
        return {}
    src = open(os.path.join(ROOT, dispatcher), errors="replace").read()
    defs = {m.group(1): int(m.group(2), 16)
            for m in re.finditer(r"#define\s+(\w+)\s+(0x[0-9a-fA-F]+)", src)}
    out = {}
    for m in re.finditer(r"^\s*case\s+(\w+)\s*:", src, re.M):
        if m.group(1) in defs:
            out.setdefault(defs[m.group(1)], m.group(1))
    return out


def serviced(dispatcher):
    """The ids the dispatcher SERVICES: `neneeds.serviced_ids` minus the labels
    that only ever appear in a NAME TABLE.
    ⚠ wow32.h has `case WOW32_WRITEOUTPROFILES: return "WriteOutProfiles";` --
      a `case` that names an id for the log and answers nothing. Counted as
      handled, every id with a log name looked done."""
    if not dispatcher:
        return set()
    src = open(os.path.join(ROOT, dispatcher), errors="replace").read()
    defs = {m.group(1): int(m.group(2), 16)
            for m in re.finditer(r"#define\s+(\w+)\s+(0x[0-9a-fA-F]+)", src)}
    out = set()
    lab = re.compile(r"\s*case\s+(\w+)\s*:")
    for m in re.finditer(r"^\s*case\s+(\w+)\s*:", src, re.M):
        tok = m.group(1)
        fid = defs.get(tok) if tok in defs else (int(tok, 16) if tok.startswith("0x") else None)
        if fid is None:
            continue
        rest, p = src[m.end():], 0
        while True:                             # skip stacked `case X:` labels
            n = lab.match(rest, p)
            if not n:
                break
            p = n.end()
        if re.match(r'\s*return\s+"', rest[p:]):
            continue                            # a name table, not a service
        out.add(fid)
    return out


def module_rows(mod):
    """[(table, id) -> row] for every stub, plus ordinal -> (table, id)."""
    rel, disp, spec = SYSMODS[mod]
    path = os.path.join(ROOT, rel)
    ne = NE(path)
    wine = wine_spec(spec)
    names = {}
    for o, s in ne.resident_names() + ne.nonresident_names():
        if o:
            names.setdefault(o, s)
    done = serviced(disp)
    hn = host_names(disp)
    rows, ord2key = {}, {}

    if mod in NOSTUBS:
        for o, (wn, args, stub) in sorted(wine.items()):
            key = ("exports", o)
            rows[key] = dict(module=mod, table="exports", id=o, args=None,
                             names=[names.get(o, wn)], ords=[o], how="EXPORT",
                             wine=args, kind=kind_of([wn], args),
                             handled=False, users=set())
            ord2key[o] = key
        return rows, ord2key, ne

    tables = stub_tables(path)
    starts = {}
    for tkey, stubs in tables.items():
        tname = table_name(tkey)
        _ne, byid = build(path, stubs, ne)
        for (seg, off), (fid, _c) in stubs.items():
            starts[(seg, off)] = (tname, fid)
        for fid, e in byid.items():
            key = (tname, fid)
            rows[key] = dict(module=mod, table=tname, id=fid, args=e["args"],
                             names=[e["name"]] if e["name"] else [],
                             ords=[e["ord"]] if e.get("ord") else [],
                             how=e["how"] or "internal", wine=None, kind="?",
                             handled=fid in done, users=set(), host=hn.get(fid))

    # every export -> its stub, through neneeds' classifier (it knows the
    # validating-wrapper shapes wowmap's simpler walk does not)
    segs = ne.segments()
    for o, ent in ne.entries().items():
        seg, off = ent[2], ent[3]
        if not o or not isinstance(seg, int) or not isinstance(off, int):
            continue
        if not 1 <= seg <= len(segs):
            continue
        kind, fid, argb, rets = classify(ne, segs, seg, off, 0)
        if kind != "wow32" or rets is None:
            continue
        hit = starts.get((seg, (rets - 13) & 0xFFFF)) or starts.get((seg, (rets - 14) & 0xFFFF))
        if not hit or hit[1] != fid:
            continue
        r = rows[hit]
        nm = names.get(o) or (wine.get(o, ("?",))[0])
        if nm not in r["names"]:
            r["names"].append(nm)
        if o not in r["ords"]:
            r["ords"].append(o)
        if r["how"] == "internal":
            r["how"] = "EXPORT"
        ord2key[o] = hit

    # ⚠ AND wowmap's OWN MAPPING. `classify` knows only the imported-thunk stub
    #   (`... 9a`), so every krnl386 export -- whose stubs end `nop / push cs /
    #   call rel16` into krnl386's own thunk -- classified native16, and the first
    #   run reported KERNEL "used by 0 programs". wowmap's DIRECT/WRAPPER walk
    #   reads krnl386's shape; it gives one ordinal per id, which is enough.
    for key, r in rows.items():
        for o in r["ords"]:
            ord2key.setdefault(o, key)
            nm = names.get(o)
            if nm and nm not in r["names"]:
                r["names"].append(nm)

    for r in rows.values():
        for o in r["ords"]:
            if o in wine:
                r["wine"] = wine[o][1]
                break
        r["kind"] = kind_of(r["names"] or [r.get("host") or ""], r["wine"]) \
            if (r["wine"] is not None or r["names"]) else "?"
        if r["wine"] is None and r["kind"] != "callback":
            r["kind"] = "?"
    return rows, ord2key, ne


# ── the shelf ─────────────────────────────────────────────────────────────
def imports_of(path):
    """{(MODULE, ordinal)} and {(MODULE, NAME)} a file imports."""
    ne = NE(path)
    mods = ne.modules()
    byord, byname = set(), set()
    for s in ne.segments():
        for r in ne.relocs(s):
            k = r["rel_type"] & 3
            if not 1 <= r["a"] <= len(mods):
                continue
            m = mods[r["a"] - 1].upper()
            if k == REL_ORDINAL:
                byord.add((m, r["b"]))
            elif k == REL_NAME:
                nm, _ = pstr(ne.d, ne.off + ne.imp_tab + r["b"])
                byname.add((m, nm.upper()))
    return byord, byname


def module_name(ne):
    rn = ne.resident_names()
    return rn[0][1].upper() if rn else "?"


def shelf():
    """name -> (kind 'exe'|'dll', module name, imports-by-ord, imports-by-name)"""
    out = {}
    for fn in sorted(os.listdir(SHELF)):
        if not fn.upper().endswith((".EXE", ".DLL", ".DRV")):
            continue
        p = os.path.join(SHELF, fn)
        try:
            ne = NE(p)
        except Exception:                                       # noqa: BLE001
            continue                    # MZ-only (EXPAND.EXE) -- not a Win16 program
        mn = module_name(ne)
        if mn in SYSMODS:
            continue                    # a system module, or 3.1's COMMDLG -- see SYSMODS
        bo, bn = imports_of(p)
        out[fn] = ("exe" if fn.upper().endswith(".EXE") else "dll", mn, bo, bn)
    return out


# ── what the rig SAW ───────────────────────────────────────────────────────
FUNC_RE = re.compile(r"FUNC=0x([0-9a-f]+)[^\[\n]*\[([^\]]*)\].*?args=([0-9a-f]+)b "
                     r"retstub=0x([0-9a-f]+)")


def stub_index():
    """(id, argbytes, retstub) -> (module, table). The same triple the host's
    anchors recognise a module by, so a logged call resolves exactly."""
    out = {}
    for mod, (rel, _d, _s) in SYSMODS.items():
        if mod in NOSTUBS:
            continue
        for tkey, stubs in stub_tables(os.path.join(ROOT, rel)).items():
            for (_seg, off), (fid, cnt) in stubs.items():
                for k in (13, 14):
                    out.setdefault((fid, cnt, (off + k) & 0xFFFF), (mod, table_name(tkey)))
    return out


def mine_logs(paths):
    """{(module, table, id): {serviced, unimpl, declined, partial}} from host logs.
    ★ THE SOURCE SCAN CANNOT SEE EVERYTHING THE HOST ANSWERS. main.c services
      ids in front of the dispatchers (WaitEvent, the scheduler, GetMessage's
      wait...), so a `case`-only test called WaitEvent unhandled. The log is
      what happened: every call is a FUNC= line and an outcome line.
    `partial` = SERVICED, but the note says part of it is "not implemented"."""
    idx = stub_index()
    seen = {}
    for p in paths:
        pend = None
        for line in open(p, "rb"):
            line = line.decode("latin-1")
            m = FUNC_RE.search(line)
            if m:
                fid, tag = int(m.group(1), 16), m.group(2)
                argb, rets = int(m.group(3), 16), int(m.group(4), 16)
                hit = idx.get((fid, argb, rets & 0xFFFF))
                if not hit and tag == "krnl":
                    hit = ("KERNEL", "seg1 -> own thunk 0x2bb6")
                pend = hit + (fid,) if hit else None
                continue
            s = line.strip()
            if pend and s.startswith("->"):
                e = seen.setdefault(pend, dict(serviced=0, unimpl=0, declined=0, partial=0))
                if s.startswith("-> UNIMPLEMENTED"):
                    e["unimpl"] += 1
                elif s.startswith("-> DECLINED"):
                    e["declined"] += 1
                elif "not implemented" in s.lower() or "unimplemented" in s.lower():
                    e["partial"] += 1
                else:
                    e["serviced"] += 1
                pend = None
    return seen


def main():
    want_md = "--md" in sys.argv
    want_json = "--json" in sys.argv
    logs = []
    for a in sys.argv[1:]:
        if a.startswith("--logs="):
            import glob
            logs += sorted(glob.glob(a.split("=", 1)[1], recursive=True))
    allrows, o2k, names_rev = {}, {}, {}
    for mod in SYSMODS:
        rows, ord2key, ne = module_rows(mod)
        for key, r in rows.items():
            allrows[(mod,) + key] = r
        o2k[mod] = ord2key
        names_rev[mod] = {s.upper(): o for o, s in ne.resident_names() + ne.nonresident_names() if o}

    for r in allrows.values():
        r["seen"] = None
    if logs:
        for key, e in mine_logs(logs).items():
            if key in allrows:
                r = allrows[key]
                r["seen"] = e
                if e["serviced"] or e["declined"]:
                    r["handled"] = True        # main.c answers it, whatever the switch says

    # ⚠ A PHANTOM TABLE. wowthunks.py matches a byte pattern, and COMMDLG's seg3
    #   holds data that happens to fit it: ids 0x0 / 0x1 / 0x7f00, "255 argument
    #   bytes", reached by no export and never seen on the rig. A table is
    #   dropped only when ALL of that is true and it is small; krnl386's seg2
    #   table (121 stubs, no exports, but 165 calls in the logs) survives.
    bytable = {}
    for key, r in allrows.items():
        bytable.setdefault((key[0], key[1]), []).append(key)
    for tk, keys in bytable.items():
        rs = [allrows[k] for k in keys]
        junk = any(r["id"] >= 0x1000 or r["args"] == 255 for r in rs)
        if (junk and len(rs) < 8 and not any(r["ords"] for r in rs)
                and not any(r["seen"] for r in rs)):
            for k in keys:
                del allrows[k]

    sh = shelf()
    dllmods = {mn: fn for fn, (k, mn, _a, _b) in sh.items() if k == "dll"}

    def direct(fn):
        _k, _mn, bo, bn = sh[fn]
        keys, deps = set(), set()
        pairs = set(bo)
        for m, nm in bn:
            if m in names_rev and nm in names_rev[m]:
                pairs.add((m, names_rev[m][nm]))
        for m, o in pairs:
            if m in o2k and o in o2k[m]:
                keys.add((m,) + o2k[m][o])
            elif m in dllmods:
                deps.add(dllmods[m])
        return keys, deps

    memo = {}

    def reach(fn, stack=()):
        if fn in memo:
            return memo[fn]
        keys, deps = direct(fn)
        via = set()
        for d in deps:
            if d in stack:
                continue
            k2, _v = reach(d, stack + (fn,))
            via |= k2 - keys
            keys = keys | k2
        memo[fn] = (keys, via)
        return memo[fn]

    used_dll = set()
    for fn, (k, *_r) in sh.items():
        if k == "exe":
            used_dll |= direct(fn)[1]
    users = [fn for fn, (k, *_r) in sh.items() if k == "exe" or fn not in used_dll]
    for fn in users:
        keys, via = reach(fn)
        for key in keys:
            if key in allrows:
                allrows[key]["users"].add(fn.rsplit(".", 1)[0].upper()
                                          + ("*" if key in via else ""))

    def gap(r):
        """Used by the shelf and unanswered, OR seen stepped over on the rig and
        never answered -- the second catches the internal ids an import list
        cannot see (USER's own 16-bit code calling 0x13a, krnl386 calling 0xc6)."""
        if r["handled"]:
            return False
        return bool(r["users"]) or bool(r["seen"] and r["seen"]["unimpl"])

    mods = list(SYSMODS)
    summary = []
    for mod in mods:
        rs = [r for k, r in allrows.items() if k[0] == mod]
        used = [r for r in rs if r["users"]]
        summary.append(dict(
            module=mod, ids=len(rs), handled=sum(r["handled"] for r in rs),
            used=len(used), used_handled=sum(r["handled"] for r in used),
            gaps=len([r for r in rs if gap(r)]),
            partial=len([r for r in rs if r["seen"] and r["seen"]["partial"]])))

    if want_json:
        doc = dict(summary=summary, shelf=sorted(users), logs=logs, rows=[
            dict(r, users=sorted(r["users"])) for r in allrows.values()])
        print(json.dumps(doc, indent=1, sort_keys=True))
        return 0

    def label(r):
        return "/".join(r["names"]) if r["names"] else \
            ("(%s)" % r["host"] if r.get("host") else "(internal)")

    def sig(r):
        return "" if r["wine"] is None else "(" + " ".join(r["wine"]) + ")"

    def users_s(r, cap=99):
        u = sorted(r["users"], key=lambda s: (s.endswith("*"), s))
        more = " +%d" % (len(u) - cap) if len(u) > cap else ""
        return "%d" % len(u) + (": " + " ".join(u[:cap]) + more if u else "")

    def rig_s(r):
        e = r["seen"]
        if not e:
            return ""
        out = []
        if e["serviced"] or e["declined"]:
            out.append("ok %d" % (e["serviced"] + e["declined"]))
        if e["partial"]:
            out.append("part %d" % e["partial"])
        if e["unimpl"]:
            out.append("**STEPPED %d**" % e["unimpl"])
        return " ".join(out)

    def gapkey(r):
        stepped = r["seen"]["unimpl"] if r["seen"] else 0
        return (-len(r["users"]), -stepped, r["module"], r["id"])

    gaps = sorted((r for r in allrows.values() if gap(r)), key=gapkey)
    parts = sorted((r for r in allrows.values() if r["seen"] and r["seen"]["partial"]),
                   key=lambda r: -r["seen"]["partial"])

    if not want_md:
        print("%-9s %5s %8s %6s %13s %5s %5s" % ("module", "ids", "handled", "used",
                                                 "used+handled", "GAPS", "part"))
        for s in summary:
            print("%-9s %5d %8d %6d %13d %5d %5d" % (
                s["module"], s["ids"], s["handled"], s["used"], s["used_handled"],
                s["gaps"], s["partial"]))
        print("\nshelf (%d): %s" % (len(users), " ".join(sorted(users))))
        print("logs: %d" % len(logs))
        print("\nGAPS -- used by the shelf or stepped over on the rig, unanswered:")
        for r in gaps:
            print("  %-8s 0x%03x %-8s %-30s %-34s %-14s %s" % (
                r["module"], r["id"], r["kind"], label(r)[:30], sig(r)[:34],
                rig_s(r).replace("*", ""), users_s(r, 8)))
        if parts:
            print("\nPARTIAL -- answered, but the host's note says part is not implemented:")
            for r in parts:
                print("  %-8s 0x%03x %-30s %s" % (r["module"], r["id"], label(r)[:30], rig_s(r)))
        return 0

    # ── markdown ──
    p = print
    p("# Win16 call surface — every 16→32 call, library by library\n")
    p("> **Generated** by `tools/ne/wowinventory.py --md --logs=…`. Do not edit by hand — "
      "regenerate after any dispatcher change. GH #128 / the s89 inventory. "
      "Messages are a separate, hand-kept table: [`win16-messages.md`](win16-messages.md).\n")
    p("One row is one numbered WOW call a system module's 16-bit code makes to the "
      "32-bit side — the whole of what this host has to answer. Exports that are "
      "16-bit code inside the module are not rows: they run on the real CPU.\n")
    p("- **handled** = the module's dispatcher has a `case` that does more than name "
      "the id, **or** the rig log shows it answered (main.c services some ids in front "
      "of the dispatchers). *Not* that the answer is right — a row is *verified* only "
      "once its batch is checked against stock.")
    p("- **rig** = outcomes in the host logs read (%d logs): `ok` answered, `part` "
      "answered but the host's own note says part is not implemented (usually a "
      "message — see the message table), **STEPPED** = unimplemented, stepped over, "
      "the guest got a sentinel." % len(logs))
    p("- **kind** (Wine's argument types): `values` = words/longs only (handles "
      "still need mapping); `pointer` = reads/writes guest memory; `callback` = takes "
      "or installs 16-bit code (needs `wow_call16_sync()`); `?` = no Wine entry.")
    p("- **shelf** = programs in `guest/win16/` that import the call; `NAME*` = only "
      "through one of the shelf's 16-bit DLLs (an over-count: the program may not "
      "reach every call its DLL makes).")
    p("- `(internal)` / `(HOST_NAME)` = a stub no export maps to; the module reaches it "
      "from its own 16-bit code (e.g. `LoadIcon` → USER `0xad`). **0 shelf users does "
      "not mean unused** — the rig column is the evidence for these.")
    p("- MMSYSTEM has no WOW stubs at all (#5); its rows are its exports.\n")
    p("## Summary\n")
    p("| module | ids | handled | used by shelf | used + handled | **gaps** | partial |")
    p("|---|---:|---:|---:|---:|---:|---:|")
    t = [0] * 6
    for s in summary:
        p("| %s | %d | %d | %d | %d | **%d** | %d |" % (
            s["module"], s["ids"], s["handled"], s["used"], s["used_handled"],
            s["gaps"], s["partial"]))
        for i, f in enumerate(("ids", "handled", "used", "used_handled", "gaps", "partial")):
            t[i] += s[f]
    p("| **all** | %d | %d | %d | %d | **%d** | %d |\n" % tuple(t))
    p("Shelf (%d): %s\n" % (len(users), ", ".join(sorted(users))))
    p("## The gap list — the work, most-used first\n")
    p("Used by the shelf and unanswered, or stepped over on the rig.\n")
    p("| module | id | name | Wine signature | kind | rig | shelf |")
    p("|---|---|---|---|---|---|---|")
    for r in gaps:
        p("| %s | `0x%03x` | %s | %s | %s | %s | %s |" % (
            r["module"], r["id"], label(r), ("`%s`" % sig(r)) if sig(r) else "",
            r["kind"], rig_s(r), users_s(r)))
    p("")
    if parts:
        p("## Answered, but partly not implemented (from the host's own notes)\n")
        p("| module | id | name | rig |")
        p("|---|---|---|---|")
        for r in parts:
            p("| %s | `0x%03x` | %s | %s |" % (r["module"], r["id"], label(r), rig_s(r)))
        p("")
    for mod in mods:
        rs = sorted((r for k, r in allrows.items() if k[0] == mod),
                    key=lambda r: (r["handled"], -len(r["users"]), r["table"], r["id"]))
        if not rs:
            continue
        s = [x for x in summary if x["module"] == mod][0]
        p("## %s — %d ids, %d handled, %d gaps\n" % (mod, s["ids"], s["handled"], s["gaps"]))
        p("Unhandled first, then most-used.\n")
        p("| id | table | args | name | Wine signature | kind | handled | rig | shelf |")
        p("|---|---|---:|---|---|---|---|---|---|")
        for r in rs:
            p("| `0x%03x` | %s | %s | %s | %s | %s | %s | %s | %s |" % (
                r["id"], r["table"].replace(" -> ", "→"),
                "" if r["args"] is None else r["args"], label(r),
                ("`%s`" % sig(r)) if sig(r) else "", r["kind"],
                "✅" if r["handled"] else "—", rig_s(r), users_s(r)))
        p("")
    return 0


if __name__ == "__main__":
    sys.exit(main())
