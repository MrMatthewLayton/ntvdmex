/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * INT 2Fh AX=1213h, DOS's "upper-case a character", pinned off-VM. COMMAND.COM's critical-
 * error prompt compares the key it gets back with A/R/I/F; unanswered, "Abort, Retry,
 * Fail?" had no way out (s101 rig report). DosCtabMultiplexUppercaseAx, src/dos/dos_ctab.h.
 *
 *   cc -std=c99 -I src -I src/dos -o ctab_test tests/unit/ctab_test.c
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include "dos_ctab.h"

#define CHECK(condition, message) do { ++g_Checks; if (condition) printf("  PASS  %s\n", (message)); \
    else { printf("  FAIL  %s\n", (message)); ++g_Failures; } } while (0)

static INT g_Checks;
static INT g_Failures;

static VOID CtabTestMultiplexUppercase(VOID)
{
    printf("INT 2Fh AX=1213h\n");
    CHECK(DosCtabMultiplexUppercaseAx(0x1213, 'a') == 0x1241, "'a' -> AL='A', AH kept");
    CHECK(DosCtabMultiplexUppercaseAx(0x1213, 'r') == 0x1252, "'r' -> 'R' (Retry)");
    CHECK(DosCtabMultiplexUppercaseAx(0x1213, 'f') == 0x1246, "'f' -> 'F' (Fail)");
    CHECK(DosCtabMultiplexUppercaseAx(0x1213, 'i') == 0x1249, "'i' -> 'I' (Ignore)");
    CHECK(DosCtabMultiplexUppercaseAx(0x1213, 'A') == 0x1241, "'A' stays 'A'");
    CHECK(DosCtabMultiplexUppercaseAx(0x1213, '1') == 0x1231, "a digit is kept");
    CHECK(DosCtabMultiplexUppercaseAx(0x1213, 0x81) == 0x129A, "81h -> 9Ah through the 6.22 table");
    CHECK(DosCtabMultiplexUppercaseAx(0x1213, 0x7A61) == 0x1241, "only the low byte of the pushed word counts");
    CHECK(DosCtabMultiplexUppercaseAx(0x1213, 'a') != 0x1213, "AL is answered -- never left as 13h");
}

int main(VOID)
{
    CtabTestMultiplexUppercase();
    printf("%d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}
