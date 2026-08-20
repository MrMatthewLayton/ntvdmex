# Demo sweep — observed defects (2026-08-20, session 14)

Live observation by the user at the physical box, one demo at a time, after mode 12h
started rendering (`b3abbce`). **These are watched-on-the-screen findings, not log
readings** — several of them are invisible in a screenshot or a counter, which is the
whole point of running the sweep this way.

Status key: OBSERVED = seen and reproduced live; not yet diagnosed unless stated.

---

## 1. BOUNCEBX — vertical retrace is not paced, so animation runs far too fast

**OBSERVED.** The 40×40 box moves so fast it "rarely appears as a box" — it tears in
the Luna window instead of animating smoothly.

**Where it comes from:** `demos/src/BOUNCEBX.BAS:42-43` paces its frames with

    WAIT &H3DA, 8
    WAIT &H3DA, 8

`WAIT port, mask` blocks until `INP(port) AND mask` is non-zero — i.e. it waits on bit
3 of Input Status 1, the VGA vertical-retrace bit. That is the program's entire frame
clock.

**Why it does not pace us:** `video_state.retrace` (`src/vdd/vdd_video.h`) is a bit we
*toggle on every read* of 0x3DA, so that a guest polling for retrace always makes
progress and never spins forever. That was the right call when the alternative was a
hang, but it means the bit has no relationship to time: `WAIT` returns almost
immediately, every frame, so the guest's frame rate is bounded only by how fast we can
execute it. On real hardware it would be bounded to ~70 Hz.

**The fix shape** (not yet done): drive the retrace bit from the same
QueryPerformanceCounter clock `host_pit_sync()` already uses, so 0x3DA reports retrace
active for a realistic fraction of a ~70 Hz period rather than alternating per read.
Anything that polls 0x3DA for pacing — which is most DOS graphics — depends on this.

**Scope: this is NOT a mode-12h defect.** It is a timing defect that mode 12h merely
made visible, and it will apply equally to 13h and to games. Skyroads polls 0x3DA too
(session 11 recorded its vblank `IN AL,DX`), so this is worth checking against the
"a little sluggish" calibration before assuming the two are unrelated.

---

## 2. BUBBLES — also far too fast, but for a DIFFERENT reason. Do not conflate.

**OBSERVED.** Renders correctly (greyscale starfield — see below) but runs "really
fast". No visible tearing, unlike BOUNCEBX.

**It is NOT finding #1.** The user's guess was that this one also waits on vsync; it
does not. `demos/src/BUBBLES.BAS` contains **no pacing of any kind** — no
`WAIT &H3DA`, no `INP`, no timer gate, only `RANDOMIZE TIMER` on line 2. It is an
uncapped draw loop, so on real hardware it runs as fast as the CPU allows too, and a
period-correct 386 is the only thing that ever slowed it down.

⇒ There are **two distinct speed defects** in this sweep and they need different fixes:
   - **#1 BOUNCEBX**: a program that ASKS to be paced (`WAIT &H3DA,8`) and we do not
     honour the request. Fix = make the retrace bit time-based.
   - **#2 BUBBLES**: a program that never asks. Nothing in the emulator can pace it
     except overall execution rate. Fix = #3 below.

**Confirmed correct, do NOT "fix" it:** the greyscale is the program's own. BUBBLES
installs `PALETTE index, index*4` over all 16 entries before drawing, so grey squares
on a light background is the right picture. This makes it standing evidence for the
palette path.

---

## 3. FEATURE REQUEST (user, 2026-08-20): configurable approximate CPU speed

**Not a defect — a product decision, stated while watching BUBBLES.** The user wants a
**dropdown in the menu that sets an approximate CPU speed** — 33 MHz, 66 MHz, 100 MHz,
200 MHz and so on — accepting that we run on the real CPU.

**Why it matters more than it looks:** finding #2 proves the emulator has NO other
lever over an uncapped DOS program. A large class of period software has its speed
baked in to the machine it was written for, and "runs too fast to use" is a correctness
problem for the user even though every instruction is executed correctly. This is also
squarely superset territory — XP's own ntvdm offers nothing like it.

