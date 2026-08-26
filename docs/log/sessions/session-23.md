# Session 23 — 2026-08-24

> Archived verbatim from `return-ntvdm.md`, which was the single rolling handoff
> file until 2026-08-26. Split by session so it can be read by topic and date
> rather than by scrolling. **Nothing has been edited** — including conclusions
> that later sessions REFUTED, which are kept on purpose: the refutations are
> some of the most valuable material here.

```
═══════════════════════════════════════════════════════════════════════════════
██ ▶▶▶ SESSION 23 (2026-08-24). **BOTH REMAINING DEFECTS RE-DIAGNOSED.**      ██
██     Session 22's cause for EACH was wrong.                                 ██
██  ⚠⚠ ITS AUDIO/TIMER CHAIN IS REFUTED BY SESSION 24 ABOVE. The status-bar   ██
██     section stands; "the timer starves the audio" does NOT. Read the       ██
██     numbers below as HISTORY -- `delivered` there is the async arm alone.  ██
═══════════════════════════════════════════════════════════════════════════════

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★ START HERE — THE 60-SECOND VERSION (⚠ superseded in part; see session 24)  │
└──────────────────────────────────────────────────────────────────────────────┘
  Doom is still playable; nothing regressed. Session 23 did **no feature work**: it
  re-diagnosed both remaining defects, and **session 22 was wrong about each**.
```
   AUDIO   was "incoherent glitching", is now "on par with stock ntvdm, more like
           an ECHO than a glitch" (user's words, after 7a13b45). One cause FIXED
           (device IRQs had no cooperative PM path: delivery 73.5% -> 99.5%).
           The residual echo is MEASURED as a 185.8 ms ring-lap replay, and its
           cause is the TIMER: Doom asks 144 Hz, we deliver 56 Hz, and DMX mixes
           PCM in the timer ISR -- so missing ticks ARE missing refills.
   VIDEO   the status bar is UNCHANGED and still ~60% wrong. But the planned fix
           is CANCELLED: the latch copy is not the cause. New signature measured.
   ROOT    the timer dies on g_lock, and Doom's mode-Y drawing does ~43,000 port
           writes a second through that same lock. THE VIDEO PATH STARVES THE
           TIMER, AND THE TIMER STARVES THE AUDIO. One chain, both defects.
```
  ▶ **THE SINGLE NEXT ACTION** is at the bottom of this block: instrument WHICH
    refusal `async_inject_irq` returns at 144 Hz. One run; it decides between two
    quite different fixes and neither of them is anything tried so far.

  Branch `m9/completeness`, tree CLEAN (11 untracked files, all pre-existing from
  session 22: `MAINICON.ico`, `demos/`, `doom-screenshots/`, `tools/dostest/pm*.com`).
  Gates green on the shipped binary: off-VM **630 checks / 18 suites, 0 failed**,
  check-imports pass. **Five commits, `7919416`..`ceb178c`:**
```
   7919416  doom: both remaining defects re-diagnosed -- session 22 was wrong on each
   e0ca881  video: describe EVERY latch burst -- the refutation was right by luck
   7a13b45  audio: give the device lines a cooperative PM path -- 73.5% -> 99.5%
   637e2b8  audio: the echo is the TIMER deficit -- live replay counter proves it
   ceb178c  timer: find where 144 Hz becomes 56 Hz, and rule out the obvious fix
```
  Rig `192.168.1.29` is UP, share mounted at `/tmp/xpshare`, current build deployed
  (`bm/ntvdmhost.exe`, md5-verified), **all knobs cleared, `headless_ms.txt`=45000**.
  Archived run logs are in `build/rigruns/` (gitignored, local only).

┌──────────────────────────────────────────────────────────────────────────────┐
│ ⚠⚠ REFUTED: THE STATUS BAR IS **NOT** THE WRITE-MODE-1 LATCH COPY            │
└──────────────────────────────────────────────────────────────────────────────┘
  Session 22 planned an A0000 page-trap subsystem (~614k faults/run) on the strength
  of the latch-copy story. **DO NOT BUILD IT.** Every burst was described (the bound
  was raised from 6 to 4096) and the row range printed next to the offsets:
```
     120 bursts, 116 changed bytes, and ALL 116 land in rows 184..199
        rows 184..199 : 111 bursts     rows 185..199 : 4     rows 184..197 : 1
     rows 168..183  NO burst ever reaches them   3184/5120 = 62.2% wrong
     rows 184..199  ALL 116 bursts land here     2949/5120 = 57.6% wrong
