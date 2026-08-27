# Project state — start here

> **This is the canonical resume point.** If you have never seen this project before, read
> this file top to bottom and you will know where it is, what works, what does not, and
> what to do next.

- **Last updated:** 2026-08-26 (session 30)
- **Branch:** `m9/completeness`
- **Tracker:** [140+ issues](https://github.com/MrMatthewLayton/ntvdmex/issues) — reconciled against the repo on 2026-08-26 (`tools/gh/backfill.py` is the manifest)
- **Knowledge base:** the [wiki](https://github.com/MrMatthewLayton/ntvdmex/wiki)
- **Day-by-day history:** [`docs/log/sessions/`](log/sessions/)

---

## What this is

**NTVDMEX is a replacement for `ntvdm.exe` on 32-bit Windows XP SP3.** It runs DOS
programs by executing 16-bit code on the **real CPU** in Virtual-8086 mode — reusing the
NT kernel's own VDM machinery through the undocumented `NtVdmControl` syscall — not by
emulating a CPU. It is not DOSBox and it is deliberately not a fork of anything.

**The goal:** install it so that every MS-DOS *and* Win16 launch routes to NTVDMEX and
stock `ntvdm` lies dormant. Not by overwriting `System32\ntvdm.exe` — that is Windows File
Protection territory — but by an Image File Execution Options `Debugger` value on
`ntvdm.exe`, which achieves the same routing and is reversible with one `reg delete`.

**The bar it was built against:** run real DOS games well — Doom, Skyroads, ZAR — with
flawless sound.

---

## Where it actually is

### ✅ Working, and confirmed by hand on real hardware

| | Status |
|---|---|
| **Doom** | **Fully playable** — 3D rendering, status bar, menus, PCM + MIDI, keyboard **and mouse**. Runs its own 32-bit code through DOS/4GW on real silicon. |
| **Skyroads** | **Fully playable** — menus, Controls, level select, in-game. |
| **MS-DOS 6.22 `COMMAND.COM`** | Runs as a guest: prompt, line editing, internals (VER/VOL/CLS/ECHO/SET/TYPE/COPY/DIR/EXIT), and an external program EXEC'd and **returned from**. |
| **DOS API** | 103 INT 21h functions. XMS 3.0, EMS (LIM 4.0). |
| **Video** | Text (authentic IBM ROM font), mode 13h, mode 12h planar, VESA banked. Windowed GDI + exclusive-fullscreen DirectDraw, Luna-themed. All ten QuickBASIC demos render with **zero pixel defects**. |
| **Sound** | SB16 PCM at **99.999%** delivery, clean-room OPL2/OPL3 FM (MIT, Nuked used only as a black-box oracle), MPU-401 MIDI, PC speaker. |
| **DPMI** | 0.9 host running unmodified third-party clients; real-CPU protected mode; 32-bit DOS/4GW. |
| **Host UI** | Menu bar, status strip (program \| 16/32-bit \| Real/Protected mode \| capture state), six-tab Settings dialog backed by `HKCU\Software\NTVDMEX`. |

### ❌ Not working — and these are the honest blockers

| | Why it matters |
|---|---|
| **Win16 / WOW — loader done, nothing executes yet** | `ntvdm.exe` is *also* the host for every 16-bit **Windows** program. The NE loader now loads, relocates and binds the **whole** XP WOW module set on real hardware — but nothing is executed yet, and there is no 16:16↔flat thunking. Since interception is an IFEO key on `ntvdm.exe`, and Win16 launches go through `ntvdm.exe` too, **installing NTVDMEX permanently would break every 16-bit Windows app today**. → [#128](https://github.com/MrMatthewLayton/ntvdmex/issues/128) |
| **Console/stdio integration** | DOS output is buffered and flushed to `CONOUT$` at exit, so shell redirection and piping are bypassed and every DOS program pops a window. Blocks non-interactive use. |
| **In-guest redirection** | `echo x > file` writes to the screen and leaves the file 0 bytes. Three fixes attempted, all at the wrong end. |
| **No INT 13h / INT 25h / 26h** | No direct disk access. |
| **No TSRs** | AH=31h is handled but residency is explicitly *not* honoured (and says so, rather than pretending). |
| **`MEM.EXE` reports wrong figures silently** | The "runs but lies" class — the most expensive kind. |
| **ZAR** | Needs VBE 2.0 hi-colour + linear framebuffer. |
| **40 of 46 settings** | Stored in the registry, honoured by nothing. `settings_apply()` in `src/host/main.c` is the honest list of what actually works. |

---

## Next actions, in order

> ⚠️ **The plan changed on 2026-08-26.** #129 was going to make "leave it installed"
> safe by handing Win16 launches back to stock ntvdm. **That is impossible** — measured
> three ways (see #129). Windows validates the VDM image's identity, so a renamed copy is
> refused outright, and the real name re-enters us through the IFEO hook. So there is no
> safe install story until WOW exists, and #128 moved onto the critical path.

1. **[#128] WOW / Win16 — IN PROGRESS, and the loader half is DONE.** On real
   hardware the **entire XP WOW module set loads, gets LDT selectors and binds**:
   krnl386 + system/keyboard/mouse/sound/comm drivers + gdi + user + shell + toolhelp +
   wowexec. Every import resolves; 27 descriptors installed and confirmed by `LAR`
   readback. Site counts match the off-VM battery to the digit (KERNEL 495, GDI 781,
   USER 1269, WOWEXEC 144). Evidence in `docs/research/evidence/wow-bind-rig.txt`;
   `src/wow/ne.h` + a 209-check battery over all 15 real binaries.
   ⚠️ **krnl386 is a LIBRARY, not a program** — no stack of its own, and its `CS:IP` is a
   DLL *init* entry. Do not jump to it. Bootstrap: init krnl386 → user + gdi → run
   **wowexec.exe** (the PROGRAM) → wowexec launches the app.
   ⚠️ **Load every module, assign every selector, then relocate ONCE.** Relocation is not
   idempotent: a chained record's next site is the word *at* the current site.
   ⚠️ **Its init entry demands `AX == 0x4B4F` ('OK')** and it is a **DPMI client** — not
   the documented Win16 `LibMain` convention. Measured by disassembly.
   ⚠️ **LDT indices below `DPMI_LDT_RESERVED` are force-typed to data by
   `dpmi_install()`** — WOW's first allocation is a CODE segment and silently became
   data until the `LAR` readback caught it.
   ⚠️ **krnl386's init entry runs in V86, NOT protected mode.** Proven three ways from
   its own code: it does `mov ax,es / shl ax,4` (a selector shifted by 4 is nonsense), it
   stores through `cs:` (never legal in PM), and at `c0c2` it calls `INT 2Fh 1687` and
   **switches itself** — then redoes that same `cs:0x30` store through a DPMI `000A`
   alias. It is a V86 program that becomes a 16-bit DPMI client, which is why `0002`
   (paragraph → selector) is the function it calls most.
   ★ **krnl386 RUNS.** On real hardware it is entered in V86 at `0141:c02b` with
   `AX=0x4B4F`, reads the DOS list of lists, finds our DPMI host via `INT 2Fh 1687`,
   **switches itself into 16-bit protected mode**, and takes a `000A` alias of its own
   CS — every step matching the disassembly instruction for instruction.
   It used to stop with `NTVDM KERNEL: Inadequate DPMI Server` because we refused
   `INT 2Fh 168A`, the **"MS-DOS" vendor-specific API — which is REQUIRED**, not
   optional (an earlier note in session 30 says otherwise and is corrected in Part 9).
   ⚠️ **A PM guest cannot reach the IVT**, so any `INT nn` absent from the patcher's
   list stays a raw `CD nn` and silently terminates the VDM. `0x2F` was missing.
   ★ **The vendor API is implemented and krnl386 runs on through it.** Measured off
   stock: `168A` is PM-only and returns a **writable selector onto the descriptor
   table** (verified two ways — the descriptor at `window[CS & 0xFFF8]` decodes to the
   same base DPMI `0006` reports for CS, with the right access byte). Our own LDT is
   **not** user-mapped, so we hand krnl386 a **shadow** that is reconciled into the real
   LDT on entry to any PM interrupt service. Confirmed necessary and sufficient on
   hardware: krnl386 writes a descriptor directly (`base=0x400`, the BDA) and the sync
   installs it. `INT 31h 000D` (allocate specific descriptor) added for the same reason.
   The private `INT 31h` family is decoded too: **`04F2` = "commit CX descriptors from
   selector BX"** (read off its eight call sites — so stock needs a flush as well, and
   our shadow is the *same* design, not a workaround) and **`04F1` = the private twin of
   `0000`**. Both implemented, plus `000D`. krnl386 now gets through `d762` and `0x6763`.
   ★★ **NEXT, AND IT IS STRUCTURAL: krnl386 CALLS INTO A 32-BIT COMPANION VIA BOPs.**
   It halts at `seg1:0x3026` on `C4 C4 53` — a **native** BOP (in the file, unrelocated,
   not one our INT patcher planted). seg1 holds **13** of them: codes `0x51`×1, `0x53`×1,
   `0x56`×10, `0xFE`×1. That is the WOW32 side of WOW, which real Windows implements in
   `wow32.dll`. Our loop finds no patch-map entry, so `vec=0` and it reports "unexpected
   PM stop" and stops the guest (host stays alive).
   ⇒ WOW is not "NE loader + DPMI". It is that **plus a 32-bit BOP service API**.
   ⚠️ Before consulting any other NTVDM project, read
   [`reference-projects.md`](reference-projects.md).
   for this). Still unknown: **`INT 31h 04F3`**.

2. **[#131] Console/stdio integration.** Independent of WOW and needed regardless:
   anything script-driven behaves differently under NTVDMEX than under stock.
3. **[#130] Installation & routing.** Blocked on #128 — an installer is not useful while
   installing breaks every 16-bit Windows program.
4. **Known DOS defects** — #133 redirection, #134 the `$p` prompt, #47 MEM.EXE lying.

---

## Getting started on a machine that has never seen this

```bash
# Build (macOS/Linux cross-compile to XP-32; needs mingw-w64 i686)
./scripts/build.sh                 # -> build/ntvdmhost.exe

# Fast test loop -- no VM and no rig needed. Builds the batteries, then runs them.
./tools/dostest/run.sh                 # 18 batteries, 839 checks, ~10s, non-zero on failure
```

- The build is **no-CRT on purpose**: the toolchain is UCRT-default and UCRT is absent on
  XP, so a CRT-linked binary will not load there. `src/runtime.c` supplies the entry point
  and `mem*` primitives. Verify with `./scripts/check-imports.sh`.
- **`build/ntvdmhost.exe` is the host.** `build/ntvdmex.exe` is a small separate launcher.
  Deploying the wrong one has cost more than one session — checksum what you deploy.

For the bare-metal rig, the oracles, and how to run anything against real hardware, see
the wiki's testing pages. **Do not skip them**: the single most reliable way to waste a
day on this project is to measure stock `ntvdm` by accident and believe the result.

---

## How to read the rest of the docs

| Path | What it is |
|---|---|
| [`docs/log/sessions/`](log/sessions/) | The day-by-day archive, verbatim, refutations included. |
| [`docs/decisions/`](decisions/) | Architecture decision records. |
| [`docs/research/`](research/) | Raw findings — disassembly, kernel RE, oracle disagreements, measurement runs. |
| [`docs/research/evidence/`](research/evidence/) | Screen captures that back specific claims. |
| [`docs/ROADMAP.md`](ROADMAP.md) | Milestones. |
| [`docs/GLOSSARY.md`](GLOSSARY.md) | VDM, VDD, DPMI, WOW, thunk, BOP, IFEO… |
| [`docs/risks.md`](risks.md) | Standing risks. |
| [`docs/reference-projects.md`](reference-projects.md) | **Read before consulting any other NTVDM project.** What we may and may not read, and why. |

---

## The one thing to internalise

This project has repeatedly been wrong in the same way, and it is always the same shape:
**an instrument that lies**. A counter whose layout implies a claim it cannot support. A
video metric that moved the wrong way when a bug was fixed. A test that silently ran
against stock `ntvdm`. A dialog checker that reported 47 problems of which 45 were its
own. A "before" and "after" run that analysed the same stale file.

The habits that actually work, learned the expensive way:

- **Build ground truth from something that is not us** — the game's own WAD, a real MS-DOS
  under QEMU, stock `ntvdm`, Nuked-OPL. Oracles vote on truth; NTVDMEX does not vote.
- **Read the guest binary.** Disassembling `DOOM.EXE` fixed in an hour what twenty runs of
  host instruments could not.
- **When a guest dies at an address, diff the bytes there against the file on disk.** That
  found a five-session bug that was ours all along.
- **A trace that prints the request but not the answer is half an instrument.**
- **Predict the number before the run.**
- A fix measured on one guest is a fix for none — re-run the other class (V86 vs DPMI).
