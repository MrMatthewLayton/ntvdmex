# NTVDMEX

New Technology Virtual Dos Manager Extended

I want to build a full replacement for NTVDM in Windows XP SP3 (32-bit only). I do not want CPU emulation — code must actually execute on the CPU, with caveats for things like sound, graphics, networking, peripherals, etc. For all of these things, we just make calls to the host.

### Requirements

-   Actual CPU execution (no CPU emulation)
-   Emulation of devices such as mouse, keyboard, sound, graphics
-   Graphics is special case — if we can directly access VGA/VESA, then bare metal over emulation
-   Must fit the Windows XP Luna theme
-   Pluggable so that other developers can hook their own drivers into it
-   Actual replacement for NTVDM (not a right-click > "Run with NTVDMX", and not a separate DosBox experience)

### Research

ReactOS, NTVDM64, DosBox, PCEm, PC86, Vogons, etc.