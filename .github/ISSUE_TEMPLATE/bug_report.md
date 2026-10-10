---
name: A program does not work
about: A DOS or Windows 3.x program fails, looks wrong, sounds wrong or runs at the wrong speed
labels: bug
---

**The program:** name, version, and where it came from (a CD, a shareware archive, ...).

**What should happen**, and **what happens instead.** A screenshot helps when the picture is
wrong.

**How to get there:** the steps from starting the program.

**Under stock NTVDM** (`uninstall.bat`, then run it again), does it work? *(yes / no / not
tried)*

**The machine:** Windows XP SP3 32-bit, real or virtual; the processor; the graphics and
sound hardware.

**The build:** the zip's name (`ntvdmex-<date>-<sha>.zip`) or the commit.

**The log:** attach `debug\out\ntvdmhost.log` from the installation folder, from the run
that went wrong.
