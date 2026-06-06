# Project State — Living Handoff

> **This is the canonical resume point.** Update it at the end of every working session.

- **Last updated:** 2026-06-06
- **Phase:** M2 — DOS kernel (M0 + M1 done); **M2.4 in progress — committed WIP + off-VM test harness**
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

## M2.4 — DOS memory management (IN PROGRESS — committed WIP + test harness, 2026-06-06)

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

**Known gap (pinned by test T9):** `free()` coalesces *forward only* and `alloc()` never merges
adjacent free blocks — real MS-DOS merges during the alloc walk, so two adjacent free blocks can't
jointly satisfy a request real DOS would. A **`merge-on-alloc`** fix is the next M2.4 correctness item.

**Real-code evidence (partial, on the VM):** a real `C:\WINDOWS\system32\mem.exe` parsed by our MZ
loader and `AH=4Ah` **resize succeeded** (`seg=0x100 -> 0x174e`, no CF). **Not yet re-run** against
the binary that also handles `44h`/`63h`; mem.exe ends in `Parse Error 1` (suspect missing INT 21h
surface / env block, ties into M2.5). See [log/2026-06-05.md](log/2026-06-05.md),
[log/2026-06-06.md](log/2026-06-06.md).

## Single next action

1. ✅ **Done:** M2.4 committed (`4aa6f44`); off-VM Layer-1 test harness green, 30/30 (`bf3defe`).
2. **Fix the dev loop** before more VM rounds — it's the bottleneck. One long-lived telnet session,
   or a guest-side batch that does push→trigger→collect into one result file, or a QMP/file-drop
   channel (see Environment notes).
3. **Layer 2 — self-checking `memtest.com`** (in-guest): a hand-assembled `.COM` that runs the
   alloc/resize/free sequence, checks `CF`/`AX` itself, prints PASS/FAIL and sets errorlevel = #fails.
   Pairs with the batch loop (the verdict reduces to one byte + an errorlevel). Generator under
   [`tools/dostest/`](../tools/dostest/).
4. **Re-run mem.exe** (now handles 44h/63h); chase `Parse Error 1` (likely missing INT 21h surface /
   env block → M2.5). Consider a simpler real `.EXE` as the cleaner first end-to-end M2.4 target.
5. **M2.4 correctness:** add `merge-on-alloc` (the known gap pinned by test T9), then re-green the battery.

Deferred: **M2.5 cmdline recovery** (`GetNextVDMCommand` never populated `CmdLine`; PSP tail is empty
for now); **M2.6 promote spike → `src/`** (adopt `tools/dostest/dos_mcb.h` as the shared allocator).

## Environment notes

- Git repo on `main`; remote is the private GitHub repo (no Wiki — `docs/` is canonical).
- Build host: macOS with Homebrew `mingw-w64` (i686, UCRT-default → we link no-CRT) + CMake + r2.
- **Never commit:** the XP ISO, `vm/`, `*.qcow2`, or `reverse/` (extracted MS binaries) — gitignored.
- The Bash tool is sometimes sandboxed (process/socket checks can read false-negative); use
  `dangerouslyDisableSandbox` for QEMU/QMP/`lsof` operations.
- **⚠️ The VM dev loop is flaky/slow and is now the bottleneck (2026-06-05).** QEMU's host-port
  forward `localhost:2323` accepts the TCP connect the instant QEMU starts — *before* the guest
  telnet service is up — so `nc -z 2323` is a false "ready"; only a real `xp.py` login proves the
  guest is alive. `xp.py` logins are increasingly slow and intermittently hang (multi-minute stall,
  0 bytes, then everything arrives at once on kill) — XP telnet allows few concurrent sessions and
  refuses rapid reconnects. **Before more test rounds, fix the loop:** one long-lived telnet session
  rather than reconnect-per-command, or a guest-side batch that runs push→trigger→collect and writes
  one result file, or a QMP/file-drop channel instead of telnet.
