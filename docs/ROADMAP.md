# Roadmap

Milestones are ordered by **risk and dependency**, not by feature glamour. Each milestone ends in
something runnable/observable. Win16 is intentionally late: it is built on the same V86 + DOS
foundation as everything before it.

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

## M3 — Device model + video/input 🟡 STARTED
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
- [ ] Video VDD: **VESA VBE 2.0** (banked) — 0x101/0x103/0x111 modes, INT 10h AX=4F00/01/02/05.
- [ ] Planar mode 12h (640×480×16) — last/optional (latches + write modes).
- [ ] VDD interrupt delivery: install VDD INT handlers as IVT BOP stubs + real IRQ0→INT 8 via the
  kernel ICA (so the off-VM-proven PIT ticks live; needs the VM).
- [ ] **Sound VDD** stub (the third device proving the ABI generalises; full audio is M7).
- **Exit:** a DOS app with a text-mode UI (and a VGA graphics demo) runs in a themed DirectDraw
  window, driven entirely through the pluggable VDD interface.

## M4 — Memory extensions ⬜
- [ ] XMS, EMS, and **DPMI** (protected-mode DOS extenders, e.g. DOS/4GW titles)
- **Exit:** a DPMI/DOS-extender game or tool runs.

## M5 — Win16 / WOW foundation ⬜
- [ ] WOW bootstrap (`wowexec` analog), 16-bit `krnl386`/`user`/`gdi` hosting
- [ ] NE loader, 16-bit module/segment management
- **Exit:** a trivial Win16 .EXE loads and reaches its message loop.

## M6 — Win16 thunking ⬜
- [ ] 16:16 ↔ flat pointer translation; generic/flat thunks
- [ ] USER/GDI 16-bit objects mapped to Win32 handles; message bridging
- **Exit:** a real Win16 GUI app runs and paints.

## M7 — Peripheral VDDs ⬜
- [ ] Sound, networking, serial/parallel, etc., as pluggable VDDs (host-backed)
- [ ] Bare-metal vs virtualized device strategy per [risks.md](risks.md)

## M8 — Polish & SDK ⬜
- [ ] Pluggable VDD/driver SDK + docs for third-party developers
- [ ] Luna theming pass, full-screen story, installer/registration tooling
