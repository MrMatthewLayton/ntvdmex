# Session 59 — ZAR renders: its attract demo is on screen, in colour

> Session 59. Split out of `docs/STATE.md` on 2026-09-23.

> Verbatim from `docs/STATE.md`. Nothing has been edited or corrected,
> including conclusions a later session refuted — same rule as the rest of
> this archive. See [`README.md`](README.md).

## ★★★★★ SESSION 59 — **ZAR RENDERS.** ITS ATTRACT DEMO IS ON SCREEN, IN COLOUR.

> **▶ NEXT SESSION, IN ORDER. (15 commits, `10a62cf`..`543858c`, NOT PUSHED.**
> **Rig clean: no knobs set, watcher alive, staged binary = the build.)**
>
> 1. **IS ZAR PLAYABLE?** The user saw it render ("I saw it run. Good progress")
>    but has not yet driven it. **`scripts/bm/zarplay.bat` is staged and ready** —
>    it asserts the IFEO key, clears the GH #132 counter, silences the trace for
>    speed and leaves the game up. Questions: does the demo run smoothly; does
>    ESC/SPACE/ENTER reach a MENU (`MAIN MENU`/`SELECT PLAYER` exist in the
>    binary); do arrows/mouse respond and how does it FEEL. Expect NO sound.
>    ⚠⚠ **DELETE `wowquiet.txt` AFTERWARDS** — see hazard (5).
> 2. **ZAR'S AUDIO** — one named gap, and it needs its own session because the
>    right answer touches an explicit "never do this". See the block below and
>    [[zar-dos16m-frontier]]. Do NOT start it at the end of a long session.
> 3. **REMOVE THE `host_irq_sink` PM THROTTLE.** It defends against a phantom
>    16 kHz burst that the 8254 fix proved never existed. Separate, measured.
> 4. **CALC and WRITE** are still unjudged on the rig (carried from s58);
>    `guests` remains the heaviest score lever at +0.26 each.
> 5. ⚠⚠⚠ **TWO RIG HAZARDS BIT THIS SESSION — READ BEFORE TRUSTING A RESULT:**
>    `zarplay.bat` leaves `wowquiet.txt` behind (a TIMING change for every later
>    run; a 6 KB `result_*.log` is the tell), and `doomrun.bat` exists TWICE with
>    `rt.bat` calling the ROOT copy by absolute path. Also: the watcher dies with
>    a host teardown and needed restarting three times.

### ▶ THE ROOT CAUSE OF #23, AND IT WAS OURS

**DPMI `0300` (simulate real-mode interrupt) implemented ONLY `INT 21h` and `INT 33h`
— and then cleared CF unconditionally, so every other vector was told "done".**

A Watcom/DOS4GW program does not write `int 10h`. It calls `int86()`, and `int86`
under an extender is `INT 31h AX=0300, BL=10h`. So ZAR's `SetMode(0x13)` returned
SUCCESS having done nothing, and the game ran its demo — frames advancing, data
streaming — into a screen it had never been allowed to open. Same 45 s run:

| | before | after |
|---|---|---|
| `STAGE2: mode sets` | `none` | **`mode=0x13/kind=02/320x200`** |
| `STAGE2: video now` | `mkind=00` (text) | **`mkind=02 gw=0x140 gh=0xc8`** |
| `linear_bar_nonzero` | `0/0x2800` | **`0x2800/0x2800`** (framebuffer full) |
| unhandled `simInt` | *never printed* | `int2fh x3 int66h x3` (the game's own probes) |

✅ **SCREENSHOT: 3D terrain, the `Z.A.R. - DEMO` banner, a vehicle — the attract demo
playing in our VDM window.** ⚠ That is RENDERING, not "playable": input, sound and a
real game session are unproven, and `done` means USER-CONFIRMED.

**RE-GATED** (the video path is shared; same day, same box, 45 s):

| | Doom | Skyroads |
|---|---|---|
| `simInt UNHANDLED` | **0** (all serviced) | **0** (all serviced) |
| `mode sets` | `0x03` **and** `0x13` | `0x13` |
| delivery | raises 6196, TOTAL 6197 (**100%**), owed_max 31 | `pit_reload=0x19e4` (180 Hz) |
| `irq0_inj` | — | **5412** vs 5415/5413 baseline |

⇒ Neither guest affected; ZAR gains a picture.

★★★ **THIS IS THE SIXTH INSTANCE OF THE PROJECT'S MOST EXPENSIVE BUG SHAPE** — a
service that does nothing and reports success (see [[stepped-over-call-answers-at-random]]).
The `0300` arm's own comment says *"widen this when the evidence names an
interrupt"*, which is right — but it still cleared CF for vectors it did not
service, so nothing ever failed loudly.
⚠⚠ **AND THE HOST HAD BEEN COUNTING THEM FOR FOUR SESSIONS.** `g_simint_unhandled`
and `g_simint_vec[]` recorded ZAR's seven unhandled `INT 10h` calls every run — into
the periodic KEYLOG block, **which a headless run never reaches**. Every "ZAR never
calls INT 10h" note in sessions 58 and 59 (including the ones above, written before
this was found) rested on a grep that could not see them. The counters are in STAGE2
now. ▶ **If a service dispatches on a sub-function, its unimplemented arms must be
reported where a HEADLESS run prints.**

