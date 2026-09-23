# Session 60 — CPU speed, honestly -- then the Skyroads wobble, root-caused

> Session 60. Split out of `docs/STATE.md` on 2026-09-23.

> Verbatim from `docs/STATE.md`. Nothing has been edited or corrected,
> including conclusions a later session refuted — same rule as the rest of
> this archive. See [`README.md`](README.md).

## ★★★★ SESSION 60 — **CPU SPEED, HONESTLY. THEN THE SKYROADS WOBBLE, ROOT-CAUSED.**

**Date:** 2026-09-09. **Score: 86.9%, unmoved** — everything this session is a row
already at 1.0 (`host-ui`) or accuracy on a counted row (`settings-live`). The 8
points to 95% live in `guests` (w15 at 63%). **Tree: UNCOMMITTED (13 files).** Rig
clean. Deterministic CPU test 30/30; full build clean.

### ▶ NEXT ACTIONS, IN ORDER

1. **★★★★★ FIX THE SKYROADS WOBBLE — ROOT CAUSE IS PINNED, USER DEFERRED THE FIX TO
      A FRESH HEAD.** The lever: **deliver the timer tick to a SPINNING guest without
      waiting on `g_lock`.** Skyroads spin-waits at `0110:3b40` (IF set) for its 180 Hz
      tick; during a spin it never traps, so only ASYNC delivery works, and async is
      starved (75% bail `not_in_exec`; residual 8–20 ms gaps are `g_lock` contention
      with the audio mixer — which is why timer AND music wobble together). During a
      pure spin the guest holds no lock, so suspend-to-inject is safe WITHOUT taking
      `g_lock`. ⚠⚠ `g_lock` is the load-bearing interlock (never suspend a lock-holder);
      hardest area of the codebase, needs care. Full chain + instruments in
      [[skyroads-playable-input-stack]]. ⚠ **Skyroads wants UNLIMITED, not a throttle**
      — the throttle STARVES its timer (measured: 84/180, 1 s gaps).
2. **THE PORT TRAP IS THE REAL CEILING (2.33 us).** Below every speed label; the only
      lever that also speeds Doom/Skyroads at Unlimited (and would cut the OPL trap
      volume that starves async delivery in #1). Own session.
3. `guests` (w15, 63%) is still the heaviest lever; **CALC and WRITE STILL unjudged**
      on the rig since s59.
4. **THE MOUSE + MENUS NEED YOUR EYES** (popup contents, Edit-greying-in-graphics) —
      `scripts/bm/menushot.bat` / `setshot.bat` put a live host up. ⚠ Do NOT drive the
      desktop while the user is on the box (I did once; use headless `dlgcheck.flag`).

### ★★★ DONE THIS SESSION (all in the uncommitted tree)

- **CPU throttle rewritten as a CLOSED-LOOP INVARIANT + a DETERMINISTIC TEST.** The
     open-loop debt carry (leaked twice) → `hold = max(0, E/duty − T)`, one pure
     function `cpuspeed_step`, driven against a virtual clock in `cpuspeed_test.c`
     (30/30, bit-identical). Rig, host-measured `delivered_bp` (non-circular): whole
     ladder within ~12% of label (100→88, 66→61, 33→30, 16→15, 8→7). Ref reverted
     **5900→3704** (native ALU) — 5900 was a fit to hide the broken mechanism. Two
     real-world fixes the det-test can't see: torn exec-clock read (clamp `dexec` to
     run wall) and per-period catch overhead (1 ms Sleep-able run floor above 32 bp).
     See [[acceptance-test-is-the-calibration]].
- **SETTINGS RESTRUCTURED (user's asks):** "CPU"→"Processor" tab (one honest "Limit
     speed" control + a live CPU-name line; Type/Core/Cycles/FPU/Turbo/slider/affinity
     removed as dead or clutter), Memory split to its own tab, PIT/UI-tick moved to a
     new Advanced tab. Speed ladder trimmed 18→6. `Help▸About`→system ShellAbout. All 8
     pages verified building via `dlgcheck.flag` (headless).
