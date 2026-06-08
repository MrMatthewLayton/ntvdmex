#!/usr/bin/env bash
#
# make-vga12demo.sh -- emit vga12.com, a FAST mode-12h (640x480x16 PLANAR) demo
# using DIRECT A0000 writes (not AH=0C). It sets mode 12h, selects VGA write-mode
# 2 + Map Mask 0x0F, then fills the framebuffer with 16 horizontal colour bands
# via `rep stosb` (one per band). Each A0000 write traps to the host's planar
# write engine -> 4 bit-planes -> 16-colour render. Proves the planar write-trap.
#
# Hand-assembled (org 0x100), 64 bytes:
#   mov ax,0012;int10 | GC: idx5=writemode2 | SEQ: idx2=mapmask 0F |
#   es=A000;di=0;bl=0 | band: al=bl; cx=2400; rep stosb; inc bl; cmp 16; jb |
#   mov ah,0;int16 | mov ax,3;int10 | mov ax,4C00;int21
#
set -euo pipefail
out="$(dirname "$0")/vga12.com"
{
  printf '\xB8\x12\x00\xCD\x10'                               # set mode 12h
  printf '\xBA\xCE\x03\xB0\x05\xEE\xB2\xCF\xB0\x02\xEE'       # GC idx 5 <- 2 (write mode 2)
  printf '\xBA\xC4\x03\xB0\x02\xEE\xB2\xC5\xB0\x0F\xEE'       # SEQ idx 2 <- 0F (map mask)
  printf '\xB8\x00\xA0\x8E\xC0\x31\xFF\x30\xDB'              # es=A000; di=0; bl=0
  printf '\x88\xD8\xB9\x60\x09\xF3\xAA'                       # band: al=bl; cx=2400; rep stosb
  printf '\xFE\xC3\x80\xFB\x10\x72\xF2'                       # inc bl; cmp bl,16; jb band
  printf '\xB4\x00\xCD\x16'                                   # wait key
  printf '\xB8\x03\x00\xCD\x10\xB8\x00\x4C\xCD\x21'           # mode 3; exit
} > "$out"
sz=$(wc -c < "$out")
echo "wrote $out ($sz bytes)"
[ "$sz" -eq 64 ] || { echo "ERROR: expected 64 bytes, got $sz" >&2; exit 1; }