### ▶ ★★★★ AND SOUND IS THE SAME BUG, ONE GAP FURTHER DOWN

**DPMI `0300` is specified to invoke the GUEST'S OWN real-mode handler from the IVT.**
Ours serviced three vectors host-side (21h, 33h, and now 10h) and silently did nothing
for the rest. ZAR installs its own real-mode INT 66h handler (`0201 setRMvec int 0x66
= 0x34d3:0x01d1`, an AIL-style private API), calls it three times with AX=0300/0301/
0304, and restores the vector. **We dropped all three.** Consequences, measured:
the Miles driver it loads — `SOUND\SBLASTER.DIG`, opened, seeked, read — is **never
once called**, all **2,386** real-mode calls in a run go to our own DOS handler at
`0050:0000`, and no SB port is ever touched (`sb_dspwr=0`, `opl writes=0`).

✅ **WITH REFLECTION ON, ZAR PROGRAMS THE SOUND BLASTER FOR THE FIRST TIME:**
`SNDIO out 0x22c`, `sb{blocks=1 left=0x10 len=0x10 rate=0x56ce}` — 0x56ce = 22222 Hz,
exactly the `sound SamplingRate 22222` in `USER1.CFG`.
⚠ **AND THEN IT WEDGES.** The guest spins in REAL MODE at `0x34d3:0x06b1` with that
DMA block queued and never draining (`irq0=1 intpend=1`) until the watchdog kills it.
The driver is polling for a completion that never arrives: **the nested V86 loop that
`0301/0302` runs a real-mode procedure in does not appear to deliver the SB's IRQ**,
so `v86_run` never returns and the poll never ends. ▶ THAT is the next gap.

⇒ Committed behind **`simintrefl.flag`, OFF by default** — a wedge is strictly worse
than "renders, silent", and this is the call `wowquiet.txt` / `pitinj.txt` already
make. Turn it on to work the audio thread; leave it off to play.

### ▶ ★★★ AND ONE MORE STEP DOWN THE SOUND PATH (always on, Doom re-gated)

**The SB mixer answered "no IRQ, no DMA".** Registers `0x80` (IRQ select) and `0x81`
(DMA select) fell through to the plain mixer RAM, which is zero — and on real
hardware zero there does not mean "default", it means **NO IRQ SELECTED / NO DMA
CHANNEL SELECTED**. A driver that autodetects the card instead of trusting `BLASTER`
learns it is unconfigured. ZAR's Miles driver does exactly that:
`out 224<-80 / in 225->00`, `out 224<-81 / in 225->00`, then `40 D3` (22222 Hz) and
`14 0F 00` — an 8-bit **single-cycle 16-byte** transfer, the classic init-time
DMA/IRQ **self-test** — and waits for a completion interrupt it cannot receive.
► Now answered from `st->irq`/`st->dma8`/`st->dma16`, **derived not stored**, so it
cannot drift from the numbers that go into `BLASTER`.

✅ It moves the fault exactly one step, and names the next gap precisely:
`in 225 -> 0x02` (IRQ 5) and `0x22` (DMA1|DMA5); **`sb{left}` 0x10 → 0x00 — the block
now DRAINS**; and **`ASYNC-EARLY bail irq=05 why=0x14`** — **IRQ 5 IS RAISED** and
refused, `why=0x14` being *"the CPU thread was in HOST code"*.
⇒ **An IRQ raised while the guest is inside a nested real-mode call cannot be
delivered**: the async injector only places one when it finds the thread executing
GUEST code, and single-cycle means one IRQ and no second chance.
### ▶ ...AND THAT COOPERATIVE FIX WAS TRIED. IT IS RIGHT, AND IT IS NOT ENOUGH.

