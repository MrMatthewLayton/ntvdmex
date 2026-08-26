# Session 12 — 2026-08-19

> Archived verbatim from `return-ntvdm.md`, which was the single rolling handoff
> file until 2026-08-26. Split by session so it can be read by topic and date
> rather than by scrolling. **Nothing has been edited** — including conclusions
> that later sessions REFUTED, which are kept on purpose: the refutations are
> some of the most valuable material here.

```
═══════════════════════════════════════════════════════════════════════════════
██ CHECKPOINT — 2026-08-19 (session 12). ██
═══════════════════════════════════════════════════════════════════════════════

▶ RESTART POINT (2026-08-19, session 12): branch spike/dpmi-16bit-switch. Host rebuilt clean
  and deployed to `bm/`; rig healthy (watcher + controld beating); share knobs CLEARED (no
  qimode.txt, no keys.txt, no capture.flag, headless cap back to 30 s). Verified this session:
  selftest on the rig **ALL TESTS PASSED**, off-VM input battery **34/34** (rewritten — it now
  tests the guest's BDA, not a host-side stand-in).

★★★★★ **SKYROADS MENUS NOW NAVIGATE** — screenshot-confirmed: intro → menu → DOWN to
"Controls" → Enter → the Controls screen, and on the level-select screen the cursor moves off
"Red Heat / Road 1" to Asteroid Belt. Session 11 got the game PLAYING; this session got its
MENUS working, which was the "arrows are dead in the menu and intro" report.

  ▶ TWO ROOT CAUSES, BOTH ABOVE THE DELIVERY LAYER (delivery was already correct):
  1. **The BIOS keyboard buffer was never maintained.** Our INT 09h stub consumed each
     scancode and DISCARDED it. INT 16h only appeared to work because the window proc pushed
     keycodes into a SEPARATE host-side ring that no DOS program can see. Recognise it by:
     the guest sees `0040:001A` == `0040:001C` == `0x001E`, frozen, forever. INT 09h now
     tracks the E0 prefix + shift/ctrl/alt/lock into `0040:0017`, translates make codes to
     BIOS keycodes (AL=0 for extended keys) and fills the real ring at `0040:001E`; INT 16h
     reads that same buffer; the parallel host-side ring is GONE (one buffer, by construction).
  2. **DOS could not express an extended key.** Every INT 21h console read did
     `return k & 0xFF`, so an arrow (0x4800) arrived as a lone NUL with the scancode thrown
     away. DOS returns NUL and then the SCANCODE ON THE NEXT CALL; `g_conin_pending` now
     carries that second byte across conin/coninnb/conpeek. Without this, arrows are
     structurally unreadable through DOS no matter how perfect the hardware layer is.
  Both were required: (1) puts the key where DOS looks, (2) lets DOS say "arrow".

  ▶ ROUTES — a fix for one proves NOTHING about the other:
  Skyroads' MENU reads keys through **INT 21h** (the guest parks at `DOS_HDLR_SEG:0000`, the
  INT 21h BOP, for most of a run — that heartbeat is what cracked this). IN-GAME it hooks
  **INT 09h and reads port 60h itself** (measured p60=358 in a gameplay run).

  ▶ TRAPS THAT COST TIME THIS SESSION — do not repeat:
  - **`int16=[0,0,0,0]` + `p60=0` does NOT mean "reads no keyboard."** It means "reads by a
    route that leaves no trace" — i.e. DOS calls or direct BDA polling. An earlier session
    concluded the intro reads nothing; it was reading via INT 21h the whole time.
  - **Skyroads has an ATTRACT LOOP** that reaches the credits and even demo gameplay unaided.
    Frames of "it's in game!" are worthless without a NO-KEY control run at the same timings.
    I misread attract frames as success once before the control run corrected it.
  - **Never leave `qimode.txt` on the share.** It drives synthetic keys every 250 ms, which
    makes any interactive probe look wedged (it cost the user a trip to the box).
  - A probe that installs its OWN INT 09h (keyprobe) BYPASSES the host BIOS handler, so it
    cannot test the BDA path at all. That is what `bdaprobe.com` is for — it hooks nothing.
  - Disproved by instrumentation, not argument: the scancode FIFO is NOT overflowing
    (`sc_drop=0` over 514 pushes). The new `sc_push`/`sc_drop` counters exist for this.

  ▶ BEHAVIOUR CHANGE TO KNOW: a guest that hooks INT 09h and does NOT chain now gets no
  INT 16h keys — faithful to real hardware (it replaced the BIOS ISR), but it changed
  keyprobe's output to `B16=(none)`. Guests that chain are unaffected.

  ▶ NEW TOOLING: `tools/dostest/keyprobe.com` (prompted per-key ground truth: RAW port-60h
  bytes / INT 16h AX / shift flags / BDA head-tail) and `tools/dostest/bdaprobe.com` (hooks
  nothing; watches 0040:001A-001C). Two new share knobs: `headless_ms.txt` (decimal ms,
  overrides the 30 s headless cap, clamped to 10 min — needed for interactive runs) and
  `keys.txt` (**scripted** synthetic keystrokes: `w1500` waits, `4d` taps, `e4d` taps an
  EXTENDED key — a hardcoded "tap UP 400x" cannot reach a screen, and UP is a no-op on a menu
  whose first item is already selected, so it cannot tell success from failure).

  ▶ GARBLED TEXT: **FIXED** (`633aae5`) and **user-confirmed in-game** — "Road Completed"
  renders correctly on the physical box, and gameplay through a whole road is therefore
  observed, not inferred. Two bugs, the first HIDING the second: (1) `regs_store` wrote back
  only EAX/EBX/ECX/EDX while `regs_load` read all seven, so **ES:BP was discarded** and the
  guest drew text from whatever pointer it already held -- which is why 15991e9, correctly
  setting ES:BP, changed nothing; (2) the 8x8 ROM font was MANUFACTURED by OR-ing row pairs
  of the 8x16, filling every counter ('A' solid, 'E' noise). Real 8x8/8x14/8x16 dumps now
  ship. The tell that cracked it: after fixing the font data alone the render was
  BYTE-IDENTICAL, proving the guest had never read our table.

  ▶ PERFORMANCE, as played by the user on the physical box (a calibration, not a complaint):
  **genuinely playable**, but with the feel of a game speced for a 386 16MHz / 2MB running on a
  **386 8MHz / 512KB** — a little sluggish. Keyboard and music lag are perceptible but now in
  the **milliseconds**. **DEFERRED BY DECISION — do not pick this up unasked.** When it is
  picked up: keys are still restricted to the SYNCHRONOUS exec-loop path in `host_irq_sink`
  (async key delivery off by default after it once made things worse) while the timer gets
  async delivery, and no instrument measures the real latency yet (needs an echo-on-arrival
  probe, no settle, no drain — the 1-2 s seen in keyprobe was that probe's own settle).

  ▶ NEXT DIRECTION (user's call, 2026-08-19): **GO BROAD, NOT DEEP.** We have hardened exactly
  ONE real DOS application. Start running a plethora of others -- `command.com`, `edit.com`,
  `qbasic`, Doom, and on -- and let breadth of exposure tease out the remaining problems.
  Polishing Skyroads further is NOT the priority.

  ▶ NEW WORKSTREAM: **hardware grounding** — CPU affinity, SpeedStep / power management and
  friends, handled in realistically stable code. This lands directly on our timing path: guest
  clocks come from QueryPerformanceCounter (session-11 `host_pit_sync`), so core migration and
  frequency scaling are in it. Note the framing: **XP's own ntvdm never grounded any of this**,
  so it is superset territory and a real differentiator rather than parity work.
```
