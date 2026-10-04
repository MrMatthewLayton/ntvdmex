# The inventory — build from the specs, then test the apps

> **The directive (user, 2026-09-22):** *"We are ultimately emulating the bits that
> either don't exist in modern hardware, or Windows XP won't hand over to us. The
> approach from the beginning should have been build the entire emulation layer
> (hardware, firmware, software) from specs … Once everything was implemented from
> specs, we should then test the applications to see what works and what doesn't —
> gaps implemented by what we're building, not what's asking for them."*

## Why this exists

Every guest closure in this project's log was one application dying in one place, and a
session spent finding it: Heretic on a VESA `4F00` OEM string, Hexen on two sequencer
registers a mode set never programmed, Duke3D on a DPMI `0500` block, ZAR on INT 33h
`0Ch` callbacks in protected mode. Each fix was correct. The *method* was not: the
application chose what got built, so "will program X run?" could never be answered in
advance, and every new program began with a bisect.

The user's own example says it best. Doom, Wolfenstein 3D, Mario and Skyroads all use
mode 13h and all use it **differently** — and period hardware copes with all four
because the hardware *is* a set of registers, and the picture is what those registers
make. We built a mode-13h renderer, then a mode-Y path when Doom asked, then a fix for
Mario's artefacts, then Hexen's SR4/SR2. Four implementations of one register set, each
shaped like the program that demanded it.

## The method

1. **Name the surface and its spec.** A real document, not memory: IBM technical
   references, Ralf Brown's Interrupt List, the VESA/LIM/XMS/DPMI PDFs, the Creative
   SB programming guide, the Windows 3.1 SDK.
2. **Enumerate every unit of it** — every register, every function, every bit field.
   Completeness of the *list* comes first; it is what makes coverage measurable.
3. **Mark each one from the CODE**, never from memory, with a `file:line` citation.
4. **Fill top-down**, hardest-used first, not wherever a guest last died.
5. **Applications are the acceptance test**, run after, and a failure is then
   "which off-spec behaviour does this rely on?" — a far smaller search.

## Status vocabulary

| Mark | Meaning |
|---|---|
| **IMPL** | Modelled, with the behaviour the spec describes. |
| **PART** | Some bits/cases modelled, others silently dropped. The dangerous state: it looks present. |
| **STORE** | Value is latched and read back, but nothing consumes it. |
| **MISS** | Not modelled. A write is discarded; a read returns a default. |
| **N/A** | Deliberately out of scope, with the reason recorded. |

⚠ **PART is the state to hunt.** A register that is half-modelled returns a *plausible
wrong answer*, and a guest that gets one runs on and fails somewhere else entirely —
the INT-site patcher's vote and the Lemmings colour-compare defect are both this shape.

**Coverage and verification are two axes, not one.** The mark above says what our code
does; it says nothing about whether anyone checked. `docs/PARITY.md`, which this
directory absorbed on 2026-09-23, used a single four-state column
(`missing`/`guessed`/`implemented`/`verified`) that conflated them — and so could not
express PART at all. Both are now recorded:

| Verification | Meaning |
|---|---|
| **oracle** | A probe asked this of NTVDMEX **and** of a reference, and they agree |
| **provisional** | Compared, but against a reimplementation (SeaBIOS, QEMU) rather than real hardware — re-ask on PCem |
| **untested** | Implemented and perhaps exercised by a guest, never compared |

