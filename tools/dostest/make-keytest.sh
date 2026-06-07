#!/usr/bin/env bash
#
# make-keytest.sh -- emit keytest.com, a DOS .COM that reads keys with echo via
# INT 21h AH=01 and exits when ESC is pressed. The M3 keyboard gate: typed keys
# must travel UI WM_CHAR -> input VDD ring -> INT 21h conin -> echoed to the video
# VDD -> painted in the Luna window.
#
# Hand-assembled (org 0x100):
#   100: B4 01      loop: mov ah, 01h        ; read char, with echo
#   102: CD 21            int 21h
#   104: 3C 1B            cmp al, 1Bh        ; ESC?
#   106: 75 F8            jne loop           ; (0x108 - 8 = 0x100)
#   108: B8 00 4C         mov ax, 4C00h
#   10B: CD 21            int 21h            ; exit 0   (13 bytes)
#
set -euo pipefail
out="$(dirname "$0")/keytest.com"
printf '\xB4\x01\xCD\x21\x3C\x1B\x75\xF8\xB8\x00\x4C\xCD\x21' > "$out"
sz=$(wc -c < "$out")
echo "wrote $out ($sz bytes)"
[ "$sz" -eq 13 ] || { echo "ERROR: expected 13 bytes, got $sz" >&2; exit 1; }
