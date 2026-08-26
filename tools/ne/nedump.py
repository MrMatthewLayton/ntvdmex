#!/usr/bin/env python3
"""nedump.py -- read a 16-bit New Executable and print what a loader must handle.

GH #128/#4. Written before the C loader on purpose: the NE format is old, widely
half-documented, and the parts that actually matter are the ones a real binary uses,
not the ones the spec describes. So this dumps REAL inputs -- Microsoft's
krnl386.exe, user.exe, gdi.exe, and an ordinary app like sysedit.exe -- and the
loader is written against what comes out.

    tools/ne/nedump.py guest/ne/krnl386.exe
    tools/ne/nedump.py guest/ne/*.exe --summary

⚠ The binaries are Microsoft's and are NOT in this repository. Extract your own with
  `rigshot isne` to find them and copy them out; `guest/` is gitignored.
"""
import struct
import sys

TARGET_OS = {0: "unknown/OS2", 1: "OS/2", 2: "Windows", 3: "European DOS 4.x",
             4: "Windows 386", 5: "BOSS"}

# NE header flags (word at +0x0C)
PROG_FLAGS = [(0x0001, "DGROUP=SINGLEDATA"), (0x0002, "DGROUP=MULTIPLEDATA"),
              (0x2000, "LINK_ERROR"), (0x8000, "LIBRARY")]
APP_FLAGS = [(0x0001, "FULLSCREEN"), (0x0002, "WINPIF"), (0x0004, "WINAPI"),
             (0x0800, "OS2_FAMILY"), (0x2000, "ERR_IMAGE"), (0x8000, "SELF_LOAD")]

SEG_FLAGS = [(0x0001, "DATA"), (0x0010, "MOVEABLE"), (0x0020, "SHAREABLE"),
             (0x0040, "PRELOAD"), (0x0080, "RO/EXECONLY"), (0x0100, "RELOCS"),
             (0x0200, "CONFORMING"), (0x1000, "DISCARDABLE")]

RELOC_ADDR = {0: "LOBYTE", 2: "SEGMENT", 3: "FAR_ADDR(32)", 5: "OFFSET(16)",
              11: "FAR_ADDR(48)", 13: "OFFSET(32)"}
RELOC_TYPE = {0: "INTERNALREF", 1: "IMPORTORDINAL", 2: "IMPORTNAME", 3: "OSFIXUP"}


def flags(v, table):
    out = [n for bit, n in table if v & bit]
    return ",".join(out) if out else "-"


def pstr(b, o):
    """Length-prefixed (Pascal) string."""
    n = b[o]
    return b[o + 1:o + 1 + n].decode("latin1"), o + 1 + n


class NE:
    def __init__(self, path):
        self.path = path
        self.d = open(path, "rb").read()
        if self.d[:2] != b"MZ":
            raise ValueError("no MZ header")
        self.off = struct.unpack_from("<I", self.d, 0x3C)[0]
        if self.d[self.off:self.off + 2] != b"NE":
            raise ValueError("not an NE (second header is %r)"
                             % self.d[self.off:self.off + 2])
        h = self.off
        # ⚠ Field offsets verified against the real binaries, not from memory. The
        #   first cut put "app flags" at 0x0E; 0x0E is ne_autodata, and every table
        #   offset after it was then wrong -- which showed up as a bogus import name.
        #   02 ver .03 rev .04 enttab .06 cbenttab .08 crc .0C flags .0E autodata
        #   10 heap .12 stack .14 csip .18 sssp .1C cseg .1E cmod .20 cbnrestab
        #   22 segtab .24 rsrctab .26 restab .28 modtab .2A imptab .2C nrestab
        #   30 cmovent .32 align .34 cres .36 exetyp .37 flagsothers
        #   38 pretthunks .3A psegrefbytes .3C swaparea .3E expver
        (self.linker_ver, self.linker_rev, self.entry_off, self.entry_len,
         self.crc, self.prog_flags, self.auto_data_seg,
         self.heap, self.stack, self.csip, self.sssp, self.n_seg, self.n_mod,
         self.nonres_len, self.seg_tab, self.res_tab, self.resident_tab,
         self.mod_tab, self.imp_tab, self.nonres_off, self.n_movable,
         self.align_shift, self.n_res, self.target_os, self.other_flags,
         self.ret_thunks, self.seg_thunks, self.min_swap, self.expect_ver
         ) = struct.unpack_from("<BBHHIHHHHIIHHHHHHHHIHHHBBHHHH", self.d, h + 2)
        self.app_flags = self.other_flags
        self.shift = self.align_shift or 9

    # ---- tables ----
    def segments(self):
        out = []
        for i in range(self.n_seg):
            o = self.off + self.seg_tab + i * 8
            sec, ln, fl, mn = struct.unpack_from("<HHHH", self.d, o)
            out.append(dict(i=i + 1, sector=sec, file_off=sec << self.shift,
                            length=ln or (0x10000 if sec else 0),
                            flags=fl, minalloc=mn))
        return out

    def modules(self):
        """Referenced module names -- i.e. what this NE imports FROM."""
        out = []
        for i in range(self.n_mod):
            o = self.off + self.mod_tab + i * 2
            noff = struct.unpack_from("<H", self.d, o)[0]
            s, _ = pstr(self.d, self.off + self.imp_tab + noff)
            out.append(s)
        return out

    def resident_names(self):
        out, o = [], self.off + self.resident_tab
        while True:
            n = self.d[o]
            if n == 0:
                break
            s, o = pstr(self.d, o)
            ordv = struct.unpack_from("<H", self.d, o)[0]
            o += 2
            out.append((ordv, s))
        return out

    def relocs(self, seg):
        """Relocation records for a segment, if it has any."""
        if not (seg["flags"] & 0x0100):
            return []
        o = seg["file_off"] + seg["length"]
        n = struct.unpack_from("<H", self.d, o)[0]
        o += 2
        out = []
        for _ in range(n):
            at, ty, at_off, a, b = struct.unpack_from("<BBHHH", self.d, o)
            o += 8
            out.append(dict(addr_type=at, rel_type=ty, offset=at_off, a=a, b=b))
        return out


