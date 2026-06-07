# VDD architecture — the pluggable device model (M3)

> **Status:** design, M3 kickoff (2026-06-07). Decision of record: [ADR-0008](../decisions/0008-pluggable-vdd-model.md).
> This is the load-bearing M3 document — it defines the seam every device (video, timer,
> sound, input, …) plugs into, and how the existing console host grows a DirectDraw window.

## 1. Goal

NTVDMEX must be **highly pluggable**: video, sound, the PIT timer, input, and future devices are
all **VDDs** (Virtual Device Drivers) behind one stable in-process interface. The host core knows
nothing device-specific — it owns the V86 CPU loop, the DOS kernel, and a *bus* that routes
hardware events (I/O ports, memory-window faults, interrupts, BOPs, frame ticks) to whichever VDD
claimed them. Devices ship as built-in modules now and as separate DLLs later (the SDK, M8).

Concrete M3 device requirements (from the project owner):
- **Graphics** rendered by **DirectDraw**, in **windowed *and* full-screen** modes, themed Luna
  when windowed.
- **VGA** (text + mode 13h + planar) **and VESA** (VBE 2.0) display modes.
- **Sound** and **PIT timer** are VDDs too — the model must generalise beyond video on day one.

## 2. The core insight: two ABIs, one bus

Research into the real NT/XP VDD interface (see §8 sources) gives a clear steer:

- Microsoft's **documented** third-party VDD ABI (`vddsvc.h`) is **small, stable, and the entire
  real-world ecosystem depends on the same six concepts** — I/O hook, memory hook, flat-map,
  alloc, register get/set, simulated interrupt — plus the `RegisterModule` BOP. The high-value
  binaries (VDMSound, SoundFX/VSBD-lineage, GPIB-VDD, NetFOSS) all use exactly that subset.
- **Video and the PIT are *not* part of any public VDD ABI** — they were always internal to
  `ntvdm.exe`. So for those we owe **no binary compatibility** and are free to design clean.

That yields a **hybrid**: a **clean internal plugin ABI** (`ntvdd.h`) that all our own devices use,
with a thin **`vddsvc.h` binary-compat veneer** layered on top for loading third-party MS VDDs
later. Both terminate in the same internal **device bus**. (Full rationale + the rejected
alternatives — pure clean ABI, pure MS-compat — are in [ADR-0008](../decisions/0008-pluggable-vdd-model.md).)

```
            ┌──────────────────────────────────────────────────────────┐
            │  host core: V86 loop · DOS kernel (INT 21h) · device bus   │
            └───────────────┬───────────────────────────┬──────────────┘
                            │ ntvdd.h (clean ABI)        │ vddsvc.h veneer (M7+)
        ┌───────────────────┼───────────────────┐       │
        ▼                   ▼                   ▼        ▼
   ┌─────────┐        ┌──────────┐        ┌────────┐  ┌─────────────────┐
   │ vdd_pit │        │ vdd_video│        │vdd_snd │  │ 3rd-party MS VDD │
   │ (timer) │        │ VGA+VESA │        │ (stub) │  │ (VDMSound, …)    │
   └─────────┘        │ DirectDraw└        └────────┘  └─────────────────┘
                      ▼ frame sink
                 ┌──────────────────────────────┐
                 │ presentation: window / fullscr│
                 └──────────────────────────────┘
```

## 3. The clean VDD ABI (`src/vdd/ntvdd.h`)

A VDD is a struct of callbacks plus a registration call against the bus it is handed at init. No
global mutable register macros (unlike MS `vddsvc.h`); register access is explicit through the
passed CPU context, which keeps VDDs testable off-VM (the same discipline that made `dos_mcb.h`
unit-testable).