**Implementation notes for whoever picks it up:** we now have two very different
execution paths and the throttle has to cover both — V86 on the real CPU (where the
only lever is inserting waits at the points we already get control: port traps, BOPs,
the IRQ0 gate) and the mode-12h interpreter (where we count instructions ourselves and
can pace directly against QueryPerformanceCounter, which is the easier half). Calibrate
against a period benchmark rather than a guessed instructions-per-second number, and
remember the existing calibration point: Skyroads currently feels like "a 386 8 MHz /
512 KB running software specced for a 386 16 MHz".

---

## 4. MATRIX_1 — correct speed, but NOT for the reason first recorded

**OBSERVED, no defect:** "works and is about the speed I expect." It is also the only
demo that exercises **text glyphs in a planar graphics mode** at scale rather than
filled rectangles — standing evidence for the real 8×8/8×14/8×16 ROM font dumps from
session 12.

**★ A CORRECTION, kept because the mistake is instructive.** This was first written up
as "MATRIX_1 has no pacing and comes out right because the WORK paces it, therefore our
throughput is right." That was wrong, and it was wrong because the check was a grep for
`WAIT`/`SLEEP`/`TIMER`/`INP`. `MATRIX_1.BAS` paces itself with a **software delay
loop** the grep never looked for:

    LOCATE ... : COLOR ... : PRINT CHR$(...)
    FOR counter = 0 TO delay
    NEXT

So it is paced, just not by anything the emulator can see. "Right speed" therefore means
only that **our execution rate lands near the rate the author tuned `delay` for** — a
coincidence worth knowing, not proof that our throughput is correct. *Absence of the
idiom you searched for is not absence of the behaviour.*

---

## 5. MATRIX_2 — PREDICTION CONFIRMED, and the pair inverts against stock NTVDM

The prediction in the previous revision of this file (written BEFORE the run, so the
diagnosis is falsifiable rather than fitted): `MATRIX_2.BAS` uses `WAIT &H3DA` at lines
54 and 67 where MATRIX_1 does not, so #1 says it should run too fast despite being the
more sophisticated of the pair.

**Outcome: CONFIRMED — "WAY too fast".** And the user supplied the reference that makes
it conclusive:

  ▶ **Under XP's own NTVDM, MATRIX_1 is FASTER than MATRIX_2 — the expected ordering,
    since MATRIX_2 redraws the whole 80×30 grid every round while MATRIX_1 touches one
    character at a time. Under NTVDMEX the ordering is INVERTED.**

That inversion is finding #1 with a second, independent program: stock ntvdm paces
MATRIX_2 to the retrace and we do not, so the heavier program outruns the lighter one.
(This is exactly the "stock ntvdm column" GH #26 exists to automate — here it was
supplied by eye, and it settled the question in one sentence.)

---

## 5a. THE PACING TAXONOMY — three classes, and only one is fixed by the retrace fix

Reading all four sources gives the whole picture, and it is what #3 has to be designed
against:

    class                  programs            fixable by
    -------------------------------------------------------------------------
    vsync-paced            BOUNCEBX, MATRIX_2  #1 (time-based retrace bit)
    busy-wait-paced        MATRIX_1            #3 ONLY (CPU-speed throttle)
    not paced at all       BUBBLES             #3 ONLY (CPU-speed throttle)

**Two of the three classes are unreachable without the CPU-speed control.** That moves
#3 from a nice-to-have to load-bearing: a period-correct retrace bit fixes half the
demos in this sweep and cannot, even in principle, fix the other half.

---

## 6. MOUSE — NOT A DEFECT. It has no main loop; exiting at once is correct.

Carried into this sweep as "the one demo whose behaviour is not yet explained": it set
mode 12h, returned to mode 3 and exited in ~3 s, producing ONE captured frame where
every other demo produced eleven. **Reading the source settles it — that is what the
program does.**

`demos/src/MOUSE.BAS` in its entirety: `SCREEN 12`; `PAINT (1,1), 3` (flood cyan);
`Mouse 0` (init); `Mouse 5` (show cursor); end of module. `DrawCursor` is **commented
out** and never called, `DrawDesktop` is never called, and there is no
`DO WHILE INKEY$` loop anywhere. It paints, initialises the mouse, shows a cursor and
terminates.

