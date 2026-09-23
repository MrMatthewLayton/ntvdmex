# Session 70 — Lemmings closed for real: the timer restarts per the datasheet, IRQ0 held in service

> Session 70. Split out of `docs/STATE.md` on 2026-09-23.

> Verbatim from `docs/STATE.md`. Nothing has been edited or corrected,
> including conclusions a later session refuted — same rule as the rest of
> this archive. See [`README.md`](README.md).

## ★★★★★ SESSION 70 (2026-09-13, evening) — **LEMMINGS #3 CLOSED FOR REAL: THE TIMER RESTARTS PER THE DATASHEET, AND IRQ0 IS HELD IN SERVICE.**

**HEAD `f6a2080`, committed, NOT pushed. Battery 1238/0 (pit 35). Rig binary = HEAD
(`09a42b5a…`). Deadline: the 17th.**

The s69 "fader-vs-timer coupling" was never a fader problem. Read from the guest
binary + the 8254 datasheet, then measured: Lemmings' HP-mode timer ISR (`CS:17C7`)
does `sti`, spins on `0x3DA` for the retrace, then `out 43h,36h` + the calibrated
count. Its main loop runs one game frame per 5 ticks and steps the level fade one
step per game frame — it never waits on the retrace in HP mode (`0x31E`). So the
design is a 70 Hz retrace-locked tick, and two host bugs hid each other:

1. **PIT (`vdd_pit.c`)**: read-back was the free-running phase → random calibration
      reload (the slow music). And the datasheet says Control Word + count **restarts**
      the period in modes 2/3 too ("synchronized by software"); only a *bare* count
      write waits for the period's end. s69's two models were each wrong one way. Now
      `cw_armed`/`next_pending` + count-from-load. Tests T11–T15 from the datasheet
      quotes, verified failing on the old model.
2. **PIC / host**: IRQ0 was auto-EOI'd on delivery (ba927ac). With a correct 70 Hz
      tick, an IRQ0 raised during the ISR's `sti` spin **re-entered the handler**, and
      every tick then nested one level deeper forever — no ISR body (no music), no main
      loop (black screen = "stuck fade"), stack overrun (the es=0xD000 blitter AV). The
      tell was `irq0/s == flips/s == 70` with `p3da` at 3.3M reads/s. Now IRQ0 is held in
      service until the guest EOIs (or our INT 08h BOP EOIs, as the BIOS does), PIC
      ISR/IRR updates are atomic, and a 250 ms timeout ×3 falls back to auto-EOI with a
      log line. STAGE2 `irq0_isr[strict,auto,blocks,timeouts,fallback]`.

**Measured (rig, headless, keyed to gameplay):** Lemmings IRQ0 70.0/s == vbl edges,
level fade 2.5–3 s (real ≈2.3 s), `0x3DA` share 24% (the spin), 0 blocked / 0
timeouts, no faults. Skyroads `n8=0x67 max_ms=0x13 ui_gap=0x8d23` — the baseline
exactly; keyed play run's frozen-at-`0110:3B40` ending is identical on the previous
binary (A/B). ⚠ The DPMI (protected-mode) delivery arm still auto-EOIs IRQ0 — Doom's
path, by hand only.

**Also (user-specified, same evening): capture RULE 6 — released means released.**
Raw input followed focus, not capture, so after the Windows key gave the pointer
back, dragging on the desktop still mouse-looked in the game while our window was
foreground, and right-clicks over the picture still reached it. Now a mouse-using
guest that is not captured gets no deltas, no position, no buttons; release reports
any held button as let go. `mouse_goes_to_guest()` is the one decision point.
Skyroads on baseline, Lemmings' scripted click still lands while captured.
**USER-CONFIRMED by hand in Doom and Lemmings.**

