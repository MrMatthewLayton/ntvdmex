# Session 68 — The rig was silently stock; Lemmings reaches gameplay by hand

> Session 68. Split out of `docs/STATE.md` on 2026-09-23.

> Verbatim from `docs/STATE.md`. Nothing has been edited or corrected,
> including conclusions a later session refuted — same rule as the rest of
> this archive. See [`README.md`](README.md).

## ★★★★★ SESSION 68 (2026-09-12/13) — **THE RIG WAS SILENTLY STOCK; LEMMINGS REACHES GAMEPLAY BY HAND.**

**HEAD `b4791aa`, committed, not pushed.** Battery 1200/0, interp 170/170.

1. **"No DOS app runs" = two silent mechanisms stacked** (`d6f8014`). `[0x714]`
      (`FIXED_NTVDMSTATE`) was never written and inherits the machine's real-mode boot
      garbage, so it changed after the Sep-12 reboot to a value with bit 0 set → the
      kernel raised VIP → the first `STI` was a raw #GP → XP tore the VDM down in <1 s
      with no exception and a 2.6 KB log. Then GH #132's counter, which cleared only on
      a clean exit, counted the X-button (TerminateProcess) and guest crashes as failed
      starts, and the fourth launch **removed the IFEO key** — every later launch ran
      stock ntvdm. Now: `[0x714]` written to 0 after `VdmInitialize` (logged as
      `STAGE1: FIXED_NTVDMSTATE inherited=… -> set …`, override `cfg\vdmstate.txt`), and
      the counter clears the moment the window is up. `rt.bat setup` restores the key;
      `ifeochk.bat` is READ-ONLY now (it used to `reg add` a deleted path).
2. **Lemmings** (`0e48ff7` `e595c91` `5225cb5`): the un-erased trail, the trapdoor
      flicker and lemmings falling through the ledges were **one cause** — in a planar
      mode the host interpreter is the CPU and A0000 is unprotected in V86; an opcode it
      did not model (`repne scasb`, `jmp far [m]`, `CBW`, `XLAT`, `LES`) bailed to V86
      **until the next event**, and every VRAM access in that stretch was invisible to
      `st->plane[]`. Modelled them all (+ `POP r/m16`, `WAIT`, `LAHF/SAHF` from Bubbles'
      1.17M bails). **Every report now prints `STAGE2: P12 non-BOP bail sites=` — read it
      FIRST for any planar-mode guest.** Headless: OUT 0→10 on the reference's clock.
      **User, by hand: lemmings fall into the first chamber correctly.**
3. **Settings → Display → Start fullscreen** Always / Graphics only / Never
      (`8ca0c5b`, registry `StartFullscreen`). User: "worked great".
4. Instruments: `cfg\planedump.flag` (planes beside each capture), HB line carries
      `crtc=/flips=/ofs=` (and its buffer was 384 for a 392-byte line since s62),
      `cfg\livehb.flag`, `scripts/bm/lemlive.bat`, rigshot `key` now sends scancodes.

**▶ NEXT — the user's four remaining Lemmings symptoms, measured in his own 49 s run,
saved as `runs/lemref/user_s68/ntvdmhost_byhand.log`:** (1) briefing screen flickers,
(2) game screen flickers **between right and wrong COLOURS** (`dacw=52528`, ~18 DAC
writes/frame — a palette-tear/blank-during-flip hypothesis, unmeasured), (3) music
too slow — **by hand `pit_reload=0x4bb9` = 61.5 Hz where every headless run had 0**,
delivered 59 IRQ0/s, so the s67 "not slower" verdict does not cover this path,
(4) an in-game click does not assign a skill (`MOUSEI33`: only AX=3 level polls;
`captured=1`; check the X doubling of `i33_xshift` against where the crosshair is
drawn). Then: sweep the other guests' bail sites; the user's `claude.txt` list.

---
