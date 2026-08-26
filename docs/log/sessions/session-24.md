# Session 24 — 2026-08-24

> Archived verbatim from `return-ntvdm.md`, which was the single rolling handoff
> file until 2026-08-26. Split by session so it can be read by topic and date
> rather than by scrolling. **Nothing has been edited** — including conclusions
> that later sessions REFUTED, which are kept on purpose: the refutations are
> some of the most valuable material here.

```
═══════════════════════════════════════════════════════════════════════════════
██ ▶▶▶ SESSION 24 (2026-08-24). **THE TIMER IS NOT STARVED.**                 ██
██     Session 23's central chain is refuted at its FIRST LINK. The echo is   ██
██     real, smaller than reported, and its mechanism is now measured.        ██
═══════════════════════════════════════════════════════════════════════════════

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★ START HERE — THE 60-SECOND VERSION                                         │
└──────────────────────────────────────────────────────────────────────────────┘
  Session 24 did the ONE THING session 23 asked for (TASK 1: instrument which
  refusal `async_inject_irq` returns) and the answer was **none of the three**.
  Following that answer took down session 23's headline as well.
```
   TIMER   NOT STARVED. Doom programs 140 Hz and the client's INT 08h is entered
           135 times a second -- 91% of every tick the 8254 raised. Session 23's
           "we deliver 55 Hz / 39%" counted the ASYNCHRONOUS arm only; the
           cooperative arm delivers 59% MORE on top and was not in the line.
   BACKLOG NOT SATURATED. `owed_max` is a HIGH-WATER MARK. Sampled at every sync,
           92.5% of syncs see an EMPTY backlog and 4 in 2914 see the cap.
   ⇒       "The video path starves the timer, and the timer starves the audio"
           is REFUTED at the first link. TASK 2 and TASK 3 lose their stated
           motivation. (The lock contention is real -- 103 ms waits -- and worth
           doing on its own merits. It is not why the audio echoes.)
   AUDIO   the echo is REAL but 32%, not 44%: a fifth of the old headline was the
           game being SILENT, and silence is identical to the previous lap when
           the guest refills it CORRECTLY. It is a MARGIN RACE, not a stall --
           never more than 2-3 consecutive blocks, 90% of runs are a single one.
   ▶ ROOT  DMX steers by the 8237's channel-1 CURRENT COUNT. It polls 55x/s while
           82 blocks complete -- so 32% of blocks pass unlooked-at, matching the
           32% echo to a point across four runs. And an ASYNC tick reaches that
           poll 13x more often than a COOPERATIVE one (0.89 vs 0.068 polls/tick),
           though both enter the same handler and both complete.
```
  ▶ **THE SINGLE NEXT ACTION** is at the bottom of this block: find out what DMX
    does differently inside a cooperative INT 08h. That is the whole of the echo.

  Branch `m9/completeness`, tree clean but for the same 11 untracked files.
  Gates green on the shipped binary: off-VM **581 checks / 16 suites, 0 failed**,
  check-imports pass, and the BARE-METAL gates re-run this session -- `selftest.com`
  **8/8**, `dpmitest.com` clean exit (0300/0301/0303 + nested INT 31h),
  `dpmiback.com` clean (its `<<< MISMATCH >>>` is the documented benign sentinel).
  **Six commits, `75f00c7`..`4e8d5f0`:**
```
   75f00c7  audio: separate "not refilled" from "refilled with silence"
   a5d8abe  timer: the refusal histogram says the injector is innocent
   57a8772  audio: the DMA poll comes from the TIMER ISR, only the async arm makes it
   c980a5e  docs: session-24 handoff
   b82b675  timer: stop 3249 log lines under the lock -- NOT the regression
   4e8d5f0  timing: scope the per-sync throttle to PM clients (THE SKYROADS FIX)
```
  **ONE FUNCTIONAL CHANGE: `4e8d5f0`, which fixes the Skyroads regression.** Everything
  else is instrumentation and log text, so Doom looks and sounds exactly as it did
  after session 23 -- there is nothing new to hear there until TASK B lands.
  Rig `192.168.1.29` UP, share mounted at `/tmp/xpshare`, **current build deployed
  and md5-verified**, all knobs cleared, `headless_ms.txt`=45000. Six rig runs,
  archived in `build/rigruns/result_doom_19{2358,2942,3301,3717,4040,4306}.log` and
  `..._20{3505,3719}.log` (the two bracket runs).

┌──────────────────────────────────────────────────────────────────────────────┐
│ ⚠⚠⚠ A PLAY SESSION FOUND A REGRESSION NO INSTRUMENT REPORTED. NOW FIXED.     │
└──────────────────────────────────────────────────────────────────────────────┘
  The user played Doom, then tried **SKYROADS** and reported it had regressed --
  still playable, but "a definite timing issue now that affects OPL and graphics".
  It had been fully playable since session 19. **Every counter this host prints was
  inside its normal range**; nothing flagged it, because nothing had re-run the
  title since session 21.
  Bisected against an Aug-21 reference log found on the share, all runs at a matched
  30 s cap. `irq0_inj` reproduces to ±0.3%, so it bisects cleanly:
```
   c740f4e  session 21 HEAD          irq0_inj 4485    <- the Aug-21 log says 4487
   141f347  16-bit VGA index writes  irq0_inj 4588
   07835a5  mode-Y de-interleave     irq0_inj 4505
   e2f7486  bus+timing               irq0_inj 3413    <- HERE  (-24%)
   a6fdee6 / session 24 HEAD         irq0_inj 3418 / 3410
   HEAD with the fix                 irq0_inj 4540
