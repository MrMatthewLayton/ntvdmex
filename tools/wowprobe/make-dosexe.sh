#!/usr/bin/env bash
#
# Emit dosstub.exe -- a minimal *16-bit MZ* DOS executable that exits cleanly:
#   B8 00 4C     mov ax, 4C00h   ; AH=4Ch (terminate), AL=00 (exit code 0)
#   CD 21        int 21h
#
# Unlike a headerless .COM, this has a real MZ header, so NT/XP unambiguously
# classifies it as a DOS binary and routes it to the VDM (Control\WOW\cmdline).
# Used as the Spike-002 trigger when the .COM form is rejected as "not a valid
# Win32 application". A 64-byte header (e_lfanew = 0 -> no PE/NE) + 5 code bytes.
#
set -euo pipefail
out="$(dirname "$0")/dosstub.exe"

printf '\x4D\x5A\x45\x00\x01\x00\x00\x00\x04\x00\x00\x00\xFF\xFF\x00\x00' >  "$out"  # 0x00: MZ, cblp=69, cp=1, cparhdr=4, maxalloc=FFFF
printf '\x00\x01\x00\x00\x00\x00\x00\x00\x40\x00\x00\x00\x00\x00\x00\x00' >> "$out"  # 0x10: sp=0x0100, lfarlc=0x40
printf '\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00' >> "$out"  # 0x20: reserved
printf '\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00' >> "$out"  # 0x30: e_lfanew=0 at 0x3C
printf '\xB8\x00\x4C\xCD\x21'                                             >> "$out"  # 0x40: code

echo "wrote $out ($(wc -c < "$out") bytes)"