```c
/* One device. All callbacks optional (NULL = not interested). */
typedef struct ntvdd {
    const char *name;                       /* "video", "pit", "sound"          */
    int  (*init)(struct ntvdd_bus *bus, void *self);   /* claim hooks here       */
    void (*reset)(void *self);              /* DOS process start / mode reset    */
    void (*shutdown)(void *self);
    void *self;                             /* device instance state             */
} ntvdd;

/* The bus: what a VDD may ask the host to do. Handed to init(). */
typedef struct ntvdd_bus {
    /* I/O-port trapping (IN/OUT dispatch). lo..hi inclusive. */
    void (*claim_ports)(void *h, uint16_t lo, uint16_t hi,
                        void (*in)(void *self, uint16_t port, uint8_t  w, uint32_t *val),
                        void (*out)(void *self, uint16_t port, uint8_t w, uint32_t  val),
                        void *self);
    /* Memory-window trapping (e.g. A0000/B8000). Flat guest addresses. */
    void (*claim_mem)(void *h, uint32_t base, uint32_t size,
                      uint8_t (*rd)(void *self, uint32_t off),
                      void    (*wr)(void *self, uint32_t off, uint8_t v),
                      void *self);
    /* Software-interrupt hook (e.g. INT 10h for video, 1Ah for timer). */
    void (*claim_int)(void *h, uint8_t vec,
                      void (*svc)(void *self, ntvdd_regs *r), void *self);
    /* Raise a hardware IRQ into the emulated PIC/ICA (timer→IRQ0, etc.). */
    void (*raise_irq)(void *h, uint8_t irq);
    /* Per-frame tick (~60 Hz) so a device can present / advance. */
    void (*on_frame)(void *h, void (*frame)(void *self), void *self);
    /* seg:off → flat host pointer into V86 low memory (our VdmMapFlat). */
    void *(*map_flat)(void *h, uint16_t seg, uint16_t off);
    /* Present a finished framebuffer (the video VDD → presentation layer). */
    void (*present)(void *h, const ntvdd_frame *f);
} ntvdd_bus;
```

`ntvdd_regs` is a thin view over the live V86 CONTEXT (the same `VDM_TIB+0x2D8` context the INT 21h
surface already edits), with the **CF-on-pushed-FLAGS** convention reused verbatim (see
[ntvdmcontrol-and-v86.md](ntvdmcontrol-and-v86.md)). `ntvdd_frame` is `{w, h, bpp, const uint8_t
*pixels, const uint32_t *palette /*256 ARGB*/, uint32_t stride}`.

### How the bus is fed from the existing host
The host already takes BOP/event 4 traps for INT 21h. M3 adds two more trap classes to the same
`v86_run` service loop:
- **GP-fault on `IN`/`OUT`** → decode port/width/direction → dispatch to the port owner. (This is
  the I/O-port trapping deferred from M1.)
- **Page-fault inside a claimed memory window** → dispatch to the memory owner.

Interrupts (`INT 10h`, `INT 1Ah`) already reflect through the IVT/BOP path; `claim_int` just
registers our handler in the same table the DOS kernel uses, so video/timer slot in beside INT 21h.

## 4. The presentation layer — DirectDraw, window + fullscreen

This is where the **two current binaries reconcile**. Today:
- `ntvdmhost.exe` — the working DOS engine, a **console (CUI)** app (text via the inherited console).
- `ntvdmex.exe` — the **Luna GDI window** (`src/main.c` + `console.c`), non-interactive preview.

**Decision (see ADR-0008):** the VDM host becomes a **GUI app that owns its own top-level window**,
and the **video VDD renders *all* modes — text *and* graphics — into it via DirectDraw**. We do not
keep two presentation paths (console for text, surface for graphics); the console screen-buffer
approach is abandoned. Text mode 3 is just "render the 80×25 B8000 cell grid with the embedded 8×16
VGA font into the framebuffer" — the same frame sink as mode 13h. This unifies the two binaries:
`src/console.c`'s glyph/grid knowledge moves into the video VDD's text renderer; `src/main.c`'s
window/Luna ownership moves into the host's presentation layer.

