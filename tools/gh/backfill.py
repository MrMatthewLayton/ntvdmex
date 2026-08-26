#!/usr/bin/env python3
"""backfill.py -- reconcile the GitHub issue tracker with what the repo ACTUALLY contains.

WHY THIS EXISTS. For 29 sessions and 459 commits the tracker recorded the ORIGINAL plan
(M4-M8, the Win16 epics) and almost none of the work that really happened: the whole DPMI
programme (58 spike commits), sound, the playable-games push, the DOS shell, the host UI,
the test rig and its oracles. Meanwhile a dozen open issues had quietly been finished --
32-bit DPMI is open while Doom is fully playable ON a DOS/4GW extender.

A tracker that disagrees with the repo is the same failure this project keeps paying for
in its instruments: it looks authoritative and it is wrong. So this file is the manifest,
it lives in the repo, and it is re-runnable: it matches on TITLE, creates what is missing,
and leaves what already exists alone. Running it twice is a no-op.

    tools/gh/backfill.py --dry-run     # print the plan, touch nothing
    tools/gh/backfill.py               # apply

Every CLOSED issue here is closed because the work is DONE AND EVIDENCED -- the evidence
is named in the body (a commit, a file, a measurement). Nothing is closed on a hunch; a
few things that LOOK done are deliberately left open with a note saying which part is
missing (see #28, #34, #49, #56).
"""
import argparse
import json
import subprocess
import sys

REPO = "MrMatthewLayton/ntvdmex"