```
  The half of the bar the latch copy cannot explain is **worse** than the half it
  touches. (`tools/doomoracle/barprof.py`.)
  ⚠⚠ **THE FIRST VERSION OF THIS REFUTATION WAS UNSOUND AND HAPPENED TO BE RIGHT.**
    It generalised "bursts only touch rows 186..199" from the SIX descriptions the
    instrument was bounded to -- of which only TWO had changed bytes, both at the same
    span -- out of 160 bursts. **A BOUND ON AN INSTRUMENT IS A CLAIM ABOUT WHAT IS
    REPRESENTATIVE.** Raising it changed the answer (184, not 186) and only then made
    the conclusion evidence rather than luck.
  ⚠ Also recoverable from session 22's own log: the burst spans were printed and never
    converted into rows. **A number in a log is not a measurement until it has been put
    in the same units as the thing it is meant to explain.** The burst line now prints
    `rows=` and `barbytes=` beside the hex span.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★★ THE STATUS BAR: A FOUR-WAY PLANE COLLAPSE (phase 1 survives most)      │
└──────────────────────────────────────────────────────────────────────────────┘
  New instrument `MODEYBAR` dumps all 4 planes x 3 pages x rows 168..199 at wind-down;
  `tools/doomoracle/planejudge.py` judges them against STBAR. Plane p's byte i is
  pixel x = 4i + p, so each plane can be scored against every reference PHASE:
```
             q=0     q=1     q=2     q=3        <- reference phase
     pl0    33.5%   60.7%   20.2%   17.5%
     pl1    23.6%   69.4%   20.5%   17.6%
     pl2    22.8%   60.4%   29.0%   18.8%
     pl3    22.9%   59.5%   21.2%   27.1%
     best-with-shift: EVERY plane peaks at q=1, shift k=0
```
  **All four planes contain plane 1's column set.** Each plane matches phase 1 roughly
  twice as well as it matches its own. Established alongside it:
```
   the render is INNOCENT     plane-vs-WAD (69.4/33.5/29.0/27.1) matches
                              screen-vs-WAD (70.3/33.6/29.3/27.3) to the digit
   a FOUR-WAY collapse, NOT   every plane pair agrees at ~75%: p0-vs-p1 75.5%,
   "plane 1 smeared outward"  p0-vs-p2 73.2% (the CONTROL, neither being the suspected
                              source). So one value reaches all four planes at ~3/4 of
                              bar offsets, and that value happens to be the phase-1
                              pixel more often than the others. ⚠ the "plane 1 is
                              copied into the rest" reading is WRONG -- the control
                              kills it. A four-way collapse under mask 0x0f is what
                              write mode 1 looks like when one value serves all four.
   no plane is UNWRITTEN      0.0% of any plane's bar region still holds its seed
                              marker, so this is not "three planes never drawn"
   all THREE pages identical  every figure equal to the digit across pg0/1/2
   the seed hypothesis is DEAD mapmask hist is 0x01/0x02/0x04/0x08/0x0f and nothing
                              else, so sel[0] for a multi-plane mask is ALWAYS plane 0
                              -- the scratch seed cannot produce a plane-1 bias
