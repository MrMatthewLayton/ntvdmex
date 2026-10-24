/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * fcbfind_test.c -- which drive and directory an FCB search lists (src/dos/dos_fcbfind.h, #240).
 *
 * The s101 rig report: with a floppy image mounted as A:, `A:` then `DIR` at XP's
 * COMMAND.COM listed bin\. DIR searches with an FCB whose drive byte is 0 (the current
 * drive), and the search built "*" -- the PROCESS directory -- even when the current
 * drive was the image. These pin the pattern, and then follow it through the image's
 * resolver the way DosImageMapPath does: "A:*" from A:\ and from A:\DEMOS.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "dos_fcbfind.h"
#include "dos_fatimg.h"

#define CHECK(condition, message) do { ++g_Checks; if (condition) printf("  PASS  %s\n", (message)); \
    else { printf("  FAIL  %s\n", (message)); ++g_Failures; } } while (0)

static INT g_Checks = 0;
static INT g_Failures = 0;

/* The pattern for (drive byte, VirtualDrive), as a string to compare. */
static const CHAR *FcbFindTestPattern(BYTE fcbDrive, INT virtualDrive)
{
    static CHAR pattern[64];

    if (!DosFcbSearchPattern(fcbDrive, virtualDrive, pattern, sizeof(pattern)))
        return "(refused)";

    return pattern;
}

/* What the image resolver makes of an "X:..." search pattern (the image's letter), from
 * the image's current directory.
 */
static const CHAR *FcbFindTestResolve(const CHAR *pattern, const CHAR *current)
{
    static CHAR resolved[260];

    if (!pattern[0] || pattern[1] != ':')
        return "(no drive)";

    if (!FatImageResolvePath(current, pattern + 2, resolved, sizeof(resolved)))
        return "(unresolved)";

    return resolved;
}

INT main(VOID)
{
    CHAR small[3];

    printf("== dos_fcbfind.h (#240)\n");

    /* THE BUG: the current drive is the image A: (VirtualDrive 0), drive byte 0. */
    CHECK(strcmp(FcbFindTestPattern(0, 0), "A:*") == 0, "drive byte 0 on the image A: -> \"A:*\", not \"*\" (bin\\)");
    CHECK(DosFcbSearchDrive(0, 0) == 0, "drive byte 0 on A: searches drive 0 (A:)");

    /* A current drive Win32 stands on: the process directory is right. */
    CHECK(strcmp(FcbFindTestPattern(0, -1), "*") == 0, "drive byte 0 on a Win32 drive -> \"*\"");
    CHECK(DosFcbSearchDrive(0, 2) == 2, "drive byte 0 on C: searches drive 2 (C:)");

    /* An explicit drive byte wins over the current drive. */
    CHECK(strcmp(FcbFindTestPattern(1, -1), "A:*") == 0, "drive byte 1 -> \"A:*\"");
    CHECK(strcmp(FcbFindTestPattern(3, 0), "C:*") == 0, "drive byte 3 while on A: -> \"C:*\"");
    CHECK(DosFcbSearchDrive(4, 0) == 3, "drive byte 4 searches drive 3 (D:)");

    /* Another drive Win32 cannot stand on (an empty bay, D: with no disc). */
    CHECK(strcmp(FcbFindTestPattern(0, 3), "D:*") == 0, "drive byte 0 on a virtual D: -> \"D:*\"");

    /* A buffer too small for "X:*" is refused, not overrun. */
    CHECK(!DosFcbSearchPattern(0, 0, small, sizeof(small)), "a 3-byte buffer is refused");

    /* THROUGH THE IMAGE'S RESOLVER, as DosImageMapPath does: the root, then CD DEMOS. */
    CHECK(strcmp(FcbFindTestResolve(FcbFindTestPattern(0, 0), ""), "*") == 0, "A:\\ then DIR -> the image root's \"*\"");
    CHECK(strcmp(FcbFindTestResolve(FcbFindTestPattern(0, 0), "DEMOS"), "DEMOS\\*") == 0,
          "CD DEMOS then DIR -> \"DEMOS\\*\" in the image");
    CHECK(strcmp(FcbFindTestResolve(FcbFindTestPattern(1, 2), "DEMOS"), "DEMOS\\*") == 0,
          "DIR A: from C: -> A:'s current directory, \"DEMOS\\*\"");

    /* s101: THE IMAGE AT ANOTHER LETTER (F:, Settings' choice) -- the real A: stays A:. */
    CHECK(strcmp(FcbFindTestPattern(0, 5), "F:*") == 0, "drive byte 0 on the image F: -> \"F:*\"");
    CHECK(DosFcbSearchDrive(0, 5) == 5, "drive byte 0 on F: searches drive 5 (F:)");
    CHECK(strcmp(FcbFindTestPattern(1, 5), "A:*") == 0, "drive byte 1 while on the image F: -> the real \"A:*\"");
    CHECK(strcmp(FcbFindTestResolve(FcbFindTestPattern(0, 5), "DEMOS"), "DEMOS\\*") == 0,
          "F:, CD DEMOS, DIR -> \"DEMOS\\*\" in the image");
    CHECK(strcmp(FcbFindTestResolve(FcbFindTestPattern(6, 2), ""), "*") == 0,
          "DIR F: from C: -> the image root's \"*\"");

    printf("\n%d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}