**User's by-hand result (21:03): screen no longer blank, fade completes — but the
palette still flickers and "it stalled when the trapdoors opened".** The heartbeat
named the stall: every sample in the ISR's retrace spin, `irq0 == edges == 71/s`,
`p3da` 3.1M/s, and the new counters `blocks=796 timeouts=1`. Not nesting (the guard
held) but a **LOCKSTEP**: one long tick (>250 ms in service, the trapdoor moment)
left a tick queued behind the handler; the queued tick re-entered at the EOI, that
instance spun to the next retrace and re-armed the PIT, and its own tick then
fired during the next instance's spin — queued again, forever. A real 8259 would
do the same after such a stall; the game's design is fragile, a 386 never stalled.
**Fix (rig-clean, by-hand unproven): a PIT restart (CW+count) from INSIDE the IRQ0
handler drops the tick queued behind it** (`host_pit_resync_check`, both port
paths; counted as `resync_drop`). Guests that program the timer once are untouched.
Plus `IRQ0-ISR-LONG` lines (≤8/run) naming the guest cs:ip during a long episode.
⚠ One headless run died silently at `russell.dat` (first run after a deploy, the
s69 shape, `runs/lemref/s70_headless_death1.log`); 4 later runs were clean.

**★★★★★ USER-CONFIRMED (22:xx): "everything worked! Lemmings is now playable. I've
just completed two levels."** On the `3026254` build (`0908cce5…`). The briefing
screen no longer flickers; a small part of the GAME screen still flickered — that
is the raster split below.

**The remaining flicker, root-caused and fixed off-VM (not yet deployed):** Lemmings
keeps TWO palettes for DAC 16–23 — `ds:2668` pushed by the timer tick, which is
calibrated to land at row 160 (the toolbar's top), and `ds:2650` pushed after the
retrace. Level in one, toolbar in the other, separated by the beam. Our presenter
applied one palette per frame, so the snapshot's phase decided which half was wrong.
⚠ The real-DOS oracle under QEMU shows ONE set everywhere (its default 0x3DA makes
the two writes land back to back) — for raster effects the oracle is not truth; the
game's tables and timing are. Now: `pal_base`/`pal_split`/`pal_split_row` per entry
in the video VDD (`pal_split_note`, keyed on the same beam model as 0x3DA), resolved
per row by the presenter (all three paths) and the capture (24bpp when split), pinned
by video_test T-SPLIT with the fake clock (8 checks, incl. phase independence and
expiry). Battery green.

**The rest of the evening, in order (all on the rig now, `f79d954`):**
* Deploying the raster split over the confirmed build made it WORSE ("flicker at
     the bottom") — rolled back; rule written: a user-confirmed build is the rollback
     copy and is not replaced without an explicit go.
* Instrumented runs by the user proved the mechanism: HP mode (1,2) writes two
     palettes per frame with the tick landing on rows 160–169 (reload `0x3192`);
     normal-PC mode (1,1) writes none during play and is fine.
* Why the split looked worse: the calibration landed the tick on a different row
     every run (306–383 lines), and our IRQ jitter moved it ±5 rows frame to frame.
     Root cause of the run-to-run spread: the interpreter's port path (`iio_out`) did
     not sync the PIT clock before 0x40–0x43, unlike the reflected path — load and
     latch each read a clock stale by up to a pacer round (`19dd3b8`). Spread went
     ±14 → +2..+10 lines. Sticky split boundary (12 rows) + 2-frame expiry absorb
     the jitter (`40112ac`). The multi-line "blank debt" repayment was tried twice
     and measured wrong off-VM; s69's one-blank rule stays, bounded to recent polls
     (`f79d954`). Stall-aware IRQ0 timeout (capture stalls are not the guest's).
* Headless: reload 322/322/330 lines, 0 blocks/timeouts/drops, Skyroads baseline.
* **USER VERDICT (23:05): "Clean. There is some slight flicker … Lemmings is
     playable, and flicker is minimal."** Session paused here at the user's request.

**▶ NEXT:** (a) the user wants to judge the residual flicker against the oracle
themselves — note the QEMU oracle shows ONE palette everywhere (wrong for raster
effects); a real-hardware reference or the game's own tables are the oracle;
(b) the standing ask: **oracle-backed regression gates** (`tools/lemgate`: log
invariants — reload 320–330 lines, IRQ0 == retrace ±2/s, fade 2–4 s, blocks/
timeouts/drops 0, no faults, run completes; per-region palette on split frames;
Skyroads bands) so none of this regresses silently; (c) the silent first-run-after-
deploy death at `russell.dat` (3 of ~14 headless runs); (d) #4 click→skill; (e)
strict IRQ0 on the PM arm; (f) the residual +2..+10-line calibration excess (a host
stall inside the count is repaid one line).

---