The gate was inline in the main exec loop, so a guest inside a nested real-mode call
never reached it. Factored into `v86_deliver_dev_irq()` (extracted verbatim, not
copied — two copies of an interrupt-delivery gate is how they drift) and called from
the nested `0301/0302` loop too. **A real gap, closed.**
⚠ **But it does NOT fix ZAR's audio, and the measurement says why.** Its Miles driver
waits on a **memory flag its ISR will set, not on a port** — the last `SNDIO` in the
run is the `14 0F 00` command itself, with no polling after. A guest that spins
without trapping never returns from `v86_run`, so the nested loop gets no further
turn: **zero `IRQN-REFUSE` lines**, i.e. the gate never once saw a pending device IRQ
while the driver waited. A cooperative fix structurally cannot reach this.

### ▶ THE ASYNC RETRY WAS ALSO TRIED. IT HELPS DOOM AND CANNOT HELP ZAR.

A device IRQ got exactly ONE async attempt, at the raise instant; if the CPU thread
was in host code that microsecond it was lost for ever. Now retried **once per PIT
sync**, first pending hooked line only, nothing at all when none is outstanding
(session 22's disaster was ~800 unbounded `SuspendThread` round trips *per sync*;
this is ~2 **a second**). Done in `host_pit_sync`, which already holds `g_lock` —
and holding it is what guarantees the thread we suspend is not holding it.
✅ **Doom: `try=0x5f ok=0x10` and `try=0x6f ok=0x10` — 16 SB block-completion
interrupts a run that were previously LOST are now delivered**, reproducibly.
⚠ `REPLAYED_LOUD` deserved care (a late IRQ is a plausible late-refill mechanism).
Six runs settle it as variance: **without** 112/120/152, **with** 157/79 — the retry
runs bracket the others on both sides. `idle` at its lowest, everything else flat.

### ▶ ★★★★★ AND NOW THE ZAR AUDIO BLOCKER IS FULLY EXPLAINED

With `simintrefl.flag` on, the **heartbeat** (the only instrument that survives a
wedge — STAGE2 never prints when the watchdog kills the run, so the counters were
added there) reads:

```
r5=0x1   rtry=0x65f/0x0   why=0x14
```

IRQ 5 raised **exactly once**, the retry offers it **1631 times**, **every** offer
refused with `why=0x14` = *"the CPU thread was in HOST code"*. And the log's last
line is the **third** reflected INT 66h call (`AX=0x0304`) with **no matching
`0301 -> RM proc returned`** — the previous two both returned.

⇒ **That call entered the guest's real-mode handler, programmed the SB, and never
came back. The host thread is blocked inside `v86_run`**, so `GetThreadContext`
reports the syscall frame rather than the V86 guest: the injector can neither SEE nor
REACH it. Cooperative delivery cannot help either — the loop gets no further turn
(zero `IRQN-REFUSE` lines). **Neither delivery path can reach a real-mode procedure
that spins without trapping.**

▶ The architecturally right answer is the kernel's own interrupt assist via the
`FIXED_NTVDMSTATE` pending bits. ⚠⚠ But **"Never poke `[0x714]|=1`"** is an explicit
prior finding (VME/VIF gating) — so that wants its own session, deliberately, not the
tail of this one. ZAR renders and is silent; that is a good place to stand.

⚠ Doom re-gated and the change is **provably inert** for it: Doom only ever selects
mixer index `0x82` (10 times a run), never `0x80`/`0x81`, so no path reaches it.
`sb_blocks 0xec5→0xecc`, `midi_msgs 0x309→0x30b`, `idle 0x6202→0x5a02` (less inserted
silence), `mix82 ANSWERED_NO 3→3`. `REPLAYED_LOUD 0x78→0x98` is run-to-run variance
on a metric with no route to this code — not a regression.
⚠ Only reflects when the GUEST owns the vector (IVT segment != `DOS_HDLR_SEG`).
Vectors still pointing at our own stubs keep today's behaviour and stay visible in
`STAGE2: simInt (DPMI 0300) UNHANDLED`, which is where the next one will be found.

### ▶ ⚠⚠⚠ A REPORTED DOOM REGRESSION, AND THE TWO RIG HAZARDS BEHIND IT

The user reported Doom's status bar pixelated and messages leaving pixels behind —
the symptoms `8648f41` fixed in session 25. **It did not reproduce, and the video
path measures perfect:**

- **`TITLEPIC vs shot01/shot02 : 0 of 64000 compared pixels differ (0.000%)`** —
     planes → compose → present → capture, bit-perfect over a full screen, judged
     against the IWAD.
- `planejudge`: planes **71.2/70.8/72.4/69.5%** vs STBAR, against the **70/69/71/68%**
     recorded when it was fixed (**34/71/30/28%** when broken). Plane-to-plane identity
     18–30%; the broken state was 66.8% (one plane smeared over the rest).
