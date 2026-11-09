/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Sharp's last step: from the largest whole multiple to the destination, weighting by area. (#418)
 *
 * GDI's HALFTONE did this and cost 67 ms a frame at 1920x1000 to 1920x1080 on a Q9650 (mean of 576
 * frames, runs/s105/sk3), even between system-memory bitmaps. Here a destination pixel covers one
 * guest pixel, or two at a guest pixel's edge, weighted by how much of each. The whole-multiple
 * image is never drawn: its pixels are only ever guest pixels, so the tables go straight to those,
 * and a destination row with the same guest rows as the one above it is a copy of that row.
 * A present over 14.3 ms cannot keep up with a 70 Hz guest, and Mario's text flickers (play3).
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

/* The largest whole multiple of sourceSize that fits destinationSize, at least 1. */
UINT PresentSharpWholeScale(UINT sourceSize, UINT destinationSize);

/* TRUE when PresentSharpScale can do this: an enlargement on both axes, inside the tables. */
BOOL PresentSharpCanScale(
    UINT sourceWidth,
    UINT sourceHeight,
    UINT destinationWidth,
    UINT destinationHeight);

/* Scale 32-bit guest pixels: each repeated by the whole multiples, the remainder weighted by area.
 * Rows are top-down with no padding between them.
 */
VOID PresentSharpScale(
    const DWORD *source,
    UINT sourceWidth,
    UINT sourceHeight,
    DWORD *destination,
    UINT destinationWidth,
    UINT destinationHeight);

#endif /* NTVDMEX_PRESENT_SHARP_H */
