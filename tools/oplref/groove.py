#!/usr/bin/env python3
"""groove.py -- write a SYNTHETIC rhythm-mode register trace for oplcmp.  DEV TOOL.

No game trace is kept in the tree (they are captured on the rig into build/oplref/,
see docs/log/sessions/standing-reference.md), so this is the stand-in regression for
the rhythm voices (#139): eight bars of an AdLib-style groove -- sixteenth-note
hi-hats, snare on 2 and 4, bass drum on 1 and 3, a cymbal each bar, tom fills --
over a two-voice melodic line, keyed the way AdLib drivers key drums (the 0xBD bit
cleared, then set). The patches are plausible percussive ones, not any game's.

    ./tools/oplref/groove.py > build/oplref/groove.txt
    ./build/oplref/oplcmp build/oplref/groove.txt build/oplref
Format: `us reg val`, hex, as the host's opltrace.flag capture writes it.
"""
import sys

ev = []
def w(t_us, reg, val):
    ev.append((int(t_us), reg & 0xFF, val & 0xFF))

OPOFF = [0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D,
         0x10, 0x11, 0x12, 0x13, 0x14, 0x15]
def chop(c, which):                      # register offset of channel c's operator
    return OPOFF[[0, 1, 2, 6, 7, 8, 12, 13, 14][c] + 3 * which]

def patch(t, off, am_vib_egt_ksr_mult, ksl_tl, ar_dr, sl_rr, ws):
    w(t, 0x20 + off, am_vib_egt_ksr_mult); w(t, 0x40 + off, ksl_tl)
    w(t, 0x60 + off, ar_dr); w(t, 0x80 + off, sl_rr); w(t, 0xE0 + off, ws)

t = 1000
w(t, 0x01, 0x20)
# melodic: channels 0 and 1, a soft FM bass and a pad
for c, (m, cr, fb) in enumerate([((0x21, 0x1A, 0xF3, 0x56, 0), (0x21, 0x00, 0xF4, 0x57, 0), 0x0C),
                                 ((0x61, 0x22, 0x85, 0x24, 1), (0x21, 0x04, 0x74, 0x25, 0), 0x0A)]):
    patch(t, chop(c, 0), *m); patch(t, chop(c, 1), *cr); w(t, 0xC0 + c, fb)
# percussion patches: op12/15 bass drum, op13 hi-hat, op14 tom, op16 snare, op17 cymbal
patch(t, 0x10, 0x01, 0x0B, 0xF8, 0x47, 0)   # op12 bass-drum modulator
patch(t, 0x13, 0x01, 0x00, 0xF6, 0x67, 0)   # op15 bass-drum carrier
w(t, 0xC6, 0x08)
patch(t, 0x11, 0x01, 0x03, 0xF9, 0xF7, 0)   # op13 hi-hat
patch(t, 0x12, 0x05, 0x04, 0xF7, 0x57, 0)   # op14 tom
patch(t, 0x14, 0x01, 0x02, 0xF8, 0x87, 0)   # op16 snare
patch(t, 0x15, 0x01, 0x06, 0xF5, 0x46, 0)   # op17 cymbal
# drum pitches (channels 6-8: F-number/block, NO key bit -- keyed from 0xBD)
w(t, 0xA6, 0x57); w(t, 0xB6, 0x09)
w(t, 0xA7, 0x03); w(t, 0xB7, 0x0A)
w(t, 0xA8, 0x57); w(t, 0xB8, 0x09)
bd = 0x20                                  # rhythm mode on
w(t, 0xBD, bd)

FN = [0x157, 0x16B, 0x181, 0x198, 0x1B0, 0x1CA, 0x1E5, 0x202, 0x220, 0x241, 0x263, 0x287]
def note(c, semi, octave, t_on, dur_us):
    f = FN[semi % 12]
    w(t_on, 0xA0 + c, f & 0xFF); w(t_on, 0xB0 + c, 0x20 | (octave << 2) | (f >> 8))
    w(t_on + dur_us, 0xB0 + c, (octave << 2) | (f >> 8))

step = 125000                              # a sixteenth at 120 BPM, in us
t0 = 20000
bassline = [0, 0, 7, 5, 3, 3, 10, 8]
for bar in range(8):
    for s16 in range(16):
        ts = t0 + (bar * 16 + s16) * step
        hits = 0x01                        # hi-hat every sixteenth
        if s16 in (0, 8):   hits |= 0x10   # bass drum
        if s16 in (4, 12):  hits |= 0x08   # snare
        if s16 == 0:        hits |= 0x02   # cymbal
        if bar % 4 == 3 and s16 in (13, 14, 15): hits |= 0x04   # tom fill
        w(ts, 0xBD, bd)                    # release the bits, then strike
        w(ts + 30, 0xBD, bd | hits)
        if s16 % 4 == 0:
            note(0, bassline[bar], 2, ts + 60, 3 * step)
        if s16 == 0:
            note(1, bassline[bar] + 7, 4, ts + 90, 14 * step)
w(t0 + 128 * step + 400000, 0xBD, 0x00)

ev.sort(key=lambda e: e[0])
out = sys.stdout
out.write("# synthetic rhythm-mode groove (tools/oplref/groove.py) -- not a game capture\n")
for t_us, reg, val in ev:
    out.write("%x %x %x\n" % (t_us, reg, val))
