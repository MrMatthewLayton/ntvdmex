# NTVDMEX — Documentation

**NTVDMEX** (New Technology Virtual DOS Manager, Extended) is a from-scratch replacement for
`ntvdm.exe` on **Windows XP SP3, 32-bit**, that executes 16-bit code on the **real CPU via
Virtual-8086 mode** — *not* a software CPU emulator.

## Where things live

| Kind of question | Where |
|---|---|
| **Where is the project now? What next?** | **[STATE.md](STATE.md)** — the one canonical resume point |
| **What does the hardware actually do?** | **[ref/](ref/)** — the specifications, and [ref/SOURCES.md](ref/SOURCES.md) for where each one lives |
| **What do *we* implement of it?** | **[inventory/](inventory/)** — every surface, every unit, marked from the code |
| How does it work, and why is it built this way? | **[The wiki](https://github.com/MrMatthewLayton/ntvdmex/wiki)** (source in [`wiki/`](wiki/), published by `tools/wiki/publish.sh`) |
| What is left to do? | **[Issues](https://github.com/MrMatthewLayton/ntvdmex/issues)** |
| What happened on a given day? | **[log/sessions/](log/sessions/)** — the verbatim archive |
| Why was *X* decided? | [decisions/](decisions/) — ADRs |
| What did a specific investigation find? | [research/](research/) |
| What may we legally read? | [reference-projects.md](reference-projects.md) — **read before evaluating any NTVDM project** |

## How to resume work

1. Read **[STATE.md](STATE.md)**. It says where things are, what works, what does not, and the
   next actions.
2. Before touching the test rig or trusting any measurement, read its **Standing hazards**
   section and the wiki's
   [Traps and lessons](https://github.com/MrMatthewLayton/ntvdmex/wiki/Traps-and-lessons).
3. If you are picking up a specific thread, read that session in
   **[log/sessions/](log/sessions/)**.

**When you finish a session:** update `STATE.md` and add `log/sessions/session-NN.md`.

> ⚠ **Keep STATE.md short.** Session notes go in the archive, not in STATE.md. That file has
> twice grown into a multi-thousand-line rolling narrative — once as `return-ntvdm.md`, once
> as STATE.md itself — and both times the fix was the same split. `tools/docs/split_state.py`
> exists because it had to be done twice.

## The working method: build from the specs, then test the apps

Gaps are implemented **by what we are building, not by what is asking for them**. A device is
in the inventory because it is part of the period-correct hardware contract, never because a
guest asked for it. Applications are the acceptance test, run after.

That gives two documents per surface, doing two different jobs:

- **[`ref/<surface>.md`](ref/)** — what the hardware does. Our own words, derived from the
  source documents and cited per section, so it is wiki-ready and useful to a collaborator who
  has never seen the part.
- **[`inventory/<surface>.md`](inventory/)** — what we do. Every unit marked
  **IMPL / PART / STORE / MISS / N/A** from the code with a `file:line`, plus whether anyone
  has actually compared it against an oracle.

See [inventory/README.md](inventory/README.md) for the full surface list and the method.

## Layout

| Path | Purpose |
|------|---------|
| [STATE.md](STATE.md) | The canonical resume point. Kept short on purpose. |
| [ROADMAP.md](ROADMAP.md) | Milestones and phases |
| [GLOSSARY.md](GLOSSARY.md) | NTVDM / VDM / WOW / V86 terminology |
| [ref/](ref/) | **Specifications** — held documents, and the index of every source |
| [inventory/](inventory/) | **Coverage** — what is implemented of each surface, marked from the code |
| [reference-projects.md](reference-projects.md) | Clean-room policy: what we may read, and what we must not |
| [decisions/](decisions/) | Architecture Decision Records — *why* we chose things |
| [research/](research/) | Investigations and their evidence, tagged by confidence |
| [spikes/](spikes/) | Time-boxed experiments: hypothesis → method → result |
| [log/sessions/](log/sessions/) | The verbatim session archive |
| [wiki/](wiki/) | Source of the published GitHub wiki |
| [sdk/](sdk/) | The VDD SDK |

## Conventions

- **Confidence tags** in research and decisions: `[FACT]` (verified), `[BELIEF]` (high
  confidence, unverified), `[VERIFY]` (assumption that must be confirmed before we rely on it).
  Be honest about which is which — this project rests on undocumented behaviour.
- **ADRs are immutable once Accepted.** To change one, supersede it with a new ADR and mark the
  old one `Superseded by ADR-XXXX`.
- **Dates are absolute** (YYYY-MM-DD), never "today" or "last week".
- **The session archive is verbatim.** Refuted conclusions stay, because the refutation is only
  legible if the thing it refutes is still there.
- **Numbers are re-run, never quoted.** Any score, sweep percentage or battery count written
  into a document is stale from the moment it is typed.

## Retired documents

Removed 2026-09-23, recorded here so a stale link has an answer:

| Was | Now |
|---|---|
| `PARITY.md` | Split into [inventory/](inventory/) — same data, one vocabulary, one home |
| `PLAN-17th.md` | Delivered. Its oracle list became [ref/SOURCES.md](ref/SOURCES.md) |
| `PROGRESS.md` | A daily score table that went two weeks stale. Run `tools/score/score.py` |
| `risks.md` | R1–R7 closed by shipped work; R8 lives on in [reference-projects.md](reference-projects.md) |
| `log/README.md`, `log/2026-06-*.md` | Folded into [log/sessions/](log/sessions/) — one archive, not two |
| `research/reference-projects.md` | Merged into [reference-projects.md](reference-projects.md) |
