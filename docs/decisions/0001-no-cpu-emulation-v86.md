# ADR-0001: Execute real-mode code in V86, no CPU emulation

- **Status:** Accepted
- **Date:** 2026-06-01
- **Deciders:** Matthew

## Context
The core product premise is "actual CPU execution — no CPU emulation." On 32-bit x86 this is
not exotic: it is exactly how Microsoft's own NTVDM works. NTVDM runs 16-bit real-mode code in
the CPU's **Virtual-8086 (V86) mode** on real silicon; the Insignia/SoftPC software emulator
was only ever used on non-x86 NT (MIPS/Alpha) and on 64-bit Windows where long mode removed
V86. `[FACT]`

## Decision
NTVDMEX executes guest 16-bit code in **V86 mode on the host CPU**. No interpreter / software
CPU core. Non-CPU concerns (video, sound, input, net, timers) are *virtualized* and serviced
by host calls — that is not "CPU emulation" and is permitted by the premise.

## Consequences
- (+) True to the product goal; native performance for guest code.
- (+) Aligns with the platform's native mechanism, so the kernel already has the plumbing.
- (−) A usermode process cannot enter V86 by itself; this forces a dependency on kernel
  support (see ADR-0004).
- (−) V86 only exists in 32-bit protected mode — hard-locks us to 32-bit XP (acceptable; that
  is the stated target).
- Protected-mode guest code (DPMI/DOS-extender, and Win16) is **not** V86 — it runs in real
  protected mode with descriptor/segment management we must provide. V86 covers the real-mode
  portion only.

## Alternatives considered
- **Software CPU emulation** (ReactOS Fast486, DOSBox): rejected — violates the premise, and
  is the thing we explicitly do not want.
- **Hardware virtualization (VT-x) real-mode guest:** rejected for now — XP-era CPUs may lack
  VT-x, and running a hypervisor under XP is a separate large undertaking.
