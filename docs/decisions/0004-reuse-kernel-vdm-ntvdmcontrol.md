# ADR-0004: Reuse kernel VDM via `NtVdmControl`

- **Status:** Accepted — Spike-001 (the first spike, vdmhost) ran guest code on the real CPU through `NtVdmControl` (2026-06)
- **Date:** 2026-06-01
- **Deciders:** Matthew

## Context
A usermode process cannot enter V86 on its own. XP's `ntoskrnl` already contains the VDM
machinery NTVDM uses: the `NtVdmControl` syscall, V86 entry/exit, GP-fault (I/O-port,
privileged-instruction) trapping, and interrupt reflection, driven through an undocumented
per-thread `VDM_TIB` and a fixed low-memory layout (first ~1MB+ mapped at vaddr 0). `[BELIEF]`

Two routes to V86:
1. **Reuse** that kernel machinery via `NtVdmControl` (conform to MS's structures).
2. **Custom kernel driver** that sets up V86 and trap handling ourselves.

Critically, **no open-source project validates route 1 from a third-party host**: ReactOS
uses Fast486 emulation and does *not* exercise NT's V86/`NtVdmControl` path. The only existing
precedent is Microsoft's own `ntvdm.exe`. `[FACT]`

## Decision (proposed)
Anchor on **route 1: reuse the kernel VDM via `NtVdmControl`**, pending proof from Spike-001.

## Consequences
- (+) Far less new kernel code; inherits battle-tested fault/interrupt reflection.
- (+) Behaves identically on bare metal and in VMs (kernel abstracts the CPU) — see ADR-0005.
- (−) Hard dependency on **undocumented** structures specific to XP SP3 `ntoskrnl`; risk of
  layout assumptions or sanity checks that resist a non-MS host.
- (−) Reverse-engineering burden: the shipping `ntvdm.exe`/`ntoskrnl` must be studied for
  interoperability (disassembly, used only to learn the interface; nothing of it is recorded).

## Fallback
If Spike-001 shows stock XP rejects a third-party VDM host, **supersede with a custom-driver
ADR** (route 2): more control, more kernel work, more risk, but not blocked by MS's structures.

## Alternatives considered
- **Custom V86 driver (route 2):** held as the fallback.
- **VT-x hypervisor:** out of scope per ADR-0001.