- Nothing this session touches Doom's video: **zero `simInt 0x10` calls**,
     byte-identical video counters, mixer `0x80/0x81` never selected by Doom.

⚠⚠⚠ **BUT TWO RIG HAZARDS MADE THE EVIDENCE UNTRUSTWORTHY, AND ONE WAS MINE.**
1. **`zarplay.bat` leaves `wowquiet.txt` behind** and cannot clean it up (it exits
      while the game runs). That silences `log_append` for every LATER run — **which is
      a TIMING change, not just a quiet log**: per-event logging under the device lock
      is what cost Skyroads 24% of its ticks. Measured: created 22:43, and a Doom run at
      23:19 came back **6,702 bytes** with the `WOWQUIET` banner — no STAGE2, no
      counters, and different timing from the shipping configuration. A "bad, then fine"
      report straddling that moment is explained with no code change at all — and it
      cuts both ways, since "fine" under a silenced trace is not "fine" as shipped.
      ▶ **`ls` the share for `wowquiet.txt` before believing any measurement or report.**
      A 6 KB `result_*.log` where megabytes are expected is the tell.
2. **`doomrun.bat` exists TWICE on the share** and `rt.bat`'s `:doomrun` calls the
      **ROOT** copy by absolute path. I staged a fix to `bm\` only; it did nothing and
      read as "the fix does not work". Same trap as session 58's root-`.bat` breakage.

✅ Re-baselined with the trace RESTORED (6.3 MB log): identical video counters.
▶ **And it is now catchable**: `capture.flag` → `doomrun` collects `shot*.bmp` (it
never did — the `doom` arm skips `:collect`) → `doomref.py cmp TITLEPIC` is a
0/64000 pass-fail on the whole video path.

## ★ SESSION 59 (earlier) — HOW THE PROBLEM WAS NARROWED

### ▶ ★★★★★ THE ONE FACT THAT CHANGES THE PROBLEM

Session 58 handed over *"loads ~4.8 MB of a data file and stops progressing"* and
set the next step as *"instrument the TRANSITION where the reads stop"*. **There is
no transition, because the reads never stop.** Given seven minutes instead of
forty-five seconds, ZAR's file-read sequence is **PERIODIC**: period **1073 reads**,
6.2% mismatch, ~175 s per cycle, measured by autocorrelating the `INT21 AH=3F`
trace of a 420 s run. 2,553 reads, 1,148 of them distinct, the tail set hit 10-11
times each. The "stall at `pos=0x49d454`" was a 45-second window onto a cycle.

★ **AND THE BINARY NAMES THE CYCLE.** ZAR's state machine switches on a dword at
obj1 link `0x4c878`, and each state loads a caption from the obj3 string pool:

| state | caption | | state | caption |
|---|---|---|---|---|
| 1 | `WAIT...` | | 6 | **`LOADING DEMO...`** |
| 2 | `LOADING...` | | 7 | `LOADING INTRO...` |
| 5 | `LOADING BATTLE...` | | 9 | `QUITING...` |

A load cycle that repeats every ~175 s **is the attract/demo loop**. The outer loop
at obj1+0x1289 is a plain `wait for the tick counter to advance, then run that many
frames` — the thing session 58 filed as "its idle is a wait-for-next-tick loop" is
ZAR'S MAIN LOOP, and it is turning. Nothing fails: the binary carries
`Can't load game data`, `Library reading error`, `** NOT ENOUGH MEMORY: **` and
`Error loading sound effects.`, and **not one of them is ever printed**.

⇒ **The blocker is not loading and never was. ZAR runs its game logic and never
initialises video** — `INT 10h` is called ZERO times in a seven-minute run.

### ▶ HOW TO READ THE GUEST'S ADDRESSES (this cost an hour; it need not again)

- `ZAR.EXE` is MZ + **LE at 0x2a50**, three objects; the object page map is LINEAR,
     so `obj1 + off` is at file `datapages + off` (`datapages` = LE header `+0x80`).
- The load base is IN THE LOG: `INT31h AX=0501 BX=0x0b CX=0x7000 [LE CODE OBJECT]
     -> mem 0x03f70000` is obj1 (0xb7000 = its 183 pages). obj3 is the next 0x621000.
     ⚠ **It moves run to run** — 0x03f70000 one run, 0x03b70000 the next. Take it from
     the log, never from a previous session's note.
