# NTVDMEX

**A replacement for `ntvdm.exe` on 32-bit Windows XP SP3** that runs DOS programs by
executing 16-bit code on the **real CPU** in Virtual-8086 mode, not by emulating one.

Doom is fully playable on it — 3D rendering, status bar, menus, digital sound, music,
keyboard and mouse — on a real Pentium-era XP box. So is Skyroads. So is MS-DOS 6.22's own
`COMMAND.COM`.

---

## Start here

| If you want… | Go to |
|---|---|
| **Where the project is right now** | [`docs/STATE.md`](https://github.com/MrMatthewLayton/ntvdmex/blob/main/docs/STATE.md) |
| **Why it is built this way** | [Motivations and decisions](Motivations-and-decisions) |
| **How the code is put together** | [Architecture](Architecture) |
| **How to build and run it** | [Building and running](Building-and-running) |
| **How anything is verified** | [Testing and oracles](Testing-and-oracles) |
| **What has gone wrong, repeatedly** | [Traps and lessons](Traps-and-lessons) ← *read this one* |
| **What is left to do** | [Issues](https://github.com/MrMatthewLayton/ntvdmex/issues) |
| **What happened on a given day** | [Session archive](https://github.com/MrMatthewLayton/ntvdmex/tree/main/docs/log/sessions) |

> **These pages are published from [`docs/wiki/`](https://github.com/MrMatthewLayton/ntvdmex/tree/main/docs/wiki) in the main repository.**
> Edit them there and run `tools/wiki/publish.sh`. Editing in the wiki UI works but will be
> overwritten — the repo is the source of truth, so wiki changes are reviewable in a PR and
> arrive with a `git clone`.

---

## What it is, precisely

`ntvdm.exe` is the Windows NT **Virtual DOS Machine**: the process Windows launches when
you run a 16-bit program. On 32-bit Windows it does *not* emulate a CPU — it puts the
processor into Virtual-8086 mode and lets the real silicon run the guest's instructions,
trapping I/O and interrupts back to the host. Emulation was only ever for non-x86 NT and
64-bit Windows.

NTVDMEX does the same thing, by reusing the NT kernel's own VDM machinery through the
undocumented `NtVdmControl` syscall. That is the core bet of the project and it paid off:
DOS/4GW detects our DPMI host and switches into 32-bit paged protected mode on real
hardware, which is not something you can test anywhere else.

**It is not DOSBox, and it is deliberately not a fork of anything.** DOSBox emulates a CPU
and re-implements a PC. This runs the guest on your actual processor, inside Windows'
actual VDM, and re-implements the *services* around it: the DOS kernel, the BIOS, the
video/audio/input devices, and a DPMI host.

## What works

- **Doom** — fully playable, mouse included, user-confirmed on real hardware
- **Skyroads** — fully playable
- **MS-DOS 6.22 `COMMAND.COM`** — prompt, internals, `DIR`, and an external program EXEC'd and returned from
- 103 INT 21h functions; XMS 3.0; EMS (LIM 4.0)
- Text, mode 13h, mode 12h planar, VESA banked; windowed GDI and fullscreen DirectDraw
- SB16 PCM at 99.999% delivery; clean-room OPL2/OPL3; MPU-401 MIDI; PC speaker
- DPMI 0.9 host, real-CPU protected mode, 32-bit DOS/4GW

## What does not

**Win16 is entirely absent** — no NE loader, no `krnl386`/`user`/`gdi` hosting, no
16:16↔flat thunking. `ntvdm.exe` is *also* the host for every 16-bit Windows program, so
this is the largest gap between NTVDMEX and the thing it replaces, and it blocks the
project's actual goal. See [#128](https://github.com/MrMatthewLayton/ntvdmex/issues/128).

Also open: console/stdio integration, in-guest redirection, INT 13h disk services, TSR
residency, and `MEM.EXE` silently reporting wrong figures. The honest list is in
[`docs/STATE.md`](https://github.com/MrMatthewLayton/ntvdmex/blob/main/docs/STATE.md).

## Licence and third-party content

The code is ours. **No Microsoft or id Software binaries are in this repository** — the
MS-DOS floppy images, extracted guest binaries, XP binaries used for reverse engineering,
game data and VM images are all untracked or gitignored, and `DOOM1.WAD` was purged from
history before the repository was made public. The OPL synthesiser is clean-room MIT;
Nuked-OPL was used only as a black-box oracle and none of its code is here.
