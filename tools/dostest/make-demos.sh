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
nasm -f bin "$DIR/timertst.asm" -o "$DIR/timertst.com"   # PIT timer-IRQ test (INT 1Ch dots)
nasm -f bin "$DIR/mousetst.asm" -o "$DIR/mousetst.com"   # INT 33h mouse test (draw + cursor)
nasm -f bin "$DIR/xmstest.asm"  -o "$DIR/xmstest.com"    # M4 XMS end-to-end test (2Fh + Move)
nasm -f bin "$DIR/emstest.asm"  -o "$DIR/emstest.com"    # M4 EMS end-to-end test (67h + shadowing)
nasm -f bin "$DIR/selftest.asm" -o "$DIR/selftest.com"  # one-shot regression suite (all subsystems)
nasm -f bin "$DIR/modeswitch.asm" -o "$DIR/modeswitch.com" # GH#14: graphics->text mode-switch text visibility
nasm -f bin "$DIR/dpmitest.asm" -o "$DIR/dpmitest.com"     # GH#1: DPMI 16-bit real->PM switch spike
nasm -f bin "$DIR/dpmiexe.asm"  -o "$DIR/dpmiexe.exe"      # GH#2: multi-segment MZ .EXE DPMI client
for f in vgademo.com vga12.com vesademo.com blitfast.com timertst.com mousetst.com xmstest.com emstest.com selftest.com modeswitch.com dpmitest.com dpmiexe.exe; do
    echo "  $f  ($(wc -c < "$DIR/$f") bytes)"
done
