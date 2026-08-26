# Session 26 — 2026-08-25

> Archived verbatim from `return-ntvdm.md`, which was the single rolling handoff
> file until 2026-08-26. Split by session so it can be read by topic and date
> rather than by scrolling. **Nothing has been edited** — including conclusions
> that later sessions REFUTED, which are kept on purpose: the refutations are
> some of the most valuable material here.

```
═══════════════════════════════════════════════════════════════════════════════
██ ▶▶▶ SESSION 26 (2026-08-25). FOUR FIXES CONFIRMED.                         ██
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
██ ▶▶▶ DOOM HAS NO MOUSE -- **CLOSED in session 27, see the top of this file** ██
██     Kept because two of its three claims were WRONG and that is instructive ██
═══════════════════════════════════════════════════════════════════════════════
⚠ READ THE TOP OF THE FILE FIRST. The cause was a masked 32-bit EDI on the DPMI 0300
  call structure. Of what follows: the host-side measurement was right, "Doom NEVER
  calls AX=3 or AX=0Bh" was an ARTEFACT OF THE DEFECT (it called them 2915 times in
  45 s), and NEITHER of the two competing explanations was the answer.

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
```