Both instruments agree with the source, which is why this closed without a rig
experiment:
  - the single captured frame is a cyan band with the rest black = the flood fill
    caught in progress;
  - `plane-nonzero=00009600/00009600/00000000/00000000` = planes 0 and 1 FULL
    (38400 bytes each) and planes 2-3 empty — which is exactly colour 3, cyan, over
    the whole screen. The fill completed; the screenshot merely beat it.

**What it does usefully exercise, and nothing else in the sweep does:** `CALL Absolute`.
It assembles a 57-byte machine-code thunk out of `DATA` statements into a BASIC string
and calls it, and that thunk is what issues the `INT 33h`. So it is a test of executing
guest code from a string in the data segment — now through the interpreter.

**★ THE ORACLE SETTLED IT, AND A PLAUSIBLE HYPOTHESIS WAS WRONG.** Watching the live
re-run, the reading was "the Luna window flashes and disappears — I suspect this is
because we are not implementing INT 33h and we probably should." Two checks, neither of
them an argument:

  1. **We DO implement INT 33h.** `mouse_int33()` in `src/host/main.c` handles AX=0
     (reset; reports driver-installed, 2 buttons), 01 show, 02 hide, 03 get position +
     buttons, 04 set position, 05/06 press/release info, 0Bh relative motion, with a
     host-drawn cursor overlay once the hide-count reaches 0. The run also reported
     `INT21 unimplemented: none` and `BIOS partial/unimplemented: none`.
  2. **Real DOS does the same thing.** `./scripts/oracle.sh demos/MOUSE.EXE` →
     **the program exits on its own after 2.9 s on genuine MS-DOS 6.22**, never
     reaching a 25 s screenshot timeout and printing nothing. Ours takes ~3 s.

  ▶ And the discriminator: **the oracle has NO mouse driver loaded, so its INT 33h is
    absent where ours is present — and both exit in the same ~3 seconds.** The exit is
    independent of INT 33h in both directions. Implementing more of it cannot change
    this symptom.

⇒ The open question is no longer "why does it exit" but the much smaller "does the
  cursor appear for the instant before it does".

⇒ **METHOD NOTE.** "A program flashes and vanishes" reads as a host failure and is
  almost never questioned, because the failing case and the correct case look
  identical from the outside. One 3-second oracle query separated them. This is the
  cardinal rule paying out in a new place: not a register value taken from memory, but
  a *behaviour* assumed to be broken because it looked broken.

---

## 7. INT 33h WORKS END TO END — proven with the right probe, not the wrong one

`MOUSE.BAS` cannot answer "does our mouse driver work" because it never loops: it
paints, calls the driver twice and exits. Three separate observations were spent trying
to read a mouse defect out of a program incapable of showing one.

`tools/dostest/mousetst.com` exists for exactly this and was written for it — mode 12h,
reset + show cursor, then a loop reading INT 33h AX=3 and writing a white pixel under
the cursor (INT 10h AH=0Ch) while the left button is held. **Live result: the cursor
tracks the physical mouse and the user can draw by holding the left button.**

That is position reporting, button state, AND INT 10h pixel writes through the new
mode-12h interpreter path, all confirmed in one interaction.

⇒ **Reach for the probe built for the question.** The demo was never the instrument.

**Cosmetic, carried forward:** the arrow is OURS — `src/host/main.c:1697` draws the
driver cursor into the frame whenever the hide-count is 0 — and it is hand-drawn, hence
"not quite the right shape". The user has a **16×16 cursor file** to swap in; that is
the fix, and it is presentation only.

**Also explains the manual-run screenshot** (`build/shots/mouse_stock.png`): 640×400,
which is our TEXT render target, not the 640×480 of every mode-12h frame in this sweep.
It is the POST-EXIT state — cyan painted, mode reset to 3 on termination, screen
cleared, and our cursor overlay still drawn because MOUSE.EXE showed the cursor and
never hid it. The window persisted only because a hand-launched run has no `autoexit`
file. Nothing in that picture is a defect.

---

## 8. CAVE — too fast, and it PROVES the speed defect is not mode 12h's fault

**OBSERVED:** renders correctly, "way too fast". `CAVE.BAS` uses `WAIT &H3DA`.

**Why this one is the important speed observation, not just the fourth:** CAVE is
**SCREEN 13**. Mode 13h is linear, never engages the mode-12h interpreter, and runs on
the real CPU exactly as it did before tonight's change — and it is too fast in the same
way for the same reason.

