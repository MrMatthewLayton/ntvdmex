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
[[patcher-vote-passes-random-bytes]] and the Lemmings colour-compare defect are both
this shape.

## Verification

A mark of IMPL is a claim about our code, not about hardware. Two things raise it to
*spec-verified*:

- **A probe** in `tools/dostest/` that exercises the unit and prints what it observes,
  run under NTVDMEX **and** an oracle.
- **The oracle**: PCem with a real Tseng ET4000/W32p ROM (`docs/…/pcem-oracle`), the
  6.22 box, stock ntvdm, a live dump. Where no public spec exists — the `NtVdmControl`
  contract, the WOW32 thunk tables, DOS's undocumented internals — the oracle *is* the
  spec, and that is stated per surface rather than glossed.

⛔ An all-AGREE probe is **not** a verified surface: check the probe can fail, and
poison every output register first. See `parity-by-inventory`.

## The surfaces

| Surface | File | State |
|---|---|---|
| VGA / CRT | [vga.md](vga.md) | **first pass done** — 71 registers enumerated, coverage measured |
| 8254 PIT, 8259 PIC, 8042, RTC/CMOS, 8237 DMA, 8250, LPT, speaker | — | not started |
| SB16 DSP + mixer, OPL3, MPU-401, gameport | — | not started |
| BIOS INT 10h–1Ah, the BDA, EBDA | — | not started |
| VBE 2.0/3.0 | — | partially covered by s74b's spec-first work (31 checks) |
| DOS INT 21h/2Fh/25h/26h/28h/29h/2Eh/33h, XMS, EMS, DPMI, VCPI, MSCDEX | — | partially covered by `parity-by-inventory` sweeps |
| Win16 KERNEL/USER/GDI export tables | `docs/research/wow-user-surface.md` | USER mapped (441 ids, 385 named) |
