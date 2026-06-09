#!/usr/bin/env bash
#
# make-demos.sh -- assemble the animated graphics demos with nasm.
#   demo13.asm  -> vgademo.com   (mode 13h, scrolling rainbow)
#   demo12.asm  -> vga12.com     (mode 12h planar, scrolling colour bands)
#   demovesa.asm-> vesademo.com  (VESA 0x101, scrolling smooth gradient)
# Each animates and exits on a keypress. Replaces the older hand-assembled .coms.
#
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
nasm -f bin "$DIR/demo13.asm"   -o "$DIR/vgademo.com"
nasm -f bin "$DIR/demo12.asm"   -o "$DIR/vga12.com"
nasm -f bin "$DIR/demovesa.asm" -o "$DIR/vesademo.com"
nasm -f bin "$DIR/blitfast.asm" -o "$DIR/blitfast.com"   # 12h rects, efficient REP idiom (vs QB BLIT)
for f in vgademo.com vga12.com vesademo.com blitfast.com; do
    echo "  $f  ($(wc -c < "$DIR/$f") bytes)"
done
