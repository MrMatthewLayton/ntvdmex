/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The startup-failure policy, pinned off-VM.  GH #132.
 *
 * The value of this policy is entirely in its edges: too eager and a machine
 * loses its VDM after one bad day, too lazy and a wedged host leaves every
 * 16-bit program on the box broken until someone edits the registry by hand.
 *
 *   cc -std=c99 -I src -I src/dos -o recovery_test tests/unit/recovery_test.c
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "dos_recovery.h"

#define RECOVERY_TEST_MANY_FAILURES     99  /* A count that has kept rising */
#define RECOVERY_TEST_SKIP_COUNT        6   /* The subsystems DOS_SAFE_SKIPS names */

static INT g_Checks;
static INT g_Failures;

static VOID RecoveryTestExpect(PCSTR description, LONG actual, LONG expected)
{
    ++g_Checks;

    if (actual == expected)
        return;

    ++g_Failures;
    printf("  FAIL %-58s got %ld, want %ld\n", description, (long)actual, (long)expected);
}

/* Parse a counter file's text, its length taken from the text itself. */
static UINT RecoveryTestParse(PCSTR text)
{
    return DosRecoveryParseFailureCount(text, (UINT)strlen(text));
}

/* How many subsystems a set of skips turns off. */
static LONG RecoveryTestSkipCount(DOS_SAFE_SKIPS skips)
{
    return skips.VddPlugins + skips.AudioOut + skips.RealSpeaker + skips.Joystick
         + skips.WowShims + skips.Fullscreen;
}

INT main(VOID)
{
    printf("== startup recovery policy (dos_recovery.h)\n");

    /* A healthy machine is the overwhelmingly common case and must be untouched. */
    RecoveryTestExpect("0 failures -> normal", DosRecoveryDecideStartMode(0), DOS_START_NORMAL);
    RecoveryTestExpect("1 failure  -> still normal (one bad run is not a pattern)",
                       DosRecoveryDecideStartMode(1), DOS_START_NORMAL);
    RecoveryTestExpect("2 failures -> SAFE MODE",
                       DosRecoveryDecideStartMode(DOS_RECOVERY_SAFE_MODE_FAILURES), DOS_START_SAFE);
    RecoveryTestExpect("3 failures -> UNINSTALL",
                       DosRecoveryDecideStartMode(DOS_RECOVERY_UNINSTALL_FAILURES),
                       DOS_START_UNINSTALL);
    /* [CAUTION]: AND IT MUST NOT COME BACK DOWN. A count that keeps rising has to stay
     * uninstalled -- an off-by-one that wrapped to NORMAL at 4 would put a
     * known-broken host back in the path.
     */
    RecoveryTestExpect("4 failures -> still UNINSTALL",
                       DosRecoveryDecideStartMode(DOS_RECOVERY_UNINSTALL_FAILURES + 1),
                       DOS_START_UNINSTALL);
    RecoveryTestExpect("99 failures -> still UNINSTALL",
                       DosRecoveryDecideStartMode(RECOVERY_TEST_MANY_FAILURES),
                       DOS_START_UNINSTALL);

    /* -- PARSING. A corrupt counter must not be able to uninstall us by itself. */
    RecoveryTestExpect("\"0\" parses as 0", RecoveryTestParse("0"), 0);
    RecoveryTestExpect("\"2\" parses as 2", RecoveryTestParse("2"), 2);
    RecoveryTestExpect("\"3\\r\\n\" parses as 3", RecoveryTestParse("3\r\n"), 3);
    RecoveryTestExpect("\" 7 \" parses as 7 (leading blanks skipped)", RecoveryTestParse(" 7 "), 7);
    RecoveryTestExpect("empty parses as 0", RecoveryTestParse(""), 0);
    RecoveryTestExpect("NULL parses as 0", DosRecoveryParseFailureCount(NULL, 0), 0);
    RecoveryTestExpect("\"garbage\" parses as 0, NOT as a failure",
                       RecoveryTestParse("garbage"), 0);
    RecoveryTestExpect("\"x3\" parses as 0 (leading junk, not a 3)", RecoveryTestParse("x3"), 0);
    /* An absurd count is far more likely to be a corrupt file than a machine
     * that really failed 99999 times, and it must not read as "uninstall".
     */
    RecoveryTestExpect("\"99999\" parses as 0 (absurd -> corrupt)",
                       RecoveryTestParse("99999"), 0);
    RecoveryTestExpect("\"12\" still parses as 12", RecoveryTestParse("12"), 12);

    /* The end-to-end shape: a corrupt file leaves us running normally. */
    RecoveryTestExpect("corrupt counter -> NORMAL, never uninstall",
                       DosRecoveryDecideStartMode(RecoveryTestParse("????")), DOS_START_NORMAL);

    /* s90: safe mode skips every optional subsystem; normal mode skips none. */
    {   DOS_SAFE_SKIPS normalSkips = DosRecoveryGetSafeSkips(DOS_START_NORMAL);
        DOS_SAFE_SKIPS safeSkips = DosRecoveryGetSafeSkips(DOS_START_SAFE);
        DOS_SAFE_SKIPS uninstallSkips = DosRecoveryGetSafeSkips(DOS_START_UNINSTALL);
        RecoveryTestExpect("NORMAL skips nothing", RecoveryTestSkipCount(normalSkips), 0);
        RecoveryTestExpect("SAFE skips all six", RecoveryTestSkipCount(safeSkips),
                           RECOVERY_TEST_SKIP_COUNT);
        RecoveryTestExpect("UNINSTALL is not safe mode (it hands the machine back instead)",
                           uninstallSkips.VddPlugins + uninstallSkips.AudioOut, 0);
        DOS_START_MODE twoFailureMode = DosRecoveryDecideStartMode(DOS_RECOVERY_SAFE_MODE_FAILURES);
        RecoveryTestExpect("two failures -> the skips are on",
                           DosRecoveryGetSafeSkips(twoFailureMode).AudioOut, TRUE);
        RecoveryTestExpect("one failure -> still normal",
                           DosRecoveryGetSafeSkips(DosRecoveryDecideStartMode(1)).AudioOut, FALSE);
    }

    printf("== %d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}
