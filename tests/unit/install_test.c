/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM battery for the install/uninstall decision (install.h).
 *
 * GH #13, #130.
 *
 * The registry calls need a machine. WHAT STATE WE ARE IN, and what to do about it,
 * is string comparison and a small state machine -- and every one of its failure
 * modes is silent and expensive on a real box:
 *   - report "already installed" on a machine that is not, so the user thinks they
 *     are routed to NTVDMEX and every later test measures stock ntvdm;
 *   - refuse to remove our OWN value because the stored path was quoted and ours
 *     was not, leaving a box nobody can hand back to Microsoft;
 *   - delete somebody else's Debugger value, breaking a tool we never installed
 *     and cannot name afterwards.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "install.h"
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

static INT g_Total = 0;
static INT g_Failures = 0;

INT main(VOID)
{
    static PCSTR hostPath = "C:\\ntvdmex\\ntvdmhost.exe";

    printf("== install: is that value ours? (install.h) ==\n");

    CHECK(InstallClassify(NULL, hostPath) == INSTALL_ABSENT,
          "no value at all is ABSENT -- the machine uses its own ntvdm");
    CHECK(InstallClassify("", hostPath) == INSTALL_ABSENT,
          "an EMPTY value is absent too, not an install pointing at nothing");
    CHECK(InstallClassify("   ", hostPath) == INSTALL_ABSENT,
          "...and neither is whitespace");

    CHECK(InstallClassify("C:\\ntvdmex\\ntvdmhost.exe", hostPath) == INSTALL_OURS,
          "the exact path is OURS");

    /* [CAUTION]: THE FOUR WAYS A HAND-WRITTEN VALUE DIFFERS FROM OURS. Every one of these is
     * what a person actually types, and getting any of them wrong means uninstall
     * refuses to remove a value we put there.
     */
    CHECK(InstallClassify("\"C:\\ntvdmex\\ntvdmhost.exe\"", hostPath) == INSTALL_OURS,
          "QUOTED is ours -- reg add with a quoted path is the documented form");
    CHECK(InstallClassify("C:\\NTVDMEX\\NTVDMHOST.EXE", hostPath) == INSTALL_OURS,
          "CASE does not matter: Windows paths are case-insensitive");
    CHECK(InstallClassify("  C:\\ntvdmex\\ntvdmhost.exe  ", hostPath) == INSTALL_OURS,
          "SURROUNDING SPACE does not matter -- a typed value usually has some");
    CHECK(InstallClassify("C:/ntvdmex/ntvdmhost.exe", hostPath) == INSTALL_OURS,
          "FORWARD SLASHES name the same file, and Win32 accepts them");

    CHECK(InstallClassify("C:\\other\\debugger.exe", hostPath) == INSTALL_OTHER,
          "a different program is OTHER, and is not ours to touch");
    CHECK(InstallClassify("C:\\ntvdmex\\ntvdmhost.exe.bak", hostPath) == INSTALL_OTHER,
          "a path that merely STARTS with ours is not ours -- compare whole, not prefix");
    CHECK(InstallClassify("C:\\ntvdmex\\ntvdmhos.exe", hostPath) == INSTALL_OTHER,
          "...and a shorter near-miss is not ours either");

    printf("== install: what to do about it ==\n");

    CHECK(InstallPlan(INSTALL_ABSENT, 1, 0) == INSTALL_ACT_WRITE,
          "install on a clean machine writes our path");
    CHECK(InstallPlan(INSTALL_OURS, 1, 0) == INSTALL_ACT_NOTHING,
          "install when already ours does nothing, and says so rather than rewriting");
    CHECK(InstallPlan(INSTALL_OTHER, 1, 0) == INSTALL_ACT_WRITE,
          "install over somebody else's value writes -- but see the restore case");

    CHECK(InstallPlan(INSTALL_ABSENT, 0, 0) == INSTALL_ACT_NOTHING,
          "uninstall on a clean machine is already done, not an error");
    /* #195: another COPY of NTVDMEX is ours to uninstall; a stranger only with /force. */
    CHECK(InstallNamesNtvdmex("\"C:\\zip\\ntvdmex\\bin\\ntvdmhost.exe\""), "names_ntvdmex: quoted path");
    CHECK(InstallNamesNtvdmex("C:\\X\\NTVDMHOST.EXE"), "names_ntvdmex: unquoted, upper case");
    CHECK(!InstallNamesNtvdmex("C:\\tools\\myntvdmhost.exe"), "names_ntvdmex: suffix match is not a name");
    CHECK(!InstallNamesNtvdmex("C:\\dbg\\windbg.exe"), "names_ntvdmex: a stranger");
    CHECK(InstallPlanEx(INSTALL_OTHER, 0, 0, 1, 0) == INSTALL_ACT_DELETE, "plan_ex: uninstall another NTVDMEX copy = DELETE");
    CHECK(InstallPlanEx(INSTALL_OTHER, 0, 1, 1, 0) == INSTALL_ACT_RESTORE, "plan_ex: ...with a saved value = RESTORE");
    CHECK(InstallPlanEx(INSTALL_OTHER, 0, 0, 0, 0) == INSTALL_ACT_REFUSE, "plan_ex: a stranger without /force = REFUSE");
    CHECK(InstallPlanEx(INSTALL_OTHER, 0, 0, 0, 1) == INSTALL_ACT_DELETE, "plan_ex: a stranger with /force = DELETE");
    CHECK(InstallPlanEx(INSTALL_OTHER, 1, 0, 1, 1) == INSTALL_ACT_WRITE, "plan_ex: install is unaffected by the new inputs");
    CHECK(InstallPlan(INSTALL_OURS, 0, 0) == INSTALL_ACT_DELETE,
          "uninstall with nothing displaced deletes the value");
    CHECK(InstallPlan(INSTALL_OURS, 0, 1) == INSTALL_ACT_RESTORE,
          "uninstall RESTORES whatever we displaced -- that is what reversible means");

    /* [CAUTION]: THE ONE THAT PROTECTS SOMEBODY ELSE'S MACHINE. `Debugger` is a general
     * Windows facility and something else may be using it. Deleting a value we
     * did not write would break that tool and leave no record of what it was.
     */
    CHECK(InstallPlan(INSTALL_OTHER, 0, 0) == INSTALL_ACT_REFUSE,
          "uninstall REFUSES to delete a Debugger value that is not ours");
    CHECK(InstallPlan(INSTALL_OTHER, 0, 1) == INSTALL_ACT_REFUSE,
          "...even when we have a saved value, because that one is not the one there");

    printf("== install: the key and value names ==\n");

    /* A typo in either half is an install that appears to succeed and routes
     * nothing -- the exact failure the test machine has hit from a hand-typed reg add.
     */
    CHECK(strcmp(INSTALL_KEY,
          "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution "
          "Options\\ntvdm.exe") == 0,
          "the IFEO key path is exactly the one Windows reads");
    CHECK(strcmp(INSTALL_VAL, "Debugger") == 0, "the value is named Debugger");

    printf("\n%s: %d/%d\n", g_Failures ? "FAILURES" : "ALL PASS", g_Total - g_Failures, g_Total);
    /* The runner (scripts/offvm.sh) reads this dialect; without it the battery counted
     * this whole test as 0 checks, which reads as a pass that asserted nothing. (s81)
     */
    printf("== %d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
