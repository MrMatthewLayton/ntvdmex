/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The DPMI real-mode call structure, and INT 31h 0300h's routing.
 *
 * The function definitions of dpmi_rmcs.h, which keeps their declarations and doc comments (#335).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "dpmi_rmcs.h"

DWORD RmcsRead32(const volatile BYTE *structure, UINT offset)
{
    return (DWORD)structure[offset] | ((DWORD)structure[offset + 1] << BYTE_SHIFT)
         | ((DWORD)structure[offset + 2] << WORD_SHIFT) | ((DWORD)structure[offset + 3] << TOP_BYTE_SHIFT);
}

WORD RmcsRead16(const volatile BYTE *structure, UINT offset)
{
    return (WORD)(structure[offset] | (structure[offset + 1] << BYTE_SHIFT));
}

static VOID RmcsWrite32(volatile BYTE *structure, UINT offset, DWORD value)
{
    structure[offset] = (BYTE)value;
    structure[offset + 1] = (BYTE)(value >> BYTE_SHIFT);
    structure[offset + 2] = (BYTE)(value >> WORD_SHIFT);
    structure[offset + 3] = (BYTE)(value >> TOP_BYTE_SHIFT);
}

static VOID RmcsWrite16(volatile BYTE *structure, UINT offset, WORD value)
{
    structure[offset] = (BYTE)value;
    structure[offset + 1] = (BYTE)(value >> BYTE_SHIFT);
}

VOID RmcsRead(const volatile BYTE *structure, PRMCS_REGS out)
{
    out->Edi = RmcsRead32(structure, RMCS_EDI);
    out->Esi = RmcsRead32(structure, RMCS_ESI);
    out->Ebp = RmcsRead32(structure, RMCS_EBP);
    out->Ebx = RmcsRead32(structure, RMCS_EBX);
    out->Edx = RmcsRead32(structure, RMCS_EDX);
    out->Ecx = RmcsRead32(structure, RMCS_ECX);
    out->Eax = RmcsRead32(structure, RMCS_EAX);
    out->Flags = RmcsRead16(structure, RMCS_FLAGS);
    out->Es = RmcsRead16(structure, RMCS_ES);
    out->Ds = RmcsRead16(structure, RMCS_DS);
    out->Fs = RmcsRead16(structure, RMCS_FS);
    out->Gs = RmcsRead16(structure, RMCS_GS);
}

VOID RmcsWrite(volatile BYTE *structure, PCRMCS_REGS in)
{
    RmcsWrite32(structure, RMCS_EDI, in->Edi);
    RmcsWrite32(structure, RMCS_ESI, in->Esi);
    RmcsWrite32(structure, RMCS_EBP, in->Ebp);
    RmcsWrite32(structure, RMCS_EBX, in->Ebx);
    RmcsWrite32(structure, RMCS_EDX, in->Edx);
    RmcsWrite32(structure, RMCS_ECX, in->Ecx);
    RmcsWrite32(structure, RMCS_EAX, in->Eax);
    RmcsWrite16(structure, RMCS_FLAGS, in->Flags);
    RmcsWrite16(structure, RMCS_ES, in->Es);
    RmcsWrite16(structure, RMCS_DS, in->Ds);
    RmcsWrite16(structure, RMCS_FS, in->Fs);
    RmcsWrite16(structure, RMCS_GS, in->Gs);
}

INT RmcsSimIntRoute(UINT vector, WORD ivtSegment, WORD ivtOffset, INT isReflectOn, WORD ourSegment)
{
    if (vector == VECTOR_DOS)
        return SIMINT_FAST;
    if (vector == VECTOR_MOUSE || vector == VECTOR_VIDEO)
        if (ivtSegment == ourSegment || !isReflectOn)
            return SIMINT_FAST;
    if (!isReflectOn)
        return SIMINT_NONE;
    if (ivtSegment == 0 && ivtOffset == 0)
        return SIMINT_NONE;
    return SIMINT_RUN;
}

INT RmcsStackPlan(WORD stackPointer, UINT words, UINT frame, PWORD stackPointerAfter)
{
    DWORD available = stackPointer ? (DWORD)stackPointer : RMCS_STACK_FULL;
    DWORD needed = (DWORD)words * X86_WORD_SIZE_U;

    if (needed + frame > available)
    {
        *stackPointerAfter = stackPointer;
        return 0;
    }
    *stackPointerAfter = (WORD)(available - needed);
    return 1;
}
