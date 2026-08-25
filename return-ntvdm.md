═══════════════════════════════════════════════════════════════════════════════
██ ▶▶▶ SESSION 26 (2026-08-25). FOUR FIXES CONFIRMED; DOOM'S MOUSE IS OPEN.   ██
═══════════════════════════════════════════════════════════════════════════════

Session 25 left DOOM PLAYABLE WITH SOUND. This session fixed four things around it, all
USER-CONFIRMED BY HAND ON THE RIG, and left exactly one open. **Branch `m9/completeness`
is PUSHED to origin** (196 commits, including the whole session-25 milestone, which had
never left this machine).

✅ CONFIRMED FIXED
  `22116b5`  Host cursor is a TOGGLE (Input > Mouse > Show Host Cursor, Ctrl+F8), default
             OFF. Also fixed WM_SETCURSOR hiding the cursor over the STATUS BAR and its
             size grip -- it tested the hit-test alone, and a child window forwards its own
             HTCLIENT to the parent.
  `b49485d`  **EXIT CRASH.** `DMXCHK` dereferenced Doom's hardcoded linear 0x03b431f0
             UNCONDITIONALLY while printing the STAGE2 summary. Fine under Doom, an access
             violation under every other guest -- Skyroads crashed on exit four times and
             truncated its own log at `async why irq00`. Dr Watson named it in one read
             (`drwtsn32.log` is UTF-16; `iconv -f UTF-16LE`). Guard = `mem_readable()`.
             ⚠ The comment directly above it already warned about this exact failure; the
             SCAN was guarded and the PROBE two lines below it was not.
  `5b6a4a6`  **SKYROADS KEYBOARD LAG** ("arrows throw you off the road"). IRQ1 got ONE
             asynchronous attempt, AT THE RAISE; if the guest had interrupts off it fell
             back to the exec loop, which can only place it on a pass where they are on --
             and Skyroads in gameplay has them off ~96% of the time. Pacing IRQ0 evenly
             sliced those windows into many tiny ones. FIX = retry on windows we ALREADY
             HOLD: `async_inject_irq` verifies IF/VIF before injecting, so every IRQ0
             opportunity is a PROVEN enabled moment (~184/s); when a key waits, spend one
             on the key (the tick is not lost, just deferred a raise). Headless:
             `>=64ms` 43% -> **0%**, max 684 ms -> **6 ms**, all 186 interrupts placed.
             ⚠ **THE PACER WAS NEVER IN CONFLICT WITH THE KEYBOARD.** `pitpace` stays 1.
  `55151e7`  **F10 AND ALT ARE THE GUEST'S KEYS.** They never arrived as WM_KEYDOWN at all
             -- they are SYSTEM keys, and DefWindowProc turned them into menu activation,
             so Doom's SETUP.EXE never saw F10. Routed to the guest unconditionally.
             Alt no longer opens the menu bar (deliberate). Plus capture mode.

▶ RIG / HARNESS (all still true)
  Box 192.168.1.29. `mount_smbfs -N //guest@192.168.1.29/ntvdmex /tmp/xpshare`.
  FAST DEPLOY, no reboot, no watcher: write `deploy.bat` to the share, then
  `printf 'exec cmd /c "...\deploy.bat"\r\n' > control.txt` -- controld picks it up in
  ~10 s. Verify with the `dir` it writes to `deployed.txt` AND an md5 both sides.
  Interactive launchers now in `scripts/bm/`: `skyex.bat`, `doomex.bat`, `doomsetup.bat`
  (the last two are new; `doomrun.bat` is the HEADLESS one and sets `autoexit`, which
  self-exits the host mid-session -- the interactive ones delete that marker).
  KNOBS on the share: `pitpace`=1 `qimode`=0 `uitick`=15; also `keyirq`, `msens`,
  `pitprio`, `pitinj`. ⚠ Set `qimode`=0 before playing by hand or the key script fights you.
  HEADLESS KEY REPRO: `tools/dostest/skyroads-play.keys` + `.README`.

═══════════════════════════════════════════════════════════════════════════════
██ ▶▶▶ THE OPEN PROBLEM: DOOM HAS NO MOUSE -- NEITHER LOOK NOR BUTTONS        ██
═══════════════════════════════════════════════════════════════════════════════

User: capture works (pointer never reaches the desktop, caption says captured), Doom is
configured Keyboard+Mouse with left button = fire, and NOTHING mouse works.

MEASURED (the `MOUSE` line, dumped to the log every 5 s alongside KEYLAT/IRQ1GATE):

    raw_ok=1  wm_input=3927  abs_pkts=0  totx=-530  toty=-844     <- OUR side is FINE
    i33[0]=2  i33[3]=0  i33[B]=0  i33oth=1079  simint_unh=0

  * Raw input registers, 3927 WM_INPUT packets arrive, real deltas accumulate. The host
    side of the mouse is not the problem and does not need more work.
  * Doom NEVER calls AX=3 (read buttons) or AX=0Bh (read counters) -- which is exactly
    why neither look NOR the fire button works. One fault, both symptoms.
  * **BUT 1079 INT 33h CALLS DO ARRIVE, ALL WITH AX >= 0x10** -- and the histogram
    buckets 0-15 and lumps everything else into `i33oth`, so WE DO NOT KNOW WHICH.

⚠ TWO COMPETING EXPLANATIONS. They need OPPOSITE fixes and the next instrument must
  separate them BEFORE any code is written:
   (a) Doom is calling INT 33h functions we do not implement. `mouse_int33`'s
       `default: break;` ACCEPTS them silently and returns nothing -- the same
       "does nothing, reports success" shape as the 0300 bug below.
   (b) A MIS-PATCHED `CD 33` SITE. The DPMI host rewrites `CD nn` into BOPs; a false
       positive inside data or mid-instruction would call `mouse_int33` with ARBITRARY
       EAX, which is precisely what "1079 calls with AX >= 0x10" looks like. This is the
       class of bug that killed Doom for five sessions (session 21, `src/host/x86len.h`).

► NEXT, IN ORDER
  1. WIDEN THE INSTRUMENT, do not guess. Histogram the ACTUAL AX values (top-N, not a
     bucket), and log CS:EIP of the caller for the first ~16 calls. ONE headless run
     distinguishes (a) from (b).
  2. If (b): DIFF THE BYTES at those sites against `DOOM.EXE` on disk. That is the
     session-21 method and it found the last one inside an hour.
  3. If (a): implement the functions it names.

ALSO FIXED THIS SESSION BUT NOT YET THE CAUSE (keep it -- it is a real bug):
  **DPMI 0300 (simulate real-mode interrupt) only ever serviced INT 21h.** Every other
  vector loaded the real-mode register block, did NOTHING, and copied it straight back,
  so the client got its own registers echoed and read that as a successful call returning
  "nothing happened". Doom's `I_ReadMouse` uses `DPMIInt()` = 0300 with BL=33h, so this
  looked like the answer. It now dispatches 33h and COUNTS unhandled vectors.
  ⚠ `simint_unh=0` says 0300 is NOT the path these calls take, so the fix is correct but
    is not the cause. Do not re-litigate it.

ALSO FIXED, NOT RE-CONFIRMED: **the Win key got stuck down.** Capture is entered with
  Win+F10 while the hook is NOT yet installed, so Windows sees Win go down; the hook is
  installed a moment later and ate the key-UP, leaving the system believing Win was held
  forever. Every later keystroke became Win+key -- pressing `D` minimised the window,
  because Win+D is Show Desktop (user-reported, and it is what pointed at this). The hook
  now never swallows a key-UP. ⚠ **Swallowing a down without its up is a stuck modifier.**

HOST KEY = **Win+F10** (Scroll Lock kept as an alternative; many current keyboards have
  no Scroll Lock). Deliberately NOT Ctrl+F10: **Doom fires with Ctrl and uses every
  F-key** (F10 quit, F11 gamma, F12 spy), so a Ctrl+F<n> chord fights the guest for keys
  it needs constantly -- and F10 is a SYSTEM key, so a WM_KEYDOWN binding never even ran,
  which is why the first capture attempt silently did nothing. While captured, ALL other
  host hotkeys stand down (F11, Ctrl+F5, Ctrl+F8, Alt+Enter) and go to the guest.

═══════════════════════════════════════════════════════════════════════════════
██ ▶▶▶ SESSION 26 METHOD LESSONS -- THREE INSTRUMENT ERRORS IN ONE SESSION    ██
═══════════════════════════════════════════════════════════════════════════════

Four fixes for the Skyroads lag were built and shipped to the user before the right one,
and every wrong turn came from an instrument rather than from the code.

  1. **A COUNTER'S SCOPE IS A CLAIM.** `g_irq1_inj` and `keylat_pop()` both lived ONLY in
     the cooperative branch, so keys delivered ASYNCHRONOUSLY counted as neither delivered
     nor timed. The working fix therefore read as "places 78 of 186 interrupts -- it loses
     keys", and I REVERTED IT AND TOLD THE USER IT WAS REFUTED. 78 + 108 = 186 = the
     baseline; the arithmetic was there to be done. Same shape as [[counter-layout-is-a-claim]].
  2. **A 379-BYTE LINE IN `char kb[384]`.** Adding two fields took it past 418 and smashed
     the UI thread's stack. TWO experiments "died" on that and both verdicts were mine --
     including a confident "this reproduces the historical crash". The tell: the next
     build died identically while its new code path had provably never run (counter zero).
  3. **`ui_gap_us` AND `lk_wait_us` DO NOT TRACK INPUT FEEL.** Doom runs perfectly at
     ui_gap 120 ms / lk_wait 26 ms -- far worse than Skyroads showed while it felt broken.
     Four fixes were aimed at those numbers. The per-keystroke instrument (`msgq_ms` vs
     `deliver_ms`) settled it in ONE round: the UI thread was innocent (90% of keys handed
     over in 0 ms), and 88% of keys took >64 ms to reach the guest.
  4. **HEADLESS SKYROADS IS NOT DETERMINISTIC IN GUEST STATE.** One run showed a "23%
     clock deficit" that did not exist -- its attract loop had wandered (port 0x388 = 3.4M
     accesses vs the control's 11,920). CHECK THE HOT-PORT HISTOGRAM BEFORE COMPARING ANY
     RATE; matching runs agree to the exact count.
  5. **THE REPRO MUST REACH ACTUAL GAMEPLAY.** Two Enters parks it on LEVEL SELECT, where
     arrows only move a highlight, `no_if` reads 33% and the fault vanishes -- a clean
     null result. Three Enters starts a level and `no_if` goes to 96%. The SCREENSHOTS are
     what caught it ([[oracle-first-debugging]]).
  6. **DUMP INSTRUMENTS PERIODICALLY, NOT AT EXIT.** The STAGE2 summary only writes when
     the guest terminates; two runs produced nothing (guest still up, then a reboot). A
     5 s log dump made the next run succeed regardless of how it ended.


═══════════════════════════════════════════════════════════════════════════════
██ ★★★★★★ **DOOM IS PLAYABLE WITH SOUND.** (2026-08-25, session 25)          ██
██     Both remaining defects CLOSED. The project's stated bar is MET.        ██
═══════════════════════════════════════════════════════════════════════════════
  User verdict, by ear, on the rig: *"Still 99.999% on sound — DOOM IS PLAYABLE!"*
  Real silicon, from-scratch DPMI host, no guest patch of any kind.
```
   VIDEO  status bar FIXED (8648f41). I_ReadScreen cycles GR4 and never writes the
          map mask, so every read was served from the WRITE plane. Planes vs STBAR
          34/71/30/28%  ->  70/69/71/68%.
   AUDIO  PCM FIXED (e220033) by PACING THE PIT. host_pit_sync ran 65 times a second
          against a 140 Hz timer, so ticks came out in BURSTS: 53% within 0.5 ms of
          each other, 28% of gaps over 11.6 ms = one DMA block. DMX's mixer is ARMED
          by the SB block IRQ (next_due = NOW, 0x571b4) and SERVICED on the next
          timer tick (0x57224) -- so an over-length gap lets a second block arm
          before the first is serviced, the arms COLLAPSE into one refill, and a
          block is never filled. 32.8% measured against 30% of loud blocks stale.
          FIX = a ~1 kHz thread calling host_pit_sync. A PACING change, not a rate
          change: the 8254 still advances by real elapsed time.
```
  ⚠ **BEFORE TOUCHING AUDIO AGAIN, READ THE DEAD ENDS** — all measured, none helped:
    audio lead (`awbufs`), DMA granularity (`awframes`), moving the async injection
    out of `g_lock` (WORSE: the lock is an interlock against suspending a lock
    holder), slicing the audio lock, exec-thread priority, VDMSound's ACK gate (DMX
    acks BEFORE it refills), and a "stall unfilled blocks" gate (wrong by design --
    DMX only ever fills the block AHEAD).
  ★ **METHOD, and it is the whole session:** the video bug fell within an hour of
    DISASSEMBLING DOOM.EXE after ~20 rig runs of host instruments found nothing, and
    the audio bug fell the same way — DMX's arm/service split is not inferable from
    this side of the boundary. See [[read-the-guest-binary]]. The user's EAR caught
    three things no counter did: the Skyroads regression, "gaps not echo", and
    "repeated or mixed", each of which redirected the investigation.

  ▶▶ **OPEN, CARRIED FORWARD (see GH tickets / the two items below):**
     1. Sound is 99.999%, not 100%. Residual may relate to CPU affinity, SpeedStep,
        or other hardware grounding — untested.
     2. The Doom MELT/wipe screen may still show pixelation. Needs confirming by eye
        against the WAD oracle; `I_ReadScreen` is the wipe's source and was the
        status-bar cause, so this is plausibly a remnant of the same fault.
     3. ▶ NEXT WORKSTREAM: **mouse + keyboard EXCLUSIVITY** (see the block below).

┌──────────────────────────────────────────────────────────────────────────────┐
│ ▶▶ NEXT: INPUT EXCLUSIVITY (mouse + keyboard capture)                        │
└──────────────────────────────────────────────────────────────────────────────┘
  Three concrete requirements, from the user, 2026-08-25:
  1. **The hidden mouse cursor must become a menu option.** NTVDMEX currently hides
     the cursor when it passes over the video output because it caused lag. Make it
     configurable so the lag can be re-tested rather than assumed.
  2. **Windows swallows chords the guest needs.** Doom's `SETUP.EXE` waits for **F10**
     to accept a key configuration and never sees it. (Note SETUP.EXE also currently
     fails earlier for an unrelated reason: `INT21 AH=3Dh [DEFAULT.CFG] -> AX=5`,
     access denied — most likely the file carries the read-only attribute.)
  3. **The mouse does not work in Doom at all.**
  ► THE DESIGN: while the VDM window has focus, ALL keyboard and mouse input goes to
    NTVDMEX exclusively and Windows gets nothing — until an escape chord releases it.
    The chord must be something DOS could never produce, so no guest can be locked
    out of its own input by a game that happens to use it.

═══════════════════════════════════════════════════════════════════════════════
██ ▶▶▶ SESSION 25 (2026-08-25). ★★★★★ **THE STATUS BAR IS FIXED.**            ██
██     `I_ReadScreen` reads all four planes with GR4 and never writes the     ██
██     map mask. We served every read from the WRITE plane. Found by          ██
██     DISASSEMBLING DOOM.EXE after four runs of instruments found nothing.   ██
═══════════════════════════════════════════════════════════════════════════════

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★★ THE RESULT                                                             │
└──────────────────────────────────────────────────────────────────────────────┘
```
   plane vs STBAR (WAD oracle)   before    after     delta
     plane 0                      34.4%    69.9%    +35.5
     plane 1                      70.7%    69.1%     -1.6   (was already right)
     plane 2                      29.8%    70.7%    +40.9
     plane 3                      27.8%    67.9%    +40.1

   plane-to-plane identity      70-76%   18-30%   collapsed -> distinct
   bar_planes_equal          1709/2560  176/2560   66.8% -> 6.9% (ref ~12%)
```
  The three broken planes rose to meet the one that was always correct, and plane 1
  did not move. That is the shape of a real fix rather than a metric moving.

  ▶▶ **AND THE USER CONFIRMED IT ON THE RIG: "Doom graphics is FULLY WORKING."** A play
     session at a 240 s cap, watched live. That is the acceptance test; the oracle score
     was only ever a proxy for it.
  ★ **SO THE RESIDUAL ~30% vs STBAR IS NOT A DEFECT — DO NOT CHASE IT.** `planejudge.py`
    compares the planes against the bare `STBAR` background lump, but the live bar has
    ammo, health, keys and the face drawn OVER it. ~69-71% is close to this metric's
    ceiling, and the human check says the picture is right. A future session that sees
    "only 70% correct" and starts digging is chasing the oracle's model, not a bug.
  ⚠ The play-session log came back WITHOUT the end-of-run report (copied off the rig
    before the host finished its shutdown): no `MODEYBAR`, no `modeY` block, no
    `STAGE2: complete`. Archived as `result_doom_PLAYSESSION.log` for completeness, but
    it carries NO numbers — the numbers above are from the 45 s headless run
    `result_doom_GR4FIX.log`. If a long run's report is wanted, that copy path needs
    fixing first.

  **VIDEO IS DONE FOR DOOM. WHAT REMAINS IS AUDIO** — the PCM click at DMA block offset
  2 and the ~32% echo (a margin race, not a stall; see session 24 and TASK A: measure
  what DMX *writes* to the ring, since a poll is not a refill).

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★ THE CAUSE — READ FROM THE BINARY, NOT INFERRED                             │
└──────────────────────────────────────────────────────────────────────────────┘
  `DOOM.EXE` obj1 (LE header at file 0x27acc), routine at file offset **0x5c154**:
