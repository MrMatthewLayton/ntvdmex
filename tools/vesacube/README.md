# vesacube — a VESA/VBE exerciser for NTVDMEX

`VESACUBE.COM` (real mode, NASM) lists every banked graphics mode the VESA BIOS offers
(`4F00`/`4F01`: number, geometry, depth, pitch, LFB flag) and, for the one you pick, draws a
wire-frame cube that tumbles on three axes and bounces off the screen edges, in that mode's
own pixel format (8 bpp palette, 15/16 bpp direct, 24 bpp B,G,R) through the 64 KB window
with a `4F05` bank switch on every crossing. ESC returns to the menu; ESC again quits.

    VESACUBE            interactive
    VESACUBE 111        run mode 111h for ~12 s and exit (the headless harness has no keyboard)

Build: `nasm -f bin vesacube.asm -o vesacube.com` (`sintab.inc` is generated: 256 x sin*16384).
Deployed at `demo\msdos\vesacube\VESACUBE.COM` on the rig. Verified (s74b): renders under
genuine DOS on QEMU's Bochs VBE (screenshot) and PCem's Tseng ET4000/W32p ROM (clean exit),
and on NTVDMEX (rig desktop screenshot, mode 101h). `scripts/bm/cubeshot.bat` drives the
menu with `rigshot` for unattended screenshots; XP's focus rules make it flaky — a person
at the keyboard is the better test.

Frame pacing is the BIOS tick, not port 3DAh: under the headless harness a retrace poll
sees ~2 edges a second and the cube would crawl.
