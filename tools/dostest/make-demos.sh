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
nasm -f bin "$DIR/pmfault.asm"  -o "$DIR/pmfault.com"      # GH#18: raw PM-#GP reflect probe (run 59)
nasm -f bin "$DIR/pmtick.asm"   -o "$DIR/pmtick.com"       # session 19: does the KERNEL deliver IRQ0 to a PM handler?
nasm -f bin "$DIR/pmstep.asm"   -o "$DIR/pmstep.com"       # session 19: where does the one-fault-per-PM-entry land?
nasm -f bin -dWITHCALL   "$DIR/pmstep.asm" -o "$DIR/pmcall.com"    # session 19 bisect: + a PM CALL          (completes)
nasm -f bin -dWITHINT1A  "$DIR/pmstep.asm" -o "$DIR/pmt1a.com"     # session 19 bisect: + an INT 1Ah         (completes)
nasm -f bin -dWITHSUBINT "$DIR/pmstep.asm" -o "$DIR/pmsubint.com"  # session 19 bisect: + INT 1Ah in a sub   (completes)
nasm -f bin "$DIR/pmal.asm"     -o "$DIR/pmal.com"         # session 19: AL as a program counter -- proved the guest executes nothing before the fault
nasm -f bin "$DIR/outprobe.asm" -o "$DIR/outprobe.com"     # GH#18: PM I/O-virtualization probe (run 72)
nasm -f bin "$DIR/ioverify.asm" -o "$DIR/ioverify.com"     # GH#18: PM I/O VDD round-trip verify (run 73)
nasm -f bin "$DIR/mode13.asm"   -o "$DIR/mode13.com"       # GH#18: PM VGA render slice (milestone #6)
nasm -f bin "$DIR/animate.asm"  -o "$DIR/animate.com"      # GH#18: real-CPU PM animation (milestone #6)
nasm -f bin "$DIR/bounce.asm"   -o "$DIR/bounce.com"       # GH#18: PM bouncing-box demo (milestone #6)
nasm -f bin "$DIR/kbdbox.asm"   -o "$DIR/kbdbox.com"       # GH#18: interactive PM box, arrow-key input (milestone #6)
nasm -f bin "$DIR/timerbox.asm" -o "$DIR/timerbox.com"     # GH#18: PM timing (INT 1Ah tick) demo (milestone #6)
nasm -f bin "$DIR/tmrhook.asm"  -o "$DIR/tmrhook.com"      # GH#18 #2b: async IRQ0 injection into a hooked PM INT 08h
nasm -f bin "$DIR/pm32irq.asm"  -o "$DIR/pm32irq.com"      # GH#18 run 83: async IRQ0 into a 32-bit-CS PM INT 08h hook
nasm -f bin "$DIR/pm32flat.asm" -o "$DIR/pm32flat.com"     # GH#18 run 84: base-0 ~2GB flat selector (DOS/4GW model)
for f in vgademo.com vga12.com vesademo.com blitfast.com timertst.com mousetst.com xmstest.com emstest.com selftest.com modeswitch.com dpmitest.com dpmiexe.exe pmfault.com; do
    echo "  $f  ($(wc -c < "$DIR/$f") bytes)"
done
# GH #49: the TSR pair. ORDER MATTERS -- p_tsr.asm `incbin`s the child, so the
# child must exist before the parent is assembled. That is deliberate: it makes
# the probe self-contained at run time (it writes the child to disk itself), so
# nothing has to be staged next to it on the rig or under DOSBox.
nasm -f bin "$DIR/p_tsrc.asm" -o "$DIR/p_tsrc.com"   # hooks INT 60h, then TSRs
nasm -f bin "$DIR/p_tsr.asm"  -o "$DIR/p_tsr.com"    # EXECs it, then calls INT 60h
