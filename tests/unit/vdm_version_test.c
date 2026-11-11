/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM battery for the RegisterConsoleVDM form decision (vdm_version.h). (s106)
 *
 * Four things differ between XP's VDM interface and Windows 7's (see vdm_version.h), and
 * each is wrong in the other direction: XP given Windows 7's answers breaks just as
 * surely. This pins which Windows gets which.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include "../../src/vdm/vdm_version.h"
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

static INT g_Total = 0;
static INT g_Failures = 0;

INT main(VOID)
{
    printf("csrss_version_test: which VDM interface each Windows has\n");

    CHECK(!VdmIsNt61OrLater(5, 1), "XP (5.1) keeps XP's");
    CHECK(!VdmIsNt61OrLater(5, 2), "Server 2003 (5.2) keeps XP's");
    CHECK(!VdmIsNt61OrLater(6, 0), "Vista (6.0), untested, keeps XP's");
    CHECK(VdmIsNt61OrLater(6, 1), "Windows 7 (6.1) has Windows 7's");
    CHECK(VdmIsNt61OrLater(6, 2), "Windows 8 (6.2) has Windows 7's");
    CHECK(VdmIsNt61OrLater(10, 0), "Windows 10 (10.0) has Windows 7's");

    printf("== %d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