- The file image is already relocated against the LINK bases (**obj1 @ 0x10000,
     obj3 @ 0xe0000**), so an absolute operand in a disassembly is a link address and
     `guest = link - link_base + load_base`. Confirmed: obj1+0xae71c (the LE entry
     point) is the `WATCOM C/C++32 Run-Time` banner.
- The string pool at obj3+0x100.. is the game's whole vocabulary — states, menus,
     every error message. Dump it FIRST on any new guest; it is an hour of call-graph
     work for free.

### ▶ AND SWITCHES NOTHING ON THE RIG COULD REACH

`ZAR.EXE` obj3+0x1b5 is its own `-Help` text:
`-NoSound` (disables the sound system), **`-NoVESA2` (disables VESA 2.0 linear
frame buffer modes)**, `-Join <address> <port#>`, `-Psw <password>`. Every runner
here hard-coded a bare `ZAR.EXE`. `scripts/bm/zarargs.bat` passes a guest command
line (on the REAL command line — when CSRSS names the program the host does not
consult `target.txt`); `-Help` is the smoke test that arguments reach the guest.
⚠ Note this against session 58's *"do not plan the VESA work around ZAR"*: that
conclusion still holds for the CONFIGURED mode (`USER1.CFG` ships `VGA_320x200`),
but the binary plainly has a VESA 2.0 path, a `VIDEO MODES` menu, and VBE `4F06`/
`4F07`/PM-interface call sites at obj1+0x97dca..0x980c9.

### ▶ TWO DEFECTS FIXED, BOTH FOUND BY LOOKING AT WHAT THE HOST WAS DOING

**1. ★★★★ THE REFLECTED-INTERRUPT TRACE WAS A FIREHOSE A GUEST COULD DRIVE.**
`dpmi_dispatch_to_pm_handler` wrote two `log_append` lines (~350 bytes, plus two
`host_readable` probes) on EVERY reflected INT, unconditionally. ZAR polls its own
`INT 21h` hook for `AH=2Ch` at ~3,800/s, so a 45 s run was ~170,000 dispatches,
~340,000 `WriteFile` calls and **53 MB of log** — a histogram of which is 100% one
line, one vector, one AX value. The 501 file reads that mattered were 0.1% of it.
► Now bounded **per (vector, AH)**: the first 24 of a pair always print, then at
most one per 100 ms, with the total for every pair reported at STAGE2 and a line
saying when a pair crossed into the limited regime. **A seven-minute run is 9 MB
instead of ~500 MB** — which is the only reason the periodicity above was visible.
⚠ Per-pair and RATE-based, both deliberately: a global count cap silences a rare
vector because a common one spent the budget, and a pure count cap goes dark exactly
where a long run's evidence is. `AH=3Fh` at 6/s stays fully traced.
★ This is the THIRD time this project has paid for a per-event log in a hot path
(Skyroads lost 24% of its ticks to one; `wowquiet.txt` argues it again).

**2. ★★★★ THE 8254 APPLIED HALF A COUNT.** `pit_out` read-modify-wrote `reload` on
every byte, so between a guest's two `out 40h` instructions the divisor was
(old MSB | new LSB). ZAR programs **0x8002** (36.41 Hz, ~2x the BIOS rate) as
lo=0x02 then hi=0x80, from a standing 0 (65536) — so the LSB write alone left
**`reload = 2` = 596,591 Hz** for the whole gap between the two writes, and that gap
is not microseconds for us, it is two traps out of protected mode and back.
The run's own counters had said so all along and nobody had read them:
`raises=29657` in 45 s against a programmed 36.4 Hz, `owed_max=64` (saturated),
`PIT-RELOAD 0x2 (hz=0x91a6f)` sitting in the log with nothing else out of range.
~29,000 interrupts the 8254 never generated, each a `SuspendThread` round trip under
the device lock. ► The real 8254 buffers the LSB and loads the count register on the
MSB write. LSB-only / MSB-only zeroing the other half is the same mistake in a second
dress and is fixed too, but **on datasheet grounds, not on ZAR's evidence** — ZAR
uses lo/hi and never takes that path. Three new checks in
`tools/dostest/pit_test.c`, **verified failing on the old code**, 26/26 passing now.
⚠⚠ The PIT is the most shared path here — so BOTH were re-gated, same day, same box,
`bmqueue.sh` at the standard 45 s cap, the "before" run using the session-58 binary:

