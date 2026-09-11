#!/usr/bin/env python3
"""guestdis.py -- disassemble a guest at the CS:IP an NTVDMEX instrument named.

── THE PROBLEM THIS SOLVES ─────────────────────────────────────────────────────
Our instruments report guest addresses as CS:IP under NTVDMEX: the IO-SITE lines,
the write/read/colour-compare site histograms, the VRAM watchpoint. To read the
code at one of those addresses you need the guest's memory, and for a packed
program (VGALEMMI.EXE is PKLITE; most DOS games of the era are packed somehow)
the file on disk is a decompressor, not the program. The honest way to read the
code is to let the program decompress ITSELF and dump memory -- but a dump taken
under QEMU/MS-DOS has a DIFFERENT load segment from the one NTVDMEX reported, so
the CS:IP from the instrument does not index the dump.

── HOW IT IS SOLVED, AND WHY THIS WAY ──────────────────────────────────────────
By ANCHORING on bytes the instrument already measured, never by assuming a load
address. Every site report from the host carries the bytes around the site (the
IO-SITE lines print `bytes[ip-6..]`). Find that byte string in the dump and you
have one linear address whose CS:IP you know, and therefore the segment base --
measured, not inferred. Everything else follows by arithmetic.

This is the same discipline as putting a real clock on the heartbeat line: an
instrument must not infer its own frame of reference.

── USE ─────────────────────────────────────────────────────────────────────────
    # 1. dump the guest's memory while it runs under genuine MS-DOS
    scripts/lemref.py --game games/Lemmings --out /tmp/ref.ppm --memdump /tmp/g.mem

    # 2. read the code at the sites the rig named, anchoring on measured bytes
    scripts/guestdis.py --dump /tmp/g.mem \
        --anchor 80c206eca80874fbeca80875fbc3 --anchor-ip 0x1552 \
        --site 0x95e0 --site 0x98d1 --before 0x30 --len 0x60

`--anchor-ip` is the IP the instrument reported for the FIRST byte of --anchor.
For an IO-SITE line printing `bytes[ip-6..]`, that is the reported ip minus 6.

Works on any guest and any instrument that reports CS:IP with bytes. Nothing here
is Lemmings-specific.
"""
import argparse
import os
import subprocess
import sys


def find_all(hay: bytes, nee: bytes):
    out, i = [], 0
    while True:
        i = hay.find(nee, i)
        if i < 0:
            return out
        out.append(i)
        i += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", required=True, help="raw memory image (pmemsave 0 0x100000)")
    ap.add_argument("--anchor", required=True,
                    help="hex bytes the instrument printed at a known IP")
    ap.add_argument("--anchor-ip", required=True,
                    help="the IP of the anchor's FIRST byte, e.g. 0x1552")
    ap.add_argument("--site", action="append", default=[],
                    help="an IP to disassemble; repeatable. Accepts 0x1109_5e0-style "
                         "packed cs:ip from the site histograms, or a bare IP.")
    ap.add_argument("--before", default="0x20", help="bytes of context before each site")
    ap.add_argument("--len", default="0x60", help="bytes to disassemble per site")
    ap.add_argument("--bits", default="16", choices=["16", "32"])
    a = ap.parse_args()

    dump = open(a.dump, "rb").read()
    anchor = bytes.fromhex(a.anchor.replace(" ", ""))
    aip = int(a.anchor_ip, 0)

    hits = find_all(dump, anchor)
    if not hits:
        sys.exit("anchor not found in the dump -- is this the right guest, and did it "
                 "reach the code the instrument saw? (a site only exists once it has run)")
    if len(hits) > 1:
        print("⚠ anchor is AMBIGUOUS (%d matches) -- lengthen it, or the base below is a "
              "guess: %s" % (len(hits), [hex(h) for h in hits]), file=sys.stderr)

    base = hits[0] - aip
    if base < 0:
        sys.exit("anchor found at 0x%X but that is before IP 0x%X -- wrong --anchor-ip?"
                 % (hits[0], aip))
    print("anchor at linear 0x%05X, IP 0x%04X  ->  segment base linear 0x%05X"
          % (hits[0], aip, base)
          + ("  (paragraph-aligned, seg 0x%04X)" % (base >> 4) if base % 16 == 0
             else "  ⚠ NOT paragraph-aligned -- suspect the anchor"))
    print()

    before, length = int(a.before, 0), int(a.len, 0)
    for s in a.site:
        ip = int(s, 0)
        if ip > 0xFFFF:              # packed cs:ip from the site histograms
            print("# site 0x%08X -> cs 0x%04X ip 0x%04X (segment ignored; the anchor "
                  "fixes the base)" % (ip, ip >> 16, ip & 0xFFFF))
            ip &= 0xFFFF
        start = max(0, ip - before)
        blob = dump[base + start: base + start + length]
        if not blob:
            print("# site 0x%04X is outside the dump" % ip)
            continue
        p = subprocess.run(["ndisasm", "-b", a.bits, "-o", hex(start), "-"],
                           input=blob, capture_output=True)
        print("=== site IP 0x%04X (linear 0x%05X) ===" % (ip, base + ip))
        for line in p.stdout.decode(errors="replace").splitlines():
            mark = "  <<<< the instrument named this" if line.startswith("%08X" % ip) \
                   or line.lower().startswith(("%08x" % ip).lower()) else ""
            print(line + mark)
        print()


if __name__ == "__main__":
    main()