```
  ★ **THE CONTROL MATTERED AS MUCH AS THE BISECT.** Rebuilding session 21's HEAD
    reproduced the reference to **0.04%**, which is what makes this a regression
    rather than two differently-configured runs being compared.
  **CAUSE.** `e2f7486` changed `host_irq_sink` from one async attempt per RAISE to one
  per SYNC. It was written for **Doom**, whose music driver programs the 8254 at
  16 kHz -- 800 raises for a single 50 ms catch-up gap, each a SuspendThread round
  trip inside the device lock. Real pathology, real fix. But it was applied to EVERY
  guest, and Skyroads (V86, 180 Hz, a raise or two per sync) cannot produce that
  burst -- it only paid for it. **The throttle is now scoped to PM clients**: Doom
  keeps exactly the behaviour sessions 22-23 measured and tuned, the V86 path returns
  to what every V86 measurement in this project was taken against. Doom re-measured
  after the fix: every figure inside the day's run-to-run range.
  ⚠ **AND THE FIRST HYPOTHESIS WAS WRONG.** 3249 `ASYNC-EARLY bail` lines per 30 s --
    file I/O under `g_lock` at the PIT's rate -- looked obviously guilty. Capping them
    (3249 -> 32, log 161 KB -> 62 KB) moved `irq0_inj` by 10, i.e. nothing. Kept
    anyway on its own merits (`b82b675`), because `g_async_why_hist` now carries that
    account with no I/O -- but **reasoning about a cost is not measuring it.**

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★ ...AND DOOM DOES **NOT** CARRY THE SAME HIDDEN REGRESSION. IT IMPROVED.   │
└──────────────────────────────────────────────────────────────────────────────┘
  The Skyroads fix does nothing for Doom -- it is gated on `!g_dpmi_pm` and Doom is a
  PM client, so it takes the branch it always took (TOTAL 6067 against a 6040-6090
  range across nine runs today). But having proved this CLASS of bug exists, the
  obvious question is whether Doom is silently carrying one too. It is not:
```
   session 21 (c740f4e)   async irq0  494 + coop irq0 4660 = 5154   82% of 140 Hz
   session 24 HEAD        async irq0 2485 + coop irq0 3582 = 6067   96% of 140 Hz
```
  **+17.7% of its timer ISR entries since session 21**, and the throttle that cost
  Skyroads a fifth of its clock is part of why -- e2f7486 was a real Doom fix. The
  work of sessions 22-23 moved Doom's clock forward; it only ever moved the wrong way
  for the guest nobody re-ran.
  ▶ **A STRUCTURAL FACT WORTH KEEPING.** Across all nine Doom runs today, `raises`
    spans 6432-6913 (±3.7%) while `TOTAL` spans 6040-6090 (**±0.4%**). Delivery is
    PINNED at ~135 Hz whatever the 8254 generated, so the "% of raises" figure moves
    only because its denominator wobbles. Quote TOTAL, not the percentage.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★ WHAT THE PLAYER REPORTED, VERBATIM -- TWO OF THE THREE ARE NEW DATA        │
└──────────────────────────────────────────────────────────────────────────────┘
```
   AUDIO   "the echo is still there (sounds roughly equivalent to stock NTVDM)"
           ▶ WE ARE AT PARITY WITH STOCK. That is a ceiling on what the remaining
             audio work is worth, and it should be weighed before spending more
             sessions on it. ⚠ NOT verified how the comparison was made -- stock
             ntvdm has only ever been measured as far as Doom's TITLE SCREEN.
   VIDEO   "the status bar is still pixelated, AND IMMEDIATE SCENE CHANGES
           (i.e. menu melt into FPS) ARE PIXELATED"
           ▶ ★★★ NEW, AND THE BEST VIDEO CLUE SINCE THE ORACLE. The melt/wipe is a
             screen-to-screen COPY, not a fresh render. So the rule may be: anything
             Doom COPIES within video memory collapses; anything it DRAWS from CPU
             memory is perfect (title screen 0-of-64000, the 3D view, all correct).
             That unifies the status bar with the melt and points at the copy path.
             ⚠ It does NOT simply reinstate the write-mode-1 latch story -- session
             23 killed that on the row evidence (see below) -- but "which copy path"
             is a sharper question than "which writer".
   TIMING  "once in FPS, it plays as I would expect on a period-correct DOS
           machine" -> INDEPENDENTLY CONFIRMS the 135 Hz / 91%-of-raises finding.
```

┌──────────────────────────────────────────────────────────────────────────────┐
│ ⚠⚠ REFUTED: THE TIMER DEFICIT. IT WAS AN ASYNC-ONLY COUNTER.                 │
└──────────────────────────────────────────────────────────────────────────────┘
  TASK 1 asked which clause refuses at 144 Hz. `g_async_why` held only the LAST
  refusal, so a run said "62 attempts, 56 delivered" and could not say what took
  the other six. Keyed per LINE now, with codes for two exits that previously left
  `why=0` -- **which is the code for SUCCESS**: `HOST_CS` (the CPU thread was in
  HOST code, not the client) and a failed `SetThreadContext`.
```
   async why irq00  total=2782  DELIVERED=2500  vIF_off=12  arm_quiet=2
                    HOST_CS=77  not_in_exec=154  observed=37
