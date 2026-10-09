/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM battery for the CPU-speed throttle's arithmetic
 * (src/host/cpuspeed.h). GH #56.
 *
 * The throttle itself needs a suspended thread and a real guest, neither of which
 * exists on the build machine. What it is MADE OF is integer arithmetic -- a duty
 * cycle, a Bresenham that turns that duty into whole milliseconds, and an
 * instruction budget with a microsecond debt -- and every one of those has a
 * failure mode that would look, on the rig, exactly like "the setting does
 * nothing": a duty that rounds to zero, an accumulator that drifts, a debt smaller
 * than a millisecond that is discarded on every slice. Those are checked here.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "cpuspeed.h"
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

static INT g_Total = 0;
static INT g_Failures = 0;

/* [CAUTION]: FIND A SPEED BY ITS MHz, NEVER BY A HARD-CODED INDEX. The list is ordered and
 * extensible, so an index written into a test is a fact about today's table rather
 * than about the behaviour -- and when the table grew from 7 entries to 18 every
 * such test would have gone on passing while checking the wrong speed.
 */
static UINT CpuSpeedTestIndexOf(UINT mhz)
{
    UINT index;

    for (index = 0; index < CPUSPEED_COUNT; ++index)
        if (g_CpuSpeedMhz[index] == mhz)
            return index;

    return CPUSPEED_COUNT;   /* not found: an off-end index, never a silent 0 */
}

/* THE DETERMINISTIC HEART OF THE THROTTLE (Importance = 3):
 * CpuSpeedStep (cpuspeed.h) IS the whole control law, and here it is driven against
 * a SIMULATED clock -- no threads, no Sleep, no hardware -- so the result is
 * bit-for-bit repeatable. That is the point the user asked for: the rig could only
 * measure apparent MHz through the guest's own BIOS tick, which the throttle
 * starves, so the instrument was circular. This is not. It asserts the ONE contract
 * the throttle exists to keep -- over a window, delivered exec/wall equals the
 * requested duty, or the workload's own overhead ceiling if that is slower.
 *
 * A "slice" is one turn of the loop: the guest executes dE us over dWall us of wall
 * time (dWall >= dE models host/trap overhead), then it is held. The wall[] pattern
 * cycles, so a descheduled OUTLIER slice is exercised -- that outlier is exactly
 * what made the old open-loop debt carry come out non-monotonic on the rig.
 */
static UINT CpuSpeedTestSimulateDelivered(
    UINT dutyBp,
    unsigned long long capUs,
    const UINT *wallSlices,
    INT wallCount,
    UINT executedNumerator,
    UINT executedDenominator,
    unsigned long long totalUs)
{
    /* monotonic simulated totals */
    unsigned long long executedUs = 0;
    unsigned long long wallUs = 0;
    /* the window baseline CpuSpeedStep tracks */
    unsigned long long executedBaseline = 0;
    unsigned long long wallBaseline = 0;
    INT slice = 0;

    while (wallUs < totalUs)
    {
        unsigned long long sliceWallUs = wallSlices[slice % wallCount];
        unsigned long long sliceExecutedUs = sliceWallUs * executedNumerator / executedDenominator;
        INT reset;
        unsigned long long hold;
        ++slice;
        executedUs += sliceExecutedUs;
        wallUs += sliceWallUs;               /* the run slice */
        hold = CpuSpeedStep(executedUs - executedBaseline, wallUs - wallBaseline, dutyBp, capUs, &reset);
        wallUs += hold;                         /* the guest is held */

        if (reset) /* rebaseline at the real post-hold clock */
        {
            executedBaseline = executedUs;
            wallBaseline = wallUs;
        }
    }

    return CpuSpeedDeliveredBp(executedUs, wallUs);     /* the same ratio the host reports */
}

/* Within `tol` basis points of `want`. */
static INT CpuSpeedTestIsNear(UINT actual, UINT expected, UINT tolerance)
{
    return (actual >= expected ? actual - expected : expected - actual) <= tolerance;
}