```
  ▶ **NEXT: NAME THE WRITER.** Something deposits mask-0x02-phase data into all four
    planes. The fan-out is excluded (it would smear the scratch, seeded from plane 0 =
    phase 0), the latch path writes nothing at all (`latch_solved=0`), and the render
    is excluded above. Instrument WHICH MASK WAS LIVE when each bar offset last
    changed -- a per-plane shadow of the 2560-byte bar region, diffed on mask change.
    2M swaps x 2560 bytes is too much to diff every time; sample, or diff only the
    first change of each offset.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★★ THE PCM CLICK: dropped IRQs FIXED; residual echo is the TIMER          │
└──────────────────────────────────────────────────────────────────────────────┘
  New instrument: the **block ledger** (`sb_blkrec` in vdd_sb.h, `STAGE2: sbblk` lines)
  records cap_off / block_len / 8237 state at each of the first 24 block completions.
```
     sbblk 00 cap_off=0x004 blk_len=0x004 mode=09   <- single-cycle prime
     sbblk 01 cap_off=0x00f blk_len=0x00b mode=09   <- single-cycle prime
     sbblk 02 cap_off=0x10f blk_len=0x100 mode=19   <- auto-init ring starts
     sbblk 11 ... WRAPPED                            <- ring = 0x1000 = 16 blocks
```
  ⚠ **`sbref.py`'s GRID WAS 15 BYTES OUT OF PHASE.** DMX primes the DSP with two
    single-cycle transfers (4 and 11 bytes) before the ring starts, and both land in
    the capture -- so real boundaries are at **15 + n*256**, not n*256. 11 is ODD, so
    the stereo frame parity flips there too. Session 22's "offset 2 of every block"
    was measured against an inferred grid, not a real one. **An instrument that infers
    its own reference frame will confirm whatever phase it guessed.**
  **THE ACTUAL FAULT** (`tools/doomoracle/blockphase.py`): the discontinuity sits at
  block offset 245/246 (13.0x mean) -- 11 bytes BEFORE each block ends, not at the
  boundary. Dumping the bytes there shows the pre-seam data is a **VERBATIM REPEAT OF
  THE PREVIOUS RING LAP** (two seams 4096 bytes apart share their preceding 10 bytes
  exactly; 96/182 seams are preceded by a full 256-byte repeat). We are playing ring
  content the guest never refilled.
  **WHY:** `sb_blocks=0x0dee` (3566) against `irq05=0x0a95` (2709). **857 block-
  completion IRQs -- 24% -- never reached the guest.** No IRQ, no DMX refill, so that
  block replays the previous lap. At 86 blocks/s that is the buzz.
  **▶ FIXED (partially) -- THE DEVICE LINES HAD NO COOPERATIVE PATH.** Every early bail
  was ONE reason: `1009 ASYNC-EARLY bail why=0x14` = `g_in_exec == 0`, i.e. the SB raised
  from the audio thread while the CPU thread was inside the host. A device IRQ got
  exactly ONE attempt (the synchronous `async_inject_irq()` in `host_irq_sink`), and the
  retry loop that exists for device lines needs a `tib` from a TRAPPING V86 guest -- which
  a 32-bit DPMI client never is. So the interrupt was simply lost.
  This is **the keyboard bug of session 22, one line number over**: "a pending interrupt
  is not a moment, it is a STATE". Same fix, same place in the PM loop, same
  claim-before-running rule. Measured:
```
                      blocks   async irq05   coop retry   delivered   shortfall
     before             3558          2615            -    73.5%          943
     after              3680          2809          852    99.5%           19
```
  ⚠ **IT DID NOT ELIMINATE THE REPLAY, ONLY REDUCED IT.** With delivery essentially
  complete the capture still replays the previous ring lap:
```
     total jumps                3617 -> 2621
     seams preceded by a >=64-byte verbatim lap repeat   53% -> 48%
     whole capture identical to one lap earlier          59% -> 46%
```
  ⚠ the last figure is CONTENT-SENSITIVE (silence trivially repeats) and the two runs are
    different 45 s of attract demo, so treat it as indicative, not as a score.
  **★ THE RESIDUAL IS AN ECHO, AND THE USER NAMED IT BEFORE IT WAS MEASURED.** "They sound
  more like they have an echo than an all-out glitch" -- and the ring lap IS an echo delay:
```
     repeat lag: % of bytes identical to the byte N earlier
        lag 2048 ( 92.9 ms) 21.7%    lag 4095 (185.7 ms) 24.7%
        lag 4096 (185.8 ms) 46.0%  <- ONE RING LAP     lag 4097 24.7%
