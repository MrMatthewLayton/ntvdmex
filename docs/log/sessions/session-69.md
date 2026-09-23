# Session 69 — Lemmings: music fix reverted (it blanked the screen); capture refined

> Session 69. Split out of `docs/STATE.md` on 2026-09-23.

> Verbatim from `docs/STATE.md`. Nothing has been edited or corrected,
> including conclusions a later session refuted — same rule as the rest of
> this archive. See [`README.md`](README.md).

## ★★★★★ SESSION 69 (2026-09-13) — **LEMMINGS: MUSIC-FIX REVERTED (IT BLANKED THE SCREEN); CAPTURE REFINED.**

**HEAD `0097dd1`, committed, NOT pushed. Battery 1291/0. Deadline: the 17th.**

The arc: I fixed Lemmings' "music too slow" (#3) by correcting the PIT calibration
reload, it regressed into a hang, I fixed the hang, then it turned out the *corrected
timing broke the palette fade* (blank gameplay screen). User chose REVERT for the
deadline. Net: Lemmings is visible/playable again (music slow again), plus a durable
crash guard and two capture refinements.

1. **`59ee731`+`9eec369` (REVERTED in `e084638`): the PIT count-from-load timing fix.**
      Lemmings' "High Performance PC" calibrates its tick from a 320-scanline count on
      0x3DA; `pit_current_count` read the free-running phase not the load instant, giving
      a random reload (0x4bb9 = 61.5 Hz by hand). Count-from-load fixed the tempo — but
      it is COUPLED to the palette fader (also timer-driven), and the corrected reload
      (0x2faa) left the fade stuck near-black. ⛔ **A "correct" timing fix broke a
      timing-dependent renderer.** Reverted to the s68 free-running behaviour the user
      confirmed visible. **#3 is OPEN again**; fixing it for real needs the fader
      (`ds:0x2668` scaled from raw `ds:0x25F0` by a timer-driven level; find the level +
      its driver — guest `int 60h`/`int 61h`, `CS:3e6f`/`CS:5827`).
2. **`7b59ae0`: a stray guest pointer must not jam the machine.** The blank-screen
      runs also hit an intermittent AV — the planar interpreter read guest linear
      0xd4013 (es diverged to 0xd000 in Lemmings' sprite blitter) in an unmapped UMB
      hole. `imem_r8/w8` now VirtualQuery-cache each page → unmapped reads return 0xFF,
      writes drop, instead of taking the whole host down. Root cause of the es
      divergence not yet found (needs `IMEM-OOR`/interp-regs from a by-hand crash run).
3. **`0097dd1`: capture policy — focus re-captures + WIN releases without the Start
      menu.** WM_ACTIVATE now re-captures (was click-in-video only); WIN release injects
      one tagged Ctrl tap via SendInput so Explorer sees WIN+Ctrl not lone WIN (the safe
      preventDefault — the literal WH_KEYBOARD_LL hook stays off, it jammed the rig).
      ⚠ **DESKTOP-ONLY, user tests by hand.** Auto-close-on-exit confirmed already done.
4. **Diagnostics kept** (all cheap/gated): `IMEM-OOR` log + interp regs in the fatal
      dump; exit `level-palette signature` scan + fade dump; heartbeat `pal2668=`/`1f7c=`
      (survives a by-hand death); `cfg\pitlatch.flag` gates the 0x3DA poll ring.

**▶ NEXT:** (a) user tests the capture refinements by hand; (b) #3 needs the
fader-vs-timer coupling solved before the reload can move again; (c) Hexen/Heretic
hi-res loaders + VESA hi-res are **by-hand only** (DOS/4GW `1007` headless blocker)
and our VESA is banked-only, no LFB — not quick wins. Method scars this session:
a test written from belief (not the datasheet) certified a hang; a repro harness
"survived" without ever running the game; leaving a hang running + `livehb.flag` let
the host's own crash dump name the fault.

---
