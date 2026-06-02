# Project State — Living Handoff

> **This is the canonical resume point.** Update it at the end of every working session.

- **Last updated:** 2026-06-02
- **Phase:** M0 — Feasibility
- **Overall status:** 🟢 Two of the three M0 risks down: the shell preview **runs on XP SP3**,
  and **interception is proven** (Spike-002 — repointing the WOW `cmdline` launches our binary as
  the DOS VDM host). Remaining keystone: the V86 path via `NtVdmControl` (Spike-001).

## One-paragraph summary

We are building a drop-in replacement for Windows XP SP3 (32-bit) `ntvdm.exe` that runs
16-bit DOS and Win16 code on the **real CPU in V86 mode** by driving XP's existing kernel
VDM machinery through the undocumented `NtVdmControl` syscall. Interception is done by
**repointing the WOW registry keys** at our binary, so no signed system file is replaced.
Devices (video, sound, input, net) are virtualized and serviced by host calls through a
pluggable VDD model; video is blitted into a Luna-themed window.

## What is decided (see [decisions/](decisions/))

- **ADR-0001** — No CPU emulation; execute real-mode code in V86. *Accepted.*
- **ADR-0002** — Intercept via WOW registry repoint, not binary replacement (avoids WFP). *Accepted.*
- **ADR-0003** — Scope = DOS **+** Win16/WOW; Win16 deferred to M5 behind a clean seam. *Accepted.*
- **ADR-0004** — Reuse kernel VDM via `NtVdmControl` rather than a custom V86 driver. *Proposed — contingent on Spike-001.*
- **ADR-0005** — Target XP in a VM **and** bare metal; VM-first for the dev loop. *Accepted.*
- **ADR-0006** — Build with mingw-w64 (i686) cross-compiler, no C runtime. *Accepted.*

## What is built

- **Build system:** CMake + mingw-w64 cross toolchain (`cmake/toolchain-xp32-mingw.cmake`).
  `./scripts/build.sh` → `build/ntvdmex.exe`. Produces a ~20 KB standalone PE32 that imports
  only stock-XP DLLs (kernel32/user32/gdi32/comctl32), PE version stamped 5.01.
- **Shell preview (`src/`):** a fixed 80×25 DOS-style text console (light-gray on black, OEM
  raster font, blinking cursor) in a Luna-themed window. **Non-interactive on purpose** — it
  shows a prompt but processes no input. This is the shell the V86/DOS core will later feed.
  - `src/main.c` — window, message loop, blink timer.
  - `src/console.c/.h` — the text-grid model + GDI rendering (future sink for INT 10h / B800
    text writes).
  - `src/runtime.c` — freestanding entry point + `mem*` (no CRT; see ADR-0006).
  - ✅ **Confirmed running on XP SP3** (after fixing an SxS manifest trap — XP rejects a prolog
    XML comment; the manifest is now minimal/comment-free).
- **Spike-002 harness (`tools/wowprobe/`):** `wowprobe.exe` (logs how XP invokes the VDM host)
  + `dosstub.com` (4-byte 16-bit trigger). Built and XP-clean; **awaiting the VM run**.

## What is open / unresolved

- ✅ **Interception — proven** (Spike-002): repointing `Control\WOW\cmdline` launches our binary
  as the DOS VDM host (after reboot — boot-cached). Learned the target program is **not** on the
  command line; it comes via the CSRSS/VDM channel + `NtVdmControl` — which is the next thing to
  recover (Spike-001).

- **THE keystone question:** Can a *non-Microsoft* binary fully drive *real XP SP3*
  `ntoskrnl`'s VDM via `NtVdmControl`, conforming to the undocumented VDM_TIB / address-space
  layout it expects? No open-source project proves this (ReactOS uses CPU emulation, not V86).
  → tracked as **[Spike-001](spikes/spike-001-v86-keystone.md)**.
- Reference posture: lean on ReactOS for DOS/VDD/WOW *logic & structures*; use disassembly
  of the shipping XP `ntvdm.exe`/`ntoskrnl` for the V86/`NtVdmControl` contract; use Linux
  `dosemu` as the conceptual analog. (See [research/reference-projects.md](research/reference-projects.md).)

## Single next action

**Spike-002 is done (interception proven).** The next keystone is **Spike-001 — the V86 path via
`NtVdmControl`**. Spike-002 sharpened its target: the VDM support process is launched with *no*
useful argv, so a real host must (a) get the program-to-run + VDM state from the CSRSS/VDM LPC
channel and (b) drive V86 via `NtVdmControl`. First sub-steps: capture the default `cmdline`
format (from `wow-backup.reg`), and recover how `ntvdm` queries CSRSS for its DOS program
(disassemble `kernel32!BaseCheckVDM` + `basesrv.dll` VDM path) alongside the `NtVdmControl`/
`VDM_TIB` contract. Dev-loop aids to stand up first: a host↔guest file bridge (so results come
back automatically) and the import-allowlist build check.

**Then the keystone — Run Spike-001** (V86): a ~200-line usermode host that registers via the WOW
repoint, reserves the low address space, calls `NtVdmControl(VdmInitialize)` →
`VdmStartExecution`, and executes one trivial real-mode program under real V86 with the
GP-fault reflection landing in our handler. If it works → promote ADR-0004 to Accepted and
proceed to M1. If it fails → pivot to the custom-driver path (new ADR).

## Environment notes

- Git repo initialised; remote is the private GitHub repo (no Wiki — `docs/` is canonical).
- Build host: macOS with Homebrew `mingw-w64` (i686, UCRT-default → we link no-CRT) + CMake.
- Dev target: XP SP3 32-bit guest in a VM first; validate on bare metal before M1 exit.
- **XP test bench:** local QEMU VM, HVF-accelerated (real V86), launched via `scripts/xp-vm.sh`
  (period-correct hardware; see `research/xp-test-vm.md`). ISO + `vm/` disk are gitignored.
- **No Wine on the build host**, so Windows GUI output can't be rendered on macOS — visual
  checks happen on the XP VM.
