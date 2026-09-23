# Oracle disagreements, and what we decided

GH #26. The epic's rule: **agreement is truth; disagreement is a flagged decision
with a recorded rationale, never a coin-flip.** This file is that record.

Each entry here has a machine-readable twin in `tools/dostest/oracle-rules.json`,
which `scripts/dosdiff.py` loads. That is deliberate — a rationale that lives only
in prose gets forgotten, and the harness would keep re-reporting a dispute we
already settled. A permanently DISPUTED row you have learned to ignore is worse
than no row at all.

A rule makes a named host **abstain** on one field. Abstaining is not the same as
being ignored: the value is still printed, and the rationale is printed next to it
on every run, so the reasoning stays in front of you.

## Who votes

| Host | Role | Weight |
|---|---|---|
| `msdos622` | oracle | Genuine Microsoft kernel. For INT 21h it **is** the standard. |
| `dosbox-x` | oracle | A fourth voice — decades of distilled compatibility fixes. Never truth alone. |
| `ntvdm` (stock) | oracle | What we are replacing. Fine for the DOS API; worthless for devices/sound/VESA. |
| `ntvdmex` | **subject** | **Does not vote.** It is the thing being graded; letting it into the consensus would be circular. |

FreeDOS is deliberately absent: per the epic it gets no vote on truth, and is
consulted only as readable source when the others disagree and we need to know
*why*.

---

## 1. `dosver` / `int21.30` / `AX` — DOSBox-X abstains

*Recorded 2026-08-20.*

| Host | Value |
|---|---|
| `msdos622` | `1606` (AL=06 major, AH=0x16=22 minor → 6.22) |
| `dosbox-x` | `0005` (5.0) |

**Decision: MS-DOS 6.22 is truth; DOSBox-X abstains.**

DOSBox-X's reported DOS version is a *configurable emulator setting* (`ver` in its
config, defaulting to 5.0). Its answer is therefore evidence about **DOSBox's
default**, not an observation about MS-DOS, and carries no weight on the question
"what does DOS report".

