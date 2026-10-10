/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The display's scaling and aspect arithmetic, and the Scale2x and tint filters.
 *
 * The function definitions of present_scale.h, which keeps their declarations and doc comments (#335).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "present_scale.h"

INT PresentScalerDoubles(INT scaler)
{
    return scaler == PRESENT_SCALER_SCALE2X || scaler == PRESENT_SCALER_CRT;
}

INT PresentScalerHasScanlines(INT scaler)
{
    return scaler == PRESENT_SCALER_SCANLINES || scaler == PRESENT_SCALER_CRT;
}

VOID PresentAspectRatio(INT aspect, INT *ratioWidth, INT *ratioHeight)
{
    switch (aspect)
    {
    case PRESENT_ASPECT_4_3:
        *ratioWidth = 4;
        *ratioHeight = 3;
        break;

    case PRESENT_ASPECT_16_9:
        *ratioWidth = 16;
        *ratioHeight = 9;
        break;

    case PRESENT_ASPECT_16_10:
        *ratioWidth = 16;
        *ratioHeight = 10;
        break;

    default:
        *ratioWidth = 0;
        *ratioHeight = 0;
        break;
    }
}

VOID PresentTargetRatio(
    INT aspect,
    INT sourceWidth,
    INT sourceHeight,
    INT *ratioWidth,
    INT *ratioHeight)
{
    PresentAspectRatio(aspect, ratioWidth, ratioHeight);

    if (!*ratioWidth || !*ratioHeight)
    {
        *ratioWidth = sourceWidth > 0 ? sourceWidth : PRESENT_DEFAULT_RATIO_WIDTH;
        *ratioHeight = sourceHeight > 0 ? sourceHeight : PRESENT_DEFAULT_RATIO_HEIGHT;
    }
}

INT PresentIsNative(INT aspect)
{
    return aspect == PRESENT_ASPECT_NATIVE || aspect == PRESENT_ASPECT_STRETCH;
}

VOID PresentFitRatio(
    INT destinationWidth,
    INT destinationHeight,
    INT ratioWidth,
    INT ratioHeight,
    INT *left,
    INT *top,
    INT *width,
    INT *height)
{
    long fitWidth;
    long fitHeight;

    if (destinationWidth < 1)
        destinationWidth = 1;

    if (destinationHeight < 1)
        destinationHeight = 1;

    if (ratioWidth < 1 || ratioHeight < 1)
    {
        *left = 0;
        *top = 0;
        *width = destinationWidth;
        *height = destinationHeight;
        return;
    }

    fitWidth = destinationWidth;
    fitHeight = (long)destinationWidth * ratioHeight / ratioWidth;

    if (fitHeight > destinationHeight)
    {
        fitHeight = destinationHeight;
        fitWidth = (long)destinationHeight * ratioWidth / ratioHeight;
    }

    if (fitWidth < 1)
        fitWidth = 1;

    if (fitHeight < 1)
        fitHeight = 1;

    *width = (INT)fitWidth;
    *height = (INT)fitHeight;
    *left = (destinationWidth - *width) / 2;
    *top = (destinationHeight - *height) / 2;
}

VOID PresentLayout(
    INT aspect,
    INT fit,
    INT isScreen,
    INT destinationWidth,
    INT destinationHeight,
    INT sourceWidth,
    INT sourceHeight,
    INT *left,
    INT *top,
    INT *width,
    INT *height)
{
    INT ratioWidth;
    INT ratioHeight;

    if (destinationWidth < 1)
        destinationWidth = 1;

    if (destinationHeight < 1)
        destinationHeight = 1;

    if (aspect == PRESENT_ASPECT_STRETCH && isScreen)
    {
        *left = 0;
        *top = 0;
        *width = destinationWidth;
        *height = destinationHeight;
        return;
    }

    PresentTargetRatio(aspect, sourceWidth, sourceHeight, &ratioWidth, &ratioHeight);

    if (sourceWidth > 0 && sourceHeight > 0 && fit == PRESENT_FIT_WHOLE)
    {
        if (PresentIsNative(aspect))
        {
            INT scaleX = destinationWidth / sourceWidth;
            INT scaleY = destinationHeight / sourceHeight;
            INT scale = scaleX < scaleY ? scaleX : scaleY;

            if (scale >= 1)
            {
                *width = sourceWidth * scale;
                *height = sourceHeight * scale;
                *left = (destinationWidth - *width) / 2;
                *top = (destinationHeight - *height) / 2;
                return;
            }
        }
        else
        {
            long bestArea = 0;
            INT bestScaleX = 0;
            INT bestScaleY = 0;
            INT scaleX;

            for (scaleX = 1; (long)sourceWidth * scaleX <= destinationWidth; ++scaleX)
            {
                /* ny = num/den */
                long numerator = (long)sourceWidth * scaleX * ratioHeight;
                long denominator = (long)sourceHeight * ratioWidth;

                if (numerator % denominator)
                    continue;

                if ((long)sourceHeight * (numerator / denominator) > destinationHeight || numerator / denominator < 1)
                    continue;

                if ((long)sourceWidth * scaleX * sourceHeight * (numerator / denominator) > bestArea)
                {
                    bestArea = (long)sourceWidth * scaleX * sourceHeight * (numerator / denominator);
                    bestScaleX = scaleX;
                    bestScaleY = (INT)(numerator / denominator);
                }
            }

            if (bestArea)
            {
                *width = sourceWidth * bestScaleX;
                *height = sourceHeight * bestScaleY;
                *left = (destinationWidth - *width) / 2;
                *top = (destinationHeight - *height) / 2;
                return;
            }
        }
    }

    PresentFitRatio(destinationWidth, destinationHeight, ratioWidth, ratioHeight, left, top, width, height);
}

VOID PresentWindowPicture(
    INT aspect,
    INT sourceWidth,
    INT sourceHeight,
    INT scale,
    INT *width,
    INT *height)
{
    INT ratioWidth;
    INT ratioHeight;

    if (scale < 1)
        scale = 1;

    if (sourceWidth < 1 || sourceHeight < 1)
    {
        sourceWidth = PRESENT_DEFAULT_FRAME_WIDTH;
        sourceHeight = PRESENT_DEFAULT_FRAME_HEIGHT;
    }

    *width = sourceWidth * scale;

    if (PresentIsNative(aspect))
    {
        *height = sourceHeight * scale;
        return;
    }

    PresentTargetRatio(aspect, sourceWidth, sourceHeight, &ratioWidth, &ratioHeight);
    *height = (INT)(((long)*width * ratioHeight + ratioWidth / 2) / ratioWidth);
}

VOID PresentFit(
    INT destinationWidth,
    INT destinationHeight,
    INT aspect,
    INT *left,
    INT *top,
    INT *width,
    INT *height)
{
    INT fitWidth;
    INT fitHeight;
    INT ratioWidth;
    INT ratioHeight;

    if (destinationWidth < 1)
        destinationWidth = 1;

    if (destinationHeight < 1)
        destinationHeight = 1;

    PresentAspectRatio(aspect, &ratioWidth, &ratioHeight);

    if (!ratioWidth || !ratioHeight)
    {
        *left = 0;
        *top = 0;
        *width = destinationWidth;
        *height = destinationHeight;
        return;
    }

    fitWidth = destinationWidth;
    fitHeight = destinationWidth * ratioHeight / ratioWidth;              /* as wide as possible... */

    if (fitHeight > destinationHeight) /* ...unless too tall */
    {
        fitHeight = destinationHeight;
        fitWidth = destinationHeight * ratioWidth / ratioHeight;
    }

    if (fitWidth < 1)
        fitWidth = 1;

    if (fitHeight < 1)
        fitHeight = 1;

    *width = fitWidth;
    *height = fitHeight;
    *left = (destinationWidth - fitWidth) / 2;
    *top = (destinationHeight - fitHeight) / 2;
}

VOID PresentScale2x8(
    const BYTE *source,
    int sourceWidth,
    INT sourceHeight,
    INT sourceStride,
    BYTE *destination)
{
    INT column;
    INT row;
    INT destinationStride = sourceWidth * PRESENT_SCALE2X_FACTOR;

    for (row = 0; row < sourceHeight; ++row)
    {
        const BYTE *sourceRow = source + (SIZE_T)row * sourceStride;
        const BYTE *rowAbove  = source + (SIZE_T)(row > 0        ? row - 1 : 0) * sourceStride;
        const BYTE *rowBelow  = source + (SIZE_T)(row < sourceHeight - 1   ? row + 1 : sourceHeight - 1) * sourceStride;
        BYTE *outputRow0 = destination + (SIZE_T)(row * PRESENT_SCALE2X_FACTOR)     * destinationStride;
        BYTE *outputRow1 = destination + (SIZE_T)(row * PRESENT_SCALE2X_FACTOR + 1) * destinationStride;

        for (column = 0; column < sourceWidth; ++column)
        {
            BYTE centre = sourceRow[column];
            BYTE above = rowAbove[column];
            BYTE below = rowBelow[column];
            BYTE left = sourceRow[column > 0      ? column - 1 : 0];
            BYTE right = sourceRow[column < sourceWidth - 1 ? column + 1 : sourceWidth - 1];
            BYTE topLeft = centre;
            BYTE topRight = centre;
            BYTE bottomLeft = centre;
            BYTE bottomRight = centre;

            if (above != below && left != right)
            {
                if (left == above)
                    topLeft = left;

                if (above == right)
                    topRight = right;

                if (left == below)
                    bottomLeft = left;

                if (below == right)
                    bottomRight = right;
            }

            outputRow0[column * PRESENT_SCALE2X_FACTOR] = topLeft;
            outputRow0[column * PRESENT_SCALE2X_FACTOR + 1] = topRight;
            outputRow1[column * PRESENT_SCALE2X_FACTOR] = bottomLeft;
            outputRow1[column * PRESENT_SCALE2X_FACTOR + 1] = bottomRight;
        }
    }
}

UINT32 PresentTint(UINT32 argb, INT tint)
{
    UINT32 alpha = argb & PRESENT_ALPHA_MASK;
    UINT32 red = (argb >> PRESENT_RED_SHIFT) & PRESENT_CHANNEL_MASK;
    UINT32 green = (argb >> PRESENT_GREEN_SHIFT) & PRESENT_CHANNEL_MASK;
    UINT32 blue = argb & PRESENT_CHANNEL_MASK;
    UINT32 luminance = (red * PRESENT_LUMA_RED + green * PRESENT_LUMA_GREEN + blue * PRESENT_LUMA_BLUE + PRESENT_LUMA_ROUND) / PRESENT_LUMA_SCALE;   /* 0..255 */
    UINT32 outputRed;
    UINT32 outputGreen;
    UINT32 outputBlue;

    switch (tint)
    {
    case PRESENT_TINT_SEPIA:
    {
        /* 1. desaturate to 40%: c' = y + 0.4 (c - y), signed */
        INT sepiaRed = (INT)luminance + ((INT)red - (INT)luminance) * PRESENT_SEPIA_KEEP_NUMERATOR / PRESENT_SEPIA_KEEP_DENOMINATOR;
        INT sepiaGreen = (INT)luminance + ((INT)green - (INT)luminance) * PRESENT_SEPIA_KEEP_NUMERATOR / PRESENT_SEPIA_KEEP_DENOMINATOR;
        INT sepiaBlue = (INT)luminance + ((INT)blue - (INT)luminance) * PRESENT_SEPIA_KEEP_NUMERATOR / PRESENT_SEPIA_KEEP_DENOMINATOR;
        /* 2. warm cast */
        sepiaRed = sepiaRed * PRESENT_SEPIA_WARM_RED / PERCENT;
        sepiaGreen = sepiaGreen * PRESENT_SEPIA_WARM_GREEN / PERCENT;
        sepiaBlue = sepiaBlue * PRESENT_SEPIA_WARM_BLUE / PERCENT;

        if (sepiaRed > PRESENT_CHANNEL_MAX)
            sepiaRed = PRESENT_CHANNEL_MAX;

        if (sepiaGreen > PRESENT_CHANNEL_MAX)
            sepiaGreen = PRESENT_CHANNEL_MAX;

        if (sepiaBlue > PRESENT_CHANNEL_MAX)
            sepiaBlue = PRESENT_CHANNEL_MAX;

        if (sepiaRed < 0)
            sepiaRed = 0;

        if (sepiaGreen < 0)
            sepiaGreen = 0;

        if (sepiaBlue < 0)
            sepiaBlue = 0;

        /* 3. fade: black -> (30,22,12), full scale stays full scale */
        outputRed = PRESENT_SEPIA_BLACK_RED + (UINT32)sepiaRed * PRESENT_SEPIA_SPAN_RED / PRESENT_CHANNEL_SCALE;
        outputGreen = PRESENT_SEPIA_BLACK_GREEN + (UINT32)sepiaGreen * PRESENT_SEPIA_SPAN_GREEN / PRESENT_CHANNEL_SCALE;
        outputBlue = PRESENT_SEPIA_BLACK_BLUE + (UINT32)sepiaBlue * PRESENT_SEPIA_SPAN_BLUE / PRESENT_CHANNEL_SCALE;
        return alpha | (outputRed << PRESENT_RED_SHIFT) | (outputGreen << PRESENT_GREEN_SHIFT) | outputBlue; }

    case PRESENT_TINT_MONO_WHITE:
        outputRed = 255u;
        outputGreen = 255u;
        outputBlue = 255u;
        break;

    case PRESENT_TINT_MONO_GREEN:
        outputRed = 51u;
        outputGreen = 255u;
        outputBlue = 51u;
        break;   /* P1 */

    case PRESENT_TINT_MONO_ORANGE:
        outputRed = 255u;
        outputGreen = 176u;
        outputBlue = 0u;
        break;   /* amber */

    default:
        return argb;
    }

    return alpha | (((luminance * outputRed) / PRESENT_CHANNEL_SCALE) << PRESENT_RED_SHIFT) | (((luminance * outputGreen) / PRESENT_CHANNEL_SCALE) << PRESENT_GREEN_SHIFT) | ((luminance * outputBlue) / PRESENT_CHANNEL_SCALE);
}