```
  **All three candidates the handoff named are dead.** `g_async_pm_active` (an
  injection still in flight): **ZERO**. `vdd_pic_can_deliver`: **ZERO**. The
  client's virtual-IF: 12 of 2782, **0.4%**. Ninety percent of attempts deliver.
  ▶ Then the reason the shortfall looked large: **`delivered` was HALF THE
    ACCOUNT.** It is `g_async_inj_line[0]` -- the asynchronous arm only -- and the
    client's INT 08h is entered by TWO mechanisms. The cooperative
    `dpmi_inject_pm_irq()` (the #2b latch, and the catch-up batch on the catcher's
    return) delivers MORE than the async arm does:
```
   isr08 delivery: raises=6612  async=2500  coop=3568  TOTAL=6068  (91% of raises)
                   = 135 Hz against the 140 Hz Doom programmed
```
  ▶ And **`owed_max` IS A HIGH-WATER MARK, NOT AN OCCUPANCY.** One stall anywhere in
    45 s pins it at the cap forever, so "owed_max = 0x40 = PM_TICK_OWED_MAX, the
    backlog is PERMANENTLY SATURATED" read a maximum as a steady state. Sampled at
    every sync:
```
   owed_depth_at_sync[0,1,2,3,4-7,8-15,16-31,32-63,64] = 2695,53,12,6,19,40,66,25,4
   92.5% of syncs see an EMPTY backlog.  4 syncs in 2914 ever see the cap.
```
  ⚠ **WHAT IS STILL TRUE FROM SESSION 23.** The lock contention is real and
    measured -- `wait_us=103,099` (the AUDIO thread, `host_audio_fill`),
    `hold_us=108,702` (`host_pit_sync`), `ui_gap_us=127,146`, 1.86M plane swaps a
    run. TASK 3 is still a good idea *for video and for the audio thread's own
    stalls*. It is not the timer's problem, because the timer does not have one.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★★ THE ECHO: A MARGIN RACE, AND DMX IS STEERING BY THE 8237 COUNT         │
└──────────────────────────────────────────────────────────────────────────────┘
  **1. A FIFTH OF THE 44% WAS SILENCE.** `blocks_replayed` counts blocks >=90%
  identical to the same ring offsets one lap earlier and its comment called that
  "DMX never refilled". That is a conclusion: it is equally what a CORRECT refill
  looks like whenever the guest writes the same bytes again, and for 8-bit PCM
  that is every stretch of silence. Session 23 flagged the risk in prose and left
  it uninstrumented. Classifying each block by its own dynamic range:
```
   blocks_checked=3662  REPLAYED=1619 (44%)  flat=743
   REPLAYED_LOUD=933  = 32% of NON-FLAT blocks     (stable across 4 runs)
```
  The defect is real -- ~930 blocks a run, ~21/s of audible content played twice
  186 ms apart -- and it is 32%, not 44%.

  **2. IT IS A MARGIN RACE, NOT A STALL.** A rate cannot tell "every third block is
  stale" from "fine for a second, then forty in a row", and those are different
  bugs. The run-length distribution:
```
   runs[1,2,3,4-7,8-15,16-31,32-63,64+] = 753,79,0,0,0,0,0,0    run_max=2
```
  **Never more than two or three consecutive blocks.** Nothing ever stops DMX for
  long -- which is independent evidence against the starved-timer story, because a
  starved timer would produce exactly the long runs that are absent.

  **3. DMX STEERS BY THE 8237's CURRENT COUNT, AND LOOKS LESS OFTEN THAN BLOCKS
  COMPLETE.** How does the guest decide what is safe to overwrite? Measured:
```
   ch1_addr=0   ch5_addr=0   status=0      <- it NEVER reads the current ADDRESS
   ch1_count=4962, ALL 8-BIT reads         <- so 2 reads per poll = 2481 polls
   2481 polls / 45 s = 55/s      against    82 blocks/s
   => 32% of blocks complete with no poll between them
      ...against 32% of audible blocks being lap repeats.  Four runs, both
         ratios agree to within a point every time.
```
  ⚠ The width was MEASURED, not assumed: a read count is not a poll count behind a
    lo/hi flip-flop, and the whole reading rests on the factor of two.
  ⚠ Two ratios agreeing is a correspondence, not a mechanism. Which is why:

  **4. ★★★ AN ASYNC TICK REACHES THAT POLL 13x MORE OFTEN THAN A COOPERATIVE ONE.**
  55 polls/s is also, to within 1.5% in two separate runs, the ASYNC arm's delivery
  rate -- while the cooperative arm delivers 79/s more on top. Bracketing the
  cooperative injection (the handler runs synchronously inside
  `dpmi_inject_pm_irq`, so a before/after snapshot of `rd_count[1]` is exact):
```
   ch1_count=4962 total     from_coop_isr08=484        (9.8%)
      per ASYNC tick        0.89 polls
      per COOPERATIVE tick  0.068 polls     ...for 59% of all delivered ticks
