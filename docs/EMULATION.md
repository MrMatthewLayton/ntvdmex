# NTVDMEX Emulation

> **The user's specification (2026-09-29), recorded verbatim.** It states the behaviour
> wanted from the emulation layer; the work against it is tracked in GitHub issues.

This document describes the desired behaviours for hardware emulation in NTVDMEX.

## Processor

Not emulated, but we should be able to throttle the CPU to period correct speeds, which should including the following:

| CPU               | Speed     |
| :---------------- | :-------- |
| Intel 386DX       | 16 MHz    |
| Intel 386DX       | 33 MHz    |
| Intel 486DX       | 50 MHz    |
| Intel 486DX2      | 66 MHz    |
| Intel 486DX4      | 100 MHz   |
| Intel Pentium     | 133 MHz   |
| Intel Pentium MMX | 200 MHz   |
| Intel Pentium II  | 300 MHz   |
| Intel Pentium III | 600 MHz   |
| Intel Pentium III | 1 GHz     |
| Host              | Unlimited |

This list should be dynamic according to the host's capabilities; for example:

* A host with a 3GHz physical CPU should be able to throttle to all of the other options (386DX to Pentium III) because the host is capable of all those speeds.
* A host with a 500MHz physical CPU cannot throttle to 600MHz or 1GHz, so those options appear disabled. They would see Intel Pentium II (300MHz), and Host (Unlimited) which for them would be 500MHz.

NTVDMEX already has this, but it's inaccurate; for example, I know that Doom and Skyroads will play easily, without breaking a sweat on a 486DX2 66MHz CPU, but even setting NTVDMEX to 100MHz today is unplayable.

## Graphics

We must accurately emulate IBM compatible PC VGA screen modes 0 (text mode) to 13 (graphics), and VBE/VESA 1.0, 2.0, and 3.0. All available video modes must operate exactly as they would in an IBM compatible PC.

* Graphics output appears drawn to the video output of the NTVDMEX host window, or fullscreen.
* Graphics rendering can be handled via GDI (mostly software rendered) or DirectDraw (hardware rendered).
* Applications that request vsync should sync to the screen refresh rate.
* It should also be possible to force vsync for applications that don't ask for it.
* Scaling and filtering should be a user decision.
* With scaling or filtering turned off, pixels are stretched but remain sharp over the available video output.
* Aspect ratio provides (note — I mention window or video output because a window can be resized, but the physical screen can't, therefore we can resize the window to fit the desired aspect ratio exactly to the video output, but when physical constraints are applied, then we resize the video output to fit the aspect to the physical medium):
  * Auto — The window or video output automatically sizes according to the desired graphics mode.
  * 4:3 — The window or video output is forced into 4:3
  * 16:10 — The window or video output is forced into 16:10
  * 16:9 — The window or video output is forced into 16:9
* Optional double-buffering to prevent flickering.
* Provide a setting for monochrome emulation and other color filtering; for example:
  * Default — Full color as it renders now
  * Sepia — Washed out like an old photograph
  * Monochrome white — black and white
  * Monochrome green — black and green
  * Monochrome orange  — black and orange
* Key to this is that the graphics pipeline must not be bottlenecked. It is imperative that we have as little friction between an app requesting to display something, and that thing appearing on screen.

## Sound

Since there is no standard for sound compatibility, we emulate well known standards for period correct sound playback; including:

* Sound Blaster Pro
* Sound Blaster 16
* Sound Blaster AWE
* Yamaha OPL2 / OPL3
* Gravis Ultrasound (GUS)

For the emulated sound devices:

* We should be able to select between WinMM and DirectSound.
* We should be able to adjust hardward settings like base addresses, interrupts, and DMA channels.
