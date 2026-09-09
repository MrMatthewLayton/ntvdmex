/* cpuspeed_test.c -- off-VM battery for the CPU-speed throttle's arithmetic
 * (src/host/cpuspeed.h). GH #56.
 *
 * The throttle itself needs a suspended thread and a real guest, neither of which
 * exists on the build machine. What it is MADE OF is integer arithmetic -- a duty
 * cycle, a Bresenham that turns that duty into whole milliseconds, and an
 * instruction budget with a microsecond debt -- and every one of those has a
 * failure mode that would look, on the rig, exactly like "the setting does
 * nothing": a duty that rounds to zero, an accumulator that drifts, a debt smaller
 * than a millisecond that is discarded on every slice. Those are checked here.
 */
#include <stdio.h>
#include <string.h>
#include "cpuspeed.h"

/* ⚠ FIND A SPEED BY ITS MHz, NEVER BY A HARD-CODED INDEX. The list is ordered and
   extensible, so an index written into a test is a fact about today's table rather
   than about the behaviour -- and when the table grew from 7 entries to 18 every
   such test would have gone on passing while checking the wrong speed. */
static unsigned idx_of(unsigned mhz)
{
    unsigned i;
    for (i = 0; i < CPUSPEED_COUNT; ++i) if (CPUSPEED_MHZ[i] == mhz) return i;
    return CPUSPEED_COUNT;   /* not found: an off-end index, never a silent 0 */
}

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

/* ── ★★★ THE DETERMINISTIC HEART OF THE THROTTLE. ─────────────────────────────────
 *  cpuspeed_step (cpuspeed.h) IS the whole control law, and here it is driven against
 *  a SIMULATED clock -- no threads, no Sleep, no hardware -- so the result is
 *  bit-for-bit repeatable. That is the point the user asked for: the rig could only
 *  measure apparent MHz through the guest's own BIOS tick, which the throttle
 *  starves, so the instrument was circular. This is not. It asserts the ONE contract
 *  the throttle exists to keep -- over a window, delivered exec/wall equals the
 *  requested duty, or the workload's own overhead ceiling if that is slower.
 *
 *  A "slice" is one turn of the loop: the guest executes dE us over dWall us of wall
 *  time (dWall >= dE models host/trap overhead), then it is held. The wall[] pattern
 *  cycles, so a descheduled OUTLIER slice is exercised -- that outlier is exactly
 *  what made the old open-loop debt carry come out non-monotonic on the rig. */
static unsigned sim_delivered_bp(unsigned duty_bp, unsigned long long cap_us,
                                 const unsigned *wall, int nwall,
                                 unsigned e_num, unsigned e_den,
                                 unsigned long long total_us)
{
    unsigned long long E = 0, T = 0;       /* monotonic simulated totals */
    unsigned long long e0 = 0, t0 = 0;     /* the window baseline cpuspeed_step tracks */
    int w = 0;
    while (T < total_us) {
        unsigned long long dWall = wall[w % nwall];
        unsigned long long dE = dWall * e_num / e_den;
        int reset;
        unsigned long long hold;
        ++w;
        E += dE; T += dWall;               /* the run slice */
        hold = cpuspeed_step(E - e0, T - t0, duty_bp, cap_us, &reset);
        T += hold;                         /* the guest is held */
        if (reset) { e0 = E; t0 = T; }     /* rebaseline at the real post-hold clock */
    }
    return cpuspeed_delivered_bp(E, T);     /* the same ratio the host reports */
}
/* Within `tol` basis points of `want`. */
static int near_bp(unsigned got, unsigned want, unsigned tol)
{ return (got >= want ? got - want : want - got) <= tol; }