Presentation backend (`src/vdd/present_ddraw.c`), grounded in the DirectDraw research:
- **Pure C**: `#define COBJMACROS` + `#define CINTERFACE` before `<ddraw.h>`; methods called as
  `IDirectDraw7_CreateSurface(p, …)`.
- **No new link deps**: bind `DirectDrawCreateEx` via `LoadLibrary("ddraw.dll")` +
  `GetProcAddress`, and **define `IID_IDirectDraw7` inline** — this keeps the no-CRT minimal-import
  rule (drops both `-lddraw` and `-ldxguid`). `ddraw.dll` ships on stock XP SP3 (DX9c).
- **Windowed:** `SetCooperativeLevel(hwnd, DDSCL_NORMAL)`, primary = desktop + a clipper on the
  HWND, render into a **32bpp offscreen system-memory surface** (software-convert index→RGB through
  the emulated DAC — a 32bpp desktop will *not* palette-remap an 8bpp source), `Blt` (stretched) to
  the clipped client rect. Luna chrome is the OS-painted non-client frame (our comctl32 v6 manifest).
- **Fullscreen:** `SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE|DDSCL_FULLSCREEN)`,
  `SetDisplayMode(640,480,8,…)` (320×200×8 is often unsupported on real XP HW → use 640×480 and
  centre/scale), a complex flip chain + a real `IDirectDrawPalette` on the primary, copy indices
  into the back buffer honouring `lPitch`, `Flip`.
- **Robustness:** every `Blt`/`Flip`/`Lock` checks `DDERR_SURFACELOST` → `Restore` + re-upload +
  re-attach palette. `Lock`/`GetDC` hold the Win16 lock (freeze Windows) — keep the copy tight, no
  USER/GDI inside. Toggle windowed↔fullscreen by tearing down surfaces (not the `IDirectDraw7`) and
  re-issuing `SetCooperativeLevel`.

The video VDD owns the emulated VRAM + DAC + register state and produces an `ntvdd_frame`; the
presentation layer is mode-agnostic — it just blits whatever framebuffer it's handed, in whichever
of the two display modes the user has toggled (a hotkey, e.g. Alt-Enter).

## 5. The video VDD (`src/vdd/vdd_video/`) — VGA + VESA

Structured after DOSBox's split (one file per register block keeps the planar logic isolated), built
in the research's ruthless priority order:

1. **T0 — text mode 3** (B8000 trap, char+attr cells, embedded 8×16 font, hardcoded 16-colour
   table) + the INT 10h text subset (`00,0F,01/02/03,05/06/07,08/09/0A,0E,13`) + I/O ports
   `3C0/3C4/3CE/3D4/3DA` (incl. the 3C0 flip-flop and a toggling 3DA retrace bit apps busy-wait on).
   → a real DOS console in the Luna window. **This is the M3 first-light milestone.**
2. **T1+T2 — mode 13h** (A0000 linear, 320×200×256) + the DAC (3C8/3C9, 6-bit→8-bit) + INT 10h
   `AH=10h`. → a 256-colour graphics demo.
3. **T4 — VESA VBE 2.0**, **banked** (`4F00/01/02/03/05`), modes `0x101 640×480×8`,
   `0x103 800×600×8`, `0x111 640×480×16` over a larger host-side VRAM; **no LFB advertised** (apps
   fall back to banked — the V86-friendly path; a faked LFB is deferred).
4. **T5 — mode 12h planar** (latches, GC write-modes 0–3, Sequencer Map Mask) — last, only if a
   target app needs it.

