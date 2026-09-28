# Project state — start here

> **This is the canonical resume point** for what the project IS and where it stands.
> **All outstanding work lives in GitHub issues — nothing to-do is kept in this repo.**
> Start with the pinned **[#202 Work order](https://github.com/MrMatthewLayton/ntvdmex/issues/202)**,
> then [open issues by priority](https://github.com/MrMatthewLayton/ntvdmex/issues?q=is%3Aopen+sort%3Acreated-asc)
> (`P0` > `P1` > `P2`). **History** lives in [`log/sessions/`](log/sessions/), one file per session.

- **Updated:** 2026-09-28 (session 83)
- **Branch:** `m9/completeness` — everything pushed. Rig `bin\` = `526b1090` (#175 one-shot IRQ0 +
  the counter-2 wait fix, on top of the #213 ZAR sound fix). The user confirmed most of round 5 (`runs/s83/sweep.txt`); #213 is owed by ear.
- **Resume:** [`log/sessions/session-83.md`](log/sessions/session-83.md), then #202
  (#212 IF at program start, then #162, #183).
- **Stable package (anchor):** `dist\ntvdmex-20260927-a286862.zip`, host **`0e6f5156`** —
  the s81 shelf sweep plus the user's re-check of every sweep fix (DIR, EXIT, the prompt,
  Settings' MS-DOS version group). The sweep's remaining findings are issues, deferred to the
  next zip by the user. Previous anchor: `ntvdmex-20260917-4847355.zip` (host `9448cf27`,
  tag `release-20260917b`). ⚠ No git tag has been made for the new anchor yet. `bin\` on the
  rig is free to churn; a `dist\` zip is immutable.
- **Rollback host:** `debug\prev\ntvdmhost_prev.exe` md5s as **`0e5b10e7`** (user-approved
  s81 **Part 5** — everything after Part 5 was unconfirmed; this line said `0e6f5156` until the end of s81 — the anchor zip's host). Earlier confirmed builds sit beside it by hash (`cd5f9f12` ZAR sound,
  `f484fc3d` IF/VIF gate, `a0294462` GUS, `8d795b96`, `0473d95d`, `b6a8a95b`, `d57d586c`).
  **md5 the slot; do not trust this line.**
- **Checkpoint commit:** still `ff0d956` — ⚠ several confirmed builds behind; moving it is a
  deliberate act (the anchor above is the better rollback reference now).
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
| **Doom** | Fully playable at **high and low detail** (low fixed s80: its drawers run through the VGA address generator, `pm32interp.h`) — 3D rendering, status bar, menus, PCM + MIDI, keyboard and mouse. Runs its own 32-bit code through DOS/4GW on real silicon. Launches from the shell and from SETUP, quits back to the prompt (s80). |
| **Duke Nukem 3D** | Runs, including its Setup program, VESA 800x600, with sound. |
| **Heretic / Hexen** | Both run; Hexen's hi-res loader renders. |
| **Wolfenstein 3D · Mario · Skyroads** | Fully playable. Wolf3D's status bar and Mario's mode-Y artefacts fixed s80 (⚠ Wolf3D now ~70–82% of a host core, interpreting). |
| **ZAR** | Renders (VESA 800x600), mouse, and **sound effects** (s81: the Miles driver's IRQs now reach real-mode calls). |
| **heaven7** | Renders (VBE 2.0 direct colour + linear framebuffer) **and plays its GUS music** — *"works and is accurate"* (s80). |
| **XP's own `COMMAND.COM`** | The default DOS prompt when NTVDMEX is opened: full-path prompt, `dir` (8.3 names), EXEC and return, `exit` closes the window (s81, user-confirmed). |
| **MS-DOS 6.22 `COMMAND.COM`** | Runs as a guest: prompt, line editing, internals, and an external program EXEC'd and returned from. |
| **QBasic / EDIT** | Run, including the Open dialog and building `.EXE`s. |
| **DOS API** | 103 INT 21h functions. XMS 3.0, EMS (LIM 4.0), DPMI 0.9, INT 13h, TSRs, redirection. |
| **Video** | Text (authentic IBM ROM font), mode 13h, mode 12h planar, mode Y, VESA banked + LFB, VBE 2.0/3.0 runtime. Windowed GDI + exclusive-fullscreen DirectDraw, Luna-themed. |
| **Sound** | SB16 PCM at 99.999% delivery, clean-room OPL2/OPL3 FM (MIT; Nuked used only as a black-box oracle), MPU-401 MIDI, PC speaker, **Gravis UltraSound** (s80: 240h / IRQ 11 / DMA 3, `ULTRASND=` in the env). ⚠ The mixer is mono. |
| **Win16** | Notepad edits and saves text; MS Paint draws in colour and saves bitmaps. Real HWNDs, a turning message loop, the host calls 16-bit code. |
| **Host UI** | Menu bar, status strip, six-tab Settings dialog backed by `HKCU\Software\NTVDMEX`. |
| **Packaging** | A portable zip with `install.bat` / `uninstall.bat` / `status.bat` / `smoke.bat` / `diag.bat`, installed from scratch on three machines. |


### ⛔ Known defects and all remaining work → GitHub

Nothing is tracked here any more. The order of work is **[#202](https://github.com/MrMatthewLayton/ntvdmex/issues/202)**;
the host-UI programme is **[#201](https://github.com/MrMatthewLayton/ntvdmex/issues/201)**; every
known defect, parity gap and inventory gap is an open issue with a `P0`/`P1`/`P2` label. The
session-81 review that moved them there is recorded in
[`log/sessions/session-81.md`](log/sessions/session-81.md) part 4.

The previous contents of this section (open-defect table, parked list, the s80 handoff, and
the long "Next actions" log) are preserved verbatim, as history, in
[`log/state-archive-2026-09-27.md`](log/state-archive-2026-09-27.md).

---

## How anything here is verified

| Instrument | What it is good for | Command |
|---|---|---|
| **The off-VM battery** | ~30 native unit tests over the DOS kernel and the device models. No VM, no rig. **Run it after touching any shared header.** | `./scripts/offvm.sh` |
| **The parity sweep** | Every DOS probe asked of NTVDMEX and of genuine MS-DOS 6.22 (and PCem where a probe names it), diffed. **46 probes, 748 rows, 0 unusable.** ⛔ **Its score excluded TEN device probes for three sessions** — a trailing comment on a probe's `ORACLE-ALSO:` line built a nonsense command line, the probe reported `NO ROWS`, and an unusable probe leaves *both halves* of the fraction, so the number could only **rise** when something broke (`e618011`, `e89478e`) | `./scripts/paritysweep.sh` |
| **The bare-metal rig** | A real XP box. The only thing that can see a wrong picture. | [`wiki/The-bare-metal-rig`](wiki/The-bare-metal-rig.md) |
| **PCem + a genuine IBM VGA / Tseng ET4000 ROM** | The VGA/VESA/BIOS oracle — the only period-correct firmware we have | ✅ **runs unattended**, ~60 s: `scripts/pcemoracle.py run <x.com>` ⚠ **outside the command sandbox** |
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
11. ⛔⛔ **A KNOB WITH TWO SOURCES: DELETING THE FILE DOES NOT RESTORE THE DEFAULT.** The reported DOS version comes from `cfg\dosver.txt` *or*, persistently, from `HKCU\Software\NTVDMEX\DosVersionMajor/Minor` — written by the Settings dialog and surviving every reboot, wipe and rebuild. The rig was found on 2026-09-24 reporting **5.00 to every DOS guest** from the registry, with no `dosver.txt` anywhere, on a project whose whole parity method diffs against a **6.22** oracle (`p_ver.com` → `int21.30 AX=0005`). Nothing reported it: the host logged a line only when the *file* overrode. Now unconditional and it names the source — `STAGE2: DOS version reported = 05.00 (source: …)`. ⚠ **Before quoting any DOS-side comparison, read that line**, and ask the same question of every other setting the dialog persists.

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
