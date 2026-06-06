# Project State — Living Handoff

> **This is the canonical resume point.** Update it at the end of every working session.

- **Last updated:** 2026-06-06
- **Phase:** M2 — DOS kernel (M0 + M1 done); **M2.4 DONE**; **M2.6 IN PROGRESS — clean `src/` host compiles + links (KERNEL32-only imports); only the VM gate (Hello World) remains**
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

## What is built

- **`tools/vdmhost/` — the spike that proves M0+M1+M2.1–M2.3.** The full pipeline above: IFEO host →
  CSRSS handshake → V86 → DOS process (PSP, 640KB, `.COM`/`.EXE` loader) → INT 21h/BOP service loop
  (console + Win32-backed file I/O). Tested on the VM: `hello.com`/`testps.com` (`.COM` + PSP command
  tail), `filewr.com` (creates/writes/reads a real disk file), `helloexe.exe` (MZ with a relocation).
  This is throwaway-grade experiment code, *not* the clean host.
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
- **Slice 4 ⬜ (only thing left):** one `gate.bat` run — the clean host runs Hello World in V86 →
  **M2.6 exit met**. Needs the VM (runtime correctness of the ported V86 glue proves out only there).

The clean host is built and XP-safe; what's unverified is the *runtime* behaviour of the ported V86/NT
glue. To gate it (slice 4): register `ntvdmhost.exe` as the IFEO `Debugger` on `ntvdm.exe` (in place
of the spike) and run `gate.bat`; the clean host logs to `C:\ntvdmex\ntvdmhost.log`.

## Single next action

**M2.4 is done** (allocator validated end-to-end). **Testing strategy (decided 2026-06-06):**

1. **Off-VM is the primary loop** — `tools/dostest/run.sh` (30-case `dos_mcb.h` battery) and
   `tools/dostest/verify-memtest.sh` (dosbox-x) validate allocator logic + the test program
   instantly and reliably, no VM. Use this for routine work.
2. **The XP VM is a manual integration gate** — for real-V86 confidence, run
   `tools/dostest/gate.bat` *inside the VM* (interactive desktop) and read/paste the verdict.
   Reliable + observable, no telnet. (`scripts/dostest.sh` + the logon agent are an optional
   automated telnet path that passed a live round, but it's slow and reads as a hang — not default.)

Next work:
3. **M2.6 slice 4 (only remaining):** gate the clean host on the VM — register `ntvdmhost.exe` as the
   IFEO `Debugger` and run `gate.bat`; confirm Hello World in V86 → M2.6 exit met. Slices 1–3 done:
   clean host compiles + links, KERNEL32-only imports, off-VM battery 42/42. (See the M2.6 section.)
4. **Re-run mem.exe** (now handles 44h/63h); chase `Parse Error 1` (missing INT 21h surface / env
   block → M2.5). *Needs a VM gate — investigate via `gate.bat` with mem.exe as the target.*
5. **M2.5 cmdline recovery** (`GetNextVDMCommand` never populated `CmdLine`; PSP tail is empty);
   **M2.6 promote spike → `src/`** (adopt `tools/dostest/dos_mcb.h` as the shared allocator, deleting
   the spike's now-duplicated inline copy).

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
