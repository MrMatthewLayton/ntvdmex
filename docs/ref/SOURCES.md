# Sources — the specifications this project is built from

> **The directive:** build the emulation layer from hardware, firmware and software
> specifications, then test the applications. This file is the index of those documents:
> what each surface's authority *is*, where to get it, and what it is good for.
>
> A PDF is not a plan. The working artefacts are two per surface —
> [`docs/ref/<surface>.md`](.) (what the hardware does, in our own words, cited) and
> [`docs/inventory/<surface>.md`](../inventory/) (what we do, marked from the code).
> This file only tells you where the authority lives.

## Standards to download

No third-party document is held in this repository. These are freely published; get them
from their publisher:

| Document | What |
|---|---|
| `vbe20.pdf` | VESA BIOS Extension (VBE) Core Functions Standard 2.0 |
| `vbe30.pdf` | VESA BIOS Extension (VBE) Core Functions Standard 3.0 |

⚠ **The repository is public.** Bulk-mirroring third-party specifications here is a
licensing decision, not a convenience one, and converting a copyrighted document to
Markdown verbatim is still redistribution. The default is therefore: cite the source, and
write our **own** derived reference in `docs/ref/<surface>.md` — which is better documentation anyway, because it says what *we*
need in the order *we* need it.

## Hardware

| Surface | Authority | Good for | Not good for |
|---|---|---|---|
| **VGA / CRTC / sequencer / graphics / attribute / DAC** | IBM VGA Technical Reference; the FreeVGA reference | Every register, bit field and the address generator | What a *BIOS* chose to leave in them — measure that |
| **VESA VBE 2.0 / 3.0** | Held here, above | Mode lists, LFB, direct colour, the PM interface | Pre-VBE Super VGA chipsets |
| **8254 PIT** | Intel 8254 datasheet | Counter modes, latch and read-back, the restart rule | BIOS tick conventions — that is firmware |
| **8259A PIC** | Intel 8259A datasheet | ICW/OCW, in-service and EOI rules, cascade | |
| **8237A DMA** | Intel 8237A datasheet | Channels, modes, page registers, autoinit | Windows grants no true DMA from user space |
| **8042 keyboard controller** | IBM AT Technical Reference | Commands, status bits, **A20** and CPU reset | Scan code semantics — that is the keyboard |
| **Keyboard** | IBM AT TechRef (scan code sets 1–3) | Make/break codes, typematic, the LED protocol | |
| **Mouse** | Microsoft Mouse Programmer's Reference; PS/2/serial protocol notes | INT 33h contract, packet formats, callbacks | |
| **Gameport / joystick** | IBM Game Control Adapter reference | The one-shot timing that *is* the reading | |
| **MC146818 RTC + CMOS** | Motorola MC146818 datasheet | Register file, alarm, periodic interrupt | The CMOS *map* is a BIOS convention |
| **PC speaker** | IBM TechRef (PIT channel 2 + port 61h) | Gate/data bits | |
| **Sound Blaster Pro / 16 / AWE32** | Creative Sound Blaster Programmer's Reference | DSP commands, mixer, DMA modes, IRQ | |
| **AWE32 EMU8000 wavetable** | *AWE32/EMU8000 Programmer's Guide* rev 1.00, Dave Rossum (E-mu/Creative, 1994–96) — the AWE32 Developer Information Pack; archived at [dosdays.co.uk/media/creative/emu8kpgm.pdf](https://www.dosdays.co.uk/media/creative/emu8kpgm.pdf), **not mirrored** (its licence forbids redistribution) | The port interface, every register and bit field, initialisation, the sound-memory DMA streams, starting/ending a note | The GM **ROM contents** (Creative's, not published); the reverb/chorus programs the INIT arrays load are data, not described. Inventory: [`../inventory/emu8k.md`](../inventory/emu8k.md) |
| **OPL2 (YM3812) / OPL3 (YMF262)** | Yamaha datasheets | Register map, operators, envelopes | Exact analogue timbre — use Nuked-OPL3 as a **black-box oracle** only |
| **Gravis Ultrasound** | *UltraSound SDK v2.22* (Gravis/FORTE, 1994) — manual Ch. 2 + the SDK's driver source; archived at [github.com/RobertSundling/GUSDK222](https://github.com/RobertSundling/GUSDK222), **not mirrored** (user decision, 2026-09-25) | GF1 voices, DRAM, the DMA/IRQ contract, how software detects the card | The ICS-2101 mixer and CS4231 codec — later options, not the GF1. Derived reference: [`gus.md`](gus.md) |
| **MPU-401 + General MIDI** | Roland MPU-401 Technical Reference; the GM spec | UART and intelligent modes, the GM sound set | |
| **16550 UART · LPT** | National 16550 datasheet; IBM TechRef | FIFO, modem/line status, the parallel strobe | |
| **Floppy controller** | Intel 82077AA datasheet | Command phases, result bytes, the drive table | ⚠ The *three-phase handshake* is the chip; a register-by-register reading misses it entirely — see [`fdc.md`](fdc.md) |
| **IDE / ATA + ATAPI** | ATA-x and ATAPI standards | Command set, identify, packet commands | |
| **CPU, 386 → Pentium** | Intel SDM; 386/486 Programmer's Reference | V86 mode, descriptors, VME/VIF, exceptions | We run on the **real CPU** — this describes the contract, not a model to write |

