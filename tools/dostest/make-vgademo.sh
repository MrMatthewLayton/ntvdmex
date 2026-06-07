#!/usr/bin/env bash
#
# make-vgademo.sh -- emit vgademo.com, a mode-13h graphics demo. Sets mode 13h,
# loads a rainbow 256-colour palette via the DAC (ports 3C8/3C9), fills the A0000
# linear framebuffer directly (color = pixel index & 0xFF -> diagonal colour
# sweep), waits for a key (INT 16h), restores text mode 3, and exits. Exercises
# the M3 graphics path end-to-end: mode set + DAC + direct A0000 writes -> render.
#
# Hand-assembled (org 0x100):
#   mov ax,0013;int10 | DAC: out 3C8,0; loop 256x out 3C9 R=i&3F,(i>>1)&3F,3F-(i&3F)
#   es=A000; di=0; for y=0..199 for x=0..319: [di++] = (x xor y)   <- 2D XOR fractal
#   mov ah,0;int16 (wait key) | mov ax,3;int10 (text) | mov ax,4C00;int21
#
set -euo pipefail
out="$(dirname "$0")/vgademo.com"
# --- mode 13h + rainbow DAC palette (45 bytes) ---
printf '\xB8\x13\x00\xCD\x10' > "$out"                                  # set mode 13h
printf '\xBA\xC8\x03\xB0\x00\xEE\xB2\xC9\x31\xC9'           >> "$out"    # DAC idx 0; dx=3C9; cx=0
printf '\x88\xC8\x24\x3F\xEE'                               >> "$out"    # R = i&3F
printf '\x88\xC8\xD0\xE8\x24\x3F\xEE'                       >> "$out"    # G = (i>>1)&3F
printf '\x88\xC8\x24\x3F\xB4\x3F\x28\xC4\x88\xE0\xEE'       >> "$out"    # B = 3F-(i&3F)
printf '\x41\x81\xF9\x00\x01\x72\xE2'                       >> "$out"    # inc cx; cmp 256; jb palloop
# --- 2D XOR fill: for y, for x: [di++] = (x ^ y) (47 bytes) ---
printf '\xB8\x00\xA0\x8E\xC0\x31\xFF\x31\xF6'              >> "$out"    # es=A000; di=0; si(y)=0
printf '\x31\xC9'                                          >> "$out"    # yloop: cx(x)=0
printf '\x89\xC8\x31\xF0\x26\x88\x05\x47\x41'             >> "$out"    # xloop: ax=x; ax^=y; [es:di]=al; di++; x++
printf '\x81\xF9\x40\x01\x72\xF1'                         >> "$out"    # cmp x,320; jb xloop
printf '\x46\x81\xFE\xC8\x00\x72\xE8'                     >> "$out"    # y++; cmp y,200; jb yloop
printf '\xB4\x00\xCD\x16'                                  >> "$out"    # wait for a key
printf '\xB8\x03\x00\xCD\x10\xB8\x00\x4C\xCD\x21'          >> "$out"    # mode 3; exit
sz=$(wc -c < "$out")
echo "wrote $out ($sz bytes)"
[ "$sz" -eq 92 ] || { echo "ERROR: expected 92 bytes, got $sz" >&2; exit 1; }
