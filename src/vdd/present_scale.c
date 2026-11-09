/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The display's scaling and aspect arithmetic, the effect pass (Scale2x, Scale3x,
 * Scanlines, CRT) and the tint filters.
 *
 * The function definitions of present_scale.h, which keeps their declarations and doc comments (#335).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "present_scale.h"

/* The effect pass's shading tables: [row kind][channel kind][channel value]. */
#define PRESENT_LEVELS          256
#define PRESENT_ROW_KINDS       2
#define PRESENT_CHANNEL_KINDS   2
#define PRESENT_ROW_BRIGHT      0
#define PRESENT_ROW_DARK        1
#define PRESENT_CHANNEL_UNLIT   0   /* Another colour's mask column (CRT) */
#define PRESENT_CHANNEL_LIT     1

/* The mask's columns, in order. */
#define PRESENT_PHASE_RED       0
#define PRESENT_PHASE_GREEN     1
#define PRESENT_PHASE_BLUE      2

/* The 3x3 neighbourhood of an EPX source pixel, E in the middle:
 * A B C
 * D E F
 * G H I
 */
enum
{
    PRESENT_EPX_A = 0, PRESENT_EPX_B, PRESENT_EPX_C,
    PRESENT_EPX_D, PRESENT_EPX_E, PRESENT_EPX_F,
    PRESENT_EPX_G, PRESENT_EPX_H, PRESENT_EPX_I,
    PRESENT_EPX_NEIGHBOURS
};

INT PresentScalerFactor(INT scaler)
{
    if (scaler == PRESENT_SCALER_SCALE2X)
        return PRESENT_SCALE2X_FACTOR;

    if (scaler == PRESENT_SCALER_SCALE3X)
        return PRESENT_SCALE3X_FACTOR;

    return 1;
}

INT PresentScalerHasScanlines(INT scaler)
{
    return scaler == PRESENT_SCALER_SCANLINES || scaler == PRESENT_SCALER_CRT;
}

INT PresentScalerHasEffect(INT scaler)
{
    return scaler > PRESENT_SCALER_NONE && scaler < PRESENT_SCALER_COUNT;
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

/* Scale2x: E becomes 2x2, row by row. */
static VOID PresentEpx2(const UINT32 *neighbours, UINT32 *block)
{
    UINT32 above = neighbours[PRESENT_EPX_B];
    UINT32 left = neighbours[PRESENT_EPX_D];
    UINT32 centre = neighbours[PRESENT_EPX_E];
    UINT32 right = neighbours[PRESENT_EPX_F];
    UINT32 below = neighbours[PRESENT_EPX_H];

    block[0] = block[1] = block[2] = block[3] = centre;

    if (above == below || left == right)
        return;

    if (left == above)
        block[0] = left;

    if (above == right)
        block[1] = right;

    if (left == below)
        block[2] = left;

    if (below == right)
        block[3] = right;
}

/* Scale3x: E becomes 3x3, row by row. The edge middles take a neighbour only where
 * the corner beyond them disagrees with E, so a one-pixel line keeps its width.
 */
static VOID PresentEpx3(const UINT32 *neighbours, UINT32 *block)
{
    UINT32 aboveLeft = neighbours[PRESENT_EPX_A];
    UINT32 above = neighbours[PRESENT_EPX_B];
    UINT32 aboveRight = neighbours[PRESENT_EPX_C];
    UINT32 left = neighbours[PRESENT_EPX_D];
    UINT32 centre = neighbours[PRESENT_EPX_E];
    UINT32 right = neighbours[PRESENT_EPX_F];
    UINT32 belowLeft = neighbours[PRESENT_EPX_G];
    UINT32 below = neighbours[PRESENT_EPX_H];
    UINT32 belowRight = neighbours[PRESENT_EPX_I];
    INT index;

    for (index = 0; index < PRESENT_EPX_NEIGHBOURS; ++index)
        block[index] = centre;

    if (above == below || left == right)
        return;

    if (left == above)
        block[0] = left;

    if ((left == above && centre != aboveRight) || (above == right && centre != aboveLeft))
        block[1] = above;

    if (above == right)
        block[2] = right;

    if ((left == above && centre != belowLeft) || (left == below && centre != aboveLeft))
        block[3] = left;

    if ((above == right && centre != belowRight) || (below == right && centre != aboveRight))
        block[5] = right;

    if (left == below)
        block[6] = left;

    if ((left == below && centre != belowRight) || (below == right && centre != belowLeft))
        block[7] = below;

    if (below == right)
        block[8] = right;
}

VOID PresentScaleEpx(
    const UINT32 *source,
    INT sourceWidth,
    INT sourceHeight,
    INT sourceStride,
    INT factor,
    UINT32 *destination)
{
    INT destinationStride = sourceWidth * factor;
    INT row;

    for (row = 0; row < sourceHeight; ++row)
    {
        const UINT32 *rowAbove = source + (SIZE_T)(row > 0 ? row - 1 : 0) * sourceStride;
        const UINT32 *sourceRow = source + (SIZE_T)row * sourceStride;
        const UINT32 *rowBelow = source + (SIZE_T)(row < sourceHeight - 1 ? row + 1 : row) * sourceStride;
        UINT32 *outputRow = destination + (SIZE_T)row * factor * destinationStride;
        INT column;

        for (column = 0; column < sourceWidth; ++column)
        {
            INT columnLeft = column > 0 ? column - 1 : 0;
            INT columnRight = column < sourceWidth - 1 ? column + 1 : column;
            UINT32 neighbours[PRESENT_EPX_NEIGHBOURS];
            UINT32 block[PRESENT_EPX_NEIGHBOURS];
            INT subRow;
            INT subColumn;

            neighbours[PRESENT_EPX_A] = rowAbove[columnLeft];
            neighbours[PRESENT_EPX_B] = rowAbove[column];
            neighbours[PRESENT_EPX_C] = rowAbove[columnRight];
            neighbours[PRESENT_EPX_D] = sourceRow[columnLeft];
            neighbours[PRESENT_EPX_E] = sourceRow[column];
            neighbours[PRESENT_EPX_F] = sourceRow[columnRight];
            neighbours[PRESENT_EPX_G] = rowBelow[columnLeft];
            neighbours[PRESENT_EPX_H] = rowBelow[column];
            neighbours[PRESENT_EPX_I] = rowBelow[columnRight];

            if (factor == PRESENT_SCALE3X_FACTOR)
                PresentEpx3(neighbours, block);
            else
                PresentEpx2(neighbours, block);

            for (subRow = 0; subRow < factor; ++subRow)
            {
                UINT32 *output = outputRow + (SIZE_T)subRow * destinationStride + column * factor;

                for (subColumn = 0; subColumn < factor; ++subColumn)
                    output[subColumn] = block[subRow * factor + subColumn];
            }
        }
    }
}

INT PresentEffectScale(
    INT sourceWidth,
    INT sourceHeight,
    INT pictureWidth,
    INT pictureHeight,
    UINT maxPixels)
{
    INT wholeScale;
    INT scale;
    INT largestFitting = 1;

    if (sourceWidth < 1 || sourceHeight < 1)
        return 1;

    wholeScale = pictureWidth / sourceWidth;

    if (pictureHeight / sourceHeight < wholeScale)
        wholeScale = pictureHeight / sourceHeight;

    for (scale = wholeScale; scale >= PRESENT_SCALE2X_FACTOR; --scale)
    {
        if ((UINT64)sourceWidth * scale * sourceHeight * scale > maxPixels)
            continue;

        if (wholeScale % scale == 0)
            return scale;

        if (largestFitting == 1)
            largestFitting = scale;
    }

    return largestFitting;
}

/* One channel value scaled by percent, clamped. */
static BYTE PresentLevel(INT value, INT percent)
{
    INT level = value * percent / PERCENT;

    return (BYTE)(level > PRESENT_CHANNEL_MAX ? PRESENT_CHANNEL_MAX : level);
}

/* The shading tables. Without a mask, a channel is lit whatever its column, so both
 * channel kinds hold the same values and the per-pixel code needs no branch.
 */
static VOID PresentEffectLevels(
    INT scaler,
    BYTE levels[PRESENT_ROW_KINDS][PRESENT_CHANNEL_KINDS][PRESENT_LEVELS])
{
    INT isCrt = scaler == PRESENT_SCALER_CRT;
    INT darkPercent = isCrt ? PRESENT_CRT_SCANLINE_PERCENT : PRESENT_SCANLINES_DARK_PERCENT;
    INT liftPercent = isCrt ? PRESENT_CRT_LIFT_PERCENT : PERCENT;
    INT unlitPercent = isCrt ? PRESENT_CRT_MASK_PERCENT : PERCENT;
    INT value;

    for (value = 0; value < PRESENT_LEVELS; ++value)
    {
        INT brightRow = value * liftPercent / PERCENT;
        INT darkRow = brightRow * darkPercent / PERCENT;

        levels[PRESENT_ROW_BRIGHT][PRESENT_CHANNEL_LIT][value] = PresentLevel(brightRow, PERCENT);
        levels[PRESENT_ROW_BRIGHT][PRESENT_CHANNEL_UNLIT][value] = PresentLevel(brightRow, unlitPercent);
        levels[PRESENT_ROW_DARK][PRESENT_CHANNEL_LIT][value] = PresentLevel(darkRow, PERCENT);
        levels[PRESENT_ROW_DARK][PRESENT_CHANNEL_UNLIT][value] = PresentLevel(darkRow, unlitPercent);
    }
}

/* A pixel through one row's tables; `phase` is the mask column (PRESENT_PHASE_*).
 * (Channel kind LIT is 1, so a comparison is the index.)
 */
static UINT32 PresentEffectShade(
    UINT32 pixel,
    BYTE rowLevels[PRESENT_CHANNEL_KINDS][PRESENT_LEVELS],
    INT phase)
{
    UINT32 red = rowLevels[phase == PRESENT_PHASE_RED][(pixel >> PRESENT_RED_SHIFT) & PRESENT_CHANNEL_MASK];
    UINT32 green = rowLevels[phase == PRESENT_PHASE_GREEN][(pixel >> PRESENT_GREEN_SHIFT) & PRESENT_CHANNEL_MASK];
    UINT32 blue = rowLevels[phase == PRESENT_PHASE_BLUE][pixel & PRESENT_CHANNEL_MASK];

    return (red << PRESENT_RED_SHIFT) | (green << PRESENT_GREEN_SHIFT) | blue;
}

VOID PresentEffectRender(
    const UINT32 *source,
    INT sourceWidth,
    INT sourceHeight,
    INT sourceStride,
    INT scaler,
    INT scale,
    UINT32 *scratch,
    UINT32 *destination)
{
    BYTE levels[PRESENT_ROW_KINDS][PRESENT_CHANNEL_KINDS][PRESENT_LEVELS];
    const UINT32 *image = source;
    INT imageStride = sourceStride;
    INT factor = PresentScalerFactor(scaler);
    INT isShaded = PresentScalerHasScanlines(scaler);
    INT outputWidth = sourceWidth * scale;
    INT outputHeight = sourceHeight * scale;
    INT firstDarkRow = scale - scale / 2;
    INT row;

    if (scale < 1 || sourceWidth < 1 || sourceHeight < 1)
        return;

    if (factor > scale)
        factor = scale;

    if (factor > 1)
    {
        PresentScaleEpx(source, sourceWidth, sourceHeight, sourceStride, factor, scratch);
        image = scratch;
        imageStride = sourceWidth * factor;
    }

    if (isShaded)
        PresentEffectLevels(scaler, levels);

    /* [INFO]: THE IMAGE IS factor TIMES THE FRAME AND THE OUTPUT scale TIMES, so an
     * output row or column maps to image row * factor / scale. The columns step by an
     * accumulator rather than a divide per pixel.
     */
    for (row = 0; row < outputHeight; ++row)
    {
        const UINT32 *imageRow = image + (SIZE_T)(row * factor / scale) * imageStride;
        UINT32 *outputRow = destination + (SIZE_T)row * outputWidth;
        INT rowKind = isShaded && row % scale >= firstDarkRow ? PRESENT_ROW_DARK : PRESENT_ROW_BRIGHT;
        INT imageColumn = 0;
        INT step = 0;
        INT column;

        for (column = 0; column < outputWidth; ++column)
        {
            UINT32 pixel = imageRow[imageColumn];

            outputRow[column] = isShaded
                ? PresentEffectShade(pixel, levels[rowKind], column % PRESENT_CRT_MASK_PHASES)
                : pixel;
            step += factor;

            if (step >= scale)
            {
                step -= scale;
                ++imageColumn;
            }
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