| | Doom before | Doom after | Skyroads before | Skyroads after |
|---|---|---|---|---|
| `pit_reload` | 0x214a (140 Hz) | 0x214a | 0x19e4 (180 Hz) | 0x19e4 |
| `raises` | 6624 | **6189** | 8242 | **8109** (=180.0 Hz exactly) |
| delivered / raises | 93% | **100%** | — | — |
| `owed_max` | 64 (**saturated**) | **27** | — | — |
| owed buckets 32-63 / 64 | 28 / 26 | **0 / 0** | — | — |
| `irq0_inj` (V86) | — | — | 5415 | **5413** (0.04%) |

⇒ **Doom strictly better, Skyroads unaffected**, and in both the drop in `raises` is
exactly the phantom burst disappearing. That is the shape a correctness fix should
have: the guest's programmed rate is untouched, only the invented interrupts go.

★★★ **AND IT REFUTES A BELIEF THIS HOST IS BUILT ON.** `host_irq_sink`'s throttle is
justified in a long comment by *"Doom's music driver programs the 8254 at 16 kHz
(reload 0x4a = 16 kHz, measured)"*, which makes a 50 ms catch-up gap EIGHT HUNDRED
raises. **There is no 16 kHz timer.** `0x4a` is the LOW BYTE of `0x214a`, and the
"measurement" was this bug's transient being read as the guest's intent. Doom
programs 140 Hz, once. ▶ So the whole "one attempt per sync" PM throttle was built to
defend against a burst WE CREATED — and it is the thing that later cost Skyroads a
fifth of its clock until it was scoped to PM clients. It is now defending against
nothing. **Removing it is a real lever and a separate, measured change** — do not
slip it in with this one, but it is the first thing to try for any PM guest that
looks tick-starved.

### ▶ ★★★★★ AND THE BIG ONE, FOUND BY TRYING TO PASS `-Help`:
### **NO DOS PROGRAM COULD BE GIVEN A COMMAND-LINE ARGUMENT. AT ALL.**

`target.txt` has split `path [args]` since M2.5. **The CSRSS path never did** — and
that is EVERY REAL LAUNCH, because the IFEO hook is how a program reaches us on the
user's machine. So `ZAR.EXE -Help` opened a file literally called
`C:\game\ZAR.EXE -Help`:

```
STAGE2: loaded 0x00000000 from C:\game\ZAR.EXE -Help
STAGE2: loaded 0x00000000 from C:\game\C:\game\ZAR.EXE -Help
STAGE2: cmdtail len=0x02 [20 5c 0d ...]          <- " \" -- junk from CSRSS's cmd= field
==> DOS terminate (AH=4Ch), exit code AL=0x00000000     (78 ms)
```

Zero bytes read, fall through to the four-byte `mov ah,4Ch / int 21h` embedded stub,
clean exit having done nothing. **`EDIT FOO.TXT`, `DOOM -warp 1 1`, `PKUNZIP x.zip`
— none of them could ever have worked**, and each would have looked exactly like
"the program runs and does nothing". Same symptom GH #131 chased for a session.
► Fixed by `csrss_open_split()`: try the whole string first (so a real path
CONTAINING a space still works), then split left to right and take **the first split
that names a file which actually exists** — the file system arbitrates instead of a
guess. Applied to both the absolute-title and the joined-relative arms.
✅ **MEASURED:** `zarargs.bat 30 -Help` now prints ZAR's own options block through
our host, `cmdtail len=0x06 [20 2d 48 65 6c 70 0d]` = `" -Help\r"`.

⚠⚠ **AND IT WAS NEVER "NO ARGUMENTS" — IT WAS ALWAYS A WRONG ONE.** With the title's
args unparsed the code fell back to CSRSS's `CmdLine`, which arrives as junk on this
path (`cmd=[\]`), so **every ZAR run this project has ever done handed the guest a
command tail of `" \"`** — a spurious argument, on a program whose argument parser
selects network and video behaviour. That is a live suspect for the whole #23
investigation and it has been under every measurement since session 55.

### ▶ ⚠⚠ AND ONE SELF-INFLICTED WOUND WORTH REMEMBERING

⚠⚠ **AND THE HOST UNINSTALLS ITSELF WHEN OUR OWN METHOD LOOKS LIKE CRASHES.**
GH #132 counts consecutive failed starts in `C:\ntvdmex\startfail.txt`, incremented
at every start and cleared only by a CLEAN GUEST EXIT — but `zarlong.bat` and
friends deliberately LEAVE THE HOST RUNNING for a human to look at, and the next run
`taskkill`s it. That is a "failed start" every time. **Three in a row and the host
removes its own IFEO Debugger key**, after which every later run silently measures
STOCK ntvdm. Measured this session: a run came back `start mode was UNINSTALL`, the
guest died in 31 ms, and the evidence read as *"-Help makes ZAR exit instantly"*.
The mechanism is correct and stays; the runners now `del C:\ntvdmex\startfail.txt`,
because a runner that kills the host ON PURPOSE has no business feeding that counter.
▶ This is [[stock-ntvdm-doom-oracle]]'s hazard from a NEW direction: not "somebody
forgot the key" but "we took it out ourselves". `bm\ifeochk.bat` answers it in
seconds — run it whenever a result surprises you.

