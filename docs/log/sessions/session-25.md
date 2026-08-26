# Session 25 — 2026-08-25

> Archived verbatim from `return-ntvdm.md`, which was the single rolling handoff
> file until 2026-08-26. Split by session so it can be read by topic and date
> rather than by scrolling. **Nothing has been edited** — including conclusions
> that later sessions REFUTED, which are kept on purpose: the refutations are
> some of the most valuable material here.

```
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
```
