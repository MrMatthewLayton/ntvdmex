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

⚠ The surfaces carried over from PARITY.md still carry the **old four-state column**.
Re-marking them on both axes, with a `file:line` per row, is owed — and is the point:
a row that reads `implemented` may well be **PART**.

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
| **VGA / CRTC / sequencer / graphics / attribute / DAC** | IBM VGA TechRef; FreeVGA | [vga.md](vga.md) — **71 enumerated, measured** | ✅ [`ref/vga.md`](../ref/vga.md) |
| **VESA VBE 2.0 / 3.0** | `docs/ref/vbe20.pdf`, `docs/ref/vbe30.pdf` | — | [PDFs held](../ref/) |
| 8254 PIT | Intel 8254 datasheet | — | — |
| 8259A PIC | Intel 8259A datasheet | [pic.md](pic.md) | — |
| 8237A DMA controller | Intel 8237A datasheet | — | — |
| 8042 keyboard controller | IBM AT TechRef | [keyboard.md](keyboard.md) | — |
| Keyboard (scan code sets 1–3) | IBM AT TechRef | [keyboard.md](keyboard.md) | — |
| PS/2 + serial mouse | Microsoft/Logitech protocol notes | [mouse.md](mouse.md) | — |
| Gameport / joystick | IBM Game Control Adapter | — | — |
| MC146818 RTC + CMOS map | Motorola MC146818 datasheet | — | — |
| PC speaker (PIT ch.2 + port 61h) | IBM TechRef | — | — |
| Sound Blaster Pro / 16 / AWE32 | Creative SB Programmer's Reference | — | — |
| OPL2 (YM3812) / OPL3 (YMF262) | Yamaha datasheets; Nuked-OPL3 as oracle | — | — |
| Gravis Ultrasound | Gravis GUS SDK / Programmer's Guide | — | — |
| MPU-401 + General MIDI | Roland MPU-401 TechRef; GM spec | — | — |
| 16550 UART + LPT | National 16550 datasheet; IBM TechRef | — | — |
| Floppy controller (765/82077) | Intel 82077AA datasheet | — | — |
| IDE / ATA + ATAPI | ATA-x, ATAPI specs | — | — |
| **CPU: 386 → Pentium** | Intel SDM; 386/486 Programmer's Reference | — | real CPU; V86 contract only |

### Firmware

| Surface | Primary sources | Inventory | Ref |
|---|---|---|---|
| PC BIOS INT 10h–1Ah | IBM TechRef; Ralf Brown's Interrupt List | [bios-misc.md](bios-misc.md) | — |
| VGA BIOS (INT 10h) — *distinct from the VGA* | IBM VGA TechRef | [video-bios.md](video-bios.md) | — |
| BIOS Data Area (0040:) + EBDA | IBM TechRef | — | — |

### Software

| Surface | Primary sources | Inventory | Ref |
|---|---|---|---|
| MS-DOS INT 21h/2Fh/25h/26h/28h/29h/2Eh | RBIL; *Undocumented DOS* | [dos-services.md](dos-services.md) | — |
| INT 33h mouse driver | Microsoft Mouse Programmer's Reference | [mouse.md](mouse.md) | — |
| XMS 3.0 / LIM EMS 4.0 / VCPI | XMS + LIM specs | partial | — |
| **DPMI 1.0** | DPMI 1.0 spec | partial — live frontier | — |
| DOS extenders: DOS/4GW, DOS16M | Tenberry/Rational docs | partial | — |
| MSCDEX | MSCDEX spec | — | — |
| Executable formats: MZ, LE, NE, PE | MS format specs | partial (`ne_test`) | — |
| Win16 KERNEL / USER / GDI | Win3.1 SDK; Wine; ReactOS | `docs/research/wow-user-surface.md` (441 ids, 385 named) | — |
| WOW32 thunk ABI | *no spec* — ReactOS prior art + stock WOW oracle | `docs/research/wow32-call-surface.md` | — |
