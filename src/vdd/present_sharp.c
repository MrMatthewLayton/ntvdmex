/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The function definitions of present_sharp.h, which holds the reasoning. (#418)
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "present_sharp.h"
#include <string.h>

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
static UINT g_SharpColumnsSource;
static UINT g_SharpColumnsDestination;
static UINT g_SharpRowsSource;
static UINT g_SharpRowsDestination;

static VOID PresentSharpBuildAxis(PRESENT_SHARP_STEP *steps, UINT sourceSize, UINT destinationSize)
{
    UINT scale = PresentSharpWholeScale(sourceSize, destinationSize);
    UINT wholeSize = sourceSize * scale;
    UINT position;

    for (position = 0; position < destinationSize; position++)
    {
        UINT start = position * wholeSize;
        UINT end = start + wholeSize;
        UINT index = (start / destinationSize) / scale;
        UINT boundary = (index + 1) * scale * destinationSize;

        steps[position].Index = (WORD)index;
        steps[position].Weight = end <= boundary
            ? 0
            : (WORD)(((end - boundary) * PRESENT_SHARP_WEIGHT_ONE) / wholeSize);
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

UINT PresentSharpWholeScale(UINT sourceSize, UINT destinationSize)
{
    UINT scale = destinationSize / sourceSize;

    return scale ? scale : 1;
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

    if (g_SharpColumnsSource != sourceWidth || g_SharpColumnsDestination != destinationWidth)
    {
        PresentSharpBuildAxis(g_SharpColumns, sourceWidth, destinationWidth);
        g_SharpColumnsSource = sourceWidth;
        g_SharpColumnsDestination = destinationWidth;
    }

    if (g_SharpRowsSource != sourceHeight || g_SharpRowsDestination != destinationHeight)
    {
        PresentSharpBuildAxis(g_SharpRows, sourceHeight, destinationHeight);
        g_SharpRowsSource = sourceHeight;
        g_SharpRowsDestination = destinationHeight;
    }

    for (row = 0; row < destinationHeight; row++)
    {
        const DWORD *line = source + (UINT)g_SharpRows[row].Index * sourceWidth;
        UINT weight = g_SharpRows[row].Weight;
        DWORD *out = destination + row * destinationWidth;
        UINT column;

        if (row
            && g_SharpRows[row].Index == g_SharpRows[row - 1].Index
            && g_SharpRows[row].Weight == g_SharpRows[row - 1].Weight)
        {
            memcpy(out, out - destinationWidth, destinationWidth * sizeof(DWORD));
            continue;
        }

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
