/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM tests for src/dos/dos_version.h: which INT 21h services each configured DOS
 * version has (#28). The table's rows are RBIL's "DOS x.y+"; these pin the table, and
 * above all that the default, 6.22, and everything newer lose nothing at all.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include "dos_version.h"

#define CHECK(condition, message) do { ++g_Checks; if (condition) printf("  PASS  %s\n", (message)); \
    else { printf("  FAIL  %s\n", (message)); ++g_Failures; } } while (0)

static INT g_Checks = 0;
static INT g_Failures = 0;

/* Every AH, and for 33h every AL: the version has all of them. */
static INT DosVerTestHasEverything(BYTE major, BYTE minor)
{
    UINT function;
    UINT subfunction;

    for (function = 0; function <= 0xFF; ++function)
        for (subfunction = 0; subfunction <= 0xFF; ++subfunction)
            if (DosVersionAnswer(major, minor, (BYTE)function, (BYTE)subfunction) != DOS_VERSION_HAS)
                return 0;

    return 1;
}

INT main(VOID)
{
    printf("== dos_version.h (#28)\n");

    /* THE DEFAULT IS UNTOUCHED: 6.22, and every version the dialog lists from 5.00 up. */
    CHECK(DosVerTestHasEverything(6, 22), "6.22 (the default) has every AH and every 33h AL");
    CHECK(DosVerTestHasEverything(5, 0), "5.00 (XP's shell) has every AH and every 33h AL");
    CHECK(DosVerTestHasEverything(7, 10), "7.10 has every AH and every 33h AL");

    /* 4.01 */
    CHECK(DosVersionAnswer(4, 1, 0x6C, 0) == DOS_VERSION_HAS, "4.01 has 6Ch (extended open, DOS 4.0+)");
    CHECK(DosVersionAnswer(4, 1, 0x69, 0) == DOS_VERSION_HAS, "4.01 has 69h (disk serial, DOS 4.0+)");
    CHECK(DosVersionAnswer(4, 1, 0x33, 0x05) == DOS_VERSION_HAS, "4.01 has 3305h (boot drive, DOS 4.0+)");
    CHECK(DosVersionAnswer(4, 1, 0x33, 0x06) == DOS_VERSION_LACKS_SUBFUNCTION, "4.01 lacks 3306h (true version, DOS 5.0+): AL=FF");
    CHECK(DosVersionAnswer(4, 1, 0x33, 0x00) == DOS_VERSION_HAS, "4.01 has 3300h");

    /* 3.31 */
    CHECK(DosVersionAnswer(3, 31, 0x6C, 0) == DOS_VERSION_LACKS_FUNCTION, "3.31 lacks 6Ch: AL=00");
    CHECK(DosVersionAnswer(3, 31, 0x69, 0) == DOS_VERSION_LACKS_FUNCTION, "3.31 lacks 69h");
    CHECK(DosVersionAnswer(3, 31, 0x6A, 0) == DOS_VERSION_LACKS_FUNCTION, "3.31 lacks 6Ah");
    CHECK(DosVersionAnswer(3, 31, 0x65, 0) == DOS_VERSION_HAS, "3.31 has 65h (extended country, DOS 3.3+)");
    CHECK(DosVersionAnswer(3, 31, 0x68, 0) == DOS_VERSION_HAS, "3.31 has 68h (commit, DOS 3.3+)");
    CHECK(DosVersionAnswer(3, 31, 0x33, 0x05) == DOS_VERSION_LACKS_SUBFUNCTION, "3.31 lacks 3305h");
    CHECK(DosVersionAnswer(3, 31, 0x33, 0x06) == DOS_VERSION_LACKS_SUBFUNCTION, "3.31 lacks 3306h");

    /* 3.20 and 3.00: the 3.3 and 3.1 additions. */
    CHECK(DosVersionAnswer(3, 20, 0x67, 0) == DOS_VERSION_LACKS_FUNCTION, "3.20 lacks 67h (handle count, DOS 3.3+)");
    CHECK(DosVersionAnswer(3, 20, 0x5F, 0) == DOS_VERSION_HAS, "3.20 has 5Fh (DOS 3.1+)");
    CHECK(DosVersionAnswer(3, 0, 0x5F, 0) == DOS_VERSION_LACKS_FUNCTION, "3.00 lacks 5Fh");
    CHECK(DosVersionAnswer(3, 0, 0x59, 0) == DOS_VERSION_HAS, "3.00 has 59h (extended error, DOS 3.0+)");
    CHECK(DosVersionAnswer(2, 11, 0x59, 0) == DOS_VERSION_LACKS_FUNCTION, "2.11 lacks 59h");
    CHECK(DosVersionAnswer(2, 11, 0x62, 0) == DOS_VERSION_LACKS_FUNCTION, "2.11 lacks 62h");

    /* What no version loses: the 2.0 calls and the LFN API (stock NTVDM's, not a version's). */
    CHECK(DosVersionAnswer(2, 11, 0x3D, 0) == DOS_VERSION_HAS, "2.11 has 3Dh (open)");
    CHECK(DosVersionAnswer(3, 31, 0x71, 0x6C) == DOS_VERSION_HAS, "3.31 keeps 716Ch (the LFN API is NTVDM's)");

    /* The boundary is the version, not a neighbour: 3.30 has the 3.3 calls, 3.29 not. */
    CHECK(DosVersionAnswer(3, 30, 0x66, 0) == DOS_VERSION_HAS && DosVersionAnswer(3, 29, 0x66, 0) == DOS_VERSION_LACKS_FUNCTION,
          "the 3.3 boundary falls between 3.29 and 3.30");
    CHECK(DosVersionAnswer(4, 0, 0x6C, 0) == DOS_VERSION_HAS && DosVersionAnswer(3, 99, 0x6C, 0) == DOS_VERSION_LACKS_FUNCTION,
          "the 4.0 boundary falls between 3.99 and 4.00");

    printf("== %d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}