```
  **Both paths enter the same handler and both complete** -- `done=1`, `phases=4-5`,
  every one of 4478. What DMX does *inside* them differs. That is what is left of
  the echo, and it is the only thing left of it.

  **5. ★★★ AND IT IS THE TIMER'S HANDLER, NOT THE SOUND BLASTER'S, AND NOT MAINLINE.**
  Two more brackets closed both remaining ambiguities. IRQ5 -- the block completion,
  when a refill is actually DUE -- was the more natural suspect and is excluded
  outright; then `g_async_pm_active`, which is set for exactly the async ISR's
  duration, split the remaining 89%:
```
   from_coop_isr08=546   from_coop_irq05=0        <- ZERO, of 953 IRQ5 injections
   ch1_count=4996  in_async_isr=4450 (89%)  mainline=548 (11%)
                   ...and 546 of that 548 IS the cooperative ISR (it sets no flag)
   => Doom's MAIN LOOP polls the DMA controller essentially NEVER.

      per ASYNC tick        0.89 polls   (2493 ticks -> 4450 reads)
      per COOPERATIVE tick  0.076 polls  (3597 ticks ->  546 reads)
```
  So DMX's DMA polling lives in its **timer ISR**, and **59% of the ticks we deliver
  do essentially nothing for the audio.** The reframing came free: the IRQ5 bracket
  was two lines and it turned the question from "which line?" into "which path?".

  ⚠⚠ **WHAT THIS STILL DOES NOT ESTABLISH, AND DO NOT SKIP IT.** A POLL IS NOT A
    REFILL. The 55-polls/s vs 82-blocks/s arithmetic matches the 32% replay to a
    point across five runs -- but if DMX writes more than one block per poll, the
    two ratios agreeing is a COINCIDENCE and the replay has some other proximate
    cause. Two ratios agreeing is exactly the trap that the `from_coop_isr08`
    bracket was built to escape, and the same trap is still open one level down.
    **The instrument that closes it measures what the guest WRITES into the ring,
    not what it reads from the 8237.**

┌──────────────────────────────────────────────────────────────────────────────┐
│ ▶▶▶ RESUME HERE. THE NEXT ACTION, CONCRETELY.                                │
└──────────────────────────────────────────────────────────────────────────────┘
  **TASK A (do this FIRST -- it guards everything below it).** **A POLL IS NOT A
  REFILL.** Measure what the guest WRITES into the DMA ring, not what it reads from
  the 8237. Until that exists, "55 polls/s against 82 blocks/s explains the 32%
  replay" is two ratios agreeing, which is the exact trap the brackets above were
  built to escape. If DMX writes more than one block per poll, the correspondence is
  a coincidence and TASK B is chasing the wrong thing.
  ▶ The ring is guest memory, so a write is not trapped -- but the replay detector
    already keeps a full ring shadow (`lap_buf`). Diff the shadow against the ring at
    each block completion to get "bytes the guest changed since we last looked", per
    block. That is a rewrite of the existing loop, not a new subsystem.

  **TASK B (the echo's likely root).** Find what DMX does differently inside a
  cooperative INT 08h. Both paths run the same handler to its IRET (`done=1`,
  `phases=4-5`, all of them); only one leads to the DMA poll. Candidates, cheapest
  first:
```
   the ISR takes an EARLY EXIT     DMX's INT 08h chains/divides -- it may only mix
                                   on some entries, and the cooperative path may be
                                   landing on the ones that do not. phases=4-5 on
                                   EVERY cooperative entry is suspiciously uniform
                                   for a handler that sometimes mixes: instrument
                                   the guest EIP reached, not just the phase count.
   BATCHING                        the catch-up batch drains up to DPMI_IRQ0_BATCH
                                   ticks back to back with no guest time between
                                   them. A divider in DMX fires once per BURST, not
                                   once per tick, which would produce exactly this.
                                   k=1 981x, k=2 1069x -- so most bursts are 1-2.
   register/flag state at entry    dpmi_async_inject_pm builds the frame on the
                                   SUSPENDED THREAD'S OWN CONTEXT; dpmi_inject_pm_irq
                                   builds it from the VDM_TIB register file. If those
                                   disagree about anything DMX tests, that is it.
                                   DIFF THE TWO FRAME BUILDERS FIELD BY FIELD.
   g_in_pm_irq blocks something    set for the whole cooperative injection. Anything
                                   DMX's mixer needs that is refused while it is set
                                   would produce exactly this.