⇒ **Finding #1 is a pre-existing timing defect that this sweep exposed, not a
  side-effect of interpreting the guest.** Anything that polls 0x3DA for pacing is
  affected in every graphics mode. Worth re-checking Skyroads against it: session 11
  recorded its vblank `IN AL,DX`, and its "a little sluggish" calibration was taken
  before anyone knew the retrace bit was untimed.

**Pacing of the remaining three:** GFXCOPY, PALETTE and VS87 all use `SLEEP` and none
uses `WAIT &H3DA` — a third route again (the timer, not the retrace bit), so they test
something the first seven did not.

---

## 9. GFXCOPY — the only READ-BACK test in the sweep, and it passes

**OBSERVED:** works. Speed "hard to tell — it's pretty fast in QBasic and NTVDM too",
so recorded as inconclusive rather than as a defect. That is the right call: the user's
own reference says the program is fast everywhere.

**Why it earns its place:** every other demo only ever WRITES video memory. GFXCOPY
loops `GET (0,0)-(119,159), Buffer` — reading 19,200 bytes of mode-13h video memory back
into a BASIC array — and `PUT`s it into a second panel, under `VIEW` clipping. The two
panels tracking each other is read-back plus clipping confirmed in 13h.

---

## 10. PALETTE — 256-colour DAC fidelity. Passes, and it is STATIC so speed is not a variable.

**OBSERVED:** "accurately displays the organised palette colors."

`PALETTE.BAS` programs DAC entries 16-255 with structured hue ramps (leaving 0-15 at
the defaults), paints a 16×16 grid of 20×10 swatches, then `SLEEP`s for a keypress. No
animation, so this is the one demo in the sweep where the answer is purely about colour
and nothing about timing. Smooth ramps in the right hue families = the 6-bit DAC write
path (`value = 65536*b + 256*g + r`) is correct end to end, with no lost low bits and no
R/B transposition.

---

## 11. VS87 — text mode + CP437 box drawing. Passes.

**OBSERVED:** "works. Static text mode."

An 80×25 mock-up of a period IDE, built entirely from CP437 box-drawing glyphs
(218/191/192/217/196/179/194/193/180/195) with a custom text palette and per-cell
attributes. The only demo that exercises the TEXT path, and the frames rendering
connected — corners and T-joins intact — is direct evidence for the real 8×8/8×14/8×16
ROM font dumps from session 12, in text mode rather than graphics.

---

# RESULT OF THE SWEEP

**Ten demos, watched one at a time on the physical box. All ten render correctly.**
Six SCREEN 12 (the mode that had never rendered before this session), three SCREEN 13,
one text.

**Not one pixel defect was found.** Every defect is timing, and none of it was
introduced by making the interpreter the CPU in mode 12h:

    #1  retrace bit is untimed        BOUNCEBX, MATRIX_2, CAVE   ← fixable in the VDD
    #3  no CPU-speed control          BUBBLES, MATRIX_1          ← needs the feature
        inconclusive / not a defect   GFXCOPY, PALETTE, VS87, MOUSE

**#1 is pre-existing, not ours.** CAVE proves it: SCREEN 13 never touches the
interpreter and is too fast in exactly the same way, for exactly the same reason.

**Two of the five speed-affected demos are unreachable without #3.** A period-correct
retrace bit fixes half of them and cannot, even in principle, fix the other half.

## What the sweep is worth as method

Three of the eleven findings above were *wrong readings corrected by evidence*, and all
three would have cost real work:
  - MOUSE "proves INT 33h is missing" → the oracle ran the same binary on genuine
    MS-DOS 6.22 and it exited in 2.9 s too. INT 33h was already implemented, and a
    purpose-built probe then proved it works end to end.
  - MATRIX_1 "runs at the right speed because the work paces it" → it has a software
    delay loop the pacing grep never looked for.
  - BUBBLES "is also vsync-paced" → it has no pacing of any kind, which is what made
    the CPU-speed request load-bearing rather than cosmetic.

⇒ **Watching a program run finds things no counter reports, and the log's own
  `unimplemented: none` lines were silent for every one of these.** Coverage was
  complete and correct throughout; the defects were all in behaviour over TIME.