def dump(path, summary=False):
    try:
        ne = NE(path)
    except Exception as e:                                        # noqa: BLE001
        print(f"{path}: {e}")
        return
    segs = ne.segments()
    mods = ne.modules()
    if summary:
        nrel = sum(len(ne.relocs(s)) for s in segs)
        print(f"{path:28} os={TARGET_OS.get(ne.target_os, ne.target_os):8} "
              f"segs={ne.n_seg:3} movable={ne.n_movable:3} relocs={nrel:5} "
              f"imports={len(mods):2} {flags(ne.prog_flags, PROG_FLAGS)}")
        return

    print(f"\n=== {path}")
    print(f"  NE header at 0x{ne.off:x}   linker {ne.linker_ver}.{ne.linker_rev}"
          f"   target: {TARGET_OS.get(ne.target_os, ne.target_os)}"
          f"   expects Windows {ne.expect_ver >> 8}.{ne.expect_ver & 0xFF}")
    print(f"  prog flags 0x{ne.prog_flags:04x} [{flags(ne.prog_flags, PROG_FLAGS)}]"
          f"   other flags 0x{ne.other_flags:02x}")
    print(f"  CS:IP = seg {ne.csip >> 16}:0x{ne.csip & 0xFFFF:04x}"
          f"   SS:SP = seg {ne.sssp >> 16}:0x{ne.sssp & 0xFFFF:04x}"
          f"   auto-data seg {ne.auto_data_seg}")
    print(f"  heap 0x{ne.heap:x}  stack 0x{ne.stack:x}  "
          f"align shift {ne.align_shift} (x{1 << ne.shift})  "
          f"movable entries {ne.n_movable}")

    print(f"\n  -- {ne.n_seg} segments --")
    print("   #  fileoff   len   minalloc  flags")
    for s in segs:
        print(f"  {s['i']:3} 0x{s['file_off']:06x} {s['length']:6} "
              f"{s['minalloc']:9}  0x{s['flags']:04x} [{flags(s['flags'], SEG_FLAGS)}]")

    print(f"\n  -- imports from {len(mods)} modules --")
    print("   " + (", ".join(mods) if mods else "(none)"))

    rn = ne.resident_names()
    if rn:
        print(f"\n  -- resident names (module = {rn[0][1]}) --")
        for ordv, s in rn[1:9]:
            print(f"   @{ordv:<5} {s}")
        if len(rn) > 9:
            print(f"   ... and {len(rn) - 9} more")

    print("\n  -- relocation mix (what the loader must implement) --")
    tally = {}
    for s in segs:
        for r in ne.relocs(s):
            k = (RELOC_TYPE.get(r["rel_type"] & 3, r["rel_type"] & 3),
                 RELOC_ADDR.get(r["addr_type"], r["addr_type"]))
            tally[k] = tally.get(k, 0) + 1
    if not tally:
        print("   (none)")
    for (rt, at), n in sorted(tally.items(), key=lambda kv: -kv[1]):
        print(f"   {n:6}  {rt:<14} {at}")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    summary = "--summary" in sys.argv
    if not args:
        print(__doc__)
        return 2
    for p in args:
        dump(p, summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
