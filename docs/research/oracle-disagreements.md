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
| `pit.bcd.valid` | 6.22 `0`, **PCem `0`**, dosbox-x `1`. Two of three — including the real-BIOS machine — do not model BCD, so **no oracle can verify it**. The datasheet is unambiguous that BCD exists, so implement from the spec and record it as *spec-implemented, unverifiable*, with an abstention rule so the sweep does not read it as a regression. |

## New gaps this run found — the external registers

PCem is the first oracle here with a genuine BIOS, and two registers we answer with `0x00`
are not `0x00` on it:

| Register | ours | 6.22 | dosbox-x | **PCem** |
|---|---|---|---|---|
| **Input Status 0** (`3C2` read) | `0x00` | `0x00` | `0x60`/`0x70` | **`0x10`** |
| **Feature Control** (`3CA` read) | `0x00` | `0x00` | `0x70` | **`0xFF`** |

`0x10` is **bit 4, Switch Sense** — the monitor-ID sense line, which a real card drives and
we do not. All four oracles disagree, so this needs its own probe rather than a guess, but
`0x00` is now known to be wrong for a period-correct machine.
