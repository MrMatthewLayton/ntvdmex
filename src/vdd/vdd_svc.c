/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The VDD service tables: memory hooks, IRQ reservations, user hooks, the registry list.
 *
 * The function definitions of vdd_svc.h, which keeps their declarations and doc comments (#11).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "vdd_svc.h"

#define VDD_SVC_CASCADE_LINE        2   /* The slave PIC's input on the master */
#define VDD_SVC_LINE_ORDER_COUNT    13

/* "Any free line": the lines an AT leaves to add-in cards, the commonly-free ones first
 * (10, 11, 12, 15 had no motherboard device on most AT-class machines).
 */
static const BYTE g_VddSvcLineOrder[VDD_SVC_LINE_ORDER_COUNT] = { 10, 11, 12, 15, 9, 5, 7, 3, 4, 14, 6, 13, 1 };

static INT VddSvcRangesOverlap(
    DWORD firstStart,
    DWORD firstCount,
    DWORD secondStart,
    DWORD secondCount)
{
    return firstStart < secondStart + secondCount && secondStart < firstStart + firstCount;
}

INT VddSvcMemoryHookAdd(
    PVDD_SVC_MEMORY_HOOKS hooks,
    PVOID owner,
    DWORD start,
    DWORD count,
    PVOID handler)
{
    INT index;
    INT freeIndex = -1;

    if (!owner || !handler || !count || start + count < start)
        return VDD_SVC_FAILED;

    for (index = 0; index < VDD_SVC_MEMORY_HOOKS_MAX; ++index)
    {
        PVDD_SVC_MEMORY_HOOK entry = &hooks->Entries[index];

        if (!entry->Owner)
        {
            if (freeIndex < 0)
                freeIndex = index;
        }
        else if (VddSvcRangesOverlap(entry->Start, entry->Count, start, count))
            return VDD_SVC_FAILED;
    }

    if (freeIndex < 0)
        return VDD_SVC_FAILED;

    hooks->Entries[freeIndex].Owner = owner;
    hooks->Entries[freeIndex].Start = start;
    hooks->Entries[freeIndex].Count = count;
    hooks->Entries[freeIndex].Handler = handler;
    return VDD_SVC_OK;
}

INT VddSvcMemoryHookRemove(PVDD_SVC_MEMORY_HOOKS hooks, PVOID owner, DWORD start, DWORD count)
{
    INT index;

    for (index = 0; index < VDD_SVC_MEMORY_HOOKS_MAX; ++index)
    {
        PVDD_SVC_MEMORY_HOOK entry = &hooks->Entries[index];

        if (entry->Owner && entry->Owner == owner && entry->Start == start && entry->Count == count)
        {
            entry->Owner = 0;
            entry->Handler = 0;
            return VDD_SVC_OK;
        }
    }

    return VDD_SVC_FAILED;
}

INT VddSvcMemoryHookRemoveOwner(PVDD_SVC_MEMORY_HOOKS hooks, PVOID owner)
{
    INT index;
    INT removed = 0;

    for (index = 0; index < VDD_SVC_MEMORY_HOOKS_MAX; ++index)
        if (owner && hooks->Entries[index].Owner == owner)
        {
            hooks->Entries[index].Owner = 0;
            hooks->Entries[index].Handler = 0;
            ++removed;
        }

    return removed;
}

PVDD_SVC_MEMORY_HOOK VddSvcMemoryHookFind(PVDD_SVC_MEMORY_HOOKS hooks, DWORD address)
{
    INT index;

    for (index = 0; index < VDD_SVC_MEMORY_HOOKS_MAX; ++index)
    {
        PVDD_SVC_MEMORY_HOOK entry = &hooks->Entries[index];

        if (entry->Owner && address >= entry->Start && address - entry->Start < entry->Count)
            return entry;
    }

    return 0;
}

WORD VddSvcIrqReserve(WORD *reserved, WORD line)
{
    INT index;

    if (line == VDD_SVC_NO_IRQ)
    {
        for (index = 0; index < VDD_SVC_LINE_ORDER_COUNT; ++index)
            if (!(*reserved & (1u << g_VddSvcLineOrder[index])))
            {
                *reserved |= (WORD)(1u << g_VddSvcLineOrder[index]);
                return g_VddSvcLineOrder[index];
            }

        return VDD_SVC_NO_IRQ;
    }

    if (line >= VDD_SVC_IRQ_LINES || line == VDD_SVC_CASCADE_LINE || (*reserved & (1u << line)))
        return VDD_SVC_NO_IRQ;

    *reserved |= (WORD)(1u << line);
    return line;
}

INT VddSvcIrqRelease(WORD *reserved, WORD line)
{
    if (line >= VDD_SVC_IRQ_LINES || !(*reserved & (1u << line)))
        return VDD_SVC_FAILED;

    *reserved &= (WORD)~(1u << line);
    return VDD_SVC_OK;
}

INT VddSvcUserHookAdd(
    PVDD_SVC_USER_HOOKS hooks,
    PVOID owner,
    PVOID create,
    PVOID terminate,
    PVOID block,
    PVOID resume)
{
    INT index;
    INT slot = -1;

    if (!owner)
        return VDD_SVC_FAILED;

    for (index = 0; index < VDD_SVC_USER_HOOKS_MAX; ++index)
    {
        if (hooks->Entries[index].Owner == owner)
        {
            slot = index;                           /* installing again replaces the set */
            break;
        }

        if (!hooks->Entries[index].Owner && slot < 0)
            slot = index;
    }

    if (slot < 0)
        return VDD_SVC_FAILED;

    hooks->Entries[slot].Owner = owner;
    hooks->Entries[slot].Create = create;
    hooks->Entries[slot].Terminate = terminate;
    hooks->Entries[slot].Block = block;
    hooks->Entries[slot].Resume = resume;
    return VDD_SVC_OK;
}

INT VddSvcUserHookRemove(PVDD_SVC_USER_HOOKS hooks, PVOID owner)
{
    INT index;

    for (index = 0; index < VDD_SVC_USER_HOOKS_MAX; ++index)
        if (owner && hooks->Entries[index].Owner == owner)
        {
            hooks->Entries[index].Owner = 0;
            hooks->Entries[index].Create = 0;
            hooks->Entries[index].Terminate = 0;
            hooks->Entries[index].Block = 0;
            hooks->Entries[index].Resume = 0;
            return VDD_SVC_OK;
        }

    return VDD_SVC_FAILED;
}

INT VddSvcMultiStringNext(PCSTR *cursor, PCSTR end, PSTR name, INT capacity)
{
    while (*cursor < end && **cursor)
    {
        PCSTR start = *cursor;
        INT length = 0;

        while (*cursor < end && **cursor)
        {
            ++length;
            ++*cursor;
        }

        if (*cursor < end)
            ++*cursor;                              /* past the name's NUL */

        if (capacity > 0 && length < capacity)
        {
            INT index;

            for (index = 0; index < length; ++index)
                name[index] = start[index];

            name[length] = 0;
            return 1;
        }
    }

    if (capacity > 0)
        name[0] = 0;

    return 0;
}
