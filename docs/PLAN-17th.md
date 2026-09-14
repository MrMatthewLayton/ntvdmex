# Plan to the 17th — a portable NTVDMEX package, and parity by inventory

Written 2026-09-14 (session 71). Three working days remain.

## The deliverable

A zip file that, extracted on **another Windows XP machine**, gives a working NTVDMEX
with one script to make Windows use it for MS-DOS and Win16 programs, and one to put
the machine back. It is tested on a friend's XP box on the 18th, with nobody able to
read our logs. So the package has to be **self-describing and self-recovering**:

```
ntvdmex-<date>\
  install.bat        registers NTVDMEX as the machine's VDM (ntvdmhost.exe /install)
  uninstall.bat      puts the machine's own ntvdm.exe back  (ntvdmhost.exe /uninstall)
  status.bat         says which one is in force              (ntvdmhost.exe /status)
  smoke.bat          runs the bundled self-test under the installed VDM: 8/8 or it says why
  README.txt         what it is, what to run, what to do when something goes wrong
  bm\ntvdmhost.exe   the host (the ONLY binary; build/ntvdmex.exe is the stale launcher)
  bm\selftest.com    the real-mode self-test (8 checks), run by smoke.bat
  cfg\               empty: every knob has a default
  out\               empty: the log goes here -- the one file to send back if it breaks
  guest\wow\         the Windows 3.11 system files Notepad/Paint need (private copy)
```

### Package work (first, because nothing else matters if this fails on the 18th)
1. **The folder is wherever the zip was extracted.** `NTVDMEX_DIR` is hard-coded to
   `C:\Documents and Settings\All Users\Documents\ntvdmex\` (3 uses in `main.c`).
   Derive the root from the host's own path (parent of `bm\`), keep the old path as a
   fallback for the rig. Test by installing from a differently named folder on the rig.
2. **The Win16 half must not depend on XP's `system32` copies.** The WOW launch names
   `C:\WINDOWS\SYSTEM32\KRNL386.EXE` (`main.c:13257`); the package carries `guest\wow\`.
   Confirm which one is loaded and make the package's copy win.
3. **Fresh-box hazards, each already met on the rig:** the 3-strikes counter
   (`startfail.txt`) must not count a user closing the window; the IFEO key must point
   at an existing binary; a modal "no disk in drive A:" box (process-wide
   `SetErrorMode` now); the single-instance mutex; `install.bat` needs admin (HKLM).
4. **The acceptance checklist, run by hand on the rig from a clean install**, and the
   same list is in README.txt for the 18th: selftest 8/8, COMMAND.COM prompt + DIR,
   QBasic (type, Alt-F, mouse menu, drive list), EDIT, Skyroads, Doom (full WAD), Lemmings
   normal-PC mode, Notepad, Paint save.
5. Freeze on the morning of the 17th: tag, build, zip, install the zip on the rig
   **from scratch** (uninstall, delete, extract, install.bat, checklist), push.

## Parity by inventory — the keyboard and mouse first

This week's failures were all in surfaces that were never inventoried: the 8042 byte
pacing, the INT 09h/INT 16h chain and its done-signal, the mouse driver's callback, the
BIOS variable table, the drive ceiling. The DOS services, which WERE inventoried and
measured against MS-DOS 6.22, caused none. Same method, next surface.

`docs/PARITY.md` lists every item of the surface with one of four states:
**missing / guessed / implemented / verified** (verified = a test whose expectation was
recorded on the reference machine, not written from memory). A run's end-of-run report
can only ever catch "asked and refused"; the inventory is what catches "answered
wrongly", "never asked" and "wrong timing".

### References
- **DOS services:** MS-DOS 6.22 under QEMU (`scripts/oracle.sh`, image `vm/dos622.img`).
- **BIOS + VGA:** PCem on this Mac, with the genuine IBM VGA BIOS from the ROM pack
  (`PCem-ROMs-master`), booting the same `dos622.img` (raw FAT16, PCem-compatible).
  QEMU's SeaBIOS is a rewrite and its VGA draws one palette per frame: not a BIOS or
  raster reference. **Needs: a PCem macOS binary (user), a machine choice from the pack.**
- **Chips (8042, 8259, 8254):** datasheet first, PCem second, QEMU third; the real
  Windows-98-era box only as a tie-breaker on physical timing.
- ⚠ An oracle answers the question asked. Read the guest program's own idiom first
  (`scripts/guestdis.py`) and ask exactly that.

### Day plan
- **15th:** package items 1–3; PARITY.md skeleton; oracle programs `p_kbd.asm` (INT 16h
  all functions, 0040:0017/0096, port 60h/64h re-read semantics, BIOS ring), `p_pic.asm`
  (EOI/in-service rules as a hooked INT 09h sees them), `p_mouse.asm` (INT 33h function
  table incl. 0Ch/14h callbacks -- `demos/qb45/MOUSE.COM` on the oracle image, QMP
  mouse events to drive it). Run on 6.22; pin as batteries; fix what differs.
- **16th:** PCem up on the Mac; `p_video.asm` (INT 10h sub-functions, the BDA 0449..008A
  after each mode set, 1112h/1111h/1114h rows, cursor via CRTC); close the list; one
  by-hand pass on the rig with the checklist.
- **17th:** freeze, package, fresh install on the rig, README, tag, push.

### What cannot be 1:1, stated once
Port access costs microseconds a real chip does not spend (the 6.9 MHz apparent
ceiling), and Windows allows no true DMA from user space. Everything behavioural is
reachable; anything a program does differently from the reference machine is a defect
to find by comparison, not by waiting.