```
  **TASK C (if A and B land).** If cooperative ticks can be made to produce refills
  the way async ones do, polls go from 55/s to ~135/s against 82 blocks/s and the
  margin race has no margin left -- no lock work, no page traps, no new subsystem.
┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★★ THE STATUS BAR IS **TWO** FAULTS. SESSION 23's REFUTATION ASSUMED ONE. │
└──────────────────────────────────────────────────────────────────────────────┘
  Prompted by the player's melt observation (the fault follows COPIES; fresh renders
  are pixel-exact), the write-mode-1 path was re-opened. Three measurements:
```
   1 THE FAN-OUT IS INNOCENT -- properly, this time.
       fanout_bar writes=0/0  distinct=0/0  4way=0/0   over 44 fan-outs
       ...against bar_planes_equal 1709/2560 on every page
     ⚠ SESSION 22's EXONERATION OF IT WAS INVALID even though the answer was right:
       it DISABLED the fan-out and found the bar "still 58% wrong". With it off a
       multi-plane write reaches NO plane, so the bar is wrong because UNWRITTEN
       rather than wrong because COLLAPSED -- and a percentage of differing PIXELS
       scores those the same. Count the thing, not a proxy.
   2 THE LATCH COPY'S OUTPUT IS DISCARDED.  latch_solved=0, latch_UNSOLVED=120:
     the displacement solver has NEVER once explained a burst, so the per-plane
     correction never runs -- and per (1) the bytes are never fanned out either.
     ~10,014 bytes a run are computed by the guest and thrown away.
   3 SESSION 23 MISREAD ITS OWN FIELD. "120 bursts, 116 changed bytes": 116 is the
     number of BURSTS THAT CHANGED SOMETHING. The byte total is ~10,014
     (45x64, 41x128, 25x62, 4x59, 1x100, 4 empty). `changed=` is PER BURST.
```
  ▶ **AND THE ROW CLAIM SURVIVES, WHICH IS WHAT MAKES THIS A DECOMPOSITION.** Across
    all 120 bursts: `184..199` x111, `185..199` x4, `184..197` x1, empty x4. So no
    burst reaches rows 168-183 -- the half session 23 measured as WORSE (62.2% vs
    57.6%). Both facts are true. Session 23's refutation only follows if the bar has
    ONE cause, and it does not:
```
     rows 184-199   latch-copy destination data, computed and then DISCARDED
     rows 168-183   a SECOND fault. No burst touches it. The fan-out does not
                    touch it. It is the worse half, and its writer is unnamed.
```
  ▶ **AND THE DISCARD WAS LOCATED, IN ONE LINE.** `modey_remap_wmode()` ends every
    burst with `for (k...) g_yseed[k] = sc[k];` while the fan-out decides what to
    propagate with `if (sc[k] == g_yseed[k]) continue;` -- so the re-seed erases the
    fan-out's input before it can run. That is the whole of `fanout_bar distinct=0`.
  ⚠⚠ **DELIVERING THEM WAS TRIED AND DOES NOT FIX THE BAR. DO NOT RE-APPLY IT.**
    Propagating the scratch byte to the selected planes works -- band B goes 0 -> 7352
    writes over 192 distinct offsets -- and the WAD oracle is flat:
```
     plane 0  34.4% -> 34.9%        plane 2  29.8% -> 29.8%
     plane 1  70.7% -> 70.2%        plane 3  27.8% -> 27.6%
```
    Under a point either way, and plane 1 -- the one plane that is mostly CORRECT --
    gets WORSE, because the fan-out smears plane 0 across it. **Those 192 offsets were
    no more wrong before than after, so the bytes we discard are not what is wrong with
    the bar.** Session 23's refutation stands, on better grounds than it gave.
  ▶ **NEXT, AND BOTH ARE SMALLER THAN "WHAT IS WRONG WITH THE STATUS BAR".**
    (a) rows 184-199: needs the TRUE per-plane latches. The scratch holds one byte per
        offset and cannot represent four, which is why the solver is 0-for-120 and why
        every inference scheme over it has failed.
        ⚠⚠ **THE PAGE TRAP CANNOT PROVIDE THEM -- AND THIS IS ALREADY MEASURED.** The
        note at `a000_protect` records that arming the A0000 trap FREEZES THE GUEST on
        this box, twice: `io_events` 10 against 22,532,292, with `PAGE_READONLY`
        behaving identically. Session 22 planned this subsystem; session 23 cancelled
        it for the wrong reason. **The right reason is that it does not work here.**
        Mode 12h's workaround -- run the guest in the HOST INTERPRETER while the mode
        is current -- is the only remaining candidate, scoped to the 120 short bursts.
        ⚠ Doom is a 32-bit DPMI client on the real CPU; `v86interp.h` is the V86/16-bit
        engine, so this is not a small change and its feasibility is UNASSESSED.
    (b) rows 168-183: name the writer, and this is now the BETTER-VALUE half -- it is
        the worse band, no burst reaches it, and the fan-out is excluded by count
        (`writes=0` in band A across every run). Only guest stores under a SINGLE-plane
        mask remain.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★ ...AND THE TWO BANDS ARE ONE SIGNATURE AT TWO INTENSITIES               │
└──────────────────────────────────────────────────────────────────────────────┘
  `tools/doomoracle/bandprof.py` (new, runs off the MODEYBAR dump already in every
  log -- no rig run). Session 23's plane-vs-phase matrix was built over the WHOLE bar;
  split by band it says something the union could not:
```
   band A rows168-183 (NO bursts)        band B rows184-199 (all bursts)
         q=0   q=1   q=2   q=3                 q=0   q=1   q=2   q=3
   pl0  34.9  47.1  17.0  16.2            pl0  33.8  74.4  23.8  19.4
   pl1  19.3  61.2  17.8  15.9            pl1  28.2  80.2  23.5  19.5
   pl2  18.4  46.9  30.2  16.5            pl2  27.9  73.7  29.4  21.1
   pl3  19.1  45.9  17.7  30.2            pl3  27.5  73.0  24.6  25.3
   four-way uniform 53.8% (ref 11.8%)     four-way uniform 79.8% (ref 13.4%)
