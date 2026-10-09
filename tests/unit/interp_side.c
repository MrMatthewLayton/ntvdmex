/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * ONE copy of src/host/v86interp.h, wrapped so that two copies (two
 * different versions of the header) can be linked into one binary. GH #194.
 *
 * Compiled twice by scripts/interpfuzz.sh (MODE=superset):
 *     -DSIDE=ref_ -DINTERP_H="<HEAD's header>"     and     -DSIDE=new_  (working tree)
 * Everything in the header is `static`, so each object has its own interpreter, its own
 * guest memory and its own hooks; the only exported names are the SIDE-prefixed ones
 * below. tests/unit/interp_superset.c drives both in lockstep.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdint.h>
#include <string.h>
#include "interp_xcpu.h"

#ifndef SIDE
#error "define SIDE (ref_ or new_)"
#endif
#define CAT2(first, second)     first##second
#define CAT(first, second)      CAT2(first, second)
#define FN(name)                CAT(SIDE, name)

static BYTE g_Memory[XMEM_SIZE];
static UINT64 g_Effects;                                 /* this step's effects, hashed */
static VOID InterpSideEffect(UINT64 value)
{
    INT index;

    for (index = 0; index < 8; ++index)
    {
        g_Effects ^= (BYTE)(value >> (index * 8));
        g_Effects *= 1099511628211ULL;
    }
}

/* An undo log, so a step the OTHER side did not take can be rolled back. */
#define UNDO_MAX    (1u << 20)
static UINT32 g_UndoLinear[UNDO_MAX]; static BYTE g_UndoOld[UNDO_MAX];
static UINT32 g_UndoCount; static INT g_IsUndoOn, g_IsUndoOverflow;

static BYTE V86HostRead8(UINT32 linear)
{
    return (linear < sizeof g_Memory) ? g_Memory[linear] : 0xFF;
}

static VOID V86HostWrite8(UINT32 linear, BYTE value)
{
    InterpSideEffect(((UINT64)linear << 8) | value);
    if (linear >= sizeof g_Memory)
        return;
    if (g_IsUndoOn)
    {
        if (g_UndoCount < UNDO_MAX)
        {
            g_UndoLinear[g_UndoCount] = linear;
            g_UndoOld[g_UndoCount] = g_Memory[linear];
            ++g_UndoCount;
        }
        else
            g_IsUndoOverflow = 1;
    }
    g_Memory[linear] = value;
}

static UINT32 V86HostIn(WORD port, INT width)
{
    InterpSideEffect(0x1000000ULL | port | ((UINT64)width << 16));
    return (UINT32)port * 2654435761u;
}

static VOID V86HostOut(WORD port, INT width, UINT32 value)
{
    InterpSideEffect(0x2000000ULL | port | ((UINT64)width << 16) | ((UINT64)value << 32));
}

#ifndef INTERP_H
#define INTERP_H    "../../src/host/v86interp.h"
#endif
#include INTERP_H

/* A FAKE descriptor table for the protected-mode run -- identical on both sides, so any
 * difference is the interpreter's. Index mod 4: 16-bit code / data / 32-bit code /
 * not present; limits alternate 64 KB and 32 KB; bases are spread over the low 1 MB.
 */
static UINT32 InterpSideSegmentToLinear(WORD selector)
{
    return ((UINT32)(selector >> 3) * 0x1230u) & 0xFFFF0u;
}

static INT InterpSideSelectorDescriptor(WORD selector, UINT32 *accessRights, UINT32 *limit)
{
    UINT index = selector >> 3;
    UINT kind = index & 3;

    if (!(selector & 4) || index == 0 || kind == 3)
        return 0;
    *accessRights = (kind == 1 ? 0xF2u : 0xFAu) << 8;
    if (kind == 2)
        *accessRights |= 0x4u << 20;                               /* D = 1 */
    *limit = (index & 4) ? 0x7FFFu : 0xFFFFu;
    return 1;
}

VOID FN(Initialize)(PCBYTE image, INT isProtectedMode)
{
    memcpy(g_Memory, image, sizeof g_Memory);
    g_V86SegmentToLinear  = isProtectedMode ? InterpSideSegmentToLinear : 0;
    g_V86SelectorDescriptor = isProtectedMode ? InterpSideSelectorDescriptor : 0;
}

VOID FN(Poke)(UINT32 linear, BYTE value)
{
    if (linear < sizeof g_Memory)
        g_Memory[linear] = value;
}

PCBYTE FN(Memory)(VOID)
{
    return g_Memory;
}

VOID FN(Sync)(PCBYTE image)
{
    memcpy(g_Memory, image, sizeof g_Memory);
}

INT FN(Step)(PINTERP_XCPU state, UINT64 *effects)
{
    V86_CPU cpu;
    INT isOk;
    INT index;

    for (index = 0; index < 8; ++index)
        cpu.Registers[index] = state->Registers[index];
    for (index = 0; index < 6; ++index)
        cpu.Segments[index] = state->Segments[index];
    cpu.Ip = state->Ip;
    cpu.Flags = state->Flags;
    g_Effects = 1469598103934665603ULL;
    isOk = V86Step(&cpu);
    for (index = 0; index < 8; ++index)
        state->Registers[index] = cpu.Registers[index];
    for (index = 0; index < 6; ++index)
        state->Segments[index] = cpu.Segments[index];
    state->Ip = cpu.Ip;
    state->Flags = cpu.Flags;
    *effects = g_Effects;
    return isOk;
}

VOID FN(UndoBegin)(VOID)
{
    g_UndoCount = 0;
    g_IsUndoOverflow = 0;
    g_IsUndoOn = 1;
}

/* Roll back every write since undo_begin. Returns 0 if the log overflowed (the caller
 * must then resync the whole image).
 */
INT FN(UndoRollback)(VOID)
{
    UINT32 index = g_UndoCount;

    g_IsUndoOn = 0;
    while (index)
    {
        --index;
        g_Memory[g_UndoLinear[index]] = g_UndoOld[index];
    }
    return !g_IsUndoOverflow;
}

VOID FN(UndoEnd)(VOID)
{
    g_IsUndoOn = 0;
}
