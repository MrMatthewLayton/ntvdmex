# Session 22 — 2026-08-24

> Archived verbatim from `return-ntvdm.md`, which was the single rolling handoff
> file until 2026-08-26. Split by session so it can be read by topic and date
> rather than by scrolling. **Nothing has been edited** — including conclusions
> that later sessions REFUTED, which are kept on purpose: the refutations are
> some of the most valuable material here.

```
═══════════════════════════════════════════════════════════════════════════════
██ ▶▶▶ SESSION 22 (2026-08-24, same day, after the session-21 handoff).       ██
██     **DOOM IS PLAYABLE.** Menu, a whole level, intermission, PCM + MIDI.   ██
██     What is left is TWO NAMED, MEASURED DEFECTS -- not a mystery.          ██
═══════════════════════════════════════════════════════════════════════════════

  Branch `m9/completeness`. 16 commits, `9435485`..`1079268`.
  User's verdict at the end: "Doom is playable, just glitchy."
  Gates GREEN on the shipped binary throughout: off-VM **630 checks / 18 suites,
  0 failed**; check-imports pass; bare-metal `selftest.com` 8/8; `dpmitest.com`
  clean exit; `dpmiback.com` clean (its `<<< MISMATCH >>>` is the documented
  benign sentinel -- do not chase it).

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★★ WHAT WORKS, WITH NO GUEST PATCH OF ANY KIND                            │
└──────────────────────────────────────────────────────────────────────────────┘
```
   startup     every line stock ntvdm prints, through ST_Init into D_DoomLoop
   video       title screen is PIXEL-EXACT against the IWAD (0 of 64000), the
               attract demo and the 3D view render correctly at 320x200 unchained
   keyboard    ESC opens the menu, arrows navigate; the user played a whole level
               and reached the intermission screen
   PCM         SB + DMA -> the mixer -> waveOut. sb_blocks ~3600/45s at 11025 Hz
               STEREO, host_wave=open, underruns=0
   MIDI        MPU-401 -> XP's GS Wavetable synth, ~750 messages a run. WORKS.
```

┌──────────────────────────────────────────────────────────────────────────────┐
│ ⚠⚠ THE TWO DEFECTS THAT REMAIN. BOTH ARE LOCALISED. START HERE.              │
└──────────────────────────────────────────────────────────────────────────────┘

  **1. THE STATUS BAR — VGA WRITE MODE 1 (THE LATCH COPY).**
  User: "mostly correct now, but still some pixelated areas, mostly on the game bar
  ... just sections of it". Measured with the new WAD oracle:
```
     STBAR vs a live frame     58-61% of compared pixels differ, EVERY frame
     4-pixel groups            61.8% collapsed to ONE value (ref is 12.6% uniform)
     best alignment            exactly (0,0) -- it is NOT a shift
     which plane survives      plane 1 in 924 groups, plane 0 in 370, planes 2/3 in 19
     when                      wrong from the FIRST frame the bar appears; flat after
     where                     spread over the whole bar INCLUDING the bottom border
                               rows Doom never overdraws -- so it is not the numbers
                               and face being drawn on top
```
  **CAUSE.** Doom maintains the bar with `mask := 0x0F` / `write mode := 1` / copy /
  `write mode := 0`, 120 times a run (measured: `wmode hist 00x79 01x78`, and the
  `(wmode,mask)` pairing shows every mask write happens in write mode 0). A latch
  copy reads all four planes into the VGA's latches and writes all four back. **Our
  per-plane backing points A0000 at ONE plane, so a copy that should move four moves
  one.** That is also exactly why the 3D view and the title screen are perfect --
  they are plain write-mode-0 stores.

  ⚠ **RULED OUT BY EXPERIMENT, DO NOT RE-SPEND TIME ON THESE:**
```
     "it is our multi-plane fan-out"   DISABLED IT ENTIRELY -> bar still 58% wrong
     "the render collapses it"         the PLANES are collapsed: 1707/2560 offsets
                                       hold the same byte in all four
     "we display the wrong page"       all THREE pages equally collapsed (0x6ab/0xa00)
     "it is Doom's own flat artwork"   reference is 12.6% uniform vs our 66.7%
     "it degrades over time"           wrong from the first frame, flat thereafter