```
  **Both bands collapse toward phase 1, band B roughly twice as hard** -- 79.8%
  uniform against 53.8%, and a phase-1 margin over each plane's own phase of 41-48
  points against 12-17. Band B is close to "plane 1 replicated into all four"; band A
  is a weaker version of the SAME thing.
  ▶ So this is **one mechanism whose exposure differs by band**, not the two unrelated
    causes the decomposition above first suggested. Correct that reading; keep the
    band split, because it is what made the difference visible.
  ▶ **WHAT IS LEFT, BY ELIMINATION.** The fan-out is excluded by direct count (0 bar
    bytes in band A, every run). The latch bursts are excluded by experiment
    (delivering them moves the oracle under a point). The render is excluded because
    plane-vs-WAD matches screen-vs-WAD to the digit. **That leaves the GUEST's own
    stores under single-plane masks** -- so the question is why Doom's per-plane
    stores would deposit PHASE-1 data into all four planes, and specifically
    **whether the mask a store lands under is the mask Doom believes it set.**
    ▶ **THAT PROBE IS BUILT AND RUN** (`ysmp_check`, `STAGE2: ... ysmpA/ysmpB`). It
      samples the OUTGOING plane before each swap and counts only windows that actually
      CHANGED, comparing each against the last changed window from a DIFFERENT plane:
```
       band A rows168-183   writes=243  cross_same=30  cross_diff=212   12% same
       band B rows184-199   writes=67   cross_same=32  cross_diff=34    48% same
```
      The ordering matches bandprof's collapse intensity (54% / 80% uniform), which is
      suggestive -- but the planes mostly receive DIFFERENT bytes, and 12/48 is well
      under 54/80, so **the uniformity is not produced by the writes we can see.**
      ⚠ WEAK: n is 243 and 67 for a whole run, because those rows rarely change. It
        narrows; it does not settle.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★★ THE CONSTRAINT THAT SHOULD HAVE BEEN DRIVING THIS ALL ALONG           │
└──────────────────────────────────────────────────────────────────────────────┘
  **The TITLE SCREEN is pixel-exact (0 of 64000) and the 3D view renders correctly,
  and BOTH go through the SAME unchained per-plane blit** -- same mask sequence, same
  remap, chain4=0, mapmask 0x01/0x02/0x04/0x08 at ~510k each. So Doom's per-plane
  stores and our mask-to-plane mapping are **SOUND for content that is fully redrawn.**
  The status bar is the one thing that is NOT fully redrawn: Doom updates only what
  changes. That is exactly why session 22 watched the 3D view "heal itself" while the
  bar stayed wrong and FLAT FROM THE FIRST FRAME.
  ▶ **SO THE BAR'S WRONG BYTES ARE WRITTEN ONCE, WRONGLY, AND NEVER REWRITTEN.** Stop
    hunting a writer that corrupts plane data during play -- every such hunt (fan-out,
    latch bursts, render, guest stores) has now come back excluded or inconclusive,
    which is what you would expect if nothing is corrupting anything during play.
    Ask instead: **what did the planes contain when the bar was first drawn, and why
    did that draw not cover every offset?**
  ▶ **A CONCRETE CANDIDATE, AND A DECISIVE ONE-RUN TEST FOR IT.** `modey_remap_init()`
    sets `g_ycur = 4`, and `ymap_select(-1)` on a chain4 change selects 4 as well -- so
    A0000 maps `g_ysec[4]`, **the LINEAR section, not any plane**, both before the first
    map-mask write and for as long as the guest stays chained. Anything the guest writes
    to A0000 in either window lands there and is INVISIBLE TO ALL FOUR PLANES. That is
    exactly the shape the evidence now demands: content written once, never rewritten,
    and not corrupted by anything during play.
    ▶ **THE TEST (≈10 lines, one run, and it cannot come back ambiguous).** At wind-down,
      dump `g_ysec[4]`'s BAR REGION (rows 168-199) the same way MODEYBAR dumps the
      planes, and score it against STBAR with `planejudge`/`bandprof`.
```
       if the LINEAR section's bar region scores WELL against STBAR
           -> Doom drew the bar while A0000 pointed at linear; the planes never
              received it, and the fix is about WHEN the window is repointed,
              not about how any write is handled. SMOKING GUN.
       if it is empty or scores at chance
           -> the candidate is dead, and the write-once damage happened somewhere
              else. Either way the answer is unambiguous, which is what the last
              four hypotheses were not.
```
    ⚠ Hold it loosely: several hypotheses fell over today (log spam under the lock, the
      fan-out delivery repair, the page trap). This one is structural rather than
      inferred, which is why it is worth a run.
    ▶▶ **RUN. IT IS DEAD.** `linear_bar_nonzero = 0 / 10240` -- not one byte of the bar
       region was ever written through that window. Exactly the unambiguous outcome it
       was designed for, and it cost one run.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★★ THE COLLAPSE **IS** PLANE 1 REPLICATED. SESSION 23'S CONTROL WAS VOID. │
└──────────────────────────────────────────────────────────────────────────────┘
  At offsets where all four planes hold the same byte, WHICH reference phase is that
  byte? (`bandprof.py`, free, off the dump already in every log.)
```
   band A 168-183   688 uniform offsets   q0 27.3%  q1 73.3%  q2 21.9%  q3 20.2%
   band B 184-199  1021 uniform offsets   q0 32.3%  q1 88.6%  q2 26.7%  q3 22.0%