## Firmware

| Surface | Authority | Good for | Not good for |
|---|---|---|---|
| **PC BIOS, INT 10h–1Ah** | IBM Technical Reference; **Ralf Brown's Interrupt List** | Every service, register in and out | Undocumented behaviour a guest relies on — that needs a machine |
| **VGA BIOS (INT 10h)** | IBM VGA Technical Reference | Mode tables, the font, the save area | ⚠ **Distinct from the VGA.** INT 10h is firmware; the register file is hardware. We conflate them today. |
| **BIOS Data Area (0040:) + EBDA** | IBM TechRef | Field-by-field layout | |

## Software

| Surface | Authority | Good for | Not good for |
|---|---|---|---|
| **MS-DOS** | Ralf Brown's Interrupt List; *Undocumented DOS* | INT 21h/2Fh/25h/26h and the internal structures | |
| **INT 33h mouse driver** | Microsoft Mouse Programmer's Reference | The function table | |
| **XMS 3.0 · LIM EMS 4.0 · VCPI** | The published specifications | The full function sets | |
| **DPMI 1.0** | The DPMI 1.0 specification | Descriptors, memory, exceptions, translation | What a specific extender assumes |
| **DOS extenders** | Tenberry/Rational documentation | DOS/4GW and DOS16M behaviour | |
| **MSCDEX** | The MSCDEX specification | The redirector interface | |
| **MZ · LE · NE · PE** | Microsoft format specifications | Headers, relocations, segment tables | |
| **Win16 KERNEL / USER / GDI** | **Windows 3.1 SDK Programmer's Reference**; **Wine** (`krnl386.exe16`, `user.exe16`, `gdi.exe16`); **ReactOS** | The full API contract, and two open implementations of it | |
| **WOW32 thunk ABI** | ⛔ **No public specification.** ReactOS's WOW32 is prior art; stock XP's own WOW is the live oracle | | — |

⚠ **"No public spec" is a claim, not a default.** Win16 was written off here once and should
not have been: the SDK specifies the API and two open projects implement it. Before writing
"no spec" against a surface, name what was searched.

## Oracles — when the document is not enough

A specification says what the hardware should do. These say what a machine *did*.

| Oracle | Good for | Not good for |
|---|---|---|
| **MS-DOS 6.22 under QEMU** | INT 21h — a genuine Microsoft kernel — and any driver run on it (`MOUSE.COM`) | The BIOS: QEMU's SeaBIOS is a reimplementation |
| **PCem + a genuine Tseng ET4000/W32p or IBM/AMI ROM** | INT 10h/16h, the BDA, the VGA register file, chip timing | ✅ **Runs unattended**, boot + run in ~60 s. ⚠ Must run **outside the command sandbox** — the XPC/Swift crash blamed on the WindowServer for several sessions was the sandbox, and the "it needs ROMs" was its data directory being `~/PCem/` and not the path our own notes gave |
| **Stock `ntvdm` on the test machine** | What we are replacing: the DOS API, and the WOW32 thunk ABI | Devices, sound, VESA |
| **Nuked-OPL3** | Bit-exact FM output, as a **black box** | Reading its source — the synth here is clean-room MIT |
| **The test machine** | The only instrument that can see a wrong picture | Input lag; it is unattended |

⛔ **An all-AGREE probe is not a verified surface.** Check the probe can fail, and
**poison every output register** before asking — `16.09.support` and `i33.26.maxvirt` both
read as clean matches until the probe stamped `B1`/`C1C1` in first.
⛔ **Guard every blocking call.** An unguarded `INT 16h AH=00h` on a host whose ring never
fills blocks forever, and the probe dies as a harness timeout — an absence that reads as a
hang instead of as data.
