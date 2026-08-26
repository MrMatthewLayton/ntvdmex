# Session 19 — 2026-08-23/24

> Archived verbatim from `return-ntvdm.md`, which was the single rolling handoff
> file until 2026-08-26. Split by session so it can be read by topic and date
> rather than by scrolling. **Nothing has been edited** — including conclusions
> that later sessions REFUTED, which are kept on purpose: the refutations are
> some of the most valuable material here.

```
═══════════════════════════════════════════════════════════════════════════════
██ ▶▶▶ SESSION 19 (2026-08-23/24). DOOM COMPLETES ITS ENTIRE STARTUP, MATCHING ██
██     STOCK LINE FOR LINE, AND RENDERS ITS TITLE SCREEN AT 320x200 --         ██
██     CONFIRMED ON THE PHYSICAL SCREEN. IT IS **NOT** PLAYABLE YET.           ██
═══════════════════════════════════════════════════════════════════════════════

  Branch `m9/completeness`, HEAD `d2256cd`. Session 18 ended with Doom dying after
  FIVE timer ticks. It now runs ~170,000, finishes startup identically to stock
  ntvdm, and draws its title screen correctly.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★★ READ THIS FIRST: WHAT "WORKING" CURRENTLY MEANS                        │
└──────────────────────────────────────────────────────────────────────────────┘

  The title screen ONLY appears with a **guest patch applied by hand**:
  ```
     pmbp.txt  ->  03aed1fe 00000000 00000005
  ```
  That SKIPS `D_Display`'s `call R_ExecuteSetViewSize` (obj1+0x1d1fe, 5 bytes).
  Without it Doom dies inside that function. **So this is the DISPLAY PATH proving
  itself, not playable Doom.** The view is never set up; the demo cannot play.

  ⚠ **THE LONG RUN IS FLAKY, ROUGHLY 1 IN 3.** A good run is ~550k log lines and
    ends `STAGE2: complete`; a flake is ~55k lines with no graphics. This is
    INDEPENDENT of every change made this session. **Never conclude anything from a
    single run** — that mistake was made repeatedly and cost real time.

  ⚠ **GATES NOT YET RUN ON `d2256cd`.** Committed deliberately without gating so the
    hard-won working state could not be lost. FIRST TASK NEXT SESSION: run
    `./tools/dostest/run.sh` (expect 580 checks / 16 suites, 0 failed),
    `selftest.com`, `dpmitest.com`, `dpmiback.com`, `check-imports.sh`.
    ⚠ The off-VM figure is **580 across 16 suites** — the long-quoted "349/349
      (8 suites)" is WRONG and counts only the suites printing one of the two
      formats `run.sh` uses. Add them up; do not quote from memory.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★ THE TWO FIXES THAT DID IT                                               │
└──────────────────────────────────────────────────────────────────────────────┘

  **1. THE DEFAULT PM STUBS WERE 16-BIT (`0830492`) — 5 TICKS -> 5,520.**
  Each default PM vector is `C4 C4 CF`: a BOP we service, then an IRET back to
  whoever chained in. That selector was built 16-bit unconditionally. Doom's timer
  ISR chains to the previous vector-8 handler every sixth tick, and `INT 31h 0204`
  reported that as our default stub. We serviced the BOP, advanced past it, and left
  the guest at `0x37:0x1a` about to run `CF` — **a 16-bit IRET popping the 12-byte
  frame a 32-bit ISR pushed.** Six bytes taken, garbage CS:EIP, VDM gone, no
  diagnostic. Fixed with one descriptor bit, synced at use (`dpmi_sync_defsel_width`)
  because the client's width is unknown when the table is built.
  ▶ THIRD instance of *frame width and descriptor width are the same question* —
    see also the initial-selector and PM-return-catcher notes below.

  **2. UNCHAINED "MODE Y" VIDEO (`4393f1d`, `f14d13d`, `d2256cd`).**
  Doom's DOS build uses unchained 320x200 for page flipping. `chain4=00` is CONFIRMED
  in `STAGE2: video now:`, not inferred. The implementation, and every piece was
  forced by a defect seen on the physical screen:
  ```
     chain4        SR4 bit 3 tracked.
     de-interleave SNAPSHOT ON MAP-MASK CHANGE (a port write, cheap). NEVER a page
                   trap -- arming the A000 trap collapsed the run from ~553k lines
                   to ~55k, because with it armed the interpreter becomes the CPU.
     live plane    the plane the mask currently selects is read LIVE from the
                   aperture, but ONLY when the mask selects EXACTLY ONE plane.
     page address  DETECTED from the data (modey_page): busiest 16000-byte page,
                   scanned on a **0x4000 stride** -- pages are ALIGNED to 0x4000
                   even though a page only occupies 16000 bytes.
  ```
  ★ **THE FOUR VISUAL DEFECTS AND WHAT EACH MEANT** — this is the debugging map:
  ```
     "tiled, 80px period, 50 rows"   -> chunky display of mode-Y data (200/4, 320/4)
     "cut off and stitched to end"   -> page stride 16000 instead of 0x4000
                                        (16384-16000 = 384 = 4.8 rows of roll)
     "pixelated, not stitched"       -> several planes reading the SAME aperture
                                        bytes (multi-bit mask live-read) -> each
                                        group of 4 columns repeats
     "vertical black stripes"        -> a plane never captured (every 4th column
                                        black); ymask=08 named plane 3
  ```

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★ THE ONE BUG THAT MATTERS NOW: `R_ExecuteSetViewSize`                    │
└──────────────────────────────────────────────────────────────────────────────┘

  Skipping `D_Display`'s call to it is worth **31x the run length** (55k -> 554k log
  lines) and turns a silent VDM teardown into `STAGE2: complete`. It is THE bug.

  **The call stack was walked with `pmbp.txt`, every step confirmed by HIT COUNTS:**
  ```
     obj1+0x41ce8  INT 31h helper                completes (1608 hits)
     obj1+0xa110   DPMI 0600 wrapper, 0x1c-byte stack register struct
     obj1+0x3ed8f  locks the "d_intro" MUS lump  completes
     obj1+0x1d74e  music start, song 0x1d        completes
     obj1+0x1fa2c  loop x5                       exits NORMALLY (bounded counter)
     obj1+0x1d603  call 0x1d1e0 = D_Display      ** NEVER RETURNS **
     obj1+0x1d1fe  call 0x35a70                  ** NEVER RETURNS **
     obj1+0x35a70  R_ExecuteSetViewSize
  ```
  Function identification is from Doom's public source and is EXACT: the
  disassembly at 0x35aa6..0x35ad4 computes `viewheight = (setblocks*168/10)&~7`.
  ```
     obj3+0x38fe0 setblocks   obj3+0x38fe4 setdetail   obj3+0x38fe8 setsizeneeded
     obj3+0x32304 viewheight  obj3+0x32308 scaledviewwidth  obj3+0x3230c viewwidth
  ```
  ★ **DOOM'S DATA IS IN obj3 (@0x03B40000), NOT obj1 (@0x03AD0000).** Absolute
    operands in a raw-file disassembly are UNRELOCATED; the loader fixes them
    against the DATA object. Dumping obj1_base+offset returns code bytes and
    nonsense (setblocks = 0x3bb4fc4). With the right base: setblocks=9, setdetail=0,
    scaledviewwidth=288, viewheight=144 — all healthy, all the normal defaults.

  ⚠⚠ **THE BREAKPOINT INSTRUMENT LIES INSIDE THIS FUNCTION.**
     `0x03b05af2` ALONE -> HIT. The same breakpoint with `0x03b05ae5` also armed ->
     NOT hit. Only ONE hit ever occurs in this region however many are armed (7
     one-shots: 7 armed, 1 hit — so it is not the re-arm). **Multi-breakpoint
     "never reached" conclusions are WORTHLESS here; use SOLO probes**, one per run.
     The wider call-stack walk above is sound — it used separated sites in code
     taking 1600+ hits — but the fine narrowing inside 0x35a70 is not.
  ▶ Solo probes bracket the death to a 157-byte window: `0x35af2` HIT, `0x35b8f`
    and `0x35c46` NOT. Against Doom's source that window is the centring/projection
    block and the detail-shift branch: `viewwidth = scaledviewwidth >> detailshift`,
    `centerx/centery`, `centerxfrac`, `projection`, then `colfunc/spanfunc` stores.
    **Plain movs, shifts and function-pointer stores. NOTHING THAT CAN FAULT.**
    That is the part that does not add up, and it is where to start.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ⚠⚠⚠ DEAD ENDS AND REFUTATIONS. DO NOT RE-SPEND A SESSION ON ANY OF THESE.    │
└──────────────────────────────────────────────────────────────────────────────┘

  * **"The async injector tears the VDM down" (session 18) — REFUTED.** Tagging every
    early-out (`g_async_why`) shows it bailed six times on `why=9` (the arm-quiet
    hold-off) and succeeded ONCE, and that success is immediately followed by all
    five cooperative ticks. **Async is not the killer; it is what hands the host back
    control.** The control that indicted it also removed `g_hcpu` — two variables.
  * **"Coalescing the tick drain changed nothing" — REFUTED.** The batch drain NEVER
    RAN: zero `BATCH` lines, because the async-catcher branch it hangs off is never
    taken. Not ineffective — unreachable.
  * **The A000 page trap for mode Y** — collapses the run 553k -> 55k lines.
  * **Claiming CRTC 0x3D4/0x3D5** — regresses Doom BOTH ways: with a read handler
    (55,740 lines vs 555,296) and write-only with reads left at the unclaimed
    0xFFFFFFFF (three attempts, all ~55k). Not read semantics, not range shadowing
    (0x3DA is a separate claim). **Mechanism UNKNOWN — this is a real open question**,
    and it is why the page address is detected from data instead of read from the
    register. If you retry it, gate it and run Doom THREE times.
  * **Patching Watcom thunk vectors 0x32/0x34/0x35/0x36** — no change; those vectors
    are never serviced. Reverted.
  * **Narrowing ESP on the far-jmp path** — no change to the argument bug (467 INT 31h
    either way). Kept anyway (the high half is never meaningful with a 16-bit SS).
  * **INT 15h as the argument-bug cause** — the calls (AX=0xBFDE, 0xBF02) are at line
    39 of 845, during the MZ stub, LONG before the PM switch.
  * **A null-DS fault at obj1+0x41cfc** — my own transcription error (dropped two
    bytes hand-copying a hex dump). EDI=0x03bc5b4c and DS=0x0000018f are both valid.
  * **The "entry+0 or entry+3" fault rule** — a sampling artifact; refuted by pmal.
  * **GH #18 / the raw `#GP` reflect** — a PROVEN dead end since run 71.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★ OTHER OPEN ITEMS                                                          │