```
  **The common value is STBAR's PHASE 1 pixel.** Planes 0, 2 and 3 are holding PLANE
  1'S CORRECT DATA. That is "plane 1 smeared outward" -- the reading session 23 ruled
  out with a CONTROL (p0-vs-p2 agreeing at 73%, "neither being the suspected source").
  ⚠⚠ **THE CONTROL WAS INVALID.** If plane 1's bytes are copied into 0, 2 AND 3 then
    p0-vs-p2 MUST agree -- both hold plane 1. Its outcome is exactly what the hypothesis
    PREDICTS, so it had no power to refute it. **A control has to be something the
    hypothesis forbids; this one forbade nothing.** Session 23 also described the common
    value as "the phase-1 pixel more often than the others" -- 88.6% against 22-32% is
    not "more often", it is near-exclusive, and the qualitative phrasing is most of why
    the right answer was discarded.
  ▶ **THE QUESTION IS NARROW AT LAST: what puts plane 1's bytes into planes 0, 2 and 3?**
    No code path in this host copies one plane to another. The seed writes the SCRATCH
    (from `sel[0]`, which is plane 0 for every multi-plane mask observed); the fan-out
    writes planes from the scratch and is measured at 0 bar bytes; the latch `dl` path
    copies WITHIN one plane and has never run. Which leaves the GUEST writing plane 1's
    data four times -- either its blit source not advancing per plane, or three of its
    four mask changes not moving our window.
    ▶ **THE SECOND IS DIRECTLY CHECKABLE AND SHOULD BE DONE FIRST.** Count
      `ymap_select` calls per mask VALUE against the map-mask write histogram
      (0x01/0x02/0x04/0x08 at ~510k each) and look for the window sitting on plane 1
      across passes that should have moved it. `vdd_video.c:941` only remaps when
      `(v & 0x0F) != st->y_mask`, so any path that updates `y_mask` WITHOUT calling
      `ymap_select` would strand the window on whichever plane was last mapped --
      and `st->y_mask = st->map_mask` is assigned at line 947 and again at 952.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ⚠⚠⚠ THE IMPASSE (now largely resolved above) -- AND A CIRCULAR EXCLUSION     │
└──────────────────────────────────────────────────────────────────────────────┘
  **The planes are FULL, and the collapse is real content, not emptiness:**
```
   plane zero-bytes   0.0% / 0.2% / 0.1% / 0.1%      (band A; same in band B)
   four-way uniform   ALL-ZERO 0.0%    all-equal-NON-ZERO  53.8% (A)  79.8% (B)
```
  So something writes the SAME NON-ZERO byte to all four planes at most bar offsets --
  and **every candidate writer is now excluded by measurement**: the fan-out (0 bar
  bytes, counted in its own loop), the latch bursts (delivering them moves the oracle
  under a point), the linear window (0 bytes), the guest's single-plane stores (planes
  receive mostly DISTINCT bytes).
  ▶ **WHEN EVERY WRITER IS EXCLUDED AND THE CONTENT IS STILL THERE, ONE EXCLUSION IS
    WRONG.** Distrust the one measured most indirectly.
  ⚠⚠ **THAT IS THE RENDER, AND ITS EXCLUSION IS VERGING ON CIRCULAR.** Session 23 ruled
    it innocent because plane-vs-WAD (69.4/33.5/29.0/27.1) matches screen-vs-WAD
    (70.3/33.6/29.3/27.3) "to the digit". But MODEYBAR dumps `g_yview[pl]` and the
    render reads `ymap_plane(p) = g_yview[p & 3]` -- **THE SAME MEMORY**. Those two
    agreeing is very nearly a tautology. It shows the render adds no error of its own;
    it says NOTHING about a cause upstream of both, and it cannot exclude anything that
    makes the PLANES wrong -- which is exactly what is being hunted.
  ▶ **THE NEXT MEASUREMENT MUST NOT SHARE A SOURCE WITH THE THING IT CHECKS.** Two
    candidates, neither run:
```
     (a) plane bytes vs the host's own SCREENSHOT (shotNN.bmp via capture.flag),
         scored with doomref.py -- two different paths out of the same store, so a
         disagreement localises the fault to one of them.
     (b) what the GUEST believes it wrote: Doom's screens[0] lives in its own memory
         and IS readable. Compare the bar region of screens[0] against STBAR. If the
         guest's own buffer is already collapsed, nothing in this host is at fault and
         the search has been in the wrong process all along.