```
   I_ReadScreen(scr):
     mov edx,3CEh / mov al,4 / out dx,al       GC index := 4 (READ MAP SELECT)
   plane_loop:
     mov edx,3CFh / mov al,cl / out dx,al      GR4 := plane   <- THE ONLY PORT WRITE
   byte_loop:
     mov bl,[ebx+eax]                          read video memory
     mov [edx-4],bl                            scr[plane + 4*i] := byte
     cmp eax,3E80h / jl byte_loop              16000 = 64000/4
     inc ecx / cmp ecx,4 / jl plane_loop
```
  It cycles the READ plane and **never writes the map mask**. Our A0000 window holds one
  section, positioned by the WRITE mask, and a guest read is served by the mapping
  without the VDD ever seeing it — so all four passes read the SAME plane and the buffer
  comes back `scr[p + 4i] = plane_M[i]`: every four-pixel group holding one plane's byte,
  four times. **That is the collapse.**
  It is the SCREEN WIPE (`wipe_StartScreen`/`wipe_EndScreen`). The 3D view is redrawn
  every frame and heals; the status bar is repainted only where it CHANGES
  (`ST_diffDraw`), so its collapsed pixels are never rewritten — permanent damage from a
  transient event, which is exactly the shape the evidence demanded and nobody could
  find. It also explains session 22's "the wipe looked pixelated until the redraw
  cleaned up", which was recorded and never followed.

  ▶ **THE FIX** (`modey_remap_readmap`): point the window at the GR4 plane. Safe because
    Doom sets the map mask before every plane WRITE (2,029,794 mask writes against 39,975
    GR4 writes) and the pairing matrix shows the order is GR4-then-mask. Not done while
    the scratch is up. Companion change in `vdd_video.c`: with host backing a map-mask
    write now calls `ymap_select` **even when the value is unchanged**, because a read may
    have moved the window since — otherwise a store lands in the plane the last READ
    selected. Skyroads is untouched: `chain4=1`, so the branch never executes (`swaps=0`),
    and its PIT rate is identical (182.1/s vs 182.0/s).

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★ THE METHOD FAILURE THAT COST FOUR SESSIONS — READ THIS ONE               │
└──────────────────────────────────────────────────────────────────────────────┘
  ⚠⚠ **EVERY EXCLUSION WAS ABOUT WRITES, AND THE BUG WAS IN READS.** The fan-out, the
    latch bursts, the linear section, the render, the mask routing — five careful
    measurements, each sound, all of them blind to the same thing. "Every candidate
    writer is excluded and the content is still there" was read as *one exclusion is
    wrong*. It was not. **They were all correct and the category was wrong.**
  ⚠⚠ **I SPENT ~20 RIG RUNS ON INSTRUMENTS AND ZERO ON READING THE GUEST.** Doom's DOS
    video layer was never released, so I treated it as unavailable — while `DOOM.EXE` sat
    in `build/`, disassembles in twenty lines of `capstone`, and answered the question
    outright. The user called this out; that is what ended the loop.
    **When the guest's behaviour is the unknown, READ THE GUEST. The binary is on disk
    and it is the authority. Instrumenting the host can only ever describe the host.**
  ⚠ Two of my own instruments were void THIS SESSION and I found both by computing what
    they would read under the hypothesis: `cross_same` (all-32-bytes-match, p = 2.5e-6
    under the collapse) and `cross_eqb` (whole-window, so it measured STATE not delivery).
    Before trusting a counter, work out what it reads if the hypothesis is TRUE.
  ★ **AND ONE MORE WRONG BELIEF, REMOVED BY READING.** Heretic's `I_IBM.C` (the nearest
    public relative of Doom's DOS video layer) shows the write-mode-1 latch copy under
    mask 0x0F is the **DISK-FLASH ICON** — `src = currentscreen + 184*80 + 304/4`, 16
    rows, 4 bytes each, a 16x16 sprite at (304,184). Its region is 0x39cc..0x3e7f and our
    measured burst span is 0x3a1c..**0x3e7f** — the upper bound matches to the byte.
    Our own source comment claimed those 120 bursts were "Doom carrying the STATUS BAR
    between its three pages"; the COUNT was measured, the INTERPRETATION was invented,
    and it steered three sessions (it is why the latch solver exists and why band B was
    thought special). It also explains why delivering those bytes moved the oracle under
    a point: a corner icon is 2.5% of the bar.

═══════════════════════════════════════════════════════════════════════════════
██ ▶▶▶ SESSION 25 — the instrument runs that preceded the fix                 ██
═══════════════════════════════════════════════════════════════════════════════

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★ START HERE — THE 60-SECOND VERSION                                         │
└──────────────────────────────────────────────────────────────────────────────┘
  Session 24 left two candidates for what puts plane 1's bytes into planes 0, 2 and
  3: **(a)** three of Doom's four mask changes not moving our window, or **(b)** the
  guest writing one byte stream four times. It named (a) as "directly checkable and
  should be done first". One run answered both.
```
   (a) DEAD, BY AN IDENTITY RATHER THAN A RATE.
       mask writes 2,029,794 = sel_calls 1,856,424 - c4sel 1 + skip_same 173,371
                             + skip_chain4 0                     residual 0
       sel_calls   1,856,424 = swaps 1,856,424 + sel_same 0 + sel_zero 0 + failed 0
                                                                residual 0
       EVERY mask change moved the window. The window was never stranded, never
       dropped by chain4, never left where it already was. Session 24's unexplained
       8.6% is ENTIRELY the guest rewriting the mask it already had.

   (b) CONFIRMED, and it is what the previous instrument was built to test and
       could not. CROSS-PLANE agreement, PER BYTE:
                            band A 56.0%      band B 79.1%
       against ~12% for an intact bar (bandprof reference) and 66.8% for
       `bar_planes_equal`. Different planes are handed near-identical bytes.
```
  ⚠⚠ **AND THE OLD READING OF THAT INSTRUMENT WAS VOID — THE SAME MISTAKE AS SESSION
    23'S CONTROL, IN A DIFFERENT COSTUME.** `cross_same` required **all 32 bytes** of
    the window to match. The collapse implies ~67% per-byte agreement, and
    0.668^32 = 2.5e-6 — so under the hypothesis the counter should read ~0 same out of
    246, and it read 30/246. **"cross_diff dominates" is what BOTH hypotheses predict.**
    Session 24 filed it as "guest single-plane stores excluded (weak)". It was not weak
    evidence; it was no evidence. That exclusion is **withdrawn**, and with it the
    "every candidate writer is excluded" impasse — the wrong exclusion has been found,
    and it was not the render.
  ▶ **THE QUESTION NOW.** A single-bit map mask routes a store to exactly one of six
    DISTINCT sections (`modey_remap_init` calls `CreateFileMapping` once per section),
    and the identity above proves the routing happens on every mask change. **The host
    therefore cannot replicate a byte across planes.** So Doom is issuing near-identical
    stores under four different masks, which means **its blit SOURCE is already
    collapsed** — the bar is wrong in guest memory before it reaches us.
  ▶ **THE SINGLE NEXT ACTION:** score Doom's own `screens[0]` (guest RAM, readable, and
    NOT circular — it shares no code with `g_yview[]`) against STBAR. If it is collapsed
    there, every remaining question is about the guest's drawing path, not our planes.

  The strongest single number is `p1eq` in band B: planes 0/2/3 match **plane 1's**
  window at 70/74/81%, while plane 1 matches **its own previous** content at only 61%.
  The other planes resemble plane 1 more than plane 1 resembles itself a frame ago.
  Band A is the milder band (49/56/53% against a 79% self-baseline), which is the same
  A-milder-than-B ordering `bandprof.py` reports (q1 73.3% vs 88.6%). Two instruments
  built on different data agree on the intensity ordering.

  Branch `m9/completeness`. Gates green on the shipped binary: off-VM **581 checks /
  16 suites, 0 failed**, check-imports pass. Rig `192.168.1.29` up, share at
  `/tmp/xpshare`, build deployed and md5-verified (`bd75be31…`), `headless_ms.txt`=45000.
  One rig run, archived as `build/rigruns/result_doom_MASKACCT.log`.

  ★ **METHOD NOTE, AND IT IS THE THIRD TIME THIS SESSION-PAIR:** an all-or-nothing
    predicate over a wide window cannot measure a partial effect. `cross_same` (32 bytes,
    all-must-match), session 23's p0-vs-p2 control, and session 24's `% of raises` all
    failed the same way — **the number could not have come out differently if the
    hypothesis were true.** Before trusting a counter, compute what it would read UNDER
    THE HYPOTHESIS. If that is indistinguishable from what it reads under the null, the
    counter is decoration. See [[counter-layout-is-a-claim]].

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

═══════════════════════════════════════════════════════════════════════════════
██ ▶▶▶ SESSION 21 (2026-08-24). Written MID-SESSION; session 22 above continues ██
██     the same day and supersedes the "NEXT SESSION" list at its end.          ██
██     **DOOM PLAYS ITS DEMO, WITH SOUND, AND THE                              ██
██     MENU OPENS.** THE FIVE-SESSION `R_ExecuteSetViewSize` DEATH WAS OURS: ██
██     THE INT-SITE PATCHER WAS CORRUPTING A `jle` DISPLACEMENT.             ██
═══════════════════════════════════════════════════════════════════════════════

  Branch `m9/completeness`. Commits `9435485`, `46462d5`, `ebd1bd3`.
  Gates GREEN on the final binary: off-VM **629 checks / 18 suites, 0 failed**;
  check-imports pass; bare-metal `selftest.com` 8/8; `dpmitest.com` 0300/0301/0303 +
  nested INT 31h clean; `dpmiback.com` clean (its `<<< MISMATCH >>>` is the
  documented benign sentinel -- do not chase it).

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★★ WHAT WORKS NOW, WITH NO GUEST PATCH OF ANY KIND                        │
└──────────────────────────────────────────────────────────────────────────────┘
```
   startup      every line stock ntvdm prints, through I_StartupSound /
                I_StartupTimer / D_CheckNetGame / S_Init / HU_Init / ST_Init
   video        the attract DEMO renders -- full 3D, 320x200 unchained (mode Y),
                status bar, weapon, sprites. Confirmed on the physical screen.
   PCM          sb_blocks=0xe27 at 11025 Hz through the SB + DMA into the mixer,
                host_wave=open, underruns=0
   MIDI         midi_msgs=0x2d8 out of the MPU-401 to XP's GS Wavetable synth,
                host_midi=open
   keyboard     12 scancodes pushed -> p60=12 read, 0 dropped, 0 stranded.
                ESC opens the MENU over the demo; it closes again.
```
  ⚠ `pmbp.txt` IS NO LONGER NEEDED AND MUST BE ABSENT. Session 19/20's
    `03aed1fe ... 5` skip is obsolete -- it papered over the bug fixed here.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★★ THE BUG. IT WAS OURS, AND IT HAD BEEN THERE ALL ALONG.                 │
└──────────────────────────────────────────────────────────────────────────────┘

  `dpmi_patch_code_region()` rewrites `CD nn` -> BOP because a raw protected-mode
  INT is the one fault XP will not reflect. It matched the BYTE PAIR, with no idea
  where instructions start. At obj1+0x3593f Doom's code is
```
     39 fa   7e cd   31 c9      cmp edx,edi / jle -51 / xor ecx,ecx
             ^^ ^^^^^^^
```
  The `cd` is the **jle's displacement**; the `31` is the **xor's opcode**. Patched,
  `7e c4  c4 c9`: loop 2's back edge jumped to obj1+0x35905 -- the middle of a `jl` --
  and the guest ran `cmp ecx,[ebx+0x034fe02d]` with an ANGLE in ebx. Wild read, #PF at
  CPL 3, VDM torn down silently.
  Two more sites were being corrupted the same way, unnoticed: obj1+0x0ae0f (a
  `call rel32` displacement) and obj1+0x0512d (a word in a data table).

  ★ **SESSION 20'S "IT IS THE STRETCH LENGTH" IS REFUTED.** The skip "saved" the run
    because it stopped the corrupted jump being TAKEN. This session's own instruments
    killed the theory outright: a **27 ms** BOP-free stretch survived earlier in
    startup (`PMSTRETCH`), and the async injector never touched loop 2 at all
    (`ASYNC-SITE`: six distinct sites in a whole run, every one in the ms-delay spin).

  **THE FIX — `src/host/x86len.h`.** An x86 instruction-LENGTH decoder (16/32-bit) plus
  a boundary test: decode forward from each of the preceding 48 bytes, count how many
  streams land on the candidate. Patch if it is a confirmed instruction start; reject
  ONLY when a confirmed instruction covers it AND that instruction is a RELATIVE BRANCH.
  ⚠ **A STRICTER RULE WAS TRIED AND IS WRONG. DO NOT "TIGHTEN" THIS.** "Reject anything
    a confirmed instruction covers" also rejects DOS/4GW's `mov ah,30h / int 21h`
    version check, which sits right after the string `"requires DOS/16M\n\r$"`: every
    backward anchor decodes ASCII, so it scores 1 vote in 48 -- by votes alone
    indistinguishable from Doom's `jle` at 3 in 48. Refusing it left a raw INT 21h in PM
    and killed the run inside the extender's startup, 54,000 log lines EARLIER.
    ▶ **THE TWO ERRORS ARE NOT SYMMETRIC**: a spurious patch usually costs nothing, a
      missed real site is instant death. Lean toward keeping.
  ▶ The region's D/B bit is now plumbed from the descriptor. Decoding DOS/4GW's 16-bit
    modules as 32-bit rejects obvious real sites; the scan is idempotent, so a site
    refused under a wrong width gets another chance on the next pass.
  ▶ Validated against objdump over 242 candidate byte pairs in three images; pinned by
    `tools/dostest/x86len_test.c` in the off-VM battery.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★ THE OTHER FIVE, ALL FOUND BY MEASUREMENT, ALL OURS                      │
└──────────────────────────────────────────────────────────────────────────────┘
```
 1 TIMER 27x FAST      DPMI_IRQ0_BATCH injected a fixed 64 ticks per async return
                       with no reference to elapsed time: 169,032 ISR entries in 45 s
                       against the 6,300 Doom asks for at 140 Hz. g_irq0_pending
                       cannot answer "how many are owed" (it saturates at 4 on
                       purpose), so the batch drains g_pm_tick_owed now.
 2 ABANDONED ISR       dpmi_inject_pm_irq() gave up after 64 phases and restored the
   (this froze the     interrupted context, leaving the client's handler part-run.
    clock)             ONCE per run, and fatal: it leaked DMX's dispatcher depth
                       (4->3) and one 4KB frame of its private interrupt stack,
                       permanently. ticcount froze at 0x61, I_GetTime() stopped,
                       TryRunTics() spun forever -- "the title renders and nothing
                       ever happens". A PHASE IS NOT A UNIT OF TIME: it is one PM
                       entry, and the MIDI driver pays ~11 per byte written. Bound by
                       WALL CLOCK (500 ms); log loudly if we ever stop early.
 3 "SB isn't           The card answered its reset with 0xAA and reported DSP 4.05.
   responding"         What failed was DSP command 0xF2 -- "assert your interrupt so
                       I can find your line". async_inject_irq() decided "has the
                       guest hooked this line?" from the REAL-MODE IVT only, and a
                       DPMI client hooks the PM vector. Ask both tables.
 4 NO PM KEYBOARD      IRQ0 has had a cooperative PM path since #2b; IRQ1 had only
   PATH                the async injector, which gets ONE attempt per keystroke.
                       The PM loop now offers a pending IRQ1 every pass.
 5 PENDING CLAIMED     ...and the count was decremented AFTER the handler ran, while
   TOO LATE            the handler itself re-raises (the ISR reads 0x60 from inside
                       the injection and the 8042 re-asserts). That cancelled the
                       interrupt the ISR had just raised: three bytes delivered,
                       scleft=1, keyboard dead for the rest of the run. Claim first.
```

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★ NEW INSTRUMENTS — every one of them earned its place this session        │
└──────────────────────────────────────────────────────────────────────────────┘
```
   ASYNC-SITE     each distinct PM CS:EIP the async injector finds the CPU at, ONCE.
                  The guest's timer runs at 16 kHz, so a per-attempt line is
                  unreadable and a capped one goes quiet before the interesting part;
                  deduping by SITE is neither. The observation pass RESUMES the thread
                  and returns WITHOUT injecting, so the line is on disk before
                  anything is rewritten.
   PMSTRETCH      wall-clock of each dpmi_enter_pm(), new maxima only. This is what
                  refuted "the stretch is too long".
   SNDIO          the sound-card conversation (SB / OPL / MPU), first 300 accesses.
                  ⚠ io_hot_note() is only called on the V86 arm, so STAGE2's "hot
                    ports:" is EMPTY for a PM client. It is not a measurement.
   KEYIRQ/KEYPM   every keyboard IRQ raise and every PM delivery attempt, with all
                  five gate values. Bounded; a run with no keys pays nothing.
   STAGE2 sound   sb_blocks/sb_rate/midi_msgs/mpu_uart AND whether the HOST wave and
                  MIDI devices opened -- a silent run with a happy guest and a silent
                  run with no device look identical from the guest's side.
   pmwatch.txt    up to 4 linear addresses, dumped on every IRQ0<-PM line. This is
                  what caught the frozen clock. Addresses came straight out of Doom's
                  disassembly: I_GetTime is `mov eax,ds:0x2913c / ret`, DMX keeps its
                  dispatcher depth at 0x283e8 and its stack at 0x283f0 (obj3-relative).
   capture.flag   its CONTENTS are now the screenshot period in ms. 40 frames at the
                  old fixed 300 ms only saw the first TWELVE SECONDS, so a run whose
                  first scripted key is at 14 s looked like "the keys did nothing".
   keys.txt       scripted scancodes for the synthetic-key thread (qimode bit 5,
                  `printf '20' > qimode.txt`). Format: `w<ms>` waits, `e` = extended
                  prefix, two hex digits = a make/break pair.
                  e.g. `w15000 01 w3000 e50 w2000 e50 w2000 e48 w6000 01 w5000`
```

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★ METHOD — SESSION 21 PAID FOR THESE                                       │
└──────────────────────────────────────────────────────────────────────────────┘

  ▶ **THE CALLED-OFF SUSPECT WAS THE CULPRIT'S NEIGHBOUR.** Five sessions looked for a
    fault in Doom because the death was *inside Doom's code*. Nobody asked what WE had
    written into that code. **When a guest dies at an address, diff the bytes there
    against the file on disk.** One `DPMI-BP: armed at ... (displaced 7e c4)` line --
    where DOOM.EXE says `7e cd` -- was the whole answer, and it was free.
  ▶ **TWO REPEATING BREAKPOINTS IN A LOOP RE-ARM EACH OTHER.** Session 20 recorded that
    `rep` cannot re-arm inside a BOP-free stretch and reached for `skip` instead. True
    for ONE breakpoint (the re-arm refuses while the guest stands on the footprint);
    false for two, which alternate. That is what made the loop-2 probe possible.
  ▶ **A COUNTER THAT SATURATES CANNOT BE A BACKLOG**, and a fixed batch is not catch-up.
  ▶ **NEVER ABANDON A GUEST HANDLER MIDWAY.** It is not a timeout, it is a silent state
    corruption, and the damage surfaces thousands of events later somewhere else.
  ▶ **AN INSTRUMENT THAT HARDCODES WHAT IT IS REPORTING IS WORSE THAN NONE.** ASYNC-PM
    printed "vec=0x08" literally, whatever it had delivered, so the log could not be
    asked whether a key had ever arrived.
  ▶ **CLAIM BEFORE YOU RUN, NOT AFTER.** Anything the handler can re-raise must be
    consumed before the handler runs, or the handler's own request is cancelled.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★★ NEXT SESSION: START HERE                                               │
