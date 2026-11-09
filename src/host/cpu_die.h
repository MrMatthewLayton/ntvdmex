/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Which processors make one die. (#417)
 *
 * A Core 2 Quad is two dual-core dies in one package, so "package" does not separate them, but each
 * die has its own last-level cache, and that does. Keeping the whole host on one die took BOUNCEBX
 * from 1-15 retrace edges in 29 s to 1854 on a Q9650 (runs/s104/aff), and a CPU with one shared
 * last-level cache has nothing to separate.
 *
 * Pure arithmetic with no Win32, for tests/unit/cpu_die_test.c: the host turns
 * GetLogicalProcessorInformation's cache records into CPU_DIE_CACHE rows.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_CPU_DIE_H
#define NTVDMEX_CPU_DIE_H

#include "../ntvdmex_types.h"

#define CPU_DIE_MIN_PROCESSORS  2u  /* The guest and the host's threads need room */

typedef enum
{
    CPU_DIE_AUTO = 0, CPU_DIE_OFF
} CPU_DIE_MODE;

typedef struct
{
    BYTE  Level;                            /* 1, 2, 3 */
    DWORD ProcessorMask;
} CPU_DIE_CACHE;

/* The processors to keep the host on, or 0 to leave it alone: the lowest group sharing a
 * last-level cache, when there are several and that group has room. processMask limits the answer.
 */
DWORD CpuDieChooseMask(const CPU_DIE_CACHE *caches, UINT cacheCount, DWORD processMask);

#endif /* NTVDMEX_CPU_DIE_H */
