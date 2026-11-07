/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Sharp's last step: from the largest whole multiple to the destination, weighting by area. (#418)
 *
 * GDI's HALFTONE did this and cost 67 ms a frame at 1920x1000 to 1920x1080 on a Q9650 (mean of 576
 * frames, runs/s105/sk3), even between system-memory bitmaps. Each destination pixel here covers at
 * most two source pixels, weighted by how much of each, and the source repeats every guest pixel a
 * whole number of times, so only a guest pixel's edge can blend and edges stay crisp.
 *
 * Pure arithmetic with no Win32, for tests/unit/present_sharp_test.c.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_PRESENT_SHARP_H
#define NTVDMEX_PRESENT_SHARP_H

#include "../ntvdmex_types.h"

#define PRESENT_SHARP_AXIS_MAX      8192u
#define PRESENT_SHARP_WEIGHT_ONE    256u

/* TRUE when PresentSharpScale can do this: an enlargement on both axes, inside the tables. */
BOOL PresentSharpCanScale(
    UINT sourceWidth,
    UINT sourceHeight,
    UINT destinationWidth,
    UINT destinationHeight);

/* Scale 32-bit pixels, top-down with no padding between rows. */
VOID PresentSharpScale(
    const DWORD *source,
    UINT sourceWidth,
    UINT sourceHeight,
    DWORD *destination,
    UINT destinationWidth,
    UINT destinationHeight);

#endif /* NTVDMEX_PRESENT_SHARP_H */
