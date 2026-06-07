# Project State — Living Handoff

> **This is the canonical resume point.** Update it at the end of every working session.

- **Last updated:** 2026-06-07
- **Phase:** **M3 — device model + video (STARTED).** M2 (DOS kernel) is closed: M2.1–M2.6 DONE in the
  clean `src/` host. M3 kickoff **retired the `tools/vdmhost` spike** and decided the **pluggable VDD
  architecture** ([ADR-0008](decisions/0008-pluggable-vdd-model.md) + [research/vdd-architecture.md](research/vdd-architecture.md)):
  clean `ntvdd.h` ABI + bus, built-in VDDs, DirectDraw (windowed + full-screen), VGA + VESA; the two
  binaries merge into one windowed host. **Slices 1a (bus 22/22) + 2 (PIT 19/19) off-VM; 1b (I/O
  trap, event 0) + 3 (DirectDraw) VM-CONFIRMED 2026-06-07** — `ioprobe.com` routed 4×OUT+IN through
  the real PIT VDD on the CPU (reload→0x1234, IN→0x34, exit 0x34); `present_demo.exe` rendered.
  **Video VDD text mode 3 done off-VM** (slice-4, 23/23). **MERGE DONE + VM-CONFIRMED:** ntvdmhost is
  now a windowed host (UI thread + present_ddraw + frame timer; V86 on the VdmInitialize thread); DOS
  console (INT 21h) + INT 10h route through the video VDD → DirectDraw — `hello.com` painted text in
  the Luna window on the real CPU. (Fixed present_ddraw to pack pixels to the surface's real depth;
  XP/Cirrus is 16bpp — that was the vertical-striping bug.) **Keyboard input DONE + VM-CONFIRMED**
  (INT 16h VDD + INT 21h AH=01/07/08/0A + UI WM_CHAR; interactive keytest.com typed in the window,
  Enter/Backspace/ESC); **authentic IBM VGA 8×16 ROM font** now used. Next code-side = extended keys
  (arrows/F-keys) + mouse, or graphics modes (13h → needs A0000/B8000 mapped as RAM).
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

## Single next action

**M3 is underway.** Slices 1a (VDD bus) + 2 (PIT timer) are done and proven off-VM; the next step is
**slice-1b — wire the bus into `v86_run`**, which needs a VM spike.

**Testing strategy (unchanged, decided 2026-06-06):** off-VM is the primary loop
(`tools/dostest/run.sh` now runs the MCB **49/49**, VDD bus **22/22**, and PIT **19/19** batteries
instantly, no VM); the XP VM is a **manual integration gate** via `tools/dostest/gate-clean.bat`
(interactive desktop, read/paste the verdict — no telnet).

Next work:
1. **VM GATE (your run, at the interactive desktop):** `./scripts/stage-gate.sh`, then in the VM
   `gate-clean.bat ioprobe.com` → expect `IO out/in` trace + `STAGE2: complete` errorlevel 0x34
   (confirms IOPL-0 IN/OUT reflects as event 2 and resumes — slice-1b proven; bus+PIT live). If it
   stops with `event=0x...`, paste the `info=`/`bytes@CS:IP`/`VTIB[5A8..]` dump and I'll adjust. Also
   run `present_demo.exe` to eyeball the windowed + fullscreen DirectDraw blit (Alt+Enter / Esc).
   *(All slices 1b/3/4/merge/keyboard are now VM-confirmed; the VM gate is a hot-swapped CD —
   `vm/test.iso` via QMP — read back with `screendump`; force XP to re-read by re-opening D:.)*
2. **Next code-side options:** (a) **graphics modes** — map the A0000/B8000 aperture as RAM in
   `v86_setup_memory` so direct-framebuffer programs work, render mode 13h (320×200×256) + DAC, then
   VESA banked; (b) **extended keys + mouse** — WM_KEYDOWN→scancode (ascii=0) + INT 33h.
3. **VDD interrupt/IRQ delivery (needs VM):** install PIT INT 08h/1Ah as IVT BOP stubs (like INT 21h)
   + real IRQ0→INT 8 via the kernel ICA; then INT 08h's INT 1Ch chain + PIC EOI (so the PIT ticks live).
4. *M2 best-effort follow-up (not blocking):* CSRSS multi-call arg recovery + exit-to-shell notify;
   confirm `mem.exe` past `Parse Error 1`. VM gate: `gate-clean.bat argtest.com HELLO` / `mem.exe`.

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
