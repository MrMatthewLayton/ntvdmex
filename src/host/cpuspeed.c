/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The CPU-speed governor's arithmetic: the speed list, duty cycles, pacing and the instruction budget.
 *
 * The function definitions of cpuspeed.h, which keeps their declarations and doc comments (#335).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "cpuspeed.h"

const UINT g_CpuSpeedMhz[CPUSPEED_COUNT] = {
    0,      /*  0: Host (Unlimited) -- the default */
    1000,   /*  1: Intel Pentium III 1 GHz */
    600,    /*  2: Intel Pentium III 600 MHz */
    300,    /*  3: Intel Pentium II 300 MHz */
    200,    /*  4: Intel Pentium MMX 200 MHz */
    133,    /*  5: Intel Pentium 133 MHz */
    100,    /*  6: Intel 486DX4 100 MHz */
    66,     /*  7: Intel 486DX2 66 MHz */
    50,     /*  8: Intel 486DX 50 MHz */
    33,     /*  9: Intel 386DX 33 MHz */
    16      /* 10: Intel 386DX 16 MHz */
};

PCSTR const g_CpuSpeedNames[CPUSPEED_COUNT] = {
    "Host (Unlimited)", "Intel Pentium III 1 GHz", "Intel Pentium III 600 MHz",
    "Intel Pentium II 300 MHz", "Intel Pentium MMX 200 MHz", "Intel Pentium 133 MHz",
    "Intel 486DX4 100 MHz", "Intel 486DX2 66 MHz", "Intel 486DX 50 MHz",
    "Intel 386DX 33 MHz", "Intel 386DX 16 MHz"
};

static const UINT g_CpuSpeedDoomFps10[CPUSPEED_COUNT] = {
    0,      /*  0: Host -- unthrottled */
    2100,   /*  1: Pentium III 1 GHz   (TU Wien PIII-800 188-202; above any cap) */
    1900,   /*  2: Pentium III 600     (TU Wien PIII-500 183-191) */
    1150,   /*  3: Pentium II 300      (between PII-233 105.4 and PII-350 122.4) */
    976,    /*  4: Pentium MMX 200     (thandor 97.63) */
    802,    /*  5: Pentium 133         (thandor 80.23) */
    438,    /*  6: 486DX4 100          (thandor 43.75) */
    331,    /*  7: 486DX2 66           (thandor 33.12) */
    280,    /*  8: 486DX 50            (thandor 28.00) */
    75,     /*  9: 386DX 33            (thandor Am386DX-33 7.47) */
    29      /* 10: 386DX 16            (386DX-25 4.58 scaled by clock, 16/25) */
};

INT CpuSpeedIsAvailable(UINT index, UINT hostMhz)
{
    if (index == 0u || index >= CPUSPEED_COUNT)
        return index == 0u;
    return hostMhz == 0u || g_CpuSpeedMhz[index] < hostMhz;
}

UINT CpuSpeedDutyBp(UINT index, UINT referenceMhz)
{
    UINT64 ceiling10;                   /* fps x 10 at a 100% share, this host */
    UINT fps10;

    if (index >= CPUSPEED_COUNT || referenceMhz == 0u)
        return CPUSPEED_BP_FULL_U;
    fps10 = g_CpuSpeedDoomFps10[index];
    if (fps10 == 0u)
        return CPUSPEED_BP_FULL_U;
    ceiling10 = (UINT64)CPUSPEED_FPS_PER_KMHZ10 * referenceMhz / (UINT64)MEGAHERTZ_PER_GIGAHERTZ_U;
    if (ceiling10 == 0ull || fps10 >= ceiling10)
        return CPUSPEED_BP_FULL_U;                                            /* a ceiling, never a boost */
    /* Round UP, and never to zero: a duty of 0 would stop the guest dead. */
    {   UINT64 dutyBp = ((UINT64)fps10 * (UINT64)CPUSPEED_BP_FULL_U + ceiling10 - 1ull) / ceiling10;
        return dutyBp ? (UINT)dutyBp : 1u; }
}

UINT CpuSpeedRealModeDutyBp(UINT protectedModeBp)
{
    UINT64 dutyBp;

    if (protectedModeBp == 0u || protectedModeBp >= CPUSPEED_BP_FULL_U)
        return CPUSPEED_BP_FULL_U;
    dutyBp = (UINT64)protectedModeBp * CPUSPEED_RM_PCT / PERCENT_U;
    return dutyBp >= (UINT64)CPUSPEED_BP_FULL_U ? CPUSPEED_BP_FULL_U : (UINT)dutyBp;
}

/* The finest period this host can actually deliver at `duty_bp`, given a measured
 * round-trip cost. Below rt_us/duty there is no run phase left to shorten.
 *
 * [CAUTION]: ALSO FLOORED BY THE HOLD: Sleep cannot express less than a millisecond, so a
 * period whose OFF phase rounds to zero delivers no throttling at all -- the debt
 * carries, but the guest runs free meanwhile. Hence the second term.
 */
static UINT CpuSpeedPeriodFloorMs(UINT dutyBp, unsigned long roundTripUs)
{
    unsigned long byRun, byHold;

    if (!dutyBp || dutyBp >= CPUSPEED_BP_FULL_U)
        return CPUSPEED_GRAN_MIN_MS;
    if (!roundTripUs)
        roundTripUs = CPUSPEED_DEFAULT_ROUND_TRIP_US;                                    /* unmeasured: a sane placeholder */
    byRun  = (roundTripUs * (unsigned long)CPUSPEED_BP_FULL_U + dutyBp - 1ul) / dutyBp / (unsigned long)MICROSECONDS_PER_MILLISECOND_U;  /* ms */
    byHold = ((unsigned long)CPUSPEED_BP_FULL_U + (CPUSPEED_BP_FULL_U - dutyBp) - 1ul) / (CPUSPEED_BP_FULL_U - dutyBp);
    if (byHold < 1ul)
        byHold = 1ul;
    { unsigned long floorMs = byRun > byHold ? byRun : byHold;
      if (floorMs < CPUSPEED_GRAN_MIN_MS)
          floorMs = CPUSPEED_GRAN_MIN_MS;
      if (floorMs > CPUSPEED_GRAN_MAX_MS)
          floorMs = CPUSPEED_GRAN_MAX_MS;
      return (UINT)floorMs; }
}

/* How long to let the guest run this period, in MICROSECONDS, for a target period.
 * Returns 0 when the answer is "do not wait at all -- reach for it immediately",
 * which is the fine end of the slider and the whole point of it.
 */
static unsigned long CpuSpeedRunUs(UINT dutyBp, UINT periodMs)
{
    if (!dutyBp || dutyBp >= CPUSPEED_BP_FULL_U)
        return 0ul;
    return ((unsigned long)periodMs * (unsigned long)MICROSECONDS_PER_MILLISECOND_U * dutyBp) / (unsigned long)CPUSPEED_BP_FULL_U;
}

UINT64 CpuSpeedHoldFor(UINT64 executedUs, UINT64 wallUs, UINT dutyBp)
{
    UINT64 targetUs;

    if (dutyBp == 0u || dutyBp >= CPUSPEED_BP_FULL_U)
        return 0ull;                                                 /* unlimited: never hold */
    targetUs = (executedUs * (UINT64)CPUSPEED_BP_FULL_U) / (UINT64)dutyBp;
    return targetUs > wallUs ? targetUs - wallUs : 0ull;
}

UINT64 CpuSpeedStep(UINT64 executedUs, UINT64 wallUs, UINT dutyBp, UINT64 capUs, INT *isReset)
{
    UINT64 rawHold = CpuSpeedHoldFor(executedUs, wallUs, dutyBp);
    UINT64 hold = rawHold > capUs ? capUs : rawHold;

    *isReset = (rawHold == 0ull) || (wallUs > CPUSPEED_MAX_WINDOW_US);
    return hold;
}

UINT CpuSpeedDeliveredBp(UINT64 executedUs, UINT64 wallUs)
{
    if (!wallUs)
        return 0u;
    return (UINT)((executedUs * (UINT64)CPUSPEED_BP_FULL_U) / wallUs);
}

unsigned long CpuSpeedInstructionsPerSecond(UINT index)
{
    if (index == 0u || index >= CPUSPEED_COUNT)
        return 0ul;
    return (unsigned long)g_CpuSpeedMhz[index] * (unsigned long)HERTZ_PER_MEGAHERTZ_U / CPUSPEED_CPI;
}

INT CpuSpeedCharge(
    CPUSPEED_PACE *pace,
    unsigned long ran,
    unsigned long instructionsPerSecond,
    INT64 elapsedUs)
{
    INT64 wantedUs, milliseconds;

    if (!instructionsPerSecond || !ran)
    {
        if (pace->OwedUs < 0)
            pace->OwedUs = 0;
        return 0;
    }
    wantedUs = ((INT64)ran * (INT64)MICROSECONDS_PER_SECOND_U) / (INT64)instructionsPerSecond;
    pace->OwedUs += wantedUs - elapsedUs;
    if (pace->OwedUs < 0)
        pace->OwedUs = 0;                                /* no credit for running fast */
    if (pace->OwedUs > CPUSPEED_MAX_DEBT_US)
        pace->OwedUs = CPUSPEED_MAX_DEBT_US;                                      /* 100 ms ceiling on one debt */
    milliseconds = pace->OwedUs / (INT64)MICROSECONDS_PER_MILLISECOND_U;
    pace->OwedUs -= milliseconds * (INT64)MICROSECONDS_PER_MILLISECOND_U;
    return (INT)milliseconds;
}