```
  ⚠ **IT CANNOT BE RECONSTRUCTED AFTER THE FACT. THIS IS ESTABLISHED, NOT SUSPECTED.**
  Describing the bursts directly: **most change NOTHING** (the destination already
  matches) and the rest move **~59 SCATTERED bytes** at plane offsets 0x3a1c..0x3e7f
  (rows 186-199). Only bytes that already differ between source and destination ever
  appear, so **the source offset is not in the data**. Two inference schemes agree:
```
     plausible page strides            explained 71 of 120 (a lucky guess, not a rule)
     delta derived from the copied run explained NONE -- which is the informative
                                       result: a verbatim region copy WOULD have its
                                       longest changed run appear verbatim in the
                                       pre-burst image, and it does not
```
  ▶ **NEXT: MAKE THE ACCESSES VISIBLE.** VirtualProtect A0000 to NOACCESS on entering
    a write-mode-1 burst, decode the faulting instruction in the VEH and apply the real
    latch semantics (a read loads all four planes; a write stores all four under the map
    mask), then resume. ~614k faults a run at Doom's rate, order 10% of a core --
    feasible and EXACT. It is a subsystem, not a tweak. The instruction forms to expect
    are `mov al,[mem]` / `mov [mem],al`; check for `movsb`/`rep movsb` first, because a
    rep would need the whole string emulating.
  ▶ **THE ONE UNEXPLAINED CLUE, AND IT MAY BE THE REAL ONE:** of the collapsed groups
    the surviving byte is **plane 1's 2.5x more often than plane 0's**. The scratch is
    seeded from the LOWEST selected plane, so a fan-out collapse should leave PLANE 0's
    byte. Something other than the paths already examined is writing all four planes.
    That asymmetry is not explained by the latch-copy story and is worth chasing before
    building the trap.

  **2. PCM/WAVE — A DEFECT AT THE DMA BLOCK BOUNDARY.**
  User: "still stutters", and it survived two real fixes (below). The counters were
  exhausted, so the raw DMA ring was captured and analysed (`sbdump.flag`, `sbref.py`):
```
     duration    41.5 s of audio from a 45 s run  -> THE RATE IS RIGHT
     framing     corr(L,R) = 0.978                -> THE STEREO FRAMING IS RIGHT
     periodicity discontinuities 12.8x OVER-REPRESENTED AT OFFSET 2 OF EVERY
                 128-FRAME BLOCK (183 against a mean of 14.3)
```
  A count of "glitches" says nothing -- game audio is full of sharp attacks. **A jump
  rate that peaks at ONE FIXED OFFSET in every block is a defect at the block boundary**,
  and at 86 blocks a second that is an audible buzz rather than noise.
  ▶ **NEXT:** three things happen at that boundary -- the block-completion IRQ raised
    from the AUDIO thread, the auto-init reload in the 8237 model (`dma_step`, which
    reloads `cur_addr`/`cur_count` from base on TC), and the guest's refill arriving
    asynchronously afterwards. Establish which of the three lands two frames into the
    new block. A dump of `cur_addr`/`cur_count`/`block_left` at each block completion
    for the first few blocks would settle it.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★ WHAT WAS FIXED THIS SESSION. Every one was OURS.                        │
└──────────────────────────────────────────────────────────────────────────────┘
```
 1 INT-SITE PATCHER      `dpmi_patch_code_region` matched `CD nn` as a BYTE PAIR and
   (the 5-session       rewrote a `jle`'s DISPLACEMENT at obj1+0x3593f. Fix =
    Doom killer)         src/host/x86len.h, a length decoder + boundary vote. Session
                         20's "BOP-free stretch LENGTH" is REFUTED. See session 21.
 2 16-BIT VGA INDEX      `outpw(0x3C4, index|value<<8)` is ONE instruction that writes
   WRITES               index AND data; seq/gc/crtc_out took the whole word as an index
                         and DROPPED THE DATA BYTE. Doom selects each mode-Y plane
                         exactly that way, so the map mask never moved.
 3 THE CRTC IS CLAIMED   ...and that is why claiming 0x3D4/0x3D5 "regressed Doom three
                         times, mechanism UNKNOWN" in sessions 19-20: Doom page-flips
                         with the same one-instruction idiom. RESOLVED, warning retired.
 4 MODE-Y PER-PLANE      A0000 is REMAPPED to the plane the mask selects (four
   BACKING               pagefile-backed sections + MapViewOfFileEx at a fixed address,
                         ~1.8M swaps a run, 0 failures). Six after-the-fact
                         de-interleave rules were measured and all traded resolution
                         against stale content; this replaces the guess entirely.
                         Title screen went to 0-of-64000. `noremap.flag` falls back.
 5 THE BUS CLAIM TABLE   VDD_MAX_PORTS was 16 and EXACTLY FULL. Adding the CRTC claim
   WAS FULL              pushed the LAST device added -- the MPU-401 -- off the bus,
                         its claim returned -1, nobody looked, and MIDI died silently.
                         32 now (17 in use), refusals counted and reported.
 6 THE PIT HELD THE      vdd_pit_add_clocks raises one IRQ0 per reload period for the
   DEVICE LOCK           whole elapsed gap, inside host_pit_sync's lock, and EVERY one got
                         a full SuspendThread round trip. A 50ms gap at Doom's 16kHz
                         music timer is 800 raises. Measured on a play session: 44ms
                         lock hold with the AUDIO thread blocked 36.8ms behind it.
                         One attempt per sync -> hold 14-22ms, audio wait 5.8-16.6ms.
 7 THE RESAMPLER ATE     rs_need() asked for two extra source samples every chunk. For
   PCM SAMPLES           the SB that is DATA LOSS -- vdd_sb_render IS the transport --
                         172 dropped samples a second. Pinned by a test with teeth.
 8 THE TICK BATCH        DPMI_IRQ0_BATCH drained 64 ticks back to back = 0.45 SECONDS
   COMPRESSED TIME       of game time in microseconds, and the guest's timer ISR is
                         where DMX writes PCM. Batch of 4; tick rate unchanged (135/s).
 9 ABANDONED ISR         dpmi_inject_pm_irq gave up after 64 phases and restored the
                         context, leaking DMX's dispatcher depth and 4KB of its stack.
                         Bound by WALL CLOCK now. (Session 21.)
