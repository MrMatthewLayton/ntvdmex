# NTVDMEX

**New Technology Virtual DOS Manager, Extended** — a from-scratch, drop-in replacement for
`ntvdm.exe` on **Windows XP SP3 (32-bit)** that runs legacy 16-bit DOS and Win16 software on
the **real CPU**, not a software emulator.

> **Status:** 🟢 V86 proven — into **M2 (DOS kernel)**. The make-or-break assumption holds: a real
> DOS `.COM`, loaded off disk, runs on the real CPU in **Virtual-8086 mode** via `NtVdmControl` and
> prints "Hello, World" through our own INT 21h handler — launched transparently when XP starts a
> 16-bit program. **M0** (feasibility) and **M1** (minimal V86 host) are done; the work so far lives
> in the `tools/vdmhost` spike. See [`docs/STATE.md`](docs/STATE.md) and [`docs/ROADMAP.md`](docs/ROADMAP.md)
> for live status.

## What it is

When Windows XP launches a 16-bit program, it hands the image to a support process called
NTVDM. NTVDMEX replaces that process with our own implementation. The defining choice is
**execution, not emulation**: 16-bit real-mode code runs in the CPU's **Virtual-8086 (V86)
mode** on real silicon — the same mechanism the original NTVDM uses on 32-bit x86. Software
CPU emulators (DOSBox, ReactOS's Fast486) are explicitly *not* what this project is.

Hardware that DOS/Win16 code expects (video, sound, input, timers, networking) is
**virtualized** and serviced by calls to the host, through a **pluggable device model** so
third parties can supply their own backends.

## Goals

- **Real CPU execution** of 16-bit code via V86 — no CPU emulation.
- A **true replacement** for NTVDM: every 16-bit launch routes to us automatically, not via a
  right-click "run with…" and not as a separate DOSBox-style app.
- **DOS *and* Win16** support (Win16/WOW sequenced after the DOS core is working).
- **Virtualized devices** (mouse, keyboard, sound, graphics) backed by host calls.
- **Pluggable**: a documented driver/VDD interface so others can hook in their own devices.
- Windows that **fit the XP Luna theme**.
- Runs both on **XP-era bare metal** and in **XP-32 virtual machines**.

## How it works (intended architecture)

| Concern | Approach |
|---------|----------|
| Becoming the system NTVDM | **IFEO `Debugger` redirect** on `ntvdm.exe` (`…\Image File Execution Options\ntvdm.exe\Debugger` → our binary) — registry-only, no signed-file replacement, so Windows File Protection is never triggered. (The original WOW `cmdline` repoint was disproven — XP validates the host image; see [ADR-0007](docs/decisions/0007-intercept-via-ifeo-debugger.md).) |
| Executing 16-bit code | Enter **V86 mode** by reusing XP's kernel VDM machinery via the undocumented `NtVdmControl` syscall (custom kernel driver held as a fallback). |
| DOS environment | Re-implemented DOS kernel: INT 21h, PSP/FCB, memory (MCBs), loaders, DPMI/XMS/EMS. |
| Win16 | A WOW layer (`krnl386`/`user`/`gdi` hosting + 16↔32 thunking) built on the same foundation. |
| Devices | Trap-and-service model exposed as **pluggable VDDs**; video blitted into a Luna-themed window. |

### Is this even possible?

Yes — and code signing is not the obstacle it appears to be. XP-32 does not verify user-mode
EXE signatures, and kernel driver signature enforcement is a Vista-x64+ feature. We don't even
replace the signed `ntvdm.exe`; we redirect to ours via the registry. The genuine open risk is
narrower: whether a *third-party* binary can drive XP's kernel VDM into V86 via `NtVdmControl`
— something no open-source project demonstrates (ReactOS sidesteps it with emulation). Proving
that is the project's first spike. Full reasoning in
[`docs/research/signing-and-wfp.md`](docs/research/signing-and-wfp.md).

### Reality check: graphics

The original aim of accessing VGA/VESA "bare metal over emulation" is constrained by the fact
that the XP GUI owns the display. The realistic path — like windowed NTVDM — is to
**virtualize video and blit into a themed window**; true bare-metal full-screen is a later,
cooperative path, not direct hardware access. Tracked as risk **R2** in
[`docs/risks.md`](docs/risks.md).

## Scope & platform

- **Target:** Windows XP SP3, **32-bit only** (V86 exists only in 32-bit mode).
- **Environments:** real hardware **and** VMs (VM-first for development).
- **In scope:** DOS, then Win16/WOW, virtualized devices, pluggable drivers, Luna theming.
- **Out of scope (for now):** 64-bit Windows, VT-x hypervisor execution, true bare-metal GPU
  access while the GUI is running.

## Building

The first runnable artifact exists: a **Luna-themed shell preview** — a fixed 80×25 DOS-style
text console in an XP-themed window, with a deliberately **non-functional** command line. It
locks down the toolchain and the window/console shell ahead of the V86 work.

Built with a **mingw-w64 (i686) cross-compiler driven by CMake**, linked with **no C runtime**
so the binary depends only on the Win32 DLLs that ship with XP (see
[ADR-0006](docs/decisions/0006-build-toolchain-mingw-no-crt.md)).

```sh
# prerequisites (macOS): brew install mingw-w64 cmake
./scripts/build.sh
#   → build/ntvdmex.exe   (PE32, ~20 KB, standalone; copy to an XP SP3 VM to run)
```

The window is a Windows GUI app, so it can only be *seen* on Windows; the cross-build verifies
correctness (valid PE32, XP-only imports, 5.01 version stamps, embedded manifest) but not the
visual result. Full toolchain notes and the XP-compatibility traps:
[`docs/research/build-toolchain.md`](docs/research/build-toolchain.md).

### Repository layout

| Path | What |
|------|------|
| `src/` | Shell-preview sources: `main.c` (window/loop), `console.c/.h` (text-grid model + GDI render), `runtime.c` (no-CRT entry + `mem*`) |
| `res/` | `ntvdmex.rc` + `ntvdmex.manifest` (version info; Common-Controls 6.0 → Luna visual style) |
| `cmake/` | `toolchain-xp32-mingw.cmake` — the XP-32 cross toolchain |
| `scripts/` | `build.sh` convenience wrapper |
| `docs/` | The canonical knowledge base (see below) |

## Documentation

`docs/` is the single source of truth (this is a private, free-plan repo with no Wiki). Start
with **[`docs/STATE.md`](docs/STATE.md)** to see where things stand, then:

- [`docs/ROADMAP.md`](docs/ROADMAP.md) — milestones M0–M8
- [`docs/decisions/`](docs/decisions/) — Architecture Decision Records (the *why*)
- [`docs/research/`](docs/research/) — findings, tagged by confidence
- [`docs/spikes/`](docs/spikes/) — time-boxed experiments (Spike-001 is the keystone test)
- [`docs/risks.md`](docs/risks.md) — risk register
- [`docs/GLOSSARY.md`](docs/GLOSSARY.md) — NTVDM/V86/WOW terminology

## References

ReactOS (DOS/VDD/WOW logic), Linux `dosemu` (the closest V86-via-kernel analog), DOSBox /
86Box (device behaviour), and disassembly of the shipping XP binaries (the only ground truth
for the `NtVdmControl` contract). See
[`docs/research/reference-projects.md`](docs/research/reference-projects.md).

## License

[MIT](LICENSE) © 2026 Matthew Layton.
