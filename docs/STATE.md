# Project State — Living Handoff

> **This is the canonical resume point.** Update it at the end of every working session.

- **Last updated:** 2026-08-05
- **Phase:** **M4 — memory extensions 🟡 IN PROGRESS.** **XMS 3.0 + EMS (LIM 4.0) DONE + VM-CONFIRMED
  (2026-07-20)** — off-VM 36/36 + 30/30 AND the `selftest` VM gate is **8/8 ALL TESTS PASSED** on the
  real CPU (fixed a `VdmInitialize` regression + dynamic EMS-frame relocation + two test-side bugs —
  see *M4 VM gate* below); **DPMI researched** (feasible via kernel-monitor PM
  reuse — see [research/dpmi-under-ntvdmcontrol.md](research/dpmi-under-ntvdmcontrol.md) — next is a
  16-bit mode-switch spike; DPMI not advertised until proven). See the *M4 milestone* section below.
  **M3 — device model + video ✅ DONE (2026-06-09).** M3 closed: full VGA/VESA video, keyboard, **live PIT
  timer IRQ**, **INT 33h mouse with a host-drawn cursor**, and a **PC-speaker VDD stub** (4 device
  classes on the pluggable bus); all 10 QuickBASIC demos run in the Luna window on the real CPU.**
  M2 (DOS kernel) is closed. M3 kickoff **retired the `tools/vdmhost` spike** and decided the
  **pluggable VDD architecture** ([ADR-0008](decisions/0008-pluggable-vdd-model.md) +
  [research/vdd-architecture.md](research/vdd-architecture.md)): clean `ntvdd.h` ABI + bus, built-in
  VDDs, DirectDraw (windowed + full-screen), VGA + VESA; the two binaries merged into one windowed
  host. **All of the M3 graphics/input stack is built and VM-CONFIRMED** — see the *M3 milestone*
  section below. In short: bus (22/22) + PIT (19/19) + input (15/15) + video (33/33) off-VM batteries;
  I/O traps reflect as **event 0** (VM-discovered, not the disasm's event 2); the merged windowed host
  paints DOS text + graphics in a Luna window; **text mode 3** (authentic IBM VGA 8×16 ROM font),
  **mode 13h** (320×200×256 + DAC), **mode 12h** (640×480×16 **planar** via an A0000 `PAGE_NOACCESS`
  trap + a VGA write-mode engine), and **VESA VBE 2.0** (banked hi-res) all render; **keyboard** (INT
  16h + INT 21h + WM_CHAR) works; windowed present is **GDI StretchDIBits + double-buffer snapshot +
  WaitForVerticalBlank + cursor-hide** (XP has no compositor, so fullscreen DirectDraw flip is the
  tear-free path); a **native comctl32 status bar + full menu-bar scaffold** + the **Luna manifest**
  are in. nasm animated demos (`vgademo`/`vga12`/`vesademo`) confirm each mode on the real CPU.
  **Now:** running real DOS apps — **all 10 QuickBASIC demos RUN.** 13h/text/VESA (CAVE/GFXCOPY/PALETTE/
  VS87/MATRIX) are pure real-CPU and fast; the 12h *pixel* demos (BLIT/BOUNCEBX/BUBBLES) + MOUSE run but
  are **slow by a fundamental wall** — QB plots ~50 instr + a per-pixel VGA `OUT`, so trap-per-pixel and
  batched-interpreter cost the same (see *Real DOS apps* below). A **tiered mode-12h interpreter**
  (`v86interp.h`, opt-in only on detected trap-storms) + fixes for the `XCHG` pixel-store and the INKEY$
  phantom-key got all 10 running; the opcode climb is intentionally stopped at the wall. INT 33h mouse in.
  *M2 follow-up (not blocking):* CSRSS transparent-arg recovery + exit-to-shell notify.
- **Overall status:** 🟢 **The keystone is proven.** A real DOS `.COM`, loaded off disk, runs on the
  real CPU in **Virtual-8086 mode** via `NtVdmControl` and prints "Hello, World" through our own INT
  21h handler — launched transparently when XP starts a 16-bit program (IFEO redirect). The whole
  pipeline runs end-to-end from a from-scratch host, with **no Microsoft VDM binaries**.

## One-paragraph summary

We are building a drop-in replacement for Windows XP SP3 (32-bit) `ntvdm.exe` that runs 16-bit DOS
and Win16 code on the **real CPU in V86 mode** by driving XP's existing kernel VDM machinery through
the undocumented `NtVdmControl` syscall. Interception is the **IFEO `Debugger` redirect** on
`ntvdm.exe` (registry-only, no signed system file replaced). Devices (video, sound, input, net) are
virtualized and serviced by host calls through a pluggable VDD model; video is blitted into a
Luna-themed window. Work is tracked per step through **Research → Spike → Impl → Test → Done** (see
[ROADMAP.md](ROADMAP.md)).

## Proven pipeline (M0 → M2.3)

From `tools/vdmhost/vdmhost.c` (the spike), in one process — real `.COM` and `.EXE` programs run,
with file I/O:

1. **Intercept** — IFEO `Debugger` on `ntvdm.exe` → our binary runs in ntvdm's place (ADR-0007).
2. **CSRSS handshake** — `RegisterConsoleVDM` + `GetNextVDMCommand` → resolves the program to run
   (`CurDir\Title`). (The `0x57` that blocked this for ages was a console-key harness artifact, not
   a real binding problem — the lookup is keyed on the console handle.)
3. **Kernel V86 monitor** — V86 memory map + `NtVdmControl(VdmInitialize)` + a **self-allocated
   VDM_TIB** registered at `TEB+0xF18` (the kernel does *not* allocate it — ntvdm does).
4. **Execute** — `NtVdmControl(VdmStartExecution)` runs the guest on the CPU; CONTEXT lives at
   `VDM_TIB+0x2D8`.
5. **DOS services** — `INT 21h` reflects through the real-mode **IVT** to a **BOP** (`C4 C4 nn`)
   handler that returns to the host (event 4); host services it, advances EIP past the BOP, and
   re-enters so the handler's `IRET` resumes the guest. Multiple syscalls per run.
6. **DOS process model (M2.1–M2.3)** — the V86 memory map now covers the **full 640KB** (ntvdm builds
   FOUR section maps; we were missing Map 3 `section[0x10000..]→0x10000 size 0x90000`, which is why
   guests faulted at `0x10000`). A real **PSP** is built at `PSP_SEG:0` (`0x1000`); `.COM` loads at
   `PSP:0x100`, `.EXE` (MZ) is parsed → load module at `PSP_SEG+0x10`, relocations applied,
   `CS:IP`/`SS:SP` from the header. **INT 21h surface:** `02/09` console, `40` write, `3C/3D/3E/3F/42`
   file I/O → Win32 handles (table `g_fh[]`, slots 5+), `30` version. **CF is returned on the pushed
   FLAGS at `SS:SP+4`** (the handler's `IRET` restores FLAGS, so the live `EFlags` would be clobbered);
   `AX` etc. persist via the context.

Full contract, addresses, event taxonomy, memory map + PSP layout:
[research/ntvdmcontrol-and-v86.md](research/ntvdmcontrol-and-v86.md) and
[research/dos-process-model.md](research/dos-process-model.md).

## What is decided (see [decisions/](decisions/))

- **ADR-0001** — No CPU emulation; execute real-mode code in V86. *Accepted.*
- **ADR-0002** — Intercept via WOW registry repoint. **Superseded by ADR-0007** (repoint disproven).
- **ADR-0003** — Scope = DOS **+** Win16/WOW; Win16 deferred to M5 behind a clean seam. *Accepted.*
- **ADR-0004** — Reuse kernel VDM via `NtVdmControl` rather than a custom V86 driver.
  **Accepted** (the V86 keystone is proven; no custom driver needed).
- **ADR-0005** — Target XP in a VM **and** bare metal; VM-first for the dev loop. *Accepted.*
- **ADR-0006** — Build with mingw-w64 (i686) cross-compiler, no C runtime. *Accepted.*
- **ADR-0007** — Intercept via the IFEO `Debugger` redirect on `ntvdm.exe`. *Accepted.*
- **ADR-0008** — Pluggable VDD model: clean internal ABI (`ntvdd.h`) + a deferred `vddsvc.h`
  binary-compat veneer, over one device bus; video is a built-in VDD; DirectDraw presentation in a
  host-owned window (windowed + fullscreen); `ntvdmhost`+`ntvdmex` merge. *Accepted (M3).*

## What is built

- **`src/` clean host (`ntvdmhost.exe`) — the live implementation of M0–M2.** The full pipeline
  above, promoted out of the spike into clean modules: `src/dos` (mcb / loader / psp / env / int21 /
  layout), `src/vdm` (ntvdm.h contract + v86.c + csrss.c), `src/host` (log.h + main.c). KERNEL32-only,
  off-VM battery 49/49, MEMTEST PASS in V86. **The original `tools/vdmhost` proving spike was retired
  at the M3 kickoff** (clean host reached parity) — recover it from git history if ever needed.
- **Shell preview (`src/`):** a fixed 80×25 DOS-style Luna-themed console (GDI text grid), the future
  sink for INT 10h / B800 text writes. Confirmed running on XP SP3. **Still non-interactive** — the
  DOS core has not yet been promoted into it (that's M2.6).
- **Build system:** CMake + mingw-w64 i686 cross, no-CRT (`cmake/toolchain-xp32-mingw.cmake`).
- **Dev bench:** QEMU XP SP3 VM (HVF = real V86) via `scripts/xp-vm.sh`; QMP socket for
  `screendump`/`send-key`; telnet (`scripts/xp.py`) drives the guest; TFTP pushes binaries
  (host→guest). Reverse-engineering inputs (extracted MS binaries) in gitignored `reverse/`.

## What is open / next

**Phase M2 (DOS kernel).** Trackable sub-steps with exit criteria are in [ROADMAP.md](ROADMAP.md):
M2.1 process setup (PSP + ≥640KB map), M2.2 INT 21h surface (file I/O via Win32 + console),
M2.3 MZ `.EXE` loader, M2.4 DOS memory management, M2.5 process plumbing, M2.6 promote spike → `src/`.

The big structural reality the stage model exposes: **almost everything is at ✅ Spike / ⬜ Impl.**
We've *proven* the DOS core in `vdmhost`; the clean `src/` implementation is essentially unwritten.

## M2.4 — DOS memory management (DONE — validated end-to-end, 2026-06-06)

**Committed (`4aa6f44`):** `tools/vdmhost/vdmhost.c` — MCB chain over conventional memory + `INT 21h`
`AH=48/49/4A` (alloc/free/resize, with block split + forward coalesce), plus supporting services
`44h`/`25h`/`35h`/`51h`/`62h`/`50h`/`1Ah`/`2Fh`/`19h`/`0Eh`/`0Dh`/`33h`/`63h`. Also a
`C:\ntvdmex\target.txt` **override** (load any binary by path — decouples DOS tests from the flaky
Title recovery) and incremental `applog()`.

**Off-VM test harness (`bf3defe`, [`tools/dostest/`](../tools/dostest/)):** the allocator logic
ported into a host-testable module (`dos_mcb.h`) + a **30-case native battery** (`mcb_test.c`, via
`run.sh`) — split, forward-coalesce, grow-into-neighbour, grow-blocked, fail-returns-largest,
invalid-block, and full chain integrity after every op. **All green (30/30), no VM in the loop.**
This is **Layer 1** of the test plan and the clean basis for the M2.6 promotion (where `vdmhost`
adopts the shared module instead of its inline copy).

**Gap closed (`8d84af4`, 2026-06-06):** `merge-on-alloc` is implemented — `alloc()` coalesces
adjacent free blocks during the walk (battery test T9), matching real MS-DOS, in both
`tools/dostest/dos_mcb.h` and the `vdmhost` spike. Battery green **33/33** off-VM; spike
cross-compiles clean.

**Validated end-to-end on real hardware (2026-06-06):** the self-checking `memtest.com` (Layer 2,
[`tools/dostest/`](../tools/dostest/)) ran through `vdmhost` in **V86 on the real CPU** and returned
`MEMTEST PASS`, exit code `AL=0` — exercising AH=4A shrink / AH=48 alloc / AH=4A resize / AH=49 free /
AH=48 oversized-must-fail, with segment/size values matching the off-VM battery and dosbox-x exactly
(e.g. the post-free coalesce restoring `max=0x8eff`). **This meets the M2.4 exit criterion.** Also: a
real `C:\WINDOWS\system32\mem.exe` parsed by our MZ loader and `AH=4Ah` resize succeeded; it still
ends in `Parse Error 1` (missing INT 21h surface / env block → M2.5). See
[log/2026-06-05.md](log/2026-06-05.md), [log/2026-06-06.md](log/2026-06-06.md).

## M2.6 — promote spike → clean `src/` host (IN PROGRESS, 2026-06-06)

Promoting the proven DOS core out of the `tools/vdmhost` throwaway spike into a clean, tested `src/`
tree. **Decision:** build a new clean **console** host (its own target), keeping the spike as a
reference until parity; merge into the Luna window at M3. Slices (each its own check-in):

- **Slice 1 ✅ (`be8660a`):** `src/dos/` pure DOS core — `dos_mcb.h` (allocator, moved here as the
  canonical home), `dos_loader.h` (.COM/MZ loader + relocations), `dos_psp.h` (PSP + env). Off-VM
  battery **42/42** ([`tools/dostest/`](../tools/dostest/)); `merge-on-alloc` lives here now.
- **Slice 2a ✅:** `src/vdm/ntvdm.h` (`ab1bc92`) — the undocumented NT/CSRSS/V86 contract
  (VDM_COMMAND_INFO, NtVdmControl/VdmInitialize + ICA, RegisterConsoleVDM, the section syscalls, and
  the VDM_TIB + CONTEXT field offsets) consolidated from the spike's globals; + `src/host/log.h`
  (`6a8b26a`). The whole clean `src/` surface cross-compiles clean together with the i686 XP toolchain.
- **Slice 2b ✅ (`300657f`):** the V86 glue — `src/vdm/v86.c` (memory map + VdmInitialize +
  self-allocated VDM_TIB + entry CONTEXT + VdmStartExecution/BOP service loop), `src/vdm/csrss.c`
  (RegisterConsoleVDM + GetNextVDMCommand + TaskId + resolve `CurDir\Title`), `src/host/main.c`
  orchestration, + the **`ntvdmhost`** CMake console target. **Compiles + links, zero warnings.**
- **Slice 3 ✅ (`300657f`):** the INT 21h surface (`src/dos/dos_int21.*` — console + Win32 file I/O +
  misc; AH=48/49/4A delegate to `dos_mcb.h`, no inline copy) wired into the host. Import-allowlist
  check (`scripts/check-imports.sh`) passes — **`ntvdmhost.exe` imports KERNEL32 only** (no CRT/UCRT).
- **Slice 4 ✅ (2026-06-07):** the clean host (`ntvdmhost.exe`) ran `memtest.com` in V86 on the real
  CPU → **MEMTEST PASS**, confirmed via a manual `gate-clean.bat` run on the VM (it sets IFEO→
  `ntvdmhost.exe` and prints `ntvdmhost.log`). **M2.6 exit met — the clean host works at runtime.**

**Re-running the gate** (other programs / a saved trace): on the host `./scripts/stage-gate.sh`, then
in the VM desktop run `gate-clean.bat` — it points the IFEO `Debugger` at `ntvdmhost.exe`, runs the
program in V86, and prints `C:\ntvdmex\ntvdmhost.log`. (`gate.bat` + `stage-gate.sh spike` gate the
spike; each re-points the IFEO at its own host.) NOTE: the reboot-free telnet path (`dostest.sh` +
the logon agent) is for the SPIKE's `vdmhost.log`; it needs `vdmtrig.bat` running and a clean-host
variant to capture `ntvdmhost.log` — the manual `gate-clean.bat` is the reliable clean-host gate.

## M4 milestone — memory extensions (XMS + EMS DONE; DPMI researched, 2026-06-09)

The memory-extension layer. Two real-mode thirds are done (off-VM-proven + host-wired); the
protected-mode third (DPMI) is researched and scoped to a spike.

- **XMS 3.0** (`src/dos/dos_xms.h`, off-VM **36/36** `xms_test.c`): how real-mode programs reach
  memory above 1 MB without leaving real mode. Detected/entered via `INT 2Fh AX=4300` (install →
  AL=80) / `AX=4310` (entry → ES:BX); the function goes in AH on a **FAR-CALL** to the entry. The
  clean part of the architecture: extended memory lives on the **host heap** (above the 1 MB the
  V86 map covers), so EMBs are `VirtualAlloc` blocks and **Move (0Bh)** memcpys between an EMB and
  the guest's conventional window — a pure-real-mode client never addresses an EMB directly.
  Implements version/A20/query-free/alloc/free/realloc/lock/unlock/move + error codes; 16 MB pool.
  Host: `host_xms()` + BOP 0x2F (INT 2Fh stub, IRET) + BOP 0x43 (XMS entry stub, **RETF** since
  it's far-called) at `DOS_HDLR_SEG:0x44`. VM gate `xmstest.com` (menu #14) **pending**.
- **EMS (LIM 4.0)** (`src/dos/dos_ems.h`, off-VM **30/30** `ems_test.c`): a 64 KB page frame at
  **E000:0** (four 16 KB windows) into which the program maps 16 KB logical pages from a large
  pool. Runs without a trap via **page-frame shadowing**: `ems_map()` writes the outgoing window
  back to its logical page then reads the new page in (memcpy), so the guest's direct frame
  accesses are plain RAM and the backing store stays coherent. Implements counts/alloc/map/
  dealloc/realloc/handle-pages/handle-count + save/restore page map (47h/48h); 8 MB pool. Host:
  `host_ems()` + BOP 0x67 (INT 67h); **`v86.c` extended to map 64 KB of RAM at linear 0xE0000**
  (Map 5, section grown to 1 MB) — *this changes the proven memory map, so the EMS VM gate also
  re-validates V86 bring-up*. "EMMXXXX0" device name parked at the INT 67h vector segment:000Ah
  (detection method 2); DBCS table relocated 0x10→0x18 to make room. VM gate `emstest.com`
  (menu #15) **pending**.
- **DPMI** (`research/dpmi-under-ntvdmcontrol.md`): the protected-mode third (DOS/4GW etc.).
  **Key finding from `ntvdm.exe` disasm:** the *same* `NtVdmControl` VDM runs protected mode, not
  just V86 — `fcn.0f00532e` reads `getMSW`, tests the **PE bit**, and when set drives the client's
  interrupt flag via **service 13 `VdmPMCliControl`**; LDT install is **services 10/11**
  (`VdmSetLdtEntries`/`VdmSetProcessLdtInfo`); `INT 2Fh AX=1687h` is ntvdm's own DPMI host. So DPMI
  is feasible by **reusing the kernel monitor's PM support** (ADR-0004), *not* a from-scratch LDT
  host. Plan: a **16-bit DPMI spike** (real→PM far-call → confirm PM via MSW → `INT 31h 0000`
  descriptor + `0300` thunk INT 21h back to real mode → exit) before any INT 31h surface. **DPMI is
  deliberately not advertised** (`2Fh 1687` left unhandled) until the switch is proven — advertising
  then failing the switch crashes extenders worse than absence.

## M3 milestone — pluggable VDDs + full VGA/VESA video (DONE + VM-CONFIRMED, 2026-06-08)

The device model and the whole graphics/input stack are built and confirmed on the real CPU in the VM.

- **VDD bus + ABI** (`src/vdd/ntvdd.h`, `vdd_bus.c`): devices claim ports / memory-window / interrupt /
  frame; host injects raise-IRQ / map-flat / present sinks. Off-VM **22/22**.
- **I/O-port traps reflect as event 0** (VM-discovered; the disasm taxonomy never labelled 0 and we'd
  guessed event 2). `host_try_io()` decodes IN/OUT and dispatches through the bus. Port pre-decoded
  into `VTIB_EVENT_INFO` low word. **`ioprobe.com` confirmed** 4×OUT+IN through the PIT VDD.
- **PIT VDD** (`vdd_pit.c`): 8254 ch0 + INT 08h/1Ah, off-VM **19/19**. (Live IRQ0 delivery still TODO.)
- **Input VDD** (`vdd_input.c`): keyboard ring + INT 16h (ZF "key ready"), off-VM **15/15**; wired to
  INT 21h AH=01/07/08/0A and the UI WM_CHAR capture. **Confirmed** typing in the window (Enter/BS/ESC).
- **Video VDD** (`vdd_video.c`, off-VM **33/33**) — one device, all modes, renders into `st->frame`
  (the host presents):
  - **Text mode 3**: B8000 80×25 char/attr grid, **authentic IBM VGA 8×16 ROM font** (CP437, from
    `tools/IBM_VGA_8x16.bin` via `gen-vgafont.py`), EGA 16-colour attributes, INT 10h teletype subset.
  - **Mode 13h**: 320×200×256 linear at A0000, DAC palette ports 3C7/8/9 (set in one INT 10h AH=10/12
    call — the per-OUT palette loop was the "blue for 2–3s" stall). `vgademo.com` x^y fractal.
  - **Mode 12h**: 640×480×16 **planar** — the hardest piece. A0000 is trapped via
    `VirtualProtect(PAGE_NOACCESS)` (`a000_protect()` in the host); the faulting store is decoded by
    `host_try_mem()` and run through `vga_planar_write` (write modes 0–3, Sequencer Map Mask, Graphics
    Controller set/reset + bit-mask + ALU, 4 latches) into 4 plane buffers; `render_planar` combines
    them. **NOTE the decoder currently handles only STOSB/STOSW (REP)** — arbitrary MOV/ModRM stores
    are a known gap (blocks general mode-12h programs incl. QBasic SCREEN 12).
  - **VESA VBE 2.0** (banked): INT 10h AX=4F00/01/02/03/05, modes 0x100/0x101/0x103; 0x80000 VRAM
    window + bank select. `vesademo.com` smooth gradient.
- **Presentation** (`present_ddraw.c`): **windowed = GDI `StretchDIBits`** from a double-buffer
  *snapshot* (taken under the bus lock by `_snapshot()`, blitted outside it by `_present()`) +
  `WaitForVerticalBlank` + cursor hidden over the client area. **Fullscreen = DirectDraw 7 flip chain**
  (the only tear-free path — XP's desktop has no compositor, so windowed GDI can't be fully tear-free).
  DirectDraw is bound at runtime (no `-lddraw/-ldxguid`); pixels packed to the surface's real depth
  (XP/Cirrus = 16bpp — that was the vertical-striping bug). Switched windowed off DirectDraw-to-primary
  because that flickered the cursor.
- **Host chrome** (`src/host/main.c`): two-thread model (V86 engine on the VdmInitialize thread; UI
  thread owns window + present + a ~30Hz frame timer; a `CRITICAL_SECTION` serialises bus dispatch).
  **Full menu-bar scaffold** (File/Edit/CPU/Display/Audio/Input/Drive/Capture/Debug/Help — unwired =
  `IDM_STUB`; wired Exit/Fullscreen/ShowMenuBar/About), a **native comctl32 status bar**, and the
  **Common-Controls 6.0 manifest** (`res/ntvdmhost.rc` → `ntvdmex.manifest`) for Luna theming.
- **Demo corpus:** nasm animated demos `tools/dostest/{demo13,demo12,demovesa}.asm` (→
  `vgademo`/`vga12`/`vesademo`.com via `make-demos.sh`) confirm each mode animates on the real CPU. The
  user-supplied **`demos/*.EXE`** (10 standalone QuickBASIC 4.5 programs — SCREEN 0/12/13) are the next
  driving corpus and are staged on `vm/test.iso` via `RUNTEST.BAT` (`/tmp/ntvdmex_cd/`).

## Real DOS apps — first QuickBASIC app renders (VM-CONFIRMED, 2026-06-08)

**`PALETTE.EXE` (QuickBASIC 4.5, SCREEN 13) renders its colour ladders in the Luna window on the real
CPU** — the first real DOS application running end-to-end through NTVDMEX. Two DOS-layer gaps were
diagnosed (via `ntvdmhost.log`, driving the VM headlessly with `scripts/qmp.py`) and closed:
1. **INT 21h AH=06 (Direct Console I/O)** (`7bd40a1`) — QB routes all console output + key polling
   through AH=06; was unhandled → black window. Now: DL=FF → non-blocking read; else → output char.
   Plus AH=2A/2C date/time, AH=0B status, AH=2B/2D. (Non-blocking `coninnb`/`conpeek` host hooks.)
2. **INT 10h VGA capability queries** (`8d88339`) — QB's `SCREEN` probes the adapter via AH=1B (state
   info) / 1A (DCC) / 12 (alt select) / 11/30 (font); all were no-ops → QB rejected mode 13h with
   "Illegal function call". Now they report a real VGA (AH=1B state block + a static functionality
   table advertising modes 0..0x1F). The host now logs every INT 10h call (the trace that found this).

## Real DOS apps — all 10 QuickBASIC demos RUN; mode-12h pixel plotting hits a wall (VM-confirmed, 2026-06-09)

**All 10 demos run.** Fast (pure real-CPU): **CAVE, GFXCOPY, PALETTE** (13h), **VS87** (text),
**MATRIX_1/2** (12h *text* via the glyph renderer). Run but **slow**: **BLIT, BOUNCEBX, BUBBLES** (12h
*pixel* fills) + MOUSE. Window caption "Virtual MS-DOS Prompt - PROG.EXE" + `MAINICON.ico`.

**The mode-12h tiered interpreter** (`src/host/v86interp.h` + `host_interp()` in `main.c`): by default
V86 runs on the real CPU and each VGA access (port via `host_try_io`, A0000 memory via a single
interpreter step) is emulated **one-at-a-time as a device access** — pure virtualization, no CPU
interpretation. **Only** on a detected mode-12h *trap-storm* (the same tight PC window faulting
repeatedly — storm detection lives in the service loop, covering port + memory faults) does it escalate
to the **batching interpreter**, which runs the inner pixel loop in the host (planar A0000 via the VGA
engine, IN/OUT via the bus, ALU/flags, INC/DEC, MOV incl. Sreg, PUSH/POP gp+seg, CALL/RET, Jcc/JMP/LOOP,
shifts/rotates, LEA, XCHG). Off-VM unit-tested: `interp_test.c` **63 checks**.

**THE WALL (why 12h pixel demos stay slow — fundamental, not a missing opcode):** QuickBasic's runtime
plots **one pixel at a time**, issuing ~50 instructions **and** reprogramming a VGA register via `OUT`
*per pixel*. Cost per pixel is the same either way: **real-CPU+trap** = ~50 native instr (fast) **+ 2
kernel round-trips** (the OUT + the XCHG, ~µs each, dominate); **batched interpreter** = 0 round-trips
but those ~50 instr/pixel now run *interpreted* (~50–100 ns each ≈ same µs/pixel). Batching just trades
round-trips for interpretation — roughly equal for QB's fat routine — so a ~307K-pixel fill is ~1 s
**either way**. Measured: batch factor climbed 5 → 23 → ~50 (peaks 757) as opcodes were added (XCHG →
CALL/RET → seg push/pop → shifts), with **no speed change** — confirming the wall. More opcodes
(LES/LDS next) would batch further but cannot break it. **Decision (2026-06-09): stop the opcode climb,
keep the interpreter** (it fixed real correctness bugs + helps short-loop 12h), accept 12h pixel-plot as
correct-but-slow. Linear modes (13h/VESA) have none of this — the CPU writes pixels directly, fast.

Two correctness bugs fixed here: **`XCHG ES:[DI],AL`** (QB's pixel-store opcode — was crashing
BUBBLES/BOUNCEBX with a stop dump), and the **INKEY$ phantom key** — INT 16h enhanced fns AH=10h/11h
(QB's INKEY$) hit a `ZF=0` default → a phantom keystroke, so `DO WHILE INKEY$ = ""` exited instantly and
BLIT drew nothing; implemented 10h/11h + made the default report "no key".

## M4 VM gate — GREEN 2026-07-20: 8/8 ALL TESTS PASSED on the real CPU

The `selftest` VM gate runs end-to-end on the real CPU and every subsystem passes:

```
DOS memory......  PASS           File I/O........  PASS
XMS 3.0.........  PASS           EMS LIM 4.0.....  PASS
PIT timer IRQ...  PASS           Mouse INT 33h..   PASS
Keyboard INT16..  PASS           Video 13h+A000..  PASS
==== ALL TESTS PASSED ====
```

**Both M4 memory managers (XMS 3.0 + EMS LIM 4.0) PASS on real hardware** — the point of the gate.
Getting here fixed two real regressions in the V86 bring-up (committed `562deed`):

- **`VdmInitialize` was failing (`NTSTATUS=0xc000001a` STATUS_UNABLE_TO_FREE_VM) → guest never ran.**
  Cause: the EMS slice pre-mapped a section view at 0xE0000 *inside* `v86_setup_memory` (before
  init), but `VdmInitialize` requires the UMA range free during init. **Fix:** map the EMS frame
  AFTER `v86_init()` — new `v86_map_ems_frame()` in `src/vdm/v86.c`, called from `main.c` right
  after `v86_init`. (A0000 stays pre-init; the kernel leaves the VGA aperture alone.)
- **EMS `AH=44` (map page) then crashed** the host (access violation): post-init, E0000 has only
  **32KB free** (`VirtualQuery`: state=MEM_FREE, RegionSize=0x8000) — E8000+ is occupied — so a 64KB
  frame map returned `0xc0000018` CONFLICTING_ADDRESSES and `ems_map`'s shadowing memcpy wrote to
  non-writable memory. **Fix:** `v86_map_ems_frame()` now *scans* the conventional page-frame
  segments {D000, C000, E000} via `VirtualQuery` for a free 64KB hole and maps there; the chosen
  linear base flows through `g_ems_frame_lin` → `ems_init`. The guest reads the segment via EMS
  `AH=41`, so relocation is transparent (D000 gets picked). Also added flushed BOP breadcrumb
  logging (INT 2Fh / XMS-entry / INT 67h) in the service loop — that's what pinpointed the crash.

**The two initial failures were both TEST-side bugs (the host was correct), now fixed in
`tools/dostest/selftest.asm` (committed 2026-07-20):**
- **DOS memory (was FAIL=11)** — `t_dosmem` called `AH=48` without first shrinking its own PSP
  block. A freshly-loaded `.COM` *owns all* conventional memory (`dos_mcb_init` lays the program
  block as `'Z'` owner=PSP size 0x9F00), so `AH=48` correctly returns error 8 / `max=0` until the
  program shrinks itself with `AH=4A` — exactly what `memtest.com` does. **Fix:** `t_dosmem` now
  `AH=4A`-shrinks to 0x1000 paras first, freeing the tail to allocate from. (Host was right.)
- **PIT timer (was FAIL=51)** — `t_timer` *does* yield via `INT 16h` each iteration (a valid INT 08h
  injection point), but its single 65536-poll loop can finish in ~30-65ms — under one PIT period
  (~55ms) — so a live tick could be missed. **Fix:** wrap it in a 32× outer loop so the window is
  well over a tick period; a live ~18Hz PIT now reliably lands. (`timertst.com` passed because its
  dot-prints already made it run for seconds.) IRQ0 delivery itself was fine.

**Operational:** the XP `ntvdmex` account **password has been REMOVED** (2026-08-01) — boot now lands
on the desktop with no logon typing (earlier the expired/`ntvdmex1` password cost many cycles at the
"could not log you on" dialog; if that dialog ever reappears, just click OK and it proceeds).
QEMU is otherwise stable. **QMP `send-key` is badly laggy/unreliable** on this VM
(keys arrive seconds late and interleave; UK keyboard maps qcode `backslash`→`#`, real `\` is
`less`) — **drive the GUI by mouse instead**: `qmp.py click` (its 1024×768 assumption vs the 800×600
screen means scale native coords ×1.28), double-click `D:\selftest.bat`, and press F5 to force the
CD to remount after each `qmp.py cd` (the unique volume label alone isn't enough for an open window).
`selftest.bat` now ends in `pause` so the console holds the dumped `ntvdmhost.log` for screendump.

## Single next action

> **Work items are tracked in [GitHub Issues](https://github.com/MrMatthewLayton/ntvdmex/issues)**
> (epics = milestones M4–M8; ported 2026-07-20). The list below is the current priority ordering;
> the Issues tracker is the source of truth for status.

**M4 memory extensions are VALIDATED — the `selftest` VM gate is 8/8 ALL TESTS PASSED on the real
CPU** (XMS + EMS confirmed; the two test-side fails fixed — see *M4 VM gate* above). Remaining, in
order:

0. ~~FIX the graphics→text rendering bug~~ **FIXED + VM-CONFIRMED 2026-07-31 (GH #14)** — DAC palette
   not reloaded on mode set; see *Known host bugs* below.
1. **DPMI (GH #1 CLOSED; GH #2 active) — on branch `spike/dpmi-16bit-switch`. A WORKING DPMI 0.9 host
   runs UNMODIFIED clients on the real CPU (runs 35–60), incl. a multi-segment MZ `.EXE` and a
   third-party binary (Japheth's DPMIBACK).** The kernel `#GP`-reflect (old "increment 3b") is bypassed:
   client INT nn are patched → BOP at mode-switch and serviced in a PM loop. Since run 53 the active
   frontier is the **host PM INTERPRETER** (`src/host/v86interp.h`) — a C-runtime client (`i310102`)
   walled the kernel with a setaccess-to-code `#GP`, so we run PM in our own interpreter (no descriptor
   enforcement) and grow opcode coverage per the `DPMI-INTERP: unmodeled opcode` log. **HEAD = run 60
   (`0x91`–`0x97` XCHG AX,r16 — the accumulator short-form; off-VM 112/112 `interp_test.c` T47-T49, host
   `dpmi-harness-v54`, VM-confirmed 2026-08-05 — i310102 walked PAST run 59's `0x1f:0x157` XCHG wall,
   advanced 1308 → 1476 steps (`steps=0x5c4`), printed MORE `main()` output (`... returned NC, eax=`,
   mid hex-format) and exited clean (`STAGE2: complete`); runs 56 PUSH imm + 57 far RET + 58 LEAVE +
   59 PUSHF/POPF + 60 XCHG AX,r16 all proven). The new wall: `0x66 F7 /6` = DIV EDI (32-bit divide) at
   `0x1f:0x42` (`bytes=66 f7 f7 80 c2 30 80 fa`, a `DIV EDI; ADD DL,0x30; CMP DL,…` printf hex-digit
   loop).**
   Full narrative: [research/dpmi-under-ntvdmcontrol.md](research/dpmi-under-ntvdmcontrol.md) (runs 20–60).
   **RESUME = run 61: the `F7` group (`MUL`/`IMUL`/`DIV`/`IDIV`/`NEG`/`NOT`/`TEST imm`)** — meatier than
   the recent glue: `DIV`/`IDIV` use the `EDX:EAX` (`DX:AX`) pair and can `#DE` on div-by-zero/overflow
   (no trap path — clamp/bail, don't UB); model at least `DIV` (reg 6) + `MUL`/`NEG`/`NOT` neighbours;
   watch the `0x66` 32-bit width. VM-confirm via interactive `D:\i31run.bat` (`qmp.py` mouse-drive, NOT
   telnet — the headless autorun can't reach i310102; see [[vdm-host-test-harness]] / the research doc).
   Read the next `DPMI-INTERP: unmodeled opcode` line and follow it. Don't advertise `2Fh 1687` on `main`
   until a real binary runs clean (spike branch only).
2. After DPMI: M5 (Win16/WOW foundation).

## Testing harness — consolidated self-test (2026-06-09→11)

Per user direction, the manual run-one-test-at-a-time VM flow was consolidated into **one
self-checking program** as the regression gate (committed `bca00f3`):
- **`tools/dostest/selftest.asm/.com`** — runs every subsystem in a single VDM session and prints a
  `PASS`/`FAIL=nn` line each (DOS mem, file I/O, XMS, EMS, PIT timer, mouse, keyboard, video). It
  prints each test's **label before running it**, so a hang/fault leaves the culprit's label as the
  last visible line. Exit code = number of failures.
- **`tools/dostest/selftest.bat`** — one-click runner (copy host+selftest, set IFEO, run, dump log).
- **`scripts/build-test-iso.sh`** — builds the complete test CD (host + selftest + runtest menu +
  all `.com` tests + the QB demos) with a **unique volume label each build** (XP caches CD contents
  by label — a stale label silently serves the old disc).

**Status (2026-07-20): GREEN — `selftest` captured 8/8 ALL TESTS PASSED on the VM** (see *M4 VM gate*
above). The earlier "VM kept dropping" was an **expired XP password** stalling boot at a logon dialog,
not a QEMU crash (reset to `ntvdmex1`). QEMU does occasionally drop and need a relaunch (`xp-vm.sh
run`); on reboot the stale-password auto-logon shows a "could not log you on" message — dismiss it and
the desktop comes up. The off-VM batteries are all still green.

## Known host bugs

- ~~**No text rendering after a graphics→text mode switch.**~~ **FIXED (2026-07-31, GH #14).** The
  present/render path was never the problem — `vid_frame` re-derives the frame (dims + pixels +
  palette) from `st->mode` every tick and re-renders the text grid, so a return to mode 3 presents
  fine. The real cause was the **DAC palette**: text renders palette *indices* (0–15) into the
  framebuffer and the colours come from `st->pal[]` at present time; a mode-13h program reprograms
  the DAC with its image palette, and `INT 10h AH=00` set-mode never reloaded the default — so text
  after the switch was drawn in the leftover graphics palette (near-black → dead display). Real VGA
  reloads the default palette on every mode set; now `int10` case 0x00 calls `load_default_palette()`
  (16 EGA colours + grey ramp, factored out and shared with `vdd_video_reset`). Off-VM video battery
  still 34/34; host cross-compiles clean. **VM-CONFIRMED 2026-07-31** on the real CPU via
  `tools/dostest/modeswitch.com` (menu/`modeswitch.bat`): a program sets mode 13h, wipes the DAC to
  black, holds on a keypress, returns to text mode 3 and prints — the message renders clearly
  readable (pre-fix it was invisible). Test is hang-proof (blocks on `INT 16h` in each mode, no PIT
  dependency). `selftest`'s no-visible-switch workaround can now be relaxed.

### M3 follow-ups (closed, kept for reference)

Mode-12h pixel-plot speed is a closed question (the wall above) — **do not** keep adding interpreter
opcodes for it. Prior open work, all resolved:
1. ~~MOUSE end-to-end~~ **DONE (2026-06-09, VM-confirmed).** INT 33h now has a **host-drawn arrow
   cursor** (overlay in the present path when the hide-count is 0; reset→hidden, AX=1 show / AX=2 hide).
   `mousetst.com` (menu 13) confirms it: the arrow tracks the host mouse and holding the left button
   paints pixels (position + buttons via INT 33h AX=03). Note MOUSE.EXE itself only inits+shows+exits
   (no tracking loop, cursor-draw commented out) — mousetst is the real interactive test.
2. **Sweep the demos for any remaining INT 21h / INT 10h gaps** unrelated to 12h speed.
3. ~~Live PIT IRQ delivery~~ **DONE (2026-06-09, VM-confirmed).** PIT raises IRQ0 on real-elapsed-time
   clocking; the service loop synthesises the real-mode INT 08h dispatch (push FLAGS/CS/IP, vector via
   IVT[8]) when the guest's main-line IF is set. Key gotcha: at our control points we're almost always
   *inside a BOP stub* (CS==DOS_HDLR_SEG), so the LIVE EFLAGS IF is the handler's (cleared); the guest's
   real IF is the FLAGS the stub will IRET to, at **SS:SP+4** — gate on that. IVT[8]=`BOP 08; CD 1C;
   IRET` (bump 0040:006C, chain user-timer), IVT[1Ch]=`IRET`, IVT[1Ah]=BOP→BIOS time. `timertst.com`
   (menu 12) streams a dot per tick (~18 Hz). Minor caveat: the PIT clocks on the UI thread's WM_TIMER,
   so opening the window menu (a modal loop) pauses ticks. Window title is now "Windows XP Virtual DOS
   Machine - PROG.EXE".
4. *(Only if 12h speed ever becomes a priority)* hardware-accelerated VGA — full-screen handoff, or a
   dedicated headless secondary VGA card as a planar "coprocessor" (window + hardware VGA at once).
   Captured as future work in [research/hardware-vga-acceleration.md](research/hardware-vga-acceleration.md);
   not planned. (Also: JIT hot path, or pattern-recognising QB's putpixel — same doc.)

**Empirical basis (2026-06-09):** `tools/dostest/blitfast.asm` (menu option 11) draws the same random
filled rectangles as QB's BLIT but via the efficient idiom (write-mode 2 + Map Mask + one `REP STOSB`
per scanline → one fault/scanline). VM-confirmed visibly faster than BLIT — proving the 12h wall is the
QuickBasic per-pixel method, and real `REP`-based 12h software is already fast on the existing path.

**Testing strategy (unchanged):** off-VM is primary — `tools/dostest/run.sh` runs MCB **49/49**, bus
**22/22**, PIT **19/19**, input **20/20**, video **34/34**, **interp 63/63**, **XMS 36/36**, **EMS
30/30** instantly, no VM. The XP VM
is the manual integration gate, **driven headlessly via `scripts/qmp.py`** (screendump / CD hot-swap /
send-key / click / drag); `RUNTEST.BAT` re-copies the host per run and auto-opens `ntvdmhost.log` in
Notepad. **VM gotchas learned:** a program that *loops* (BUBBLES) leaves `ntvdmhost.exe` running and
**locks the file**, so RUNTEST's next copy fails ("being used by another process") and silently runs the
*old* host — close the Luna window first, or reset the VM, before re-staging. `system_reset` via QMP
reboots cleanly (CD stays mounted) but can occasionally drop QEMU — relaunch `scripts/xp-vm.sh run` if
`pgrep -x qemu-system-x86_64` shows it gone. Closing Luna windows via send-key is flaky; just ask the
user. *M2 follow-up (not blocking):* CSRSS multi-call arg recovery + exit-to-shell.

## Environment notes

- Git repo on `main`; remote is the private GitHub repo (no Wiki — `docs/` is canonical).
- Build host: macOS with Homebrew `mingw-w64` (i686, UCRT-default → we link no-CRT) + CMake + r2.
- **Never commit:** the XP ISO, `vm/`, `*.qcow2`, or `reverse/` (extracted MS binaries) — gitignored.
- The Bash tool is sometimes sandboxed (process/socket checks can read false-negative); use
  `dangerouslyDisableSandbox` for QEMU/QMP/`lsof` operations.
- **VM testing (resolved 2026-06-06): off-VM is primary; the VM is a manual gate.** The telnet loop
  *works* (a `memtest.com` round passed end-to-end in V86), but each round is ~1–2 min of silent
  flag-polling and slirp (user-mode NAT) can wobble (`Slirp: Failed to send packet`), so it reads as
  a hang — **not** the default. For real-V86 checks: boot (`scripts/xp-vm.sh run`), run
  `tools/dostest/gate.bat` in the interactive desktop, paste the verdict, power down via QMP
  (`system_powerdown`). `xp.py --wait` does a real login probe (the forwarded port accepts before
  telnet is up, so `nc -z 2323` is a false ready). Most allocator/DOS-logic work needs no VM at all —
  use `tools/dostest/run.sh` + `verify-memtest.sh`.