10 PM KEYBOARD           A DPMI client had no cooperative IRQ1 path at all, and the
                         pending count was claimed AFTER the handler ran -- which
                         cancelled the interrupt the handler itself had raised.
                         (Session 21.)
11 BLASTER=A220 I5 D1 T3 in the guest environment. ⚠ MUST TRACK vdd_sb.h; telling the
                         guest I7 while the card raises IRQ5 is worse than silence.
12 MULTI-PLANE FAN-OUT   copies only what CHANGED, not the whole scratch. Correct in
                         itself -- but proven NOT to be the status bar's cause.
```

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★★ THE NEW INSTRUMENTS. THE ORACLES ARE THE BIG ONE.                      │
└──────────────────────────────────────────────────────────────────────────────┘

  **`tools/doomoracle/doomref.py` — A PIXEL-EXACT VIDEO ORACLE FROM DOOM1.WAD.**
  Every fixed screen Doom draws is a lump in the IWAD in a documented format with a
  documented palette, so the bytes that SHOULD reach the framebuffer are computable.
  A screenshot off another machine is a photograph of an answer; the WAD is the answer.
```
     ./doomref.py DOOM1.WAD dump TITLEPIC ref.bmp
     ./doomref.py DOOM1.WAD cmp  TITLEPIC shot02.bmp        # 0 of 64000 = exact
     ./doomref.py DOOM1.WAD cmp  STBAR    shot09.bmp --at 0,168
```
  ⚠ Compares **palette INDICES**, so it needs the host's 8bpp `shotNN.bmp`, not a PNG.
  ⚠ `DOOM1.WAD` is gitignored (4 MB, and it is id's). Copy it off the rig:
```
     printf 'exec cmd /c copy "C:\DOOMS\DOOM1.WAD" "...\doombin\"\r\n' > control.txt
