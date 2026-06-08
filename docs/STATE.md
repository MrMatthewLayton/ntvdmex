# Project State — Living Handoff

> **This is the canonical resume point.** Update it at the end of every working session.

- **Last updated:** 2026-06-08
- **Phase:** **M3 — device model + video (GRAPHICS DONE; now expanding the DOS layer for real apps).**
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
  **Now:** running real DOS apps — the first one works: **`PALETTE.EXE` (QuickBASIC 4.5, SCREEN 13)
  renders in the Luna window**, after adding INT 21h **AH=06** (QB's console path) + INT 10h **VGA
  capability queries** (AH=1B/1A/12/11, so QB accepts mode 13h). Next code-side = **mode-12h MOV-store
  decoder** (5 planar demos run but don't paint) + **INT 33h mouse**, then sweep the rest.
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

## Single next action

**Make the rest of the demos render.** Status by class: **SCREEN 13** renders — PALETTE ✓, BLIT ✓,
**CAVE ✓ (animates** after the port-3DA vertical-retrace fix; demos poll 3DA to pace frames). Window
caption is now **"Virtual MS-DOS Prompt - PROG.EXE"**. **Text** (VS87) should render via AH=06.
**SCREEN 12** demos run without the QB error but paint nothing — they need the work below.

1. **mode-12h MOV-store decoder** (blocks 5 of 10 demos) — `host_try_mem()` in `src/host/main.c` only
   decodes STOSB/STOSW (REP); QuickBASIC paints planar mode 12h with **MOV** stores. Extend the
   faulting-store decoder to MOV/ModRM forms so BLIT/BOUNCEBX/BUBBLES/MATRIX_1/2 paint.
2. **INT 33h mouse** — `MOUSE.EXE` needs it (reset/show/get-position; UI mouse → INT 33h state).
3. **Sweep the remaining demos** and fix whatever each log shows (more INT 21h: FindFirst/Next 4E/4F,
   DTA 1A/2F, free-space 36; x87 FPU only if a demo faults on an FPU opcode — none have so far).
4. **Live PIT IRQ delivery (needs VM):** PIT INT 08h/1Ah as IVT BOP stubs + real IRQ0→INT 8 via ICA.

**Testing strategy (unchanged):** off-VM is primary — `tools/dostest/run.sh` runs MCB **49/49**, bus
**22/22**, PIT **19/19**, input **15/15**, video **33/33** instantly, no VM. The XP VM is the manual
integration gate, now **driven headlessly via `scripts/qmp.py`** (screendump / CD hot-swap / send-key
/ click / drag); `RUNTEST.BAT` (`tools/dostest/runtest-demos.bat`) is a menu launcher that re-copies
the host per run and auto-opens `ntvdmhost.log` in Notepad. Reliable VM-driving primitives: taskbar-
button click to raise/focus a window then Alt+F4; Alt+Space→N to minimise; ≥0.15s key spacing;
forward-slash paths; wait-loops must use `pgrep -x qemu-system-x86_64` (not `-f`, which self-matches).
*M2 follow-up (not blocking):* CSRSS multi-call arg recovery + exit-to-shell; `mem.exe` past Parse Error 1.

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
