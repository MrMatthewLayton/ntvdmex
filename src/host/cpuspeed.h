/* cpuspeed.h -- approximate CPU speed for a host that runs on the real CPU. (GH #56)
 *
 * ── WHY A THROTTLE IS THE ONLY LEVER ────────────────────────────────────────────
 * DOS programs pace themselves in one of three ways, and the mode-12h demo sweep
 * (docs/research/demo-sweep-findings.md) sorted every demo into one of them:
 *
 *   vsync-paced (`WAIT &H3DA,8`)   -- fixed by the retrace work (#55)
 *   busy-wait-paced (`FOR i = 0 TO delay`)  -- reachable ONLY from here
 *   not paced at all                        -- reachable ONLY from here
 *
 * ★ And the retrace fix, which is measurably correct, FIXED NOBODY'S SPEED. Correct
 *   pacing caps a demo at 60 fps and a 386 never redrew an 80x30 grid at 60 fps: on
 *   period hardware the binding constraint was always CPU SPEED, and `WAIT` was only
 *   tear avoidance. So this is load-bearing, not a nicety, and it is squarely
 *   superset territory -- XP's own ntvdm offers nothing like it.
 *
 * ── WHAT "33 MHz" CAN HONESTLY MEAN HERE ────────────────────────────────────────
 * We execute 16-bit code on the real CPU. There are no cycles to count and nothing
 * to divide down, so a speed setting can only be a DUTY CYCLE: let the guest run,
 * then hold it for however much longer the ratio demands. The number on the menu is
 * therefore a CALIBRATED APPROXIMATION and this file says so out loud rather than
 * implying a precision it does not have.
 *
 * The calibration has exactly one constant -- CPUSPEED_REF_MHZ, the speed the host
 * presents to a guest when nothing is throttling it -- and it is MEASURED, not
 * guessed: tools/dostest/cpubench.asm runs a loop whose cost on a 486 is known to
 * the cycle and reports how many it completes per second. Everything else here is
 * arithmetic on that one number.
 *
 * ── TWO EXECUTION PATHS, TWO MECHANISMS ─────────────────────────────────────────
 *   V86 on the real CPU  -- the exec thread is inside VdmStartExecution and we hold
 *                           no locks, so a second thread can SuspendThread it for a
 *                           share of each period. That machinery is proven:
 *                           async_inject_irq() already suspends the same thread.
 *                           The hold is priced by cpuspeed_hold_us() off a run
 *                           phase that is MEASURED, not assumed -- see below.
 *   The interpreter      -- the easy half. It already counts instructions, so pace
 *                           slices against elapsed time directly: cpuspeed_charge().
 * The two never overlap. While the interpreter runs, the exec thread is executing
 * HOST code and g_in_exec is 0, which is exactly when the suspender refuses to act.
 *
 * Everything in here is integer arithmetic on plain values with no Windows in it,
 * so it is checked off-VM by tools/dostest/cpuspeed_test.c -- "the knob is wired"
 * and "the knob is wired and right" are not the same claim.
 */
#ifndef CPUSPEED_H
#define CPUSPEED_H

/* ── THE LIST, AND IT IS ALSO THE REGISTRY VALUE. ────────────────────────────────
     Index 0 is UNLIMITED and must stay index 0. This is the OPTIONAL SPEED LIMIT --
     for software that runs too fast on a modern PC -- and every entry is a real
     machine somebody owned, because that is how a person thinks about "slow it down
     to roughly this." Fastest first, which is the order a list of speeds reads in.
   ⚠⚠ TRIMMED FROM 18 ENTRIES TO 6 IN SESSION 60. The old ladder ran up to 3300 MHz
     -- ceilings that behave as Unlimited on any real box and are pure noise in a
     "limit speed" dropdown -- plus a rung every ~33 MHz that nobody reaches for. A
     shorter honest list was the point of the cleanup. Index 0 still means Unlimited,
     so an untouched machine is unaffected; a stored index that named one of the
     removed rungs falls OUT OF RANGE and settings_clamp resets it to Unlimited,
     which is visible rather than silently becoming a different speed.
   ⚠ ORDERING IS STILL THE CONTRACT: a new entry goes in its right place and
     renumbers the slower ones. The permanent fix (store MHz, not an index) is still
     the right thing the next time this needs to change without disturbing anyone. */