The irony is the point: NTVDMEX's version is about to become selectable too
(#28), which is precisely why a configurable emulator's value cannot be truth for
anyone else.

## 2. `dosver` / `int21.3306` / `BX` — DOSBox-X abstains

*Recorded 2026-08-20.* Same cause as #1 — `AX=3306h` reports the same configurable
version through `BL:BH`, so DOSBox-X is again reporting its own `ver` setting.

## 3. `unimp` / `int21.73` / `CF` — DOSBox-X abstains

*Recorded 2026-08-20.*

| Host | Value |
|---|---|
| `msdos622` | `CF=0` |
| `dosbox-x` | `CF=1` |

`AH=73h` is the FAT32 function group introduced in DOS 7.1. MS-DOS 6.22 does not
define it at all, so it lands in the undefined bucket and returns `CF=0` with `AX`
unchanged — exactly like `AH=FFh` and `AH=88h`. DOSBox-X implements the group and
returns `CF=1` for the unsupported subfunction: a sensible answer for the later
DOS it emulates, but an answer to a different question.

---

## A deliberate deviation of OURS (not an oracle dispute)

`int21.FF/CF` and `int21.88/CF` show as MISMATCH and are **left that way on
purpose**. This is recorded here so nobody "fixes" it without reading the
argument.

**Measured:** real MS-DOS 6.22 answers an undefined INT 21h function with `CF=0`
and `AX` unchanged (confirmed on `AH=FFh`, `73h`, `88h` — `tools/dostest/p_unimp.asm`).
**We return `CF=1`.**

Note this also refutes the plan written into GH #27, which says *"Set AX=1
(invalid function) alongside CF on unhandled INT 21h calls; DOS sets both, we
only set carry."* The oracle says DOS sets **neither**.

We keep `CF=1` anyway, because our unhandled tail is reached by two different
kinds of call and they want opposite answers:

- **Functions DOS does not define** (`FFh`, `88h`) — matching DOS means `CF=0`.
- **Functions DOS defines and we simply have not written yet** (`4Bh` EXEC, `4Eh`
  find-first, `39h` mkdir …) — here `CF=0` would tell the program *"your request
  succeeded"* when nothing happened. That is the silent-failure class #27 exists
  to remove, and it is a worse outcome than a visible error.

Splitting the two needs a table of which `AH` values MS-DOS 6.22 actually
defines. That table is worth building — #29–#38 need it anyway — at which point
the undefined bucket should switch to `CF=0`/`AX` unchanged to match the oracle,
and the not-yet-written bucket should keep failing loudly.

**Until then this is a known, evidence-backed deviation, not an oversight.**

---

## Fields deliberately NOT compared

These are excluded at the probe rather than here, via the `SIG` declaration in
the canonical dump (`tools/dostest/probe.inc`). Recorded so the reasoning isn't
lost:

- **`DS` / `ES`** — follow the PSP, which sits at a different paragraph on every
  host (`04BD` on the 6.22 oracle, `0813` under DOSBox-X). Comparing them
  manufactures disagreements that mean nothing and bury the real ones.
- **`FL`** — most flag bits are undefined after a DOS call. `CF` is compared
  where it carries an answer; the rest is informational.
- **`DH` from `AX=3306h`** — bit 4 means "DOS is in the HMA", a property of the
  host's `CONFIG.SYS` (this oracle boots `DOS=HIGH`), not of the DOS version.
  Asserting on it would report a configuration difference as a defect.

---

## `p_ioctl` (INT 21h AH=44h, session 37) — three disputes, one cause

`AL = 08h` "is this block device removable", `09h` "is it remote" and `0Eh` "get the
logical drive map" were implemented in session 37 because krnl386 probes **every drive
with all three** and our host was answering the worst thing available: carry clear —
meaning success — with the caller's own registers as the answer. That lie made krnl386
flag drive C: in its own per-drive table, which is what stopped `WOWEXEC.EXE` loading.

They were written from the documented interface, not from a run, so `tools/dostest/p_ioctl.asm`
asks the panel. It reports three DISPUTED fields, and **all three have the same cause**:

```
case                 msdos622  dosbox-x  ntvdmex
int21.19.curdrive    0000 (A:) 0002 (C:) 0002 (C:)
int21.4408.default   0000      0001      0001       removable / fixed
int21.4409.default   0000      0000      0000       AGREE -- not remote
int21.440E.default   0001      0000      0000       drive-letter alias
```

**The 6.22 oracle boots from a floppy image, so its default drive is A: — and every
case says "the default drive".** That is host geometry, not DOS behaviour:

- `4408h` — A: *is* removable, so `AX=0` is the right answer there, and C: is fixed, so
  `AX=1` is the right answer on the other two. Both are the same rule applied to
  different hardware.
- `440Eh` — a floppy-boot 6.22 has A: and B: aliasing one physical drive, so the drive
  map reports which letter is current (`AL=1`); a fixed C: with no alias reports `AL=0`.
  Again one rule, two configurations.
- `int21.19.curdrive` is in the probe **for this reason** — the first run of it had no
  such row, and there was no way to tell geometry from a behaviour difference. An
  instrument that makes you guess which drive it asked about is most of the way to
  being no instrument.

★ **The field that matters is not disputed.** `4409h`'s remote bit reads `0` on all
three hosts, and that is the one thing any caller — krnl386 included — actually asks.

⚠ A like-for-like comparison of `4408h`/`440Eh` would need a drive letter that exists on
every host in the panel, which it does not currently share. Worth revisiting if a floppy
is ever mounted on all three; not worth manufacturing one now.

⚠ `AH` is masked off in the probe for `19h` and `440Eh` — RBIL documents it as destroyed,
and the panel duly differs on it. The first run compared the whole `AX` for `440Eh` and
reported ours as `4400` against the oracles' `07xx`, which is a disagreement about a byte
the interface does not define. Same discipline as `p_dir.asm`'s note on `47h`.

---

# 2026-09-23 — dosbox-x came back, and it moved four "blocked on PCem" rows

**The adapter was excluded on a false premise.** `dosdiff.py` carried a comment saying
DOSBox-X *"insists on a real window: `SDL_VIDEODRIVER=dummy` makes dosbox-x hang"*. Re-tested
on DOSBox-X 2026.05.02 (SDL2), the dummy driver works fine — it runs to completion and writes
`OUT.TXT`. The claim was probably true of an older build, or of **dosbox-staging** (which does
abort on `dummy`), and it had quietly cost the project its **only** oracle askable without a
human at the keyboard: PCem needs the WindowServer too, so with dosbox-x excluded there was no
second voice available from an agent shell at all.

⚠ It is still **an emulator's opinion**, not silicon. It is a third voice on *chip* behaviour;
it is not a substitute for PCem on *what a real BIOS leaves*.

## Settled by the third voice

| Row | msdos622 | dosbox-x | Decision |
|---|---|---|---|
| `vga.dacmask.wr3C` | `0x0000` | **`0x003C`** | **QEMU is the outlier.** 3C6 is a read/write register that returns what was written, and the DAC mask defaults to `0xFF` — spec **and** dosbox-x agree with **us**. Our `0x3C` was never wrong. |
| `vga.mode*` byte 3 (DAC mask) | `00` | **`FF`** | Same row, same conclusion: our `0xFF` default matches dosbox-x and the datasheet. |
| `pit.bcd.valid` | `0` | **`1`** | **QEMU does not model PIT BCD; dosbox-x does**, and agrees with the datasheet. BCD is no longer blocked — implement it. |
| VGA **modes 04 / 06** | — | — | ⛔ **NEVER ACTUALLY OPEN. My mistake, not the oracle's** — see below. |

### ⛔ Modes 04/06 were a bug in the checking script, not in the data

They were recorded as blocked on PCem because the derivation produced 100 rows for a 200-line
mode. `docs/ref/vga.md` §5.1 states the rule correctly — **`CR09.7` and Max Scan Line are
alternatives, not cumulative** — and the ad-hoc script I verified with divided by **both**, in
the same commit that wrote the rule down.

Re-derived from the **same 6.22 bytes** with the stated rule: **all six modes correct**,
including 04 and 06 at `CR09 = 0xC1`. dosbox-x agrees, with the same `CR09`.

⇒ **A derivation is only as good as the code that checks it.** The document was right and the
checker was wrong, and because the checker was throwaway it got no review at all.

## Newly disputed — genuinely open, and these DO need silicon

| Row | msdos622 | dosbox-x | Why it matters |
|---|---|---|---|
| `pit.rdback.st0` | `0x34` (mode **2**) | `0x36` (mode **3**) | **Two BIOSes, two answers** — which confirms the mode is a BIOS choice, not a chip fact, and that the original memory-written "mode 3" was right *for some machines*. We currently match QEMU's. Harmless either way for the tick; a guest that reads it back gets one of two truths. |
| `pit.mode6.readback` | `0x0C` (**un**-normalised) | `0x04` (**normalised** to 2) | ⚠ **This one was called "confirmed against a real kernel" on the strength of ONE oracle.** We built `mode_raw` to report the bits as programmed because QEMU does. dosbox-x normalises. **A one-host run is not a pass**, and this is that rule catching a claim of mine from earlier the same day. |
| `vga.*` byte 2 (InpStat0) | `0x00` | `0x60` / `0x70` | Previously recorded as "confirmed `0x00`" — again on one oracle. Bits 5:6 are undefined/reserved, so neither is obviously wrong. |

---

# 2026-09-23, later — PCem ran, and it settled every open row

The blocker was never the program. **PCem's data directory is `~/PCem/`**, which it had
created itself with `configs/ nvr/ screenshots/` and **no `roms/`** — so it could not find a
single romset. The note in `scripts/pcemoracle.py` (and the memory notes repeating it) said
`~/Library/Application Support/PCem/`; symlinking there changed nothing, because nothing
looks there. ⚠ **A path in a note is a claim — `ls` the directory the program actually
creates.**

Separately: the XPC/Swift crash on launch, which I had twice told the user proved "PCem
cannot run from an agent shell", was **the command sandbox** blocking its connection to the
window service. Not the desktop session.

## Every previously-disputed row, settled against a real AMI 486 BIOS + IBM VGA ROM

| Row | 6.22 (QEMU) | dosbox-x | **PCem** | ours | Outcome |
|---|---|---|---|---|---|
| `pit.rdback.st0` | `0x34` (mode 2) | `0x36` (mode 3) | **`0x36`** | `0x36` | ✅ **Real BIOS leaves counter 0 in MODE 3.** QEMU is the outlier; we now match. |
| `pit.mode6.readback` | `0x0C` | `0x04` | **`0x0C`** | `0x0C` | ✅ **Un-normalised confirmed on silicon.** `mode_raw` was right; dosbox-x is the outlier. |
| `vga.dacmask.wr3C` | `0x0000` | `0x003C` | **`0x003C`** | `0x3C` | ✅ **Ours confirmed.** QEMU is the outlier. |
| `vga.*` DAC mask byte | `00` | `FF` | **`FF`** | `FF` | ✅ Ours confirmed. |
| VGA **modes 04/06** | — | — | **9 of 9 derive** | — | ✅ Never open; the checker was wrong. |

⛔⛔ **AND THE MODE-3 ROW IS A CORRECTION TO A CORRECTION.** `docs/ref/pit.md` said "counter 0
is programmed mode 3", written from memory. I measured QEMU, got mode 2, and "corrected" the
document — twice, in two files, with a note about never writing expectations from memory.
**The original claim was right.** A single-oracle measurement is not more trustworthy than a
remembered fact merely because it is a measurement: it is *one machine's* answer, and that
machine was the unrepresentative one.

## Still open

| Row | Status |
|---|---|
| **VGA Input Status 0 bit 7** (CRT interrupt) | dosbox-x sets it after a retrace and clears it through CR11 bit 4; **PCem's IBM VGA never sets it**, nor does QEMU. The IBM VGA spec describes the bit. 2 of 3 hosts against, no guest known to use it ⇒ **recorded as a gap, not built.** `p_vgaext` case `is0.vsync`. |
| **VGA Feature Control** (`3CA` read-back) | 6.22 and dosbox-x both return `0x00` whatever is written to `3DA`; **PCem returns `0xFF` to every read**, which is an undecoded port floating high. **No oracle implements the register, so none is evidence about one.** The spec says it reads back; we are the only host that does. *Spec-implemented, unverifiable.* |

## Closed since

### `pit.bcd.valid` — closed 2026-09-23, and the reasoning is the template

6.22 `0`, **PCem `0`**, dosbox-x `1`: two of three, including the real-BIOS machine, do not
model BCD at all. **A `0` from a host that does not implement a feature is the absence of a
measurement, not a measurement of absence** — so the majority here was a majority of
omissions. The inventory had this parked as *"blocked on PCem; implementing from the
datasheet would be writing an expectation from memory"*, which conflated two things: the
rule is never write an expectation from **memory**, and Intel 231164-005 is a **cited
source**. Implemented from the datasheet, marked *spec-implemented / unverifiable*, evidence
in `tools/dostest/pit_test.c` T12 (8 of 14 checks failed against the old code), abstention
recorded in `oracle-rules.json` so the rationale prints on every run.

### VGA Input Status 0 — closed 2026-09-23, at `0x10`

| Register | ours (was) | 6.22/QEMU | dosbox-x | **PCem (real AMI + IBM VGA)** | ours (now) |
|---|---|---|---|---|---|
| **Input Status 0** (`3C2` read) | `0x00` | `0x00` | `0x70` mode 3, `0x60` mode 13h | **`0x10`, all 12 modes** | **`0x10`** |

`0x10` is **bit 4, Switch Sense**. `p_vgaext`'s `is0.live` case settles the *shape* as well
as the value: it ANDs and ORs 65536 reads across several frames, and every host answers a
constant, so a constant was always the right shape for us — only the value was wrong. Two of
three hosts drive bit 4, and the one that matters runs period-correct firmware.

⛔ **This row had been recorded as "confirmed `0x00`" TWICE, on one oracle each time.**

## ⛔⛔⛔ The reference that scored the VGA was itself a single oracle

`tools/vgaparity.py` reported *"689/768 bytes, 89.7%"* against `vgareg.ref.txt` — 6.22
**under QEMU**, i.e. the Bochs VGABIOS. With PCem now running unattended, the same probe
under a real AMI BIOS and a genuine IBM VGA ROM gives a second reference, and **the two
oracles disagree with each other on 78 of 768 bytes** — the same order as the error the
score was reporting.

> So a byte we "failed" may have been us matching real hardware, and a byte we "passed" may
> have been us matching an anachronism. The number could not tell the two apart, and it was
> quoted as though it could.

`vgaparity.py` now scores only the bytes both oracles agree on and reports the disputed ones
separately, with how we answer each. Details, including two defects in `p_vgareg` that were
manufacturing data, are in [`../inventory/vga.md`](../inventory/vga.md) step 5.

## And PCem was never the blocker

Rows here were parked on *"PCem is unavailable / needs the WindowServer / needs ROMs"* for
several sessions. All of it was mine: the crash was **the command sandbox**, and PCem's data
directory is **`~/PCem/`**, not the `~/Library/Application Support/PCem/` our own notes gave
— so the ROMs were installed where PCem never looks. It now runs start to finish unattended
in about 60 seconds. *A path in a note is a claim; `ls` what the program actually creates.*

---

# ⛔⛔⛔ "QEMU is the outlier" does not survive a count

I wrote that phrase into three places — a rule in `oracle-rules.json`, a header comment in
`vdd_dma.h`, and the DMA inventory — as though it were a property of the host. It is not.
All three are corrected; this is why.

## The count, over session 77's seventeen three-host rows

| host alone against the other two | rows |
|---|---|
| **PCem** | **6** |
| dosbox-x | 4 |
| QEMU | 3–4 |

**PCem is the most frequent sole outlier**, and almost always because it is *the only host
that implements something*: the 8042 self test, the output port, the A20 read-back, the
ICW1 IRR question. QEMU's `pic.ocw3.poll` is the same shape in reverse — it is alone, and
it is the one that is **right**.

> ⇒ **"Sole outlier" does not measure wrongness.** It correlates with implementing
> something the others skipped, in *either* direction. Counting hosts was never the
> method and this is the sharpest demonstration of why.

## Why QEMU differs where it does — three causes, not one

**1. Different firmware, not different silicon — and this is the big one.** The PIT's
counter-0 power-on mode is a *BIOS choice*. So is most of the 78/768-byte VGA register
disagreement: those bytes are what a **VGA BIOS** writes. The 6.22 oracle runs
`qemu-system-i386 -M pc` (i440FX + PIIX) under QEMU 10.2, with **SeaBIOS** and a
Bochs-derived VGA BIOS — reimplementations written decades after the fact. PCem runs a
real AMI 486 BIOS and a genuine IBM VGA ROM. **Same chip, different program's choices.**
These rows say nothing about QEMU's device models at all.

**2. Device models that stop where their workloads stop.** Port `80h` reading `0xFF`,
Input Status 0's Switch Sense bit, the 8042 command set — QEMU's guests are Linux and
NT-family Windows, which never touch any of it. *(That these are deliberate
simplifications is inference; what is measured is only that they are absent.)*

**3. Machine generation — real, but rare, and it points the other way.** Port `92h` is the
clear case, and there it is **PCem** that lacks the feature.

## The framing that actually predicts

The three exist for different reasons, and fidelity follows purpose:

| Oracle | Built to | So it is strong on | And weak on |
|---|---|---|---|
| **QEMU + 6.22** | run modern OSes fast | a genuine Microsoft DOS kernel; whatever SeaBIOS/Linux exercise | period firmware; registers no modern OS reads |
| **dosbox-x** | run DOS games | the surfaces games touch | being any particular machine |
| **PCem** | *be* a specific period machine | chip-level period detail, real ROMs | anything post-dating the machine it is configured as |

⇒ **This is why the abstention rationales ask *why* a host answered, never *how many*
agreed.** A majority vote would have got BCD, the 8259's poll and the whole 8042 wrong —
three surfaces in one session.

---

# 2026-09-23, later still — the 82077AA, where the two oracles finally agreed

The floppy controller is the first surface where **`msdos622` and PCem gave the same
answer to every question that has an answer** — and the disputes that remain are all of
one kind.

## Settled, unanimously, and they are the load-bearing ones

| Row | 6.22/QEMU | PCem | Decision |
|---|---|---|---|
| `fdc.msr.idle` | `0080` | `0080` | **80h is the idle Main Status Register**, exactly as the datasheet writes it. Implemented. |
| `fdc.cmdwait` | `0080` | `0080` | The datasheet's own command-write loop **terminates** on both. Implemented. |
| `fdc.dir.dskchg` | `0000` | `0000` | DSKCHG clear. Implemented. |
| `fdc.version` | `0190` | `0190` | **One result byte, `90h`** — an enhanced 82077AA. Implemented. |
| `fdc.dumpreg` | `0A01` | `0A01` | **Ten result bytes.** Implemented. |
| `fdc.dor.gate` | `000C` | `000C` | /RESET released and DMAGATE through. Implemented. |
| `fdc.alt.3f6` | `0050` | `0050` | `50h` = ATA DRDY\|DSC. **Not the FDC's register** — filed against the IDE/ATA surface rather than "fixed" here. |

## Disputed — and every one of them is *configuration*, not silicon

| Row | 6.22/QEMU | PCem | Why there is nothing to copy |
|---|---|---|---|
| `fdc.dor.raw` | `001C` | `00FF` | **PCem's DOR is write-only**, which is what a genuine PC/AT part does; the 82077AA made it readable. QEMU's `1Ch` is `0Ch` plus **a motor still spinning** from the read that loaded the probe. Two different facts, neither about the chip's answer. The adjudicable bits (3:2) agree. |
| `fdc.dir.raw` | `0000` | `0001` | Bits 6:0 are **not driven** by an AT-mode part. Both hosts drive them anyway and disagree. Ours reads `00h`, recorded as a choice rather than measured. |
| `fdc.sra.srb` | `FFC1` | `FF51` | **SRA agrees at `FFh` on both** — not driven in PC/AT mode, and we match by leaving `3F0h` unclaimed. SRB they both drive, and disagree. Left unclaimed. |
| `fdc.dumpreg` first byte | `01` | `01` | Both say 1 and **we say 0** — it is the present cylinder, i.e. *where the head is*. Their BIOS seeked to cylinder 1 to load the program; our head has never moved because INT 13h does not drive the chip. A real gap, but a **coherence** gap between our two doors onto one drive, not a disagreement about the register. |

> ⇒ **The pattern worth keeping.** Every remaining dispute on this surface is a question
> about *the machine's state at the moment of asking* — which motor is spinning, which
> drive is selected, where the head sits — dressed up as a question about a register.
> The probe's job was to separate those, and the two cases where it failed to
> (`fdc.dor.low` masking `FFh` into a plausible `0Fh`; `fdc.dir.raw` asking about
> undriven bits) were **caught by the oracles disagreeing with each other**, not by us.

## ⛔ And one manufactured agreement, from our own probe

`fdc.dor.low` originally masked DOR to bits 3:0. `FFh & 0Fh` is `0Fh` — out of reset,
gated, drive 3 — which is a **perfectly plausible DOR**. So NTVDMEX, which had no register
there at all, scored `000F` against PCem's `000F` and the row read as a **MATCH**.

> **A mask can manufacture an agreement out of an absent device.** The rule the inventory
> already carries — *a property check passes by luck; pin exact values* — has a second
> half: **check what the absent-device value becomes after your mask.** `FFh` survives
> almost any narrowing as something that looks like data.