Render loop: one host-side `vram[]` + `VGA` state struct + `DAC[256]`; an **active render-function
pointer reselected on mode set** (86Box's pattern); **full-frame redraw per frame** at first
(dirty-rectangles later). The frame tick also toggles the fake 3DA vretrace bit.

## 6. The other M3 VDDs

- **`vdd_pit` (timer) — built first to validate the seam.** It's the simplest device: claim ports
  `0x40–0x43` (8254), model channel-0 reload → effective rate, `raise_irq(0)` at ~18.2065 Hz default,
  and provide the INT 8 / INT 1Ah / INT 1Ch-chain behaviour + the `0040:006C` BIOS tick. Proving the
  bus end-to-end on the *trivial* device de-risks it before the heavy video work.
- **`vdd_input` (keyboard + mouse)** — INT 16h / INT 09h from host `WM_KEY*`, INT 33h from
  `WM_MOUSE*`, feeding the same window the video VDD presents into.
- **`vdd_sound` (stub)** — claims the SB/AdLib ports and the `RegisterModule` BOP path but no real
  audio yet; its job in M3 is to **prove the ABI generalises to a third device** (full audio is M7).

## 7. Build order (M3 slices, each a check-in)

| Slice | Deliverable | Gate |
|-------|-------------|------|
| **0** | Retire the `tools/vdmhost` spike | builds clean (done at kickoff) |
| **1** | `ntvdd.h` ABI + device bus + I/O-port & mem-window trap dispatch in `v86_run` | off-VM bus unit tests |
| **2** | `vdd_pit` (timer) on the bus | guest reads a ticking `0040:006C`; INT 1Ah time advances |
| **3** | Presentation layer: GUI host window + DirectDraw windowed/fullscreen (blit a test pattern) | a frame shows in the Luna window + fullscreen toggle |
| **4** | `vdd_video` T0 (text mode 3) → unify the console into the window | a real DOS text app runs **in the Luna window** |
| **5** | `vdd_input` (keyboard/mouse) | type at a DOS prompt in the window |
| **6** | `vdd_video` T1+T2 (mode 13h + DAC) | a 256-colour demo renders |
| **7** | `vdd_video` T4 (VESA VBE 2.0 banked) | a VESA app sets `0x101`/`0x111` and draws |
| **8** | `vdd_sound` stub + ABI generalisation check | ports claimed; ABI proven on a 3rd device |

**M3 exit:** a DOS app with a text-mode UI *and* a VGA graphics demo run in a themed DirectDraw
window (windowed + fullscreen), driven entirely through the pluggable VDD interface. (Mode 12h
planar, VESA LFB, the `vddsvc.h` binary-compat veneer, and real audio are explicitly post-M3.)

## 8. Sources

Distilled from three M3-kickoff research passes (full notes in this session's history):
- **NTVDM VDD ABI** — ReactOS (`testvdd.c`, `emulator.c`, `vidbios.c`), the `vddsvc.h` surface
  (`VDDInstallIOHook`/`VDDInstallMemoryHook`/`VdmMapFlat`/`VDDAllocMem`/`VDDSimulateInterrupt`), the
  `C4 C4 58 nn` `RegisterModule` BOP, the `HKLM\System\CCS\Control\VirtualDeviceDrivers` `VDD`
  `REG_MULTI_SZ` registry contract, and the third-party ecosystem (VDMSound, SoundFX/VSBD, GPIB-VDD,
  NetFOSS). Video + PIT confirmed **internal** to ntvdm (no public ABI → free design).
- **DirectDraw 7 on XP** — pure-C `COBJMACROS`/`CINTERFACE`, `DirectDrawCreateEx` via GetProcAddress,
  inline `IID_IDirectDraw7`, windowed (clipper+`Blt`) vs exclusive fullscreen (flip chain+`Flip`),
  8bpp `IDirectDrawPalette`, `Lock`/`lPitch`, `DDERR_SURFACELOST` recovery, Win16-lock caveat.
- **VGA/VESA emulation** — FreeVGA/OSDev register map, the trap windows (A0000/B8000), the
  text/13h/12h modes, the DAC, the INT 10h subset, VBE 2.0 banked-vs-LFB (banked is the V86-friendly
  MVP), and the DOSBox/86Box "render-fn-per-mode, full-frame redraw" structure.
</content>
</invoke>