/* ── #224 (s84): THE USER'S LADDER, from docs/EMULATION.md -- real CPUs a person owned,
     fastest first, index 0 still Unlimited ("Host"). The 8 MHz rung is gone (not on
     the list). Rungs at or above THIS PC's own speed are greyed where the list is
     shown (cpuspeed_available), because a throttle is a ceiling, never a boost.
   ⚠ The registry row is "CpuSpeed", NEW with this list, so an index saved against the
     old six-entry ladder cannot silently become a different speed. */
#define CPUSPEED_COUNT   11
static const unsigned CPUSPEED_MHZ[CPUSPEED_COUNT] = {
    0,      /*  0: Host (Unlimited) -- the default                                */
    1000,   /*  1: Intel Pentium III 1 GHz                                        */
    600,    /*  2: Intel Pentium III 600 MHz                                      */
    300,    /*  3: Intel Pentium II 300 MHz                                       */
    200,    /*  4: Intel Pentium MMX 200 MHz                                      */
    133,    /*  5: Intel Pentium 133 MHz                                          */
    100,    /*  6: Intel 486DX4 100 MHz                                           */
    66,     /*  7: Intel 486DX2 66 MHz                                            */
    50,     /*  8: Intel 486DX 50 MHz                                             */
    33,     /*  9: Intel 386DX 33 MHz                                             */
    16      /* 10: Intel 386DX 16 MHz                                             */
};
static const char *const CPUSPEED_NAMES[CPUSPEED_COUNT] = {
    "Host (Unlimited)", "Intel Pentium III 1 GHz", "Intel Pentium III 600 MHz",
    "Intel Pentium II 300 MHz", "Intel Pentium MMX 200 MHz", "Intel Pentium 133 MHz",
    "Intel 486DX4 100 MHz", "Intel 486DX2 66 MHz", "Intel 486DX 50 MHz",
    "Intel 386DX 33 MHz", "Intel 386DX 16 MHz"
};
/* The '|'-separated form SET_DEFS wants. Kept adjacent to the table above so the
   two cannot drift; cpuspeed_test.c checks that they still agree. */
#define CPUSPEED_ITEMS \
    "Host (Unlimited)|Intel Pentium III 1 GHz|Intel Pentium III 600 MHz|" \
    "Intel Pentium II 300 MHz|Intel Pentium MMX 200 MHz|Intel Pentium 133 MHz|" \
    "Intel 486DX4 100 MHz|Intel 486DX2 66 MHz|Intel 486DX 50 MHz|" \
    "Intel 386DX 33 MHz|Intel 386DX 16 MHz"

/* Can THIS PC offer rung `idx`? Host is always there; a rung is only a real throttle
   below the host's own clock (`host_mhz`, 0 = unknown -> offer everything). */
static int cpuspeed_available(unsigned idx, unsigned host_mhz)
{
    if (idx == 0u || idx >= CPUSPEED_COUNT) return idx == 0u;
    return host_mhz == 0u || CPUSPEED_MHZ[idx] < host_mhz;
}

/* ── THE ONE CALIBRATION CONSTANT. ───────────────────────────────────────────────
     "How fast does an unthrottled NTVDMEX look to a DOS program, in MHz?"
     ⚠ MEASURED, NOT GUESSED, and re-measurable in one run: cpubench.asm reports
       iterations of a 12-cycle-on-a-486 loop per second, and MHz = 12 x that / 1e6.
       If the number below and a fresh run of that probe disagree, the probe wins.
     ★ 3661 is the bare-metal rig (192.168.1.29), measured 2026-09-06:
       620,007,424 iterations in 37 BIOS ticks. It is THAT BOX'S number and nobody
       else's -- which is the entire reason cpuref.txt exists.
     ⚠ It is also a FILE KNOB (cpuref.txt on the share) so the rig can be re-
       calibrated without a rebuild -- this is the sort of constant that is wrong
       on somebody else's machine by construction. */