Adding that `from CS:EIP` field **killed the host outright** on the next run:
`DPMI FATAL: exception code=0xc0000005 ... bytes@fault: 0f b6 01 c0 e8 04`. The
entry line is built in ONE pass into a `char lb[256]` with nothing counting
characters, and it was already **247 characters** long — vector, handler sel:off,
AX, DS:EDX, a linear address, a 16-byte `zdump` (48 chars by itself), SS/ESP/CS with
D/B annotations, h32. Nine bytes of headroom. The new field is 42, so it overflowed
the frame by 33 and the host died several calls later inside `zdump`'s own
nibble-to-hex lookup, running on a pointer the overflow had wrecked.
► **A fixed log buffer in this file is a silent budget nobody is tracking.** The
fault report was excellent and named the formatter, but the formatter was the
VICTIM. Before adding a field to any of these lines, add up the one that is already
there. `lb` is 512 now, with the arithmetic written down beside it.

### ▶ REFUTED THIS SESSION — DO NOT RE-TRY

- **"It is just slow / it is the log."** Ten minutes with `wowquiet.txt` on (trace
     silenced, 8.6 KB of log instead of 500 MB): still `Game loading...`. The trace
     was a real defect and worth fixing on its own merits; it is not the blocker.
- **"It stalls at 87% of ZARN0.SFS."** It reaches the same byte at 45 s and at 7
     minutes because that byte is the END OF A CYCLE. See above.
- **The timer is being starved.** It is not: `delivered ~36/s` against a programmed
     36.41 Hz. The 29,657 `raises` were the PIT bug's phantoms, not real demand.

### ▶ ★★★★★ AND THEN THE STATE VARIABLE ITSELF — IT IS **PINNED AT 4**

`pmwatch.txt` now takes `+<hex>` = **an offset from the guest's LE code-object load
base**, resolved lazily (the base is not known when the file is parsed). That matters
because an extended guest IS NOT LOADED AT A FIXED ADDRESS — ZAR came up at
`0x03f70000` one run and `0x03b70000` the next, so an absolute address copied out of
one log silently watches a neighbouring allocation on the next run. `+12c878` is
copy-pasteable from a disassembly and stays right.

With `pmwatch.txt = +4c878 +12c878 +12c8d0 +141310`, the state dword goes
**0 → 1 → … → 4 and then never changes again** for the rest of the run. It is NOT
cycling an attract loop. It is stuck in state 4 — and **4 is the one value the
caption switch at obj1+0x11f9 has no name for** (it handles 1, 2, 3, 5, 6, 7, 9).
The main loop then does exactly what the disassembly says it will: state != 0 and
!= 5, so fall into the tick-wait at obj1+0x1317, for ever.
⚠ ALSO SETTLED BY THE SAME RUN: the file image's absolute operands are **object
offsets, not link addresses** — `[0x4c878]` is `obj3 + 0x4c878`, i.e.
`codebase + 0x12c878`. The obj1-relative reading (`+4c878`) reads a constant
`0x9504`, which is code. Two candidate interpretations, one run, no guessing.
▶ **NEXT SESSION STARTS HERE.** Every write of a constant to the state lives in one
jump table at obj1+0x1824 (`jmp [eax*4+0x1628]`), plus obj1+0x1191 (→7),
obj1+0x170b (→1), obj1+0x8b15 (→0) and obj1+0x13b77 (→4). **Find who selects arm 4
and what state 4 is waiting for.** The table index comes from `eax`; a `pmbp.txt`
breakpoint at obj1+0x1824 gives it directly.

### ▶ AND THE PIT FIX TRANSFORMED ZAR'S OWN TIMER

Same 45 s run, before → after: `raises` **29,657 → 1,604** (= 35.6 Hz against the
36.41 Hz it programs), `delivered/raises` **5% → 100%**, `owed_max` 64 (saturated)
→ 44. The "timer starvation" visible in every previous ZAR log was our phantom
interrupts, not real demand. **It still loops**, so this was not the blocker either
— but every future ZAR measurement is now taken on a guest whose clock is right.

### ▶ ★★★★★ STATE 4 IS **THE DEMO**, AND IT IS RUNNING. THE PICTURE NEVER LEAVES.