```
  A spike that collapses ONE BYTE either side is a literal repeat, not a correlation. The
  transition from "incoherent" to "echo" is exactly the transition from LOST interrupts
  (data never written) to LATE refills (real audio, played twice).

  **★★ THE BINDING CONSTRAINT IS THE TIMER TICK DEFICIT, NOT THE SB IRQ OR THE LEAD.**
  New live counter `STAGE2: sb replay:` scores every block against the same ring offsets
  one lap earlier (>=90% identical = DMX never refilled it). It agrees with the offline
  capture to within 0.4 points (46.4% vs 46.0%), so the analysis script is no longer
  needed to see this. `awbufs.txt` makes the audio LEAD a controlled variable:
```
     lead   sb_blocks/45s   blocks/s   replayed        underruns
       6            3657         81    1604/3641 = 44%      0
       2            1690         38     549/1674 = 33%      0
```
  ⚠ **LEAD 2 IS NOT AN IMPROVEMENT -- IT STARVES THE PUMP.** 38 blocks/s against the 86/s
    the sample rate demands is audio at under half speed. `underruns=0` throughout, so
    that counter does NOT detect this; it only counts waveOutWrite failures. The replay
    rate fell because we consumed the ring more slowly, not because the race was fixed.
  But that is what makes the experiment decisive. Refills happen in DMX's TIMER ISR, so
  ticks ARE refill opportunities, and both rows are explained by one ratio:
```
     Doom asks for 140 Hz (pit_reload 8522). WE DELIVER 55 Hz = 39%.
       lead 6:  81 blocks/s needed vs 55 refills/s available -> 44% replayed
       lead 2:  38 blocks/s needed vs 56 refills/s available -> 33% replayed
```
  **★★★ WHERE THE TIMER GOES FROM 144 Hz TO 56 Hz** (`STAGE2: pit budget:`, new):
```
     raises     6483/45s = 144/s   the 8254 generates EVERY tick Doom asked for
     syncs      2921/45s =  65/s   host_pit_sync() runs this often -- THE CEILING
     attempts   2791/45s =  62/s   one async attempt per sync, by design
     delivered  2528/45s =  56/s
     owed_max   0x40 = PM_TICK_OWED_MAX -- the backlog is PERMANENTLY SATURATED
     ui_gap_us  24415             the UI thread targets 5 ms and misses by 5x
```
  The ticks are generated and then die at `host_pit_sync`, which takes `g_lock` -- and
  Doom's mode-Y drawing does **~43,000 port writes a second** through that same lock.
  **The video path is starving the timer, and the timer is starving the audio.**

  ⚠⚠ **RAISING THE ATTEMPT BUDGET WAS TRIED AND IS WRONG. DO NOT REPEAT IT.** Allowing up
  to 4 attempts per sync while the backlog is deep, stopping at the first refusal:
```
                     attempts/s   delivered/s   ui_gap_us   lock hold
     one per sync            62            56      24,415      ~15 ms
     up to four             189            55     260,009      188 ms
