/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * INT 31h's spec-decided answers: selector validity, the real-mode callback slots, and resize plans.
 *
 * The function definitions of dpmi_svc.h, which keeps their declarations and doc comments (#335).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "dpmi_svc.h"

INT DpmiIsSelectorValid(WORD selector, INT indexLimit, INT isAllocated, INT isGuestOwnedTable)
{
    INT index = DPMI_SELECTOR_INDEX(selector);

    if (!(selector & DPMI_SELECTOR_TI))
        return 0;                                    /* TI = 0: GDT */

    if (index < 1 || index >= indexLimit)
        return 0;                                    /* null, or off the end */

    if (!isAllocated && !isGuestOwnedTable)
        return 0;                                       /* never handed out */

    return 1;
}

WORD DpmiCallbackEntry(WORD base, INT slot)
{
    return (WORD)(base + slot * DPMI_CB_STRIDE);
}

INT DpmiCallbackSlotAt(WORD base, WORD codeSegment, WORD wantedSegment, WORD instructionPointer)
{
    INT slot;

    if (codeSegment != wantedSegment || instructionPointer < base)
        return -1;

    slot = (instructionPointer - base) / DPMI_CB_STRIDE;
    return (slot < DPMI_CB_SLOTS) ? slot : -1;
}

INT DpmiCallbackSlotOf(WORD base, WORD codeSegment, WORD wantedSegment, WORD offset)
{
    INT slot = DpmiCallbackSlotAt(base, codeSegment, wantedSegment, offset);

    return (slot >= 0 && DpmiCallbackEntry(base, slot) == offset) ? slot : -1;
}

INT DpmiResizePlan(UINT32 newSize, UINT32 committed, UINT32 *copy)
{
    if (copy)
        *copy = 0;

    if (newSize == 0)
        return DPMI_RESIZE_BAD;

    if (newSize <= committed)
        return DPMI_RESIZE_INPLACE;

    if (copy)
        *copy = committed;                           /* new > committed: all of the old */

    return DPMI_RESIZE_MOVE;
}

INT DpmiLimitOk(DWORD limit)
{
    if (limit <= DPMI_BYTE_LIMIT_MAX)
        return 1;

    return (limit & DPMI_PAGE_LIMIT_LOW_BITS) == DPMI_PAGE_LIMIT_LOW_BITS;
}

INT DpmiAccessOk(WORD rights)
{
    BYTE access = (BYTE)rights;

    if (!(access & DPMI_ACCESS_S))
        return 0;                                    /* a system descriptor */

    if ((access & DPMI_ACCESS_EXECUTABLE) && (access & DPMI_ACCESS_CONFORMING))
        return 0;                                    /* conforming code */

    if (rights & DPMI_RIGHTS_RESERVED_BIT)
        return 0;                                    /* CH bit 5 must be 0 */

    return 1;
}

INT DpmiBaseOk(DWORD base, DWORD cap)
{
    return base <= cap;
}

INT DpmiDosChainCount(WORD paragraphs)
{
    return paragraphs ? (INT)(((DWORD)paragraphs + DPMI_DOS_CHAIN_PARAGRAPHS - 1) / DPMI_DOS_CHAIN_PARAGRAPHS) : 1;
}

VOID DpmiDosChainDescriptor(WORD paragraphs, INT index, DWORD *baseOffset, DWORD *limit)
{
    DWORD bytes = (DWORD)paragraphs << 4;
    DWORD offset = (DWORD)index * DPMI_DOS_CHAIN_SPAN;
    DWORD left;

    *baseOffset = offset;

    if (!paragraphs)
    {
        *limit = 0;
        return;
    }

    if (index == 0)
    {
        *limit = bytes - 1;                          /* a 32-bit host: the whole block */
        return;
    }

    left = bytes - offset;
    *limit = (left > DPMI_DOS_CHAIN_SPAN ? DPMI_DOS_CHAIN_SPAN : left) - 1;
}

VOID DpmiFreeMemoryInfo(DWORD info[DPMI_FREE_INFO_DWORDS], DWORD poolPages, DWORD usedPages)
{
    DWORD freePages = usedPages < poolPages ? poolPages - usedPages : 0;
    INT index;

    for (index = 0; index < DPMI_FREE_INFO_DWORDS; ++index)
        info[index] = DPMI_FREE_INFO_UNAVAILABLE_U;

    info[DPMI_FREE_INFO_LARGEST_BLOCK] = freePages << 12;
    info[DPMI_FREE_INFO_MAX_UNLOCKED] = freePages;
    info[DPMI_FREE_INFO_MAX_LOCKED] = freePages;
    info[DPMI_FREE_INFO_LINEAR_TOTAL] = poolPages;
    info[DPMI_FREE_INFO_UNLOCKED] = poolPages;
    info[DPMI_FREE_INFO_FREE] = freePages;
    info[DPMI_FREE_INFO_PHYSICAL] = poolPages;
    info[DPMI_FREE_INFO_LINEAR_FREE] = freePages;
    info[DPMI_FREE_INFO_PAGING_FILE] = 0;
}

VOID DpmiRawSwitchPlan(
    DWORD eax,
    DWORD ebx,
    DWORD ecx,
    DWORD edx,
    DWORD esi,
    DWORD edi,
    INT isWide,
    PDPMI_RAW_SWITCH target)
{
    target->Ds  = (WORD)eax;
    target->Es  = (WORD)ecx;
    target->Ss  = (WORD)edx;
    target->Cs  = (WORD)esi;
    target->Esp = isWide ? ebx : (ebx & 0xFFFFu);
    target->Eip = isWide ? edi : (edi & 0xFFFFu);
}