└──────────────────────────────────────────────────────────────────────────────┘

  **1. PLAY IT.** The bar is a HUMAN at the keyboard: new game, move, shoot, use.
     Everything measured so far is scripted keys against the demo. The known-unknowns
     are held keys (typematic -- see the standing KEYBOARD section, still open) and
     whether the 1-in-3 long-run flake from session 19 survives the fix (it should not;
     it was this bug, but it has not been measured over enough runs to say so).
  **2. SOUND FIDELITY, NOT SOUND PRESENCE.** PCM and MIDI are flowing and the host
     devices are open; nobody has LISTENED yet. Ask the user. The OPL synth is unused
     here (`opl: writes=0`) because Doom picks the MPU-401 when it answers -- if FM
     music is wanted, that is a device-selection question, not a synth question.
  **3. THE CRTC (0x3D4/0x3D5) IS STILL UNCLAIMED** -- `STAGE2: unclaimed ports touched`.
     Page flipping is inferred from the data (`modey_page`) instead of read from the
     register, and claiming it has regressed Doom three times for reasons still
     UNKNOWN. Now that the real killer is gone, retry it -- gated, three runs.
  **4. `dpmi_invoke_callback()` STILL HAS A BARE 64-PHASE CAP** (search `ph < 64`).
     It is the same shape as the bug that froze the clock. Fix it before it bites.

  ⚠ **RIG STATE.** `qimode.txt`, `keys.txt`, `capture.flag`, `pmbp.txt` and `pmwatch.txt`
    were all REMOVED before the gate runs. A stale one silently alters every later run.
  ⚠ Every write to `/tmp/xpshare` needs `dangerouslyDisableSandbox`; a sandboxed write
    FAILS SILENTLY and you then read a stale log.
  ▶ Evidence archive + exact repro: **`docs/research/doom-int-site-patch/`** (three
    gzipped runs and the menu screenshot; `build/` is gitignored).

═══════════════════════════════════════════════════════════════════════════════
██ ▶▶▶ SESSION 20 (2026-08-24). ⚠⚠ ITS CENTRAL CONCLUSION IS **REFUTED** BY   ██
██     SESSION 21 -- the death was not stretch LENGTH, it was our own INT-site ██
██     patcher corrupting a `jle`. Kept for the landmarks and the rig notes,   ██
██     which are still good. DO NOT act on the "find the threshold length"     ██
██     plan below; it was chasing a symptom.                                   ██
═══════════════════════════════════════════════════════════════════════════════

  Branch `m9/completeness`. Gates were run on `d2256cd` FIRST, as session 19 asked.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★ GATES ON `d2256cd` -- ALL GREEN (the session-19 open item, now closed)    │
└──────────────────────────────────────────────────────────────────────────────┘
```
   off-VM battery      580 checks / 16 suites, 0 failed   <- the corrected figure, verified
   check-imports.sh    pass (all imports XP-shipped)
   selftest.com        8/8 PASS   (and chain4=01 there: mode-Y did NOT disturb chained modes)
   dpmitest.com        0300/0301/0303 + nested INT 31h OK, clean exit
   dpmiback.com        real<->PM round trip, exact expected output
```
  ⚠ `dpmiback`'s `<<< MISMATCH >>>` is BENIGN: that line tests a sentinel `0x005A` at
    guest linear 0x1600 that only *dpmitest* writes. Not a regression. Do not chase it.
  ▶ The binary built from HEAD was byte-identical (sha `3c1629e7`) to the deployed
    d2256cd host, so the gates really did measure the working-render build.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★★ THE FINDING: SHORTEN THE LOOP AND THE VDM SURVIVES                     │
└──────────────────────────────────────────────────────────────────────────────┘

  `R_ExecuteSetViewSize` calls `R_InitTextureMapping` (obj1+0x35870). Its **loop 2**
  is Doom's `for (x=0;x<=viewwidth;x++) { while (viewangletox[i]>x) i++; ... }`.
  Watcom compiled it resetting `i` each x, so it is **289 x ~3073 = ~890k inner
  iterations, ~3.5M instructions with NO BOP** -- the first such stretch in the whole
  program. Everything before it is dense with INT 21h/31h, so the host gets constant
  cooperative turns; here it gets none.

  **THE EXPERIMENT.** `pmbp.txt` breaking on loop 2's BACK-EDGE and skipping it:
  ```
     03b0593f 0 2        # obj1+0x3593f = "jle 0x3590e", 2 bytes -> exit after ONE x
  ```
  ```
     no skip (control, same binary)   VDM torn down, NO STAGE2 block at all   6 / 6
     loop 2 shortened to one pass     clean wind-down, "STAGE2: complete"     2 / 2
  ```
  Nothing else changed. **The arithmetic is not the bug; the uninterrupted LENGTH is.**

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★ WHAT WAS REFUTED -- all by evidence, all cheap, do not re-run these      │
└──────────────────────────────────────────────────────────────────────────────┘
```
   "the BSS tables are unallocated"  obj3 is 0501'd BX:CX=0x86000 at 0x03b40000;
                                     the highest write is 0x5c89c. Comfortably inside.
   "finetangent[] is corrupt"        dumped finetangent[3072] = 0x00010032 (~1.0007
                                     in 16.16). Exactly right. focallength=0x008fe3e5.
   "viewangletox[] never terminates" dumped: starts 0x121 (=viewwidth+1=289), ends
                                     all 0xffffffff (-1). Monotone, terminates.
   "the Sound Blaster IRQ kills it"  nosb.flag -> IDENTICAL death, same last line.
   "the async injector kills it"     with early bails now logged: ZERO async attempts
                                     of ANY kind across the death window.
   "pmnoirq proves it"               INCONCLUSIVE, not a refutation: without ticks
                                     Doom wedges at I_StartupTimer() and never
                                     reaches D_Display at all.
```

┌──────────────────────────────────────────────────────────────────────────────┐
│ ⚠⚠⚠ SESSION 19'S BRACKET WAS WRONG, AND THE "INSTRUMENT LIES" WAS PARTLY US   │
└──────────────────────────────────────────────────────────────────────────────┘

  Session 19 bracketed the death to "a 157-byte window of movs that cannot fault".
  **That window was never executed.** At obj1+0x35b44 `test edi,edi / jne 0x35b7b`
  branches on `detailshift`, which is **0**, so the fall-through runs 0x35b48..0x35b79
  and jumps to 0x35bad. The probe at **0x35b8f sits in the not-taken branch -- DEAD
  CODE.** "NOT HIT" meant "never reached", not "died before here". The contradiction
  ("nothing here can fault") dissolves: nothing there RAN.

  And the breakpoint mechanism is **one-shot BY DESIGN** -- `dpmi_bp_disarm()` on hit
  unless `rep`/`skip` is set. So "7 armed, 1 hit" is not necessarily a lie.
  ⚠ `rep` does NOT work inside a BOP-free stretch: re-arming happens in
    `dpmi_bp_rearm_pending()`, which needs the host to get a turn -- exactly what
    such a stretch denies. A rep BP in loop 2 fires ONCE. Use `skip`, which is
    applied at the hit itself, when you need to alter a loop.

  ▶ **ALWAYS DISASSEMBLE AROUND A PROBE BEFORE BELIEVING "NOT HIT".** Check it is on
    an instruction boundary AND on a path the data actually takes.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★ THE DEATH IS KERNEL-DRIVEN. That question is now settled.                 │
└──────────────────────────────────────────────────────────────────────────────┘

  Every host exit path logs BEFORE it goes: the watchdog prints "watchdog terminating
  (wedged)", the VEH prints "DPMI FATAL", headless prints "deadline reached".
  **None of the three appears.** `veh{any=0 fatal=0}` for the entire run, and the log
  ends on a COMPLETE line (\r\n), not a torn write. So the process did not exit through
  any path of its own: **the kernel tears the VDM down**, exactly as session 16 saw.

  ▶ NEXT: why does a long BOP-free PM stretch make XP kill the VDM? Candidates, in the
    order worth testing -- an interrupt going pending with nowhere to be delivered (the
    PMKERNEL comment's point: POPFD at CPL 3 cannot set VIF, and the kernel's delivery
    gate reads VIF), or a quantum/APC interaction with the far-jmp PM thread. The
    `skip` lever gives a CHEAP DIAL: vary how short loop 2 is cut and find the
    threshold length at which death starts. A threshold in INSTRUCTIONS points at the
    guest; one in MILLISECONDS points at the kernel's timer. **Measure the threshold
    before theorising further.**

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★ LANDMARKS — the address arithmetic every probe needs. Get this wrong and │
│     you dump code bytes and "discover" nonsense (session 19 did exactly that).│
└──────────────────────────────────────────────────────────────────────────────┘
```
   obj1 (CODE) base 0x03AD0000     0501 BX:CX=0x00045000   [LE CODE OBJECT] in the log
   obj3 (DATA) base 0x03B40000     0501 BX:CX=0x00086000   <- 548,864 bytes, covers ALL BSS
   linear = base + offset. Disassembly operands are ALREADY obj3 offsets (unrelocated
   file values that the loader fixes against the DATA object), so [0x38fe0] -> obj3+0x38fe0.
   ⚠ Both bases were STABLE across all 11 runs this session. Re-check them in the log
     before trusting a probe address anyway -- one 0501 line, it costs nothing.

   CODE (obj1+off / linear)                  DATA (obj3+off / linear)
     0x1d1e0 / 03aed1e0  D_Display             0x32304 / 03b72304  viewheight   = 144
     0x1d1fe / 03aed1fe  call R_ExecSetViewSz  0x32308 / 03b72308  scaledviewwidth=288
     0x35a70 / 03b05a70  R_ExecuteSetViewSize  0x3230c / 03b7230c  viewwidth    = 288
     0x34e10 / 03b04e10  R_InitBuffer          0x38fe0 / 03b78fe0  setblocks    = 9
     0x35870 / 03b05870  R_InitTextureMapping  0x38fe4 / 03b78fe4  setdetail    = 0
     0x358fa / 03b058fa  end of loop 1  (HITS) 0x38ff8 / 03b78ff8  detailshift  = 0
     0x35924 / 03b05924  loop 2 outer body     0x34fe0 / 03b74fe0  viewangletox[0] = 289
     0x3593f / 03b0593f  loop 2 BACK-EDGE      0x39020 / 03b79020  xtoviewangle[]
     0x35943 / 03b05943  after loop 2 (NEVER)  0x01a84 / 03b41a84  finetangent[]
     0x1d180 / 03aed180  FixedDiv              0x04a84 / 03b44a84  finetangent[3072]
```

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★★ NEXT SESSION: START HERE                                               │
└──────────────────────────────────────────────────────────────────────────────┘

  **1. FIND THE THRESHOLD LENGTH.** This is the one measurement that splits the
  remaining hypotheses, and nothing should be built before it.
  ```
     a threshold in INSTRUCTIONS  -> the guest is doing something illegal eventually
     a threshold in MILLISECONDS -> the kernel's timer/quantum is killing the VDM
  ```
  ▶ **The back-edge skip is a 2-point dial only** (full 3.5M = dies; one pass ≈ 12k =
    survives), because a `skip` re-arms via `dpmi_bp_rearm_pending()`, which needs a
    host turn the stretch never grants -- so it fires ONCE and ends the loop. For a
    CONTINUOUS dial use Doom's own `screenblocks`: loop 2 costs
    `(viewwidth+1) x ~3073` and `viewwidth = screenblocks*32`. Write a `DEFAULT.CFG`
    into `C:\DOOMS` with `screenblocks 3` … `11` and the stretch sweeps ~300k…~990k
    instructions with **no host change and no guest patch** — a real user setting, so
    nothing about the run is artificial. Find where death starts.
  ▶ Timing needs a clock in the log: BP-HIT lines print `after NNNN svc`, not a time.
    Add `GetTickCount()` to the BP HIT and ASYNC lines before sweeping, or the
    milliseconds answer is unobtainable.

  **2. THEN ask why the kernel kills it.** Leading candidate, from the `PMKERNEL_PATH`
  commentary already in `main.c`: an interrupt goes pending with nowhere to land —
  **POPFD at CPL 3 cannot set VIF, and the kernel's delivery gate reads VIF**, so an
  in-process far-jmp PM guest can never be handed a hardware interrupt. The async
  SuspendThread injector exists to work around exactly that, and it is measurably
  ABSENT across the death window. `pmkernel.flag` (run PM under the kernel monitor
  instead of the far-jmp path) is the opt-in lever built for this question.

  **3. STILL OWED FROM SESSION 19: confirm the render is stably correct.** Needs the
  `pmbp` skip `03aed1fe 00000000 00000005` plus `capture.flag`, several runs, eyes on
  the physical screen. Not attempted in session 20.

  ⚠ **WHAT "SURVIVES" MEANT, PRECISELY.** With loop 2 cut to one pass, `xtoviewangle[]`
    is left almost entirely unfilled, so Doom exits a few hundred ms later
    (`run_ms=0xf72`, `plane-nonzero` all zero — **nothing was drawn**). The result
    measured is "**the VDM was not torn down**" (`STAGE2: complete` present, all three
    host exit paths silent), **NOT** "Doom rendered". Do not upgrade that claim.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ▶ EVIDENCE ARCHIVE + EXACT REPRO (session 20)                                │
└──────────────────────────────────────────────────────────────────────────────┘
```
   build/doomlogs/result_doom_004351.log   probe @0x35bbd -> HIT; finetangent dump
   build/doomlogs/result_doom_004622.log   probe @0x35943 -> NOT hit (after loop 2)
   build/doomlogs/result_doom_004651.log   probe @0x358fa -> HIT (end of loop 1)
   build/doomlogs/result_doom_004821.log   viewangletox TAIL dump (all -1)
   build/doomlogs/result_doom_004906.log   pmnoirq: wedges at I_StartupTimer -- INCONCLUSIVE
   build/doomlogs/result_doom_005042.log   rep BP in loop 2: fires ONCE (rep cannot re-arm)
   build/doomlogs/result_doom_005229.log   raised bail cap: still zero async near death
   build/doomlogs/result_doom_005514.log   nosb.flag: IDENTICAL death
   build/doomlogs/result_doom_005631.log   ★ loop-2 skip -> STAGE2: complete (run 1/2)
   build/doomlogs/result_doom_005921.log   ★ CONTROL on the same binary -> death
   build/doomlogs/result_doom_010039.log   ★ loop-2 skip -> STAGE2: complete (run 2/2)
```
  ⚠ **`build/` IS GITIGNORED, so those 11 logs (48MB) are LOCAL ONLY** and do not
    survive a clean or a fresh clone. The three ★ runs — the experiment/control pair the
    whole conclusion rests on — are therefore COMMITTED, gzipped (~190KB each), at
    **`docs/research/doom-loop2-stretch/`**, with a README that states the claim, how to
    read the logs, and what "survives" does and does not mean. If you need one of the
    other eight, re-run it; the recipe is below.
  Reproduce (sandbox MUST be off for share writes -- see RIG NOTES):
  ```
     printf '03b0593f 0 2\r\n' > /private/tmp/xpshare/pmbp.txt      # the survival case
     ARCHIVE=build/doomlogs TIMEOUT=300 ./scripts/bmqueue.sh doom
     rm -f /private/tmp/xpshare/pmbp.txt                            # ALWAYS clean up
     # verdict: grep -c 'STAGE2: complete'  ->  1 = survived, 0 = VDM torn down
  ```
  Commits: `1099104` (early-bail instrumentation + `scripts/bmqueue.sh`),
  `16ef457` (this analysis). Gates re-run GREEN after the instrumentation change:
  580/16 off-VM, check-imports, and bare-metal `selftest.com` 8/8 on the new binary.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ▶ RIG NOTES (session 20)                                                     │
└──────────────────────────────────────────────────────────────────────────────┘
```
   scripts/bmqueue.sh   NEW. Queues a target and waits on the RESULT LOG'S MTIME.
                        ARCHIVE=build/doomlogs archives each result -- doomrun.bat
                        overwrites ONE filename, which destroyed the 52MB reference run.
   sandbox              writes to /private/tmp/xpshare are DENIED by the tool sandbox.
                        Rig commands need it disabled; a blocked write looks like a
                        240s timeout, not an error.
   ICMP                 the box does NOT answer ping. Liveness = watcher.txt's mtime.
   headless_ms.txt      45000 on the share. Deliberate, standing, leave it.
```
  ⚠ **STILL NOT DONE from session 19: the render was NOT re-confirmed stable.** Every
    Doom run this session was a no-skip fault hunt with no graphics. The ~1-in-3
    flake remains unmeasured.

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

═══════════════════════════════════════════════════════════════════════════════
██  STANDING REFERENCE — rig operations, instruments, landmarks, older traps.  ██
██  Everything below is still true; the narrative history has been pruned.     ██
██  ⚠ "this session" below means SESSION 17 or 15 -- NOT 18, NOT 19.            ██
═══════════════════════════════════════════════════════════════════════════════
┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★ FOUR DEAD ENDS, EACH RULED OUT BY MEASUREMENT. DO NOT RE-SPEND A SESSION. │
└──────────────────────────────────────────────────────────────────────────────┘

  1. **`CLI`/`STI` in PM are FINE.** `tools/dostest/pmfault.asm` settles the family in
     twenty seconds:
     ```
        IN AL,0x21   SURVIVED   (arrives as VDM_EVENT_IO -- the CONTROL CASE)
        STI          SURVIVED
        CLI          SURVIVED
        INT3         DIED
        HLT          DIED
     ```
     A previous handoff named CLI/STI as the blocker and GH #18 as the critical path.
     **That was wrong** (see the method lesson below).
  2. **IOPL cannot be raised.** Setting IOPL=3 in `VTIB_EFLAGS_PM` does nothing — the
     kernel STRIPS it (live EFLAGS across a whole run: 0x…0296/0292/0206/0202/0246,
     bits 12-13 never set). `NtSetInformationProcess(ProcessUserModeIOPL)` is worse: at
     CPL <= IOPL the I/O permission bitmap is BYPASSED, so guest `IN`/`OUT` would reach
     real hardware instead of our VDDs.
  3. **`AX=FF00` failing is a RED HERRING.** It is the last service before the run ends,
     which is exactly why it looked causal. Its caller settles it: the code after the
     call is `testb $0x1,0x347e / jne / jmp` — a MEMORY flag. **FF00's CF is never
     tested.** Its handler services it internally and never chains, so it is not a
     missing host service either.
  4. **`DOOM.ETX` is NOT an error signal.** DOS/4GW opens `<program>.ETX` — its
     error-TEXT file — during normal startup, before the DPMI switch.

  ▶ **GH #18 IS STILL WANTED** (`INT3`/`HLT` still kill the VDM, so a raw PM trap is
    undeliverable) but it is **not** what stands between us and Doom.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★ THE METHOD LESSON, AND IT IS THE EXPENSIVE ONE                           │
└──────────────────────────────────────────────────────────────────────────────┘

  I committed a handoff naming CLI/STI as the blocker on the strength of (a) a
  breakpoint that fired on an `STI` and (b) a skip test that moved the death by one
  byte. Both were *consistent* with "STI kills us"; **neither was evidence for it.** The
  control case (`IN`, already known to reflect) would have caught it in one run, and I
  only built the probe AFTER writing the wrong conclusion down.

  ▶ **BUILD THE INSTRUMENT THAT CAN SAY NO BEFORE YOU WRITE DOWN A YES.**
  ▶ Corollary, learned three times this session: **an instrument that faults kills the
    run it exists to observe.** `IsBadReadPtr` (faults on purpose), the breakpoint
    footprint eating a call target, `dpmi_bp_arm()` reading unmapped memory.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★ THE INSTRUMENTS. USE THEM BEFORE THEORISING.                              │