Following the chain out of the state machine, all measured or read off the image:

- The caption switch keys on the **request code**, not the state: request 6 →
     `LOADING DEMO...` → jump-table index 5 (`lea eax,[ebx-1]`, table at obj1+0x1628) →
     **state 4**. `ebx` is the return of the scene run loop **obj1+0x111a**, the function
     that contains the tick-wait at obj1+0x1317. So ZAR asked for the demo, entered the
     demo scene, and never leaves it.
- **A VIDEO DRIVER IS INSTALLED AND IT IS THE RIGHT ONE.** `[0x5ea320]` (the driver
     pointer) = `0x046377e8` from the first tick, and the object there begins
     `0x5f414756` = ASCII **`"VGA_"`** — it is the `VGA_320x200` descriptor from
     `USER1.CFG`. Its put slot `[obj+0x80]` = `0x03ffc024` = **obj1+0x8c024**, the real
     mode-13h blitter (`mov ecx,0xfa00 / mov edi,0xa0000 / rep movsd`). Nothing here is
     a stub: mode SELECTION ran and chose correctly. (The descriptor lives at
     obj3+0x5e77e8, past the initialised region, so it was BUILT at runtime.)
- **THE FRAME LOOP TURNS.** The per-tick body at obj1+0x1343 increments a frame
     counter `[0x4c870]`, and it climbs **monotonically and never resets**.
     ⚠ Its RATE varies wildly run to run — 0x34d (845) in one 45 s run against 0x3a (58)
     in another at a similar tick count — so do NOT quote a frame rate from it; the
     claim it supports is only "the loop is turning".
- ⇒ The periodic 1073-read cycle is the demo **streaming**, not restarting.
- **THE MODE IS NEVER SET.** The mode-set is obj1+0x98b0a (`eax=0x14`, then `0x13`),
     called from obj1+0x8cdf2. It forks on `[0x4b82c]`: non-zero → `call ptr [0x4b714]`
     (an alternate/external video dispatch), zero → the built-in path that issues
     `INT 10h`. **Measured at runtime: `[0x4b82c]=0` and `[0x4b714]=0`** — the fork is
     NOT taken, so the built-in path would have issued `INT 10h`. It never does.
     ⇒ **obj1+0x98b0a is never CALLED.** Nothing is ever written to 0xA0000 either.

▶ **NEXT SESSION STARTS HERE, and it is now a narrow question:** is obj1+0x8cdf2
(the function containing the mode-13h set) ever entered, and if so what does its gate
return? The gate is `mov eax,0x5e7724 / call 0x98de7 / cmp eax,1 / jne -> return 0`,
and with `[0x4b82c]=0` that resolves to **obj1+0x9cf2b**. A breakpoint at obj1+0x8cdf2
and obj1+0x9cf2b answers it in one run.
⚠ **`pmbp.txt` still takes ABSOLUTE addresses and the load base moves every run** —
it needs the same `+<hex>` treatment `pmwatch.txt` just got, but it ARMS breakpoints
(patches the guest) rather than reading passively, and arming happens at several
points, so it is real work rather than a two-line change. Do that first.

### ▶ NEXT

▶ **Find what state 4 is waiting for** (above). `INT 10h` is still called ZERO times
and `mode sets: none`, so video init is not merely failing — it is never entered.
⚠ **A LEAD WORTH ONE MEASUREMENT FIRST:** `p3da_reads=481,618` against
`vbl_edges=94` in 45 s. The guest hammers the VGA status register ~10,700 times a
second and completes a retrace wait only **twice** a second, where real hardware
gives 70. Either an `in al,0x3DA` costs us ~90 µs, or the polls arrive in bursts and
the edge counter is fine. **Measure which before acting** — a counter's layout is a
claim, and this one has two very different readings.
⚠ Cheap and now possible: `zarargs.bat 180 -NoVESA2` / `-NoSound`. Neither has been
run — the first attempt failed because `bmqueue.sh skyroads` had `rmdir`'d `C:\game`
out from under it (rt.bat's `:game` arm wipes the directory; re-run `bmqueue.sh zar`
to restore it).
⚠ The reflected-dispatch line now carries `from <CS>:<EIP> lin=<linear>` — session
58's open question. For ZAR it names the Watcom CRT (`obj1+0xAEC87` read,
`obj1+0xB5C2C` clock), not game code, so the game's own frame needs a stack walk.
▶ And the new STAGE2 line answers "what is this guest DOING?" in one place:
`21/2c=533,808` — ZAR asks its own clock hook **11,862 times a second**.

---