```
    ▶ (b) is the stronger of the two and has never been attempted. It is the only check
      that can distinguish "we corrupt Doom's data" from "Doom hands us collapsed data
      because of something we did upstream of the blit".

  **TASK D (video -- and the player just narrowed it).** Name the writer that puts
  phase-1 data into all four planes. Session 23's status-bar section below is
  unchanged and still correct, but the new observation that **the menu-to-FPS MELT is
  also pixelated** says the fault follows COPIES, not the status bar specifically:
  the melt is a screen-to-screen copy, the title screen and 3D view are fresh renders
  and both are perfect. ▶ Start by finding which copy path the melt uses and whether
  it is the same one the bar uses -- that is a much smaller search than "name the
  writer", and one run with the oracle against a melt frame would confirm the rule.
  **TASK E (still worth doing, on its own merits).** `g_lock` contention: 103 ms
  waits with the AUDIO thread the longest waiter, 108 ms holds, 1.86M plane swaps.
  It will help video and the audio thread's own stalls. It will NOT fix the echo.
  ⚠ Do not spend a run on the async injector's refusal rate. It is 90% efficient and
    the three suspected clauses are at ZERO, ZERO and 0.4%.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★ NEW INSTRUMENTS (session 24). All bounded; all on by default.           │
└──────────────────────────────────────────────────────────────────────────────┘
```
   STAGE2: async why irqNN   the refusal histogram, PER LINE, named not numbered.
                             Codes 0-14 from dpmi_async_inject_pm + the two new
                             ones; 20-27 from async_early_bail. Sums to `total`.
   STAGE2: isr08 delivery    raises / async / coop / TOTAL / % of raises /
                             owed_now / owed_depth_at_sync[9]. The whole account,
                             in the units of the claim. USE THIS, not pit budget's
                             `delivered`, which is the async arm alone.
   STAGE2: coop per IRQ      cooperative dpmi_inject_pm_irq injections by vector.
   STAGE2: sb replay ...     now also flat= / REPLAYED_LOUD= / % of NON-FLAT /
                             runs[8] / run_max. REPLAYED alone cannot support a
                             claim -- silence replays correctly.
   STAGE2: 8237 guest reads  ch1/ch5 addr+count, status, count reads BY WIDTH, and
                             from_coop_isr08. This is what found the poll rate.
   PMIRQ vec=0x..            was "IRQ0<-PM" for every vector it injected, including
                             the device lines it has served since 7a13b45.
   from_coop_isr08 /         DMA count-register reads attributed to the injection
   from_coop_irqNN /         path that caused them. The cooperative ones are
   in_async_isr / mainline   bracketed exactly (the handler runs synchronously
                             inside dpmi_inject_pm_irq); the async ones are
                             identified by g_async_pm_active, which is set for
                             precisely that ISR's duration. This is what proved
                             the asymmetry is PATH, not LINE.
```

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★★ METHOD — SESSION 24 PAID FOR THESE                                     │
└──────────────────────────────────────────────────────────────────────────────┘
  ▶ **A COUNTER PRINTED BESIDE ANOTHER IS A CLAIM THAT THEY ARE COMPARABLE.**
    `raises=144/s ... delivered=56/s` invited "we deliver 39% of the ticks" -- but
    `delivered` was one of TWO delivery paths and the other was larger. Nothing was
    wrong with either number. The LAYOUT made the false statement, and a whole
    session's root cause was built on it. **Before comparing two counters, ask what
    each one does NOT count.**
  ▶ **A MAXIMUM IS NOT AN OCCUPANCY.** `owed_max` saturates permanently on one stall,
    so "PERMANENTLY SATURATED" and "touched the cap once in 45 s" are the same
    number. They mean opposite things. Sample the distribution, not the extreme.
  ▶ **WHY=0 MEANT "SUCCESS" AND ALSO "NOBODY SET IT".** Two exits in the async
    injector returned failure with the reason field untouched, so a histogram keyed
    on it would have booked them as deliveries -- and one of those two (`HOST_CS`)
    was the third-largest bucket. **When you turn a last-value field into a
    histogram, audit every path that reaches the field, not just the ones that set
    it.**
  ▶ **A METRIC WHOSE NAME IS A CONCLUSION WILL BE READ AS ONE.** `blocks_replayed`
    measured "identical to one lap earlier" and its comment said "DMX never
    refilled". Those differ by every silent block, which was a fifth of the total.
  ▶ **A RATE CANNOT SHOW A SHAPE.** 32% replayed is the same number for a margin
    race and for a half-second stall. The run-length histogram cost one counter and
    excluded an entire family of causes -- including the one the previous session
    had settled on.
  ▶ **A READ COUNT IS NOT A POLL COUNT.** The 8237's count is 16 bits behind an
    8-bit port with a flip-flop, so the poll rate depends on a factor of two that
    depends on the guest's operand width. Measuring it cost one run and one counter;
    assuming it would have put a 2x error under the session's central number.
  ▶ **TWO RATIOS AGREEING IS NOT A MECHANISM.** 55 polls/s vs 56 async ticks/s
    matched to 1.5% in two runs, which is suggestive and proves nothing. Bracketing
    the cooperative injection turned it into 0.89 vs 0.068 polls per tick -- a
    direct measurement of the thing itself, for two lines of code.
  ▶ **FOLLOWING A REFUTATION IS THE WORK.** TASK 1 was meant to choose between three
    fixes. It eliminated all three, and the value of the run was entirely in what it
    ruled out. Two of the three tasks below it were then cancelled without being
    attempted, which is the cheapest possible outcome for both.
```
