# NTVDMEX -- Documentation

**NTVDMEX** is a from-scratch replacement for `ntvdm.exe` on **Windows XP SP3, 32-bit**. It
runs 16-bit code on the **real CPU, in Virtual-8086 mode** -- not in a software CPU emulator.

## Start here

| If you want to... | Read |
|---|---|
| Install it and run programs | [quick-start.md](quick-start.md) |
| Build it and run the tests | [building.md](building.md), then [testing.md](testing.md) |
| Understand how it is put together | [architecture.md](architecture.md) |
| Know why it is built this way | [motivations-and-decisions.md](motivations-and-decisions.md) and the [decisions/](decisions/) |
| Avoid the expensive mistakes | [lessons.md](lessons.md) |
| Contribute code | [STYLE.md](STYLE.md) and [CLEAN-ROOM.md](../CLEAN-ROOM.md) |
| Find something to work on | the [issue tracker](https://github.com/MrMatthewLayton/ntvdmex/issues) |

## The working method: build from the specs, then test the apps

Gaps are filled **by what the machine is, not by what a program asks for**. A device is
implemented because it is part of the period-correct hardware, never only because one guest
needed it. Programs are the acceptance test, run afterwards.

That gives two documents per surface, doing two different jobs:

- **[`ref/<surface>.md`](ref/)** -- what the hardware does, in our own words, derived from the
  source documents and cited per section. [ref/SOURCES.md](ref/SOURCES.md) says where each
  source lives.
- **[`inventory/<surface>.md`](inventory/)** -- what NTVDMEX does of it. Every unit marked
  **IMPL / PART / STORE / MISS / N/A** from the code, plus whether it has been compared with
  an oracle.

See [inventory/README.md](inventory/README.md) for the list of surfaces and the method.

## Layout

| Path | What it holds |
|---|---|
| [quick-start.md](quick-start.md) | Installing, running, shortcuts, troubleshooting |
| [building.md](building.md) | The toolchain, the build, configuration |
| [testing.md](testing.md) | The off-machine battery, probes and oracles, real-hardware testing |
| [architecture.md](architecture.md) | How the host, the VDM layer, DOS, the devices and WOW fit together |
| [motivations-and-decisions.md](motivations-and-decisions.md) | The big decisions and what they cost |
| [lessons.md](lessons.md) | Traps and lessons |
| [STYLE.md](STYLE.md) | The code style |
| [ROADMAP.md](ROADMAP.md) | Milestones and phases |
| [GLOSSARY.md](GLOSSARY.md) | NTVDM / VDM / WOW / V86 terminology |
| [EMULATION.md](EMULATION.md) | What is emulated, and what runs on the CPU |
| [ref/](ref/) | Specifications, and the index of every source |
| [inventory/](inventory/) | Coverage: what is implemented of each surface |
| [decisions/](decisions/) | Architecture Decision Records |
| [reference-projects.md](reference-projects.md) | Clean-room policy: what may be read, and what must not |
| [sdk/](sdk/) | The device SDK |

## Conventions

- **Confidence tags**: `[FACT]` (verified), `[BELIEF]` (high confidence, unverified),
  `[VERIFY]` (an assumption that must be confirmed before anything relies on it). This
  project rests on undocumented behaviour, so say which is which.
- **ADRs are immutable once Accepted.** To change one, supersede it with a new ADR and mark
  the old one `Superseded by ADR-XXXX`.
- **Dates are absolute** (YYYY-MM-DD), never "today" or "last week".
- **Numbers are re-run, never quoted.** A score or a test count written into a document is
  stale from the moment it is typed.
