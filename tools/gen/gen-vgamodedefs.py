#!/usr/bin/env python3
"""gen-vgamodedefs.py -- turn the p_vgareg reference into src/vdd/vga_modedefs.h.

    ./tools/gen/gen-vgamodedefs.py            # reads tools/gen/data/vgareg.ref.txt

WHY THIS EXISTS. docs/inventory/vga.md measured what a REAL BIOS leaves in the VGA
register file after a mode set, and what we leave: about sixty meaningful values
against about five. A guest that reads a register back to learn its geometry -- or
saves and restores the card's state, which plenty do -- sees zeros here.

⚠ ONLY THE MODES THE PROBE ACTUALLY MEASURED GET A ROW. Writing a table for a mode
  nobody ran would be inventing the thing the inventory exists to stop; extend the
  probe instead and re-run this. Same rule vga_defaults.h follows.

⚠ THE DAC PIXEL MASK AND INPUT STATUS 0 ARE DELIBERATELY NOT EMITTED. The first is an
  open disagreement between the oracles, and the second is read-only status, not state
  to restore. (Input Status 0 was settled at 0x10 in s77 and lives in ext_in.)

⛔⛔⛔ TWO ORACLES, NOT ONE, AND IT MATTERED THE FIRST TIME IT WAS TRIED. This read only
  vgareg.ref.txt -- 6.22 under QEMU, i.e. the Bochs VGABIOS. PCem's genuine AMI 486 BIOS
  and IBM VGA ROM disagrees with it on 78 of 768 bytes, and GR7 is the worked example:
  both say 0x0F in the GRAPHICS modes, but in the TEXT and CGA modes (03, 04, 06, 07, 11)
  QEMU says 0x0F and PCem says 0x00. Regenerating from QEMU alone would have written
  0x0F into the text modes AGAINST THE REAL CARD -- and it would have looked like a fix,
  because it would have moved the parity score.

  ⇒ Where the two agree, that byte is a fact about VGAs and it is emitted.
  ⇒ Where they disagree, PCEM WINS and the byte is listed in the generated header, by
    name, so a tie-break is never invisible. This project targets a period-correct PC;
    it is the same rule that settled counter 0's power-on mode and Input Status 0.
  ⇒ EXCEPT MODE 7, WHICH IS EXCLUDED FROM THE TIE-BREAK -- see PCEM_BLIND below.

⛔⛔ AND "PCem WINS" IS NOT A RULE THAT CAN BE APPLIED BLIND. Mode 7 is the case that
  proves it: INT 10h AX=0007 leaves MiscOut 0x66 on QEMU -- bit 0 clear, the CRTC
  genuinely moved to 3B4, an MDA-compatible 80-column mode -- and 0x67 with a
  40-COLUMN CRTC on PCem, whose BIOS does not enter mode 7 on that machine at all.
  PCem is therefore not a better answer about mode 7; it is an answer about a
  different mode. Taking it would have programmed a 40-column colour text setup
  every time a guest asked for mode 7, from 30 tie-broken bytes, and every one of
  them would have been "measured". An oracle is only authoritative about the
  question it actually answered.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
REF = os.path.join(ROOT, "tools", "gen", "data", "vgareg.ref.txt")
REF2 = os.path.join(ROOT, "tools", "gen", "data", "vgareg.pcem.txt")
OUT = os.path.join(ROOT, "src", "vdd", "vga_modedefs.h")

GROUPS = ["Misc Output", "SEQ SR0-SR4", "CRTC CR00-CR18", "GC GR0-GR8", "AC AR00-AR14"]


# The 64-byte BUF layout p_vgareg emits, and the offsets each group lives at.
SPAN = {"Misc Output": (0, 1), "SEQ SR0-SR4": (4, 9),
        "CRTC CR00-CR18": (9, 34), "GC GR0-GR8": (34, 43), "AC AR00-AR14": (43, 64)}


def split_buf(raw):
    """A whole 64-byte capture -> the same {group: bytes} shape the block form gives."""
    return {g: raw[lo:hi] for g, (lo, hi) in SPAN.items()}


def parse(path):
    """-> {mode_label: {group: bytes}} for the ORACLE column only.

    Two forms are accepted, because the reference grew one. The original is a
    block per mode with a `6.22=` line per group; the compact one is the whole
    64-byte capture on the `==` line itself, which is what a raw probe run gives
    and what the later modes were recorded as. Silently supporting only the first
    is how five modes stayed in a table that had eleven available to it."""
    modes, cur, grp = {}, None, None
    for line in open(path):
        m = re.match(r"^== (vga\.\S+?)(?:\.(?:com|COM))?\s+([0-9A-Fa-f]{128})\s*$", line)
        if m:                                   # compact: name + the whole buffer
            modes[m.group(1)] = split_buf(bytes.fromhex(m.group(2)))
            cur = None
            continue
        m = re.match(r"^== (vga\.mode\S+)", line)
        if m:
            cur = m.group(1)
            modes[cur] = {}
            continue
        if cur is None:
            continue
        m = re.match(r"^\s*!?\s*(.+?)\s{2,}6\.22=([0-9A-Fa-f]*)\s*$", line)
        if m:
            name, hexs = m.group(1).strip(), m.group(2)
            for g in GROUPS:
                if name.startswith(g):
                    modes[cur][g] = bytes.fromhex(hexs) if hexs else b""
    return modes


def carr(b, per=16):
    out, i = [], 0
    while i < len(b):
        out.append(", ".join("0x%02X" % x for x in b[i:i + per]))
        i += per
    return (",\n          ").join(out)


# Modes where one oracle is not answering the question, so it does not get a vote.
PCEM_BLIND = {"vga.mode07mono": "PCem's BIOS does not enter MDA-compatible mode 7 "
                                "on this machine -- MiscOut 0x67, a 40-column CRTC"}


def merge(qemu, pcem):
    """-> ({mode: {group: bytes}}, [tie-break descriptions]).

    Agreement is emitted as-is; a disagreement is resolved to PCem and RECORDED.
    A silent tie-break is the thing this file exists to stop."""
    out, notes = {}, []
    for label, qg in qemu.items():
        pg = None if label in PCEM_BLIND else pcem.get(label)
        if label in PCEM_BLIND:
            notes.append("%s: PCem ABSTAINS -- %s" % (label, PCEM_BLIND[label]))
        if not pg:
            out[label] = qg                      # only one oracle ran it
            continue
        merged = {}
        for g, qb in qg.items():
            pb = pg.get(g)
            if not pb or len(pb) != len(qb):
                merged[g] = qb
                continue
            mb = bytearray(qb)
            for i in range(len(qb)):
                if qb[i] != pb[i]:
                    mb[i] = pb[i]
                    notes.append("%s %s[%d]: QEMU 0x%02X, PCem 0x%02X -> PCem"
                                 % (label, g.split()[0], i, qb[i], pb[i]))
            merged[g] = bytes(mb)
        out[label] = merged
    return out, notes


def main():
    for pth in (REF, REF2):
        if not os.path.exists(pth):
            sys.exit("missing %s -- run ./scripts/paritysweep.sh p_vgareg first" % pth)
    modes, ties = merge(parse(REF), parse(REF2))
    # ⚠ modeY and modeX are deliberately NOT here. They are not BIOS modes -- a
    #   program makes them out of 13h -- so a table keyed by an INT 10h mode number
    #   cannot apply them, and pretending otherwise would program them on a plain
    #   mode 13h set. They stay in the reference as documentation of the unchained
    #   path; docs/ref/vga.md 5.2 is where that path is specified.
    want = [("vga.mode03", 0x03), ("vga.mode04", 0x04), ("vga.mode06", 0x06),
            ("vga.mode0D", 0x0D), ("vga.mode0E", 0x0E), ("vga.mode10", 0x10),
            ("vga.mode11", 0x11), ("vga.mode12", 0x12), ("vga.mode13", 0x13),
            ("vga.mode07mono", 0x07),
            # #188 (s84): the CGA-era modes, added to p_vgareg.
            ("vga.mode00", 0x00), ("vga.mode01", 0x01), ("vga.mode02", 0x02),
            ("vga.mode05", 0x05)]
    rows = []
    for label, num in want:
        d = modes.get(label)
        if not d or not d.get("CRTC CR00-CR18"):
            print("skip %s: not in the reference" % label, file=sys.stderr)
            continue
        rows.append((label, num, d))

    w = ["/* GENERATED by tools/gen/gen-vgamodedefs.py. Do not edit: edit the probe,",
         "   re-run it against the oracles, regenerate.",
         "",
         "   What a REAL VGA BIOS leaves in the register file after INT 10h AH=00h --",
         "   measured, not taken from a datasheet. Our own mode set programmed none of",
         "   this, so every one of these registers read back as zero and a guest that",
         "   asks the card about itself was told nothing. See docs/inventory/vga.md.",
         "",
         "   SOURCES: tools/gen/data/vgareg.ref.txt   (MS-DOS 6.22 under QEMU)",
         "            tools/gen/data/vgareg.pcem.txt  (PCem: real AMI 486 + IBM VGA ROM)",
         "   Where they agree the byte is a fact about VGAs. Where they DISAGREE, PCem",
         "   wins -- it is the period-correct machine -- and every such byte is listed",
         "   below so no tie-break is invisible.",
         "",
         "   ⚠ ONLY MEASURED MODES ARE HERE. A mode absent from this table is left",
         "     alone rather than guessed at; extend p_vgareg.asm and regenerate.",
         " */",
         "#ifndef NTVDMEX_VGA_MODEDEFS_H",
         "#define NTVDMEX_VGA_MODEDEFS_H",
         "",
         "typedef struct {",
         "    unsigned char mode;      /* INT 10h mode number                     */",
         "    unsigned char misc;      /* Miscellaneous Output (3C2)              */",
         "    unsigned char seq[5];    /* SR0..SR4                                */",
         "    unsigned char crtc[25];  /* CR00..CR18                              */",
         "    unsigned char gc[9];     /* GR0..GR8                                */",
         "    unsigned char attr[21];  /* AR00..AR14                              */",
         "} vga_modedef;",
         "",
         "static const vga_modedef VGA_MODEDEFS[] = {"]
    if ties:
        w[14:14] = ["", "   ORACLES SPLIT ON %d BYTE(S), RESOLVED TO PCem:" % len(ties)] \
                   + ["     %s" % t for t in ties]
    for label, num, d in rows:
        w.append("    /* %s */" % label)
        w.append("    { 0x%02X, 0x%02X," % (num, d["Misc Output"][0]))
        w.append("      { %s }," % carr(d["SEQ SR0-SR4"]))
        w.append("      { %s }," % carr(d["CRTC CR00-CR18"]))
        w.append("      { %s }," % carr(d["GC GR0-GR8"]))
        w.append("      { %s } }," % carr(d["AC AR00-AR14"]))
    w.append("};")
    w.append("")
    w.append("#define VGA_MODEDEFS_N ((int)(sizeof VGA_MODEDEFS / sizeof VGA_MODEDEFS[0]))")
    w.append("")
    w.append("#endif /* NTVDMEX_VGA_MODEDEFS_H */")
    open(OUT, "w").write("\n".join(w) + "\n")
    print("wrote %s (%d modes)" % (OUT, len(rows)))


if __name__ == "__main__":
    main()
