/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The clock's arithmetic: the function definitions of tsc_clock.h, which holds the reasoning. (#416)
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "tsc_clock.h"

#define TSC_CLOCK_FIXED_SHIFT       32
#define TSC_CLOCK_WINDOW_MAX_US     0x7FFFFFFFull   /* Past this the 32.32 numerator overflows */
#define TSC_CLOCK_MULTIPLIER_MAX    0xFFFFFFFFull
#define TSC_CLOCK_BUSY_GAP_US       8000u           /* Anchors further apart than this: it slept */

static BOOL TscClockRateIsNear(UINT32 rate, UINT32 sample)
{
    UINT32 difference = rate > sample ? rate - sample : sample - rate;

    return difference <= (rate >> TSC_CLOCK_RATE_NEAR_SHIFT);
}

static VOID TscClockSetRate(TSC_CLOCK *clock, UINT32 multiplier)
{
    clock->Multiplier = multiplier;
    clock->PendingMultiplier = 0;
    clock->Resync = ((UINT64)TSC_CLOCK_RESYNC_US << TSC_CLOCK_FIXED_SHIFT) / multiplier;
}

static VOID TscClockOpenRateWindow(TSC_CLOCK *clock, UINT64 tsc, UINT64 referenceMicroseconds)
{
    clock->RateTscBase = tsc;
    clock->RateMicrosecondsBase = referenceMicroseconds;
}

/* Close the rate window if it is long enough, and open the next. Every sample, the first included,
 * passes the same test: near the current rate, or seen twice. A CPU that really changed its clock
 * repeats itself; a jump inside the window does not, because a hop to the other die and the hop
 * back land in different windows with opposite signs.
 */
static VOID TscClockSampleRate(TSC_CLOCK *clock, UINT64 tsc, UINT64 referenceMicroseconds)
{
    UINT64 ticks;
    UINT64 microseconds;
    UINT64 sample;
    UINT32 multiplier;

    if (tsc <= clock->RateTscBase || referenceMicroseconds < clock->RateMicrosecondsBase)
    {
        /* A window that ran backwards is a jump, so whatever waited to be seen twice was one too. */
        clock->PendingMultiplier = 0;
        TscClockOpenRateWindow(clock, tsc, referenceMicroseconds);
        return;
    }

    ticks = tsc - clock->RateTscBase;
    microseconds = referenceMicroseconds - clock->RateMicrosecondsBase;

    if (microseconds < TSC_CLOCK_RATE_WINDOW_US)
        return;

    TscClockOpenRateWindow(clock, tsc, referenceMicroseconds);

    if (microseconds > TSC_CLOCK_WINDOW_MAX_US)
        return;

    sample = (microseconds << TSC_CLOCK_FIXED_SHIFT) / ticks;

    if (!sample || sample > TSC_CLOCK_MULTIPLIER_MAX)
        return;

    multiplier = (UINT32)sample;

    if ((clock->Multiplier && TscClockRateIsNear(clock->Multiplier, multiplier))
        || (clock->PendingMultiplier && TscClockRateIsNear(clock->PendingMultiplier, multiplier)))
        TscClockSetRate(clock, multiplier);
    else
        clock->PendingMultiplier = multiplier;
}

BOOL TscClockNeedsAnchor(const TSC_CLOCK *clock, UINT64 tsc)
{
    return !clock->Multiplier || tsc < clock->TscBase || tsc - clock->TscBase >= clock->Resync;
}

UINT64 TscClockAnchor(TSC_CLOCK *clock, UINT64 tsc, UINT64 referenceMicroseconds)
{
    if (!clock->HasBase
        || referenceMicroseconds - clock->AnchorReference > TSC_CLOCK_BUSY_GAP_US)
    {
        /* First read, or the thread slept: the TSC may have stopped, so no rate can come of this. */
        clock->HasBase = TRUE;
        TscClockOpenRateWindow(clock, tsc, referenceMicroseconds);
    }
    else
    {
        TscClockSampleRate(clock, tsc, referenceMicroseconds);
    }

    clock->AnchorReference = referenceMicroseconds;

    if (clock->Multiplier && tsc >= clock->TscBase)
    {
        UINT64 predicted = clock->MicrosecondsBase
            + (((tsc - clock->TscBase) * (UINT64)clock->Multiplier) >> TSC_CLOCK_FIXED_SHIFT);
        INT64 error = (INT64)(referenceMicroseconds - predicted);

        if (error >= -TSC_CLOCK_STEP_US && error <= TSC_CLOCK_STEP_US)
            referenceMicroseconds = (UINT64)((INT64)predicted + error / (1 << TSC_CLOCK_SLEW_SHIFT));
    }

    clock->TscBase = tsc;
    clock->MicrosecondsBase = referenceMicroseconds;

    if (referenceMicroseconds > clock->Last)
        clock->Last = referenceMicroseconds;

    return clock->Last;
}

UINT64 TscClockRead(TSC_CLOCK *clock, UINT64 tsc)
{
    UINT64 microseconds = clock->MicrosecondsBase
        + (((tsc - clock->TscBase) * (UINT64)clock->Multiplier) >> TSC_CLOCK_FIXED_SHIFT);

    if (microseconds > clock->Last)
        clock->Last = microseconds;

    return clock->Last;
}