└──────────────────────────────────────────────────────────────────────────────┘

  * **COMMAND-LINE ARGUMENTS: the "quit" is OURS.** The trampoline arm ends with
    `break` — *"for run 59 the reflect firing IS the deliverable"*. So ANY real PM
    fault ends the run with a tidy `STAGE2: complete`, which reads exactly like the
    client choosing to exit. **DOS/4GW is not quitting; we kill it.** The fix is
    DPMI-standard and the plumbing exists: clients register exception handlers with
    `INT 31h 0203` and **DOS/4GW registers THIRTEEN**. Dispatch to `g_pm_exc[n]`.
    ⚠ Blocked on: **`VTIB_FLT_SAVCS`/`VTIB_FLT_SAVEIP` DO NOT HOLD CS:EIP.** They
      hold SS and ESP+0xa — proved when clearing the junk top half of ESP changed the
      reported "EIP" from `0xb33b6f1e` to `0x00006f1e`. A field that tracks ESP is
      not EIP. `sav3` (tib+0x640) is the likelier faulting EIP. Calibrate against a
      fault at a KNOWN address first; `pmfault`'s HLT/INT3 CANNOT do it (they die
      with no reflect) — write a variant that loads a bad selector.
  * **`pmkernel.flag` (VdmStartExecution runs PM)** is NOT the road to Doom: two PM
    entries vs full startup on the far-jmp path. Keep it as a spike only.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★ HOW TO RUN IT, AND THE KNOBS ADDED THIS SESSION                          │
