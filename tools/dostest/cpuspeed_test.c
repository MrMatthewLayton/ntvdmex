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
    return 0u;
}

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

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
         speed at or above the host's own clamps to flat out, so on a 700 MHz box
         1000/2000/3300 MHz are all TIED at 10000 -- which is correct (they are
         ceilings, not boosts) and would have failed a naive "strictly decreasing"
         check the moment the list grew past the reference. */
    {   int mono = 1, ties = 0;
        for (i = 2; i < CPUSPEED_COUNT; ++i) {
            unsigned a = cpuspeed_duty_bp(i - 1, 700), b = cpuspeed_duty_bp(i, 700);
            if (CPUSPEED_MHZ[i - 1] >= 700u) { if (a != 10000u) mono = 0; ties++; continue; }
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
    CHECK(cpuspeed_duty_bp(idx_of(3300), 100) == 10000,
          "a speed above the reference clamps to flat out -- a ceiling, never a boost");

    printf("== CPU speed: the hold, priced off a MEASURED run phase ==\n");

    /* ⚠⚠ THE REGRESSION THIS SECTION EXISTS FOR, AND IT TOOK THREE CUTS ON REAL
         HARDWARE. Pricing the hold off the 1 ms we ASK for came out 3.5x too fast
         at EVERY setting -- 56 MHz where 16 was requested, 740 where 200 was. The
         run phase is not 1 ms: it is 1 ms plus Sleep's inaccuracy plus whatever it
         costs the kernel to stop a thread inside VdmStartExecution. So the hold is
         computed from the run phase actually MEASURED, and these checks are in the
         units that measurement produces -- microseconds, not assumptions. */
    CHECK(cpuspeed_hold_us(10000, 3500) == 0, "Unlimited asks for no hold at all");
    CHECK(cpuspeed_hold_us(500, 0) == 0, "a run of zero cannot be priced, and says so");

    /* 5% duty: for every 1 unit of running, 19 of held. A 3.5 ms measured run
       therefore owes 66.5 ms. */
    CHECK(cpuspeed_hold_us(500, 3500) == 66500ul, "a 5% duty owes 19x the measured run");
    CHECK(cpuspeed_hold_us(5000, 3500) == 3500ul, "a 50% duty owes as long as it ran");

    /* THE POINT: the same duty against a LONGER measured run must owe more. A
       constant would not do this, and a constant is what was 3.5x wrong. */
    CHECK(cpuspeed_hold_us(500, 7000) > cpuspeed_hold_us(500, 3500),
          "a run phase that turned out longer owes proportionally more");

    /* ── THE DEBT CARRY. One long run must not buy the guest free execution. */
    /* ⚠ `total` IS THE HARNESS'S OWN COUNTER. Declaring one in these blocks
         SHADOWED it, so CHECK counted into the local and the assertions compared
         against a number the harness had been incrementing -- one case reported a
         failure it did not have and its neighbour passed by luck. Name them
         something else. */
    {   unsigned long owed = 0ul; unsigned paid_ms = 0; int k;
        /* 8 MHz-ish: every 2 ms run owes ~900 ms, which fits under the cap. */
        for (k = 0; k < 5; ++k) { owed += cpuspeed_hold_us(22, 2000);
                                  paid_ms += cpuspeed_pay_ms(&owed); }
        CHECK(paid_ms >= 4400 && paid_ms <= 4600,
              "five 2 ms runs at a 0.22% duty are paid for in full, ~907 ms each");
        CHECK(owed < 1000ul, "...and almost nothing is left owing"); }

    {   unsigned long owed = 0ul; unsigned paid_ms = 0, k;
        /* An OUTLIER run: 19 ms at the same duty owes 8.6 s, past the arrears cap.
           What CAN be carried must be paid off across later periods, not thrown
           away -- throwing it away is what made the rig sweep non-monotonic. */
        owed += cpuspeed_hold_us(22, 19000);
        for (k = 0; k < 12; ++k) paid_ms += cpuspeed_pay_ms(&owed);
        CHECK(paid_ms == 4000,
              "an outlier run's debt is paid down over later holds, to the arrears cap");
        CHECK(owed == 0ul, "...and the carried debt is fully discharged, not left to grow"); }

    {   unsigned long owed = 10000000ul;   /* ten seconds of arrears */
        cpuspeed_pay_ms(&owed);
        CHECK(owed <= CPUSPEED_MAX_OWED_US,
              "arrears are bounded, so an unreachable setting cannot freeze the guest forever"); }

    {   unsigned long owed = 400ul;        /* less than a millisecond owed */
        CHECK(cpuspeed_pay_ms(&owed) == 0, "a sub-millisecond debt sleeps 0 rather than rounding up");
        CHECK(owed == 400ul, "...and stays owed, so it is not lost either"); }

    /* Delivered speed across the table, against the rig's measured reference and a
       realistic 3.5 ms run phase. ⚠ 8 MHz is EXCLUDED and the next check says why. */
    {   int ok = 1;
        /* Only the speeds the hold cap can actually reach; the next check is the
           one that pins where that boundary is. */
        for (i = 1; i < CPUSPEED_COUNT; ++i) {
            unsigned want = CPUSPEED_MHZ[i];
            unsigned got  = cpuspeed_delivered_mhz(i, 3661, 3500);
            if (want < 16u) continue;
            if (got + want / 10u < want || got > want + want / 10u) ok = 0;
        }
        CHECK(ok, "every speed from 16 MHz up is delivered within 10% at a 3.5 ms run phase"); }

    /* ⚠ AND THE ONE THAT IS NOT. 8 MHz against 3661 with a 3.5 ms run phase wants a
         1.6 second hold, which is not a speed setting, it is a machine that has
         stopped answering. It clamps -- and the host LOGS delivered beside
         requested, so the shortfall is visible rather than being a menu entry that
         quietly means something else. */
    {   unsigned got = cpuspeed_delivered_mhz(idx_of(8), 3661, 3500);
        CHECK(got > CPUSPEED_MHZ[idx_of(8)],
              "8 MHz cannot be delivered against a 3661 MHz reference -- it clamps");
        CHECK(got < 20u, "...but it still lands near it, not somewhere unrelated"); }

    CHECK(cpuspeed_delivered_mhz(0, 3661, 3500) == 0,
          "Unlimited delivers 'unlimited', not a number");

    /* ⚠ THE HOLD IS CAPPED, and the cap is what stops an absurd duty becoming a
         freeze the user's only answer to is the power button. */
    {   unsigned long owed = cpuspeed_hold_us(1, 3500);
        CHECK(cpuspeed_pay_ms(&owed) == CPUSPEED_MAX_OFF_MS,
              "an absurd duty pays the cap, not a multi-second freeze"); }

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
