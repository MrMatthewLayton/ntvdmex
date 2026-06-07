#!/usr/bin/env bash
#
# make-vesademo.sh -- emit vesademo.com, a VESA VBE 2.0 demo. Sets mode 0x101
# (640x480x256 via VBE 4F02), loads a rainbow DAC palette, then fills the whole
# framebuffer through the BANKED A0000 window: for bank 0..4 it sets the window
# (4F05) and writes 64KB (colour = offset>>8). Waits for a key, restores text.
# Exercises VESA: 4F02 mode set + 4F05 banking + hi-res packed-256 render.
#
# Hand-assembled (org 0x100), 96 bytes.
#
set -euo pipefail
out="$(dirname "$0")/vesademo.com"
{
  printf '\xB8\x02\x4F\xBB\x01\x01\xCD\x10'                   # mov ax,4F02; mov bx,0101; int10
  # rainbow DAC palette (i -> i&3F, (i>>1)&3F, 3F-(i&3F))
  printf '\xBA\xC8\x03\xB0\x00\xEE\xB2\xC9\x31\xC9'           # dx=3C8; idx0; dl=C9; cx=0
  printf '\x88\xC8\x24\x3F\xEE'                               # R
  printf '\x88\xC8\xD0\xE8\x24\x3F\xEE'                       # G
  printf '\x88\xC8\x24\x3F\xB4\x3F\x28\xC4\x88\xE0\xEE'       # B
  printf '\x41\x81\xF9\x00\x01\x72\xE2'                       # inc cx; cmp 256; jb palloop
  # fill 5 banks of 64KB via 4F05; each bank a solid colour (= bank*0x30) so the
  # banking shows as 5 clean horizontal colour bands (no bank-boundary shear).
  printf '\x31\xF6'                                           # xor si,si   (bank=0)
  printf '\xB8\x05\x4F\x31\xDB\x89\xF2\xCD\x10'               # bankloop: 4F05 set window=si
  printf '\x89\xF0\xB4\x30\xF6\xE4\x88\xC3'                   # ax=si; ah=0x30; mul ah; bl=al (colour)
  printf '\xB8\x00\xA0\x8E\xC0\x31\xFF\x31\xC9'               # es=A000; di=0; cx=0(=65536)
  printf '\x26\x88\x1D\x47\xE2\xFA'                           # fillb: [es:di]=bl; di++; loop
  printf '\x46\x83\xFE\x05\x72\xDA'                           # inc si; cmp si,5; jb bankloop
  printf '\xB4\x00\xCD\x16'                                   # wait key
  printf '\xB8\x03\x00\xCD\x10\xB8\x00\x4C\xCD\x21'           # mode 3; exit
} > "$out"
sz=$(wc -c < "$out")
echo "wrote $out ($sz bytes)"
[ "$sz" -eq 102 ] || { echo "ERROR: expected 102 bytes, got $sz" >&2; exit 1; }
