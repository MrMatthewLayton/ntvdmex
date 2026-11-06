/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The TSC clock (src/host/tsc_clock.h), driven with two simulated dies. (#416)
 *
 * The skews are the ones measured on the Q9650 (runs/s104/tsc): 14 ms at 880 s of uptime, up to
 * 240 ms at 83 min. The reference is either fine (a PM-timer QPC) or a 1 ms tick.
 *
 *   cc -std=c99 -I src/host -o tsc_clock_test tests/unit/tsc_clock_test.c src/host/tsc_clock.c
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include "tsc_clock.h"

#define CHECK(condition, name) \
    do \
    { \
        if (!(condition)) \
        { \
            printf("FAIL: %s\n", name); \
            g_Failures++; \
        } \
    } while (0)

typedef struct
{
    UINT64 EndMicroseconds;
    UINT64 WorstBehind;
    UINT64 WorstAhead;
    UINT64 LongestFreeze;
    UINT64 LongestForward;
} RUN_RESULT;

static INT g_Failures;

/* One thread reading the clock every microsecond for runMicroseconds of true time, 3000 ticks per
 * microsecond, hopping to the other die every hopMicroseconds (0 = never).
 */
static RUN_RESULT RunClock(
    UINT64 skewMicroseconds,
    UINT64 hopMicroseconds,
    UINT64 referenceTick,
    UINT64 runMicroseconds,
    UINT64 hopPhase)
{
    TSC_CLOCK clock = { 0 };
    RUN_RESULT result = { 0 };
    UINT64 previous = 0;
    UINT64 frozenFor = 0;
    UINT64 trueTime;
    INT die = 0;

    for (trueTime = 1000; trueTime <= runMicroseconds; trueTime++)
    {
        UINT64 tsc = (1ull << 40) + trueTime * 3000 + (die ? skewMicroseconds * 3000 : 0);
        UINT64 value;

        if (hopMicroseconds && trueTime % hopMicroseconds == hopPhase)
            die ^= 1;

        if (TscClockNeedsAnchor(&clock, tsc))
            value = TscClockAnchor(&clock, tsc, trueTime - trueTime % referenceTick);
        else
            value = TscClockRead(&clock, tsc);

        if (trueTime > 200000)   /* The first rate takes two 64 ms windows */
        {
            UINT64 step = value - previous;

            frozenFor = step ? 0 : frozenFor + 1;
            result.LongestFreeze = frozenFor > result.LongestFreeze ? frozenFor : result.LongestFreeze;
            result.LongestForward = step > result.LongestForward ? step : result.LongestForward;

            if (value < trueTime && trueTime - value > result.WorstBehind)
                result.WorstBehind = trueTime - value;

            if (value > trueTime && value - trueTime > result.WorstAhead)
                result.WorstAhead = value - trueTime;
        }

        previous = value;
    }

    result.EndMicroseconds = previous;
    return result;
}

static VOID TestSteadyCpu(VOID)
{
    RUN_RESULT result = RunClock(0, 0, 1, 2000000, 0);

    CHECK(result.WorstBehind <= 10, "one die, fine reference: tracks true time");
    CHECK(result.WorstAhead <= 3, "one die, fine reference: does not run ahead");
    CHECK(result.LongestFreeze <= 2, "one die, fine reference: never freezes");
}

static VOID TestCoarseReference(VOID)
{
    RUN_RESULT result = RunClock(0, 0, 1000, 2000000, 0);

    CHECK(result.WorstBehind <= 1100, "one die, 1 ms reference: within a tick behind");
    CHECK(result.WorstAhead <= 100, "one die, 1 ms reference: not ahead");
    CHECK(result.LongestForward <= 130, "one die, 1 ms reference: no step shakes the beam");
    CHECK(result.LongestFreeze <= 130, "one die, 1 ms reference: no freeze shakes the beam");
}

/* The original clock froze for 29.8 s of a 30 s run here: a hop back just after an anchor wrapped
 * tsc - base and the never-backwards clamp held the result (runs/s105/oldclock_sim.c).
 */
static VOID TestTwoDies(VOID)
{
    static const UINT64 skews[] = { 14000, 34000, 240000 };
    static const UINT64 hops[] = { 5000, 20000, 100000 };
    INT skewIndex;
    INT hopIndex;
    UINT64 phase;

    for (skewIndex = 0; skewIndex < 3; skewIndex++)
    {
        for (hopIndex = 0; hopIndex < 3; hopIndex++)
        {
            for (phase = 0; phase < 3000; phase += 251)
            {
                RUN_RESULT result = RunClock(skews[skewIndex], hops[hopIndex], 1, 3000000, phase);

                CHECK(result.EndMicroseconds <= 3000000 + 2100 && result.EndMicroseconds >= 3000000 - 2100,
                      "two dies: ends on true time");
                CHECK(result.LongestFreeze <= 2100, "two dies: a hop freezes under one resync interval");
                CHECK(result.WorstAhead <= 2100, "two dies: never far ahead");
                CHECK(result.WorstBehind <= 2100, "two dies: never far behind");
            }
        }
    }
}

static VOID TestTwoDiesCoarseReference(VOID)
{
    RUN_RESULT result = RunClock(240000, 20000, 1000, 3000000, 7);

    CHECK(result.WorstAhead <= 3200, "two dies, 1 ms reference: never far ahead");
    CHECK(result.WorstBehind <= 3200, "two dies, 1 ms reference: never far behind");
    CHECK(result.LongestFreeze <= 3200, "two dies, 1 ms reference: no long freeze");
}

/* A TSC that stopped while the thread slept: ticks say little, the reference says a lot. */
static VOID TestStoppedInIdle(VOID)
{
    TSC_CLOCK clock = { 0 };
    UINT64 tsc = 1ull << 40;
    UINT64 reference = 1000;
    UINT32 rate;
    INT sleepIndex;

    while (reference < 200000)
    {
        if (TscClockNeedsAnchor(&clock, tsc))
            TscClockAnchor(&clock, tsc, reference);

        tsc += 3000;
        reference++;
    }

    rate = clock.Multiplier;
    CHECK(rate > 0, "idle: a rate was learned");

    for (sleepIndex = 0; sleepIndex < 20; sleepIndex++)
    {
        UINT64 woke = 0;
        INT readIndex;

        reference += 1000000;
        tsc += 3000 * 269000ull;

        for (readIndex = 0; readIndex < 3000; readIndex++)
        {
            woke = TscClockNeedsAnchor(&clock, tsc) ? TscClockAnchor(&clock, tsc, reference)
                                                    : TscClockRead(&clock, tsc);
            tsc += 3000;
            reference++;
        }

        CHECK(woke + 2100 >= reference && woke <= reference + 2100, "idle: back on time within an anchor");
    }

    CHECK(clock.Multiplier >= rate - rate / 16 && clock.Multiplier <= rate + rate / 16,
          "idle: sleeping does not change the rate");
}

/* A CPU that truly changes speed: the old rate is wrong for good, so the new one must be taken. */
static VOID TestRealRateChange(VOID)
{
    TSC_CLOCK clock = { 0 };
    UINT64 tsc = 1ull << 40;
    UINT64 reference = 1000;
    UINT64 value = 0;

    while (reference < 200000)
    {
        value = TscClockNeedsAnchor(&clock, tsc) ? TscClockAnchor(&clock, tsc, reference)
                                                 : TscClockRead(&clock, tsc);
        tsc += 3000;
        reference++;
    }

    while (reference < 700000)
    {
        value = TscClockNeedsAnchor(&clock, tsc) ? TscClockAnchor(&clock, tsc, reference)
                                                 : TscClockRead(&clock, tsc);
        tsc += 2000;
        reference++;
    }

    CHECK(clock.Multiplier > 2000000u, "a real speed change is taken");
    CHECK(value + 2100 >= reference && value <= reference + 2100, "a real speed change keeps time");
}

static VOID TestFirstRateIsNotARaceWithTheWindow(VOID)
{
    TSC_CLOCK clock = { 0 };
    UINT64 reference;
    UINT64 tsc = 1ull << 40;

    for (reference = 1000; reference < 200000; reference++)
    {
        if (TscClockNeedsAnchor(&clock, tsc))
            TscClockAnchor(&clock, tsc, reference);

        tsc += 3000;
    }

    CHECK(clock.Multiplier != 0, "a flat-out reader still gets a rate");
}

INT main(VOID)
{
    TestSteadyCpu();
    TestCoarseReference();
    TestTwoDies();
    TestTwoDiesCoarseReference();
    TestStoppedInIdle();
    TestRealRateChange();
    TestFirstRateIsNotARaceWithTheWindow();

    if (g_Failures)
    {
        printf("tsc_clock_test: %d FAILED\n", g_Failures);
        return 1;
    }

    printf("tsc_clock_test: all passed\n");
    return 0;
}
