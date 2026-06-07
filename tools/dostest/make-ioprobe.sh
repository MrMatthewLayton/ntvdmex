#!/usr/bin/env bash
#
# make-ioprobe.sh -- emit ioprobe.com, the M3 slice-1b I/O-trap probe. It programs
# the 8254 PIT channel 0 and reads the count back over IN/OUT, then exits with
# errorlevel = the byte read. Under our host the IN/OUT instructions #GP-fault
# (IOPL=0, event 2) and must dispatch through the VDD bus to the PIT VDD; if the
# round-trip works the program exits with errorlevel 0x34 (the low byte of the
# 0x1234 reload it programmed). The host log shows each "IO out/in" line.
#
# Hand-assembled (org 0x100):
#   100: B0 36         mov al, 36h          ; ch0, lo/hi, mode 3
#   102: E6 43         out 43h, al          ; PIT command
#   104: B0 34         mov al, 34h
#   106: E6 40         out 40h, al          ; reload lo
#   108: B0 12         mov al, 12h
#   10A: E6 40         out 40h, al          ; reload hi  -> 0x1234
#   10C: B0 00         mov al, 00h
#   10E: E6 43         out 43h, al          ; latch ch0 count
#   110: E4 40         in  al, 40h          ; lo byte of latched count -> AL (0x34)
#   112: B4 4C         mov ah, 4Ch
#   114: CD 21         int 21h              ; exit, errorlevel = AL   (22 bytes)
#
set -euo pipefail
out="$(dirname "$0")/ioprobe.com"
printf '\xB0\x36\xE6\x43\xB0\x34\xE6\x40\xB0\x12\xE6\x40\xB0\x00\xE6\x43\xE4\x40\xB4\x4C\xCD\x21' > "$out"
sz=$(wc -c < "$out")
echo "wrote $out ($sz bytes)"
[ "$sz" -eq 22 ] || { echo "ERROR: expected 22 bytes, got $sz" >&2; exit 1; }
