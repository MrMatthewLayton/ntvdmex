# Roadmap

Milestones are ordered by **risk and dependency**, not by feature glamour. Each milestone ends in
something runnable/observable. Win16 is intentionally late: it is built on the same V86 + DOS
foundation as everything before it.

> **Open work now lives in [GitHub Issues](https://github.com/MrMatthewLayton/ntvdmex/issues)** —
> epics are **milestones** (M4–M10), plus unmilestoned bugs/follow-ups. This file is the narrative
> roadmap + stage-history. Re-run
> [`scripts/gh-bootstrap-issues.sh`](../scripts/gh-bootstrap-issues.sh) to sync newly-added items
> (idempotent).
>
> ⚠ **The tracker is not the source of truth for "what's done".** Several M9 issues are still
> open there against work that has since been finished and gated on hardware (#44, #45, #47,
> #49, #50, #52 among them). Where this file and the tracker disagree, the arbiter is
> **`./tools/score/score.py`** — the one number in this project with a model behind it rather
> than a judgement. **Run it; do not quote a figure from any document, including this one.**

## Where it is, as of session 54 (2026-09-06)

`./tools/score/score.py` says **80.2%** of the full vision — MS-DOS **85.9%**,
WOW/Win16 **78.7%**, Product/Packaging **61.6%**. Against the two narrower bars:

- **The DOS games bar** (Doom / Skyroads / ZAR, flawless sound) — two of three fully
  playable and user-confirmed by hand and by ear. ZAR is the gap.
- **The Win16 north star** (MS Paint + Notepad from Windows 3.x) — **met**. Notepad edits
  and saves text; Paint draws in colour and writes a valid 24-bit `.BMP`. Nine Win16
  guests are routed and windowed, five user-confirmed doing their job.

The live detail — what works, what does not, and what to do next — is
[`STATE.md`](STATE.md). This file is the shape of the programme.

## How each step is tracked

Every milestone/step moves through five stages:

> **Research → Spike → Impl → Test → Done**

- **Research** — recover the contract: disassemble XP `ntvdm` / `basesrv` / `ntoskrnl`, read
  ReactOS for logic & structures; findings land in [`research/`](research/).
- **Spike** — a minimal, throwaway proof in the experiment harness ([`tools/vdmhost/`](../tools/vdmhost/)):
  does it work *at all*? Driven and logged from the XP VM.
- **Impl** — the real, clean implementation promoted into the host (`src/`).
- **Test** — verified on the XP SP3 VM (the canonical bench, `scripts/xp-vm.sh`).
- **Done** — exit criterion met, committed, this file + [`STATE.md`](STATE.md) updated.

**Research and Spike are risk-scaled.** For undocumented territory (most V86/VDM work) they are
essential. For documented, low-risk work (e.g. parsing an MZ header) they compress toward
Research → Impl → Test — *don't spike what's already known.*

> Stage status: ⬜ not started · 🟡 in progress · ✅ done · `–` not applicable

**History:** M0–M1 and M2.1–M2.4 were first proven in the throwaway **`tools/vdmhost` spike** (hence
the ✅ Spike / ⬜ Impl rows below). M2.6 promoted that proven DOS core into the clean `src/` host
(`ntvdmhost.exe`); the spike was then **retired** (removed at the start of M3 — see git history). The
✅ Spike / ⬜ Impl rows are kept as the historical audit trail; the live implementation is `src/`.

---

## M0 — Feasibility ✅ DONE
Prove the premise before writing real code.
**Exit:** one real-mode instruction executed in V86 under our host, fault reflected to us. ✅

| Step | Res | Spike | Impl | Test | Done |
|------|:--:|:--:|:--:|:--:|:--:|
| Interception — our binary runs as the VDM host | ✅ | ✅ | ✅ | ✅ | ✅ |
| V86 keystone — `NtVdmControl` runs one real-mode instr, fault reflects to us | ✅ | ✅ | – | ✅ | ✅ |
| XP-targeted no-CRT build toolchain | ✅ | ✅ | ✅ | ✅ | ✅ |

- Interception **pivoted** from the WOW `cmdline` repoint to the **IFEO `Debugger`** on `ntvdm.exe`
  ([ADR-0007](decisions/0007-intercept-via-ifeo-debugger.md)) — the repoint was disproven (XP
  validates the host image), see [ADR-0002 superseded](decisions/0002-intercept-via-wow-registry.md).
- V86 keystone proven: `NtVdmControl(VdmStartExecution)` ran `mov ax,0xBEEF; mov [0x80],ax` on the
  real CPU; GP/BOP faults reflect back to us. **[ADR-0004](decisions/0004-reuse-kernel-vdm-ntvdmcontrol.md)
  is now Accepted.** Full contract: [research/ntvdmcontrol-and-v86.md](research/ntvdmcontrol-and-v86.md).

## M1 — Minimal V86 host ✅ DONE (proven as spike)
**Exit:** a real-mode program that does INT 21h AH=09h prints a string to our console.
✅ — "Hello, World" from a real `.COM` loaded off disk.

| Step | Res | Spike | Impl | Test | Done |
|------|:--:|:--:|:--:|:--:|:--:|
| Low-memory map + self-allocated VDM_TIB + `VdmInitialize` | ✅ | ✅ | ⬜ | ✅ | ✅ |
| Fetch the program from CSRSS (`GetNextVDMCommand`) | ✅ | ✅ | ⬜ | ✅ | ✅ |
| Enter V86, run a hand-written real-mode stub | ✅ | ✅ | ⬜ | ✅ | ✅ |
| Interrupt reflection (real-mode IVT) + BOP host-callback loop | ✅ | ✅ | ⬜ | ✅ | ✅ |
| INT 21h AH=09h prints to our console | ✅ | ✅ | ⬜ | ✅ | ✅ |

I/O-port (`IN`/`OUT`) trapping is **not** done yet (deferred to M3 device work); interrupt
reflection — the harder half — is. **Impl ⬜:** all of M1 still lives in the `vdmhost` spike.

## M2 — DOS kernel 🟢 CLOSED (for M3)
M2.1–M2.6 done; the DOS core runs in V86 from the clean `src/` host. One documented best-effort
follow-up remains (recovering arbitrary real-shell args from CSRSS's undocumented multi-call protocol
+ the exit-code-to-shell notify) — it does not block M3.
**Exit:** run a real-world DOS `.EXE` that does file + console I/O, transparently.

| Step | Res | Spike | Impl | Test | Done |
|------|:--:|:--:|:--:|:--:|:--:|
| **M2.1** Real DOS process setup (≥640KB map, PSP, IVT seed, `.COM` at `PSP:0x100`) | ✅ | ✅ | ⬜ | ✅ | ✅ |
| **M2.2** INT 21h service surface (console + Win32-backed file I/O + misc) | ✅ | ✅ | ⬜ | ✅ | ✅ |
| **M2.3** MZ (`.EXE`) loader (header, relocations, segment setup) | ✅ | ✅ | ⬜ | ✅ | ✅ |
| **M2.4** DOS memory management (MCB chain, AH=48/49/4A) | ✅ | ✅ | ⬜ | ✅ | ✅ |
| **M2.5** Process plumbing (PSP command tail, env block, errorlevel) | ✅ | – | ✅ | 🟡 | 🟡 |
| **M2.6** Promote the DOS core from `vdmhost` spike → clean `src/` host | ✅ | – | ✅ | ✅ | ✅ |

Per-step exit criteria:
- **M2.1** — a `.COM` launches with a valid PSP and full conventional memory; a program that reads
  its PSP command tail sees the right bytes. ✅ **met** (spike `testps.com`: printed `args=[ HELLO]`
  from `DS:0x80`, and `himem=Y` proving a write/read at `0x90000` no longer faults). *Caveat:* the
  command tail is a fixed placeholder — real args need `CmdLine`, which `GetNextVDMCommand` does not
  populate yet (deferred to M2.5; the recovery of the real command line is the open item).
- **M2.2** — a program that opens/reads/writes a file (handles 3C/3D/3E/3F/40/42) and prints works.
  ✅ **met** (spike `filewr.com`: created `C:\ntvdmex\FILEIO.TXT` on disk, wrote/closed/reopened/read
  it back, printed `read back: Hello from DOS file I/O!`). Implemented: AH=02/06?/09 console,
  40 write, 3C/3D/3E/3F/42 file I/O (→ Win32 `CreateFile`/`ReadFile`/`WriteFile`/`SetFilePointer`),
  30 version; CF returned via the pushed FLAGS on the V86 stack. The rest of the ~40-function surface
  (input 01/08/0A, FindFirst/Next, get/set-attr, FCB calls, …) is added on demand as programs need it.
- **M2.3** — a real MZ `.EXE` (not just flat `.COM`) loads and runs. ✅ **met** (spike `helloexe.exe`:
  MZ header parsed, load module placed at `PSP_SEG+0x10`, the one relocation fixed up `mov ax,<seg>`
  to the load segment, `CS:IP`/`SS:SP` taken from the header → printed `Hello from a real .EXE!`).
- **M2.4** — a program that allocates/frees DOS memory runs. ✅ **met:** the self-checking
  `memtest.com` ran through `vdmhost` in **V86 on the real CPU** → `MEMTEST PASS`, exit 0
  (AH=4A shrink / AH=48 alloc / AH=4A resize / AH=49 free / AH=48 oversized-fails). Off-VM battery
  green 33/33 ([`tools/dostest/`](../tools/dostest/)) and verified under dosbox-x; `merge-on-alloc`
  now implemented (test T9). Impl ⬜ = the M2.6 `src/` promotion.
- **M2.5** — exit codes propagate to the launching shell; args + environment are visible to the guest.
  🟡 **Guest-visible plumbing done + off-VM-tested** (`67b433b`): a real env block (`src/dos/dos_env.h`),
  PSP command-tail builder (`dos_cmdtail_build`), errorlevel capture (`g_ci.ExitCode`); battery 49/49;
  `argtest.com` dosbox-verified (echoes its tail, exit = tail length). VM gate pending
  (`gate-clean.bat argtest.com HELLO`). **Best-effort follow-up (not blocking M3):** recover arbitrary
  real-shell args from CSRSS's undocumented multi-call `GetNextVDMCommand` protocol + the exit-to-shell
  notify; confirm `mem.exe` past `Parse Error 1` (likely fixed by the env block).
- **M2.6** — the clean host (not the spike) runs Hello World, gated by an import-allowlist check.
  ✅ **met (2026-06-07):** `ntvdmhost.exe` — the clean `src/` host (`src/dos` core + `src/vdm`
  V86/CSRSS glue + INT 21h surface + `src/host`) — ran `memtest.com` in V86 on the real CPU →
  **MEMTEST PASS** (VM-confirmed via `gate-clean.bat`). Imports **KERNEL32 only**
  (`scripts/check-imports.sh`); off-VM battery 42/42. The `tools/vdmhost` spike is now reference-only.

## M3 — Device model + video/input ✅ DONE (2026-06-09)
- [x] **Retire the `tools/vdmhost` spike** (clean host has parity) — done at M3 kickoff.
- [x] Pluggable **VDD** interface (requirement #13) — clean `src/vdd/ntvdd.h` ABI + device bus
  (`vdd_bus.c`): claim ports / memory-window / interrupt / frame, services raise-IRQ / map-flat /
  present. **Off-VM battery 22/22.** Design: [research/vdd-architecture.md](research/vdd-architecture.md).
- [x] **Timer VDD** (PIT 8254 + INT 08h/1Ah, IRQ0) — `src/vdd/vdd_pit.c`, the first device on the
  bus. **Off-VM battery 19/19** (8254 ports, clocks→IRQ0 engine, BIOS tick + rollover, time-of-day).
- [x] **I/O-port (`IN`/`OUT`) trap dispatch wired into `v86_run`** (slice-1b) — **VM-CONFIRMED
  2026-06-07.** IOPL-0 IN/OUT traps reflect as **event 0** (VM-discovered; the disasm taxonomy never
  labelled it; port pre-decoded into `VTIB_EVENT_INFO`). `host_try_io()` decodes the instruction and
  dispatches through the bus, then resumes. `ioprobe.com` ran on the real CPU: 4×`OUT`+`IN` routed to
  the PIT VDD (reload→0x1234, latched-count `IN`→0x34), guest resumed + exited 0x34. **End-to-end
  I/O virtualization proven through a real VDD.**
- [x] **DirectDraw presentation layer** (slice-3) — `src/vdd/present_ddraw.c`: windowed + exclusive
  fullscreen, one index→ARGB path, lost-surface recovery; `present_demo.exe` **rendered in the VM**.
  Imports kernel32+user32 only (ddraw bound at runtime).
- [x] **Video VDD — text mode 3** (slice-4): B8000 trap + INT 10h text subset + 80×25 cell grid +
  8×16-font/EGA-palette renderer → `present_ddraw` frame sink. Off-VM **23/23** (incl. a pixel-exact
  render check vs the font glyph). Font generated by `tools/gen-vgafont.py` (placeholder until the
  authentic IBM VGA ROM font).
- [x] **Merged `ntvdmhost`+`ntvdmex`** into one windowed host (**VM-CONFIRMED**): a UI thread owns the
  window + present_ddraw + a ~30Hz frame timer; the V86/DOS engine runs on the VdmInitialize thread;
  DOS console output (INT 21h) + INT 10h route through the video VDD → DirectDraw. A real DOS program
  (`hello.com`) painted **text in the Luna window** on the real CPU. (Also fixed: present_ddraw now
  packs pixels to the surface's real depth — XP/Cirrus is 16bpp — which removed the vertical striping.)
- [x] **Keyboard input** (INT 16h VDD + INT 21h AH=01/07/08/0A + UI WM_CHAR capture) — **VM-CONFIRMED**:
  typed an interactive `keytest.com` in the Luna window (letters, Enter, Backspace, ESC). Also swapped
  in the **authentic IBM VGA 8×16 ROM font** (CP437). Remaining: extended keys (arrows/F-keys via
  WM_KEYDOWN scancodes) + mouse (INT 33h).
- [x] **Graphics mode 13h** (320×200×256) + **DAC palette** + the **video aperture A0000-BFFFF mapped
  as RAM** — **VM-CONFIRMED**: `vgademo.com` drew an `x XOR y` rainbow fractal (correct 2D addressing,
  direct A0000 writes, DAC palette) in the Luna window on the real CPU.
- [x] **Video VDD: VESA VBE 2.0** (banked) — **VM-CONFIRMED**: INT 10h AX=4F00/01/02/03/05, modes
  0x100/0x101/0x103, 0x80000 VRAM window + bank select; `vesademo.com` drew a smooth scrolling gradient.
- [x] **Planar mode 12h** (640×480×16) — **VM-CONFIRMED**: A0000 trapped via
  `VirtualProtect(PAGE_NOACCESS)`; faulting stores decoded by `host_try_mem()` → `vga_planar_write`
  (write modes 0–3, Map Mask, set/reset + bit-mask + ALU, 4 latches) → 4 plane buffers; `render_planar`
  combines them. `vga12.com` drew the 16 horizontal colour bands. **Known gap:** the store decoder
  handles only STOSB/STOSW (REP) — arbitrary MOV/ModRM stores (QBasic SCREEN 12) are TODO.
- [x] **Presentation: windowed + fullscreen** (`present_ddraw.c`) — windowed GDI `StretchDIBits` from a
  double-buffer snapshot + `WaitForVerticalBlank` + cursor-hide; fullscreen DirectDraw 7 flip (the
  tear-free path; XP has no compositor). Pixels packed to the surface's real depth.
- [x] **Host chrome** — merged windowed host (UI thread + present + 30Hz timer; V86 on the
  VdmInitialize thread; `CRITICAL_SECTION` bus lock), full menu-bar scaffold + native comctl32 status
  bar + Common-Controls 6.0 manifest (Luna). nasm animated demos confirm each mode.
- [x] **VDD interrupt delivery — live PIT timer IRQ** (2026-06-09, **VM-CONFIRMED**). IVT[8] BOP stub
  (`BOP 08; CD 1C; IRET`) + IVT[1Ch]/[1Ah]; the service loop synthesises the real-mode INT 08h dispatch
  when the guest's main-line IF is set (read from the pushed stack frame at SS:SP+4, since at our
  control points the live EFLAGS IF is the in-handler value). `timertst.com` streams a dot per tick at
  ~18 Hz. (We synthesise the dispatch in user mode rather than going through the kernel ICA delay
  machinery — simpler and sufficient; documented in `log/2026-06-09.md`.)
- [x] **Run real DOS apps** — all 10 QuickBASIC `demos/*.EXE` run (**VM-CONFIRMED**): AH=06 console I/O
  + INT 10h VGA queries + port-3DA vsync + mode-12h glyph render + the MOV/XCHG store decoder + INT 33h
  mouse (with a host-drawn cursor). Linear modes (13h/VESA/text) are fast; per-pixel 12h plotting is
  slow by a documented wall (`research/hardware-vga-acceleration.md`). Two correctness fixes: the `XCHG`
  pixel-store opcode and the INKEY$ enhanced-keyboard (INT 16h AH=10/11) phantom key.
- [x] **Sound VDD stub** (2026-06-09) — `src/vdd/vdd_speaker.c`: PC-speaker control port 0x61 + the tone
  frequency from PIT channel 2 (channel-2 tracking added to `vdd_pit.c`). The third device *class* on the
  bus, proving the VDD ABI generalises beyond timer/video/input. Reports active/Hz; **no audio yet — full
  sound is M7.** Off-VM battery **13/13** (`speaker_test.c`).
- **Exit:** ✅ **MET.** A DOS app with a text-mode UI (VS87) and VGA graphics demos run in a themed
  DirectDraw window, driven entirely through the pluggable VDD interface (timer, video, input, speaker),
  with a live timer IRQ and a working mouse. **M3 DONE.**

## M4 — Memory extensions ✅ DONE
- [x] **XMS 3.0** (HIMEM.SYS) — `src/dos/dos_xms.h` + host wiring (`INT 2Fh AX=4300/4310`
  install/entry, a FAR-CALL API entry BOP, `host_xms()`). Extended memory is host-heap-backed
  (above the 1 MB V86 map); Move (0Bh) memcpys between an EMB and the conventional window.
  Off-VM battery **36/36** (`tools/dostest/xms_test.c`). VM gate `xmstest.com` (menu #14) pending.
- [x] **EMS (LIM 4.0)** (EMM) — `src/dos/dos_ems.h` + host wiring (`INT 67h`, `host_ems()`).
  64 KB page frame at E000:0 (V86 map extended with Map 5); `ems_map()` does **page-frame
  shadowing** (write-back + read-in memcpy) so the guest's direct frame accesses need no trap.
  "EMMXXXX0" device name parked for detection. Off-VM battery **30/30** (`tools/dostest/ems_test.c`).
  VM gate `emstest.com` (menu #15) pending.
- [x] **DPMI** (protected-mode DOS extenders, e.g. DOS/4GW) — **DONE.** A DPMI 0.9 host running
  unmodified third-party clients, in **real-CPU protected mode** (not the interpreter), including
  32-bit DOS/4GW. `INT 2Fh AX=1687h` is advertised and **required**. Exception reflection works:
  NT builds the DPMI 0.9 frame itself and the fault table is indexed by the x86 vector. A raw
  `INT nn` in PM is serviced from the `#GP` (the IDT bit in the error code names the vector),
  which retired the project's oldest silent VDM killer.
- **Exit: MET** — Doom runs its own 32-bit code through DOS/4GW on real silicon.
- **Still open here:** ZAR needs VBE 2.0 hi-colour + a linear framebuffer.

## M5 — Win16 / WOW foundation ✅ DONE
- [x] NE loader, 16-bit module/segment management — loads, relocates and binds the whole XP WOW
  module set on real hardware (209-check battery over all 15 real binaries).
- [x] WOW bootstrap — **XP's own `krnl386` executes**: protected mode, its own segments, its
  interrupt handlers, its own DPMI exceptions, and all eight 16-bit system modules. It reads
  `[boot] WOWSHELL` out of `SYSTEM.INI`, finds `WOWEXEC.EXE`, loads and **runs** it.
- [x] The 16↔32 boundary — the WOW32 call interface is pinned to the byte (**args at `bp+16`,
  return value in a stack hole at `bp-16`**) and dispatched. ⚠ **The id space is PER MODULE**
  — krnl386 seg1, krnl386 seg2, USER, GDI, COMMDLG, SHELL and MMSYSTEM are **seven** different
  numberings, gated on the BOP's own CS. krnl386's seg2 table has its own dispatcher now.
- [x] A host-side **Win16 task scheduler** (`src/wow/wowsched.h`) — krnl386 has none and we are it.
- **Exit: MET, and the caveats that qualified it are gone.** `WOWEXEC.EXE` reaches its message
  loop; `SYSEDIT.EXE` is a full MDI app with four `EDIT` children; the `0xd1` experiment value
  is a real implementation; and nine real applications have been launched through the same path.
- ⚠ **Two standing hazards from this layer.** An *implemented* service can be unreachable
  because its module was never identified, which is indistinguishable from unimplemented in a
  log — so anchors are generated from the binary (`tools/ne/wowthunks.py --anchor`), never by
  hand. And an export does **not** point at its stub: COMMDLG prefixes a far call, USER/GDI
  tail-jump, and some validate first.

## M6 — Win16 thunking ✅ EXIT MET — a real Win16 GUI app runs and paints
The line that used to read *"nothing has drawn a pixel"* is retired. It was written before
sessions 39–53.

- [x] **Win16 windows are real Win32 `HWND`s on the XP desktop** — not a framebuffer drawn
  inside the NTVDMEX window. ⚠ That was a **user correction** and it cost a whole VGA-renderer:
  drawing Win16 windows ourselves is the DOSBox-shaped answer. HWNDs belong to the exec thread,
  which is the reason threading is the design rather than an implementation detail.
- [x] **USER/GDI 16-bit objects mapped to Win32 handles; message bridging** (#6) — the queue
  and the 18-byte `MSG`, `PeekMessage`/`GetMessage`/`TranslateMessage`/`DispatchMessage`,
  keyboard and mouse from the host reaching a 16-bit window procedure. System classes
  (`MDICLIENT`, `EDIT`, `BUTTON`, `LISTBOX`, …) map to the real Win32 class.
- [x] **The host calls INTO 16-bit code** (`src/wow/wowcall.h`) — `C4 C4 57` plus a 16-bit CODE
  selector as the far return address, dispatched by linear address. ⚠ **`DS` on entry is the
  contract**: `DS = AX = the window's hInstance`.
- [x] **GDI drawing** — pens, brushes, blits, DIBs, fonts, clipping and metafiles. MS Paint
  draws every shape tool including flood fill, keeps it across a repaint, and saves it.
- [x] **Common dialogs** — the real XP File Open / Save As, driven from a Win16 guest.
- [x] **Resources** — icons, cursors and menus, **named as well as numbered** (that gap was
  found three separate times).
- [ ] **16:16 ↔ flat pointer translation — generic/flat thunks (#5) — ~25%.** Translation is
  real but it is **per-service and ad hoc**; there is no generic mechanism. That is fine at two
  guests and it is not fine at twenty, which is the honest reason this milestone is not closed.
- **Exit: MET** — Notepad is a working text editor and MS Paint is a paint program that saves
  files, both user-confirmed on real hardware.
- **Next unlock, and the entry point is pinned, not guessed:** dialogs. USER implements them in
  its own 16-bit code and calls a 32-bit helper at thunk `0xEF`, an id no export maps to; it has
  already done `FindResource`/`LoadResource`/`LockResource` and hands us the locked
  `DLGTEMPLATE`. That is TASKMAN, CALC, Solitaire's Options/Deck and Minesweeper's preferences.

## M7 — Peripheral VDDs 🟡 IN PROGRESS
- [x] **Sound — DONE and confirmed by ear.** SB16 PCM at 99.999% delivery, clean-room MIT
  **OPL2** FM (Nuked used only as a black-box oracle), MPU-401 MIDI, PC speaker.
  ⚠ *This line used to say OPL2/OPL3 and that was never true* — `vdd_opl` is a 9-channel OPL2.
  Remaining sound gaps: OPL3, and the snare/hi-hat/cymbal phase generator (#139).
- [x] **The PC speaker actually makes a sound, on both possible outputs** — the emulated square
  wave through the mixer *and* the transducer soldered to the motherboard, via Beep.sys.
  `PcSpeaker` is a four-way setting: Off / Sound card / Real PC speaker / Both.
  ⚠ `\\.\Beep` does not exist even when the service is running; the path is
  `\\?\GLOBALROOT\Device\Beep`, and `Beep()` itself is unusable here because it blocks and wants
  a duration up front.
- [x] **Video/input** — text, mode 13h, mode 12h planar, VESA banked; keyboard and mouse.
- [ ] Networking (#8), serial/parallel (#9)
- [ ] Bare-metal vs virtualized device strategy per [risks.md](risks.md) (#10)

## M8 — Polish & SDK 🟡 IN PROGRESS
- [x] Host UI shell — menu bar, status strip, six-tab Settings dialog backed by the registry,
  windowed GDI + exclusive-fullscreen DirectDraw, Luna-themed.
- [x] **Theming measured rather than eyeballed** — our Win16 client area is **pixel-identical to
  stock ntvdm** running the same program. The flat 3.x control look is what a Win16 app gets on
  XP from *both* hosts; a bevelled one ships `CTL3D.DLL` and draws its own. Gap: the fullscreen
  story (#12).
- [ ] **23 of the 47 settings are live** (#136) — up from 7. What is missing and why is written
  out above `settings_apply()` in `src/host/main.c`, which is the honest list. The largest
  remaining piece is the **CPU page**, which needs a duty-cycle throttle on the exec loop (#56).
- [ ] Pluggable VDD/driver SDK + docs for third-party developers (#11) — **not started**
- [x] Installer/registration tooling (#13) — done as a command and a menu; see M10.

## M9 — DOS/BIOS completeness (TDD) 🟡 IN PROGRESS
**The method, and it is the point of the milestone:** close every DOS/BIOS gap **test-first
against a real MS-DOS 6.22 oracle** (`./scripts/oracle.sh`), then gate it on the bare-metal XP
box. **Cardinal rule: never write a test expectation from memory.** The oracle is a panel, and
NTVDMEX is not ground truth.

- [x] INT 21h service surface (103 functions), EXEC 4Bh AL=01/03, TSR residency (AH=31h/INT 27h)
- [x] BIOS INT 10h/16h/1Ah/15h; INT 13h + INT 25h/26h absolute disk; INT 14h serial +
  INT 17h printer; INT 10h 11h user-defined fonts
- [x] The error model — 59h extended error, INT 24h critical error, InDOS/SDA adjacency (#34, 90%)
- [x] AH=52h SysVars — real DPB chain, NUL device header, CDS array (#48, 80%)
- [x] **`MEM.EXE` reports correct figures and matches the 6.22 oracle row for row** (#47).
  ★ Root cause was the bug shape that has now bitten this project twice: **a guest reads a fixed
  absolute offset in a segment we handed it** — `<SysVars segment>:0x008C` — with no call to
  intercept and nothing in any log. Wrong number plus a clean trace ⇒ disassemble the guest.
- [x] MS-DOS 6.22 `COMMAND.COM` runs as a guest, EXECs and returns (70%)
- [x] **Approximate CPU speed (#56)** — a menu dropdown under Machine, and the Settings CPU
  page's "Speed:" combo, both backed by one registry value: Unlimited / 200 / 100 / 66 / 33 /
  16 / 8 MHz. **Load-bearing, not cosmetic**: after the retrace fix, two of the five
  speed-affected demos pace on a software delay loop or on nothing at all, so no vsync fix can
  reach them even in principle. A real CPU cannot be clocked down, so the lever is a **duty
  cycle** — the exec thread is suspended for a share of each period, reusing the machinery the
  IRQ injector has used for twenty sessions. Calibrated against a probe whose loop costs 12
  cycles on a 486 (`tools/dostest/cpubench.asm`): the rig presents **3665 MHz** unthrottled,
  and 100 MHz measures 103. ⚠ **Two caveats, both in the log rather than hidden**: the slowest
  settings *saturate* on a host this fast, so the host reports `delivered_mhz` beside the
  requested one; and a slow setting is **chunky**, because 1 ms is the smallest slice `Sleep`
  can hand out. **Still to do:** the user's own acceptance bar — whether the five demos *feel*
  period-correct, judged on the box.
- [ ] Configurable DOS version (#28); VESA 4F0A PM bank switching (#53); INT 15h C0h/87h (#54)
- [ ] `$p` prompt degrades after an EXEC (#134); XP's own `COMMAND.COM` exits during init (#135)
- [ ] Verify the BIOS layer against real hardware end to end (#51)
- **Exit:** a DOS program cannot tell us from MS-DOS 6.22 without looking for the difference.

## M10 — Installation & routing 🟡 IN PROGRESS
Making NTVDMEX the machine's VDM, **reversibly**. This is what turns a host into a product.

> ⚠ **The plan changed on 2026-08-26.** #129 was going to make "leave it installed" safe by
> detecting Win16 launches and handing them back to stock `ntvdm`. **That is impossible** —
> measured three ways: Windows validates the VDM image's identity, so a renamed copy is refused
> outright and the real name re-enters us through the IFEO hook. There is therefore no safe
> install story until WOW works, which is what put M6 on the critical path.

- [x] **Routing verified behaviourally, not by the registry value** (#130) — a key that reads
  correctly and does not route is exactly the failure this project has been bitten by. So:
  delete the `Debugger` value, run a program, confirm **no** host log (stock ran it); add it
  back, run the same program, confirm the host log appears; then delete and re-add again.
- [x] **Recovery path** (#132) — consecutive failed starts are counted and cleared only on a
  clean exit; at three the host **drops its own IFEO value** and hands the machine back to
  Microsoft's `ntvdm`. Gap: SAFE mode is decided at two failures but does not skip anything yet.
- [x] **The launch matrix** (#140) — six rows across three DOS shapes and a Win16 GUI launch,
  each with a real two-host comparison and screenshots **diffed rather than eyeballed**. Row 2
  is a genuine superset result: stock refuses 6.22's `MEM.EXE` with `Incorrect DOS version`.
  Gap: rows 4–5 (DPMI client, in-guest redirection) have no stock half.
- [ ] **Console/stdio integration (#131)** — a DOS program launched from `cmd.exe` should run
  *inline in that console*, so `myprog.exe > out.txt` behaves. **Located, not fixed.** Five
  routes to the handle are eliminated by measurement (inheritance, `ATTACH_PARENT_PROCESS`,
  explicit parent pid, CSRSS's `STARTUPINFO`, and `VDM_COMMAND_INFO`'s own
  `StdIn/StdOut/StdErr`). The instrument that killed the fifth **named the real defect**:
  `GetNextVDMCommand` returns TRUE and populates *nothing* — the whole struct is constant
  garbage — because our command line arrives as `… -f` with **no `-i<taskid>`**, so CSRSS
  cannot tell which queued task we are asking about. ⇒ **#131 is the same defect as M2.5's
  "recover the real command line from CSRSS's multi-call protocol"**; fixing one fixes both.
  ⚠ *Not* a subsystem problem — this binary is already CUI, like `ntvdm.exe`.
  ► And the target is proven reachable by the oracle rather than by argument: `hello.com >
  out.txt` typed at `cmd` writes **136 bytes under stock ntvdm and 0 under ours**, same box,
  same command.
- [x] **An installer** (#13, shared with M8) — `ntvdmhost.exe /install`, `/uninstall`,
  `/status`, and the same three on the File menu behind a confirmation. It **checks for
  an existing value and can put it back**: a `Debugger` belonging to another program is
  saved before we displace it and restored on uninstall, and one we never installed is
  **refused** rather than deleted. Every claim is verified by reading the value back,
  not by a return code. Rig-gated behaviourally on all six cases.
  ⚠ Remaining: there is no **package** — nothing that copies the exe somewhere sensible,
  makes a Start Menu entry, or uninstalls through Add/Remove Programs. The routing half
  is done; the shipping half is not.
- **Exit:** the box can be left with NTVDMEX installed as its VDM, and nothing a user or a
  batch file does behaves differently except for being better.
