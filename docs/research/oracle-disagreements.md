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