✅ **The surfaces carried over from PARITY.md are re-marked on both axes** (2026-10-01,
#200): `keyboard.md`, `mouse.md`, `bios-misc.md`, `video-bios.md` and `dos-services.md` now
mark every unit IMPL / PART / STORE / MISS / N/A with a `file:line`, and carry a
verification column. `sweep.md` is a triage log, not a unit inventory; it now maps each open
row to the inventory that owns it. The old column's `verified` split three ways in the
re-mark: **oracle** where a real machine or real DOS answered, **provisional** where the
answer came from SeaBIOS/SeaVGABIOS under QEMU, and **untested** where only our own
batteries ran.

## Verification

A mark of IMPL is a claim about our code, not about hardware. Two things raise it to
*spec-verified*:

- **A probe** in `tools/dostest/` that exercises the unit and prints what it observes,
  run under NTVDMEX **and** an oracle.
- **The oracle**: PCem with a real Tseng ET4000/W32p ROM (`docs/…/pcem-oracle`), the
  6.22 box, stock ntvdm, a live dump. Where no public spec exists — the `NtVdmControl`
  contract, the WOW32 thunk tables, DOS's undocumented internals — the oracle *is* the
  spec, and that is stated per surface rather than glossed.

⚠ **"No public spec" is a much smaller set than it first looks, and claiming it too
early is how a surface escapes the inventory.** Win16 is the example: the *API
semantics* are fully specified by the Windows 3.1 SDK Programmer's Reference, and
implemented in the open by **Wine** (`krnl386.exe16`, `user.exe16`, `gdi.exe16`) and
**ReactOS** (NTVDM + WOW32); the NE format is documented; *Undocumented Windows*
(Schulman et al.) covers much of the remainder. What is genuinely unspecified is
narrow — the WOW32 thunk ABI and krnl386's private structures — and even those have
stock XP's own WOW as a live oracle. Before writing "no spec" against a surface,
name what was searched.

⛔ **An all-AGREE probe is not a verified surface.** Check the probe can fail, and
**poison every output register** before asking — `16.09.support` and `i33.26.maxvirt`
both read as clean matches until the probe stamped `B1`/`C1C1` in first. Only ever omit
the poison where the register is an *input*.

⛔ **Guard every blocking call.** An unguarded `INT 16h AH=00h` on a host whose ring
never fills blocks forever and the probe dies as a harness timeout — an absence that
reads as a hang instead of as data.

The whole-sweep results and their triage (what is a real gap, what was a false positive,
what is blocked on an oracle) are in **[sweep.md](sweep.md)**. Where each specification
and each oracle lives is **[`../ref/SOURCES.md`](../ref/SOURCES.md)**.

## The surfaces

**Scope rule (user, 2026-09-23):** a device is in the inventory because it is part of
the **period-correct hardware contract we are emulating**, not because a guest has
asked for it. *"We don't not implement it because only one app asked for it. We do
implement it because it's part of the period correct hardware contract."* Counting
how many shelf guests touch a surface is app-driven reasoning wearing a spec-first
hat, and it is not a reason to defer.

Each surface gets two documents, doing two different jobs:

- `docs/ref/<surface>.md` — **what the hardware does.** A coherent technical reference
  in our own words, derived from the source documents and cited per section, suitable
  for the project wiki and for a collaborator who has never seen the part.
- `docs/inventory/<surface>.md` — **what we do.** Every unit enumerated and marked
  IMPL / PART / STORE / MISS / N/A from the code, with a `file:line` citation.

### Hardware

| Surface | Primary sources | Inventory | Ref |
|---|---|---|---|
| **VGA / CRTC / sequencer / graphics / attribute / DAC** | IBM VGA TechRef; FreeVGA | [vga.md](vga.md) — **71 enumerated, measured** (2026-09-22) | ✅ [`ref/vga.md`](../ref/vga.md) |
| **VESA VBE 2.0 / 3.0** | `docs/ref/vbe20.pdf`, `docs/ref/vbe30.pdf` | [vesa.md](vesa.md) — 171 units: 139 IMPL, 7 PART, 2 STORE, 20 MISS, 3 N/A | [PDFs held](../ref/) |
| **8254 PIT** | Intel 8254 datasheet | [pit.md](pit.md) — 34 units: 26 IMPL, 4 PART, 1 STORE, 3 N/A (re-cited 2026-10-01) | ✅ [`ref/pit.md`](../ref/pit.md) |
| 8259A PIC | Intel 8259A datasheet | [pic.md](pic.md) — marked 2026-09-23, group table | ✅ [`ref/pic.md`](../ref/pic.md) |
| 8237A DMA controller | Intel 8237A datasheet | [dma.md](dma.md) — marked 2026-09-23; status DRQ bits and command bit 2 re-marked 2026-10-01 (#176) | ✅ [`ref/dma.md`](../ref/dma.md) |
| 8042 keyboard controller | IBM AT TechRef | [kbc.md](kbc.md) — marked 2026-09-23; ⚠ its `vdd_input.c` line numbers predate #188 | ✅ [`ref/kbc.md`](../ref/kbc.md) |
| Keyboard device (commands, ACKs, scan code sets) | IBM AT TechRef | [kbc.md](kbc.md) §5 — ACKs MISS | — |
| PS/2 + serial mouse | Microsoft/Logitech protocol notes | [mouse.md](mouse.md) §3 — **all MISS**: no aux device, no INT 15h `C2h`, no serial mouse | — |
| Gameport / joystick | IBM Game Control Adapter | [gameport.md](gameport.md) — 22 units (s84) | — |
| MC146818 RTC + CMOS map | Motorola MC146818 datasheet | [rtc.md](rtc.md) — marked 2026-09-23 | ✅ [`ref/rtc.md`](../ref/rtc.md) |
| PC speaker (PIT ch.2 + port 61h) | IBM TechRef | [pit.md](pit.md) §6 — 6 units; bit 1 as a PWM sample output is PART | [`ref/pit.md`](../ref/pit.md) §2 |
| Sound Blaster Pro / 16 / AWE32 | Creative SB Programmer's Reference | [sb.md](sb.md) — 112 units: 29 IMPL, 34 PART, 21 STORE, 23 MISS, 5 N/A (s84) | — |
| OPL2 (YM3812) / OPL3 (YMF262) | Yamaha datasheets; Nuked-OPL3 as oracle | [opl.md](opl.md) — OPL3 built in #232 and selected by the `Opl` setting; 3 inferences owed a measurement | — |
| AWE32 EMU8000 wavetable | Creative *AWE32/EMU8000 Programmer's Guide* rev 1.00 | [emu8k.md](emu8k.md) — built in #233; on the bus for the AWE32 model (`BLASTER E`); WC clock and diagnostics owed | — |
| Gravis Ultrasound | Gravis GUS SDK v2.22 (archived) | [gus.md](gus.md) — built in s80–s82 (#189 stereo); remainder is #190 | ✅ [`ref/gus.md`](../ref/gus.md) |
| MPU-401 + General MIDI | Roland MPU-401 TechRef; GM spec | [mpu401.md](mpu401.md) — 32 units (s84) | — |
| 16550 UART + LPT | National 16550 datasheet; IBM TechRef | [uart.md](uart.md) — marked 2026-09-23, measured on three oracles; OUT2 and COM3/COM4 re-marked 2026-10-01 (#181) (LPT: —) | ✅ [`ref/uart.md`](../ref/uart.md) |
| **Floppy controller (765/82077)** | Intel 82077AA datasheet | [fdc.md](fdc.md) — **the chip existed nowhere; MSR read `FFh` and the datasheet's own command loop never exited**. ⚠ INT 13h still does not drive it ([dos-services.md](dos-services.md) §6) | ✅ [`ref/fdc.md`](../ref/fdc.md) |
| IDE / ATA + ATAPI | ATA-x, ATAPI specs | [ide.md](ide.md) — 34 units: 21 IMPL, 1 PART, 12 N/A — **an adapter fitted, both channels empty** (#179, 2026-10-02); the BSY wait that hung on `FFh` now exits on all four hosts | — |
| **CPU: 386 → Pentium** | Intel SDM; 386/486 Programmer's Reference | — | real CPU; V86 contract only |

### Firmware

| Surface | Primary sources | Inventory | Ref |
|---|---|---|---|
| PC BIOS: INT 08h, 11h, 12h, 14h, 15h, 17h, 1Ah, 1Ch | IBM TechRef; Ralf Brown's Interrupt List | [bios-misc.md](bios-misc.md) — 39 units: 17 IMPL, 7 PART, 12 MISS, 3 N/A (2026-10-01, after #206, #253) | — |
| PC BIOS: INT 09h, 16h (keyboard) | IBM AT / PS/2 TechRef (K1S) | [keyboard.md](keyboard.md) — 39 units: 19 IMPL, 4 PART, 11 MISS, 5 N/A (2026-10-01) | — |
| PC BIOS: INT 13h, 25h, 26h (disk) | IBM TechRef; RBIL | [dos-services.md](dos-services.md) §6 — 13 units | — |
| VGA BIOS (INT 10h) — *distinct from the VGA* | IBM VGA TechRef | [video-bios.md](video-bios.md) — 75 units: 36 IMPL, 19 PART, 1 STORE, 17 MISS, 2 N/A (2026-10-01) | — |
| BIOS Data Area (0040:) + EBDA | IBM TechRef; RBIL `MEMORY.LST` | [bda.md](bda.md) — 38 units: 18 IMPL, 2 PART, 14 MISS, 4 N/A (2026-10-01, #253) | — |

### Software

| Surface | Primary sources | Inventory | Ref |
|---|---|---|---|
| MS-DOS INT 21h/2Fh/20h–29h/2Eh | RBIL; *Undocumented DOS* | [dos-services.md](dos-services.md) — 132 units (every INT 21h function): 89 IMPL, 23 PART, 17 MISS, 3 N/A (2026-10-01) | — |
| INT 33h mouse driver | Microsoft Mouse Programmer's Reference | [mouse.md](mouse.md) — 51 units: 22 IMPL, 2 PART, 3 STORE, 20 MISS, 4 N/A; ⚠ we claim v8.00 and none of `25h`–`34h` exists (2026-10-01) | — |
| XMS 3.0 / LIM EMS 4.0 / VCPI | XMS + LIM specs | [xms-ems.md](xms-ems.md) — 187 units: 65 IMPL, 17 PART, 88 MISS (EMS 4.0 subfunctions, VCPI), 17 N/A (s84) | — |
| **DPMI 0.9 / 1.0** | DPMI 0.9 and 1.0 specs | [dpmi.md](dpmi.md) — 64 units: 45 IMPL, 8 PART, 9 MISS, 2 N/A (#247 `0300h`, #248 `0503h`/`0304h`/16 callbacks/`0400h` CL/bad selectors, 2026-10-02; #267 the `0303h` entry contract, #268 `0006h`/`0007h`–`0009h` values, the `0100h` chain, `0500h`, 2026-10-04 — from the code; `p_dpmi2` unrun) | — |
| DOS extenders: DOS/4GW, DOS16M | Tenberry/Rational docs | — still to write. The host contract they run on is already inventoried: [dpmi.md](dpmi.md) (incl. DOS/16M's INT 21h `FF80h`, §10), VCPI in [xms-ems.md](xms-ems.md) (MISS), INT 15h `87h`/`88h` in [bios-misc.md](bios-misc.md) §4 | — |
| MSCDEX + CD audio | MSCDEX spec | [mscdex.md](mscdex.md) — 26 units, 22 MISS: data CDs read as files, but `1500h` is passed through (the answer is the caller's own `BX`) and there is no CD audio (2026-10-01) | — |
| Executable formats: `.COM`, MZ, NE, LE, PE | MS format specs | [exe-formats.md](exe-formats.md) — 36 units: 20 IMPL, 2 PART, 10 MISS, 4 N/A; ⚠ EXEC ignores `e_minalloc`/`e_maxalloc` and runs a Windows program's MZ stub (2026-10-01) | — |
| **Win16 KERNEL / USER / GDI** | Win3.1 SDK; Wine; ReactOS | [win16.md](win16.md) — **deterministic tests now exist**; **[win16-surface.md](win16-surface.md)** (every 16→32 call, all 13 modules, handled / shelf use / rig evidence — generated by `tools/ne/wowinventory.py`) + **[win16-messages.md](win16-messages.md)** (every message, by direction); `docs/research/wow-user-surface.md` (441 ids, 385 named) | — |
| WOW32 thunk ABI | *no spec* — ReactOS prior art + stock WOW oracle | `docs/research/wow32-call-surface.md` | — |
| NTVDM BOP interface (`C4 C4 nn`) | *no spec* — XP's `ntvdm.exe` dispatch table + stock as oracle | [bop.md](bop.md) — our allocations re-cited 2026-10-01; `BOP 54h` subs: 5 IMPL, 1 PART, 6 MISS, 2 N/A | — |
