#!/usr/bin/env python3
"""cmdcom.py -- re-derive XP's COMMAND.COM interactive path FROM THE BINARY.

Everything this project believes about XP's `C:\\WINDOWS\\System32\\COMMAND.COM` --
where its BOPs are, which bytes gate the prompt, and which INT 21h answer decides
whether it ever reads the keyboard -- came out of this file's own code. This script
re-derives all of it so nobody has to trust a transcription.

    ./tools/ntvdm/cmdcom.py guest/ntvdm/COMMAND.COM

WHY A SCRIPT AND NOT A TABLE IN A DOC. The same session that produced these numbers
also produced two wrong ones by reading a disassembler's output off by two bytes, and
a third by trusting a note instead of the image. Relative jumps are origin-independent;
everything below is computed from the bytes, so an off-by-two cannot survive it.

⚠ THIS IS A READER. It never writes and never needs the rig.

--------------------------------------------------------------------------------
THE TWO ORIGINS

A .COM loads at 0x0100, so for the RESIDENT part `guest = file + 0x100`. COMMAND.COM
then copies its TRANSIENT part to the top of memory (segment 0x9342 on the rig) and
addresses it from 0. That second origin is not a guess: the host's own trace caught a
`BOP 0x54 sub 0x01` at 9342:03CE, and there is exactly one `C4 C4 54 01` in the image,
so `transient = file - (that file offset - 0x3CE)`.
"""
import sys, struct

BOP_SUB01_GUEST = 0x03CE      # measured by the host: `BOP 0x54 sub 0x01` at 9342:03ce
RESIDENT_ORG    = 0x0100

def find_all(d, pat):
    out, off = [], 0
    while True:
        i = d.find(pat, off)
        if i < 0:
            return out
        out.append(i)
        off = i + 1

def transient_org(d):
    """file offset -> transient guest address = file - org."""
    sites = find_all(d, b'\xc4\xc4\x54\x01')
    if len(sites) != 1:
        raise SystemExit("expected exactly one `C4 C4 54 01`, found %d -- the anchor "
                         "this origin rests on is gone; re-derive it before trusting "
                         "anything below." % len(sites))
    return sites[0] - BOP_SUB01_GUEST

def bops(d, org):
    rows = []
    for i in find_all(d, b'\xc4\xc4'):
        if i + 3 >= len(d):
            continue
        rows.append((i, d[i + 2], d[i + 3], i + RESIDENT_ORG, i - org))
    return rows

def statebytes(d, org):
    """Every instruction that READS or WRITES [0x320..0x32F], the state block.

    The encodings that matter, all with a 16-bit displacement:
        80 3E <lo> 03 <imm>   cmp byte [disp],imm
        C6 06 <lo> 03 <imm>   mov byte [disp],imm     <- a WRITE
        A2    <lo> 03         mov [disp],al           <- a WRITE
        83 3E <lo> 03 <imm>   cmp word [disp],imm
    """
    rows = []
    for lo in range(0x20, 0x30):
        disp = bytes([lo, 0x03])
        for i in find_all(d, disp):
            # the displacement is at i; the opcode starts before it
            for back, opc, kind in ((2, b'\x80\x3e', 'cmp byte'),
                                    (2, b'\xc6\x06', 'MOV byte'),
                                    (1, b'\xa2',     'MOV [..],al'),
                                    (2, b'\x83\x3e', 'cmp word')):
                s = i - back
                if s >= 0 and d[s:i] == opc:
                    imm = d[i + 2] if kind.startswith(('cmp byte', 'MOV byte')) else None
                    rows.append((s, 0x300 + lo, kind, imm, s + RESIDENT_ORG, s - org))
                    break
    rows.sort()
    return rows

def image_defaults(d):
    """The state block's STATIC initial values, as linked into the image.

    This is why `[0x32A] == 1` at run time even though nothing ever writes a 1 to it:
    it is the image's own default, and the single writer in the whole binary CLEARS it.
    """
    return d[0x320 - RESIDENT_ORG:0x330 - RESIDENT_ORG]