INT main(VOID)
{
    UINT index;

    printf("== CPU speed: the table (cpuspeed.h) ==\n");

    /* [CAUTION]: INDEX 0 MUST BE UNLIMITED. This list replaced the dead SpeedMode combo,
     * whose index 0 also meant "do nothing", so every value already sitting in
     * somebody's registry keeps its meaning across the upgrade. If this ever
     * fails, an existing installation silently acquires a throttle.
     */
    CHECK(g_CpuSpeedMhz[0] == 0, "index 0 is Unlimited, so old SpeedMode=0 still means no throttle");
    CHECK(strcmp(g_CpuSpeedNames[0], "Host (Unlimited)") == 0, "...and it says so (#224)");
    CHECK(CpuSpeedIsAvailable(0, 500), "#224: Host is always available");
    CHECK(!CpuSpeedIsAvailable(2, 500) && CpuSpeedIsAvailable(3, 500),
          "#224: a 500 MHz host greys Pentium III 600 and offers Pentium II 300");
    CHECK(CpuSpeedIsAvailable(1, 0), "#224: an unknown host speed offers everything");

    /* Fastest first: the old "Maximum" (1) lands on the fastest throttled speed. */
    {   INT isDescending = 1;

        for (index = 2; index < CPUSPEED_COUNT; ++index)
            if (g_CpuSpeedMhz[index] >= g_CpuSpeedMhz[index - 1])
                isDescending = 0;

        CHECK(isDescending, "speeds run fastest-first, which is what makes the migration sane"); }

    /* The '|' string and the name table are two spellings of one list. They are
     * adjacent in the header precisely so they cannot drift, and this is the check
     * that says they have not: a mismatch would put the wrong label on every row of
     * the dropdown while every value behind it stayed correct.
     */
    {   PCSTR item = CPUSPEED_ITEMS;
    INT count = 0;
    INT isOk = 1;

        for (index = 0; index < CPUSPEED_COUNT; ++index)
        {
            size_t length = strlen(g_CpuSpeedNames[index]);

            if (strncmp(item, g_CpuSpeedNames[index], length) != 0)
            {
                isOk = 0;
                break;
            }

            item += length;
            ++count;

            if (index + 1 < CPUSPEED_COUNT)
            {
                if (*item != '|')
                {
                    isOk = 0;
                    break;
                }

                ++item;
            }
        }

        CHECK(isOk && count == CPUSPEED_COUNT && *item == 0,
              "CPUSPEED_ITEMS spells out exactly CPUSPEED_NAMES, in order"); }

    printf("== CPU speed: the duty cycle ==\n");

    CHECK(CpuSpeedDutyBp(0, 700) == 10000, "Unlimited runs flat out");
    CHECK(CpuSpeedDutyBp(99, 700) == 10000, "an index off the end runs flat out, not at zero");
    CHECK(CpuSpeedDutyBp(CpuSpeedTestIndexOf(33), 0) == 10000, "a zero reference cannot throttle (never divide by it)");

    /* #225: a rung's share is its real machine's Doom rate over this host's ceiling
     * (66.4 fps per 1000 ref-MHz). The rig (ref 3704 -> 246 fps): 486DX2-66 = 33.1 fps
     * -> 13.5%; measured 1.8% before this, which could not finish demo3.
     */
    {   UINT duty = CpuSpeedDutyBp(CpuSpeedTestIndexOf(66), 3704);
        CHECK(duty >= 1210 && duty <= 1240, "#225: 486DX2-66 on the rig is ~12.2% (was 1.8%)"); }
    {   UINT duty = CpuSpeedDutyBp(CpuSpeedTestIndexOf(133), 3704);
        CHECK(duty >= 2950 && duty <= 2980, "#225: Pentium 133 on the rig is ~29.7%"); }
    CHECK(CpuSpeedDutyBp(CpuSpeedTestIndexOf(600), 700) == 10000,
          "#225: a rung above a slow host's Doom ceiling (46 fps at ref 700) runs flat out");

    /* [CAUTION]: MONOTONIC ONLY BELOW THE REFERENCE, and the exception is the point. Every
     * speed at or above the host's own clamps to flat out, so on a 50 MHz box the
     * 100 and 66 MHz entries are TIED at 10000 -- correct (they are ceilings, not
     * boosts) and would fail a naive "strictly decreasing" check the moment the
     * list runs past the reference. (The reference here is 50 rather than 700
     * because the session-60 ladder tops out at 100 MHz.)
     */
    {   INT isMonotonic = 1, flatOutCount = 0;

        for (index = 2; index < CPUSPEED_COUNT; ++index)
        {
            UINT faster = CpuSpeedDutyBp(index - 1, 50);
            UINT slower = CpuSpeedDutyBp(index, 50);

            if (g_CpuSpeedMhz[index - 1] >= 50u)
            {
                if (faster != 10000u)
                    isMonotonic = 0;

                flatOutCount++;
                continue;
            }

            if (slower >= faster)
                isMonotonic = 0;
        }

        CHECK(isMonotonic, "below the reference a slower setting is always a smaller duty");
        CHECK(flatOutCount > 0, "...and the speeds above it are all flat out, which is why"); }

    /* [CAUTION]: THE ROUND-TO-ZERO TRAP. On a very fast host the slowest setting divides to
     * under half a basis point. Truncating that to 0 would HOLD THE GUEST FOREVER
     * -- a hang wearing a setting's clothes -- so it must clamp to 1.
     */
    CHECK(CpuSpeedDutyBp(CpuSpeedTestIndexOf(16), 4000000u) >= 1, "an absurd reference still leaves the guest some time");

    /* A target at or above the reference is a ceiling, not a boost: we cannot make
     * the host faster and must not pretend by handing back more than 100%.
     */
    CHECK(CpuSpeedDutyBp(CpuSpeedTestIndexOf(100), 66) == 10000,
          "a speed above the reference clamps to flat out -- a ceiling, never a boost");

    /* #225: a real-mode program's share is the protected-mode share x CPUSPEED_RM_PCT,
     * clamped to flat out, and Unlimited stays Unlimited.
     */
    {   UINT duty66 = CpuSpeedDutyBp(CpuSpeedTestIndexOf(66), CPUSPEED_REF_MHZ_DEFAULT);
        CHECK(CpuSpeedRealModeDutyBp(duty66) == duty66 * CPUSPEED_RM_PCT / 100u,
              "#225: real mode at 486DX2-66 gets the protected-mode share x CPUSPEED_RM_PCT");
        CHECK(CpuSpeedRealModeDutyBp(10000u) == 10000u, "#225: Unlimited is Unlimited in real mode too");
        CHECK(CpuSpeedRealModeDutyBp(9000u) == 10000u, "#225: a scaled share past 100% clamps to flat out");
        {   INT isMonotonic = 1;

            for (index = 2; index < CPUSPEED_COUNT; ++index)
                if (CpuSpeedRealModeDutyBp(CpuSpeedDutyBp(index, CPUSPEED_REF_MHZ_DEFAULT)) >
                    CpuSpeedRealModeDutyBp(CpuSpeedDutyBp(index - 1, CPUSPEED_REF_MHZ_DEFAULT)))
                    isMonotonic = 0;

            CHECK(isMonotonic, "#225: the real-mode ladder still gets slower rung by rung"); } }

    printf("== CPU speed: the throttle delivers the requested duty (deterministic) ==\n");

    /* The cap on one hold, in microseconds (CPUSPEED_MAX_OFF_MS = 1000 ms). Below
     * this, a setting is reachable and the invariant delivers it exactly; above it,
     * the setting saturates and delivers honestly-faster-than-asked.
     */
    {   const unsigned long long capUs = (unsigned long long)CPUSPEED_MAX_OFF_MS * 1000ull;
        const UINT steadySlices[]  = { 2000 };                       /* a plain 2 ms slice */
        const UINT jitterSlices[]  = { 2000,2000,2000,19000,2000 };  /* a descheduled outlier */
        const UINT fineSlices[]    = { 100 };                        /* the smooth end */
        const unsigned long long runUs = 20000000ull;                /* 20 s of simulated run */

        /* 1. PURE COMPUTE, STEADY SLICES: DELIVERED == REQUESTED, EXACTLY:
         * This is the claim the rig kept failing to prove. With no trap overhead
         * each window pays to exactly E/duty, so the ratio is the duty to the bp.
         * Every duty on the trimmed ladder, against the shipped reference.
         */
        {   INT isOk = 1;
        UINT badIndex = 0;

            for (index = 1; index < CPUSPEED_COUNT; ++index)
            {
                UINT duty   = CpuSpeedDutyBp(index, CPUSPEED_REF_MHZ_DEFAULT);
                UINT actual = CpuSpeedTestSimulateDelivered(duty, capUs, steadySlices, 1, 1, 1, runUs);
                /* Reachable at 2 ms slices iff one slice's hold fits the cap. */
                if ((unsigned long long)2000 * 10000ull / duty - 2000ull > capUs)
                    continue;

                if (!CpuSpeedTestIsNear(actual, duty, 2))
                {
                    isOk = 0;
                    badIndex = index;
                }
            }

            CHECK(isOk, "every reachable ladder speed is delivered to within 2 bp of its duty");
            (VOID)badIndex; }

        /* -- 2. THE OUTLIER, ABSORBED. A 19 ms descheduled slice among 2 ms ones was
         * what made the debt carry non-monotonic. The closed loop prices each slice
         * against the running total, so the average is still the duty exactly.
         */
        {   UINT duty = CpuSpeedDutyBp(CpuSpeedTestIndexOf(33), CPUSPEED_REF_MHZ_DEFAULT);
            UINT smooth = CpuSpeedTestSimulateDelivered(duty, capUs, steadySlices, 1, 1, 1, runUs);
            UINT rough  = CpuSpeedTestSimulateDelivered(duty, capUs, jitterSlices, 5, 1, 1, runUs);
            CHECK(CpuSpeedTestIsNear(rough, duty, 3), "a descheduled 19 ms outlier does not move the delivered speed");
            CHECK(CpuSpeedTestIsNear(rough, smooth, 3), "...it is within 3 bp of the jitter-free run"); }

        /* -- 3. THE PORT-TRAP CEILING, WHICH IS PHYSICS, NOT A BUG. A workload that
         * only executes 40% of its wall time (the rest trapped in the host) cannot
         * be sped past 40% however high the target; a lower target is still hit.
         */
        {   UINT nearForty = CpuSpeedTestSimulateDelivered(5000, capUs, steadySlices, 1, 2, 5, runUs);   /* want 50% */
            UINT atTwenty   = CpuSpeedTestSimulateDelivered(2000, capUs, steadySlices, 1, 2, 5, runUs);   /* want 20% */
            CHECK(CpuSpeedTestIsNear(nearForty, 4000, 20), "a 40%-overhead workload tops out near 40%, not the 50% asked");
            CHECK(CpuSpeedTestIsNear(atTwenty, 2000, 5),    "...but a target below that ceiling is delivered exactly"); }

        /* -- 4. SATURATION IS HONEST, AND GRANULARITY IS ITS CURE. A duty low enough
         * that one 2 ms slice needs a hold past the 1 s cap CANNOT be reached at
         * coarse slices -- it saturates and delivers FASTER than asked -- but the
         * SAME duty at fine (100 us) slices fits the cap and lands exactly. This is
         * the whole "smoothness" story, deterministically, and it is pinned to a
         * fixed low duty (10 bp) rather than a ladder label so it does not depend on
         * the reference: 2 ms / 0.001 = 2 s hold >> the 1 s cap.
         */
        {   UINT duty      = 10u;                                    /* 0.10% */
            UINT coarse = CpuSpeedTestSimulateDelivered(duty, capUs, steadySlices, 1, 1, 1, runUs);
            UINT smooth = CpuSpeedTestSimulateDelivered(duty, capUs, fineSlices,   1, 1, 1, runUs);
            CHECK(coarse > duty && !CpuSpeedTestIsNear(coarse, duty, 3),
                  "0.1% at coarse slices saturates -- delivered faster than asked, in the open");
            CHECK(CpuSpeedTestIsNear(smooth, duty, 2), "...and at fine slices it fits the cap and is delivered exactly"); }

        /* -- 5. A HOLD IS BOUNDED, so an absurd setting cannot freeze the guest: even
         * at a 1 bp duty the single hold never exceeds the cap.
         */
        {   INT reset;
        unsigned long long hold = CpuSpeedStep(2000, 0, 1, capUs, &reset);
            CHECK(hold <= capUs, "one hold is capped, so an unreachable setting slows but never freezes");
            CHECK(!reset, "...and a capped hold is carried, not forgiven"); }

        /* -- 6. UNLIMITED NEVER HOLDS, whatever the totals. */
        {   INT reset;
            CHECK(CpuSpeedStep(999999, 0, 10000, capUs, &reset) == 0 && reset,
                  "Unlimited holds for nothing and keeps no window"); }
        (VOID)jitterSlices;
        (VOID)fineSlices;
    }

    printf("== CPU speed: the interpreter's instruction budget ==\n");

    CHECK(CpuSpeedInstructionsPerSecond(0) == 0ul, "Unlimited sets no instruction budget at all");
    {   unsigned long instructionsPerSecond = CpuSpeedInstructionsPerSecond(CpuSpeedTestIndexOf(33));
        CHECK(instructionsPerSecond == 33000000ul / CPUSPEED_CPI, "33 MHz budgets 33e6/CPI instructions a second"); }

    /* A slice that ran FASTER than the budget owes time. 1,000,000 instructions at
     * 11M/s should take ~90.9 ms; if it really took 5 ms we owe ~86 ms of sleep.
     */
    {   CPUSPEED_PACE pace;
    INT sleepMs;
        memset(&pace, 0, sizeof pace);
        sleepMs = CpuSpeedCharge(&pace, 1000000ul, CpuSpeedInstructionsPerSecond(CpuSpeedTestIndexOf(33)), 5000ll);
        CHECK(sleepMs >= 84 && sleepMs <= 88, "a slice that outran its budget sleeps the difference"); }

    /* [CAUTION]: THE SUB-MILLISECOND DEBT IS THE ONE THAT MATTERS. Small slices each owe a
     * few hundred microseconds; discard that and the throttle is a no-op exactly
     * where slices are smallest, which is the fast settings. Accumulated, ten
     * slices owing 300us each must produce 3 ms of sleep.
     */
    {   CPUSPEED_PACE pace;
    INT slice;
    INT sleepMs;
    INT totalMs = 0;
        memset(&pace, 0, sizeof pace);

        for (slice = 0; slice < 10; ++slice)
        {
            /* 3300 instructions at 11M/s = 300us of budget, executed in 0us. */
            sleepMs = CpuSpeedCharge(&pace, 3300ul, CpuSpeedInstructionsPerSecond(CpuSpeedTestIndexOf(33)), 0ll);
            totalMs += sleepMs;
        }

        CHECK(totalMs == 3, "ten 300us debts accumulate into 3 ms, rather than rounding to nothing"); }

    /* Running SLOWER than the setting must not bank credit: a guest that stalled for
     * a second cannot then be handed a second of unthrottled burst.
     */
    {   CPUSPEED_PACE pace;
    INT sleepMs;
        memset(&pace, 0, sizeof pace);
        sleepMs = CpuSpeedCharge(&pace, 100ul, CpuSpeedInstructionsPerSecond(CpuSpeedTestIndexOf(33)), 500000ll);
        CHECK(sleepMs == 0 && pace.OwedUs == 0, "a slice slower than its budget sleeps 0 and banks nothing");
        sleepMs = CpuSpeedCharge(&pace, 1000000ul, CpuSpeedInstructionsPerSecond(CpuSpeedTestIndexOf(33)), 0ll);
        CHECK(sleepMs >= 89 && sleepMs <= 91, "...and the NEXT slice is throttled in full, not offset"); }

    /* One slice can never sleep more than the ceiling, however wild the arithmetic. */
    {   CPUSPEED_PACE pace;
    INT sleepMs;
        memset(&pace, 0, sizeof pace);
        sleepMs = CpuSpeedCharge(&pace, 100000000ul, CpuSpeedInstructionsPerSecond(CpuSpeedTestIndexOf(16)), 0ll);
        CHECK(sleepMs <= 100, "one slice's debt is capped, so a stall cannot become a freeze"); }

    CHECK(CpuSpeedCharge(&(CPUSPEED_PACE){0}, 1000000ul, 0ul, 0ll) == 0,
          "no budget means no sleep, whatever the slice did");

    printf("\n%s: %d/%d\n", g_Failures ? "FAILURES" : "ALL PASS", g_Total - g_Failures, g_Total);
    return g_Failures ? 1 : 0;
}