└──────────────────────────────────────────────────────────────────────────────┘

  **`pmbp.txt` on the share — guest breakpoints.** One line per breakpoint; absent file
  = zero cost. Up to 32.
  ```
    <hex LINEAR addr>  [dump addr]  [skip bytes]  [mode]  [repeat]   # comment
    00018d62 00005ca0                      # break, and dump that memory on hit
    00011ad2 00000000 00000001             # break, then STEP OVER 1 byte
    00011ad2 00000000 00000000 00000001    # plant a 1-byte INT3 instead of the BOP
    0000a8b4 00000000 00000000 00000000 00000001   # REPEATING (re-arms)
  ```
  * one-shot by default — right for "how far did it get", useless in a loop;
  * **repeat** re-arms via a pending flag once the guest is off the footprint, so put
    **at least two** repeating breakpoints in a loop and they alternate;
  * **skip** turns a breakpoint into a one-instruction patch ("would it survive if this
    simply did not happen?");
  * **mode 1** plants `CC` (INT3) — one byte, the only thing that fits over CLI/STI.
  ⚠ **A BREAKPOINT HAS A TWO-BYTE FOOTPRINT.** It displaces the byte AFTER the one you
    name. One placed on a `c3` (ret) ate the entry point of a routine called two
    instructions earlier; `call` landed on the second half of our BOP, decoded as
    `LES DX,[BX+0x8b]`, read past the segment limit and killed the VDM — and the log
    presented that as the CLIENT's death, mid-bisection. Overlaps are now refused and the
    footprint is printed. **Put breakpoints on instructions of at least two bytes.**
  ⚠ **DELETE `pmbp.txt` WHEN DONE** — a stale one silently alters every later run.

  **`tools/dostest/mkbp.py <carve.bin> <off> [--count N] [--base L] [--carve-off O]`**
  writes a sweep from a disassembly, skipping one-byte instructions.

  **`tools/dostest/pmfault.asm`** builds five ~350-byte clients — `pmfsti` / `pmfcli` /
  `pmfint3` / `pmfhlt` / `pmfin` — that enter PM, execute ONE privileged instruction and
  print a verdict. Twenty seconds against Doom's forty. **`pmfin` is the CONTROL CASE on
  purpose:** without it, "nothing happened" cannot distinguish a real wall from a broken
  test. Selected at ASSEMBLY time because `rt.bat` launches targets with no arguments.

  **Other knobs on the share:** `pmverbose.flag` (per-event checkpoint dump: 8 → 0x100000
  — the firehose, needed to find a last-known position), `pmnoirq.flag` (above).
  `LOG_MAX_BYTES` is now **32 MB** (was 4 MB, which truncated mid-startup).

┌──────────────────────────────────────────────────────────────────────────────┐
│ CLIENT LANDMARKS ALREADY MAPPED (save yourself the bisection)                │
└──────────────────────────────────────────────────────────────────────────────┘

  ```
    mod:0x84    PM interrupt dispatch TABLE: `call <common> ; db <vector>` per entry
    mod:0x550   the common dispatcher -- begins `LAR eax,SS` + `bt eax,22`, i.e. it
                tests the D/B bit of OUR stack descriptor to size its frame
    mod:0x4b60  the generic INT 21h thunk (register block in, `int 21h` at 0x4b7f,
                results written back; epilogue 0x4b81..0x4ba7 ends `retf`)
    mod:0x4ce   the 16->32 GATEWAY: cli / load 32-bit SS:ESP / jmp 0x691
    mod:0x691   ... rep movsl the frame, `mov ss,bx`, `mov esp,ebp`
    mod:0x6d5   the `IRETD` into the application  (entered 281 times)
    mod:0x4eb0  the AX=FF00 wrapper (`push 0xff00` at 0x4ed5, returns at 0x4edd)
    obj2:0x745  extender startup tail -> 0x797 -> 0x7a6 -> `jmp 0x823`
    obj2:0x823  `lcall <mod>:0x4ce`  = the call into 32-bit code
    obj2:0xb00a the FF00 caller's return point
  ```
  The unwind after the final FF00 (all breakpointed and hit, in order):
  `mod:0x4b81 → mod:0x4bc5 → mod:0x4edd → obj2:0xb00a → obj2:0x745`.

┌──────────────────────────────────────────────────────────────────────────────┐
│ 0. RESTARTING THE RIG (it is healthy and SELF-RECOVERABLE as of session 16)  │
└──────────────────────────────────────────────────────────────────────────────┘

  Every LAN command below needs `dangerouslyDisableSandbox` (the rig is not in the
  sandbox's allowed-hosts list, so a plain probe returns a FALSE "down").

  ```
  nc -z -w 3 192.168.1.29 445                                  # .29 is stable
  mkdir -p /tmp/xpshare
  mount_smbfs -N //guest@192.168.1.29/ntvdmex /tmp/xpshare     # remount after a reboot
  ```

  ★ **IS THE WATCHER ALIVE?** Do NOT test by "does watcher.txt exist".
    `runwatch.bat` writes it ONCE before `:loop` and again every ~3 s inside the loop.
    So the signature is:
      `watcher.txt` mtime ADVANCING + `controld.txt` advancing  → healthy
      `watcher.txt` FROZEN + `controld.txt` advancing           → **WATCHER DEAD**
    (frozen-but-controld-beating is exactly what a broken `goto` looks like; that is
     how session 16 found the LF-only Startup copy.)
    ```
    stat -f '%Sm %N' -t '%H:%M:%S' /tmp/xpshare/watcher.txt /tmp/xpshare/controld.txt
    ```

  ★★ **IF THE WATCHER IS DEAD, YOU CAN NOW FIX IT REMOTELY** — this was impossible
     before session 16 and cost a physical trip to the box:
    ```
    printf 'exec cmd /c "C:\Documents and Settings\All Users\Documents\ntvdmex\bm\runwatch.bat"\r\n' \
      > /tmp/xpshare/control.txt
    ```
    `controld` gained a generic `exec` (scripts/bm/controld.c). Running `bm\runwatch.bat`
    also re-installs a correct CRLF copy to Startup, so it repairs the cause too.

  **REBOOT** — two INDEPENDENT channels, on purpose, so each can recover the other:
    ```
    printf 'reboot\r\n' > /tmp/xpshare/control.txt      # via controld
    printf 'reboot\r\n' > /tmp/xpshare/cmd.tmp && mv ... cmd.txt   # via the watcher (rt.bat arm)
    ```
    ~75 s, then **umount + remount** (the mount goes stale across a reboot).

  **DRIVE A TEST — write `cmd.txt` ATOMICALLY, never with a plain `>`:**
    ```
    printf 'selftest.com\r\n' > /tmp/xpshare/cmd.tmp
    mv /tmp/xpshare/cmd.tmp /tmp/xpshare/cmd.txt
    ```
    Targets: a file in `bm\tests\`; a DIRECTORY on the share root = a "game" (runs
    `<name>.EXE`); `stock <target>` = STOCK NTVDM oracle; `doom` = C:\DOOMS\DOOM.EXE;
    `reboot`. `headless_ms.txt` caps a run (max 600000). Guest console output IS
    captured into the host log, so probes need no screen-reading.

  ⚠ **`cmd.txt` DISAPPEARING MEANS THE RUN *STARTED*, NOT FINISHED.** `runwatch.bat`
    deletes it BEFORE invoking rt.bat. I misread this twice in session 16 and read a
    half-written log. **The completion signal is `result_<target>.log` SIZE GOING
    STABLE** (poll it; 3 s apart, twice equal).

  ⚠ **DEPLOY THE RIGHT EXE.** `build/ntvdmhost.exe` (~420 KB) is the host;
    `build/ntvdmex.exe` (~20 KB) is the launcher and deploying it self-relaunches and
    leaves runs "succeeding" on a STALE log. Verify:
    ```
    cp build/ntvdmhost.exe /tmp/xpshare/bm/ntvdmhost.exe
    md5 -q build/ntvdmhost.exe /tmp/xpshare/bm/ntvdmhost.exe    # must match
    ./scripts/check-imports.sh build/ntvdmhost.exe              # XP-safe (no UCRT)
    ```

  ⚠ **`bm\rt.bat` EDITS NEED A REBOOT.** Only `runwatch.bat` at startup copies it to
    `C:\WINDOWS\rt.bat`, which is what the watcher actually runs.

  ⚠ **SMB attribute caching lies.** `stat` reported a log unchanged right after a run
    that HAD rewritten it; `ls -lt` on the directory showed the truth.

  ⚠ **Never edit a Windows .bat with Python text mode on macOS** — it strips CRs and
    `cmd.exe` then fails on `goto`/labels, i.e. the watcher loop. Read/write BINARY and
    normalise to CRLF, then confirm with `file`.

  BASELINE AFTER ANY RESTART: `selftest.com` → **8/8 PASS** (re-verified end of
  session 17, twice, with the final build).

  ★ **DISASSEMBLING THE CLIENT IS NOW A LOCAL, OFFLINE OPERATION** — this is what made
    session 17 short, and it costs one command to set up:
    ```
    printf 'exec cmd /c copy "C:\DOOMS\*.EXE" "C:\Documents and Settings\All Users\Documents\ntvdmex\doombin\"\r\n' \
      > /tmp/xpshare/control.txt
    ```
    Then map a guest address to a file offset by SEARCHING for bytes the log already
    dumped (`bytes@cs:eip=`), which pins the whole segment in one step:
    ```
    python3 -c "d=open('DOOM.EXE','rb').read(); print(hex(d.find(bytes.fromhex('919 8c3b0ff...'.replace(' ','')))))"
    # DOS/4GW's 16-bit half: guest 0x0F:off == file 0x1DD0+off
    # its runtime-loaded PM module: guest 0x8F:off == file 0xF384+off
    i686-w64-mingw32-objdump -D -b binary -m i8086 --start-address=0x... carved.bin
    ```
    ⚠ Pick a pattern with **no relocated immediates** in it — `mov di,<selector>` differs
      between file and memory, and a longer "safer" pattern that includes one will simply
      not be found.

┌──────────────────────────────────────────────────────────────────────────────┐
│ 2. NEW HARNESS — STOCK NTVDM IS NOW A FIRST-CLASS ORACLE (closes #26's gap)   │
└──────────────────────────────────────────────────────────────────────────────┘

  #26 left stock ntvdm unwired ("needs an rt.bat variant that drops the IFEO
  Debugger key, and a decision on the display-wedge risk"). Both done:
    • `bm\rt_stock.bat` — drops the IFEO Debugger value, runs the target, **restores
      it on every exit path** and writes `stock_state.txt` with `reg query` output
      proving it. Leaving that key absent is the failure worth fearing: every later
      test would silently measure stock ntvdm while the logs looked plausible.
    • `rt.bat` now dispatches `stock <target>` → `rt_stock.bat`, so the oracle is
      drivable **remotely**, like any other test.
    • Output is redirected to a file (`result_stock_<target>.txt`) — there is no
      host log under stock, and the window closes the instant the program exits,
      which is exactly how the first attempt lost its results.
  ▶ Display-wedge risk applies to GRAPHICS targets. Text-mode probes carry none.
  ▶ **This is the answer to "what does real DOS actually do?" for everything from
    here on.** It settled the keyboard question in one run after I had produced
    three wrong hypotheses from reasoning.

┌──────────────────────────────────────────────────────────────────────────────┐
│ 3. KEYBOARD — PINNED BY THE USER, IMPROVED 1-in-10 → 1-in-5, NOT FIXED        │
└──────────────────────────────────────────────────────────────────────────────┘

  Symptom: crash in Skyroads holding UP; the restarted level should accelerate, as
  it does on real DOS. **Ruled out by measurement:** the scancode FIFO (3205 pushes,
  ZERO drops, peak depth 6 of 32) and lock contention (max wait 0.57 ms, hold
  0.80 ms). Root cause was architectural — we modelled an AT keyboard but
  outsourced its REPEAT to Windows' message queue, and the UI thread stalls up to
  857 ms. We now generate typematic ourselves, pumped from both threads.

  ▶▶ **SESSION 16 — DO NOT JUST RETUNE THE RATE; THE MEASUREMENT IS AIMED WRONG.**
   • **Skyroads never programs its own typematic**: a 30 s run measured
     `kbd 8042: writes=0 typematic_set=0`. So OUR generated rate is the only source.
     (Caveat: that run only covered the ATTRACT LOOP — `int16=[0,0,0,0]`, `p60=0` —
     so it does not rule out a `0xF3` once gameplay starts.)
   • **`tymat.com` measures the INT 16h PATH ONLY** — deliberately, per its own header,
     because stock ntvdm may not grant raw 8042 access. But Skyroads reads IN-GAME via
     **INT 09h + port 60h**. So our verified 35.3/s says nothing about the path the bug
     actually lives on. That fits the otherwise-odd fact that making repeats 3.3x
     faster (92 ms → 28 ms) only moved failures from 1-in-10 to 1-in-5.
   • ⇒ **NEXT STEP IS AN INT 09h-LEVEL PROBE WITH A HELD KEY**, not a constant change:
     does a held key produce ~N makes/second at the guest's INT 09h handler under
     NTVDMEX? Counters already exist (`ty_sent`, `irq1_inj`, `sc_push`, `sc_drop`).
     Only once that is known does the SPI->ms mapping (still overshooting: ours 35.3/s
     vs stock 22.1/s, delay 500 ms vs stock 385 ms) become the right thing to fix.
  **STILL OPEN:** rate fidelity. Measured with `tymat.com` under both hosts:
        stock 385 ms / 22.1 per second      ours now 494 ms / 35.3 per second
  We **overshoot** — the documented SPI mapping ("31 ≈ 30/s") does not match what a
  DOS program observes under stock. Fix the mapping against the oracle, then
  re-test the actual restart behaviour, which is the only acceptance test that
  counts. Also still unanswered: **does Skyroads set its own rate via 8042 `0xF3`?**
  Those writes are now logged (`STAGE2: kbd 8042:`) but only a SKYROADS run answers
  it — `tymat.com` reports `writes=0` and that says nothing about the game.

┌──────────────────────────────────────────────────────────────────────────────┐
│ 4. ★★ TRAPS FROM SESSION 15 — EVERY ONE COST REAL TIME, SEVERAL WERE MINE     │
└──────────────────────────────────────────────────────────────────────────────┘

  1. **TWO EXEs, AND ONLY ONE IS THE HOST.** `build/ntvdmhost.exe` (~409 KB) is the
     DOS host — deploy THAT. `build/ntvdmex.exe` (~20 KB) is the launcher and is
     stale. Deploying the launcher makes `rt.bat` install it as the IFEO Debugger,
     and its job is to launch ntvdm.exe — which redirects straight back into it.
     **Runs still "succeed" in ~20 s because rt.bat copies a STALE log.** Tell: the
     same md5 for two different targets. Only a reboot cleared it. **CHECKSUM THE
     DEPLOYED BINARY AGAINST THE LOCAL ONE EVERY TIME.**
  2. **NEVER EDIT WINDOWS BATCH FILES WITH PYTHON TEXT MODE ON macOS.** It strips
     the CRs on read and writes LF. `cmd.exe` breaks on `goto`/labels first, which
     is the entire watcher loop. Read and write **binary**, normalise to CRLF. I did
     this to all three harness scripts and killed the watcher with it.
  3. **`$TMPDIR` DIFFERS INSIDE AND OUTSIDE THE SANDBOX.** A file written by a
     sandboxed command is not where an unsandboxed one looks. Use absolute paths, or
     patch the file on the share in place.
  4. **THE STALE-`TN` RACE (user caught this one).** `for /f ... do set TN=%%c` only
     assigns if the read yields a line; an SMB-empty `cmd.txt` left `TN` at its
     PREVIOUS value, so the watcher silently re-ran the last target. Queued
     `skyroads`, got `p_ver`, and the log looked entirely plausible. Fixed with
     `set TN=` + an empty check; **also write `cmd.txt` atomically via rename.**
  5. **THE WATCHER DIES WHEN THE HOST EXITS.** `runwatch.bat` ran the test with
     `call`, i.e. inside its own `cmd.exe`, and the host is linked
     `--subsystem,console` so it shares in console teardown. Now `cmd /c`, giving
     the test its own process. (Reported by the user; the earlier deaths predate my
     CRLF damage, so these are two separate causes.)
  6. **A CONSTANT LAG READS EXACTLY LIKE A WRONG WAVEFORM.** Both audio harnesses
     now report best-lag correlation. The bass drum scored -0.135 at lag 0 and
     **+0.999 at lag 4**.

┌──────────────────────────────────────────────────────────────────────────────┐
│ 5. ★★★ THE METHOD LESSON OF SESSION 15 — READ THIS BEFORE FIXING ANYTHING     │
└──────────────────────────────────────────────────────────────────────────────┘

  **I shipped a regression by reasoning where I should have measured.** I capped the
  PIT catch-up at 10 ms on the assumption that syncs are always closer together than
  that. They are not — `host_pit_sync` takes `g_lock` and a heavy I/O-trap loop
  starves the UI thread, which the code says in a comment I had already read. The
  user's verdict was immediate: *"speed is all over the place now... a definite
  regression."* The original burst was at least CORRECT ON AVERAGE; discarding time
  is a bigger and permanent error. **Reverted.** One counter would have refuted the
  assumption before it shipped.

  Then **three successive wrong hypotheses about the keyboard** — FIFO overflow,
  lock contention, and a remembered typematic rate — before the user said *"I'd be
  better testing the exact behavior in stock NTVDM."* That was better methodology
  than anything I had produced, and it settled the question in one run.

  ▶ **THE RULE, restated for a domain this file had not yet aimed it at:** the
    cardinal rule is not only about DOS API expectations. It applies to **hardware
    timing constants, repeat rates, and anything else you "know"**. Take it from an
    executable oracle. We now have three: MS-DOS 6.22 under QEMU, Nuked OPL3, and —
    new — **stock ntvdm on the rig itself**.

═══════════════════════════════════════════════════════════════════════════════
██ THE OPL TIMBRE FAULT IS FIXED (#21). WHAT IS LEFT IS THREE DRUM VOICES.   ██
═══════════════════════════════════════════════════════════════════════════════

╔══════════════════════════════════════════════════════════════════════════════╗
║ ▶▶▶ SESSION 15 (2026-08-21): "the instruments sound flat" IS CLOSED.         ║
║     Commits `94dc86f`, `fc6995b`, `8159d28` on `m9/completeness`.            ║
╚══════════════════════════════════════════════════════════════════════════════╝

**THE COMPLAINT WAS:** Skyroads plays "the right tune, but the instruments sound a
bit flat" / "melodic synths sound inaccurate". Same 90 s trace, same harness:

                          BEFORE      AFTER      (and at best alignment)
    waveform correlation  0.4119  ->  0.9114              0.9255
    envelope correlation  0.8680  ->  0.9732
    level ratio           1.628   ->  1.004
    RMS error              152%   ->   42%

**The melodic synthesis — the actual complaint — is at 0.96-0.97.** The per-segment
scores show it plainly: 0.962 and 0.971 over the first 20 s, before any percussion
enters. Everything below 0.92 after that is the three unimplemented drum voices.

┌──────────────────────────────────────────────────────────────────────────────┐
│ WHAT WAS WRONG. FIVE DEFECTS + TWO MISSING FEATURES, ALL MEASURED             │
└──────────────────────────────────────────────────────────────────────────────┘

  1. **MODULATION DEPTH WAS HALVED — this was the timbre fault itself.** An
     operator's output goes straight into the phase index, so full modulation
     swings the carrier FOUR whole cycles; we did two. Measured ratio 0.501, flat
     across the whole TL sweep. Halving the index changes neither pitch, tempo nor
     loudness — phase modulation preserves power — only WHICH harmonics exist.
     That is exactly "right tune, wrong instruments". Feedback was halved in the
     same place: our FB=n matched the reference's FB=n-1 step for step.
  2. **THE ENVELOPE NEVER STARTED FROM SILENCE.** `env` counts ATTENUATION, so a
     zeroed struct is FULL VOLUME. Key-on does not reset it (measured — the
     reference resumes an interrupted attack), so the first note of a run jumped to
     full level whatever its attack rate said. Attack measured 0.00 ms at EVERY
     rate against the reference's 1689 ms at AR=1.
  3. **THE RATE LAW HAD NO SUB-STEPS.** Speed is `(4 + rate_lo) / 2^(15 - rate_hi)`
     — linear 4:5:6:7 inside a group of four, doubling at the boundary. We shifted
     by whole octaves and rounded the mantissa away: mid-range decays 1.5x too slow.
  4. **KSL WAS OFF BY AN OCTAVE AND A FACTOR OF TWO.** The ROM is in 0.75 dB units,
     not 0.375, and the octave origin is 8, not 7 — together under-attenuating high
     notes by up to 18 dB, so bass and treble sat at the wrong relative levels
     across the whole keyboard.
  5. **AR=0 MEANT "INSTANT" INSTEAD OF "NEVER"** — turning silent voices into loud
     ones.
  6/7. **TREMOLO AND VIBRATO** implemented (0xBD was stored and never acted on).

  **Already correct, and now under regression:** total level (1.003 at full scale,
  0.75 dB/step to 0.01 dB), all four waveforms, all sixteen MULT settings, pitch.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ▶▶▶ THE NEXT TASK: SNARE, HI-HAT AND CYMBAL. ~548+138+70 HITS PER SONG.       │
└──────────────────────────────────────────────────────────────────────────────┘

  Rhythm mode is implemented except for these three voices. They need the chip's
  **special phase generator**: a boolean function of bits taken from TWO phase
  accumulators, plus a noise source. **I deliberately did not guess it** — writing
  it from half-memory produces something plausible and wrong, and the harness would
  score it as an improvement because ANY sound beats silence. They are COUNTED
  instead and reported in STAGE2 as `NOT SYNTHESISED`.

  **EVERYTHING NEEDED TO DERIVE THEM IS ALREADY MEASURED** (`oplprobe rhythm`):

    voice       envelope   PHASE runs on   tonality   dominant component
    bass drum   op12+op15  own             1.000      ordinary 2-op FM   ✔ DONE
    tom-tom     op14       own             1.000      a plain sine       ✔ DONE
    snare       op16       **op13's**      0.509      H2 (2x op13)
    hi-hat      op13       op13 + op17     0.003      essentially noise
    cymbal      op17       op13 + op17     0.749      near Nyquist

    Percussion is summed at **DOUBLE amplitude** (tom-tom peaks 8170 where an
    ordinary operator peaks 4085). Verified per voice: tom-tom **+1.000**, bass
    drum **+0.999**.

  ★ **THE SUGGESTED METHOD, and it is tractable.** The cymbal's output is BINARY —
    its RMS equals its peak, at 0.707 of full scale — so its phase alternates
    between two values and **the output sign is a one-bit sequence you can read
    straight out of the oracle.** Extract that bit sequence, compute the candidate
    phase bits yourself (you control op13/op17 rates via MULT and F-num), and
    SEARCH over small boolean functions of those bits for the one that reproduces
    it. That is a derivation, not a guess, and it is a for-loop.

  ▶ **DO NOT MEASURE A NOISE VOICE WITH WAVEFORM CORRELATION.** Uncorrelated noise
    of exactly the right character scores 0. Judge hi-hat and snare on envelope
    correlation, RMS and the per-segment level instead.

  ▶ **AFTER THAT:** the user has not yet heard any of this. **The acceptance test is
    still the user's ears on the physical box.** Everything here is measured against
    a reference core, which is necessary and not sufficient.

┌──────────────────────────────────────────────────────────────────────────────┐
│ RIG STATUS AT THE END OF SESSION 15 — VERIFIED, AND LEFT CLEAN                │
└──────────────────────────────────────────────────────────────────────────────┘

  Rig `192.168.1.29`, share mounted, **new host deployed and VERIFIED on the
  physical box: `selftest.com` -> `==== ALL TESTS PASSED ====`.** Off-VM battery
  325/325. Host cross-builds clean and passes `check-imports.sh`.
  Share left clean: `headless_ms.txt` back to 30000, no cmd.txt, no control.txt,
  no flags. Watcher up, controld beating.

  ▶ **NOT DONE: a Skyroads run on the rig.** Two attempts at a 60 s cap produced no
    `result_skyroads.log` and left the watcher blocked in `rt.bat`'s `start /wait`,
    which needed a reboot to clear. The previous session used a **90 s** cap for
    this game; try that first. The rhythm counters are therefore verified as
    PRESENT (they print, correctly zero, on every run) but have not yet been seen
    counting real hits on hardware — the 548/142/138/70 figures come from replaying
    the captured trace offline, which is solid but is not the same evidence.

  ★★★ **THE TRAP THAT COST TWO REBOOTS, AND IT WILL CATCH ANYONE: THE BUILD
      PRODUCES TWO EXEs AND ONLY ONE OF THEM IS THE HOST.**
        build/ntvdmhost.exe   ~409 KB   ** THIS is the DOS host — deploy THIS **
        build/ntvdmex.exe      ~20 KB   the launcher; it is not rebuilt and is stale
    I copied `ntvdmex.exe` over `bm/ntvdmhost.exe`. `rt.bat` then installed it as
    the **IFEO Debugger for ntvdm.exe** — and that binary's job is to LAUNCH
    ntvdm.exe, so every launch redirected straight back into it. The box wedged.
    ▶ **The symptom is deeply misleading:** runs still "complete" in ~20 s and
      `rt.bat` faithfully copies `C:\ntvdmex\ntvdmhost.log` to `result_<target>.log`
      — so you get a plausible log for a run that never happened. **It was the OLD
      log every time.** `controld kill` did not clear it; only a reboot did.
    ▶ **HOW TO TELL, in one command:** checksum the result against a known previous
      log. `md5 result_p_ver.com.log result_skyroads.log` returning the SAME hash
      for two different targets is the tell. Grepping the log for something only
      your new build prints is the other.
    ▶ `result_selftest.log` on the share is a **stale copy of a Skyroads run** left
      by this. The real one is `result_selftest.com.log` — `rt.bat` resolves targets
      out of `bm\tests\`, so bare `selftest` matches nothing and silently falls
      through to copying the previous log.

┌──────────────────────────────────────────────────────────────────────────────┐
│ THE METHOD — NUKED AS A BLACK-BOX ORACLE. THIS IS NOT OPTIONAL CEREMONY.      │
└──────────────────────────────────────────────────────────────────────────────┘

▶ **DO NOT READ `build/oplref/opl3.c` FOR CONSTANTS.** Nuked OPL3 is **LGPL-2.1**;
  `vdd_opl_synth.c` is deliberately clean-room MIT ("written from the documented
  YM3812 behaviour rather than ported from an existing core, so it is ours and
  MIT-clean"). Reading it forfeits that, and the loss is not limited to the line you
  looked at: some constants are FORCED by the hardware (one valid value, no exposure)
  and others are the implementation's own CHOICES (structure, edge cases, rounding) —
  **and you cannot tell which is which by looking.** Reading contaminates you for all
  of it. The user was asked and chose to keep the MIT posture.

▶ **DO use it as an ORACLE:** controlled input -> observe output -> derive the value.
▶ **DO read public, non-LGPL documentation** for the expectation: OPL2/OPL3 datasheets
  and the public hardware write-ups. This is the project's cardinal rule pointed at a
  new domain — **docs for the expectation, executable oracle for the verification.**
▶ **Converging on the same constant Nuked uses is EXPECTED and FINE.** Clean-room is
  about provenance, not divergence; the Phoenix BIOS was functionally identical by
  design. (Engineering practice, not legal advice.)
▶ The usual objection is "clean-room is the long way round". Normally yes — two teams,
  months. **Here it is a for-loop**: the harness scores automatically, so a sweep is
  minutes. The saving from reading the source is small.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★ THE SINGLE-NOTE RIG — `oplprobe`. THIS IS WHAT MADE THE SESSION SHORT.      │
└──────────────────────────────────────────────────────────────────────────────┘

  `./tools/oplref/build.sh` now builds TWO binaries. **`oplcmp` proves the timbre is
  wrong; it cannot say WHICH parameter**, because every note of a game trace moves
  every variable at once. `oplprobe` does the opposite: holds one channel still,
  moves ONE register, and reports a DERIVED PHYSICAL QUANTITY for both cores.

      ./build/oplref/oplprobe <experiment>     # or `all`
        validate  silence/determinism/pure-tone/pitch -- RUN THIS FIRST, ALWAYS
        tl        total level: full scale and dB per step
        mod       MODULATION INDEX in radians -- the prime suspect, fitted from
                  the sideband amplitudes via a Bessel fit
        fb        feedback          wave  the four waveforms     mult  all sixteen
        ksl / kslrom   the whole block x F-num surface, and the ROM read back
        env       attack/decay/release times     egrate  the RATE LAW, derived
        attack    the attack CURVE (geometric), fitted against the decay slope
        lfo       tremolo/vibrato rate, depth AND SHAPE
        rhythm    maps percussion from the outside: which operator, whose phase

  ▶ **EVERY CONSTANT IN `vdd_opl_synth.c` NAMES THE EXPERIMENT THAT PRODUCED IT.**
    Re-derive rather than argue; a sweep is seconds.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★ FOUR TRAPS IN THE INSTRUMENT ITSELF. ALL FOUR PRODUCED CONFIDENT NONSENSE. │
└──────────────────────────────────────────────────────────────────────────────┘

  Every one of these was MY measurement being wrong, not the synth — and each
  looked right at the time. This is the same lesson the rest of this file keeps
  teaching, pointed at a new domain.

  1. **A "PURE TONE" THAT WAS NOT PURE.** A modulator parked at TL=63 is only
     -47 dB, not silent, and it still bends the carrier: 5% THD. Fine for a level
     reading, fatal for a distortion one. **Park the unwanted operator on ANOTHER
     HARMONIC (MULT=12)** so its residual cannot reach the bins being measured.
  2. **THE QUANTISATION FLOOR.** Fitting a decay slope over points near zero
     amplitude — where the 16-bit output has stopped moving — reported a rate 8%
     too slow AND a beautifully constant anchor column that made it look correct.
     Only when I raised the floor did the divisor snap to exactly 2^15.
  3. **A CONSTANT LAG READS EXACTLY LIKE A WRONG WAVEFORM.** The bass drum scored
     **-0.135 at lag 0 and +0.999 at lag 4** — identical waveform, four samples
     apart. The reference is cycle-accurate and reaches its output a few samples
     after the write that caused it. **Both harnesses now report best-lag
     correlation**; without it I would have hunted a defect that does not exist.
  4. **AN UNRESOLVABLE READING IS NOT EVIDENCE.** The vibrato sweep printed
     "121 cents" at F-num 0x100 — a pitch whose test note has too few zero
     crossings per window to resolve a 7-cent shift. That reading is now DELETED
     from the sweep rather than reported, because a number with no precision behind
     it is worse than no number.

┌──────────────────────────────────────────────────────────────────────────────┐
│ THE TOOLING — BUILT AND WORKING. USE IT, DO NOT REBUILD IT.                   │
└──────────────────────────────────────────────────────────────────────────────┘

  **1. CAPTURE a register trace from a real run** (host writes it itself):
      : > /tmp/xpshare/opltrace.flag            # the knob; absent = zero cost
      printf '90000' > /tmp/xpshare/headless_ms.txt
      rm -f /tmp/xpshare/result_skyroads.log /tmp/xpshare/opltrace.txt
      printf 'skyroads\r\n' > /tmp/xpshare/cmd.txt
      # wait for result_skyroads.log to appear, then:
      cp /tmp/xpshare/opltrace.txt build/oplref/skyroads.txt
      rm -f /tmp/xpshare/opltrace.flag          # ALWAYS clear it afterwards
    Format: one `us reg val` per line, hex. Timestamps are the guest's REAL write
    times, so a replay reproduces its phrasing. Hook is `opl_state.trace`, set by the
    host only when the flag exists, so the VDD stays pure C.
    ► ★★ **NEVER end a trace run with controld `kill`.** It is `taskkill /f`; the
      trace and the whole STAGE2 block are written during the CLEAN wind-down, so a
      killed run yields a log file with the useful half missing. **It looks like it
      worked.** Let the headless cap expire, or quit the guest from its own menu
      (rt.bat sets `autoexit`). This cost the user replaying Skyroads twice.

  **2. BUILD the comparison harness** (pulls the reference out-of-tree on demand):
      ./tools/oplref/fetch.sh      # -> build/oplref/opl3.[ch]  (gitignored)
      ./tools/oplref/build.sh      # -> build/oplref/oplcmp

  **3. COMPARE:**
      ./build/oplref/oplcmp build/oplref/skyroads.txt build/oplref
    Prints the metrics above and writes `ours.wav`, `ref.wav`, `envelope.csv`.
    **`ref.wav` is what Skyroads should sound like; `ours.wav` is what we produce.**

┌──────────────────────────────────────────────────────────────────────────────┐
│ THE PLAN, IN ORDER                                                            │
└──────────────────────────────────────────────────────────────────────────────┘

  **0. VALIDATE THE HARNESS FIRST (~30 min). Do not bisect against an unverified
     instrument.** It has ALREADY produced one artefact: the reference was driven
     with `OPL3_WriteRegBuffered` (a realtime write-latency queue) and that alone
     cost 0.17 vs 0.41 waveform correlation — **a quarter of the apparent defect was
     mine, not the synth's.** Sanity checks worth doing: silence in -> silence out in
     both; a single pure tone matches; the same trace twice is bit-identical.

  **1. Build the SINGLE-NOTE experiment (~30 min).** A hand-written trace: one
     operator pair, one sustained note, ONE variable moving. A game trace proves the
     timbre is wrong; one note with one variable NAMES THE PARAMETER. This is what
     turns a multi-hour search into a short one.

  **2. Derive the constants, in this order** (each is a sweep scored automatically):
     a. **Attenuation / TL mapping** — one operator, no modulation, sweep TL 0..63,
        measure output amplitude in both cores. Should settle the 1.628 level ratio
        on its own.
     b. **Modulation index scaling** — two operators, fixed carrier, sweep modulator
        TL, compare sideband amplitudes. **PRIME SUSPECT for the timbre.**
     c. **Feedback scaling** — one self-modulating operator, sweep FB 0..7.

  **3. THEN the two genuinely missing features** (both currently no-ops; `0xBD` is
     stored in `reg[]` and never acted on — see `vdd_opl.c`):
     a. **Tremolo/vibrato LFOs.** MEASURED as genuinely used: **103 notes start with
        tremolo and 149 with vibrato out of 982**. Those counters are per-note EDGES,
        so they are trustworthy. ~2-3 h.
     b. **Rhythm mode** — the 5 percussion voices. **ONLY IF PROVEN NEEDED**, see the
        correction below. ~half a day.

  **ACCEPTANCE:** beat the baseline above — waveform correlation toward 0.9+, level
  ratio toward 1.0 — and the user confirms by ear on the rig. Every synth change now
  has a regression score, so never change it without re-running `oplcmp`.

  **ESTIMATE GIVEN (honest, wide because the parameter is not yet identified):**
  ~half a day if it is a scaling constant; up to two days if it is structural (our
  1/96 dB, 8.8 fixed-point envelope vs the chip's exact 9-bit attenuation pipeline)
  and all gaps are closed.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★ A WRONG CONCLUSION I REACHED — DO NOT REPEAT IT                             │
└──────────────────────────────────────────────────────────────────────────────┘

  I added register counters, read `bd_or=0xFB` + 888 writes to `0xBD`, and announced
  that Skyroads drives ~888 percussion hits and our missing rhythm mode was the cause.
  **That was wrong, and the user — who knows the game — corrected it: the music is
  mostly melodic synths.**
  ▶ **The flaw was the instrument.** `bd_or` is an **OR across the whole run**, so it
    cannot distinguish "these bits were set once during init or a silence-all reset"
    from "drums play throughout". It is a weak assertion dressed as evidence —
    exactly the trap already recorded in this file, in a new costume.
  ▶ **The second trace makes it starker:** 2144 of 4243 writes go to `0xBD` against
    only 545 notes — **~4 writes per note**, which is a driver hammering the register
    in its update loop, not drum triggering.
  ▶ **Trustworthy counters** in the same STAGE2 line are the per-note EDGE ones:
    `keyon_am`, `keyon_vib`, `keyons`. Use those; treat every `_or` field as a hint.
  ▶ **The lesson generalises:** a counter can tell you a feature was TOUCHED. Only a
    waveform comparison can tell you a feature SOUNDS WRONG. That is why the harness
    exists and why it should be the first thing you reach for.

  ▶ WHERE THE CODE IS: `src/vdd/vdd_opl.c` (device + registers, the profile counters),
    `src/vdd/vdd_opl_synth.c` (**the FM synthesis — the thing to fix**),
    `src/vdd/opl_tables.h` (log-sin / exp tables), `tools/oplref/` (harness, in tree),
    `build/oplref/` (gitignored: the reference core, the binary, the WAVs, the CSV).

▶▶▶ **SESSION 14 (2026-08-20, late): THE MODE-12h WALL IS DOWN. #55 IS CLOSED.**
  BLIT.EXE renders 640x480 16-colour filled boxes on the physical box, matching
  `build/shots/demos/oracle_blit.png` in kind (the boxes are random per run).
  Frames in `build/shots/p12/`. Commit `b3abbce`.

  ★ **THE MOVE THAT WORKED WAS TO STOP TRYING TO INTERCEPT THE WRITES.** Session 13
    had already measured the answer and framed it as a question about interception:
    with the A0000 page trap off the guest runs perfectly (22.5M I/O events) and the
    only defect is that its planar writes bypass the VGA engine. The unexamined
    assumption was that we therefore had to SEE those writes. We don't — we can
    simply BE the CPU that performs them. While a planar mode is set the guest now
    runs in the host interpreter, whose A0000 accesses go through the planar write
    engine by construction (`imem_r8`/`imem_w8`). No page protection, no kernel RE,
    no VDD memory hook. **The planned next step — disassembling XP's VDM memory
    handling — was not needed and was not done.**

  ★ **THREE BUGS HAD TO DIE FIRST, and two of them were silent corrupters:**
    1. The interpreter could not survive an interrupt: no `INT nn`, no `IRET`, no far
       `JMP`/`CALL`. It stopped at the first DOS call and handed the guest back to
       V86 — precisely where the writes become invisible. Now modelled, vectoring
       through the real IVT. **`LES` (`C4`) stays unmodelled ON PURPOSE**: bailing on
       it is how a BOP still reaches the kernel. Modelling it would swallow every
       DOS/BIOS call in the system.
    2. **`host_interp` never wrote the SEGMENT registers back.** It modelled `POP ES`
       and `MOV DS,AX` and then threw the result away, so the guest resumed with the
       segment it had BEFORE the batch and the offset the batch had reached. Harmless
       while batching was confined to a fill loop that reloads nothing; fatal the
       moment CS changes on every interrupt.
    3. **`POPF` masked `IF` out of the flag image**, so every interpreted `POPF`
       silently disabled the guest's interrupts.

  ★ MEASURED, same build, same program, one policy switch:
        page trap    io_events=0x1d       plane-nonzero = 0/0/0/0        frozen
        interpret    io_events=0x50d4e6   plane-nonzero = 1f5b/389e/79c6/25f8
    575M instructions interpreted, 11 captured frames. `p12off.flag` reverts to the
    page trap. STAGE2 reports `p12-batches/instrs/bails`, and **every opcode the
    interpreter declines is named (`P12-BAIL`)** — that list is the to-do list for
    this path, and it is how you tell "we ran it" from "we lost the guest to V86".

  ★★★ **THEN ALL TEN WERE WATCHED LIVE, ONE AT A TIME, AND ALL TEN RENDER CORRECTLY.**
    Full write-up + every observation: **`docs/research/demo-sweep-findings.md`** — read
    that before touching video or timing. Headlines:
      • **NOT ONE PIXEL DEFECT.** Every defect found is TIMING.
      • **The retrace bit (0x3DA) is untimed** — we toggle it on every read, so
        `WAIT &H3DA,8` returns instantly and anything that paces on vblank runs
        unbounded (BOUNCEBX, MATRIX_2, CAVE). **CAVE proves this is PRE-EXISTING and
        not ours: it is SCREEN 13, which never touches the interpreter.** Re-check
        Skyroads against it — its "a little sluggish" calibration predates knowing this.
      • **USER FEATURE REQUEST, and it is load-bearing: a menu dropdown for approximate
        CPU speed** (33/66/100/200 MHz). Two of the five speed-affected demos pace
        themselves with a busy-wait or not at all, so NO retrace fix can ever reach
        them. See finding #3 for implementation notes across both execution paths.
      • **MOUSE was NOT a defect** and neither was INT 33h. The oracle ran the same
        binary on genuine MS-DOS 6.22: it exits in 2.9 s there too. `mousetst.com` then
        proved INT 33h works end to end — cursor tracks, left button draws
        (`build/shots/mousetst_live.png`). The only real item is cosmetic: our arrow is
        hand-drawn and the user has a 16×16 cursor to swap in.
      • Three readings were WRONG and corrected by evidence mid-sweep (see the method
        note at the end of that file). Every one of them looked obviously right.

  ★ **THE DEMO SWEEP RAN: ALL TEN QuickBASIC DEMOS, ALL TEN DRAW.** Six SCREEN 12,
    three SCREEN 13, one SCREEN 0; `video modes unsupported: none` everywhere.
        BLIT      16-colour random filled boxes -- matches the oracle in kind
        MATRIX_1  full-screen Matrix rain, glyphs + green ramp  ← the strongest one
        MATRIX_2  same, sparser (planes 0b04/0b21/0b47/0bed)
        BUBBLES   greyscale starfield. **The greys are CORRECT** -- BUBBLES.BAS sets
                  its own `PALETTE index, index*4` ramp, so this also proves the
                  palette path. Do not "fix" it into colour.
        BOUNCEBX  40x40 filled box, caught mid erase-redraw (planes b4/00/b4/00)
        MOUSE     sets 12h, returns to mode 3 and exits in ~3 s -- ONE shot, and the
                  only demo whose behaviour is not yet explained. Look here first.
        CAVE / GFXCOPY / PALETTE (13h) and VS87 (text) unchanged -- no regression.
    Frames: `build/shots/p12/`. Screenshots need `capture.flag` on the share; it was
    deleted afterwards, as it must be.

  ▶ **WHAT IS STILL OPEN HERE:** every bail is a stretch of guest execution running
    on the real CPU with its A0000 writes going nowhere. BLIT had ~5.3M of them
    against 515k batches, so the picture is right but not provably complete. Work the
    `P12-BAIL` list before claiming planar parity.

  ▶ THE PAGE-TRAP FREEZE ITSELF IS STILL UNEXPLAINED and is now a curiosity rather
    than a blocker. If it is ever picked up: the exec thread does not return from
    `VdmStartExecution` at all (the TIB's CS:IP stays frozen at whatever the last
    event left it — 0050:0037, the `CD 1C` in our INT 08h stub, is a STALE reading,
    not where the guest is). Do not read it as "the guest is in its timer handler".


▶ **READ IN THIS ORDER:** (1) this block, (2) the session-13 checkpoint below — it holds the
  measurements, the ruled-out list and the traps, (3) **GitHub epic #24** for the programme's
  standing policy, and **#55** for the task in front of you.

▶ **M9 STATUS: INT 21h 103/103, BIOS complete, all 15 probes clean against the oracle panel.**
  17 sub-issues closed, 9 raised (#47-#55), 5 left open with a comment stating exactly what
  remains. Verified at host `v180`: selftest 8/8, off-VM battery 325/325.

▶ **THE STANDING PRINCIPLE (user, 2026-08-19), unchanged:** *"There should be no cause in
  NTVDMEX itself to fail — whatever we throw at it should just work."* The achievable form is
  **no SILENT failure**: every unimplemented thing announces itself. That is now built in — a
  run ends with a to-do list (`STAGE2: INT21 unimplemented:` and friends), and it is how the
  whole evidence pass was driven.

▶ **THE CARDINAL RULE, and it has now earned its keep three times over:** *never write a test
  expectation from memory of what DOS does.* Take it from RBIL **confirmed against the oracle**.
  Refuted from memory this session: #27's own headline instruction (real DOS sets NEITHER AX
  nor CF on an unhandled call), the find-first "not found" code (18, not 2), and the FCB
  convention (result in AL; **carry is undefined** — a successful open returns CF=1). Each
  would have made us *less* accurate while looking like a fix.

▶ **THE DECISION THAT SHAPED THE SESSION (user, 2026-08-20):** completeness before breadth,
  then push for 100%, then EXEC, then the demos. All delivered except the demos, which are
  blocked on mode 12h.

▶ **WHERE THE REAL FRONTIER IS NOW: COVERAGE IS NOT CORRECTNESS.**
  We have 100% of the documented API, oracle-matched. We have **application** evidence for
  exactly one game (Skyroads, mode 13h) and four command-line tools. Two independent things say
  that is not the same as working:
    • **#47** — MEM.EXE reports nothing missing and prints WRONG NUMBERS. The failure mode this
      epic exists to remove, surviving *because* the coverage is complete.
    • **#55** — mode 12h has never rendered. Six of the ten QuickBASIC demos need it.
  **Next milestone should be measured in APPLICATIONS THAT BEHAVE CORRECTLY, not functions
  implemented.** The bar remains Doom / Skyroads / ZAR.

▶ **THE ACCEPTANCE BAR:** Skyroads is fully playable (menus, gameplay, sound, text — confirmed
  on the physical box, sessions 11-12). Doom and ZAR remain; both are DOS/4GW, so they sit
  behind the DPMI workstream, not this one.

▶ **TEST TIERS — put each test in the right one:** off-VM C battery (`tools/dostest/run.sh`,
  **325 checks**, runs on the Mac in seconds) for anything that is pure logic; a guest `.COM`
  through **`scripts/dosdiff.py`** for anything guest-observable; the rig (`selftest`, 8/8) as
  the final gate. selftest is a SMOKE TEST at ~2 min a round — it is NOT the TDD loop.
  ► The fast loop is now the **oracle** (`./scripts/oracle.sh <probe>.com`, ~3 s, offline).

▶ DEFERRED BY DECISION — DO NOT PICK UP UNASKED: keyboard/music latency (user rates Skyroads
  "genuinely playable, a little sluggish"; lag is in the milliseconds). Also queued: hardware
  grounding (CPU affinity, SpeedStep), which lands on our timing path since guest clocks come
  from QueryPerformanceCounter.

═══════════════════════════════════════════════════════════════════════════════
██ CHECKPOINT — 2026-08-20 (session 13). M9 API COMPLETE; MODE 12h IS THE WALL. ██
═══════════════════════════════════════════════════════════════════════════════

▶ RESTART POINT: branch `m9/completeness`, **19 commits UNPUSHED**, working tree clean apart
  from the usual not-mine untracked files (MAINICON.ico, demos/, scripts/kd_*.py,
  trace_break.py) and the untracked native `tools/dostest/*_test` binaries (repo convention).
  Host = **dpmi-harness-v180**, built clean, deployed to the share `bm/`. Rig healthy.
  **Share knobs CLEARED** — no capture.flag, no noa000.flag, no interp12.flag. Leave them that
  way; a stray knob makes every later run lie to you.
  Verified at v180: selftest **8/8**, off-VM battery **325/325**, all 15 probes clean.

▶ **THE RIG IP: `192.168.1.29`. TRY IT FIRST — A REBOOT DOES NOT MOVE IT.** (User's
  correction, 2026-08-21, after I swept the LAN following a reboot for no reason.) Only a
  **network drop** has ever moved it: `.34` → `.29` after a broadband outage. So sweep only
  when `.29` genuinely does not answer on port 445. Note an ARP sweep leaves INCOMPLETE
  entries for every address, so `arp -a` afterwards lists all 254 and means nothing — probe
  port 445 instead. LAN access needs `dangerouslyDisableSandbox`.
  `mount_smbfs -N //guest@192.168.1.29/ntvdmex /tmp/xpshare`
  ▶ **If the box goes down, the SMB mount goes STALE and any `ls` of it HANGS** (it cost a
    3-minute timeout). Once the box is back the mount may simply be empty — just re-run
    `mount_smbfs`. Do not reach for a forced unmount first.

┌──────────────────────────────────────────────────────────────────────────────┐
│ 1. WHERE M9 GOT TO                                                            │
└──────────────────────────────────────────────────────────────────────────────┘

  **INT 21h: 103/103.** Every service MS-DOS 6.22 defines is implemented and oracle-matched.
  **BIOS: complete** — modes, palette, character generator, VESA, and INT 11h/12h/13h/14h/15h/
  17h/20h/25h/26h/27h/28h/29h, every one of which was previously a bare IRET handing the caller
  its own registers back.
  **All five real 6.22 tools report `INT21 unimplemented: none`** — MEM, CHKDSK, TREE, ATTRIB,
  COMMAND.COM. EXEC works: a parent launches a child, the child runs, the parent resumes with
  the child's exit code.

  GitHub: **17 issues closed**, 9 raised (#47-#55), 5 left open with a comment saying exactly
  what remains. Epic #24's body carries the full picture.

  ▶▶ **COVERAGE IS NOT CORRECTNESS, AND WE HAVE PROOF.** MEM.EXE reports nothing missing and
     prints WRONG NUMBERS (#47): "largest executable program size 0K (4,294,967,280 bytes)",
     conventional free 0K, XMS 0K against a 16 MB pool. Every function it calls is implemented
     and oracle-matched. **That is the failure mode this whole epic exists to remove, surviving
     precisely BECAUSE the coverage is complete.** Fix it before any memory-parity claim.

┌──────────────────────────────────────────────────────────────────────────────┐
│ 2. THE NEXT TASK: WHY DOES PROTECTING A0000 STALL THE VDM?  (GH #55)          │
└──────────────────────────────────────────────────────────────────────────────┘

  **Mode 12h has never rendered.** Six of the ten QuickBASIC demos in `demos/` are SCREEN 12
  and all six are affected; the three SCREEN 13 ones work. Skyroads is 13h, which is why our
  one game never touched this path. (Sources in `demos/src` — BLIT.BAS is four lines and draws
  random filled boxes in all 16 colours.)

  ★★★ **THE MEASUREMENT. Same program, same build, one knob:**
        trap armed             io_events =         10   guest frozen at 0050:0037 (58/60 HBs)
        trap OFF               io_events = 22,532,292   guest running, PC moving every sample
        trap off + interpret   io_events =         10   two batches of 46/38 instrs, then quiet
    `PAGE_NOACCESS` and `PAGE_READONLY` behave IDENTICALLY, so it is NOT reads-vs-writes — it
    is protecting the range at all. Knob: **`noa000.flag`** on the share disables the trap.

  ★★★ **THE FRAMING THAT MATTERS, and it narrows the RE a lot:** with the trap simply OFF the
    guest RUNS PERFECTLY — 22.5M events, correct execution, PC advancing. The only thing wrong
    is that its A0000 writes bypass the planar engine into the raw aperture. **So the guest is
    not the problem and the planar engine is not the problem. The sole issue is INTERCEPTING
    those writes.** The question is therefore narrow: what does the VDM require of that address
    range that VirtualProtect breaks, and **is there a kernel-sanctioned way to ask for the
    same interception** (a VDD memory hook, the kernel's own A0000 handling) rather than doing
    it behind the kernel's back?

  ▶ **RULED OUT BY MEASUREMENT — DO NOT RE-INVESTIGATE ANY OF THESE:**
    • **The mode table** (#39). Resolves 12h correctly: `mode=0x12/kind=01/640x480`, proved by
      the `STAGE2: mode sets:` line. I nearly rewrote it anyway.
    • **The planar write engine.** Complete and correct — 4 write modes, set/reset, ALU, bit
      mask, latches. I nearly rewrote THIS anyway too.
    • **The IVT.** `ivt08=0050:0034 ivt1C=0050:003a`; QuickBASIC hooks neither.
    • **Async IRQ injection.** 545 successes, ZERO bails, zero nest-blocks.
    • **The "mode-12h MOV-store decoder gap"** from the M3 notes: `interp-refused=0`. The
      interpreter never declines an opcode. **THAT LEAD IS DEAD.**
    • **Unhandled events.** None — no `STAGE2: stop event` line; every event is serviced.
    • **Interpreter-driven mode 12h** (`interp12.flag`, committed as a measured negative). The
      reasoning was sound — port traps alone hand us control 22M times, so no page protection
      should be needed — but the storm gate is met twice in thirty seconds and the guest goes
      quiet afterwards.

  ★★ **FIXED ON THE WAY (keep, it stands alone): `host_interp()` could not take interrupts.**
    It ran up to 2,000,000 guest instructions in the host with no way to be interrupted. A
    guest loop that can only END on an interrupt — BLIT's `DO WHILE INKEY$ = ""` — ran there
    forever. It now checks for a pending IRQ every 256 instructions and yields. 15x improvement.
    The interpreter stands in for the CPU; a real CPU takes interrupts mid-loop.

  ▶ **WHY THIS PROBABLY NEVER WORKED:** the M3 planar trap was **VM-confirmed on HVF and never
    on real hardware**, and there is precedent for exactly this class of difference — session 8
    found HVF reflects IOPL-0 I/O as event 0 while real silicon uses event 3.

┌──────────────────────────────────────────────────────────────────────────────┐
│ 3. THE TOOLING BUILT THIS SESSION — USE IT, DO NOT REBUILD IT                 │
└──────────────────────────────────────────────────────────────────────────────┘

  **THE ORACLE (#25)** — genuine MS-DOS 6.22 under QEMU, ~3s a query, offline.
      ./scripts/oracle.sh tools/dostest/p_ver.com      # run a guest binary on real DOS
      ./scripts/oracle.sh --batch "VER"                # run DOS commands
      ./scripts/oracle.sh --selftest                   # 4/4
      python3 scripts/dosoracle/build.py               # rebuild the image, ~75s
    Media = `./msdos-622` (4 retail floppies, gitignored). Runs `snapshot=on`, so a probe
    calling destructive DOS functions cannot corrupt it. Full write-up:
    `docs/research/dos-oracle.md`.
    ► **PIXELS:** there is no capture-on-success path. The only way to get a picture out is the
      TIMEOUT screendump: `dosoracle.py run BLIT.EXE --timeout 22 --screenshot out.ppm`.
      That is how `build/shots/demos/oracle_blit.png` (the mode-12h reference) was captured.

  **THE DIFFERENTIAL HARNESS (#26)** — one .COM, three hosts, one diff.
      python3 scripts/dosdiff.py tools/dostest/p_ver.com
    • **NTVDMEX does not vote.** It is the subject; the oracles vote. Letting the thing being
      graded into its own consensus would be circular.
    • **`SIG`** — each case declares which registers hold ITS answer. DS/ES follow the PSP and
      most FLAGS bits are undefined after a DOS call.
    • **Buffers are diffed too**, with `ignore_bytes` as the buffer analogue of SIG.
    • **54 recorded rationales** in `tools/dostest/oracle-rules.json`, merged not first-match.
    • 15 probes in `tools/dostest/p_*.asm`, all built on `probe.inc`. Companion files via a
      `<probe>.deps` sidecar. Every host runs a probe from its own directory.
    • **NOT WIRED: stock `ntvdm`** — needs an rt.bat variant that drops the IFEO Debugger key,
      and a decision on the display-wedge risk.

  **THE LOUD-FAILURE BLOCK (#27)** — every run ends with a to-do list:
      STAGE2: INT21 unimplemented: / undefined-on-6.22: / BIOS partial: / INT10 unimplemented:
      STAGE2: video modes unsupported: / mode sets: / video now: / ivt08=...
    Plus `INTERP-REFUSED` (names any opcode the interpreter declines) and `BATCH ran=...`
    (how far each interpreter escalation got). **These instruments are what killed four wrong
    hypotheses this session. Read them before theorising.**

┌──────────────────────────────────────────────────────────────────────────────┐
│ 4. TRAPS — EVERY ONE OF THESE COST REAL TIME                                  │
└──────────────────────────────────────────────────────────────────────────────┘

  ▶ **THE REFERENCE IS THE ORACLE, NEVER OUR OWN PREVIOUS BUILD.** I called mode 12h "a
    regression I introduced today" because our new output differed from our old. **Neither was
    correct** — the oracle showed 16 colours, both of our builds showed 2. *Different is not
    wrong when nothing is right.* One oracle run settled in seconds what an hour of comparing
    our own screenshots could not.
  ▶ **BOP NUMBERS ARE A SHARED NAMESPACE** across DOS, BIOS, XMS (0x43) and DPMI (0x50-0x57).
    Planting INT 20h with BOP 0x20 — already INT 21h's — made the BIOS dispatch intercept EVERY
    INT 21h call as "terminate program"; selftest exited at its first DOS call with no output.
  ▶ **LINEAR 0x714 IS KERNEL VDM STATE.** Putting the AH=65h tables at segment 0x0071 (the
    DOS-resident MCB block's data area — by the memory map, exactly the right home) wrote over
    it and wedged EVERY guest including selftest. The map says free; the kernel disagrees.
  ▶ **DOS CALLS THAT RETURN A SEGMENT IN DS** (1Bh, 1Ch, 32h, 52h) corrupted the probes' own
    output: every probe store is DS-relative, so the probe wrote its state into DOS's segment
    and printed labels read from there. Fixed in `probe_capture`, not per-probe.
  ▶ **MEASURE THE BUFFER, NOT JUST THE REGISTERS.** The country block is **24 bytes, not the
    commonly quoted 34** — poison the destination with 0xEE first. Writing 34 would clobber ten
    bytes of the caller's memory.
  ▶ **POISON THE OUTPUT REGISTERS.** "Untouched" and "deliberately zero" are otherwise
    identical, and BX may hold leftovers from the probe's own print routine — which is exactly
    what happened (a "result" of 3246 that `probe_emit` had left there).
  ▶ **A WEAK ASSERTION IS WORSE THAN NO TEST.** My own selftest asserted `FIND "File(s)"`,
    matched nothing (6.22 prints `file(s)` lower-case, FIND is case-sensitive) and still
    reported PASS, because it only checked "some output appeared".
  ▶ **CASE NAMES MATTER.** A case first called `4F.exhausted` actually measured whether a
    failed find-first clobbers the DTA — the misleading name framed a CORRECT result as our bug.
  ▶ **A .COM OWNS ALL OF MEMORY**, so EXEC returns AX=0008 until the parent gives some back
    with 4Ah. Not a defect — it is what every real shell does. "EXEC says out of memory" is
    otherwise a mystifying first symptom.
  ▶ **DOSBOX CANNOT RUN HEADLESS.** `SDL_VIDEODRIVER=dummy` hangs dosbox-x and aborts
    dosbox-staging. The adapter opens a real window; it will not work over plain ssh.
  ▶ **THE DEV SANDBOX REFUSES TO BIND A UNIX SOCKET ANYWHERE**, TMPDIR included — the QEMU
    monitor runs on stdio. `$TMPDIR` also differs inside and outside the sandbox; use absolute
    paths when handing files between the two.
  ▶ **`cd` PERSISTS BETWEEN COMMANDS** in this harness. A leaked `cd` silently ran a whole
    probe sweep from the wrong directory and reported nothing.

▶▶ RESUME — NEXT STEPS (in order):
  1. **GH #55 — RE XP's VDM memory handling.** The narrow question: what does the VDM require
     of A0000 that VirtualProtect breaks, and is there a kernel-sanctioned interception (VDD
     memory hook / the kernel's own A0000 path)? Everything else is ruled out and listed above.
     Acceptance: BLIT.EXE renders 16-colour filled boxes matching `build/shots/demos/
     oracle_blit.png`. Then the other five SCREEN 12 demos.
  2. **GH #47 — MEM.EXE's wrong numbers.** Silent wrongness in the MCB chain / stubbed SysVars
     (#48) / XMS reporting. Do NOT close it by making the numbers look nicer.
  3. **Then the demo sweep** — 10 QuickBASIC demos, screenshots, USER WATCHING THE SCREEN
     (they asked to be told before it runs). `capture.flag` on the share enables self-capture;
     shots come back as `shot_<test>_*.bmp`. **Delete the flag afterwards.**
  4. Finish #26 (stock ntvdm host) and #28 (the version menu) if the display-wedge risk is
     acceptable.
  5. `git push` — 19 commits sitting local on `m9/completeness`.

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

═══════════════════════════════════════════════════════════════════════════════
██ CHECKPOINT — 2026-08-19 (session 11). ██
═══════════════════════════════════════════════════════════════════════════════

▶ RESTART POINT (2026-08-19, session 11): HEAD = `3164690`; branch spike/dpmi-16bit-switch;
  **38 commits UNPUSHED**. Host = **dpmi-harness-v126**, built clean, deployed to the share `bm/`,
  and RE-VERIFIED on the rig: selftest **8/8 PASS**, off-VM battery **519/519**. Rig healthy
  (watcher + controld beating). Working tree clean except the same pre-existing untracked files
  that are NOT mine (MAINICON.ico, demos/, scripts/kd_*.py, scripts/trace_break.py) and the
  untracked native `tools/dostest/*_test` binaries (repo convention). New this session:
  `build/re/` holds XP's ntoskrnl/ntvdm/ntdll pulled off the box for RE (gitignored; /tmp was
  wiped, so re-fetch with the cmd.txt injection trick below if it disappears again).

  THIS SESSION went after session 10's blocker ("we cannot asynchronously interrupt a V86
  guest"). It found the kernel lever, found that a line of session-10 code was ACTIVELY
  WEDGING guests, and -- the most consequential result -- found that **the blocker was
  misdiagnosed**: Skyroads is not starved of injection points, it is being refused them.

★★★★★ **SKYROADS IS PLAYABLE** (user-confirmed on the physical box, 2026-08-19). Host v126.
It boots, renders, animates its intro, plays OPL music and PCM, takes keyboard input and
plays. Getting there took four fixes after the async-injection breakthrough, three of them
timing and NONE of them in the sound code:
  1. **The guest's CLOCK was driven by the UI thread.** Skyroads reads PIT counter 0 directly
     (`out 43h,al; in al,40h; in al,40h` at 0110:5a85 -- caught by the new IO-SITE dump), so
     that counter IS its sense of time; we advanced it from the frame loop, which is starved
     exactly when the guest is hammering I/O. Everything time-paced crawled: fade, music
     tempo (pitch was always right -- the OPL is correct, only the sequencer was slow) and the
     rate it fed PCM (slow AND pitched down). Now `host_pit_sync()` derives clocks from
     QueryPerformanceCounter, called from BOTH threads -- exec (so a poll reads real time) and
     UI (so the clock runs while the guest spins). Driving it from one only deadlocks.
  2. **My own 55 ms rate limit on async IRQ0 pinned the timer to 18 Hz** while the game asked
     for 180. Removed. Per-3s delivered ticks went ~3 -> ~540.
  3. **Timer ticks were coalesced by a boolean pending flag** and dropped whenever the guest
     was CLI'd (which is most of the time -- it CLIs around 256-colour palette writes, 585k
     writes to 0x3C9 per run). Now a saturating count (cap 4), like a real 8259 latching.
     Delivery is now 540/541 per 3 s against a programmed 180 Hz.
  4. **Keyboard IRQ1 never got the async path**, so input arrived whenever the game yielded.
  Plus: default IRET stubs for IRQ2-7/8-15 (their vectors pointed at unowned ROM); never
  deliver a line whose vector is still that stub (Skyroads installs no SB ISR, and delivering
  derailed it); INT 21h AH=00 implemented; audio thread at TIME_CRITICAL with 6 buffers
  (starvation by the 100%-busy exec thread was heard as ticking).
  ► METHOD NOTE worth keeping: every one of these was found by MEASURING, not reasoning --
    per-3s heartbeat deltas showed the game doing all its work in 3 s then idling at ~1 Hz,
    and a port histogram showed the real load was the palette. My first port histogram was
    WRONG (12 slots, filled before the hottest port appeared) and my first slowness diagnosis
    was wrong with it. Widen the instrument before trusting the reading.

★★★★ THE BLOCKER IS BROKEN: **WE CAN NOW INTERRUPT A SPINNING V86 GUEST.** Three sessions
were stuck behind this. The kernel will not do it (VdmQueueInterrupt is transition-only --
proven, see below), so we do it ourselves: **a suspended thread's CONTEXT is readable and
writable even while it sits inside VdmStartExecution, and for a V86 thread that context IS
the guest's frame.** `async_inject_irq()` (src/host/main.c) does from the device thread
exactly what the CPU does on a hardware interrupt -- push FLAGS/CS/IP on the guest stack,
vector CS:EIP through the IVT -- via SuspendThread / GetThreadContext / SetThreadContext /
ResumeThread. Commit `c1e5c34`, host v116, opt-in behind **qimode bit 4** (`10`).
  ► Guard rails (we are rewriting a context the kernel is actively running): only when
    EFLAGS.VM is set; only when the guest's interrupts are on (**IF or VIF**); never while CS
    is our own handler segment; and never unless the exec thread is inside `v86_run`
    (`g_in_exec`), so it cannot race the exec loop. Declines are counted (`async_bail`).
  ► **The timer needed this as much as the devices did** -- arguably more. IRQ0 now takes the
    async path too, rate-limited to the 55 ms BIOS tick.
  ► MEASURED: `qirq.com` reports **c0d=03** -- three interrupts delivered into a guest
    spinning in a pure memory loop that cannot trap -- then exits cleanly. **selftest 8/8
    with the path ON**, PIT case included.
  ► ★★ **SKYROADS' INTRO NOW ANIMATES.** It no longer freezes: the full 30 s runs with
    `io_events` still climbing and the BIOS tick advancing, the host exits cleanly instead of
    being force-killed, and **8+ distinct captured frames** show the ship flying from the
    foreground away down the road (session 10 got 3 frames then a static screen). PNGs in
    build/shots/shot_skyroads_shot*.png.
  ► DIAGNOSTIC WORTH KEEPING: the IVT dump at injection time showed **Skyroads never installs
    an SB ISR at all** (vectors 0x0B-0x0F all still point at unowned BIOS stubs). It was never
    waiting on the Sound Blaster -- it was waiting on the TICK. Injecting IRQ 5 alone just
    vectored it into ROM at F000:A390. Do not assume a game's device IRQ is what it wants.

★★★ SESSION 10'S DIAGNOSIS STANDS -- I briefly concluded otherwise and the instrumentation
refuted me; the corrected story is below. Mid-session I saw Skyroads' 4.5M port-I/O traps
and concluded the exec loop had millions of injection points it was refusing. It does not.
A refusal log at the gate proves `irqn_refused = 0` for the whole run: **we never decline
anything**. The 4.5M traps all happen in the first 6 s, before the transfer exists.

★★★ THE MEASURED SKYROADS TIMELINE (heartbeat, bare metal -- this is the useful artifact):
    ~8.5 s  SB block programmed: len 0x7d64 (32,100 bytes) @ rate 0x1788 (6024 Hz)
    8.5-13s block_left drains at EXACTLY the programmed rate (0x665a -> 0x23c0 in 3 s)
    ~13 s   `io_events` FREEZES at 0x45aba0 and never moves again; guest parked at
            DOS_HDLR_SEG:0x0037 -- the `CD 1C` in our INT 08h stub, i.e. it has entered its
            own INT 1Ch handler and spins there taking no traps at all
    ~14 s   block completes: blocks=1, raised[5]=1, sb_irq=5 -- the IRQ IS raised, ONE
            SECOND AFTER the guest stopped trapping, into an exec loop that can never run
  ⇒ **At the instant that matters there is genuinely no injection point.** Async delivery
    is required, exactly as session 10 said.
  ► THE SILVER LINING IS REAL: DMA -> SB -> mixer is **functionally correct end to end**.
    The block is programmed, drained at the right rate, and completed. Only the completion
    interrupt cannot reach the guest. That is a much smaller remaining gap than it looked.

★★★ WHY WE REFUSE: **VME. The guest's STI sets VIF (bit 19), not IF -- and every gate we
have reads IF.** Virtual Mode Extensions are enabled for our VDM; that is PROVEN, not
assumed: the kernel sets EFLAGS.VIP in our guest's frame, a branch it only takes when
`KeI386VirtualIntExtensions` has V86 VME on. Under VME a V86 guest's CLI/STI never touch
IF. So a game that enables interrupts by EXECUTING STI -- rather than by inheriting IF=1
from the entry EFLAGS (session 10's fix, which is why programs that never STI worked) --
looks permanently interrupt-disabled to us. Gates are now `IF || VIF` (commit `b7bc111`). This is correct on VME hardware and selftest
stays 8/8, but **it is not what unblocks Skyroads** -- see the timeline above.
  ► ALSO TESTED AND REFUTED, do not repeat: an entry trampoline that makes the guest execute
    a real `sti` (`sti; jmp far <entry>` at DOS_HDLR_SEG:0x60, qimode bit 3) so VIF is set
    the only way the CPU accepts. No delivery. And qirq.com already executed its own `sti`
    before spinning, so the earlier runs had refuted this before I built it.

★★ THE ASYNC LEVER, RE'd FROM XP's KERNEL (full write-up + every address in
docs/research/dpmi-under-ntvdmcontrol.md, "Runs 87-93"):
  • **`NtVdmControl(VdmQueueInterrupt=1, ServiceData)` -- ServiceData is a THREAD HANDLE**,
    not a pointer. It queues an APC to that thread, which IS the preemption we lacked.
    Rig-confirmed accepted (`st=0`) and its APC demonstrably runs.
  • The APC's gate `VdmpCanDeliver` (0x56dce0) reads **VIF** on VME hardware, not IF -- the
    same root cause as above. Every run stopped there: it sets VIP and defers.
  • The kernel emulates a **full 8259 in memory we hand it at VdmInitialize**, which we had
    been passing ZEROED -- so it could never dispatch anything. Layout now recovered and
    programmed (master 0x08 / slave 0x70): `v86_ica_raise/eoi/set_mask/state`.
  • `VTIB+0x5A8 = 3` is written by that same APC -- session 10's event-3 "interrupt pending"
    reflect, seen from the kernel side.

★★★ A SESSION-10 LINE WAS WEDGING GUESTS: `*(DWORD*)0x714 |= 1` (VDM_INT_HARDWARE) in
`host_irq_sink`. With VIP set and VIF clear the guest's next IRET faults under VME into a
dispatch that refuses to deliver and re-arms VIP -- the guest froze at `DOS_HDLR_SEG:0x0003`
with the exec loop starved and NO exit path running. Reproduced in every run that set the
bit **including the control that made no queue call at all**. Removed; the same probe then
runs to a clean `INT 21h 4Ch` exit. (It is NOT what freezes Skyroads -- tested directly,
v104 froze identically. Do not conflate the two.)
  ► Also measured and worth not repeating: bit 9 (0x200) of `[0x714]` is the VDM's virtual
    interrupt flag and **the KERNEL already maintains it** (read set from the first
    instruction). Writing it from user mode only clobbers correct state.

★ NEW TEST + TOOLING
  • `tools/dostest/qirq.asm/.com` -- async-IRQ probe: hooks INT 05h and INT 0Dh with separate
    counters (which one fires tells you WHICH kernel path delivered), then waits in a pure
    memory spin that cannot trap, so any vector that fires was delivered asynchronously.
  • `qimode.txt` on the share = a no-rebuild knob (hex digit: bits0-1 = `[0x714]` bits to set,
    bit2 = raise a periodic IRQ 5, bit3 = start the guest with VIF). **Absent = everything off**,
    so normal runs are untouched. Delete it before non-experiment runs.
  • A headless **heartbeat** (always on) logs guest CS:IP/EFLAGS + counters every 500 ms. This
    is what turned "the log just stops" into a timestamped last known position.
  • Fetching files off the box for RE, no physical access:
    `printf 'dpmitest.com&copy C:\\WINDOWS\\system32\\ntoskrnl.exe "<share>\\re_ntoskrnl.exe"\r\n' > cmd.txt`
    (the watcher interpolates cmd.txt into a command line, so `&` chains a command after rt.bat).

▶ DEAD LEADS -- CLOSED BY MEASUREMENT/DISASSEMBLY THIS SESSION, do not re-open:
  • **`VdmQueueInterrupt` as an async lever** (see above -- transition-only, tested on the real
    Skyroads IRQ). The RE of it is still valuable and stays documented; the *use* is dead.
  • **`VdmPMCliControl` (service 13) is PM-ONLY.** ServiceData is a pointer to a subfunction
    dword: {0,1} clear/set bit 0 of the word at VdmObjects+0xBA (the PM client's virtual CLI),
    {2} = the CLI-timeout watchdog that force-sets `[0x714] |= 0x200`, {3,4} dispatch helpers.
    It never touches the V86 VIF. (The previous checkpoint called this the top lead. It isn't.)
  • **`VdmDelayInterrupt` (service 2)** takes a 12-byte struct keyed by IRQ line -- it is the
    ICA's delay/undelay machinery (`pDelayIrq`/`pUndelayIrq`/`pDelayIret`), not an async timer.
  • Setting EFLAGS.VIF via the VTIB CONTEXT (sanitised away), and making the guest execute a
    real `sti` (above). Neither produces a kernel dispatch.

★★★ THE ASYNC LEVER IS CLOSED -- `VdmQueueInterrupt` CANNOT PREEMPT A RUNNING V86 GUEST.
Tested at the only moment that matters: Skyroads with the lever armed on its REAL Sound
Blaster IRQ (qimode `9`, no artificial raiser). `qi_calls=1`, status 0, fired exactly at the
block completion -- and nothing happened: `state714` went to `...31` and STAYED, `io_events`
stayed frozen, `irqn_inj` stayed 0, for the remaining 15 s. Matches the disassembly: the APC's
first pass always requeues itself as a **user-mode** APC (0x46fead; it is entered with
NormalContext = 0 and only a non-zero NormalContext reaches the dispatch), and a user APC is
never delivered to a thread spinning inside VdmStartExecution. **The service is
transition-only.** Do not spend more time on it. Probe: `tools/dostest/qirq2.asm`, which
retargets the kernel's PIC to base 0x60 so a KERNEL-delivered IRQ 5 arrives as INT 65h and a
HOST-delivered one as INT 0Dh -- previously the same vector, hence unattributable.

★★ A REAL BUG FIXED ON THE WAY: `inject_int` pushed a **16-bit** FLAGS word, truncating away
EFLAGS.VIF -- so the guest's IRET restored its virtual interrupt state from a zero bit and came
back with interrupts off PERMANENTLY. Measured on qirq2: after the first injected INT 08h,
`irq0_inj` stuck at 1 for the entire run. The CPU folds VIF into the pushed IF itself when it
vectors a hardware interrupt; we synthesise the frame, so we must too (commit `8d42d15`).
This did NOT unblock Skyroads (its timer was already flowing, 483 ticks), but it was silently
disabling interrupts for any guest whose enable lives in VIF.

★★★★ **ROUND 3 (host v165): DOS API 102/103 = 99%, BIOS ~70%, OVERALL ~85%.**
  ▶ **INT 21h: only 4Bh EXEC remains.** Added this round: the FCB group (0Fh-24h, 27h-29h),
    1Bh/1Ch/1Fh/32h drive params, 26h/55h PSP creation, 31h TSR, 37h switch char ('/'),
    53h, 5Eh/5Fh, 64h, 66h code page (437), plus the earlier file/handle batch.
    **All five real 6.22 apps report `INT21 unimplemented: none`** — MEM, CHKDSK, TREE,
    ATTRIB, COMMAND.COM.
  ▶ **BIOS: eight interrupts planted** (11h/12h/13h/14h/15h/17h/25h/26h) — every one was a
    bare IRET before, handing the caller its own registers back. New STAGE2 line reports
    partial/unimplemented BIOS services.
  ▶ **#39 VIDEO MODES: the epic's named defect is fixed.** There is now a MODE TABLE
    (`vid_modes[]`): text 0/1/2/3/7 with real geometry (mode 0 is 40 columns, not 80), planar
    0Dh/0Eh/0Fh/10h/11h/12h with per-mode resolution, linear 13h. The renderer follows
    `st->mkind` + `st->gw/gh` instead of branching on 12h/13h only, so **11h no longer shows a
    text screen while the program writes pixels**. CGA 4/5/6 are marked UNSUPPORTED and LOUD
    rather than approximated -- their two-bank interleaved B800 layout shares nothing with the
    planar path, and quietly showing text is the failure mode #27 exists to remove.

★★ **TWO SCAFFOLDING BUGS FOUND THIS ROUND, both of which would have corrupted future work:**
  1. **DOS calls that RETURN A SEGMENT IN DS** (1Bh, 1Ch, 32h, 52h) broke the probes' own
     output: every probe store is DS-relative, so the probe wrote its state into DOS's segment
     and printed labels read from there -- the dump came out as unlabelled hex, then as
     fragments of executable code. Fixed IN `probe_capture` (restores DS=CS on exit, after
     capturing the guest's DS faithfully), not per-probe, so it cannot be forgotten.
  2. **The oracle harness decoded helper output as UTF-8**, so any probe whose buffer dump held
     a byte above 0x7F aborted the whole run with a UnicodeDecodeError. Now CP437. The harness
     must never be the thing that fails on unusual data — that is the data worth seeing.

★★★★★ **#30 EXEC (4Bh) WORKS — INT 21h IS 103/103. host v166.**
    EXEC: "P_CHILD.COM"
    EXEC: child at seg=0x1101 entry=1101:0100 (COM) depth=01
    EXEC: child exited rc=0x2a, parent resumed (depth=00)
  Oracle-matched on all three properties that matter: the child RAN, the parent RESUMED at the
  instruction after its INT 21h, and the child's EXIT CODE came back through AH=4Dh.
  ▶ **HOW THE RETURN WORKS — the load-bearing idea.** The parent entered via `INT 21h`, so the
    CPU pushed FLAGS/CS/IP on ITS stack and we are inside our BOP stub. We snapshot the parent's
    ENTIRE frame — including CS:IP pointing AT the BOP and SS:SP pointing at that IRET frame —
    then overwrite it with the child's entry state. On child exit we put the frame back and step
    EIP past the BOP, so the stub's own IRET pops the parent's own frame and lands exactly where
    EXEC returning normally would have. **No stack is unwound by hand.** Nesting stack is 8 deep
    (`g_exec[]` in main.c); child memory is freed on exit.
  ▶ SPLIT BY LAYER: `dos_int21` only RECORDS the request (path + parameter block) and sets
    `exec_pending`; the host's `exec_begin()` does the load and the transfer, because the loader,
    file I/O and the guest register frame all live there.
  ▶ **A .COM OWNS ALL OF MEMORY, so EXEC returns AX=0008 until the parent gives some back.**
    The probe shrinks itself with 4Ah first — that is not a workaround, it is what COMMAND.COM
    does before launching anything. Worth knowing before diagnosing an "out of memory" EXEC.
  ▶ AL=01 (load-without-execute) and AL=03 (overlay) are LOUD-unimplemented, not silent.
  ▶ NEW: probes can ship companion files via a `<probe>.deps` sidecar, and all three hosts now
    run a probe FROM ITS OWN DIRECTORY so a relative companion path resolves everywhere.

★★★★★ **ROUND 4 (host v170): DOS 103/103, BIOS COMPLETE. ALL 15 PROBES CLEAN.**
  ▶ **#39 CGA modes 4/5/6 now RENDER.** They were the last "unsupported" modes and the layout
    is why: rows INTERLEAVE between two 8 KB banks at B800 (even rows from 0, odd from 0x2000)
    and pixels are 2 bits (4/5) or 1 bit (6), packed high-bit-first. Nothing is shared with the
    planar path — approximating them with a text screen was never going to work.
  ▶ **#41 palette complete**: 10h AL=00/01/02/03/07/08/09/13/15/17/1A/1B, plus **0Bh** (border +
    CGA palette), **0Dh** (read pixel), **07h** (scroll DOWN — 06h scrolled up and 07h fell
    through to nothing, so downward scrolls silently did nothing), **1Ch** save/restore state.
  ▶ **#40 character generator**: 11h AL=x1-x4 ROM font selection and AL=20-24 graphics font
    pointers. User-font LOADS (AL=x0) are LOUD — we render from our own tables, so accepting a
    user font would silently draw the wrong glyphs.
  ▶ **#42 VESA complete**: 4F03/06/07/08/09. 4F0A (PM interface) and 4F15 (DDC) report NOT
    SUPPORTED rather than returning success with a null pointer a client would call into.
  ▶ **#46 INT 20h/27h/28h/29h planted.** 29h (fast console out) was an IRET that swallowed
    output silently.

★★ **A REGRESSION I CAUSED AND THE SELFTEST CAUGHT — the reason that gate exists.**
  I planted INT 20h with **BOP number 0x20 — which is ALREADY the INT 21h handler's**. The new
  BIOS dispatch sits ahead of INT 21h, so it intercepted EVERY INT 21h call as "terminate
  program": selftest exited at its first DOS call with no output at all. BOP numbers are a
  SHARED NAMESPACE across DOS, BIOS, XMS (0x43) and DPMI (0x50-0x57). INT 20h now uses BOP 0x30
  and the table is {vector, bopnum} pairs so the two can differ. **Check the namespace before
  adding a BOP.**

▶ **PROBE HYGIENE, prompted by making every host run from its own directory:** several probes
  were comparing values that describe WHERE THE PROBE IS rather than what DOS does — the default
  drive, the current directory, the volume serial, truename's base. Those are now dumped but not
  compared, with the reason recorded. Also dropped: AX after 47h/60h/69h, which RBIL documents as
  destroyed and which the hosts duly disagree on.
  ► And a real fidelity fix it exposed: FCB open now fills in the RESOLVED DRIVE (DOS replaces a
    "default drive" 0 with the actual drive; we were leaving the caller's 0).

▶ **54 recorded rationales** in `tools/dostest/oracle-rules.json`. Every DOSBox divergence is
  explained, and in each case **we match the genuine kernel** — including the CP437 collating
  table, where DOSBox fails to fold lower case onto upper and ours is byte-identical to 6.22's.

★★★★ **MODE 12h: ROOT-CAUSE HUNT. One real deadlock FIXED; the remaining blocker is
KERNEL-SIDE and is the next piece of work.**

▶ **SCOPE, from the QuickBASIC demos in `demos/` (sources in `demos/src`):**
    SCREEN 12 — BLIT, BOUNCEBX, BUBBLES, MATRIX_1, MATRIX_2, MOUSE   **6 of 10, all broken**
    SCREEN 13 — CAVE, GFXCOPY, PALETTE                                 work (PALETTE confirmed)
    SCREEN 0  — VS87
  Matches the user's recollection exactly ("the mode 12h ones never did, at least not very
  well"). Skyroads is 13h, which is why our one game never touched this path.

★★★ **FIXED: `host_interp()` ran up to 2,000,000 guest instructions WITH NO WAY TO TAKE AN
INTERRUPT.** BLIT's outer loop is `DO WHILE INKEY$ = ""` — it can only END when an interrupt
fires. Escalated to the interpreter, it burned the whole cap, returned, re-faulted,
re-escalated. **TEN I/O events in thirty seconds.** The interpreter is standing in for the CPU
and a real CPU takes interrupts mid-loop, so it now checks for a pending IRQ every 256
instructions and yields. **15x improvement (10 -> 157 events, 8x more pixel data).** selftest
still 8/8. That fix stands on its own regardless of the rest.

★★★ **THE REMAINING BLOCKER: ARMING THE A0000 PAGE TRAP STOPS THE GUEST RUNNING.**
    trap ON  -> io_events = 10,          guest frozen at 0050:0037 in 58/60 heartbeats
    trap OFF -> io_events = 22,532,292,  guest running QB code, PC moving every sample
  `PAGE_NOACCESS` and `PAGE_READONLY` behave IDENTICALLY, so it is not reads-vs-writes: it is
  protecting that range at all. Diagnostic knob added: **`noa000.flag` on the share** disables
  the trap (absent = normal). Delete it after use.
  ► With the trap off the guest's real inner loop is visible at its PC:
    `DEC DX / MOV AL,07 / OUT DX,AL / INC DX / MOV AL,0F / OUT DX,AL` — per-pixel VGA register
    reprogramming, exactly what the batching interpreter exists to absorb.

▶ **RULED OUT BY MEASUREMENT — do not re-investigate these:**
  • **The mode table** (#39). Resolves 12h correctly: `mode=0x12/kind=01/640x480`. New
    `STAGE2: mode sets:` line proves it.
  • **The planar write engine.** Complete and correct — 4 write modes, set/reset, ALU, bit
    mask, latches. I nearly rewrote working code TWICE on the strength of a screenshot.
  • **The IVT.** `ivt08=0050:0034 ivt1C=0050:003a`, and QuickBASIC has NOT hooked either.
  • **Async IRQ injection.** 545 successes, **zero bails**, zero nest-blocks.
  • **The "mode-12h MOV-store decoder gap"** from the M3 notes: `interp-refused=0`. The
    interpreter never declines an opcode. That lead is DEAD.
  • **Unhandled events.** None — no `STAGE2: stop event` line; every event is serviced.

▶ **THE LIKELY SHAPE OF THE ANSWER.** The M3 planar trap was **VM-confirmed on HVF, never on
  real hardware**, and there is precedent for exactly this class of difference: session 8 found
  HVF reflects IOPL-0 I/O as event 0 while real silicon uses event 3. So the A0000 trap may
  simply never have worked on the rig. Next step is kernel-side: **why does VirtualProtect on
  A0000 stall `VdmStartExecution`** — same class of work as the #18 reflect RE, not a patch.
  ► If that proves hard, the alternative is to stop trapping altogether and drive the
    INTERPRETER from mode set. It already runs the guest correctly and now yields properly, and
    it needs no page protection at all.

▶ **A METHOD NOTE WORTH KEEPING.** I called this "a regression I introduced today" on the
  strength of our new output differing from our old. **Neither was correct** — the oracle showed
  16 colours, both builds showed 2. Different is not wrong when nothing is right. The reference
  is the ORACLE, never our own previous build; one oracle run settled in seconds what an hour of
  comparing our own screenshots could not. (`dosoracle.py run BLIT.EXE --timeout 22 --screenshot`
  — the timeout path is currently the only way to get pixels out of the oracle.)

▶▶ RESUME — NEXT STEPS (in order):
  1. **SOAK THE ASYNC PATH, THEN MAKE IT THE DEFAULT** (drop the qimode bit 4 gate). Run the
     whole tests/ battery with it on -- especially the graphical demos, the DPMI/PM tests (it
     must never fire while the guest is in PM: the VM check covers that, verify it does), and a
     long Skyroads run. Watch `async_bail`: a high bail count means the guard rails are refusing
     more than they should. The one real risk is a torn context if a guard is wrong, so look for
     any run whose guest wanders to an unexpected CS:IP.
  2. **Skyroads is playable -- now judge it against the bar.** Remaining user-reported
     roughness after v126 (all UNVERIFIED by me; they need ears/hands on the box): is the
     music now smooth and correct tempo, is the PCM still pitched low (if so that is a
     separate SB rate bug -- suspect the mixer's resample ratio, NOT the timer), and is input
     latency acceptable. Then: a game that DOES install an SB ISR, to exercise the device-IRQ
     half of async delivery end to end (Skyroads never installs one).
  3. (was 2) Drive Skyroads further -- keyboard input through the
     menu, as run 86 did. Then the sound epic's real acceptance test: a game that DOES install an
     SB ISR (Skyroads does not -- see above), so the device-IRQ half of async delivery gets
     exercised end to end.
  4. PIC VDD claiming 0x20/0x21 is a PREREQUISITE for the kernel ICA path, not a nicety: the ICA's
     ISR bit stays set until an EOI clears it (`v86_ica_eoi`), or that line never fires again.
     Skyroads already writes EOI to 0x20 and it goes nowhere (still in `unclaimed ports`).
  5. Calibrate the OPL envelope rates (`OPL_EG_ANCHOR`); decide on un-folding SB stereo.
  6. `git push` -- 33 commits are sitting local.

▶ HARNESS GOTCHA (cost me a wrong conclusion this session): **rt.bat copies the log while the
  host may still be finishing, and SMB caches the result.** A `result_*.log` read too early is a
  PARTIAL file -- I read one that was missing its last lines and concluded a counter was absent.
  After `cmd.txt` disappears, wait ~25-40 s, `ls` the share to force a readdir, and sanity-check
  the version string and a known-final line (`STAGE2: complete` or the HEADLESS report).

▶ OPEN QUESTION (unexplained, low priority): in the runs that set the hardware-pending bit, the
  process ended at ~8 s having reached NO exit path -- not the guest's 4Ch flush, not the 30 s
  deadline backstop's report. The heartbeat proves it was alive until then. Kernel-side VDM
  termination is the obvious suspect. Harmless now that the bit is gone.


═══════════════════════════════════════════════════════════════════════════════
██  PRUNED: SESSIONS 6, 8, 9, 10 (stale restart snapshots)                     ██
═══════════════════════════════════════════════════════════════════════════════

  These were "RESTART POINT" blocks — HEAD hashes, unpushed-commit counts, working
  -tree state on branch `spike/dpmi-16bit-switch`. Every one of those facts is now
  WRONG, and stale operational state in a rehydration doc is worse than none.
  Removed 2026-08-22; recoverable from git history. What they established that still
  matters is kept in memory and in docs/research/dpmi-under-ntvdmcontrol.md:
    * **session 8** — the pivot to BARE-METAL testing (QEMU+HVF SIGABRTs on DOS/4GW
      paged 32-bit PM); real mode 8/8 on real silicon.
    * **session 9** — THE CRACK: 16- AND 32-bit DPMI real-CPU PM RUN on bare metal.
      Session 8's "the kernel won't run PM" was OUR OWN `dpmi_enter.S`
      interrupt-pending guard firing on a STALE `[0x714]&3`; fix v68 `c0831b1`.
      (Referenced from memory as "return-ntvdm.md session-9" — full detail is in
      docs/research/dpmi-under-ntvdmcontrol.md, runs 65-79.)
    * **session 10** — host v97 iteration on the same track.
    * **session 6** — power-down snapshot; runs 78-79 (async IRQ0 injection, the
      D/B-aware `host_try_io_pm`).
  The KD / GH #18 history that sat at the end of this file is likewise in
  [[kd-guest-debugger-ops]] and the research doc — including run 71, whose verdict
  (a raw PM #GP silently terminates the VDM) is quoted in DO-NOT-RE-SPEND above.

═══════════════════════════════════════════════════════════════════════════════
██ CHECKPOINT — 2026-08-18 (session 7). ██
═══════════════════════════════════════════════════════════════════════════════

BIG PICTURE: the 32-bit DOS/4GW real-CPU foundation is now PROVEN END-TO-END and
ALL PUSHED. GH #18 is broken for both 16-bit (runs 72-78) and 32-bit (runs 80-82).

REPO STATE (branch `spike/dpmi-16bit-switch`): **all pushed to origin, 0 unpushed**
  (HEAD = `274b4aa`). New this session: `f86e850` (runs 80-81 + run 78 confirm),
  `274b4aa` (run 82). Working tree clean except pre-existing untracked (MAINICON.ico,
  demos/, scripts/kd_*.py, scripts/trace_break.py, vm/ artifacts) — NONE mine; leave them.
  Host builds clean: `./scripts/build.sh` → build/ntvdmhost.exe (has runs 80-82).

WHAT WAS DONE + VM-CONFIRMED THIS SESSION (all now FACT; docs runs 78/80/81/82,
  [[dpmi-realcpu-pm]] memory):
  • run 78 = async IRQ0 injection into a hooked PM INT 08h — CONFIRMED (tmrhook box
    marched off the injected ISR, no polling; user saw it live). The timer-hook path Doom needs.
  • run 80 = a 32-bit (CS.D=1) PM `OUT` reflects as event 0 and is serviced — the run-72
    gate holds for 32-bit. Prereq: fixed INT 31h 0009 D/B parse (was CH low nibble → now
    `(ECX>>12)&0xF`). Probe tools/dostest/pm32io.asm.
  • run 81 = the DPMI mode SWITCH itself produces a working 32-bit CS: `dpmi_switch_to_pm`
    honors client_is_32bit (D/B=1 CS/DS/SS); main.c derives is32=EAX&1 + mirrors to g_ldt[];
    INT 2Fh 1687 now BX bit0=1. No 16-bit regression. Probe pm32sw.asm.
  • run 82 = a 32-bit renderer: mode 13h + grayscale DAC palette (768 PM OUTs) + a `rep stosd`
    vertical-gradient framebuffer fill → the Luna window showed the ramp. Probe pm32gfx.asm.

VM STATE (IMPORTANT): the XP VM was **left RUNNING** with 2 idle DOS windows (pm32sw +
  pm32gfx). An IDE restart likely orphans/kills that qemu. ON RESUME, start clean:
  `pkill -9 -f qemu-system-x86_64` then `./scripts/xp-vm.sh run` (boots the CURRENT
  vm/xp.qcow2, which is the freshly-restored clean debug-only image from this session).
  If it wedges on the logon logo: `cp vm/xp-debugonly-backup.qcow2 vm/xp.qcow2` then relaunch.

▶▶ RESUME — #3 remaining work for a REAL extender + REAL game (run-79 inventory items 3-5),
  needed once a 32-bit client uses callbacks / async timer hooks / >64K offsets:
  1. Widen the two catcher IRET frames (`dpmi_run_callback`, `dpmi_inject_pm_irq`) + the
     injected-IRQ frame to DWORD EFLAGS/CS/EIP when the target selector is 32-bit
     (`dpmi_sel_is32`). The catcher/trampoline CODE selectors must have D/B matching the client.
  2. Gate the `EIP/off & 0xFFFF` masks in the PM loop / `dpmi_service_pm_int` / `g_pm_int[].off`
     / buffer-offset reads on `dpmi_sel_is32` (note `g_int_vec[]` is only 0x10000 wide — a
     32-bit fault EIP>64K needs a different dispatch key).
  3. Base-0 ~2GB G=1 flat-selector ALLOC test (INT 31h 0000/0007/0008/0009) — the DOS/4GW flat
     model; verify it installs (dpmi_install already does the >1MB→G=1 path) and executes.
  4. Then a real DOS/4GW extender, then a real game (the acceptance test).

HARNESS LESSONS (hard-won this session): **ONE VDM/probe at a time** — 4 concurrent VDMs on
  this time-dilated HVF guest REBOOTED XP, which then wedged >6 min on the dirty-boot logon
  (autochk at 102% CPU) → forced a restore. Prefer a FRESH BOOT per probe. A looping/idle prior
  probe also holds `\\.\COM1`, so later probes get no serial → the SCREENDUMP is authoritative.
  Autorun CD trigger: fresh volume LABEL each mount (`autorun.inf` `open=<run>.bat`), hot-swap via
  `python3 scripts/qmp.py cd <abs-iso>`. Only ONE qemu at a time. See [[vdm-host-test-harness]].
═══════════════════════════════════════════════════════════════════════════════