```
  ▶ **STBAR's opaque area covers where the numbers and face are drawn ON TOP**, so a
    raw `cmp` overstates. Look at the DIFF MAP (which rows/columns) before concluding;
    that check is what stopped a wrong conclusion this session.

  **`tools/doomoracle/sbref.py` — THE SAME IDEA FOR SAMPLED AUDIO.**
  `sbdump.flag` makes the host record every byte `vdd_sb_render()` pulls out of the
  guest's DMA ring to `C:\ntvdmex\sb.raw` (no I/O on the audio thread; written at
  wind-down). Reports duration (rate), corr(L,R) (framing) and the jump histogram
  WITHIN the block (periodicity). The third is the one that found the click.

  **Other instruments added this session:**
```
   SNDIO            bounded trace of the sound-card conversation (SB/OPL/MPU). ⚠
                    io_hot_note only fires on the V86 arm, so STAGE2's "hot ports:"
                    is EMPTY for a PM client -- it is not a measurement.
   KEYIRQ / KEYPM   every keyboard IRQ raise and PM delivery attempt with all five
                    gate values. Bounded; a run with no keys pays nothing.
   ASYNC-SITE       each distinct PM CS:EIP the async injector finds the CPU at, ONCE.
                    The observation pass RESUMES the thread and returns WITHOUT
                    injecting, so the line is on disk before anything is rewritten.
   PMSTRETCH        wall-clock of each dpmi_enter_pm(), new maxima only.
   STAGE2 sound     sb_blocks/rate/mode/dspwr, midi_msgs, mpu_uart, and whether the
                    HOST wave and MIDI devices opened.
   STAGE2 modeY     remap/swaps/fanouts/failed, latch_solved/UNSOLVED, mapmask hist,
                    wmode hist, (wmode,mask) PAIRS, per-page bar plane-equality.
   STAGE2 bus       "bus ok: N refused, ports=17/32 mem=1/8 dev=9/16".
   MODEY-LATCH      describes a burst: bytes changed, span, and whether the
                    destination is a CONSTANT (a latched fill) or varies.
```

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★ RIG KNOBS (all on the share; ALL need dangerouslyDisableSandbox to write) │
└──────────────────────────────────────────────────────────────────────────────┘
```
   headless_ms.txt   run cap in ms, max 600000. **45000 for measurement runs,
                     600000 for a play session.** Put it back to 45000 afterwards.
   capture.flag      self-screenshot. ITS CONTENTS ARE THE PERIOD IN MS (1100 spans a
                     45s run; the old fixed 300ms only ever saw the first 12 SECONDS,
                     which made a scripted keypress at 14s look like it did nothing).
   sbdump.flag       raw PCM capture -> C:\ntvdmex\sb.raw. **doomrun.bat now DELETES the
                     share's copy and collects the new one into doombin\, reporting the
                     result in sbcopy.txt.** ⚠⚠ IT DID NOT BEFORE: nothing ever copied
                     sb.raw back, so the share kept ONE capture from whenever somebody
                     last did it by hand (14:16) and every later before/after comparison
                     silently re-analysed THAT SAME FILE. Two runs of a deliberately
                     changed binary produced BYTE-IDENTICAL histograms and it was only
                     caught by md5-ing the two "different" captures. **A stale artefact
                     is worse than a missing one: absent fails loudly, stale reads as a
                     result.** Delete first, and never swallow the copy's output.
   awbufs.txt        waveOut buffers queued = THE AUDIO LEAD (each ~11.6 ms). 2..6,
                     absent = 6. ⚠ LOW IS NOT BETTER: at 2 the pump starves to 38
                     blocks/s against the 86/s the rate demands, and `underruns` stays
                     0 because it only counts waveOutWrite failures. It is a MEASURING
                     instrument, not a tuning knob -- put it back (delete it) after.
   modey.txt         mode-Y run-coalescing slack, only used when the remap is OFF
   noremap.flag      disable per-plane backing, fall back to the heuristic
   qimode.txt        `20` starts the synthetic-key thread
   keys.txt          scripted scancodes: `w<ms>` waits, `e` = extended prefix, two hex
                     digits = a make/break pair.
                     e.g. `w15000 01 w3000 e50 w2000 e50 w2000 e48 w6000 01 w5000`
   ⚠ DELETE EVERY KNOB BEFORE A GATE RUN OR A PLAY RUN. A stale one silently alters it.
