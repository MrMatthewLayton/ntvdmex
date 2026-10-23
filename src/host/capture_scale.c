/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Video capture's fixed output size: choose it, place a frame in it, scale the frame in.
 *
 * The function definitions of capture_scale.h, which keeps their declarations and doc comments.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "capture_scale.h"

#define CAPTURE_SCALE_LADDER_STEPS  5

static const DWORD g_CaptureScaleLadder[CAPTURE_SCALE_LADDER_STEPS][2] =
{
    { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 1024 }, { 1600, 1200 }
};

VOID CaptureScaleChooseOutput(
    DWORD sourceWidth,
    DWORD sourceHeight,
    PDWORD outputWidth,
    PDWORD outputHeight)
{
    INT step;

    for (step = 0; step < CAPTURE_SCALE_LADDER_STEPS; ++step)
    {
        if (sourceWidth <= g_CaptureScaleLadder[step][0] && sourceHeight <= g_CaptureScaleLadder[step][1])
        {
            *outputWidth = g_CaptureScaleLadder[step][0];
            *outputHeight = g_CaptureScaleLadder[step][1];
            return;
        }
    }

    *outputWidth = (sourceWidth + 1) & ~1u;     /* codecs want even sizes */
    *outputHeight = (sourceHeight + 1) & ~1u;
}

VOID CaptureScalePlace(
    DWORD sourceWidth,
    DWORD sourceHeight,
    DWORD outputWidth,
    DWORD outputHeight,
    PCAPTURE_SCALE_PLACEMENT placement)
{
    DWORD factor;

    if (!sourceWidth || !sourceHeight)
    {
        placement->Width = placement->Height = placement->Left = placement->Top = placement->Factor = 0;
        return;
    }

    factor = outputWidth / sourceWidth;

    if (outputHeight / sourceHeight < factor)
        factor = outputHeight / sourceHeight;

    if (factor)
    {
        placement->Width = sourceWidth * factor;
        placement->Height = sourceHeight * factor;
    }
    else if ((ULONGLONG)sourceWidth * outputHeight >= (ULONGLONG)sourceHeight * outputWidth)
    {
        placement->Width = outputWidth;                         /* wider: the width fills */
        placement->Height = (DWORD)((ULONGLONG)sourceHeight * outputWidth / sourceWidth);
    }
    else
    {
        placement->Height = outputHeight;                       /* taller: the height fills */
        placement->Width = (DWORD)((ULONGLONG)sourceWidth * outputHeight / sourceHeight);
    }

    if (!placement->Width)
        placement->Width = 1;

    if (!placement->Height)
        placement->Height = 1;

    placement->Factor = factor;
    placement->Left = (outputWidth - placement->Width) / 2;
    placement->Top = (outputHeight - placement->Height) / 2;
}

DWORD CaptureScaleStride(DWORD width)
{
    return (width * CAPTURE_SCALE_RGB_BYTES + CAPTURE_SCALE_ROW_ALIGN) & ~(DWORD)CAPTURE_SCALE_ROW_ALIGN;
}

VOID CaptureScaleBlit(
    const BYTE *source,
    DWORD sourceWidth,
    DWORD sourceHeight,
    DWORD sourceStride,
    BYTE *target,
    DWORD outputWidth,
    DWORD outputHeight,
    PCCAPTURE_SCALE_PLACEMENT placement)
{
    DWORD targetStride = CaptureScaleStride(outputWidth);
    DWORD row;

    for (row = 0; row < outputHeight; ++row)
    {
        BYTE *line = target + (SIZE_T)(outputHeight - 1 - row) * targetStride;     /* the DIB is bottom-up */
        DWORD column;

        for (column = 0; column < targetStride; ++column)
            line[column] = 0;

        if (row < placement->Top || row >= placement->Top + placement->Height)
            continue;

        {
            DWORD sourceRow = (DWORD)((ULONGLONG)(row - placement->Top) * sourceHeight / placement->Height);
            const BYTE *from = source + (SIZE_T)sourceRow * sourceStride;

            for (column = 0; column < placement->Width; ++column)
            {
                DWORD sourceColumn = (DWORD)((ULONGLONG)column * sourceWidth / placement->Width);
                BYTE *to = line + (SIZE_T)(placement->Left + column) * CAPTURE_SCALE_RGB_BYTES;
                const BYTE *pixel = from + (SIZE_T)sourceColumn * CAPTURE_SCALE_RGB_BYTES;
                to[0] = pixel[0];
                to[1] = pixel[1];
                to[2] = pixel[2];
            }
        }
    }
}
