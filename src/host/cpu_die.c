/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The function definitions of cpu_die.h, which holds the reasoning. (#417)
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "cpu_die.h"

static UINT CpuDieCountBits(DWORD mask)
{
    UINT count = 0;

    while (mask)
    {
        mask &= mask - 1;
        count++;
    }

    return count;
}

DWORD CpuDieChooseMask(const CPU_DIE_CACHE *caches, UINT cacheCount, DWORD processMask)
{
    BYTE topLevel = 0;
    DWORD chosen = 0;
    UINT cacheIndex;

    for (cacheIndex = 0; cacheIndex < cacheCount; cacheIndex++)
    {
        if ((caches[cacheIndex].ProcessorMask & processMask) && caches[cacheIndex].Level > topLevel)
            topLevel = caches[cacheIndex].Level;
    }

    for (cacheIndex = 0; cacheIndex < cacheCount; cacheIndex++)
    {
        DWORD usable = caches[cacheIndex].ProcessorMask & processMask;

        if (caches[cacheIndex].Level != topLevel || CpuDieCountBits(usable) < CPU_DIE_MIN_PROCESSORS)
            continue;

        if (!chosen || (usable & (~usable + 1)) < (chosen & (~chosen + 1)))
            chosen = usable;
    }

    return chosen == processMask ? 0 : chosen;
}
