# NTVDMEX — Documentation

**NTVDMEX** (New Technology Virtual DOS Manager, Extended) is a from-scratch replacement
for `ntvdm.exe` on **Windows XP SP3, 32-bit**, that executes 16-bit code on the **real CPU
via Virtual-8086 (V86) mode** — *not* a software CPU emulator.

## Where things live

| Kind of question | Where |
|---|---|
| **Where is the project now? What next?** | **[STATE.md](STATE.md)** — the one canonical resume point |
| How does it work, and why is it built this way? | **[The wiki](https://github.com/MrMatthewLayton/ntvdmex/wiki)** (source in [`wiki/`](wiki/), published by `tools/wiki/publish.sh`) |
| What is left to do? | **[Issues](https://github.com/MrMatthewLayton/ntvdmex/issues)** — epics and tasks, reconciled against the repo |
| What happened on a given day? | **[log/sessions/](log/sessions/)** — the verbatim session archive |
| Why was *X* decided? | [decisions/](decisions/) |
| What did a specific investigation find? | [research/](research/) |

## How to resume work

1. Read **[STATE.md](STATE.md)**. It says where we are, what works, what does not, and the
   next four actions with issue numbers.
2. If you are picking up a specific thread, read that session in
   **[log/sessions/](log/sessions/)**.
3. Before touching the test rig or trusting any measurement, read the wiki's
   **[Traps and lessons](https://github.com/MrMatthewLayton/ntvdmex/wiki/Traps-and-lessons)**.

When you finish a working session: **update `STATE.md`** and **add
`log/sessions/session-NN.md`**. That contract is what makes resumption reliable — and it is
what broke last time, when `STATE.md` sat three weeks stale while the project moved four
milestones on.

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
