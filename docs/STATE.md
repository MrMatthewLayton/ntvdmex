# Project State — Living Handoff

> **This is the canonical resume point.** Update it at the end of every working session.

- **Last updated:** 2026-06-01
- **Phase:** M0 — Feasibility (pre-code)
- **Overall status:** 🟡 Premise validated on paper; the one make-or-break assumption is
  not yet tested in practice.

## One-paragraph summary

We are building a drop-in replacement for Windows XP SP3 (32-bit) `ntvdm.exe` that runs
16-bit DOS and Win16 code on the **real CPU in V86 mode** by driving XP's existing kernel
VDM machinery through the undocumented `NtVdmControl` syscall. Interception is done by
**repointing the WOW registry keys** at our binary, so no signed system file is replaced.
Devices (video, sound, input, net) are virtualized and serviced by host calls through a
pluggable VDD model; video is blitted into a Luna-themed window.

## What is decided (see [decisions/](decisions/))

- **ADR-0001** — No CPU emulation; execute real-mode code in V86. *Accepted.*
- **ADR-0002** — Intercept via WOW registry repoint, not binary replacement (avoids WFP). *Accepted.*
- **ADR-0003** — Scope = DOS **+** Win16/WOW; Win16 deferred to M5 behind a clean seam. *Accepted.*
- **ADR-0004** — Reuse kernel VDM via `NtVdmControl` rather than a custom V86 driver. *Proposed — contingent on Spike-001.*
- **ADR-0005** — Target XP in a VM **and** bare metal; VM-first for the dev loop. *Accepted.*

## What is open / unresolved

- **THE keystone question:** Can a *non-Microsoft* binary fully drive *real XP SP3*
  `ntoskrnl`'s VDM via `NtVdmControl`, conforming to the undocumented VDM_TIB / address-space
  layout it expects? No open-source project proves this (ReactOS uses CPU emulation, not V86).
  → tracked as **[Spike-001](spikes/spike-001-v86-keystone.md)**.
- Reference posture: lean on ReactOS for DOS/VDD/WOW *logic & structures*; use disassembly
  of the shipping XP `ntvdm.exe`/`ntoskrnl` for the V86/`NtVdmControl` contract; use Linux
  `dosemu` as the conceptual analog. (See [research/reference-projects.md](research/reference-projects.md).)

## Single next action

**Run Spike-001** (V86 keystone): a ~200-line usermode host that registers via the WOW
repoint, reserves the low address space, calls `NtVdmControl(VdmInitialize)` →
`VdmStartExecution`, and executes one trivial real-mode program under real V86 with the
GP-fault reflection landing in our handler. If it works → promote ADR-0004 to Accepted and
proceed to M1. If it fails → pivot to the custom-driver path (new ADR).

## Environment notes

- No git repo yet (`git init` pending). User may create a GitHub repo for issues/actions/wiki.
- Dev target: XP SP3 32-bit guest in a VM first; validate on bare metal before M1 exit.