└──────────────────────────────────────────────────────────────────────────────┘

  ```
  cp build/ntvdmhost.exe /tmp/xpshare/bm/ntvdmhost.exe
  md5 -q build/ntvdmhost.exe /tmp/xpshare/bm/ntvdmhost.exe     # MUST match, every time
  printf '03aed1fe 00000000 00000005\r\n' > /tmp/xpshare/pmbp.txt   # the skip
  rm -f /tmp/xpshare/result_doom.log
  printf 'doom\r\n' > /tmp/xpshare/cmd.tmp && mv /tmp/xpshare/cmd.tmp /tmp/xpshare/cmd.txt
  ```
  ⚠ **EVERY write to `/tmp/xpshare` NEEDS `dangerouslyDisableSandbox`.** A sandboxed
    write FAILS SILENTLY and you then read a STALE log and report a stale result.
    This happened twice; check `md5`/timestamps.
  ⚠ **DELETE `pmbp.txt` AFTER EVERY RUN** — a stale one silently alters later runs.

  **New knobs:** `nosb.flag` (unfits BOTH sound devices: SB DSP withholds its 0xAA,
  OPL status floats 0xFF — Doom still loads MUS lumps, so sound is NOT the cause of
  anything), `pmvehpass.flag` (let a non-INT PM fault fall through the VEH —
  VdmStartExecution does NOT return it as an event; the swallow is load-bearing).
  `capture.flag` self-screenshots to `C:\ntvdmex\shotNN.bmp` every ~300 ms (raised
  from 2 s); the `doom` target uses doomrun.bat which does NOT copy them off, so
  fetch with `controld exec ... copy`. `LOG_MAX_BYTES` is now **256 MB**.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★ METHOD — SESSION 19 PAID FOR THESE                                       │