```
  ⚠ **THE BASH TOOL TIMES OUT AT 2 MINUTES AND A DOOM RUN TAKES ~90-100s PLUS THE LOG
    COPY.** Start `bmqueue.sh` with `( ... &)` and then `sleep 100`, or the tool kills
    the wrapper mid-run and you read a stale log.
  ⚠ **A PLAY SESSION (the user listens/plays) is queued differently:** clear EVERY knob,
    `headless_ms.txt`=600000, and write `cmd.txt` DIRECTLY rather than via bmqueue.sh --
    a 10-minute run far exceeds its result-log wait. Poll for `cmd.txt` to be CONSUMED
    (that is the proof the watcher is alive), then hand back. Put `headless_ms.txt` back
    to 45000 afterwards.
  ⚠ **ALWAYS `md5` THE DEPLOYED EXE AGAINST `build/ntvdmhost.exe`.** Two of the three
    traps that cost runs this session were "is the box running what I think it is".
    Deploy `ntvdmhost.exe`, never `ntvdmex.exe`.
  ⚠ **PING FAILS FROM THE MAC SANDBOX** (the allowlist has no LAN hosts) -- that is NOT
    the rig being down. Use `nc -z -w 3 192.168.1.29 445` with
    `dangerouslyDisableSandbox`, and remember every share write needs it too.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★★ METHOD — SESSION 22 PAID FOR EVERY ONE OF THESE                        │
└──────────────────────────────────────────────────────────────────────────────┘

  ▶ **MY METRIC MEASURED THE ARTEFACT, NOT THE PICTURE.** "Even-column match" went UP
    (0.55 -> 0.70) when the plane-remap fix landed, because real Doom content has many
    equal neighbours and the lower number had been our own misattributed columns
    scoring as detail. **A metric that moves the wrong way when a bug is fixed is worse
    than no metric.** It is retired; use the WAD oracle.
  ▶ **CHECK THE MODE BYTE BEFORE BELIEVING A RATE.** Reading `sb_blocks x 256` against
    the programmed rate gives "the DMA runs 1.87x too fast". It is wrong: Doom asks for
    STEREO (`0xC6`, mode byte `0x20`), so a 256-byte block is 128 FRAMES.
  ▶ **A SILENT RETURN VALUE COST US MIDI.** `vdd_claim_ports` returned -1 into a table
    that was exactly full and nobody looked. It took diffing a port trace against a
    working run to see 0x330 answering 0xffffffff.
  ▶ **AN INSTRUMENT THAT READS ZERO BECAUSE IT WAS NEVER WIRED IS WORSE THAN NONE.** A
    `pageflips` counter printed zeros for an event that was demonstrably happening,
    because its backing field never made it into the file.
  ▶ **EVERY `log_write()` BEFORE THE PREAMBLE TRUNCATES THE LOG.** Three separate
    reports were eaten by this in one session (bus health, the A0000 remap report, the
    region probe). Buffer anything produced before the preamble and flush it after.
  ▶ **CLAIM BEFORE YOU RUN, NOT AFTER.** The keyboard's pending count was decremented
    after the handler ran -- and the handler re-raises, so it cancelled the interrupt
    the ISR had just asked for.
  ▶ **CONSUME ONLY WHAT WAS DELIVERED.** The mirror image: taking a timer tick before
    the injection discarded it whenever the injection declined.
  ▶ **A PHASE IS NOT A UNIT OF TIME**, and **A BATCH IS NOT A LICENCE TO COMPRESS TIME.**
  ▶ **RUN THE EXPERIMENT INSTEAD OF REASONING ABOUT THE SUSPECT.** Disabling the
    multi-plane fan-out outright killed the leading status-bar theory in one run, after
    several rounds of arguing about it.
  ▶ **THE USER'S DESCRIPTION IS A MEASUREMENT.** "The intro screen is 320x200 until the
    menu shows... the score screen went back to correct and then degraded again on the
    next level" IS the full-screen-blit vs dirty-box split, and it identified the
    mechanism when no instrument I had would have.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ▶ COMMITS (16), oldest first                                                 │
└──────────────────────────────────────────────────────────────────────────────┘
```
   9435485 dpmi: patch only real INT sites -- this is what killed Doom
   46462d5 doom: the attract demo plays, with sound
   ebd1bd3 input: the keyboard reaches a protected-mode client -- Doom's menu opens
   c740f4e docs: session-21 handoff
   141f347 video: honour 16-bit VGA index writes; claim the CRTC; dword the copy
   07835a5 video: de-interleave mode Y by attributing runs, and make the trade a knob
   e2f7486 bus+timing: the MPU had been pushed off the bus, and the PIT held the lock
   28d3e00 audio: the mixer was eating PCM samples it never played
   78104e6 video: settle whether A0000 can be remapped -- it can
   8475c49 video: A0000 IS the selected plane -- mode Y stops being a guess
   7a083fd video+timing: fan out only what was written; stop compressing time
   549e1c7 video: add a WAD-based pixel oracle; it finds the status bar is a LATCH COPY
   26cda7d tools: do not commit the IWAD
   78cf3ec video/audio: instrument the latch burst and the DMA ring
   2a0fc29 video: the status bar collapse is NOT our fan-out -- ruled out by experiment
   1079268 audio: capture the DMA ring and find the click -- at the block boundary
```
```