def rel_target(d, at):
    """Decode a relative jump/call at `at`; returns (mnemonic, file target) or None."""
    op = d[at]
    if op in (0xE8, 0xE9):
        return ('call' if op == 0xE8 else 'jmp',
                at + 3 + struct.unpack_from('<h', d, at + 1)[0])
    if op == 0xEB or 0x70 <= op <= 0x7F:
        return ('jmp' if op == 0xEB else 'j%02X' % op,
                at + 2 + struct.unpack_from('<b', d, at + 1)[0])
    return None

def read_a_line_callers(d, org, routine=0x0A0D):
    """Callers of the read-a-line routine, and the AL each passes.

    ★ THE WHOLE INTERACTIVE QUESTION IS HERE. `AL=0` reads the keyboard, `AL!=0` only
      prints the prompt. Every AL=0 caller sits behind `cmp byte [0x327],1 / jz`, so
      [0x327] == 1 makes the keyboard UNREACHABLE -- and [0x327]'s only writer is
      `mov al,5 / mov ah,53h / int 21h / mov [0x327],al`.
    """
    out = []
    for i in range(len(d) - 3):
        if d[i] != 0xE8:
            continue
        t = rel_target(d, i)
        if not t or t[1] - org != routine:
            continue
        al = None
        # `mov al,imm8 / mov ah,imm8 / call` is the shape at all four sites
        if i >= 4 and d[i - 4] == 0xB0 and d[i - 2] == 0xB4:
            al = d[i - 3]
        out.append((i, i - org, al))
    return out

def tr(f, org):
    """Transient address, or `--` for a site in the RESIDENT part.

    ⚠ Without this a resident site prints a wrapped 16-bit transient address that
      looks exactly like a real one. A plausible wrong number in a table nobody can
      check is the failure this whole file exists to avoid.
    """
    return '0x%04X' % (f - org) if f >= org else '  --  '


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'guest/ntvdm/COMMAND.COM'
    d = open(path, 'rb').read()
    org = transient_org(d)
    print("image     : %s  (%d bytes)" % (path, len(d)))
    print("origins   : resident = file + 0x%04X   transient = file - 0x%04X" %
          (RESIDENT_ORG, org))

    print("\n-- BOP sites --------------------------------------------------------")
    print("  file    bop sub   resident   transient")
    for f, b, s, rg, tg in bops(d, org):
        print("  0x%04X   %02X  %02X    0x%04X     %s" % (f, b, s, rg, tr(f, org)))

    print("\n-- state block, STATIC image defaults --------------------------------")
    dv = image_defaults(d)
    print("  [0x320..0x32F] = %s" % ' '.join('%02X' % b for b in dv))
    print("  ⇒ [0x324]=%02X and [0x32A]=%02X are LINKED-IN DEFAULTS, not written at run time."
          % (dv[4], dv[0x0A]))

    print("\n-- who touches the state block ---------------------------------------")
    print("  file    var      what          imm   resident   transient")
    for f, var, kind, imm, rg, tg in statebytes(d, org):
        print("  0x%04X  [0x%03X]  %-12s  %s   0x%04X     %s" %
              (f, var, kind, ('%02X' % imm) if imm is not None else '--',
               rg, tr(f, org)))

    print("\n-- callers of the read-a-line routine (transient 0x0A0D) --------------")
    for f, tg, al in read_a_line_callers(d, org):
        what = ('READS THE KEYBOARD' if al == 0 else 'prompt only') if al is not None else '?'
        print("  file 0x%04X  transient %s  AL=%s  %s" %
              (f, tr(f, org), ('%02X' % al) if al is not None else '??', what))

    print("""
-- the conclusion these three tables force ----------------------------------
  * the only writer of [0x327] is `mov al,5 / mov ah,53h / int 21h / mov [0x327],al`
  * every AL=0 (keyboard-reading) caller is behind `cmp byte [0x327],1 / jz away`
  ⇒ INT 21h AX=5305h MUST return AL=0 or this shell can never read a key.
  * the loop's top is `AX=5302h`; CF=1 goes to the gate chain, CF=0 goes to
    `cmp [0x32A],1` -- and [0x32A] is 1 by default and cleared ONLY on the line-read
    path, so CF=0 asks the host for a command for ever.
  ⇒ INT 21h AX=5302h MUST return CF=1 once there is no host command left.
  See docs/inventory/bop.md.""")

if __name__ == '__main__':
    main()