def gh(*args, check=True):
    r = subprocess.run(["gh", *args], capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError(f"gh {' '.join(args)}\n{r.stderr}")
    return r.stdout.strip()


def api(path, method="GET", **fields):
    args = ["api", "-X", method, f"repos/{REPO}/{path}"]
    for k, v in fields.items():
        args += ["-f", f"{k}={v}"]
    return gh(*args)


# ── milestones ────────────────────────────────────────────────────────────────
# The first three are retrospective: M1-M3 happened, were never milestones, and their
# work is what everything since stands on. M10 is the goal the project is actually for.
MILESTONES = [
    ("M1 - Host bootstrap & VDM interception", "closed",
     "Become the process Windows launches for a DOS program, and run 16-bit code on the real CPU."),
    ("M2 - DOS kernel core", "closed",
     "A DOS that loads and runs a real .COM/.EXE: MCB chain, PSP, loader, environment, INT 21h."),
    ("M3 - Device model & video", "closed",
     "VGA/VESA video, keyboard, the presentation window, and the VDD bus they hang off."),
    ("M4 - Memory extensions", "closed", "XMS 3.0 and EMS (LIM 4.0)."),
    ("DPMI - protected mode", "closed",
     "A DPMI 0.9 host running unmodified third-party clients, then real-CPU PM, then 32-bit DOS/4GW."),
    ("Playable Games (Doom / Skyroads / ZAR)", "open",
     "The project's stated bar. Doom and Skyroads are met; ZAR needs VBE 2.0."),
    ("M10 - Installation & routing", "open",
     "Install NTVDMEX so that every DOS and Win16 launch routes to it and stock ntvdm lies dormant."),
]

E = "epic"

# ── the manifest ──────────────────────────────────────────────────────────────
# (title, state, labels, milestone, epic-parent-title-or-None, body)
ISSUES = []


def add(title, state, labels, milestone, parent, body):
    ISSUES.append(dict(title=title, state=state, labels=labels,
                       milestone=milestone, parent=parent, body=body.strip()))


# ══ EPIC: M1 host bootstrap ═══════════════════════════════════════════════════
M1 = "EPIC: Host bootstrap — become the VDM Windows launches, and run 16-bit code"
add(M1, "closed", [E], "M1 - Host bootstrap & VDM interception", None, """
**DONE.** NTVDMEX is launched by Windows in place of `ntvdm.exe`, obtains the program to
run from the session, and executes real 16-bit code in V86 mode on the real CPU via
`NtVdmControl` — no CPU emulation.

**How interception actually works, after one disproven approach:** repointing
`HKLM\\SYSTEM\\CurrentControlSet\\Control\\WOW\\cmdline` was the plan and **did not work**.
The mechanism that does is an **Image File Execution Options `Debugger` value on
`ntvdm.exe`**. See `docs/research/` and the M10 epic — turning this into a real,
reversible install is the remaining piece.

⚠️ The IFEO key is also the project's most reliable foot-gun: left absent, every test
silently measures *stock* ntvdm and still "passes".
""")

add("V86 execution via NtVdmControl (no emulation)", "closed", [], "M1 - Host bootstrap & VDM interception", M1, """
**DONE.** 16-bit code runs in Virtual-8086 mode on the real CPU by reusing the NT kernel's
own VDM machinery through the undocumented `NtVdmControl` syscall, rather than shipping a
custom V86 driver. The VDM_TIB is self-allocated.

Consequence that shaped everything after: real NTVDM already used V86 on x86 — emulation
was only ever for non-x86 NT and 64-bit Windows. ReactOS was the primary reference for the
undocumented structures.
""")

add("Recover the program to run: GetNextVDMCommand + CurDir/Title", "closed", [], "M1 - Host bootstrap & VDM interception", M1, """
**DONE.** The program, its arguments, environment and current directory come from
`GetNextVDMCommand`. `target.txt` overrides it so DOS-kernel tests do not depend on the
recovery path.

Deferred: recovering the real shell's argv and notifying the shell of the exit code
(#16).
""")

add("IFEO Debugger interception on ntvdm.exe (WOW cmdline repoint disproven)", "closed", [], "M1 - Host bootstrap & VDM interception", M1, """
**DONE, and it corrected the founding assumption.** `Control\\WOW\\cmdline` /
`wowcmdline` repointing was the documented-looking route and does not work. The working
mechanism is an IFEO `Debugger` value on `ntvdm.exe`.

⚠️ This intercepts **Win16 launches too** — they also go through `ntvdm.exe`. Until the
WOW epic lands, that is a live hazard: see "Passthrough guard" in the M10 epic.
""")

# ══ EPIC: M2 DOS kernel ═══════════════════════════════════════════════════════
M2 = "EPIC: DOS kernel core — load and run a real DOS program"
add(M2, "closed", [E], "M2 - DOS kernel core", None, """
**DONE.** A DOS implementation that builds a real process in conventional memory and
services the INT 21h API: **103 distinct AH functions** implemented (`src/dos/dos_int21.c`).

Proven by running MS-DOS 6.22's own `COMMAND.COM` as a guest — internals, `DIR`, and an
external program EXEC'd and returned from.
""")

for t, b in [
    ("MCB chain, memory allocation and the DOS memory map",
     "**DONE.** `src/dos/dos_mcb.h` + `mcb_test` (49/49). Allocate/free/resize, the walkable chain, and Map 3 of the memory layout."),
    ("PSP construction and process structure",
     "**DONE.** `src/dos/dos_psp.h` — PSP, command tail, FCBs, the parent/child fields EXEC needs."),
    (".COM and .EXE loading (relocation, segment fixups)",
     "**DONE.** `src/dos/dos_loader.h`. Both image types, including relocation of MZ executables."),
    ("Environment block construction",
     "**DONE.** `src/dos/dos_env.h` — the environment segment plus the argv[0] path DOS appends after it."),
    ("INT 21h: character I/O and the console model",
     "**DONE.** 01h-0Ch. ⚠️ Every blocking input call must POLL via `retry`, never park the exec thread: AH=0Ah blocked it and deadlocked COMMAND.COM outright, because the BIOS key ring is filled by the guest's own INT 09h ISR, which cannot run while we are inside its INT 21h call."),
    ("INT 21h: file handles — open/create/read/write/seek/close",
     "**DONE.** 3Ch-42h, mapped onto Win32 file handles. Handles 0-4 are ordinary table slots and DOS hands out the lowest free one."),
    ("INT 21h: FCB services",
     "**DONE.** 0Fh-24h, 27h-29h. Traps found the hard way: a DOS filename ends at a TERMINATOR not only a NUL (0x0D leaked into the FCB and broke every internal command unless you typed a trailing space), and an EXTENDED search must return an EXTENDED result (7-byte prefix) or DIR reads every field seven bytes early."),
    ("INT 21h: directory search (4Eh/4Fh) and DTA semantics",
     "**DONE** — tracked as #29. Volume-label searches (attribute 08h) are not file searches and are answered from GetVolumeInformation; `.` and `..` are NAMES, not empty extensions."),
    ("INT 21h: EXEC (4Bh) and child termination",
     "**DONE** — tracked as #30. A child process loads, runs and returns. ⚠️ 4Bh AL=01/03 (load-without-execute, overlay) are NOT done — #50."),
    ("XMS 3.0 driver",
     "**DONE.** `src/dos/dos_xms.h`, off-VM 36/36, VM-confirmed."),
    ("EMS (LIM 4.0) with a dynamically relocatable page frame",
     "**DONE.** `src/dos/dos_ems.h`, off-VM 30/30, VM-confirmed."),
]:
    add(t, "closed", [], "M2 - DOS kernel core" if "XMS" not in t and "EMS" not in t else "M4 - Memory extensions", M2, b)

# ══ EPIC: M3 device model ═════════════════════════════════════════════════════
M3 = "EPIC: Device model — video, input, and the VDD bus"
add(M3, "closed", [E], "M3 - Device model & video", None, """
**DONE and VM-confirmed.** Text and graphics video, keyboard, mouse, timer, and the
VDD bus they hang off. All ten QuickBASIC demos in `demos/` render with **zero pixel
defects** — evidence in `docs/research/evidence/mode12-demos/`.
""")

for t, b in [
    ("VGA text mode with the authentic IBM ROM font",
     "**DONE.** 8x8/8x14/8x16 fonts (`src/vdd/vga_font_*.h`). Cursor shape comes from the guest (INT 10h AH=01 CX) and blink is driven from the injected clock, so the VDD stays off-VM testable."),
    ("Mode 13h (320x200x256) and the palette/DAC",
     "**DONE.** Includes mode-Y and page-flipping paths that Doom uses."),
    ("Mode 12h planar EGA/VGA via an A0000 page trap",
     "**DONE.** ⚠️ In planar modes the interpreter IS the CPU: arming the A0000 NOACCESS trap freezes the guest on real hardware (#55), so planar stores are decoded and executed by us."),
    ("VESA/VBE modes",
     "**DONE** for the banked modes the demos and games use. VBE 2.0 hi-colour + linear framebuffer remains open (#23, needed for ZAR)."),
    ("Presentation: windowed GDI and exclusive-fullscreen DirectDraw",
     "**DONE.** `src/vdd/present_ddraw.c`. Luna-themed frame via the Common-Controls 6.0 manifest."),
    ("Keyboard: INT 09h, the BIOS ring at 0040:001E, and INT 16h",
     "**DONE.** ⚠️ The cause of Skyroads being unplayable was NOT delivery: the INT 09h stub DISCARDED scancodes instead of filling the BIOS ring, and INT 21h never did DOS's extended-key two-call protocol."),
    ("Mouse: INT 33h driver with raw relative motion",
     "**DONE.** ⚠️ Function 0Bh must report MICKEYS FROM THE DEVICE (WM_INPUT), not deltas derived from the clamped Windows pointer — otherwise a turn stops at the window edge and you cannot spin."),
    ("PIT timer, PIC, and the shared timebase",
     "**DONE.** `src/vdd/vdd_pit.c`, `vdd_pic.c`. The host paces the PIT from a dedicated thread so ticks come out evenly rather than in bursts."),
    ("VDD bus + off-VM test batteries",
     "**DONE.** `src/vdd/vdd_bus.c` and 17 native batteries under `tools/dostest/` that run on the build machine in seconds. This is the fast loop; the rig is the slow one."),
]:
    add(t, "closed", [], "M3 - Device model & video", M3, b)

# ══ EPIC: DPMI ════════════════════════════════════════════════════════════════
DP = "EPIC: DPMI — protected mode, from a 16-bit spike to 32-bit DOS/4GW on real silicon"
add(DP, "closed", [E], "DPMI - protected mode", None, """
**DONE.** The single largest body of work in the project (58 `spike(dpmi)` commits before
it was even a feature). A DPMI 0.9 host that runs **unmodified third-party clients**, then
real-CPU protected mode, then 32-bit DOS/4GW — which is what makes Doom possible.

Route: descriptors and INT 31h → a host interpreter that ran two of Japheth's HX clients
end to end → the kernel `FIXED_NTVDMSTATE` gate (#18) → real-CPU PM → 32-bit (#19).
""")

for t, b in [
    ("INT 31h service surface + advertise INT 2Fh AX=1687h",
     "**DONE** — was #2. Descriptor management, memory, callbacks, 0300/0301/0303 real↔protected transitions, 0305/0306 state save/raw mode switch."),
    ("Real→protected mode switch and the initial client state",
     "**DONE.** ⚠️⚠️ **THE DOOM FIX:** the mode-switch initial CS/DS/SS must be **D/B=0 even for a 32-bit client** — the RETF-on-failure path proves the client's post-switch code is still 16-bit. Setting D/B=1 ran DOS/4GW's stub as 32-bit, i.e. wild writes through ESI and silent death. Our own pm32 tests HID this and regressed when it was fixed."),
    ("Run PM on the real CPU: satisfy the kernel's monitored-gate so PM faults reflect",
     "**DONE** — was #18, the longest-standing research issue. Real-CPU PM I/O, graphics, input and timing through the VDDs with no reflect and no interpreter."),
    ("32-bit DPMI / DOS-4GW protected-mode execution",
     "**DONE** — was #19. Doom's own 32-bit code runs on real silicon. Four distinct bugs, all ours: the LE object table names which blocks are code; EIP is only 16-bit when its selector is; the PM-return catcher's D/B bit is how the extender sizes pointer arguments; and the RMCS copy-back dropped FLAGS, so every DOS call reported success."),
    ("Asynchronous PM interrupt delivery (SuspendThread/SetThreadContext)",
     "**DONE.** A spinning PM guest never leaves PM, so interrupts are injected by suspending the thread and rewriting its context. The host owns IRQ vectors 08h-0Fh."),
    ("PM host interpreter (v86interp.h) as the fallback path",
     "**DONE.** `src/host/v86interp.h`. Built when the kernel walled us out; kept as the fallback. It runs Japheth's `i310102` C-runtime client and `DPMIBACK` to completion."),
    ("x86 instruction-length decoder for safe INT-site patching",
     "**DONE.** `src/host/x86len.h`. ⚠️⚠️ **THIS WAS THE FIVE-SESSION DOOM KILLER AND IT WAS OURS:** `dpmi_patch_code_region` matched `CD nn` as a byte pair and rewrote a `jle`'s DISPLACEMENT. The fix is a real length decoder plus a boundary vote. **METHOD: when a guest dies at an address, diff the bytes there against the file on disk.**"),
]:
    add(t, "closed", [], "DPMI - protected mode", DP, b)

# ══ EPIC: sound ═══════════════════════════════════════════════════════════════
SND = "EPIC: Sound — SB16 PCM, clean-room OPL2/OPL3, and MPU-401 MIDI"
add(SND, "closed", [E], "Playable Games (Doom / Skyroads / ZAR)", None, """
**DONE** — was #7. Doom plays with digital sound effects and music, user-confirmed by ear.
PCM delivery measured at **99.999%**.

The residual 0.001% and the hardware-grounding question are #57, which stays open.
""")

for t, b in [
    ("SB16 digital audio: DSP + DMA PCM playback",
     "**DONE** — was #20. `src/vdd/vdd_sb.c` + `vdd_dma.c`. ⚠️ The fix that got PCM to 99.999% was **pacing the PIT**: `host_pit_sync` ran only 65×/s so ticks came out in BURSTS and the game's refill arms collapsed. The cause was in the CLOCK, not the audio path."),
    ("OPL2/OPL3 FM synthesis, clean-room, with Nuked as a black-box oracle",
     "**DONE** — was #21. `src/vdd/vdd_opl_synth.c`, MIT clean-room. Waveform correlation 0.41→0.91 after finding modulation depth was halved, plus four more defects, all found with a purpose-built single-note rig (`tools/oplprobe`). ⚠️ Snare/hi-hat/cymbal need the chip's special phase generator and are deliberately NOT guessed — counted as not synthesised. See the follow-up issue."),
    ("MPU-401 / General MIDI output",
     "**DONE** — was #22. `src/vdd/vdd_mpu.c`. Doom's music plays."),
    ("PC speaker",
     "**DONE.** `src/vdd/vdd_speaker.c`."),
    ("Host audio output path (waveOut) with a lead-time buffer",
     "**DONE.** `src/vdd/audio_wave.c`. ⚠️ Loads winmm dynamically; the process raises timer resolution with `timeBeginPeriod(1)` elsewhere, which has knock-on effects on the frame timer — see the pacer notes in `main.c`."),
]:
    add(t, "closed", [], "Playable Games (Doom / Skyroads / ZAR)", SND, b)

# ══ EPIC: playable games ══════════════════════════════════════════════════════
PG = "EPIC: Playable games — the project's stated bar"
add(PG, "closed", [E], "Playable Games (Doom / Skyroads / ZAR)", None, """
**MET for Doom and Skyroads**, both user-confirmed by hand on real hardware.

- **Doom** — fully playable: 3D rendering, status bar, menus, PCM + MIDI, keyboard and mouse.
- **Skyroads** — fully playable: menus, Controls, level select, in-game.
- **ZAR** — still blocked on VBE 2.0 hi-colour + linear framebuffer (#23).

Remaining polish: #57 (sound 99.999%), #58 (melt/wipe).
""")

for t, b in [
    ("Doom: 32-bit rendering through DOS/4GW",
     "**DONE.** Runs its own 32-bit code, sets video modes, renders in full 3D."),
    ("Doom: the status bar (I_ReadScreen reads the WRONG PLANE)",
     "**DONE.** ⚠️ **Found by DISASSEMBLING DOOM.EXE after four runs of host instruments found nothing.** `I_ReadScreen` cycles GR4 and never writes the map mask, so every read was served from the WRITE plane. Every exclusion made beforehand was about WRITES; they were all correct and the CATEGORY was wrong."),
    ("Doom: mouse look (a 32-bit EDI masked to 16 bits)",
     "**DONE, user-confirmed.** ⚠️ Doom WAS calling AX=3/0Bh 2915 times in 45s all along — the filed claim that 'Doom never asks' was wrong, and so were both filed explanations. The real cause: a 32-bit EDI masked to 16 bits on the DPMI 0300 call structure, so we read the function number out of, and wrote answers into, junk memory. The tell was 2595 calls reading back the value our own reset had written."),
    ("Skyroads: playable end to end",
     "**DONE.** The input stack was the blocker, not delivery — see the keyboard task under M3. ⚠️ Beware the attract loop faking success."),
    ("Timing: pace the PIT so ticks are even",
     "**DONE.** ⚠️ **A fix measured on ONE guest is a fix for none:** a PIT throttle validated on Doom cost Skyroads 24% of its clock for two sessions and no counter flagged it — the user's ear did. Re-run a guest from the other class (V86 vs DPMI) after touching any shared path."),
]:
    add(t, "closed", [], "Playable Games (Doom / Skyroads / ZAR)", PG, b)

# ══ EPIC: DOS shell ═══════════════════════════════════════════════════════════
SH = "EPIC: Run a real DOS shell — MS-DOS 6.22 COMMAND.COM"
add(SH, "closed", [E], "M9 - DOS/BIOS completeness (TDD)", None, """
**DONE.** MS-DOS 6.22's own `COMMAND.COM` runs as a guest: prompt, line editing with
backspace, internals (VER/VOL/CLS/ECHO/SET/TYPE/COPY/DIR/EXIT), correct "File not found",
and an external program (`ATTRIB.EXE`) loaded, run and **returned from**.

A shell is the *other shape* of DOS guest — resident, line-at-a-time, walks directories,
EXECs children that must come back. No game exercises that, and it found **five defects in
an afternoon, four of them in code every guest uses**. See the child issues.

Still open from this work: redirection (`echo x > file`), the `$p` prompt degrading after
an EXEC, and XP's own COMMAND.COM.
""")

for t, b in [
    ("AH=0Ah buffered input blocked the exec thread (deadlocks any shell)",
     "**FIXED.** AH=01/07/08 and INT 16h were made to poll years ago; 0Ah was the last input call still parking the thread in C. What it waits for CANNOT arrive while it waits — the BIOS key ring is filled by the guest's own INT 09h ISR, which cannot run because we are inside its INT 21h call. Measured: 40 scancodes queued, IRQ1 attempted 691 times, EVERY one refused `not_in_exec`. Now collects the line ACROSS retries."),
    ("A DOS filename ends at a TERMINATOR, not only at a NUL",
     "**FIXED.** `fcb_put_name` copied the command line's 0x0D into the FCB, so AH=29h parsed `ver` as `VER\\r    `. COMMAND.COM matches its table entry then checks THE NEXT BYTE IS BLANK — so every internal command was 'Bad command or file name' **unless you typed a space after it**. Found by a differential trace of two runs differing by one typed space."),
    ("An extended FCB search must return an extended result",
     "**FIXED.** We skipped the 7-byte prefix going in and wrote the answer back in the SHORT layout, so DIR — which must use an extended FCB to see directories — read every field seven bytes early: blank names, one impossible size repeated, volume label 'COM'."),
    ("A failed search must record WHY (AH=59h after an exhausted search)",
     "**FIXED.** Extended errors are only recorded where CF is set, and FCB calls leave CF alone, so an exhausted search left `last_err` holding COMMAND.COM's startup probe for 0xFFFF paragraphs — error 8. DIR finished its listing, asked 59h why, and printed 'Insufficient memory' over its own summary."),
    ("Volume-label searches and the . / .. entries",
     "**FIXED.** Attribute 08h searches are not file searches and are answered from GetVolumeInformation; `.` and `..` are NAMES, not empty extensions."),
    ("Reported DOS version as a knob (registry + dialog)",
     "**DONE.** 6.22 is the reference; XP's own COMMAND.COM demands 5.00. ⚠️ Version-ALIGNED API BEHAVIOUR is a separate, still-open concern — see #28."),
]:
    add(t, "closed", [], "M9 - DOS/BIOS completeness (TDD)", SH, b)

# ══ EPIC: host UI ═════════════════════════════════════════════════════════════
UI = "EPIC: Host UI — menus, status strip, and settings"
add(UI, "closed", [E], "M8 - Polish & SDK", None, """
**DONE** for this pass, verified on real hardware with screenshots.

- Caption is the constant `Microsoft Windows XP Virtual DOS Machine`.
- Status strip: program name | 16/32-bit | Real/Protected mode, and the input-capture
  state with its release chord.
- Settings live in `HKCU\\Software\\NTVDMEX`, edited through a six-tab dialog.
- Menu bar reduced to File | Edit | View | Machine | Capture | Debug | Help.

⚠️ **40 of the 46 settings are STORED BUT NOT HONOURED** — see the open follow-up.
""")

for t, b in [
    ("Settings in the registry, with the text-file precedence contract",
     "**DONE.** Precedence is `built-in default < registry < text file on the share`, and `settings_load()` runs at the TOP of the knob block in WinMain. ⚠️ **Do not 'tidy' this ordering:** the rig configures the host by writing files and re-launching; if the registry overrode them, every headless measurement would silently report whatever was last clicked in a dialog on that machine."),
    ("Tabbed settings dialog (tab control + child dialog pages)",
     "**DONE.** Six pages, one table (`SET_DEFS`) driving defaults, load, save, clamp and both dialog directions. Traps: pages need `DS_CONTROL`; pages must be `HWND_TOP` or the tab paints over them; `EnableThemeDialogTexture` stops a page rendering as a grey slab; `TCM_ADJUSTRECT` computes the page rectangle because tab-row height belongs to the visual style."),
    ("Status strip reporting program, bit width and CPU mode",
     "**DONE and measured:** `COMMAND.COM | 16-bit | Real mode` vs `DOOM.EXE | 32-bit | Protected mode`. Polled from the UI tick and pushed only on change, because two of the three facts are set on the V86 thread inside the DPMI mode switch."),
    ("Input capture (exclusivity) with a reserved release chord",
     "**DONE.** Win+F10 toggles. ⚠️ **NEVER SWALLOW A KEY-UP** — capture is entered while the hook is not yet installed, so eating the key-UP leaves Windows believing Win is held forever and every later keystroke becomes Win+key. Deliberately NOT Ctrl+F10: Doom fires with Ctrl and uses every F-key."),
    ("Visible host cursor and a blinking text cursor",
     "**DONE.** The host cursor was hidden on an unmeasured input-lag theory and now defaults to visible. Text-cursor shape comes from the guest, so shape and hide had to arrive together."),
]:
    add(t, "closed", [], "M8 - Polish & SDK", UI, b)

# ══ EPIC: test infrastructure ═════════════════════════════════════════════════
TI = "EPIC: Test infrastructure — the rig, the oracles, and the batteries"
add(TI, "closed", [E], None, None, """
**DONE**, and it is the reason the rest of the project is trustworthy. Three layers:

1. **Off-VM batteries** — 17 native test binaries under `tools/dostest/`, seconds on the
   build machine. The fast loop.
2. **Oracles** — never grade our own homework. Stock ntvdm, a real MS-DOS 6.22 under QEMU,
   Nuked-OPL as a black box, and Doom's own WAD.
3. **The bare-metal rig** — a real XP box driven over SMB, because V86/DPMI on real
   silicon cannot be tested anywhere else.
""")

for t, b in [
    ("Bare-metal XP test rig driven over SMB",
     "**DONE.** A real XP box with a watcher loop plus an independent `controld` daemon (`reboot | poweroff | kill | quit | exec`) so each channel can recover the other. ⚠️ Traps that each cost a reboot: deploying the wrong one of the two built EXEs; Python text mode stripping CRs from .bat files; a stale `TN` silently re-running the last target; and `watcher.txt` merely existing meaning nothing."),
    ("Stock ntvdm as a first-class oracle",
     "**DONE** — `stock <target>` runs anything under Microsoft's ntvdm instead of ours, and proves the IFEO key state with `reg query`. This is the answer to 'what does real DOS actually do?' and it settled a keyboard question in one run after three wrong hypotheses. It also established that **stock ntvdm reaches Doom's title screen on this hardware** — previously a load-bearing assumption, never measured."),
    ("MS-DOS 6.22 oracle under QEMU with automated capture",
     "**DONE** — was #25. `scripts/dosoracle/`."),
    ("Differential test harness: one .COM, several hosts, one diff",
     "**DONE** — was #26. `scripts/dosdiff.py`. ⚠️ **The voting rule is the point:** the oracles vote on truth, agreement between them IS truth, and **NTVDMEX does not vote** — it is the subject under test. Disagreement is reported as DISPUTED, never resolved by majority."),
    ("Make every unimplemented path LOUD",
     "**DONE** — was #27. No silent no-ops, no 0000:0000. A call that does nothing and reports success is the defect class that costs the most sessions."),
    ("Nuked-OPL as a black-box oracle for the clean-room synth",
     "**DONE.** `tools/oplref` + `tools/oplprobe`. Keeps our synth MIT clean-room while still having ground truth."),
    ("doomoracle: ground truth from the game's own data files",
     "**DONE.** `tools/doomoracle/`. ⚠️ Replaced a home-made even-column video metric that MOVED THE WRONG WAY when a bug was fixed — and had been steered by for rounds. ⚠️ The IWAD is deliberately NOT committed (it is id's); point the tool at your own copy."),
    ("rigshot: let the rig see its own window",
     "**DONE.** `scripts/bm/rigshot.c`. The host's own screenshot captures the GUEST framebuffer, so it can never show the caption, status strip, menu bar or a dialog — which is why every UI change needed a human at the box. ⚠️ Change a tab by CLICK, not `TCM_SETCURSEL`: the latter does not raise `TCN_SELCHANGE` and cross-process `WM_NOTIFY` carries an unmarshalled pointer."),
    ("dlgcheck: verify dialog layout from the linked binary",
     "**DONE.** `tools/dlgcheck/dlgcheck.py` parses RT_DIALOG templates out of the PE. ⚠️ It reported 47 problems and 45 were its own: **a COMBOBOX's template height is the height of its DROPPED LIST**, not the closed control. An instrument's model of its subject is a claim and must be checked before its output is believed."),
]:
    add(t, "closed", [], None, TI, b)

# ══════════════════════════════════════════════════════════════════════════════
# OPEN WORK
# ══════════════════════════════════════════════════════════════════════════════

WOW = "EPIC: WOW / Win16 — run 16-bit Windows programs"
add(WOW, "open", [E, "P0"], "M5 - Win16/WOW foundation", None, """
**NOT STARTED. This is the single largest gap between NTVDMEX and `ntvdm.exe`, and it
blocks the project's actual goal.**

`ntvdm.exe` is not only a DOS box — it is the **WOW subsystem host**: every 16-bit
*Windows* program on XP runs inside it. NTVDMEX has none of that. Verified by inspection:
no NE loader, no `krnl386`/`user`/`gdi` hosting, no 16:16↔flat thunking anywhere in `src/`.

**Consequence today:** because interception is an IFEO `Debugger` key on `ntvdm.exe`, and
Win16 launches also go through `ntvdm.exe`, installing NTVDMEX permanently would break
every 16-bit Windows application. The passthrough guard in the M10 epic is the stopgap;
this epic is the real answer.

Existing children: #3, #4, #5, #6.
""")

add("Win16 launch detection + passthrough to stock ntvdm (stopgap)", "open", ["P0"], "M10 - Installation & routing", WOW, """
**Stopgap that makes 'leave it installed' safe long before WOW is written.**

A WOW/Win16 invocation of `ntvdm.exe` is distinguishable from a DOS one by its command
line (`-m -w -f...` and friends). Detect it and hand the launch straight back to the real
`ntvdm.exe`, so NTVDMEX only claims the DOS path it actually implements.

Without this, the M10 install is a trap: 16-bit Windows apps silently stop working.
Small change, high value, and it is the difference between a test-rig hack and something
you can leave on a machine.
""")

M10 = "EPIC: Installation & routing — make NTVDMEX the machine's VDM, reversibly"
add(M10, "open", [E, "P0"], "M10 - Installation & routing", None, """
**The project's actual goal, and it has never been a tracked work item.**

Install NTVDMEX so that **every MS-DOS and Win16 launch routes to it** and stock `ntvdm`
lies dormant — without overwriting `System32\\ntvdm.exe`.

**Why not replace the file:** it is Windows File Protection territory. The mechanism that
works is an **IFEO `Debugger` value on `ntvdm.exe`** (the `Control\\WOW\\cmdline` repoint
was tried and disproven). That achieves the routing goal without fighting WFP, and it is
reversible with a single `reg delete`.

**What this epic needs:**
- An installer/uninstaller that sets and clears the key, with a verifiable state readout.
- The Win16 passthrough guard, so routing everything does not break Win16 before WOW exists.
- Console/stdio behaviour good enough for non-interactive use (see the console issue).
- A documented recovery path for a machine where NTVDMEX will not start.
""")

add("Installer / registration tooling", "open", ["P1"], "M10 - Installation & routing", M10, """
Was #13, now scoped to the routing goal. Set/clear the IFEO `Debugger` value, report the
current state, and refuse to install over a half-configured machine.

⚠️ The IFEO key is the project's most reliable foot-gun: **left absent, every test
silently measures stock ntvdm and still "passes"**. The installer should be able to say,
out loud, which VDM a given machine will actually use.
""")

add("Console/stdio integration: DOS output bypasses shell redirection", "open", ["bug", "P1"], "M10 - Installation & routing", M10, """
**Found by inspection during the M10 readiness review; not previously tracked.**

NTVDMEX always creates a GDI window, and flushes captured DOS output to `CONOUT$` **at
exit** (and truncatably — `out_trunc`). Real `ntvdm.exe` runs a console-mode DOS program
*inline in the console it was launched from*, inheriting stdin/stdout/stderr, so
`myprog.exe > out.txt` and `myprog.exe | more` work from cmd.exe.

Consequences as things stand:
- Redirection from the Win32 shell is bypassed — output goes to the console, not the file.
- Nothing appears until the program exits.
- Every DOS program pops a window, including from a batch file.

This is a blocker for "leave it installed as the machine's VDM": anything script-driven
changes behaviour.
""")

add("Recovery path when NTVDMEX will not start", "open", ["P2"], "M10 - Installation & routing", M10, """
If the host crashes or wedges on startup, every DOS and Win16 launch on the machine is
broken and the fix requires editing the registry. Document and, where possible, automate:
a safe-mode switch, a watchdog that clears the key after N consecutive failures, or an
uninstaller reachable without a working VDM.
""")

# open defects / follow-ups discovered but untracked
add("BUG: `echo x > file` writes to the screen and leaves the file 0 bytes", "open", ["bug", "P1"], "M9 - DOS/BIOS completeness (TDD)", SH, """
In-guest output redirection does not work. **Three attempts failed, all aimed at the WRITE
end** — AH=40h honouring a bound handle, AH=02h/`OUTC` routed through handle 1, and
lowest-free-handle allocation. All three are correct on their own terms and are kept; none
was sufficient.

The trace does not fit the textbook idiom:

```
21:3c/06 ...        <- CREATE hi.txt
21:3e/24 bx=0001    <- CLOSE handle 1, AFTER the create
21:3e/24 bx=0005..0013
...no 45h and no 46h anywhere in the whole run...
```

▶ **THE ONE FACT STILL MISSING is what AH=3Ch RETURNS.** The handle number is the whole
question and the trace prints only the request. Instrument that first — a trace that
prints the REQUEST but not the ANSWER is half an instrument. Do not guess a fourth time.
""")

add("BUG: the `$p` prompt degrades `C:\\dostest>` to `C>` after an EXEC", "open", ["bug", "P2"], "M9 - DOS/BIOS completeness (TDD)", SH, """
`$p` is the current directory, so AH=47h stops answering after a child process runs.
Spotted, **not diagnosed** — undiagnosed, not known-benign.

Lead: `src/dos/dos_int21.c` notes that AH=47h is **unimplemented for any drive other than
the current one** ("only the current drive is tracked"). Per-drive current directories are
real DOS behaviour. Suspect the EXEC path disturbs the tracked directory, or COMMAND.COM
asks for an explicit drive after an EXEC.
""")

add("XP's own COMMAND.COM exits during init, printing nothing", "open", ["research", "P2"], "M9 - DOS/BIOS completeness (TDD)", SH, """
With the version knob set to 5.00 it gets past "Incorrect DOS version", reaches deep shell
init and terminates from `CS:IP=0x95eb:0x03ce` having printed nothing.

- **AH=53h (BPB→DPB) was TESTED as the cause and REFUTED** — answering success with a
  zeroed DPB gave the same CS:IP after the same 32 ms. Reverted rather than left lying.
- What it asks for and does not get is **INT 2Fh AX=122Eh**, the five DOS error-message
  table addresses (DL=00/02/04/06/08, five calls in a row). We pass unrecognised INT 2Fh
  straight through, so the guest reads its own registers back as our answer.

⚠️ "Died after 122Eh" is still not "died because of 122Eh". Prove it first.
""")

add("Wire up the settings that are stored but not honoured", "open", ["P2"], "M8 - Polish & SDK", UI, """
40 of the 46 settings round-trip through `HKCU` faithfully and **change nothing**. They
came from the old menu scaffold where they were `IDM_STUB`.

`settings_apply()` in `src/host/main.c` is the honest list of what actually works — a
setting absent from it is one the emulator does not consult. Wiring one up means adding a
line **there**; the storage already exists.

Highest value first: CPU speed (#56), window size/scaler, master volume/mute, SB
address/IRQ/DMA, drive mounts.
""")

add("Ctrl+Tab does not switch settings pages", "open", ["P2"], "M8 - Polish & SDK", UI, """
A real property sheet switches pages on Ctrl+Tab; this hand-rolled tab dialog has no
handler for it. Not a defect against the original spec, but it is the first thing a
keyboard user tries.

Also unverified: keyboard navigation ACROSS the page boundary (what `DS_CONTROL` is for).
The dialog has been driven by mouse clicks only.
""")

add("Fullscreen + captured shows the release chord nowhere", "open", ["P2"], "M8 - Polish & SDK", UI, """
In exclusive fullscreen the DirectDraw primary covers the status strip, exactly as it
covers the menu bar. While fullscreen **and** captured, nothing on screen names the
release chord. Win+F10 still works.

The caption used to carry it everywhere; moving it to the strip traded reach for tidiness.
Fix is an on-screen hint drawn in the present path, not a return to the caption.
""")

add("OPL: snare, hi-hat and cymbal need the chip's special phase generator", "open", ["P2"], "Playable Games (Doom / Skyroads / ZAR)", SND, """
Deliberately **not guessed** and counted as NOT SYNTHESISED rather than approximated.
The rhythm-mode percussion voices use the OPL's special phase-generator behaviour, which
has not been reverse-engineered against the Nuked oracle yet.

Melodic passages correlate 0.96-0.97; this is the remaining gap.
""")

add("Verify NTVDMEX against the WOW16 + DOS launch matrix", "open", ["P1"], "M10 - Installation & routing", M10, """
Before NTVDMEX can be the machine's VDM, there needs to be a matrix of launch shapes that
all demonstrably work or demonstrably pass through:

- DOS `.COM` / `.EXE` from Explorer, from cmd.exe, from a batch file, redirected, piped
- A DOS program that EXECs a child
- A 16-bit Windows `.EXE` (NE) — must pass through until the WOW epic lands
- A DPMI/DOS-4GW game
- A program that fails to load at all

Each row: what stock ntvdm does, what NTVDMEX does, verdict. This is `scripts/dosdiff.py`
applied to launch shapes rather than API calls.
""")


# ── issues that already exist and should now be CLOSED ────────────────────────
CLOSE_EXISTING = {
    2:  "Closed by the DPMI epic. INT 31h surface is implemented and INT 2Fh AX=1687h responds; Doom (a DOS/4GW client) is fully playable through it.",
    7:  "Closed by the Sound epic. SB16 PCM at 99.999%, clean-room OPL2/OPL3, MPU-401 MIDI — Doom plays with sound, user-confirmed by ear. Residual grounding question stays open as #57; OPL percussion has its own follow-up.",
    18: "Closed. The kernel monitored-gate is satisfied: real-CPU protected mode runs with I/O, graphics, input and timing through the VDDs, with no reflect and no interpreter.",
    19: "Closed. 32-bit DOS/4GW protected mode runs on real silicon — Doom executes its own 32-bit code and is fully playable.",
    20: "Closed. SB16 DSP + DMA PCM playback works; delivery measured at 99.999%. The fix was pacing the PIT, not the audio path.",
    21: "Closed. Clean-room OPL2/OPL3 with Nuked as a black-box oracle: waveform correlation 0.41→0.91, melodic passages 0.96-0.97. Percussion voices tracked separately as a follow-up.",
    22: "Closed. MPU-401 / General MIDI output works — Doom's music plays.",
    26: "Closed. `scripts/dosdiff.py` implements the differential harness, including the voting rule: oracles vote on truth, NTVDMEX does not vote.",
}

# ── issues that LOOK done but are not — corrected scope, left open ────────────
COMMENT_EXISTING = {
    13: "**Re-scoped to the project's actual goal** and adopted under the Installation & routing epic. This is no longer 'polish': it is how NTVDMEX becomes the machine's VDM for every DOS and Win16 launch, reversibly, without touching WFP-protected `System32\\ntvdm.exe`. The mechanism is the IFEO `Debugger` value on `ntvdm.exe`. Blocked on the Win16 passthrough guard, without which installing breaks every 16-bit Windows program.",
    28: "**Partially done — deliberately left open.** The reported DOS version is now a real knob (registry + Settings > General, default 6.22, 5.00 for XP's COMMAND.COM). What is NOT done is the second half of this issue: **version-ALIGNED API behaviour**. We report a version; we do not change API behaviour to match it.",
    34: "**Partially done — deliberately left open.** AH=59h extended error is implemented (and an exhausted FCB search now records a reason). **INT 24h critical-error handling and the 34h InDOS flag are NOT implemented** — no critical-error path exists in `src/dos/`.",
    49: "**Still open, and honestly reported in-code.** `ah == 0x31` is handled but residency is explicitly NOT honoured: the host runs one program at a time and says so rather than pretending. INT 27h likewise. See the note at `src/dos/dos_int21.c` AH=31h.",
    56: "**UI exists, behaviour does not.** Settings > CPU now has Speed (Auto/Maximum/Fixed cycles) and a Cycles field, stored in `HKCU`. Nothing reads them yet — see the 'settings stored but not honoured' issue.",
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    existing = {}
    for it in json.loads(gh("issue", "list", "--state", "all", "--limit", "500",
                            "--json", "number,title")):
        existing[it["title"]] = it["number"]

    have_ms = {m["title"]: m["number"] for m in
               json.loads(gh("api", f"repos/{REPO}/milestones?state=all"))}

    print(f"== {len(existing)} existing issues, {len(have_ms)} milestones")

    # milestones
    for title, state, desc in MILESTONES:
        if title in have_ms:
            continue
        print(f"  + milestone: {title}")
        if not a.dry_run:
            out = api("milestones", "POST", title=title, state="open", description=desc)
            have_ms[title] = json.loads(out)["number"]

    created, skipped = 0, 0
    numbers = {}
    for spec in ISSUES:
        if spec["title"] in existing:
            numbers[spec["title"]] = existing[spec["title"]]
            skipped += 1
            continue
        print(f"  + [{spec['state']:6}] {spec['title'][:78]}")
        created += 1
        if a.dry_run:
            continue
        args = ["issue", "create", "-R", REPO, "-t", spec["title"], "-b", spec["body"]]
        for l in spec["labels"]:
            args += ["-l", l]
        if spec["milestone"] and spec["milestone"] in have_ms:
            args += ["-m", spec["milestone"]]
        url = gh(*args)
        num = int(url.rstrip("/").split("/")[-1])
        numbers[spec["title"]] = num
        if spec["state"] == "closed":
            gh("issue", "close", str(num), "-R", REPO, check=False)

    print(f"== issues: {created} to create, {skipped} already present")

    # link children under their epic
    if not a.dry_run:
        by_parent = {}
        for spec in ISSUES:
            if spec["parent"]:
                by_parent.setdefault(spec["parent"], []).append(spec["title"])
        for parent, kids in by_parent.items():
            pn = numbers.get(parent)
            if not pn:
                continue
            lines = "\n".join(f"- #{numbers[k]} {k}" for k in kids if k in numbers)
            body = gh("issue", "view", str(pn), "-R", REPO, "--json", "body", "--jq", ".body")
            if "### Children" in body:
                continue
            gh("issue", "edit", str(pn), "-R", REPO,
               "-b", f"{body}\n\n### Children\n{lines}", check=False)
            print(f"  ~ linked {len(kids)} children under #{pn}")

    # close what is demonstrably done
    for num, why in CLOSE_EXISTING.items():
        print(f"  x close #{num}")
        if not a.dry_run:
            gh("issue", "comment", str(num), "-R", REPO, "-b", why, check=False)
            gh("issue", "close", str(num), "-R", REPO, check=False)

    for num, why in COMMENT_EXISTING.items():
        print(f"  ~ annotate #{num}")
        if not a.dry_run:
            gh("issue", "comment", str(num), "-R", REPO, "-b", why, check=False)

    print("== done")


if __name__ == "__main__":
    sys.exit(main() or 0)