/* ── "HOW FAST DOES AN UNTHROTTLED GUEST LOOK, IN MHz?" -- AND IT IS THE NATIVE ALU
     RATE AGAIN, MEASURED. ────────────────────────────────────────────────────────
     The duty is delivered ACCURATELY now (session 60: the closed-loop invariant
     holds guest execution to `duty` of wall time, delivered_bp tracks duty_bp to
     ~10% across the whole ladder on the rig, proven by a deterministic test). So the
     reference has one honest job left: set what a speed LABEL means. apparent =
     native x duty, and duty = mhz/ref, so apparent = mhz x native/ref -- which equals
     the label exactly when ref == native. So ref IS the native rate.
   ★ MEASURED 2026-09-09 on the rig (mixbench.com ALU case, Unlimited): 3704 MHz.
     "66 MHz" then delivers duty 66/3704 = 178 bp, and ALU runs at 3704 x 0.0178 =
     66 MHz. The label is honest for COMPUTE.
   ⚠⚠ THIS WENT BACK DOWN FROM 5900. The 5900 was a FIT to paper over a throttle that
     was itself inaccurate (open-loop debt carry, two leaks). With the mechanism fixed
     the fit is not just unnecessary, it is WRONG -- it would make every label ~1.6x
     fast. The lesson: do not calibrate a constant to hide a mechanism bug; fix the
     mechanism and the constant becomes what it always should have been, the measured
     native rate.
   ⚠ IT IS THIS BOX'S NUMBER. cpuref.txt on the share overrides it per machine, which
     is the whole reason that knob exists; the default is a sane modern figure.
   ► STILL TRUE, AND STILL NOT A CONSTANT'S JOB TO FIX: a port trap is ~6.9 MHz
     apparent (iobench case 3), below every label, so hardware-bound code runs slower
     than the label says -- as it did on period hardware, whose I/O was also slow next
     to its ALU. The dropdown is an approximation for compute and says so. */
#define CPUSPEED_REF_MHZ_DEFAULT 3704u

/* Duty cycle in BASIS POINTS (1/10000) for a speed index against a reference.
   10000 = run flat out. A target at or above the reference cannot be delivered by
   slowing down, so it clamps to 10000 and the setting is a CEILING, never a boost. */
/* ── #225: WHAT A RUNG MEANS IS WHAT THAT MACHINE DID, NOT ITS CLOCK. ─────────────
     The user: "Doom and Skyroads play easily on a 486DX2-66, but NTVDMEX at 100 MHz is
     unplayable." The old duty was MHz / ref, ref being this host's raw ALU rate (3704
     here) -- honest for a pure arithmetic loop, and ruinous for a game: Doom's guest
     time is dominated by traps and emulated video, not ALU, so unthrottled it runs as
     a Pentium II-350 (121 fps), and 66/3704 gave the 486DX2 rung a 1.8% share it could
     not finish demo3 in. Measured on the rig (s84, runs/s84/td/): throttled, fps is
     LINEAR in the share -- fps = 2134 x duty / E, with E = 8.67 s of guest time for the
     whole demo -- so 246 fps at a 100% share, i.e. 0.0664 fps per host ALU-MHz.
   ⇒ Each rung carries the REAL MACHINE's Doom 1.9s `-timedemo demo3` rate
     (CPUSPEED_DOOM_FPS10, tenths of fps; thandor.net/benchmark/32, one tester, PCI
     video on the 486s, 430VX on the Pentiums, see #225), and the share is
         duty = target_fps / (CPUSPEED_FPS_PER_KMHZ x ref_mhz / 1000)
     which carries to another PC through the ONE per-host number we already measure
     (ref_mhz, cpubench / cpuref.txt).
   ⚠ THE TRADE, STATED: labels now mean "plays like that machine" for games. A pure
     ALU delay loop runs faster than it did on the real chip (the 486DX2 rung is ~13%
     of a 3.7 GHz ALU, ~500 MHz of arithmetic). The acceptance test is the calibration
     (memory: acceptance-test-is-the-calibration), and the user's test is games. */