int main(void)
{
    unsigned i;

    printf("== CPU speed: the table (cpuspeed.h) ==\n");

    /* ⚠ INDEX 0 MUST BE UNLIMITED. This list replaced the dead SpeedMode combo,
         whose index 0 also meant "do nothing", so every value already sitting in
         somebody's registry keeps its meaning across the upgrade. If this ever
         fails, an existing installation silently acquires a throttle. */
    CHECK(CPUSPEED_MHZ[0] == 0, "index 0 is Unlimited, so old SpeedMode=0 still means no throttle");
    CHECK(strcmp(CPUSPEED_NAMES[0], "Unlimited") == 0, "...and it says so");

    /* Fastest first: the old "Maximum" (1) lands on the fastest throttled speed. */
    {   int desc = 1;
        for (i = 2; i < CPUSPEED_COUNT; ++i)
            if (CPUSPEED_MHZ[i] >= CPUSPEED_MHZ[i - 1]) desc = 0;
        CHECK(desc, "speeds run fastest-first, which is what makes the migration sane"); }

    /* The '|' string and the name table are two spellings of one list. They are
       adjacent in the header precisely so they cannot drift, and this is the check
       that says they have not: a mismatch would put the wrong label on every row of
       the dropdown while every value behind it stayed correct. */
    {   const char *p = CPUSPEED_ITEMS; int n = 0, ok = 1;
        for (i = 0; i < CPUSPEED_COUNT; ++i) {
            size_t len = strlen(CPUSPEED_NAMES[i]);
            if (strncmp(p, CPUSPEED_NAMES[i], len) != 0) { ok = 0; break; }
            p += len; ++n;
            if (i + 1 < CPUSPEED_COUNT) { if (*p != '|') { ok = 0; break; } ++p; }
        }
        CHECK(ok && n == CPUSPEED_COUNT && *p == 0,
              "CPUSPEED_ITEMS spells out exactly CPUSPEED_NAMES, in order"); }

    printf("== CPU speed: the duty cycle ==\n");

    CHECK(cpuspeed_duty_bp(0, 700) == 10000, "Unlimited runs flat out");
    CHECK(cpuspeed_duty_bp(99, 700) == 10000, "an index off the end runs flat out, not at zero");
    CHECK(cpuspeed_duty_bp(idx_of(33), 0) == 10000, "a zero reference cannot throttle (never divide by it)");

    /* 33 of 700 = 4.71% -> 472bp with the round-up. The exact value matters less
       than that it is in the right neighbourhood and monotonic; a 10x error here
       is the difference between "slow" and "stopped". */
    {   unsigned bp = cpuspeed_duty_bp(idx_of(33), 700);
        CHECK(bp >= 460 && bp <= 480, "33 MHz against a 700 MHz reference is ~4.7% duty"); }

    /* ⚠ MONOTONIC ONLY BELOW THE REFERENCE, and the exception is the point. Every
         speed at or above the host's own clamps to flat out, so on a 50 MHz box the
         100 and 66 MHz entries are TIED at 10000 -- correct (they are ceilings, not
         boosts) and would fail a naive "strictly decreasing" check the moment the
         list runs past the reference. (The reference here is 50 rather than 700
         because the session-60 ladder tops out at 100 MHz.) */
    {   int mono = 1, ties = 0;
        for (i = 2; i < CPUSPEED_COUNT; ++i) {
            unsigned a = cpuspeed_duty_bp(i - 1, 50), b = cpuspeed_duty_bp(i, 50);
            if (CPUSPEED_MHZ[i - 1] >= 50u) { if (a != 10000u) mono = 0; ties++; continue; }
            if (b >= a) mono = 0;
        }
        CHECK(mono, "below the reference a slower setting is always a smaller duty");
        CHECK(ties > 0, "...and the speeds above it are all flat out, which is why"); }

    /* ⚠ THE ROUND-TO-ZERO TRAP. On a very fast host the slowest setting divides to
         under half a basis point. Truncating that to 0 would HOLD THE GUEST FOREVER
         -- a hang wearing a setting's clothes -- so it must clamp to 1. */
    CHECK(cpuspeed_duty_bp(idx_of(8), 4000000u) >= 1, "an absurd reference still leaves the guest some time");

    /* A target at or above the reference is a ceiling, not a boost: we cannot make
       the host faster and must not pretend by handing back more than 100%. */
    CHECK(cpuspeed_duty_bp(idx_of(100), 66) == 10000,
          "a speed above the reference clamps to flat out -- a ceiling, never a boost");

    printf("== CPU speed: the throttle delivers the requested duty (deterministic) ==\n");

    /* The cap on one hold, in microseconds (CPUSPEED_MAX_OFF_MS = 1000 ms). Below
       this, a setting is reachable and the invariant delivers it exactly; above it,
       the setting saturates and delivers honestly-faster-than-asked. */
    {   const unsigned long long CAP = (unsigned long long)CPUSPEED_MAX_OFF_MS * 1000ull;
        const unsigned steady[]  = { 2000 };                       /* a plain 2 ms slice   */
        const unsigned jitter[]  = { 2000,2000,2000,19000,2000 };  /* a descheduled outlier*/
        const unsigned fine[]    = { 100 };                        /* the smooth end       */
        const unsigned long long RUN = 20000000ull;                /* 20 s of simulated run*/

        /* ── 1. PURE COMPUTE, STEADY SLICES: DELIVERED == REQUESTED, EXACTLY. ──────
             This is the claim the rig kept failing to prove. With no trap overhead
             each window pays to exactly E/duty, so the ratio is the duty to the bp.
             Every duty on the trimmed ladder, against the shipped reference. */
        {   int ok = 1; unsigned bad = 0;
            for (i = 1; i < CPUSPEED_COUNT; ++i) {
                unsigned d   = cpuspeed_duty_bp(i, CPUSPEED_REF_MHZ_DEFAULT);
                unsigned got = sim_delivered_bp(d, CAP, steady, 1, 1, 1, RUN);
                /* Reachable at 2 ms slices iff one slice's hold fits the cap. */
                if ((unsigned long long)2000 * 10000ull / d - 2000ull > CAP) continue;
                if (!near_bp(got, d, 2)) { ok = 0; bad = i; }
            }
            CHECK(ok, "every reachable ladder speed is delivered to within 2 bp of its duty");
            (void)bad; }

        /* ── 2. THE OUTLIER, ABSORBED. A 19 ms descheduled slice among 2 ms ones was
             what made the debt carry non-monotonic. The closed loop prices each slice
             against the running total, so the average is still the duty exactly. */
        {   unsigned d = cpuspeed_duty_bp(idx_of(33), CPUSPEED_REF_MHZ_DEFAULT);
            unsigned smooth = sim_delivered_bp(d, CAP, steady, 1, 1, 1, RUN);
            unsigned rough  = sim_delivered_bp(d, CAP, jitter, 5, 1, 1, RUN);
            CHECK(near_bp(rough, d, 3), "a descheduled 19 ms outlier does not move the delivered speed");
            CHECK(near_bp(rough, smooth, 3), "...it is within 3 bp of the jitter-free run"); }

        /* ── 3. THE PORT-TRAP CEILING, WHICH IS PHYSICS, NOT A BUG. A workload that
             only executes 40% of its wall time (the rest trapped in the host) cannot
             be sped past 40% however high the target; a lower target is still hit. */
        {   unsigned near40 = sim_delivered_bp(5000, CAP, steady, 1, 2, 5, RUN);   /* want 50% */
            unsigned at20   = sim_delivered_bp(2000, CAP, steady, 1, 2, 5, RUN);   /* want 20% */
            CHECK(near_bp(near40, 4000, 20), "a 40%-overhead workload tops out near 40%, not the 50% asked");
            CHECK(near_bp(at20, 2000, 5),    "...but a target below that ceiling is delivered exactly"); }

        /* ── 4. SATURATION IS HONEST, AND GRANULARITY IS ITS CURE. A duty low enough
             that one 2 ms slice needs a hold past the 1 s cap CANNOT be reached at
             coarse slices -- it saturates and delivers FASTER than asked -- but the
             SAME duty at fine (100 us) slices fits the cap and lands exactly. This is
             the whole "smoothness" story, deterministically, and it is pinned to a
             fixed low duty (10 bp) rather than a ladder label so it does not depend on
             the reference: 2 ms / 0.001 = 2 s hold >> the 1 s cap. */
        {   unsigned d      = 10u;                                    /* 0.10% */
            unsigned coarse = sim_delivered_bp(d, CAP, steady, 1, 1, 1, RUN);
            unsigned smooth = sim_delivered_bp(d, CAP, fine,   1, 1, 1, RUN);
            CHECK(coarse > d && !near_bp(coarse, d, 3),
                  "0.1% at coarse slices saturates -- delivered faster than asked, in the open");
            CHECK(near_bp(smooth, d, 2), "...and at fine slices it fits the cap and is delivered exactly"); }

        /* ── 5. A HOLD IS BOUNDED, so an absurd setting cannot freeze the guest: even
             at a 1 bp duty the single hold never exceeds the cap. */
        {   int reset; unsigned long long h = cpuspeed_step(2000, 0, 1, CAP, &reset);
            CHECK(h <= CAP, "one hold is capped, so an unreachable setting slows but never freezes");
            CHECK(!reset, "...and a capped hold is carried, not forgiven"); }

        /* ── 6. UNLIMITED NEVER HOLDS, whatever the totals. */
        {   int reset;
            CHECK(cpuspeed_step(999999, 0, 10000, CAP, &reset) == 0 && reset,
                  "Unlimited holds for nothing and keeps no window"); }
        (void)jitter; (void)fine;
    }

    printf("== CPU speed: the interpreter's instruction budget ==\n");

    CHECK(cpuspeed_ips(0) == 0ul, "Unlimited sets no instruction budget at all");
    {   unsigned long ips = cpuspeed_ips(idx_of(33));
        CHECK(ips == 33000000ul / CPUSPEED_CPI, "33 MHz budgets 33e6/CPI instructions a second"); }

    /* A slice that ran FASTER than the budget owes time. 1,000,000 instructions at
       11M/s should take ~90.9 ms; if it really took 5 ms we owe ~86 ms of sleep. */
    {   cpuspeed_pace p; int ms;
        memset(&p, 0, sizeof p);
        ms = cpuspeed_charge(&p, 1000000ul, cpuspeed_ips(idx_of(33)), 5000ll);
        CHECK(ms >= 84 && ms <= 88, "a slice that outran its budget sleeps the difference"); }

    /* ⚠ THE SUB-MILLISECOND DEBT IS THE ONE THAT MATTERS. Small slices each owe a
         few hundred microseconds; discard that and the throttle is a no-op exactly
         where slices are smallest, which is the fast settings. Accumulated, ten
         slices owing 300us each must produce 3 ms of sleep. */
    {   cpuspeed_pace p; int k, ms, tot = 0;
        memset(&p, 0, sizeof p);
        for (k = 0; k < 10; ++k) {
            /* 3300 instructions at 11M/s = 300us of budget, executed in 0us. */
            ms = cpuspeed_charge(&p, 3300ul, cpuspeed_ips(idx_of(33)), 0ll);
            tot += ms;
        }
        CHECK(tot == 3, "ten 300us debts accumulate into 3 ms, rather than rounding to nothing"); }

    /* Running SLOWER than the setting must not bank credit: a guest that stalled for
       a second cannot then be handed a second of unthrottled burst. */
    {   cpuspeed_pace p; int ms;
        memset(&p, 0, sizeof p);
        ms = cpuspeed_charge(&p, 100ul, cpuspeed_ips(idx_of(33)), 500000ll);
        CHECK(ms == 0 && p.owed_us == 0, "a slice slower than its budget sleeps 0 and banks nothing");
        ms = cpuspeed_charge(&p, 1000000ul, cpuspeed_ips(idx_of(33)), 0ll);
        CHECK(ms >= 89 && ms <= 91, "...and the NEXT slice is throttled in full, not offset"); }

    /* One slice can never sleep more than the ceiling, however wild the arithmetic. */
    {   cpuspeed_pace p; int ms;
        memset(&p, 0, sizeof p);
        ms = cpuspeed_charge(&p, 100000000ul, cpuspeed_ips(idx_of(8)), 0ll);
        CHECK(ms <= 100, "one slice's debt is capped, so a stall cannot become a freeze"); }

    CHECK(cpuspeed_charge(&(cpuspeed_pace){0}, 1000000ul, 0ul, 0ll) == 0,
          "no budget means no sleep, whatever the slice did");

    printf("\n%s: %d/%d\n", fails ? "FAILURES" : "ALL PASS", total - fails, total);
    return fails ? 1 : 0;
}
