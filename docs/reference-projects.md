# Reference projects — what we may read, and what we must not

NTVDMEX is a **public, clean-room** project. That is not a slogan: it has already cost
real work to maintain (the OPL synth is clean-room MIT with Nuked used only as a
black-box oracle; the repository's *history* was rewritten to remove `DOOM1.WAD` before
going public). This page exists so that a future session evaluating some newly-discovered
NTVDM project does not have to re-derive the rule under time pressure.

Tracked by **R8** in [`risks.md`](risks.md).

---

## The rule

| | |
|---|---|
| ✅ **Behaviour** | What a program *does*, measured. Register values, byte dumps, timings, error strings. Facts about an interface are not copyrightable. |
| ✅ **Published specifications** | DPMI 0.9/1.0, RBIL, Microsoft KB articles, OS ABI layouts. |
| ✅ **Our own disassembly for interoperability** | Reading `krnl386.exe` to learn what it demands of a host. This is what most of session 30 was. |
| ⚠️ **Clean-room reimplementations under a copyleft licence** | Readable as *documentation of semantics*. Do not copy expression. Same discipline as Nuked-OPL. |
| ❌ **Leaked Microsoft source, or anything derived from it** | Including patch files against it — a patch quotes its context. |

The last row is the one that matters, and the reason is worse than a licence violation.
A licence problem can be fixed by deleting a file. **Provenance taint cannot**: it lives in
what the implementer knew, it cannot be purged from history the way a WAD file could, and
it would attach to the entire WOW subsystem — the newest and least replaceable part of
this project.

---

## Verdicts

### ❌ `leecher1337/ntvdmx64` — DO NOT READ `ntvdmpatch/`

Runs Microsoft's real NTVDM on 64-bit Windows. Genuinely impressive, and the closest
thing to "someone else has already solved our problem" that exists.

**It is off limits.** `ntvdmpatch/` is patch files against the **leaked Windows NT 4
source**, © Insignia and Microsoft. There is no `LICENSE` file, and the author's own
statement is a hope rather than a licence: *"publishing patches shouldn't be a copyright
issue, I hope."*

Two things are safe and were the only things read here: the `readme` prose (his own
writing) and the repository's directory listing.

⚠️ **Do not "just look" at a patch hunk to check one constant.** That is exactly how this
goes wrong, and the surrounding context lines are the leaked source.

**And we would gain little.** Its whole premise is that x64 has no V86 mode, so it
emulates the CPU (SoftPC's CCPU) and injects AppInit DLLs. We run **real V86 on real
silicon** through `NtVdmControl` — the harder thing, already working. As an *oracle* it
would be strictly worse than what we already have: stock `ntvdm` on the XP box is the
actual Microsoft implementation, running the actual Microsoft `krnl386`, on the actual
target OS.

*Salvageable facts, both merely observational:* WOW32 support is separable from DOS
support; NTVDM's DPMI memory is registry-configurable beyond the 64 MB `.PIF` ceiling.
Verify on our own box before believing either.

### ⚠️ `otya128/winevdm` (OTVDM) — usable as an oracle, wrong subject for now

GPL-2.0, built on Wine's clean-room Win16 work. Legally far cleaner: read it for
*semantics*, never copy expression, exactly as [Nuked was used for OPL](research/).

Two caveats:

1. **It replaces `krnl386` with Wine's own reimplementation** — it never loads Microsoft's
   `krnl386.exe`. So it cannot answer any question of the form *"what does Microsoft's
   krnl386 expect from its host?"*, which is every question we currently have.
2. GPL into an MIT project means black-box discipline, not copy-paste.

**Where it will earn its keep:** *after* krnl386 boots, when the questions become Win16
semantics rather than NTVDM internals — `krnl386/ne_module.c` (NE module handling,
moveable/discardable segments, entry thunks) and `krnl386/selector.c` (selector
management) are the direct analogues of where `src/wow/ne.h` is heading.

Incidental confirmation: it notes 64-bit Windows cannot modify the LDT, which is why its
approach differs from ours. Our LDT-based design is inherently XP-bound — which is fine,
that is the target.

### ✅ ReactOS — the existing second opinion

Already used, and correctly: the `VDM_COMMAND_INFO` layout came from here and is an **OS
ABI**, not an expressive work. Clean-room by policy. Its own NTVDM reimplementation covers
DPMI and BIOS and is the natural place to look for a second opinion on structure layouts.

### ✅ Published specifications

DPMI 0.9 spec, RBIL, Microsoft KB. Note that RBIL is confirmed against an **executable
oracle** before becoming a test expectation — that is the M9 cardinal rule and it is not
negotiable, because RBIL is a compilation of other people's reports.

---

## What the reference projects could *not* tell us

Checked during session 30, against our two open unknowns:

| Question | Public documentation | Source of truth |
|---|---|---|
| `INT 31h 04F3` | **None.** Not in the DPMI 0.9 spec, not in RBIL. It falls in the undocumented "true DPMI" space Windows implements beyond the published 0.9 surface. | rig: stock `ntvdm` |
| SysVars `+0x6A` WOW block | **None.** Not documented anywhere; past the documented end of the DOS list of lists. | rig: stock `ntvdm` (measured — see session 30 part 7) |
| krnl386's error strings | Microsoft KB Q220155 *"Troubleshooting NTVDM and WOW Startup Errors"* documents **no message text at all** — it is a troubleshooting flowchart. | the binary itself, `seg1:0xb9a9` |

★ **This is the finding, and it is a positive one.** The things blocking us are precisely
the things nobody has written down, which is why the rig-as-oracle method is not a
fallback here — it is the only source. It has now answered three of them (the launch
shape, SysVars+0x6A, and the `168A` vendor API) and no external project would have.
