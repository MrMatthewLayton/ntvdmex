# Future work: hardware-accelerated VGA for fast mode-12h

**Status:** Research / future work — NOT planned for the current milestone.
**Date:** 2026-06-09.
**Context:** `docs/STATE.md` "Real DOS apps" section; `docs/log/2026-06-09.md`.

## The problem this would solve

Mode 12h (640×480×16 **planar**) is the one VGA mode that can't run on the real
CPU at full speed in our windowed architecture. The planar write path (latches,
Map Mask, write modes, set/reset, bit mask, ALU) is **hardware logic between the
CPU store and memory**; with no real VGA under the guest we must trap each access
and run that logic in software. Linear modes (13h, VESA) and text have no such
logic — the CPU writes straight to a RAM aperture, so they're already fast.

**The slowness is method-specific, not mode-specific.** Measured 2026-06-09:

- **QuickBasic's runtime** plots one pixel at a time — ~50 instructions **plus a
  per-pixel VGA register `OUT`** — so a fill is hundreds of thousands of traps and
  crawls (BLIT/BOUNCEBX/BUBBLES).
- **`blitfast.com`** (`tools/dostest/blitfast.asm`) draws the *same* random filled
  rectangles the way real 12h software does — write-mode 2 + Map Mask + one
  `REP STOSB` per scanline — which the host batches into **one fault per
  scanline**. VM-confirmed visibly faster.

So well-written 12h software (Windows 3.x, CAD, paint, business apps — all
`REP`-based fills/blits) is **already fast on the existing trap-batching path**.
The "wall" is purely the QuickBasic per-pixel idiom, a pathological minority.
**Nothing below is needed to run real 12h software well.** It would only exist to
make per-pixel-plotting code (QB toys) run at native speed, and/or to provide a
hardware-perfect VGA.

The in-host **batching interpreter** (`src/host/v86interp.h`) was the attempt to
speed up the QB idiom by software; it hit a cost-equivalence wall (interpreting
~50 ops/pixel ≈ the kernel round-trips it removes) and was capped there. The
options below are the only ways to go *faster* than that, and all involve real
VGA silicon.

## Option A — full-screen handoff to the primary adapter (the legacy-NTVDM way)

On a graphics-mode `INT 10h`, switch the real adapter into legacy VGA, map the
guest's `A0000` to the physical aperture, let `OUT`s reach the real CRTC, and hand
the session full-screen ownership. The card's silicon does planar at bus speed —
no traps. This is exactly what NT-era NTVDM did.

- **Pro:** full native 12h/13h speed; hardware-perfect.
- **Con:** **full-screen only** — surrenders the windowed Luna experience that is
  the whole point of NTVDMEX; jarring mode switch on every graphics call.
- **Requires:** display-driver **legacy VGA** support — XPDM drivers have it
  (incl. our Cirrus VM), WDDM (Vista+) do not — *and* reproducing the undocumented
  win32k/CSRSS full-screen DOS handoff, **or** a kernel driver to arbitrate VGA
  ownership. A driver conflicts with the registry-only / no-driver interception
  principle ([[foundational-decisions]], ADR-0007).
- **Viability:** plausible on XP + XPDM; could be an **opt-in** `Display →
  Full-screen (hardware VGA)` mode while the emulated windowed path stays default.

## Option B — dedicated headless secondary VGA as a "planar coprocessor"

Put a **second VGA card** in the machine, with **no display attached**. Route
legacy VGA (`A0000` + ports `3B0–3DF`/`3C0–3CF`) to *that* card; keep the desktop
on the primary card via its own (non-legacy) MMIO framebuffer. The guest's writes
reach the secondary card **natively, no traps**, its silicon does the planar work
into its VRAM, and we **read that VRAM back once per frame** and blit it into the
Luna window on the primary card.

Key realisation that makes it viable: the VGA **write engine operates on memory
writes, not on scan-out** — so a *headless* card (no CRTC scanning to a monitor)
still does planar writes into its VRAM correctly. We never need it to drive a
display; we only need its memory + write logic, then we snapshot it.

- **Pro:** the best of both — **keeps the window** (primary card) *and* gets
  **hardware-speed, hardware-perfect VGA** for the guest (text/13h/12h/VESA, exact
  timing, **zero device emulation**). Resolves the windowed-vs-silicon conflict by
  splitting the two roles across two cards instead of time-sharing one.
- **Con / requires:**
  1. A **kernel driver** to own the secondary card, put it in legacy VGA mode, and
     hand its aperture + ports to the V86 guest (breaks the no-driver principle).
  2. **Remapping the V86 guest's `A0000` to the secondary card's physical
     aperture** — `NtVdmControl` maps `A0000` to our RAM today; pointing it at
     device memory is a kernel page-table operation the kernel VDM owns, so we'd
     have to override what the monitor sets up. This is the deepest piece.
  3. **Native port pass-through** (IOPM) so VGA `OUT`s hit the secondary card.
  4. **PCI VGA arbitration** configured to route legacy cycles to the secondary
     card persistently.
  5. **Per-frame VRAM read-back → present** (reading planar VRAM is awkward but
     once-per-frame, not per-pixel).
- **Hardware reality:** needs a secondary card that cooperates as a legacy-VGA
  target — an **old PCI VGA card** likely will; a **modern PCIe GPU** generally
  won't; so this is a *specific-rig* solution, not portable.
- **Can't prototype in the dev VM:** QEMU routes legacy VGA only to the *primary*
  VGA; secondary display devices don't receive legacy cycles. **Real-hardware-only**
  experiment. The make-or-break first question is purely hardware: *can legacy VGA
  arbitration be routed to a headless secondary card on the target machine?* —
  everything else is downstream of that.

## Other directions (noted for completeness)

- **JIT / optimised interpreter hot path** — make the in-host interpreter fast
  enough per instruction that batching QB's ~50-ops/pixel beats the trap cost.
  Big effort, and philosophically the furthest from "runs on the real CPU."
- **Pattern-recognise QB's putpixel routine and shortcut it** — detect the
  specific runtime routine signature and replace it with a native planar fill.
  Fragile, QuickBasic-version-specific; what the design brief calls "recognising
  another planar write idiom in the fast path."
- **GPU-shader present** (R8 + palette LUT for 13h; plane textures + shader-combine
  for 12h) — this is a *presentation*-side optimisation (present cost / scaling),
  **orthogonal** to the per-pixel draw wall; it would not help the QB case, which
  is bottlenecked upstream of present. Worth it only if windowed present ever
  becomes the bottleneck (it currently isn't).

## Decision

**Not building any of these now.** The windowed, software-emulated path is the
right default: it runs all 10 demos, keeps the Luna window, ships no driver, and
runs real `REP`-based 12h software fast. Per-pixel-plot speed (QB toys) is a known,
documented corner that doesn't justify a driver-bound, hardware-specific subsystem.

If hardware-speed 12h ever becomes a real requirement, **Option A as an opt-in
full-screen mode** is the smallest step; **Option B** is the most elegant
(window + hardware VGA together) but the largest and most hardware-dependent.