```
  **Attempts TRIPLED and delivery did not move.** The ceiling was never the attempt
  budget: it is how often the guest is in an INJECTABLE STATE. Extra attempts pay full
  SuspendThread round trips under the lock to be told no. Reverted; the reasoning is
  preserved in the comment at `host_irq_sink`.
  ▶ **NEXT, IN PRIORITY ORDER.**
    1. **Instrument WHICH refusal fires** at 144 Hz -- `g_async_pm_active` (an injection
       still in flight), `vdd_pic_can_deliver`, or the client's virtual-IF. One of those
       is the real ceiling and none of them is the attempt count.
    2. **Consider the COOPERATIVE path instead of the asynchronous one** for the timer:
       it needs no SuspendThread at all, and the PM loop is entered constantly.
    3. **Attack the lock contention itself** -- 43k mode-Y port writes a second through
       `g_lock` is the root of the starvation, and it would speed up video too.
    ⚠ Still do not raise `DPMI_IRQ0_BATCH`: session 22 measured that draining 64 ticks
      back to back compressed 0.45 s of game time into microseconds.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ▶▶▶ RESUME HERE. THE NEXT ACTION, CONCRETELY.                                │
└──────────────────────────────────────────────────────────────────────────────┘
  **TASK 1 (one rig run, decides everything else).** `async_inject_irq()` already sets
  `g_async_why` on every refusal and `async_early_bail()` gives the early exits codes
  20-24. What is missing is a **HISTOGRAM PER LINE**: `g_async_why_hist[irq][why]`,
  printed in STAGE2. Today a run says "62 attempts, 56 delivered" and cannot say which
  clause consumed the other six, nor -- more importantly -- what the refusal profile
  looks like when the sync rate itself is the binding constraint.
  Then read it against these three, because they need OPPOSITE fixes:
```
     why=9 / g_async_pm_active   an injection is still in flight -> the guest's ISR is
                                 slow to IRET, and MORE attempts can never help
     vdd_pic_can_deliver == 0    IRQ0's in-service bit is still set -> we are not seeing
                                 the guest's EOI, which would be OUR bug, not a rate one
     virtual-IF clear            the client has interrupts off -> only the cooperative
                                 path can ever deliver, and TASK 2 is the answer
```
  **TASK 2 (probably the real fix).** Give the timer the same cooperative treatment the
  Sound Blaster got in `7a13b45`. The cooperative PM-loop injection at
  `dpmi_inject_pm_irq(&m, tib, 0x08, steps)` needs **no SuspendThread and no g_lock**,
  and the PM loop is entered constantly (every INT 31h, every trapped port access). It
  is gated today by `g_pm_irq0_latch` + `DPMI_IRQ0_ARM_QUIET_MS`; the SB fix worked
  precisely because it stopped depending on the async path. Check whether IRQ0 can lean
  on the same mechanism rather than on `SuspendThread` at 144 Hz.
  **TASK 3 (helps BOTH defects, biggest and riskiest).** `g_lock` contention. Doom's
  mode-Y drawing takes it ~43,000 times a second for `outpw(0x3C4, ...)` mask changes,
  each one an `UnmapViewOfFile`+`MapViewOfFileEx` pair (1.95M swaps a run). That is the
  root of the timer starvation AND most of the video cost. A cheaper plane swap, or a
  mask path that does not need the device lock, would pay twice.
  **TASK 4 (video, independent).** Name the writer that puts phase-1 data in all four
  planes -- see the status-bar section above. Record which mask was live when each bar
  offset last changed; sample it, since 2M swaps x 2560 bytes cannot be diffed each time.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★ NEW INSTRUMENTS (session 23). All bounded; all on by default.           │
└──────────────────────────────────────────────────────────────────────────────┘
```
   STAGE2: sb replay:     blocks >=90% identical to the same RING OFFSETS one lap
                          earlier = blocks DMX never refilled. Live, on the audio
                          thread, no allocation, no I/O. Agrees with the offline
                          capture to 0.4 points, so sbdump+copy+anchor is no longer
                          needed to see the echo. (vdd_sb.c / SB_LAP_MAX.)
   STAGE2: sbblk NN       the block ledger: cap_off / block_len / 8237 state at each
                          of the first 24 completions. cap_off is the load-bearing
                          column -- it MEASURES the block grid that sbref.py used to
                          infer. First 24 entries, filled on the audio thread.
   STAGE2: pit budget:    raises / syncs / attempts / delivered / owed_max /
                          ui_gap_us. Separates four losses nothing distinguished.
   STAGE2: devirq         cooperative PM delivery of device lines 2-7.
   MODEYBAR               4 planes x 3 pages x rows 168..199 at wind-down, ~67 KB,
                          file only. Diff against the WAD with planejudge.py.
   MODEY-LATCH burst      now prints rows= and barbytes= beside the hex span, and
                          the cap is 4096 not 6.
   awbufs.txt             the audio LEAD as a controlled variable. ⚠ MEASURING
                          instrument, NOT a tuning knob -- see the knob table.
