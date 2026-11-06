/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Choosing one die (src/host/cpu_die.h). (#417)
 *
 *   cc -std=c99 -I src/host -o cpu_die_test tests/unit/cpu_die_test.c src/host/cpu_die.c
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include "cpu_die.h"

#define CHECK(condition, name) \
    do \
    { \
        if (!(condition)) \
        { \
            printf("FAIL: %s\n", name); \
            g_Failures++; \
        } \
    } while (0)

static INT g_Failures;

INT main(VOID)
{
    /* Q9650: L1 per core, one L2 per die. */
    static const CPU_DIE_CACHE quad[] = {
        { 1, 0x1 }, { 1, 0x2 }, { 1, 0x4 }, { 1, 0x8 }, { 2, 0xC }, { 2, 0x3 }
    };
    /* E8600: one die. */
    static const CPU_DIE_CACHE duo[] = { { 1, 0x1 }, { 1, 0x2 }, { 2, 0x3 } };
    /* A quad with one L3 and per-core L2s. */
    static const CPU_DIE_CACHE sharedL3[] = {
        { 2, 0x1 }, { 2, 0x2 }, { 2, 0x4 }, { 2, 0x8 }, { 3, 0xF }
    };
    /* Pentium D: two dies with a core each. */
    static const CPU_DIE_CACHE twoSingles[] = { { 2, 0x1 }, { 2, 0x2 } };
    /* Two sockets of hyperthreaded quads. */
    static const CPU_DIE_CACHE sockets[] = { { 3, 0xFF00 }, { 3, 0x00FF } };

    CHECK(CpuDieChooseMask(quad, 6, 0xF) == 0x3, "Q9650: the lower die");
    CHECK(CpuDieChooseMask(quad, 6, 0xC) == 0, "Q9650 already held to the upper die: nothing to do");
    CHECK(CpuDieChooseMask(quad, 6, 0x3) == 0, "Q9650 already held to the lower die: nothing to do");
    CHECK(CpuDieChooseMask(quad, 6, 0xE) == 0xC, "Q9650, cpu0 refused: the die with room");
    CHECK(CpuDieChooseMask(quad, 6, 0x5) == 0, "Q9650, one cpu of each die: no die has room");
    CHECK(CpuDieChooseMask(duo, 3, 0x3) == 0, "E8600: one die");
    CHECK(CpuDieChooseMask(sharedL3, 5, 0xF) == 0, "a shared L3: one die");
    CHECK(CpuDieChooseMask(twoSingles, 2, 0x3) == 0, "a core a die: the host needs two");
    CHECK(CpuDieChooseMask(sockets, 2, 0xFFFF) == 0x00FF, "two sockets: the lower one");
    CHECK(CpuDieChooseMask(NULL, 0, 0xF) == 0, "no cache records: leave it alone");

    if (g_Failures)
    {
        printf("cpu_die_test: %d FAILED\n", g_Failures);
        return 1;
    }

    printf("cpu_die_test: all passed\n");
    return 0;
}
