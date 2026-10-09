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
