# Testing

Three layers, fastest first. Most changes are developed against the first; the last is
where correctness is finally established, and it is the slowest.

## 1. The off-machine battery (a minute or two, no Windows needed)

```sh
./scripts/offvm.sh
```

The unit tests in `tests/unit/` (52 test programs, over 3,000 checks) compiled natively for
the build machine and linked against the real `src/` code: the DOS kernel (memory blocks,
the PSP, the loader, INT 21h's pure parts, XMS, EMS, the clock), the device models (PIC,
PIT, DMA, CMOS, keyboard controller, Sound Blaster, OPL, MPU-401, Gravis UltraSound,
EMU8000, video, joystick, serial), both instruction interpreters, the instruction-length
decoder, the NE loader and the Windows 3.x conversions.

A test that does not compile is reported as a failure, not skipped. This is the loop to
develop against, and it must be green before any change is merged.

## 2. Probes, compared with the real thing

`tests/probes/dos/` holds small DOS programs (the CMake build assembles them into
`build/probes/`), each of which asks the machine one question -- an INT 21h function, a
BIOS service, a device register -- and prints the answer in a fixed format.
`tests/probes/win16/` does the same for Windows 3.x programs, with the answers stock NTVDM
gave recorded in `tests/probes/win16/stock/`.

A probe's answer under NTVDMEX is compared with the answers of **oracles**: real MS-DOS 6.22,
stock NTVDM on Windows XP, and other implementations where they help.

> **The voting rule.** The oracles vote on what is true, and their agreement *is* the
> truth. **NTVDMEX does not vote** -- it is the subject under test, and counting it would
> be circular. When the oracles disagree, the result is recorded as disputed, with the
> reasoning, never settled by majority. And the specification outranks them all: a
> documented behaviour wins over any one machine's answer.

### Why oracles disagree

When two oracles give different answers, it has almost always been one of three causes:

1. **Different firmware, not different silicon** -- by far the most common. The PIT's
   power-on mode, most VGA register reset values, the floppy controller's SPECIFY values:
   these are what a *BIOS* writes, not what the chip does. MS-DOS 6.22 under QEMU runs on
   SeaBIOS and a Bochs-derived VGA BIOS; PCem runs a real AMI 486 BIOS and a genuine IBM VGA
   ROM. Same chip, a different program's choices.
2. **Device models that stop where their workloads stop.** An emulator leaves out what no
   guest it runs has ever used (port `80h` reading back, the 8042's rarer commands, the
   floppy's deleted-data commands). A `0` from a machine without the feature is the absence
   of a measurement, not a measurement of absence.
3. **Machine generation** -- real, but rare.

A disagreement is recorded with the decision and its reasoning, so it is not re-argued on
every run.

The harness that drives the oracles is the maintainer's; the probes and their recorded
answers are in the repository so anyone can run them.

## 3. Real hardware

The virtual-8086 and DPMI paths can only be tested on a real Windows XP machine: a virtual
machine cannot run DOS extenders' 32-bit paged protected mode even under stock NTVDM. Before
a change is merged, the maintainer runs it on an XP SP3 test machine:

- a set of games (Skyroads, Doom and ZAR -- timing, graphics, sound, protected mode),
  interleaved with the previous build, so a regression shows against the same conditions;
- the Windows 3.x probes, compared with stock NTVDM's answers;
- for a change that should not alter behaviour at all, a proof that the compiled code is
  identical (below) instead of, or as well as, the runs.

## Proving a change changes nothing

Large refactors here are proven, not just tested:

```sh
./tools/fncmp/fncmp.sh main .                 # main.c's translation unit
FNCMP_ALL=1 ./tools/fncmp/fncmp.sh main .     # every translation unit
```

`fncmp` compiles two trees and compares the generated code function by function and the
initialised data section by section, ignoring order and `__LINE__`. It is itself tested
against deliberate mutants: a reordering must compare identical, and a changed expression,
string or table entry must not.

## Instruments

- `tools/dlgcheck/dlgcheck.py` reads the dialog templates out of the linked executable --
  the bytes Windows will parse, not the `.rc` that produced them -- and reports overlapping,
  out-of-bounds and duplicate controls.
- `cfg\dostrace.flag` makes the host log every INT 21h call. Two runs that differ by one
  input, compared line by line, find what a single run cannot.

Every instrument can be wrong. Check what it measures against the thing it measures before
believing its output.

## What "verified" means

- **Off-machine pass**: the logic is right in isolation. Necessary, nowhere near sufficient.
- **Confirmed on real hardware**: it runs on an XP machine, and the log proves NTVDMEX ran it
  (not stock NTVDM).
- **User-confirmed**: a person watched or heard it. Timing, input latency and sound can
  only be judged this way; no counter stands in for them.
