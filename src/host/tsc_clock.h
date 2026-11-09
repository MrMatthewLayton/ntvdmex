/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * A microsecond clock that reads the CPU's time-stamp counter cheaply and does not believe it. (#416)
 *
 * WHY RDTSC AT ALL: the guest's status-port loops ask for the time flat out, and a reference read is
 * a system call on XP. So the TSC interpolates between reference ANCHORS taken about every 2 ms.
 *
 * WHY IT IS NOT TRUSTED: on a CPU built from two dies the TSCs differ. Measured on a Q9650
 * (runs/s104/tsc): 224-247 ms between dies at 71-83 min uptime, and a 1 s Sleep advanced TSC by as
 * little as 27% of that, so it also appears to stop in idle. A thread that changes die sees the TSC
 * jump either way. Three rules follow:
 * - A jump moves the anchor, never the rate. Rate samples come from long windows of busy reads,
 *   and a sample far from the current rate must be seen twice to be taken.
 * - Time since the anchor is only computed for a TSC at or past the anchor, inside one resync
 *   interval. A hop back would otherwise wrap the unsigned difference.
 * - The anchor follows the reference by a slew, so a reference that ticks once a millisecond
 *   does not shake the beam position by a millisecond every anchor.
 *
 * Pure arithmetic with no Win32, so tests/unit/tsc_clock_test.c can drive it with two simulated dies.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_TSC_CLOCK_H
#define NTVDMEX_TSC_CLOCK_H

#include "../ntvdmex_types.h"

#define TSC_CLOCK_RESYNC_US         2000u   /* ~2 ms of ticks between anchors */
#define TSC_CLOCK_RATE_WINDOW_US    64000u  /* Long enough for a 1 ms reference to give a rate */
#define TSC_CLOCK_STEP_US           2000    /* Further than this from the reference is a jump */
#define TSC_CLOCK_SLEW_SHIFT        3       /* An anchor removes 1/8 of a small error */
#define TSC_CLOCK_RATE_NEAR_SHIFT   4       /* A rate sample within 1/16 agrees */

typedef struct
{
    UINT64 TscBase;
    UINT64 MicrosecondsBase;
    UINT64 Last;                    /* Never backwards within a thread */
    UINT64 Resync;                  /* Ticks between anchors */
    UINT64 RateTscBase;             /* Where the current rate window opened */
    UINT64 RateMicrosecondsBase;
    UINT64 AnchorReference;         /* The reference at the last anchor */
    UINT32 Multiplier;              /* Microseconds per tick, 32.32 */
    UINT32 PendingMultiplier;       /* A far sample, waiting to be seen twice */
    BOOL   HasBase;
} TSC_CLOCK;

/* TRUE when this tick count needs a reference time: TscClockAnchor, not TscClockRead. */
BOOL TscClockNeedsAnchor(const TSC_CLOCK *clock, UINT64 tsc);

/* Take a reference time at this tick count and return the clock's value. */
UINT64 TscClockAnchor(TSC_CLOCK *clock, UINT64 tsc, UINT64 referenceMicroseconds);

/* The clock's value at a tick count that does not need an anchor. */
UINT64 TscClockRead(TSC_CLOCK *clock, UINT64 tsc);

#endif /* NTVDMEX_TSC_CLOCK_H */
