/* install_test.c -- off-VM battery for the install/uninstall decision (install.h).
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
 */
#include <stdio.h>
#include <string.h>
#include "install.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

int main(void)
{
    static const char *SELF = "C:\\ntvdmex\\ntvdmhost.exe";

    printf("== install: is that value ours? (install.h) ==\n");

    CHECK(install_classify(NULL, SELF) == INSTALL_ABSENT,
          "no value at all is ABSENT -- the machine uses its own ntvdm");
    CHECK(install_classify("", SELF) == INSTALL_ABSENT,
          "an EMPTY value is absent too, not an install pointing at nothing");
    CHECK(install_classify("   ", SELF) == INSTALL_ABSENT,
          "...and neither is whitespace");

    CHECK(install_classify("C:\\ntvdmex\\ntvdmhost.exe", SELF) == INSTALL_OURS,
          "the exact path is OURS");

    /* ⚠ THE FOUR WAYS A HAND-WRITTEN VALUE DIFFERS FROM OURS. Every one of these is
         what a person actually types, and getting any of them wrong means uninstall
         refuses to remove a value we put there. */
    CHECK(install_classify("\"C:\\ntvdmex\\ntvdmhost.exe\"", SELF) == INSTALL_OURS,
          "QUOTED is ours -- reg add with a quoted path is the documented form");
    CHECK(install_classify("C:\\NTVDMEX\\NTVDMHOST.EXE", SELF) == INSTALL_OURS,
          "CASE does not matter: Windows paths are case-insensitive");
    CHECK(install_classify("  C:\\ntvdmex\\ntvdmhost.exe  ", SELF) == INSTALL_OURS,
          "SURROUNDING SPACE does not matter -- a typed value usually has some");
    CHECK(install_classify("C:/ntvdmex/ntvdmhost.exe", SELF) == INSTALL_OURS,
          "FORWARD SLASHES name the same file, and Win32 accepts them");

    CHECK(install_classify("C:\\other\\debugger.exe", SELF) == INSTALL_OTHER,
          "a different program is OTHER, and is not ours to touch");
    CHECK(install_classify("C:\\ntvdmex\\ntvdmhost.exe.bak", SELF) == INSTALL_OTHER,
          "a path that merely STARTS with ours is not ours -- compare whole, not prefix");
    CHECK(install_classify("C:\\ntvdmex\\ntvdmhos.exe", SELF) == INSTALL_OTHER,
          "...and a shorter near-miss is not ours either");

    printf("== install: what to do about it ==\n");

    CHECK(install_plan(INSTALL_ABSENT, 1, 0) == INSTALL_ACT_WRITE,
          "install on a clean machine writes our path");
    CHECK(install_plan(INSTALL_OURS, 1, 0) == INSTALL_ACT_NOTHING,
          "install when already ours does nothing, and says so rather than rewriting");
    CHECK(install_plan(INSTALL_OTHER, 1, 0) == INSTALL_ACT_WRITE,
          "install over somebody else's value writes -- but see the restore case");

    CHECK(install_plan(INSTALL_ABSENT, 0, 0) == INSTALL_ACT_NOTHING,
          "uninstall on a clean machine is already done, not an error");
    CHECK(install_plan(INSTALL_OURS, 0, 0) == INSTALL_ACT_DELETE,
          "uninstall with nothing displaced deletes the value");
    CHECK(install_plan(INSTALL_OURS, 0, 1) == INSTALL_ACT_RESTORE,
          "uninstall RESTORES whatever we displaced -- that is what reversible means");

    /* ⚠ THE ONE THAT PROTECTS SOMEBODY ELSE'S MACHINE. `Debugger` is a general
         Windows facility and something else may be using it. Deleting a value we
         did not write would break that tool and leave no record of what it was. */
    CHECK(install_plan(INSTALL_OTHER, 0, 0) == INSTALL_ACT_REFUSE,
          "uninstall REFUSES to delete a Debugger value that is not ours");
    CHECK(install_plan(INSTALL_OTHER, 0, 1) == INSTALL_ACT_REFUSE,
          "...even when we have a saved value, because that one is not the one there");

    printf("== install: the key and value names ==\n");

    /* A typo in either half is an install that appears to succeed and routes
       nothing -- the exact failure the rig has hit from a hand-typed reg add. */
    CHECK(strcmp(INSTALL_KEY,
          "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution "
          "Options\\ntvdm.exe") == 0,
          "the IFEO key path is exactly the one Windows reads");
    CHECK(strcmp(INSTALL_VAL, "Debugger") == 0, "the value is named Debugger");

    printf("\n%s: %d/%d\n", fails ? "FAILURES" : "ALL PASS", total - fails, total);
    return fails ? 1 : 0;
}