/* 73.0 fps per 1000 ref-MHz, in tenths. First derived as 66.4 (246 fps / 3.704) from a
   run whose guest time E included Doom's start-up; the rungs then measured ~10% fast
   (486DX2-66 37.1 vs 33.1, DX4-100 48.6 vs 43.8, P133 86.2 vs 80.2), so 664 x 1.10. */
#define CPUSPEED_FPS_PER_KMHZ10 730u
static const unsigned CPUSPEED_DOOM_FPS10[CPUSPEED_COUNT] = {
    0,      /*  0: Host -- unthrottled                                           */
    2100,   /*  1: Pentium III 1 GHz   (TU Wien PIII-800 188-202; above any cap)  */
    1900,   /*  2: Pentium III 600     (TU Wien PIII-500 183-191)                 */
    1150,   /*  3: Pentium II 300      (between PII-233 105.4 and PII-350 122.4)  */
    976,    /*  4: Pentium MMX 200     (thandor 97.63)                            */
    802,    /*  5: Pentium 133         (thandor 80.23)                            */
    438,    /*  6: 486DX4 100          (thandor 43.75)                            */
    331,    /*  7: 486DX2 66           (thandor 33.12)                            */
    280,    /*  8: 486DX 50            (thandor 28.00)                            */
    75,     /*  9: 386DX 33            (thandor Am386DX-33 7.47)                  */
    29      /* 10: 386DX 16            (386DX-25 4.58 scaled by clock, 16/25)     */
};

static unsigned cpuspeed_duty_bp(unsigned idx, unsigned ref_mhz)
{
    unsigned long long ceil10;                   /* fps x 10 at a 100% share, this host */
    unsigned fps10;
    if (idx >= CPUSPEED_COUNT || ref_mhz == 0u) return 10000u;
    fps10 = CPUSPEED_DOOM_FPS10[idx];
    if (fps10 == 0u) return 10000u;
    ceil10 = (unsigned long long)CPUSPEED_FPS_PER_KMHZ10 * ref_mhz / 1000ull;
    if (ceil10 == 0ull || fps10 >= ceil10) return 10000u;   /* a ceiling, never a boost */
    /* Round UP, and never to zero: a duty of 0 would stop the guest dead. */
    {   unsigned long long bp = ((unsigned long long)fps10 * 10000ull + ceil10 - 1ull) / ceil10;
        return bp ? (unsigned)bp : 1u; }
}

/* ── #225: A REAL-MODE PROGRAM GETS ITS OWN SHARE. ─────────────────────────────────
     The ladder above is Doom's: 32-bit protected-mode code, verified on the rig (the
     486DX2-66 run counted 62.0 s of Doom time in 63.3 s of wall, incl. load; no
     interpreted slices). At that share Skyroads -- 16-bit real-mode code -- drew ~16
     frames/s in-game where a real 486DX2-66 drew ~35 (user, rounds 11-14), with its
     timer and retrace both delivered in full. So this host is not a fixed multiple of
     a 486: how much faster it is depends on the code, and 16-bit real-mode code gains
     far less than Doom's 32-bit code did. One ladder cannot be right for both.
   ⇒ A program with no protected-mode client (the host's g_dpmi_pm) is throttled at
     the protected-mode share scaled by CPUSPEED_RM_PCT, clamped to flat out.
   ⚠ 200 IS BY EAR, AND LABELLED SO: Skyroads' in-game rate at 486DX2-66 against the
     user's memory of the real machine. The measured replacement is 3DBench V1.0
     against published real-486 results, blocked on #238 (it times itself on a 1 kHz
     timer we deliver ~44% of). */
#define CPUSPEED_RM_PCT 200u
static unsigned cpuspeed_duty_rm_bp(unsigned pm_bp)
{
    unsigned long long bp;
    if (pm_bp == 0u || pm_bp >= 10000u) return 10000u;
    bp = (unsigned long long)pm_bp * CPUSPEED_RM_PCT / 100u;
    return bp >= 10000ull ? 10000u : (unsigned)bp;
}

