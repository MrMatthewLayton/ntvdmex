/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM battery for the RegisterConsoleVDM form decision (csrss_version.h). (s106)
 *
 * Windows 7's RegisterConsoleVDM takes nine arguments where XP's takes eleven, and
 * the call is stdcall: the wrong form leaves the host's stack unbalanced and it dies
 * on its first return. On the Win7-32 rig it did exactly that. XP must keep eleven.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include "../../src/vdm/csrss_version.h"
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

static INT g_Total = 0;
static INT g_Failures = 0;

INT main(VOID)
{
    printf("csrss_version_test: which RegisterConsoleVDM form each Windows takes\n");

    CHECK(!CsrssUsesNineArgumentRegister(5, 1), "XP (5.1) takes eleven");
    CHECK(!CsrssUsesNineArgumentRegister(5, 2), "Server 2003 (5.2) takes eleven");
    CHECK(!CsrssUsesNineArgumentRegister(6, 0), "Vista (6.0), untested, keeps eleven");
    CHECK(CsrssUsesNineArgumentRegister(6, 1), "Windows 7 (6.1) takes nine");
    CHECK(CsrssUsesNineArgumentRegister(6, 2), "Windows 8 (6.2) takes nine");
    CHECK(CsrssUsesNineArgumentRegister(10, 0), "Windows 10 (10.0) takes nine");

    printf("== %d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
