# ADR-0008: Pluggable VDD device model (clean ABI + MS-compat veneer; DirectDraw presentation)

- **Status:** Accepted
- **Date:** 2026-06-07
- **Deciders:** Matthew
- **Relates to:** M3 (device model + video); requirement #13 (third-party VDD hook point).
  Design detail: [research/vdd-architecture.md](../research/vdd-architecture.md).

## Context
M2 closed with the DOS kernel running in V86 from the clean `src/` host (`ntvdmhost.exe`). M3 adds
devices — video, timer, sound, input — and the project owner's requirement is that NTVDMEX be
**highly pluggable**: every device a VDD behind one interface, with graphics rendered by
**DirectDraw** in **windowed and full-screen** modes, supporting **VGA and VESA**.

Research into the real NT/XP VDD interface establishes:
- Microsoft's **documented** third-party VDD ABI (`vddsvc.h`) is small and stable, and the entire
  in-the-wild ecosystem (VDMSound, SoundFX/VSBD lineage, GPIB-VDD, NetFOSS) depends on the *same*
  core six concepts — I/O hook, memory hook, flat-map, alloc, register get/set, simulated
  interrupt — plus the `RegisterModule` BOP (`C4 C4 58 nn`), loaded via the
  `HKLM\System\CCS\Control\VirtualDeviceDrivers` `VDD` `REG_MULTI_SZ` value.
- **Video and the PIT timer were never a public VDD ABI** — they were internal to `ntvdm.exe`. So
  we owe **no binary compatibility** for them and may design cleanly.

## Decision
Adopt a **hybrid, two-ABI device model over one internal bus**:

1. **A clean internal plugin ABI (`src/vdd/ntvdd.h`)** that all our own devices use — a struct of
   callbacks + a *bus* handed at init that exposes: claim-ports, claim-memory-window, claim-interrupt,
   raise-IRQ, per-frame tick, seg:off→flat map, and present-frame. Register access is **explicit via
   the passed CPU context** (no global mutable get/set macros), preserving off-VM testability.
2. **A thin `vddsvc.h` binary-compat veneer (post-M3, M7)** layered over the same bus, so existing
   third-party Microsoft VDDs load and run unmodified. Built only when the audience (sound, comms,
   GPIB) is reached.
3. **Video is a built-in VDD** owning emulated VGA/VESA state, producing a framebuffer.
4. **Presentation is DirectDraw 7** into a host-owned GUI window: the VDM host becomes a **GUI app
   that owns its window**, and the video VDD renders **all** modes — text *and* graphics — into it.
   The console-screen-buffer path is abandoned; `ntvdmhost` (console DOS engine) and `ntvdmex` (Luna
   GDI window) **merge** into one windowed host.

## Consequences
- (+) **One seam for every device** — the host core stays device-agnostic; new devices are additive.
- (+) **Captures the whole third-party ecosystem** at modest, deferrable cost (the veneer), while our
  own devices use a clean, testable ABI — the hybrid dominates either pure option.
- (+) **DirectDraw** gives windowed + fullscreen + palettised 8bpp + stretch for free; ships on XP
  SP3; bound via `LoadLibrary`+`GetProcAddress` with an inline `IID_IDirectDraw7`, so the **no-CRT
  minimal-import rule holds** (no `-lddraw`/`-ldxguid`).
- (+) Unifying the two binaries kills the "two presentation paths" problem — text mode 3 is just a
  framebuffer render (embedded 8×16 VGA font), same sink as mode 13h.
- (−) The host gains a **window + message loop + DirectDraw lifecycle** (lost-surface recovery,
  windowed↔fullscreen toggle, Win16-lock discipline) — real complexity, isolated in the presentation
  layer.
- (−) `src/console.c` / `src/main.c` (the M0 Luna preview) get **absorbed** into the video VDD +
  presentation layer rather than reused as-is.
- (−) Emulating VGA faithfully (esp. mode 12h planar latches/write-modes) is substantial; mitigated
  by the ruthless priority order (text → 13h → VESA-banked → planar last) and deferring planar/LFB.

## Alternatives considered
- **Reimplement only the MS `vddsvc.h` ABI** (pure binary-compat): rejected — its global register-
  macro model fights off-VM testability and forces video/PIT through an ABI MS never exposed them on.
- **Pure clean ABI, no MS compat ever:** rejected — needlessly abandons the entire existing
  third-party VDD ecosystem (VDMSound et al.) the veneer captures cheaply.
- **Keep the console for text, DirectDraw only for graphics** (two presentation paths, like stock
  ntvdm's window-text/fullscreen-graphics split): rejected — it's the exact limitation that made
  stock ntvdm refuse windowed graphics; one framebuffer path is simpler and meets the windowed-VGA
  requirement.
- **GDI/Direct2D instead of DirectDraw:** rejected — owner specified DirectDraw; it's also the best
  fit for palettised 8bpp + fullscreen mode-set on XP-era hardware.
</content>