/* ── THE V86 HALF: HOW LONG THE GUEST RUNS, AND HOW LONG IT IS HELD. ─────────────
     One millisecond is the floor: Sleep() cannot express less even with the
     multimedia timer resolution raised. So the slice is a PAIR -- run for on_ms,
     hold for off_ms -- and the ratio between them, not the absolute size, is what
     delivers the speed.

   ⚠⚠ THE FIRST CUT SPREAD THIS OVER UNIFORM 1 ms SLOTS with a Bresenham, deciding
     per millisecond whether the guest got that one. It was correct arithmetic and
     it MEASURED 6x SLOWER where it had asked for 87x (rig sweep, 3661 -> 602 MHz
     at a 1.15% duty). The reason is that the mechanism has a fixed cost per slot:
     a SuspendThread / GetThreadContext / ResumeThread round trip and a Sleep whose
     real granularity is 1-2 ms, so a 1 ms hold delivered about half a millisecond
     of actual holding and the guest ran free through the rest. Amortise the SAME
     cost over one long hold instead and it disappears into the noise.
   ⇒ So: keep on_ms at its floor and make off_ms as long as the ratio demands.
     At 33 MHz against a 3661 MHz reference that is 1 ms of running per 108 ms of
     held, which is what 0.9% of a 3.6 GHz machine actually means.

   ⚠ AND THAT IS THE HONEST COST OF THE FEATURE: a slow setting is CHUNKY. The
     guest advances in ~11 bursts a second rather than continuously, because 1 ms
     is the smallest unit this host can hand out and everything else follows from
     it. A real 8 MHz machine was slow and smooth; this is slow and stepped. The
     only finer lever is a cycle-counting interpreter, which is the thing this
     project exists not to be. */
#define CPUSPEED_WAIT_MS    1u     /* legacy: superseded by the granularity lever   */
#define CPUSPEED_MAX_OFF_MS 1000u  /* a hold longer than this is a hang, not a knob */

/* ── ★★★★ GRANULARITY: THE LEVER THAT DECIDES WHETHER A SETTING IS PLAYABLE. ─────
 * ⚠⚠ THROUGHPUT WAS ALREADY ROUGHLY RIGHT AND THE FEATURE WAS STILL UNUSABLE. The
 *    user's report after the calibration work: "66 MHz is still unplayable." The
 *    counters say why, and it is not the amount of work delivered -- it is the SHAPE
 *    of the delivery. Measured at index 11 on the rig:
 *        ran_us=1551  wall_us=1954  run_ms=44  held_ms=3982
 *    i.e. the guest runs ~1.55 ms, is frozen ~140 ms, and repeats: **about SEVEN
 *    BURSTS A SECOND**. A game rendering 35 frames a second gets its whole second's
 *    work in seven clumps. That is a slideshow whatever the average says, and no
 *    calibration constant can touch it -- which is why the last session's
 *    recalibration improved the numbers and not the experience.
 *
 * ── THE ARITHMETIC, WHICH NAMES THE FIX EXACTLY ─────────────────────────────────
 *    period = run / duty.  So the period is set by THE RUN PHASE, not by the hold.
 *    At 66 MHz the duty is ~1.1%, so for a period inside one 60 Hz frame (16 ms)
 *    the run phase must be at most 16 x 0.011 = 0.17 ms = 170 us.
 *    It was 1551 us. And the reason is one statement: `Sleep(CPUSPEED_WAIT_MS)`
 *    before reaching for the guest. Sleep(1) with the multimedia timer at 1 ms is
 *    1-2 ms in practice -- an order of magnitude more than the budget allows.
 * ⇒ So make the run phase a TARGET PERIOD instead of a fixed millisecond, and let
 *   it go to zero: suspend immediately and let the round trip itself be the run.
 *
 * ── WHY THIS IS A SLIDER AND NOT A CONSTANT (the user's WinAmp analogy) ─────────
 *   The floor is the SuspendThread / GetThreadContext / ResumeThread round trip,
 *   and that is a property of the MACHINE, not of us. Below it there is no run
 *   phase to shorten. Above it, every halving of the period doubles the number of
 *   round trips per second -- smoothness bought with host CPU. That is exactly the
 *   trade WinAmp's refresh slider exposed, and exactly what its auto-detect button
 *   measured. So: a target period the user can move, and an AUTO setting that
 *   measures the round trip on this box and picks the finest period it can sustain.
 * ⚠ AUTO MUST MEASURE, NOT ASSUME. The round trip depends on core count, on what
 *   else is running and on whether the target is inside a syscall -- the whole
 *   reason a constant was wrong the first three times this feature was tuned.
 */
