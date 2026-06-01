# NTVDMEX — Documentation

**NTVDMEX** (New Technology Virtual DOS Manager, Extended) is a from-scratch replacement
for `ntvdm.exe` on **Windows XP SP3, 32-bit**, that executes 16-bit code on the **real CPU
via Virtual-8086 (V86) mode** — *not* a software CPU emulator.

This `docs/` tree is the project's source of truth. It is designed so that work can be
**paused and resumed across sessions/contexts** without loss of state.

## How to resume work (read this first every session)

1. Read **[STATE.md](STATE.md)** — the living handoff doc. It says where we are, what is
   decided, what is open, and the single next action.
2. Skim the most recent entry in **[log/](log/)** for the last session's narrative.
3. Check **[ROADMAP.md](ROADMAP.md)** for the current milestone.
4. Then continue.

When you finish a working session, **update `STATE.md`** and **append a dated entry to
`log/`**. These two steps are the contract that makes resumption reliable.

## Layout

| Path | Purpose |
|------|---------|
| [STATE.md](STATE.md) | Living handoff / "pick up where we left off" — the canonical resume point |
| [ROADMAP.md](ROADMAP.md) | Milestones and phases |
| [risks.md](risks.md) | Risk register (likelihood × impact, with mitigations) |
| [GLOSSARY.md](GLOSSARY.md) | NTVDM / VDM / WOW / V86 terminology |
| [decisions/](decisions/) | Architecture Decision Records (ADRs) — *why* we chose things |
| [research/](research/) | Research findings, with confidence levels and sources |
| [spikes/](spikes/) | Time-boxed experiments: hypothesis → method → result |
| [log/](log/) | Chronological journal: work done, learnings, surprises |

## Conventions

- **Confidence tags** in research/decisions: `[FACT]` (verified), `[BELIEF]` (high
  confidence, unverified), `[VERIFY]` (assumption that must be confirmed before we rely on it).
  Be honest about which is which — this project rests on undocumented behaviour.
- **ADRs are immutable once Accepted.** To change one, supersede it with a new ADR and mark
  the old one `Superseded by ADR-XXXX`.
- **Dates are absolute** (YYYY-MM-DD), never "today"/"last week".

## GitHub mapping

The repo is **private on the free plan — no Wiki**. Therefore `docs/` is the **sole, canonical
knowledge base**; everything lives in-repo as Markdown. Do not assume a Wiki exists.

What still maps onto GitHub primitives:
- `roadmap` milestones → GitHub **Milestones**; each phase task → an **Issue**.
- `spikes/` → spike **Issues** labelled `spike`, with results recorded back here in `docs/`.
- CI in **Actions** (build the driver + usermode host, run the spike harness).
- ADRs, research, logs, and `STATE.md` → stay as files in `docs/` (the source of truth).
