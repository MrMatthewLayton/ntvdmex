#!/usr/bin/env python3
"""vgaparity.py -- how much of the VGA register file do we get right, per mode?

    ./tools/vgaparity.py                      # compare the last rig run
    ./tools/vgaparity.py --ours <file>        # ...or a saved probe output

WHY A SCRIPT AND NOT A NUMBER IN A DOCUMENT. "a BIOS mode set leaves ~60 values on
real hardware and ~5 here" was true when it was written and is the reason db4c059
exists; it stopped being true the moment that commit landed, and a reader had no way
to tell. Same rule as the other scores in this project: RE-RUN, NEVER QUOTE.

⛔⛔⛔ AND THE SCORE USED TO BE AGAINST ONE ORACLE, WHICH IS THE MISTAKE THIS
PROJECT KEEPS MAKING. The reference was 6.22 under QEMU, whose video BIOS is the
Bochs VGABIOS -- not period-correct firmware. PCem, with a real AMI 486 BIOS and a
genuine IBM VGA ROM, disagrees with it on scores of bytes: a disagreement of the
same ORDER as the error the score was reporting. So "89.7% parity" could not tell
you whether a byte we failed was us being wrong or QEMU being anachronistic.

    ⇒ SCORE ONLY WHAT THE TWO ORACLES AGREE ON. A byte both hosts answer the same
      way is a fact about VGAs; a byte they answer differently is a fact about
      emulators, and counting it either way manufactures a number. Disputed bytes
      are reported separately, with how we answer each -- which is the interesting
      column, because matching PCem where QEMU differs is a WIN the old score
      recorded as a loss.

INPUT. Two references: tools/dostest/vgareg.ref.txt (6.22 under QEMU) and
tools/dostest/vgareg.pcem.txt (PCem, real BIOS). Ours is the raw `BUF=vga.*` lines
from a p_vgareg run under NTVDMEX; by default they are taken from the rig's own
result_Probe.log, which is where scripts/dosdiff.py leaves them.

⚠ THE OLD DAC-PIXEL-MASK CARVE-OUT IS GONE. It was a hand-maintained exception for
  exactly this situation -- one oracle said 0x00, we said 0xFF, and it needed PCem.
  PCem says 0xFF. The byte is now disputed by the general rule and the special case
  is not needed; a rule beats a list of exceptions that someone has to remember to
  prune.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = os.path.join(ROOT, "tools", "dostest", "vgareg.ref.txt")
REF2 = os.path.join(ROOT, "tools", "dostest", "vgareg.pcem.txt")
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
    ap.add_argument("--ref", default=REF, help="6.22 under QEMU")
    ap.add_argument("--ref2", default=REF2, help="PCem, real AMI BIOS + IBM VGA ROM")
    a = ap.parse_args()
    for p in (a.ref, a.ref2, a.ours):
        if not os.path.exists(p):
            sys.exit("missing: %s" % p)

    qemu, pcem, ours = (load_reference(a.ref), load_reference(a.ref2),
                        load_ours(a.ours))
    common = sorted(set(qemu) & set(pcem) & set(ours))
    if not common:
        sys.exit("no cases in common -- is %s a p_vgareg run?" % a.ours)

    print("  VGA REGISTER PARITY -- NTVDMEX vs the bytes TWO oracles agree on")
    print("  (6.22/QEMU and PCem's real AMI BIOS + IBM VGA ROM)")
    print("\n  %-22s %5s %5s  %s" % ("mode", "bad", "disp", "groups still differing"))
    scored = bad = disputed = 0
    win = lose = 0                      # on DISPUTED bytes: whom do we match?
    per_group = {}
    for case in common:
        q, p2, o = qemu[case], pcem[case], ours[case]
        agree = [i for i in range(64) if q[i] == p2[i]]
        split = [i for i in range(64) if q[i] != p2[i]]
        diff = [i for i in agree if q[i] != o[i]]
        for i in split:
            if o[i] == p2[i]:  win += 1
            elif o[i] == q[i]: lose += 1
        scored += len(agree)
        bad += len(diff)
        disputed += len(split)
        for g, lo, hi in GROUPS:
            n = len([i for i in diff if lo <= i < hi])
            if n:
                per_group[g] = per_group.get(g, 0) + n
        groups = sorted({g for g, lo, hi in GROUPS for i in diff if lo <= i < hi})
        print("  %-22s %5d %5d  %s"
              % (case, len(diff), len(split), ", ".join(groups) or "- identical -"))

    print("\n  %d modes. Of the %d bytes BOTH oracles agree on, %d match"
          " -> PARITY %.1f%%"
          % (len(common), scored, scored - bad, 100.0 * (scored - bad) / scored))
    if per_group:
        print("  still differing, by group: %s"
              % ", ".join("%s x%d" % (g, n) for g, n in sorted(per_group.items())))

    # ── THE DISPUTED BYTES ARE NOT A FOOTNOTE. They are where "we are wrong" and
    #    "the modern emulator is wrong" used to be indistinguishable, and the old
    #    single-oracle score silently resolved every one of them against us.
    print("\n  %d bytes DISPUTED between the oracles -- excluded from the score."
          % disputed)
    print("    of those, we match PCem (period-correct) on %d and QEMU on %d;"
          " %d match neither." % (win, lose, disputed - win - lose))
    return 0


if __name__ == "__main__":
    sys.exit(main())