#define CPUSPEED_GRAN_AUTO      0u     /* 0 = measure the round trip and choose     */
#define CPUSPEED_GRAN_MIN_MS    2u     /* finer than this is round trips, not speed */
#define CPUSPEED_GRAN_MAX_MS    250u   /* coarser than this is the old slideshow    */
#define CPUSPEED_GRAN_DEFAULT   16u    /* one 60 Hz frame: the bar to clear         */

/* The finest period this host can actually deliver at `duty_bp`, given a measured
   round-trip cost. Below rt_us/duty there is no run phase left to shorten.
   ⚠ ALSO FLOORED BY THE HOLD: Sleep cannot express less than a millisecond, so a
     period whose OFF phase rounds to zero delivers no throttling at all -- the debt
     carries, but the guest runs free meanwhile. Hence the second term. */
static unsigned cpuspeed_period_floor_ms(unsigned duty_bp, unsigned long rt_us)
{
    unsigned long by_run, by_hold;
    if (!duty_bp || duty_bp >= 10000u) return CPUSPEED_GRAN_MIN_MS;
    if (!rt_us) rt_us = 100ul;                      /* unmeasured: a sane placeholder */
    by_run  = (rt_us * 10000ul + duty_bp - 1ul) / duty_bp / 1000ul;  /* ms */
    by_hold = (10000ul + (10000u - duty_bp) - 1ul) / (10000u - duty_bp);
    if (by_hold < 1ul) by_hold = 1ul;
    { unsigned long f = by_run > by_hold ? by_run : by_hold;
      if (f < CPUSPEED_GRAN_MIN_MS) f = CPUSPEED_GRAN_MIN_MS;
      if (f > CPUSPEED_GRAN_MAX_MS) f = CPUSPEED_GRAN_MAX_MS;
      return (unsigned)f; }
}

/* How long to let the guest run this period, in MICROSECONDS, for a target period.
   Returns 0 when the answer is "do not wait at all -- reach for it immediately",
   which is the fine end of the slider and the whole point of it. */
static unsigned long cpuspeed_run_us(unsigned duty_bp, unsigned period_ms)
{
    if (!duty_bp || duty_bp >= 10000u) return 0ul;
    return ((unsigned long)period_ms * 1000ul * duty_bp) / 10000ul;
}

/* ── ★ THE RUN PHASE IS MEASURED, NOT ASSUMED, AND THAT IS THE WHOLE DESIGN. ─────
     The second cut asked for a 1 ms run and computed the hold from that constant.
     It came out 3.5x TOO FAST at every single setting -- 56 MHz where 16 was asked,
     740 where 200 was -- which is the signature of a FIXED per-period cost the
     arithmetic knew nothing about. Sleep(1) is not 1 ms, and stopping a thread that
     is inside VdmStartExecution is not free: the kernel has to unwind it out of V86
     first. Whatever that costs, the guest is RUNNING for all of it.
   ⇒ So stop guessing the run phase and time it: from the moment we let the guest go
     to the moment a suspend actually lands is exactly how long it ran, wall clock,
     including every cost we did not think of. Then hold for whatever that implies.
     Self-correcting, and it absorbs Sleep's inaccuracy and the suspend latency
     without either of them having to be named or measured separately.
   ⚠ A constant would have had to be re-derived for every machine. This does not. */

