/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * X86 instruction lengths: which CD nn byte pairs are really INT instructions.
 *
 * The function definitions of x86len.h, which keeps their declarations and doc comments (#335).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "x86len.h"

/* One-byte opcode map: XL_MR | <imm kind>. */
static const BYTE g_X86OneByteTable[256] = {
/*00 */ 0x10,0x10,0x10,0x10,   1,   2,   0,   0,
/*08 */ 0x10,0x10,0x10,0x10,   1,   2,   0,   0,   /* 0F is handled before the table */
/*10 */ 0x10,0x10,0x10,0x10,   1,   2,   0,   0,
/*18 */ 0x10,0x10,0x10,0x10,   1,   2,   0,   0,
/*20 */ 0x10,0x10,0x10,0x10,   1,   2,   0,   0,
/*28 */ 0x10,0x10,0x10,0x10,   1,   2,   0,   0,
/*30 */ 0x10,0x10,0x10,0x10,   1,   2,   0,   0,
/*38 */ 0x10,0x10,0x10,0x10,   1,   2,   0,   0,
/*40 */    0,   0,   0,   0,   0,   0,   0,   0,
/*48 */    0,   0,   0,   0,   0,   0,   0,   0,
/*50 */    0,   0,   0,   0,   0,   0,   0,   0,
/*58 */    0,   0,   0,   0,   0,   0,   0,   0,
/*60 */    0,   0,0x10,0x10,   0,   0,   0,   0,
/*68 */    2,0x12,   1,0x11,   0,   0,   0,   0,
/*70 */    1,   1,   1,   1,   1,   1,   1,   1,
/*78 */    1,   1,   1,   1,   1,   1,   1,   1,
/*80 */ 0x11,0x12,0x11,0x11,0x10,0x10,0x10,0x10,
/*88 */ 0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,
/*90 */    0,   0,   0,   0,   0,   0,   0,   0,
/*98 */    0,   0,   6,   0,   0,   0,   0,   0,
/*A0 */    4,   4,   4,   4,   0,   0,   0,   0,
/*A8 */    1,   2,   0,   0,   0,   0,   0,   0,
/*B0 */    1,   1,   1,   1,   1,   1,   1,   1,
/*B8 */    2,   2,   2,   2,   2,   2,   2,   2,
/*C0 */ 0x11,0x11,   3,   0,0x10,0x10,0x11,0x12,
/*C8 */    5,   0,   3,   0,   0,   1,   0,   0,
/*D0 */ 0x10,0x10,0x10,0x10,   1,   1,   0,   0,
/*D8 */ 0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,   /* x87 */
/*E0 */    1,   1,   1,   1,   1,   1,   1,   1,
/*E8 */    2,   2,   6,   1,   0,   0,   0,   0,
/*F0 */    0,   0,   0,   0,   0,   0,0x17,0x18,
/*F8 */    0,   0,   0,   0,   0,   0,0x10,0x10
};

/* Is `op` a prefix?  (Segment overrides, operand/address size, lock, rep.) */
static INT X86IsPrefix(BYTE opcode)
{
    return opcode == X86_PREFIX_ES || opcode == X86_PREFIX_CS || opcode == X86_PREFIX_SS || opcode == X86_PREFIX_DS
        || opcode == X86_PREFIX_FS || opcode == X86_PREFIX_GS || opcode == X86_PREFIX_OPERAND_SIZE || opcode == X86_PREFIX_ADDRESS_SIZE
        || opcode == X86_PREFIX_LOCK || opcode == X86_PREFIX_REPNE || opcode == X86_PREFIX_REP;
}

/* 0F-escaped opcodes.  Most take a modrm and no immediate; these are the exceptions. */
static UINT X86TwoByteEntry(BYTE opcode)
{
    switch (opcode)
    {
    case 0x05:
    case 0x06:
    case 0x07:
    case 0x08:
    case 0x09:
    case 0x0B:
    case 0x0E:
    case 0x30:
    case 0x31:
    case 0x32:
    case 0x33:
    case 0x34:
    case 0x35:
    case 0x77:
    case 0xA0:
    case 0xA1:
    case 0xA2:
    case 0xA8:
    case 0xA9:
    case 0xAA:
        return XL_NONE;

    case 0xC8:
    case 0xC9:
    case 0xCA:
    case 0xCB:                 /* bswap */

    case 0xCC:
    case 0xCD:
    case 0xCE:
    case 0xCF:
        return XL_NONE;

    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:                 /* modrm + imm8 */

    case 0xA4:
    case 0xAC:
    case 0xBA:
    case 0xC2:
    case 0xC4:
    case 0xC5:
    case 0xC6:
        return XL_MR | XL_IB;

    default:
        if (opcode >= X86_JCC_NEAR_FIRST && opcode <= X86_JCC_NEAR_LAST)
            return XL_IZ;                                                                          /* jcc rel16/32 */
        return XL_MR | XL_NONE;
    }
}

/* Bytes consumed by a modrm (+sib +disp).  0 = ran off the end. */
static UINT X86ModrmLength(const BYTE *bytes, UINT offset, UINT length, INT isAddress32)
{
    BYTE modrm;
    UINT mode;
    UINT registerMemory;
    UINT modrmLength = 1;

    if (offset >= length)
        return 0;
    modrm = bytes[offset];
    mode = (UINT)(modrm >> X86_MODRM_MODE_SHIFT);
    registerMemory = (UINT)(modrm & X86_MODRM_FIELD_MASK);
    if (mode == X86_MODE_REGISTER)
        return 1;
    if (isAddress32)
    {
        if (registerMemory == X86_RM_SIB)                                     /* sib */
        {
            if (offset + 1 >= length)
                return 0;
            if (mode == 0 && (bytes[offset + 1] & X86_MODRM_FIELD_MASK) == X86_RM_DISP32)
                modrmLength += X86_DISP32_SIZE;
            modrmLength += 1;
        }
        else if (mode == 0 && registerMemory == X86_RM_DISP32)
        {
            modrmLength += X86_DISP32_SIZE;
        }
        if      (mode == X86_MODE_DISP8)
            modrmLength += 1;
        else if (mode == X86_MODE_DISP_FULL)
            modrmLength += X86_DISP32_SIZE;
    }
    else
    {
        if (mode == 0 && registerMemory == X86_RM16_DISP16)
            modrmLength += X86_DISP16_SIZE;
        if      (mode == X86_MODE_DISP8)
            modrmLength += 1;
        else if (mode == X86_MODE_DISP_FULL)
            modrmLength += X86_DISP16_SIZE;
    }
    return modrmLength;
}

UINT X86InstructionLength(const BYTE *bytes, UINT offset, UINT length, INT isDefault32)
{
    UINT start = offset;
    UINT prefixCount = 0;
    UINT entry;
    UINT immediateSize;
    INT isOperand32 = isDefault32;
    INT isAddress32 = isDefault32;
    INT registerField = -1;
    BYTE opcode;

    while (offset < length && X86IsPrefix(bytes[offset]))
    {
        if      (bytes[offset] == X86_PREFIX_OPERAND_SIZE)
            isOperand32 = !isDefault32;
        else if (bytes[offset] == X86_PREFIX_ADDRESS_SIZE)
            isAddress32 = !isDefault32;
        ++offset;
        if (++prefixCount > X86_MAX_PREFIXES)
            return 0;                                                          /* prefix soup: not code */
    }
    if (offset >= length)
        return 0;
    opcode = bytes[offset++];
    if (opcode == X86_ESCAPE)
    {
        BYTE opcode2;
        if (offset >= length)
            return 0;
        opcode2 = bytes[offset++];
        if (opcode2 == X86_ESCAPE_38 || opcode2 == X86_ESCAPE_3A)                    /* 3-byte escapes */
        {
            UINT modrmLength;
            if (offset >= length)
                return 0;
            ++offset;
            modrmLength = X86ModrmLength(bytes, offset, length, isAddress32);
            if (!modrmLength)
                return 0;
            offset += modrmLength;
            if (opcode2 == X86_ESCAPE_3A)
                ++offset;
            return (offset <= length) ? offset - start : 0;
        }
        entry = X86TwoByteEntry(opcode2);
    }
    else
    {
        entry = g_X86OneByteTable[opcode];
    }
    if (entry & XL_MR)
    {
        UINT modrmLength;
        if (offset >= length)
            return 0;
        registerField = (INT)((bytes[offset] >> X86_MODRM_REG_SHIFT) & X86_MODRM_FIELD_MASK);
        modrmLength = X86ModrmLength(bytes, offset, length, isAddress32);
        if (!modrmLength)
            return 0;
        offset += modrmLength;
    }
    immediateSize = isOperand32 ? X86_IMM32_SIZE : X86_IMM16_SIZE;
    switch (entry & XL_KIND_MASK)
    {
    case XL_NONE:
        break;

    case XL_IB:
        offset += 1;
    break;

    case XL_IZ:
        offset += immediateSize;
    break;

    case XL_IW:
        offset += X86_IMM16_SIZE;
    break;

    case XL_MOFF:
        offset += isAddress32 ? X86_IMM32_SIZE : X86_IMM16_SIZE;
    break;

    case XL_ENTER:
        offset += X86_ENTER_IMMEDIATE_SIZE;
    break;

    case XL_FAR:
        offset += immediateSize + X86_SELECTOR_SIZE;
    break;

    case XL_G3B:
        if (registerField >= 0 && registerField < X86_GROUP3_IMMEDIATE_FORMS)
            offset += 1;
    break;

    case XL_G3Z:
        if (registerField >= 0 && registerField < X86_GROUP3_IMMEDIATE_FORMS)
            offset += immediateSize;
    break;

    default:
        return 0;
    }
    return (offset <= length) ? offset - start : 0;
}

INT X86IsInstructionStart(const BYTE *bytes, UINT offset, UINT length, INT isDefault32)
{
    UINT span = (offset < X86_BOUNDARY_SPAN) ? offset : X86_BOUNDARY_SPAN;
    UINT streamStart;
    UINT tries = 0;
    UINT votes = 0;

    if (offset >= length)
        return 0;
    for (streamStart = offset - span; streamStart < offset; ++streamStart)
    {
        UINT position = streamStart;
        ++tries;
        while (position < offset)
        {
            UINT instructionLength = X86InstructionLength(bytes, position, length, isDefault32);
            if (!instructionLength)
                break;                                                 /* not a decodable stream */
            position += instructionLength;
        }
        if (position == offset)
            ++votes;
    }
    if (!tries)
        return 1;                                        /* at the very start: trust it */
    return votes * X86_VOTE_FRACTION >= tries;
}

INT X86IsIntSiteReal(const BYTE *bytes, UINT offset, UINT length, INT isDefault32)
{
    UINT back;

    if (X86IsInstructionStart(bytes, offset, length, isDefault32))
        return 1;
    for (back = 1; back < X86_MAX_INSTRUCTION && back <= offset; ++back)
    {
        UINT instructionLength = X86InstructionLength(bytes, offset - back, length, isDefault32);
        if (instructionLength > back && X86IsInstructionStart(bytes, offset - back, length, isDefault32))
            return 0;               /* an instruction owns it -> it is an OPERAND */
    }
    return 1;                       /* nothing owns it -> keep */
}