- **MENU BAR** `File Edit View Tools Help` (photographed); **mouse auto-capture** on
     INT 33h use-calls (Doom captures, Skyroads doesn't).
- **SKYROADS ROOT-CAUSED** (see #1) — and the music-overrun hypothesis REFUTED with
     data (big gaps 5 io/gap vs normal 360).

### ★★★ THE ACCEPTANCE TEST WAS THE CALIBRATION WORKLOAD (early-session, superseded above)

`CPUSPEED_REF_MHZ` is calibrated by `cpubench.com`, whose header says outright *"NO
REGISTER IN THE LOOP TOUCHES MEMORY, and that is deliberate"*. `cpuswp.bat` then
re-runs **that same program** at each index and checks the reported MHz tracks the
label. **It passes however wrong the constant is for every other kind of code**,
because the ratio it measures is the ratio it was built from.

`tools/dostest/mixbench.asm` is the instrument that can fail: five shapes of work,
four of them 12 cycles on a 486 *by construction* so they are directly comparable
with no arithmetic in between. On the rig at **index 11, where the menu says 66 MHz**:

```
ALU 68    MEM 209    VID13 92    VID12 23    PORT ~1      (MHz apparent)
```

A **200x spread at one setting**. The ALU figure is right because ALU code is what
the constant was made from.

### ★★ FOUR DEFECTS, ALL OURS

1. **`cpuspd.txt` PARSED A SINGLE DIGIT.** `c[0] - '0'` cannot express an index above
      9, and the ladder went to 17 in s54. So 75, 66, 50, 33, 25, 16, 12 and 8 MHz --
      **every period-hardware setting** -- were unreachable from the file knob, and
      `echo 13` ran as index **1 = 3300 MHz** while reporting that it had. The range
      check *hid* it: `c[0] < '0' + CPUSPEED_COUNT` with COUNT=18 accepts up to `'A'`.
      ⇒ The rig sweep this knob exists to drive **never tested the slow half of the
      ladder even once**, and `cpuswp.bat` only ever swept 0-6.
      ⚠ File knob only. The menu and dialog set the index directly, so this is a
      testability defect and **not** the cause of any speed a user has seen.
2. **THE THROTTLE BILLED THE GUEST FOR OUR OWN OVERHEAD.** `ran_us` was wall clock
      from resume to suspend, and a DOS guest spends much of that window not executing
      but trapped inside us. Every port write is an IOPL-0 #GP costing **2.33 us**
      (iobench case 3: 117,920 accesses / 5 ticks = 429,400/s). At a 1.8% duty the
      guest is held 54x as long as it "ran", so every mis-attributed microsecond costs
      it 54 more -- hardware-touching code penalised in proportion to how slow *we* are.
      Now charged on guest **execution** time (`g_exec_us_acc`, bracketed by
      `g_in_exec`, gated off entirely at Unlimited).
3. **THE EXECUTION BASELINE MOVED ON FAILED RETRIES**, discarding execution that was
      never charged. The throttle reported `delivered_bp=100` -- a 1.00% duty, exactly
      as asked -- while mixbench measured the guest getting **2.6%**. `missed=594` is
      where it went.
4. **`delivered_mhz` REPORTED THE REQUEST, NOT THE DELIVERY.** At index 15 it logged
      16 MHz while the guest measured **34** -- and index 15 was *faster than index 13*,
      so **the ladder is non-monotonic at the slow end** and this field could not say
      so. `owed_ms=1326` of unpayable arrears was sitting three fields away saying the
      opposite. Now `requested_mhz` beside a measured `delivered_bp`, with `wall_us`
      next to `ran_us` so the overhead is visible.

### WHAT IT BOUGHT, MEASURED

| index 11 ("66 MHz") | before | after |
|---|---|---|
| ALU | 68 | 76 |
| MEM | 209 | 76 |
| VID13 | 92 | 54 |
| VID12 (interpreter) | 23 | 23 |
| **PORT** | **89,600 outs/s** | **220,400 outs/s** |

Spread across the real-CPU cases **9.1x -> 3.3x**; port throughput **2.46x**;
Unlimited unchanged (3781 / 3926 / 4012 / 27).

⚠ **`CPUSPEED_REF_MHZ_DEFAULT` IS NOW 5900 AND ITS UNITS CHANGED.** It is MHz of
guest *execution* time, not of wall clock. **`cpubench` still reports the wall-clock
number** and will read ~3700 on this rig for ever, which looks like a disagreement
and is not -- putting 3661 back would silently restore the old behaviour. 5900 is a
**fit across two points** (1.44x at index 11, 1.85x at 13), not a derivation: one
constant cannot satisfy both because the residual is per-workload escape.
⚠ **STILL OPEN:** the throttle is workload-dependent and still not monotonic -- MEM
reads 80 MHz at index 13 against ALU's 26, and higher than its own index-11 figure.

### ★★★ AND NO CALIBRATION CAN FIX THE REAL PROBLEM

A port trap is **6.9 MHz apparent** against a 486's 16-cycle `OUT`. That is **below
every label on the menu**. Doom does ~43,000 port writes a second while drawing;
Skyroads' AdLib helper spends 43 port accesses per OPL register. So the dropdown is
an approximation for **compute**, and `cpuspeed.h` has always said so -- it now says
by how much. Making the trap faster is the only route to an honest 66 MHz, and it is
the one change that would also speed the games up at Unlimited.

### ★★ SKYROADS: THE TIMER IS FLAT. DO NOT RE-INSTRUMENT IT.

User report: "the weird slow down/speed up timing issue". The last recorded run was
`idx=0 duty_bp=10000` -- **unthrottled** -- so the CPU throttle is not involved.
New instrument (`STAGE2: IRQ0TL` / `IRQ0GAP`, both delivery paths) over 45 s:

```
IRQ0TL persec = 183,181,179,180,180,180,180,181,180,...,177,179,183,180,179,181,177
IRQ0GAP ms[<1,1,2,4,8,16,32,64+] = 79,59,416,7375,171,1,0,0  n=8101  max_ms=16
```

**180 ticks/second flat, range 177-183 (+-1.7%).** 91% of gaps in the 4-8 ms bucket
(180 Hz = 5.56 ms), **one** gap over 16 ms, worst case 16 ms. 8101 delivered against
the session-21 baseline of ~4485 -- delivery is better than it has ever been.
⚠ **BUT THIS IS A HEADLESS ATTRACT-MODE RUN**, and the memory warns twice that
Skyroads' attract loop fakes success and that in-game uses a different input path.
⇒ The tick rate is cleared **for this run shape**. Next suspects are frame
presentation and OPL pacing, not the timer.

### ★ MOUSE EXCLUSIVITY IS THE GUEST'S OWN DECLARATION

A program that wants the mouse says so through INT 33h; one that does not never
calls it. Capture now keys on the **use** functions (01/03/05/06/0B) and explicitly
**not** on the detection probes -- validated against real logs before it was written:

- **Doom:** `0000 x1  0015 x1  53c1 x1  0003 x1338  000b x1338` -- probes once, then
     polls every frame. Confirmed live: `autocap_want=1 fired=1 captured=1`.
- **Skyroads:** *no `MOUSEI33` lines at all.* Never calls INT 33h, so its mouse stays
     on the Windows desktop. Exactly the split the user described.

★ **AND IT CLOSED AN OPEN QUESTION IN THE CODE.** The `i33oth=1079` note listed two
hypotheses for high AX values -- unimplemented functions, or a mis-patched `CD 33`.
It is neither: **`AX=53c1` is Logitech CyberMan SWIFT detection**, made once, and
Doom prints `CyberMan: Wrong mouse driver - no SWIFT support (AX=53c1)` in the same
log. The bucket was the defect, not the thing bucketed.

Latched once per program so a polling guest cannot drag the pointer back after
Win+F10; requires foreground so a background VDM cannot steal it. **Show Host Cursor
and Ctrl+F8 are gone** -- pointer visibility is what exclusive mode looks like, not a
knob of its own. `ShowHostCursor` survives, narrowed to "when not captured".

### ⚠ TWO HARNESS TRAPS I WALKED INTO TODAY

- **TWO CONCURRENT `build.sh` RUNS INTO ONE `build/`.** A backgrounded job re-ran the
     build while I ran it in the foreground; the binary came out with *some* edits and
     not others, and I spent a detour concluding a field "was missing".
- **`grep`/`strings` ON THE PE DOES NOT FIND ITS STRING LITERALS.** `site_ovf=` is
     demonstrably printed by the running binary and matches **zero** times in the file.
     Two wrong conclusions came from that before I stopped trusting it. **The only
     trustworthy check that a build contains a change is to RUN IT.**
- Also: a stray background job racing the foreground over `cpuspd.txt` produced an
     interleaved, contaminated sweep -- *stale artefact worse than missing*, again.

---
