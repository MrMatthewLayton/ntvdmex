#!/usr/bin/env bash
#
# make-vga12demo.sh -- emit vga12.com, a mode-12h (640x480x16 PLANAR) demo. Sets
# mode 12h, then plots colour = (x XOR y) & 0x0F for every pixel via INT 10h AH=0C
# (the BIOS write-pixel, which our video VDD turns into a 4-bit-plane write). The
# host's UI thread renders the 4 planes progressively, so the XOR fractal fills in
# live. Waits for a key, restores text. Proves planar storage + plane-combine render.
#
# Hand-assembled (org 0x100), 51 bytes:
#   mov ax,0012;int10 | for y for x: cx=x;dx=y;ax=x^y;al&=0F;ah=0C;int10 |
#   mov ah,0;int16 | mov ax,3;int10 | mov ax,4C00;int21
#
set -euo pipefail
out="$(dirname "$0")/vga12.com"
{
  printf '\xB8\x12\x00\xCD\x10'                               # set mode 12h
  printf '\x31\xF6'                                           # xor si,si  (y=0)
  printf '\x31\xFF'                                           # yloop: xor di,di (x=0)
  printf '\x89\xF9\x89\xF2\x89\xF8\x31\xF0\x24\x0F\xB4\x0C\xCD\x10'  # xloop: cx=x;dx=y;ax=x^y;al&=0F;AH=0C;int10
  printf '\x47\x81\xFF\x80\x02\x72\xEB'                       # inc di; cmp di,640; jb xloop
  printf '\x46\x81\xFE\xE0\x01\x72\xE2'                       # inc si; cmp si,480; jb yloop
  printf '\xB4\x00\xCD\x16'                                   # wait key
  printf '\xB8\x03\x00\xCD\x10\xB8\x00\x4C\xCD\x21'           # mode 3; exit
} > "$out"
sz=$(wc -c < "$out")
echo "wrote $out ($sz bytes)"
[ "$sz" -eq 51 ] || { echo "ERROR: expected 51 bytes, got $sz" >&2; exit 1; }
