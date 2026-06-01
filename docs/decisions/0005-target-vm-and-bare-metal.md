# ADR-0005: Target XP in VM and bare metal; VM-first dev loop

- **Status:** Accepted
- **Date:** 2026-06-01
- **Deciders:** Matthew

## Context
The product must run on real XP-era hardware *and* in XP-32 VMs. V86 is a feature of 32-bit
protected mode and is faithfully exposed by mainstream hypervisors to a 32-bit guest, so the
**CPU-execution layer is essentially environment-agnostic** — especially because we delegate
V86 setup to XP's kernel rather than touching hardware (ADR-0004). Portability bites mainly at
the **device layer** (real sound/NIC/video silicon vs. virtualized devices), which is exactly
where the pluggable VDD model absorbs the difference.

## Decision
Support **both** VM and bare metal. Use **XP-32 in a VM as the primary development loop**
(snapshots, fast reset, debugger attach), and **validate on bare metal before exiting each
milestone** that touches hardware-sensitive areas (video full-screen, timers, peripherals).

## Consequences
- (+) Fast, safe iteration; spike results transfer to hardware unchanged at the CPU layer.
- (−) Two test surfaces; device milestones (M3, M7) need explicit bare-metal validation passes.
- (−) Some "bare-metal" device ambitions (direct VGA/VESA) may be infeasible regardless of
  host — tracked in risks.md and to be resolved at M3.

## Alternatives considered
- **VM-only** / **bare-metal-only:** both rejected — the requirement is explicitly "both".
