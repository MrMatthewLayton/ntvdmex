# Motivations and decisions

Why this project exists, and why it is built the way it is. Every decision below was made
against a specific alternative, and several were made *twice* because the first answer was
wrong — those are the interesting ones.

---

## Why build this at all

Windows XP's `ntvdm.exe` works, but it is a black box that stopped evolving in 2001, and on
modern hardware it is the only thing standing between a 30-year-old DOS program and a
processor that could run it a thousand times over. The interesting question is not *can a
DOS program be made to run* — DOSBox answers that — but **can the operating system's own
VDM be replaced with something better, on the real CPU, while keeping everything the
platform expects.**

The bar was set concretely rather than aspirationally: **run real DOS games well.** Doom,
Skyroads, ZAR, with flawless sound. A bar you can hear and see is much harder to fool
yourself about than "80% API coverage".

---

## Real CPU, not emulation

**Decision:** execute guest code in Virtual-8086 mode on the real processor, reusing the NT
kernel's VDM machinery via the undocumented `NtVdmControl` syscall.

**Alternative rejected:** write a CPU emulator (the DOSBox approach), or ship a custom V86
kernel driver.

**Why:** real `ntvdm` already does this on x86 — emulation in Microsoft's implementation
was only for non-x86 NT and 64-bit Windows. Matching the platform means a DOS program runs
at native speed, and it means the *hard* parts (paging, protection, mode switches) are
handled by silicon that is already correct. ReactOS's reimplementation was the primary
reference for the undocumented structures.

**What it cost:** the kernel enforces state it does not document. Protected-mode faults
would not reflect back to us until a specific monitored-gate condition was satisfied, which
took a multi-session reverse-engineering effort against a kernel debugger. A host
interpreter (`src/host/v86interp.h`) was built as the fallback during that period and is
still there.

**What it bought:** DOS/4GW detects our DPMI host and enters 32-bit *paged* protected mode
on real silicon. That is untestable under QEMU+HVF, which aborts on exactly that path even
under stock `ntvdm`. Doom would not exist on this project without it.

---

## Interception: how NTVDMEX becomes the VDM

**Goal:** every MS-DOS *and* Win16 launch routes to NTVDMEX; stock `ntvdm` lies dormant.

**Not by replacing `System32\ntvdm.exe`.** That file is under Windows File Protection.
Signing is *not* the obstacle — XP-32 does not enforce user-mode EXE signatures — but WFP
will restore the original from `dllcache` behind your back, which is a uniquely confusing
failure to debug.

**First plan, disproven:** repoint
`HKLM\SYSTEM\CurrentControlSet\Control\WOW\cmdline` (DOS) and `wowcmdline` (Win16). This
looked like the designed extension point. It does not work.

**What actually works:** an **Image File Execution Options `Debugger` value on
`ntvdm.exe`**. Windows launches your binary with the original command line appended, you
call `GetNextVDMCommand` to recover the program, and you are the VDM. It achieves the
routing goal without touching a protected file and is reversible with one `reg delete`.

> ⚠️ **This is the project's most reliable foot-gun.** With the key absent, every test
> silently runs against *stock* `ntvdm` and still "passes". More than one session has been
> spent debugging code that was never executed. Any harness that runs a test must be able
> to state which VDM it actually used.

> ⚠️ **It intercepts Win16 too.** 16-bit Windows programs also launch through `ntvdm.exe`.
> Until the WOW layer exists, installing NTVDMEX permanently breaks them — hence the
> passthrough guard in [#129](https://github.com/MrMatthewLayton/ntvdmex/issues/129).

---

## Scope: DOS *and* Win16

Win16 (the WOW layer: `krnl386`/`user`/`gdi` hosting, NE loading, 16:16↔flat thunking) has
always been in scope, and is by some distance the heaviest single chunk of the project. It
is also entirely unstarted. Everything shipped so far is the DOS half.

This is stated plainly because it is easy to look at "Doom is fully playable" and conclude
the project is nearly done. It is nearly done with *one of its two halves*.

---

## Video: virtualise and blit, not bare metal

An early requirement was "bare-metal VGA/VESA". That did not survive contact with reality:
a windowed VDM cannot own the CRTC. Video is virtualised and blitted to a Luna-themed
window (GDI) or an exclusive-fullscreen DirectDraw surface, which is what real `ntvdm`
does.

**One consequence is load-bearing:** in planar modes (mode 12h) the A0000 page trap that
would let us watch guest writes *freezes the guest* on real hardware. So in planar modes
**the interpreter is the CPU** — we decode and execute the guest's stores ourselves. That
is why an x86 instruction-length decoder exists in a project that was supposed to avoid
emulation.

---

## Sound: clean-room, with an oracle

The OPL2/OPL3 FM synthesiser is written from the chip documentation and is MIT-licensed.
Nuked-OPL — which is exact and GPL — was used strictly as a **black-box oracle**: generate
the same note on both, compare waveforms, find the discrepancy. None of its code is here.

This is the pattern the whole project leans on: *have something that is not you tell you
what the right answer is.* It found that modulation depth was halved (correlation 0.41 →
0.91) plus four more defects, none of which listening would have caught.

Percussion voices (snare, hi-hat, cymbal) need the chip's special phase generator and are
**deliberately not guessed** — they are counted as *not synthesised* rather than
approximated, because a wrong drum that plays is harder to notice than a missing one.

---

## Settings precedence: the text files win

Configuration lives in `HKCU\Software\NTVDMEX` with a dialog. But the order is:

```
built-in default   <   registry   <   text file on the test share
```

and `settings_load()` runs at the *top* of the knob block in `WinMain`, never after it.

**This looks backwards and is deliberate.** The headless rig configures the host by writing
files and re-launching it. If the registry overrode them, every headless measurement would
silently report whatever was last clicked in a dialog on that machine — which is exactly
the "instrument lied" failure this project keeps paying for. Do not tidy this ordering.

---

## Honesty over coverage

A running theme, and it is a real engineering decision rather than a slogan:

- **Every unimplemented path is LOUD.** No silent no-ops, no `0000:0000` returns. A call
  that does nothing and reports success is the most expensive defect class there is.
- `AH=31h` (TSR) is *handled* and explicitly refuses to pretend residency worked.
- `AH=53h` was tested as the cause of a bug, **refuted**, and reverted rather than left in
  as a plausible-looking success.
- 40 of 46 settings are stored and honoured by nothing — and `settings_apply()` is
  documented as the honest list of which ones the emulator actually consults.

The reasoning: this project's failures have almost never been "we did not implement X".
They have been "we implemented X wrongly and something told us it was fine".
