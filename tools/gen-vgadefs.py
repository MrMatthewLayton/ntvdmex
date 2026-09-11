#!/usr/bin/env python3
"""gen-vgadefs.py -- turn a vgadefs.com run into src/vdd/vga_defaults.h.

WHAT A MODE SET LEAVES BEHIND is a table, not a formula, and it is per mode. The
host used to hold ONE sixteen-entry attribute table and ONE DAC seeding for every
mode, which is wrong for most of them:

    modes 00-03, 10h, 12h   AC 00 01 02 03 04 05 14 07 38..3F   DAC = the EGA 64
    modes 0Dh, 0Eh          AC 00 01 02 03 04 05 06 07 10..17   DAC = CGA 16, x4
    mode  13h               AC 00..0F (identity)                DAC = the VGA 256
    modes 04h/05h/06h/07h/0Fh/11h each have their own AC table again

Transcribing four 256-entry DAC tables by hand is exactly the sort of thing that
goes wrong in one entry and is then invisible for a year, so they are generated
from the probe's own output. tools/dostest/vgadefs.ref.txt is that output, taken
from genuine MS-DOS 6.22 under scripts/dosoracle -- regenerate it with

    nasm -f bin tools/dostest/vgadefs.asm -o tools/dostest/vgadefs.com
    scripts/dosoracle/dosoracle.py run tools/dostest/vgadefs.com \\
        > tools/dostest/vgadefs.ref.txt
    tools/gen-vgadefs.py

⚠ The oracle runs SeaVGABIOS, which dosoracle's own docstring warns is only
another reimplementation's opinion at the BIOS layer. For THIS table it is
corroborated twice over: the values match the tables documented for the IBM VGA
BIOS, and mode 10h's table is byte-for-byte the one Lemmings carries inside
VGALEMMI.EXE at img+5507 to decide which DAC entries to program -- a shipping
1991 game's opinion of the hardware it ran on.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = os.path.join(ROOT, "tools", "dostest", "vgadefs.ref.txt")
OUT = os.path.join(ROOT, "src", "vdd", "vga_defaults.h")


def parse(path):
    modes, order, cur = {}, [], None
    for line in open(path):
        m = re.match(r"MODE=(\w\w) AC=(.*)", line)
        if m:
            cur = int(m.group(1), 16)
            order.append(cur)
            modes[cur] = {"ac": [int(x, 16) for x in m.group(2).split()], "dac": []}
            continue
        m = re.match(r"DAC(\w\w)=(.*)", line)
        if m and cur is not None:
            idx = int(m.group(1), 16)
            if idx != len(modes[cur]["dac"]):
                sys.exit("mode %02X: DAC line %02X arrived with %d entries read -- "
                         "the dump is truncated or out of order"
                         % (cur, idx, len(modes[cur]["dac"])))
            for v in m.group(2).split():
                modes[cur]["dac"].append((int(v[0:2], 16), int(v[2:4], 16),
                                          int(v[4:6], 16)))
    for k, v in modes.items():
        if len(v["ac"]) != 16 or len(v["dac"]) != 256:
            sys.exit("mode %02X: got %d AC and %d DAC entries, want 16 and 256"
                     % (k, len(v["ac"]), len(v["dac"])))
    if not modes:
        sys.exit("%s holds no MODE= lines" % path)
    return modes, order


def x8(v):
    """6-bit DAC component -> 8 bits, the way a VGA DAC does it: the top two bits
    are replicated into the bottom. 0x3F -> 0xFF, 0x2A -> 0xAA, 0x15 -> 0x55, so
    the EGA-64 entries come out exactly as the old ega64_rgb() computed them."""
    return ((v << 2) | (v >> 4)) & 0xFF


def check_ega64(modes):
    """The EGA-64 table vdd_video.c used to COMPUTE must equal what mode 10h
    MEASURES, or this generator has silently changed every default colour.

    The old ega64_rgb() read a six-bit rgbRGB value as two bits per channel, the
    primary worth twice the secondary, and expanded each channel as 0/85/170/255.
    If that still agrees entry for entry, replacing the formula with the table is
    a no-op for every mode that used it -- which is what makes this change safe to
    assess as "did Lemmings get its colours" rather than "did everything shift"."""
    for i in range(64):
        R = ((i >> 2) & 1) * 2 + ((i >> 5) & 1)
        G = ((i >> 1) & 1) * 2 + ((i >> 4) & 1)
        B = ((i >> 0) & 1) * 2 + ((i >> 3) & 1)
        want = (R * 85, G * 85, B * 85)
        got = tuple(x8(c) for c in modes[0x10]["dac"][i])
        if want != got:
            sys.exit("mode 10h DAC[%02X] measures %s but ega64_rgb() computed %s -- "
                     "the old default palette and the measured one disagree, so this "
                     "is not the no-op it is documented to be" % (i, got, want))


def main():
    modes, order = parse(REF)
    check_ega64(modes)

    # Modes measured with bit 7 set exist only to prove the bit changes nothing.
    for m in list(modes):
        if m & 0x80:
            base = m & 0x7F
            if base in modes and modes[base] != modes[m]:
                sys.exit("mode %02X and %02X disagree: bit 7 ('do not clear the "
                         "buffer') is not supposed to affect the palette load" % (base, m))
            del modes[m]
            order.remove(m)

    ac_tabs, dac_tabs = [], []
    ac_of, dac_of = {}, {}
    for m in order:
        a = tuple(modes[m]["ac"])
        d = tuple(modes[m]["dac"])
        if a not in ac_of:
            ac_of[a] = len(ac_tabs)
            ac_tabs.append(a)
        if d not in dac_of:
            dac_of[d] = len(dac_tabs)
            dac_tabs.append(d)

    w = []
    w.append("/* GENERATED by tools/gen-vgadefs.py from tools/dostest/vgadefs.ref.txt.")
    w.append("   Do not edit: edit the probe, re-run it against the oracle, regenerate.")
    w.append("   These are MEASURED on genuine MS-DOS 6.22, not taken from a datasheet")
    w.append("   and not inferred from a picture -- inferring them from a picture is how")
    w.append("   the single-table version got index 6 wrong. */")
    w.append("#ifndef NTVDMEX_VGA_DEFAULTS_H")
    w.append("#define NTVDMEX_VGA_DEFAULTS_H")
    w.append("")
    w.append("/* The sixteen Attribute Controller palette registers, per mode. */")
    w.append("static const unsigned char VGA_AC_DEFAULT[%d][16] = {" % len(ac_tabs))
    for i, a in enumerate(ac_tabs):
        ms = [m for m in order if tuple(modes[m]["ac"]) == a]
        w.append("    /* %d: modes %s */" % (i, ", ".join("%02Xh" % m for m in ms)))
        w.append("    { " + ", ".join("0x%02X" % v for v in a) + " },")
    w.append("};")
    w.append("")
    w.append("/* The 256 DAC entries, per mode, as 0x00RRGGBB. */")
    w.append("static const unsigned long VGA_DAC_DEFAULT[%d][256] = {" % len(dac_tabs))
    for i, d in enumerate(dac_tabs):
        ms = [m for m in order if tuple(modes[m]["dac"]) == d]
        w.append("    /* %d: modes %s */" % (i, ", ".join("%02Xh" % m for m in ms)))
        w.append("    {")
        for row in range(0, 256, 4):
            vals = []
            for (r, g, b) in d[row:row + 4]:
                vals.append("0x%02X%02X%02XuL" % (x8(r), x8(g), x8(b)))
            w.append("    " + " ".join(v + "," for v in vals))
        w.append("    },")
    w.append("};")
    w.append("")
    w.append("/* mode -> (AC table, DAC table). A mode that is not listed is not one the")
    w.append("   BIOS has an opinion about; vga_defaults_for() picks a fallback. */")
    w.append("static const unsigned char VGA_DEFAULT_BY_MODE[3][%d] = {" % len(order))
    w.append("    { " + ", ".join("0x%02X" % m for m in order) + " },")
    w.append("    { " + ", ".join("%d" % ac_of[tuple(modes[m]["ac"])] for m in order) + " },")
    w.append("    { " + ", ".join("%d" % dac_of[tuple(modes[m]["dac"])] for m in order) + " },")
    w.append("};")
    w.append("#define VGA_DEFAULT_MODES %d" % len(order))
    w.append("")
    w.append("#endif")
    open(OUT, "w").write("\n".join(w) + "\n")
    print("wrote %s: %d AC tables, %d DAC tables, %d modes"
          % (OUT, len(ac_tabs), len(dac_tabs), len(order)))


if __name__ == "__main__":
    main()
