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
