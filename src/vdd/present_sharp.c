/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The function definitions of present_sharp.h, which holds the reasoning. (#418)
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "present_sharp.h"

#define PRESENT_SHARP_RED_BLUE      0x00FF00FFu
#define PRESENT_SHARP_GREEN         0x0000FF00u
#define PRESENT_SHARP_WEIGHT_SHIFT  8

typedef struct
{
    WORD Index;         /* First source pixel this destination pixel covers */
    WORD Weight;        /* Share of the next one, out of PRESENT_SHARP_WEIGHT_ONE */
} PRESENT_SHARP_STEP;

/* Used by the presenting thread alone. */
static PRESENT_SHARP_STEP g_SharpColumns[PRESENT_SHARP_AXIS_MAX];
static PRESENT_SHARP_STEP g_SharpRows[PRESENT_SHARP_AXIS_MAX];
static DWORD g_SharpRowBlend[PRESENT_SHARP_AXIS_MAX];

static VOID PresentSharpBuildAxis(PRESENT_SHARP_STEP *steps, UINT sourceSize, UINT destinationSize)
{
    UINT position;

    for (position = 0; position < destinationSize; position++)
    {
        UINT start = position * sourceSize;
        UINT end = start + sourceSize;
        UINT index = start / destinationSize;
        UINT boundary = (index + 1) * destinationSize;

        steps[position].Index = (WORD)index;
        steps[position].Weight = end <= boundary
            ? 0
            : (WORD)(((end - boundary) * PRESENT_SHARP_WEIGHT_ONE) / sourceSize);
    }
}

static DWORD PresentSharpBlend(DWORD first, DWORD second, UINT weight)
{
    UINT keep = PRESENT_SHARP_WEIGHT_ONE - weight;
    DWORD redBlue = (first & PRESENT_SHARP_RED_BLUE) * keep + (second & PRESENT_SHARP_RED_BLUE) * weight;
    DWORD green = (first & PRESENT_SHARP_GREEN) * keep + (second & PRESENT_SHARP_GREEN) * weight;

    return ((redBlue >> PRESENT_SHARP_WEIGHT_SHIFT) & PRESENT_SHARP_RED_BLUE)
        | ((green >> PRESENT_SHARP_WEIGHT_SHIFT) & PRESENT_SHARP_GREEN);
}

BOOL PresentSharpCanScale(
    UINT sourceWidth,
    UINT sourceHeight,
    UINT destinationWidth,
    UINT destinationHeight)
{
    return sourceWidth >= 1
        && sourceHeight >= 1
        && sourceWidth <= destinationWidth
        && sourceHeight <= destinationHeight
        && destinationWidth <= PRESENT_SHARP_AXIS_MAX
        && destinationHeight <= PRESENT_SHARP_AXIS_MAX;
}

VOID PresentSharpScale(
    const DWORD *source,
    UINT sourceWidth,
    UINT sourceHeight,
    DWORD *destination,
    UINT destinationWidth,
    UINT destinationHeight)
{
    UINT row;

    PresentSharpBuildAxis(g_SharpColumns, sourceWidth, destinationWidth);
    PresentSharpBuildAxis(g_SharpRows, sourceHeight, destinationHeight);

    for (row = 0; row < destinationHeight; row++)
    {
        const DWORD *line = source + (UINT)g_SharpRows[row].Index * sourceWidth;
        UINT weight = g_SharpRows[row].Weight;
        DWORD *out = destination + row * destinationWidth;
        UINT column;

        if (weight)
        {
            const DWORD *nextLine = line + sourceWidth;

            for (column = 0; column < sourceWidth; column++)
                g_SharpRowBlend[column] = PresentSharpBlend(line[column], nextLine[column], weight);

            line = g_SharpRowBlend;
        }

        for (column = 0; column < destinationWidth; column++)
        {
            const PRESENT_SHARP_STEP *step = &g_SharpColumns[column];

            out[column] = step->Weight
                ? PresentSharpBlend(line[step->Index], line[step->Index + 1], step->Weight)
                : line[step->Index];
        }
    }
}
