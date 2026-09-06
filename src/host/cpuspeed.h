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
 * to divide down, so a speed setting can only be a DUTY CYCLE: let the guest run for
 * a slice of each millisecond and hold it for the rest. The number on the menu is
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
 *                           slice of each millisecond. That machinery is proven:
 *                           async_inject_irq() already suspends the same thread.
 *                           cpuspeed_step() is the Bresenham that decides per ms.
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

/* ── THE MENU, AND IT IS ALSO THE REGISTRY VALUE. ────────────────────────────────
     Index 0 is UNLIMITED and must stay index 0: this list replaces the old dead
     `SpeedMode` combo ("Auto|Maximum|Fixed cycles"), whose index 0 meant "do
     nothing" too, so every value already in a registry somewhere keeps its
     meaning. The list runs FASTEST FIRST so the migration of the other two old
     indices is the least surprising one available -- "Maximum" (1) lands on the
     fastest throttled setting rather than the slowest.
   ⚠ The ordering is load-bearing for that reason. Add new speeds at the END. */
#define CPUSPEED_COUNT   7
static const unsigned CPUSPEED_MHZ[CPUSPEED_COUNT] = {
    0,      /* 0: Unlimited -- the shipped behaviour, and the default            */
    200,    /* 1: Pentium MMX era                                                */
    100,    /* 2: Pentium / 486DX4                                               */
    66,     /* 3: 486DX2-66                                                      */
    33,     /* 4: 486DX-33 / 386DX-33                                            */
    16,     /* 5: 386SX-16  -- Skyroads' stated target hardware                  */
    8       /* 6: 8086/286 era                                                   */
};
static const char *const CPUSPEED_NAMES[CPUSPEED_COUNT] = {
    "Unlimited", "200 MHz", "100 MHz", "66 MHz", "33 MHz", "16 MHz", "8 MHz"
};
/* The '|'-separated form SET_DEFS wants. Kept adjacent to the table above so the
   two cannot drift; cpuspeed_test.c checks that they still agree. */
#define CPUSPEED_ITEMS "Unlimited|200 MHz|100 MHz|66 MHz|33 MHz|16 MHz|8 MHz"

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
#define CPUSPEED_REF_MHZ_DEFAULT 3661u

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
#define CPUSPEED_WAIT_MS    1u     /* how long to let it run before trying to hold */
#define CPUSPEED_MAX_OFF_MS 1000u  /* a hold longer than this is a hang, not a knob */

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

/* How long the guest OWES for having run ran_us, in microseconds. Uncapped: this
   is the requirement, not what we are able to pay in one go.
   Returns 0 when there is nothing to do (unlimited, or a run too short to price). */
static unsigned long cpuspeed_hold_us(unsigned duty_bp, unsigned long ran_us)
{
    if (duty_bp >= 10000u || duty_bp == 0u || !ran_us) return 0ul;
    return (ran_us * (unsigned long)(10000u - duty_bp)) / (unsigned long)duty_bp;
}

/* ── ★ THE DEBT IS CARRIED, AND THAT IS WHAT MAKES THE SLOW SETTINGS WORK. ───────
     A single hold is capped, because a guest frozen for several seconds is not a
     slow machine, it is one that has stopped answering. But the run phase is
     MEASURED, and measurements have outliers: on the rig it is usually ~2 ms and
     occasionally 19 ms (the whole box gets descheduled after a long hold). Pricing
     each period independently and then clamping THREW THE REMAINDER AWAY, so one
     long run bought the guest 19 ms of free execution it never paid for -- which is
     exactly why the first adaptive cut came out at 52 MHz where 33 was asked, and
     came out NON-MONOTONIC below that.
   ⇒ So carry the unpaid remainder into the next period. Each hold pays what it can;
     the long-run average is then exact for any setting whose average requirement
     fits under the cap, and saturates -- visibly, in the log -- for any that does
     not. Same shape as the interpreter half's microsecond debt, for the same reason.
   ⚠ THE DEBT IS BOUNDED. A setting the host cannot reach would otherwise accumulate
     an ever-growing arrears that would go on freezing the guest long after the user
     had put the speed back to Unlimited. */
#define CPUSPEED_MAX_OWED_US 4000000ul     /* four seconds of arrears, and no more */

/* Pay what we can off the debt. Updates *owed_us and returns the milliseconds to
   sleep now (0 = nothing owed yet, so do not sleep at all). */
static unsigned cpuspeed_pay_ms(unsigned long *owed_us)
{
    unsigned long ms;
    if (*owed_us > CPUSPEED_MAX_OWED_US) *owed_us = CPUSPEED_MAX_OWED_US;
    ms = *owed_us / 1000ul;
    if (!ms) return 0u;
    if (ms > CPUSPEED_MAX_OFF_MS) ms = CPUSPEED_MAX_OFF_MS;
    *owed_us -= ms * 1000ul;
    return (unsigned)ms;
}

/* ── ★ WHAT IT ACTUALLY DELIVERS, WHICH IS NOT ALWAYS WHAT WAS ASKED. ────────────
     The hold is capped, because a hold long enough to deliver 8 MHz on a 3.6 GHz
     box is over a second and a guest frozen for a second is not a slow machine, it
     is one that has stopped answering. So on a fast host the slowest settings floor
     out -- and a throttle that quietly delivers 13 MHz while the menu says 8 is the
     "runs but lies" class this project treats as the most expensive kind of defect.
     Hence this: the host logs requested AND delivered, every run, side by side.
     ran_us is the measured run phase; 0 means unlimited. */
static unsigned cpuspeed_delivered_mhz(unsigned idx, unsigned ref_mhz,
                                       unsigned long ran_us)
{
    unsigned bp = cpuspeed_duty_bp(idx, ref_mhz);
    unsigned long hold_us;
    if (bp >= 10000u || !ran_us) return 0u;
    hold_us = cpuspeed_hold_us(bp, ran_us);
    /* With the debt carried, a setting is delivered in full as long as its AVERAGE
       requirement fits under the cap. Past that it saturates, and the saturated
       figure -- not the label -- is what the guest will feel. */
    if (hold_us > (unsigned long)CPUSPEED_MAX_OFF_MS * 1000ul)
        hold_us = (unsigned long)CPUSPEED_MAX_OFF_MS * 1000ul;
    return (unsigned)(((unsigned long)ref_mhz * ran_us) / (ran_us + hold_us));
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
