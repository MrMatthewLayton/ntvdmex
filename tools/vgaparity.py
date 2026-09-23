#!/usr/bin/env python3
"""vgaparity.py -- how much of the VGA register file do we get right, per mode?

    ./tools/vgaparity.py                      # compare the last rig run
    ./tools/vgaparity.py --ours <file>        # ...or a saved probe output

WHY A SCRIPT AND NOT A NUMBER IN A DOCUMENT. "a BIOS mode set leaves ~60 values on
real hardware and ~5 here" was true when it was written and is the reason db4c059
exists; it stopped being true the moment that commit landed, and a reader had no way
to tell. Same rule as the other scores in this project: RE-RUN, NEVER QUOTE.

INPUT. The reference is tools/dostest/vgareg.ref.txt, which holds two forms -- an
early per-group block with `6.22=` lines, and the compact whole-buffer form later
captures were recorded in. Both are read. Ours is the raw `BUF=vga.*` lines from a
p_vgareg run under NTVDMEX; by default they are taken from the rig's own
result_Probe.log, which is where scripts/dosdiff.py leaves them.

⚠ THE DAC PIXEL MASK IS COUNTED BUT FLAGGED. The oracle says 0x00 and we say 0xFF,
  which is the documented reset value; that disagreement is OPEN and needs PCem, so
  it is reported separately rather than quietly counted as a defect or quietly
  excluded as a known-good.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = os.path.join(ROOT, "tools", "dostest", "vgareg.ref.txt")
RIG = "/private/tmp/xpshare/debug/out/result_Probe.log"

GROUPS = [("MiscOut", 0, 1), ("FeatCtl", 1, 2), ("InpStat0", 2, 3), ("DACmask", 3, 4),
          ("SR0-4", 4, 9), ("CR00-18", 9, 34), ("GR0-8", 34, 43), ("AR00-14", 43, 64)]
ORDER = ["Misc Output", "SEQ SR0-SR4", "CRTC CR00-CR18", "GC GR0-GR8", "AC AR00-AR14"]


def load_reference(path):
    """Both recorded forms -> {case: 64 bytes}."""
    out, cur, parts = {}, None, {}

    def flush():
        if cur and len(parts) == len(ORDER):
            buf = bytearray(64)
            buf[0:1] = parts["Misc Output"][:1]
            buf[4:9] = parts["SEQ SR0-SR4"]
            buf[9:34] = parts["CRTC CR00-CR18"]
            buf[34:43] = parts["GC GR0-GR8"]
            buf[43:64] = parts["AC AR00-AR14"]
            out.setdefault(cur, bytes(buf))

    for line in open(path):
        m = re.match(r"^== (vga\.\S+)\s+([0-9A-Fa-f]{128})\s*$", line.strip())
        if m:                                   # compact: the whole buffer
            out[m.group(1)] = bytes.fromhex(m.group(2))
            continue
        m = re.match(r"^== (vga\.\S+)", line)
        if m:
            flush()
            cur, parts = m.group(1), {}
            continue
        m = re.match(r"^\s*!?\s*(.+?)\s{2,}6\.22=([0-9A-Fa-f]*)\s*$", line)
        if m and cur:
            name, hexs = m.group(1).strip(), m.group(2)
            for g in ORDER:
                if name.startswith(g) and hexs:
                    parts[g] = bytes.fromhex(hexs)
    flush()
    return out


def load_ours(path):
    out = {}
    with open(path, "rb") as fh:
        text = fh.read().decode("latin-1")
    for m in re.finditer(r"BUF=(vga\.\S+)\s+([0-9A-Fa-f]{128})", text):
        out[m.group(1)] = bytes.fromhex(m.group(2))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ours", default=RIG, help="a p_vgareg run under NTVDMEX")
    ap.add_argument("--ref", default=REF)
    a = ap.parse_args()
    for p in (a.ref, a.ours):
        if not os.path.exists(p):
            sys.exit("missing: %s" % p)

    ref, ours = load_reference(a.ref), load_ours(a.ours)
    common = sorted(set(ref) & set(ours))
    if not common:
        sys.exit("no cases in common -- is %s a p_vgareg run?" % a.ours)

    print("  VGA REGISTER PARITY -- NTVDMEX vs the MS-DOS 6.22 oracle")
    print("  %-22s %5s  %s" % ("mode", "bad", "groups still differing"))
    total = bad = dac = 0
    for case in common:
        r, o = ref[case], ours[case]
        diff = [i for i in range(64) if r[i] != o[i]]
        dac += 1 if 3 in diff else 0
        total += 64
        bad += len(diff)
        groups = sorted({g for g, lo, hi in GROUPS for i in diff if lo <= i < hi})
        print("  %-22s %5d  %s" % (case, len(diff), ", ".join(groups) or "— identical —"))
    print("\n  %d modes, %d of %d bytes agree -> PARITY %.1f%%"
          % (len(common), total - bad, total, 100.0 * (total - bad) / total))
    if dac:
        print("  ⚠ %d of those modes include the DAC pixel mask byte, which is an OPEN\n"
              "    disagreement (oracle 0x00, ours 0xFF = the documented reset) and needs\n"
              "    PCem to settle. Excluding it: PARITY %.1f%%"
              % (dac, 100.0 * (total - bad + dac) / total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
