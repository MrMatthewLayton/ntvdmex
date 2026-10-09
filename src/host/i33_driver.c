/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * INT 33h: cursor masks, acceleration profiles, alternate handlers.
 *
 * The function definitions of i33_driver.h, which keeps their declarations and doc comments (#335).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "i33_driver.h"

VOID I33GraphicsCursorRow(
    BYTE *row,
    INT width,
    INT left,
    WORD screenMask,
    WORD cursorMask,
    BYTE ones,
    const BYTE *colourMap)
{
    INT index;

    if (!colourMap)
    {
        for (index = 0; index < I33_GC_PIXELS; ++index)
        {
            INT column = left + index;
            WORD bit = (WORD)(I33_GC_LEFT_BIT >> index);
            BYTE value;
            if (column < 0 || column >= width)
                continue;
            value = (screenMask & bit) ? row[column] : 0;
            if (cursorMask & bit)
                value = (BYTE)(value ^ ones);
            row[column] = value;
        }
        return;
    }
    for (index = 0; index < I33_GC_CGA_PIXELS; ++index)
    {
        INT column = left + index, colour;
        UINT shift = (UINT)(I33_GC_CGA_LEFT_SHIFT - I33_GC_CGA_BITS * index);
        UINT screenBits = (screenMask >> shift) & I33_GC_CGA_MASK, cursorBits = (cursorMask >> shift) & I33_GC_CGA_MASK, valueBits = 0;
        if (column < 0 || column >= width)
            continue;
        for (colour = 0; colour < I33_GC_CGA_COLOURS; ++colour) if (colourMap[colour] == row[column])
        {
            valueBits = (UINT)colour;
            break;
        }
        valueBits = (valueBits & screenBits) ^ cursorBits;
        row[column] = colourMap[valueBits & I33_GC_CGA_MASK];
    }
}

VOID I33GraphicsCursorDraw(
    BYTE *pixels,
    INT width,
    INT height,
    INT stride,
    INT pointerX,
    INT pointerY,
    INT hotX,
    INT hotY,
    const WORD *screenMask,
    const WORD *cursorMask,
    BYTE ones,
    const BYTE *colourMap)
{
    INT rowIndex;
    INT left = pointerX - (colourMap ? hotX / I33_GC_CGA_BITS : hotX);
    INT top = pointerY - hotY;

    if (!pixels || width <= 0 || height <= 0)
        return;
    for (rowIndex = 0; rowIndex < I33_GC_ROWS; ++rowIndex)
    {
        INT row = top + rowIndex;
        if (row < 0 || row >= height)
            continue;
        I33GraphicsCursorRow(pixels + (long)row * stride, width, left, screenMask[rowIndex], cursorMask[rowIndex], ones, colourMap);
    }
}

static const CHAR g_I33AccelerationDefaultNames[I33_ACC_N][I33_ACC_NAMELEN + 1] = {
    "Slow            ", "Moderate        ", "Fast            ", "Unaccelerated   " };

VOID I33AccelerationDefaultNames(BYTE *names)
{
    INT profile, index;

    for (profile = 0; profile < I33_ACC_N; ++profile)
        for (index = 0; index < I33_ACC_NAMELEN; ++index)
            names[profile * I33_ACC_NAMELEN + index] = (BYTE)g_I33AccelerationDefaultNames[profile][index];
}

VOID I33AccelerationDefaults(BYTE *acceleration)
{
    INT profile, index;

    for (profile = 0; profile < I33_ACC_N; ++profile)
    {
        acceleration[I33_ACC_LENS + profile] = 1;
        for (index = 0; index < I33_ACC_ENTRIES; ++index)
        {
            acceleration[I33_ACC_THRESH + profile * I33_ACC_ENTRIES + index] = I33_ACC_UNUSED_THRESHOLD;
            acceleration[I33_ACC_FACTOR + profile * I33_ACC_ENTRIES + index] = I33_ACC_FACTOR_ONE;
        }
    }
    I33AccelerationDefaultNames(acceleration + I33_ACC_NAMES);
}

UINT I33SettingsBlock(
    BYTE *out,
    UINT capacity,
    const I33_SETTINGS *settings,
    const BYTE *acceleration)
{
    BYTE block[I33_SET_LEN];
    UINT count = capacity < I33_SET_LEN ? capacity : I33_SET_LEN, index;

    for (index = 0; index < I33_SET_HDR; ++index)
        block[index] = 0;
    block[I33_SET_TYPE] = settings->Type;
    block[I33_SET_LANGUAGE] = settings->Language;
    block[I33_SET_HORIZONTAL_SPEED] = settings->HorizontalSpeed;
    block[I33_SET_VERTICAL_SPEED] = settings->VerticalSpeed;
    block[I33_SET_DOUBLE_SPEED] = settings->DoubleSpeed;
    block[I33_SET_CURVE] = settings->Curve;
    block[I33_SET_RATE] = settings->Rate;
    for (index = 0; index < I33_ACC_LEN; ++index)
        block[I33_SET_HDR + index] = acceleration[index];
    for (index = 0; index < count; ++index)
        out[index] = block[index];
    return count;
}

UINT I33ShiftBits(BYTE keyboardFlags)
{
    return ((keyboardFlags & I33_KB_SHIFT) ? I33_ALT_SHIFT : 0u) | ((keyboardFlags & I33_KB_CTRL) ? I33_ALT_CTRL : 0u)
         | ((keyboardFlags & I33_KB_ALT) ? I33_ALT_ALT : 0u);
}

INT I33AlternateSet(I33_ALTERNATE *alternates, WORD mask, WORD segment, UINT32 offset)
{
    INT index, freeIndex = -1;
    UINT shifts = mask & I33_ALT_SHIFTS;

    if (!shifts)
        return 0;                                           /* needs one of Shift/Ctrl/Alt */
    for (index = 0; index < I33_ALT_N; ++index)
    {
        if (alternates[index].Mask && (alternates[index].Mask & I33_ALT_SHIFTS) == shifts)
            break;
        if (!alternates[index].Mask && freeIndex < 0)
            freeIndex = index;
    }
    if (index == I33_ALT_N)
    {
        if (freeIndex < 0)
            return 0;
        index = freeIndex;
    }
    alternates[index].Mask = mask;
    alternates[index].Segment = segment;
    alternates[index].Offset = offset;
    return 1;
}

INT I33AlternateFind(const I33_ALTERNATE *alternates, WORD mask)
{
    INT index;
    UINT shifts = mask & I33_ALT_SHIFTS;

    if (!shifts)
        return -1;
    for (index = 0; index < I33_ALT_N; ++index)
        if (alternates[index].Mask && (alternates[index].Mask & I33_ALT_SHIFTS) == shifts)
            return index;
    return -1;
}

INT I33AlternateAny(const I33_ALTERNATE *alternates)
{
    INT index;

    for (index = 0; index < I33_ALT_N; ++index)
        if (alternates[index].Mask & I33_ALT_EVENTS)
            return 1;
    return 0;
}

INT I33PickHandler(
    const I33_ALTERNATE *alternates,
    UINT events,
    BYTE keyboardFlags,
    UINT mainMask,
    UINT *conditions)
{
    UINT shifts = I33ShiftBits(keyboardFlags);
    INT index;

    if (shifts)
        for (index = 0; index < I33_ALT_N; ++index)
        {
            UINT mask = alternates[index].Mask;
            if (mask && (mask & I33_ALT_SHIFTS) == shifts && (events & mask & I33_ALT_EVENTS))
            {
                *conditions = (events & mask & I33_ALT_EVENTS) | shifts;
                return index;
            }
        }
    if (events & mainMask)
    {
        *conditions = events & mainMask;
        return I33_PICK_MAIN;
    }
    *conditions = 0;
    return I33_PICK_NOBODY;
}