```
  **New analysis tools, all in `tools/doomoracle/` (they need `DOOM1.WAD` there):**
```
   barprof.py     per-row STBAR diff + 4-pixel collapse.  barprof.py <shot.bmp>
   whichplane.py  which plane survives each collapsed group
   replicate.py   is the bar one plane replicated four times?
   planejudge.py  judge the MODEYBAR dump against the WAD.  planejudge.py <log>
   blockphase.py  discontinuities by position within the DMA block, two anchors
```

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★★ METHOD — SESSION 23 PAID FOR THESE, AND THREE COST RIG RUNS            │
└──────────────────────────────────────────────────────────────────────────────┘
  ▶ **A BOUND ON AN INSTRUMENT IS A CLAIM ABOUT WHAT IS REPRESENTATIVE.** "The bursts
    only touch rows 186..199" was generalised from the SIX descriptions the instrument
    was capped at -- of which only TWO had changed bytes -- out of 160 bursts. It
    retired a root cause and cancelled a subsystem. Raising the cap changed the answer
    (184, not 186). The conclusion survived; until then it was luck, not evidence.
  ▶ **AN INSTRUMENT THAT INFERS ITS OWN REFERENCE FRAME CONFIRMS WHATEVER IT GUESSED.**
    `sbref.py` inferred the block grid and anchored it at byte 0. Real boundaries were
    at 15 + n*256. Every "offset 2 of the block" statement was measured against a
    guessed phase. Fix: make the HOST emit the anchor (the block ledger).
  ▶ **A NUMBER IN A LOG IS NOT A MEASUREMENT UNTIL IT IS IN THE UNITS OF THE CLAIM.**
    `0x3a1c..0x3e7f` was read as "the status bar" for a whole session. It is rows
    184..199. The burst line now prints `rows=` next to the offsets.
  ▶ **A STALE ARTEFACT READS AS A RESULT; A MISSING ONE FAILS LOUDLY.** Nothing ever
    copied `sb.raw` off the box, so "before" and "after" runs of a deliberately changed
    binary analysed the SAME hours-old file and produced BYTE-IDENTICAL histograms.
    Caught only by `md5`-ing two captures that were supposed to differ. Delete the
    destination BEFORE the run; never `>nul 2>&1` a collection step; never delete the
    source afterwards (the first fix did, and left nothing to diagnose).
  ▶ **TWO COUNTERS IN THE SAME BASIC BLOCK CANNOT DISAGREE ABOUT WHETHER THEY RAN.**
    When one printed and the adjacent one did not, two rig runs went on "stale binary
    vs code not reached". It was the report buffer: `base` points PAST the preamble, so
    `report[8192]` has well under 8 KB of headroom. **Suspect the transport.**
  ▶ **THE OBVIOUS FIX MUST STILL BE MEASURED.** Raising the per-sync attempt budget was
    sound reasoning from correct numbers and did NOTHING (attempts 62->189/s, delivery
    56->55/s) while making the UI gap 10x worse. It would have shipped without the
    counters that were added ten minutes earlier.
  ▶ **A COUNTER THAT READS ZERO IS NOT A GUARANTEE.** `underruns=0` in EVERY run,
    including the one where the audio pump was starving at 38 blocks/s against the 86/s
    the sample rate demands. It only counts `waveOutWrite` failures. Had the replay
    metric been read alone, "lead 2 is better" would have shipped a half-speed mixer.
  ▶ **THE USER'S DESCRIPTION IS A MEASUREMENT, AGAIN.** "More like they have an echo
    than an all-out glitch" named the mechanism before the instrument did: the ring lap
    IS an echo delay, and the capture is 46% identical to the byte 185.8 ms earlier
    against ~22% at every neighbouring lag. Session 22 recorded this same lesson.
```
