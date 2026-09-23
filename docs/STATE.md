# Project state — start here

> **This is the canonical resume point.** Read it top to bottom and you will know where the
> project is, what works, what does not, and what to do next. It is deliberately short.
> **History does not live here** — it lives in [`log/sessions/`](log/sessions/), one file per
> session. When this file starts growing session blocks again, split them out; it has
> happened twice now (`return-ntvdm.md` in August, this file in September).

- **Updated:** 2026-09-23 (session 76)
- **Branch:** `m9/completeness`
- **Checkpoint commit:** **`59fac7d`** — the rollback point. `git diff a5dd042..HEAD -- src/`
  is empty, so every `src/` byte matches the tree that built `2565bffe`, the last host the
  user confirmed by hand. *No git tags yet: the first will be `0.0.1` at the first beta.*
- **Stable package:** `dist\ntvdmex-20260917-4847355.zip`, host `9448cf27`
  (tag `release-20260917b`). **Immutable until a new build is confirmed by hand.**
  `bin\` on the rig is free to churn.
- **Tracker:** [issues](https://github.com/MrMatthewLayton/ntvdmex/issues) ·
  **Knowledge base:** [wiki](https://github.com/MrMatthewLayton/ntvdmex/wiki) ·
  **History:** [`log/sessions/`](log/sessions/)

---

## What this is

**NTVDMEX is a replacement for `ntvdm.exe` on 32-bit Windows XP SP3.** It runs DOS programs
by executing 16-bit code on the **real CPU** in Virtual-8086 mode — reusing the NT kernel's
own VDM machinery through the undocumented `NtVdmControl` syscall — not by emulating a CPU.
It is not DOSBox and it is deliberately not a fork of anything.

**How it installs:** an Image File Execution Options `Debugger` value on `ntvdm.exe`, so every
MS-DOS *and* Win16 launch routes to NTVDMEX and stock `ntvdm` lies dormant. Not by overwriting
`System32\ntvdm.exe` — that is Windows File Protection territory — and reversible with one
`reg delete`. See [ADR-0007](decisions/0007-intercept-via-ifeo-debugger.md).

**The two bars it was built against, both met:**

- **Real DOS games, with sound** — Doom is fully playable, mouse and audio included.
- **MS Paint and Notepad from Windows 3.x** — both run and both do their job, user-confirmed.
  Paint draws in colour, keeps what it draws, and `File > Save As` writes a valid 24-bit BMP.

---

## The programme now: build from the specs, then test the apps

> **User directive, 2026-09-22:** *"The approach from the beginning should have been build the
> entire emulation layer (hardware, firmware, software) from specs … gaps implemented by what
> we're building, not what's asking for them."*

Every guest closure in this project's log was one application dying in one place, and a session
spent finding it. Each fix was correct; the *method* was not, because the application chose what
got built. The inventory drives the work now and applications are the acceptance test.

**Scope rule (2026-09-23):** a device is in the inventory because it is part of the
**period-correct hardware contract**, never because a guest asked for it. Counting how many
shelf guests touch a surface is app-driven reasoning wearing a spec-first hat.

**Two documents per surface:**

| Document | Says |
|---|---|
| [`ref/<surface>.md`](ref/) | What the hardware does — our own words, cited per section, wiki-ready |
| [`inventory/<surface>.md`](inventory/) | What **we** do — every unit marked IMPL / PART / STORE / MISS / N/A, with a `file:line` |

The full surface list, with the primary source named for each, is in
[`inventory/README.md`](inventory/README.md). **Order: VGA to completion first**, then outward.

---

## Where it actually is

### ✅ Working, confirmed by hand on real hardware

| | Status |
|---|---|
| **Doom** | Fully playable at **high detail** — 3D rendering, status bar, menus, PCM + MIDI, keyboard and mouse. Runs its own 32-bit code through DOS/4GW on real silicon. ⛔ **Low detail is broken — see below.** |
| **Duke Nukem 3D** | Runs, including its Setup program. |
| **Heretic / Hexen** | Both run; Hexen's hi-res loader renders. |
| **Wolfenstein 3D · Skyroads** | Fully playable. |
| **ZAR** | Renders, including its VESA modes, and its mouse buttons arrive. **Silent** — pinned in state 4. |
| **heaven7** | Renders (VBE 2.0 direct colour + linear framebuffer). Music is GUS-only and there is no GUS model yet. |
| **MS-DOS 6.22 `COMMAND.COM`** | Runs as a guest: prompt, line editing, internals, and an external program EXEC'd and returned from. |
| **QBasic / EDIT** | Run, including the Open dialog and building `.EXE`s. |
| **DOS API** | 103 INT 21h functions. XMS 3.0, EMS (LIM 4.0), DPMI 0.9, INT 13h, TSRs, redirection. |
| **Video** | Text (authentic IBM ROM font), mode 13h, mode 12h planar, mode Y, VESA banked + LFB, VBE 2.0/3.0 runtime. Windowed GDI + exclusive-fullscreen DirectDraw, Luna-themed. |
| **Sound** | SB16 PCM at 99.999% delivery, clean-room OPL2/OPL3 FM (MIT; Nuked used only as a black-box oracle), MPU-401 MIDI, PC speaker. |
| **Win16** | Notepad edits and saves text; MS Paint draws in colour and saves bitmaps. Real HWNDs, a turning message loop, the host calls 16-bit code. |
| **Host UI** | Menu bar, status strip, six-tab Settings dialog backed by `HKCU\Software\NTVDMEX`. |
| **Packaging** | A portable zip with `install.bat` / `uninstall.bat` / `status.bat` / `smoke.bat` / `diag.bat`, installed from scratch on three machines. |

### ⛔ Open defects

| | What is known |
|---|---|
| **Doom's low detail renders wrong** | User: *"High detail works. Low detail is broken!"* **Measured:** low detail is dominated by **two-plane map masks** (`0x03` and `0x0c`, ~143k writes each, vs ~16.9k per single-plane mask) and our mode-Y path maps the A0000 window to **one plane at a time**. The status bar loses ~70% of its detail although Doom draws it full-res either way. This is the concrete instance of the mode-Y approximation the VGA inventory predicted. **The fix belongs in VGA steps 3–4, not a special case.** [`inventory/vga.md`](inventory/vga.md) |
| **Mario's mode-Y artefacts** | A strong candidate for the same root cause as the row above. |
| **Duke3D took no keyboard on one machine** | Reproduced on a friend's Win98-era box only; works on both of the user's own boxes. |
| **DOS/4GW guests ran typewriter-slow on one machine, once** | First session on the friend's box; fine after a reboot that *also* changed the BIOS to optimized defaults. **Cause unknown and confounded**; logs unrecoverable. |
| **ZAR is silent** | Renders and plays, no audio. Eight hypotheses refuted. [`zar-dos16m`](log/sessions/) · user framing: *"if ZAR doesn't work then NTVDMEX doesn't work."* |
| **Win16 shelf** | X does not close WinMine/Charmap · Calc/Charmap/Clock draw incorrectly · Bubbles palette · Matrix_1 slow · `graphics\VS87.EXE` · MPLAYER GPF `0001:3983`. |
| **Console/stdio integration** | DOS output is buffered and flushed to `CONOUT$` at exit, so shell redirection and piping are bypassed and every DOS program pops a window. |
| **`MEM /C`** | The main report matches the 6.22 oracle row for row; `MEM /C` still says MSDOS is 1,028K, contradicting its own summary. |
| **Parity, open rows** | `p_tsr` paras-still-held · `xms.08` BH (undefined by spec). |
| **`/uninstall` can lock the user out** | It refuses when the IFEO value names a third binary, which the displaced-value restore can produce. Needs a `/force` or a message naming the path. |

### ⏸ Parked

- **Windows 2000** — the host loads (four XP-only imports bound at run time, `ffebdab`), but the
  `NtVdmControl` / WOW contract is unmeasured there. Parked at the user's request; the user's
  second box triple-boots 98/2000/XP and they test by hand.
- **Windows 7** — on the list, 32-bit only. No machine.

---

## Next actions, in order

1. **VGA step 3 — derive geometry from MiscOut + CRTC + SR.** Measurement first: extend
   `tools/dostest/p_vgareg.asm` to the address-generator registers for the modes that have no
   reference rows yet. ⛔ **PCem runs only from the user's own terminal** — it needs the
   WindowServer.
2. **Re-test `db4c059`.** It is reverted on an **unproven** regression: the by-hand report that
   condemned it (*"columns too wide, flickery, status bar"*) is two-thirds the low-detail
   signature, and that install was at `detaillevel 1`. Un-revert on a branch and re-A/B with
   `detaillevel 0` **pinned** and the odd-column measurement. "Flickery" is the one symptom
   detail level does not explain, so this is a re-test, not an assumption the other way.
3. **VGA step 4 — one address generator** from CR17/CR14/GR5/GR6/SR4, replacing `chain4` + the
   mode-Y snapshot + `mkind`. This is where Doom's low detail is fixed.
4. **Write `ref/vga.md`** — the derived hardware reference, the template for the other surfaces.
5. **Then outward**, per [`inventory/README.md`](inventory/README.md).

⚠ **One observable change at a time, with a by-hand check between.** Read-back fidelity is a
guest-visible behaviour change. **The regression set must include Doom at low detail** — every
Doom check this project has ever run was high detail.

---

## How anything here is verified

| Instrument | What it is good for | Command |
|---|---|---|
| **The off-VM battery** | ~30 native unit tests over the DOS kernel and the device models. No VM, no rig. **Run it after touching any shared header.** | `./scripts/offvm.sh` |
| **The parity sweep** | 36 DOS probes asked of NTVDMEX and of genuine MS-DOS 6.22, diffed | `./scripts/paritysweep.sh` |
| **The bare-metal rig** | A real XP box. The only thing that can see a wrong picture. | [`wiki/The-bare-metal-rig`](wiki/The-bare-metal-rig.md) |
| **PCem + a real Tseng ET4000/W32p ROM** | The VGA/VESA/BIOS oracle | ⛔ **user's terminal only** |
| **Stock `ntvdm` on the rig** | The oracle for everything with no public spec — the WOW32 thunk ABI above all | [`research/stock-vdm-dump-oracle`](research/) |
| **The score** | A model, not a measurement | `./tools/score/score.py` |

⛔ **Re-run these, never quote them.** Any number written down here is stale by definition.

---

## Standing hazards — each of these has cost a session

1. ⛔ **Check the guest's own config before blaming code.** `findstr /I "detaillevel screenblocks" default.cfg`. Doom's detail level has burned three sessions. A game config survives binary swaps, reverts, reboots **and a full wipe**.
2. ⛔ **Find every install before believing a run.** `dir /s /b C:\ntvdmhost.exe`. The rig has had two complete NTVDMEX installations with different game configs; one party launched one and the other described the other, and both were right.
3. ⛔ **A first run after a deploy is not evidence.**
4. ⛔ **A by-hand verdict is only as good as its baseline**, and an A/B between runs you did not control is not an A/B.
5. ⛔ **A guard that returns success is a lie the whole stack repeats.** Verify *state*, not exit codes.
6. ⛔ **An absence in a report means nothing** — a guard's own log line must never share a budget with what it guards against.
7. ⛔ **A regression need not be code.** A folder rename broke Hexen; a flag file containing a log line killed every PM timer.
8. ⛔ **NTVDMEX can jam the whole machine.** The rig is one folder, nothing on `C:`. A dead rig means checking the IFEO key points at an existing binary.
9. ⛔ **Launch a Win16 guest after any shared-path change** — it was dead for five sessions and nobody noticed.
10. ⛔ **Copy the user's log to `runs/` before running anything** — the host truncates `out\ntvdmhost.log`.

The full list, with the story behind each, is the wiki's
[Traps and lessons](https://github.com/MrMatthewLayton/ntvdmex/wiki/Traps-and-lessons).

---

## Getting started on a machine that has never seen this

```bash
# Build (macOS/Linux cross-compile to XP-32; needs mingw-w64 i686)
./scripts/build.sh

# Fast test loop -- no VM and no rig needed.
./scripts/offvm.sh
```

Then read [`README.md`](README.md) for the map of the rest of the documentation, and the
wiki's [Building and running](https://github.com/MrMatthewLayton/ntvdmex/wiki/Building-and-running).

---

## The one thing to internalise

**A thing that looks done has been wrong more often than a thing that looks broken.** A
stepped-over call still answers — with stack litter. A half-modelled register returns a
*plausible* wrong number and the guest fails somewhere else entirely. An unimplemented call
cannot return a default. That is why the inventory marks **PART** separately from **MISS**, why
probes poison every output register before asking, and why "user-confirmed by hand" is the only
status that closes a guest.