└──────────────────────────────────────────────────────────────────────────────┘

  ▶ **THE PHYSICAL SCREEN FOUND FOUR BUGS MY INSTRUMENTATION DID NOT.** Every mode-Y
    defect — tiling, roll, pixelation, stripes — was reported by the user looking at
    the monitor. My analysis kept inspecting the RICHEST captured frame, which
    systematically hides bad runs. **When output is visual, a human glance beats a
    metric on the best sample.**
  ▶ **PARSE THE LOG, DO NOT RETYPE IT.** I hand-copied a hex dump into an analysis
    script, dropped two bytes, and published a confident "null DS fault" that was
    pure transcription error.
  ▶ **A COUNT OF LOG LINES IS NOT A HEALTH CHECK — DECODE WHAT THEY SAY.** "8 clean
    entries, 8 INT 31h serviced" went into a commit subject; every one of those lines
    was a fault we had mislabelled.
  ▶ **WHEN THE SAMPLES ARE INCIDENTAL, CHOOSE THE CODE.** Ten fault addresses read off
    whatever the client happened to have there fitted FOUR incompatible rules. A
    purpose-built client (`pmstep.asm`, `pmal.asm`) settled it in one run.
  ▶ **CHANGE ONE THING.** Bundling the A000 trap with the CRTC claim made a
    regression unattributable and cost several runs to unpick.
  ▶ **AN INSTRUMENT CAN LIE BY OMISSION.** The 32 MB log cap made a truncated run look
    like a finished one; only the absence of the cap marker caught it.


╔══════════════════════════════════════════════════════════════════════════════╗
║ THE USER'S INSTRUCTION (2026-08-22): "North star is playable Doom"           ║
╚══════════════════════════════════════════════════════════════════════════════╝
```
