#!/usr/bin/env bash
#
# make-vesademo.sh -- emit vesademo.com, a VESA VBE 2.0 demo. Sets mode 0x101
# (640x480x256 via 4F02), loads a rainbow DAC palette, then fills the whole
# framebuffer through the BANKED A0000 window with a SMOOTH gradient whose colour
# is a function of the TRUE linear address -- colour = (bank<<5) | (offset>>11) --
# so it is continuous across the 64KB bank boundaries (no shear). Exercises VESA
# 4F02 mode set + 4F05 banking + hi-res packed-256 render.
#
# Hand-assembled (org 0x100), 112 bytes.
#
set -euo pipefail
out="$(dirname "$0")/vesademo.com"
{
  # --- mode 0x101 + rainbow DAC palette (48 bytes) ---
  printf '\xB8\x02\x4F\xBB\x01\x01\xCD\x10'                   # set mode 0x101
  printf '\xBA\xC8\x03\xB0\x00\xEE\xB2\xC9\x31\xC9'           # DAC idx 0; dl=C9; cx=0
  printf '\x88\xC8\x24\x3F\xEE'                               # R = i&3F
  printf '\x88\xC8\xD0\xE8\x24\x3F\xEE'                       # G = (i>>1)&3F
  printf '\x88\xC8\x24\x3F\xB4\x3F\x28\xC4\x88\xE0\xEE'       # B = 3F-(i&3F)
  printf '\x41\x81\xF9\x00\x01\x72\xE2'                       # inc cx; cmp 256; jb palloop
  # --- smooth gradient fill over 5 banks (64 bytes) ---
  printf '\x31\xF6'                                           # xor si,si  (bank=0)
  printf '\xB8\x05\x4F\x31\xDB\x89\xF2\xCD\x10'               # bankloop: 4F05 set window=si
  printf '\xB8\x00\xA0\x8E\xC0'                               # es=A000
  printf '\x89\xF3\xB1\x05\xD2\xE3'                           # bx=si; cl=5; bl = si<<5
  printf '\x31\xFF\x31\xC9'                                   # di=0; cx=0 (65536 inner)
  printf '\x89\xF8\x88\xE0\xD0\xE8\xD0\xE8\xD0\xE8'           # fillb: al = di>>11
  printf '\x08\xD8\x26\x88\x05\x47\xE2\xEE'                   # or al,bl; [es:di]=al; di++; loop
  printf '\x46\x83\xFE\x05\x72\xD0'                           # inc si; cmp si,5; jb bankloop
  printf '\xB4\x00\xCD\x16'                                   # wait key
  printf '\xB8\x03\x00\xCD\x10\xB8\x00\x4C\xCD\x21'           # mode 3; exit
} > "$out"
sz=$(wc -c < "$out")
echo "wrote $out ($sz bytes)"
[ "$sz" -eq 112 ] || { echo "ERROR: expected 112 bytes, got $sz" >&2; exit 1; }