/* ── ★★★★ THE CLOSED-LOOP INVARIANT: keep exec time a fixed fraction of wall time.
 * The throttle has exactly one job -- hold guest EXECUTION time E to the fraction
 * `duty` of WALL time T:  E = duty * T.  Everything the old code bookkept by hand
 * (the per-run hold, the carried debt, jitter tolerance, saturation) is a
 * CONSEQUENCE of that one equation, so compute it directly and let the equation keep
 * the books.
 *
 * After the guest has executed E microseconds, wall time SHOULD read E/duty for the
 * ratio to hold. So the hold needed right now is simply:
 *       hold = max(0, E/duty - T)
 * where E and T are RUNNING TOTALS since the last time the guest was caught up.
 *
 * ⚠ WHY THIS REPLACED THE DEBT CARRY (hold_us + pay_ms + owed_us), session 60. The
 *   open-loop version priced each run phase alone and accumulated the remainder in
 *   `owed_us`. The arithmetic was right and the LOOP was wrong TWICE: a baseline
 *   re-sampled on a failed suspend discarded uncharged execution, and a spin-guard
 *   Sleep(1) added an off phase the accounting never saw. Both are impossible here,
 *   because there is no per-period state to fall out of step -- E and T are the only
 *   state and they are MEASURED, not accumulated by hand.
 * ★ SELF-CORRECTING AND JITTER-PROOF. Sleep overshoots? T ran ahead, next hold is
 *   shorter, no credit is banked. Sleep undershoots? T is behind, next hold is
 *   longer. A descheduled 19 ms outlier corrects itself the same way -- which is the
 *   exact failure ("non-monotonic below 33 MHz") the debt carry was chasing.
 * ★ SATURATION IS HONEST WITH NO SPECIAL CASE. One hold is capped (a guest frozen
 *   >1 s has stopped answering, it is not "slow"). If a setting needs more than the
 *   cap, T never reaches E/duty, delivered E/T stays above the target -- and that IS
 *   the true achievable speed, reported not hidden.
 * ★ AND THE PORT-TRAP CEILING FALLS OUT. If host overhead alone already makes the
 *   guest slower than the target (E/T < duty before we hold at all) the hold is 0
 *   and we deliver E/T: we can add holds, never remove the host's own trap cost, so
 *   delivered = min(target, the guest's own trap-limited rate). Physical truth.
 *
 * ⚠ 64-BIT. E and T are totals across a window that only resets when the guest is
 *   caught up, so under saturation they grow; E*10000 stays inside 64 bits for the
 *   life of any session. */
#define CPUSPEED_MAX_WINDOW_US 60000000ull  /* force a rebaseline after this, bounded */
/* ── ★ WHERE THE 1 ms RUN-PHASE FLOOR KICKS IN (see the throttle loop). Above this
     duty the immediate-catch hold is too small to swamp the per-period catch cost, so
     the guest must run a Sleep-able chunk first; below it, an immediate catch already
     earns a large hold and the floor would over-run. Rig-tuned against ref 3704: 8 MHz
     (22 bp) must NOT floor -- it over-runs 1.5x if it does -- and 16 MHz (44 bp) must;
     the threshold sits between. Keyed on duty, which already carries the reference. */
#define CPUSPEED_RUN_FLOOR_BP  32u

static unsigned long long cpuspeed_hold_for(unsigned long long exec_us,
                                            unsigned long long wall_us,
                                            unsigned duty_bp)
{
    unsigned long long target;
    if (duty_bp == 0u || duty_bp >= 10000u) return 0ull;   /* unlimited: never hold */
    target = (exec_us * 10000ull) / (unsigned long long)duty_bp;
    return target > wall_us ? target - wall_us : 0ull;
}

