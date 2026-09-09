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
#define CPUSPEED_COUNT   6
static const unsigned CPUSPEED_MHZ[CPUSPEED_COUNT] = {
    0,      /* 0: Unlimited -- the host's own speed, and the default             */
    100,    /* 1: Pentium / 486DX4                                               */
    66,     /* 2: 486DX2-66                                                      */
    33,     /* 3: 486DX-33 / 386DX-33                                            */
    16,     /* 4: 386SX-16 -- Skyroads' stated target hardware                   */
    8       /* 5: 8088 / 286 era                                                 */
};
static const char *const CPUSPEED_NAMES[CPUSPEED_COUNT] = {
    "Unlimited", "100 MHz", "66 MHz", "33 MHz", "16 MHz", "8 MHz"
};
/* The '|'-separated form SET_DEFS wants. Kept adjacent to the table above so the
   two cannot drift; cpuspeed_test.c checks that they still agree.
 ⚠ A speed AT OR ABOVE the host's own is not a throttle and cannot be: it clamps to
   flat out (see cpuspeed_duty_bp), so on a slow enough host the faster entries just
   behave as Unlimited -- honest, they are ceilings and never boosts. */
#define CPUSPEED_ITEMS \
    "Unlimited|100 MHz|66 MHz|33 MHz|16 MHz|8 MHz"

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
static unsigned cpuspeed_duty_bp(unsigned idx, unsigned ref_mhz)
{
    unsigned mhz;
    if (idx >= CPUSPEED_COUNT) return 10000u;
    mhz = CPUSPEED_MHZ[idx];
    if (mhz == 0u || ref_mhz == 0u || mhz >= ref_mhz) return 10000u;
    /* Round UP, and never to zero: a duty of 0 would stop the guest dead, which is
       a hang wearing a setting's clothes. The slowest expressible speed is one
       millisecond in every ten thousand, which is far below 8 MHz on any host. */
    {   unsigned bp = (mhz * 10000u + ref_mhz - 1u) / ref_mhz;
        return bp ? bp : 1u; }
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