/* One period of the controller, as a PURE FUNCTION so the loop and the deterministic
   test run the identical law (tools/dostest/cpuspeed_test.c drives this against a
   simulated clock). E and T are totals since the window began; returns the hold to
   take now and sets *reset when the caller should rebaseline the window.
 ⚠ *reset FIRES ONLY WHEN THE GUEST IS AT OR AHEAD OF TARGET (hold rounds to nothing),
   never on a paid-but-positive hold. That is what makes this survive Sleep's 1 ms
   granularity: a sub-millisecond hold is NOT forgiven -- it rides forward in the
   running totals until it is worth a whole millisecond -- and the ONLY thing a reset
   throws away is accumulated LEAD (the guest ran slower than asked), which must not
   be bankable as a later burst. A stall (wall jumps, exec flat) lands here too and is
   correctly forgiven. The window bound is the only other reason to rebaseline. */
static unsigned long long cpuspeed_step(unsigned long long E, unsigned long long T,
                                        unsigned duty_bp, unsigned long long cap_us,
                                        int *reset)
{
    unsigned long long raw = cpuspeed_hold_for(E, T, duty_bp);
    unsigned long long hold = raw > cap_us ? cap_us : raw;
    *reset = (raw == 0ull) || (T > CPUSPEED_MAX_WINDOW_US);
    return hold;
}

/* The duty actually achieved over a window, in basis points: exec / wall. This is
   what the guest FEELS, and it equals the target only when the target is reachable
   -- below the port-trap ceiling and inside the hold cap. Logged beside the
   requested duty so the two can disagree in the open rather than the label lying. */
static unsigned cpuspeed_delivered_bp(unsigned long long exec_us,
                                      unsigned long long wall_us)
{
    if (!wall_us) return 0u;
    return (unsigned)((exec_us * 10000ull) / wall_us);
}

/* ── THE INTERPRETER HALF: PACE BY INSTRUCTIONS, NOT BY DUTY. ────────────────────
     Here we know exactly how much work was done, so the throttle can be precise
     rather than statistical. Charge the instructions a slice really executed
     against the budget the setting allows, and carry the remainder in MICROSECONDS
     so a debt smaller than a millisecond is never rounded away -- rounding it away
     is how a throttle silently becomes a no-op at the fast settings. */
typedef struct { long long owed_us; } cpuspeed_pace;

/* Instructions per second a setting allows. 0 = unlimited.
   ⚠ CPUSPEED_CPI IS AN ASSUMPTION AND IS LABELLED AS ONE: 486 mixed code averages
     somewhere around three cycles per instruction once memory is in the picture.
     It is the only unmeasured number in this file. It affects the interpreter path
     only, and it is a scale factor -- if the interpreter comes out uniformly fast
     or slow against the V86 path at the same setting, this is the constant to move. */
#define CPUSPEED_CPI 3u
static unsigned long cpuspeed_ips(unsigned idx)
{
    if (idx == 0u || idx >= CPUSPEED_COUNT) return 0ul;
    return (unsigned long)CPUSPEED_MHZ[idx] * 1000000ul / CPUSPEED_CPI;
}

/* Charge `ran` instructions that really took `elapsed_us`, and return how many
   WHOLE MILLISECONDS to sleep to make them take as long as the setting says they
   should. The sub-millisecond remainder stays owed.
 ⚠ THE DEBT IS CLAMPED. A slice that arrives after the host was descheduled for a
   second would otherwise book a second of sleep and stall the guest visibly; and a
   guest running FASTER than the setting (elapsed > owed) must not accumulate
   negative debt it can spend later as a burst. Both ends are held. */
static int cpuspeed_charge(cpuspeed_pace *p, unsigned long ran, unsigned long ips,
                           long long elapsed_us)
{
    long long want_us, ms;
    if (!ips || !ran) { if (p->owed_us < 0) p->owed_us = 0; return 0; }
    want_us = ((long long)ran * 1000000ll) / (long long)ips;
    p->owed_us += want_us - elapsed_us;
    if (p->owed_us < 0) p->owed_us = 0;              /* no credit for running fast */
    if (p->owed_us > 100000ll) p->owed_us = 100000ll; /* 100 ms ceiling on one debt */
    ms = p->owed_us / 1000ll;
    p->owed_us -= ms * 1000ll;
    return (int)ms;
}

#endif /* CPUSPEED_H */
