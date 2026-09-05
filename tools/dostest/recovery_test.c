/* recovery_test.c -- the startup-failure policy, pinned off-VM.  GH #132.
 *
 * The value of this policy is entirely in its edges: too eager and a machine
 * loses its VDM after one bad day, too lazy and a wedged host leaves every
 * 16-bit program on the box broken until someone edits the registry by hand.
 *
 *   cc -std=c99 -I src/dos -o recovery_test tools/dostest/recovery_test.c
 */
#include <stdio.h>
#include <string.h>
#include "dos_recovery.h"

static int checks, fails;
static void eq(const char *what, long got, long want)
{
    ++checks;
    if (got == want) return;
    ++fails;
    printf("  FAIL %-58s got %ld, want %ld\n", what, got, want);
}

int main(void)
{
    printf("== startup recovery policy (dos_recovery.h)\n");

    /* A healthy machine is the overwhelmingly common case and must be untouched. */
    eq("0 failures -> normal", dos_recovery_decide(0), DOS_START_NORMAL);
    eq("1 failure  -> still normal (one bad run is not a pattern)",
       dos_recovery_decide(1), DOS_START_NORMAL);
    eq("2 failures -> SAFE MODE", dos_recovery_decide(2), DOS_START_SAFE);
    eq("3 failures -> UNINSTALL", dos_recovery_decide(3), DOS_START_UNINSTALL);
    /* ⚠ AND IT MUST NOT COME BACK DOWN. A count that keeps rising has to stay
       uninstalled -- an off-by-one that wrapped to NORMAL at 4 would put a
       known-broken host back in the path. */
    eq("4 failures -> still UNINSTALL", dos_recovery_decide(4), DOS_START_UNINSTALL);
    eq("99 failures -> still UNINSTALL", dos_recovery_decide(99), DOS_START_UNINSTALL);

    /* ── PARSING. A corrupt counter must not be able to uninstall us by itself. */
    eq("\"0\" parses as 0", dos_recovery_parse("0", 1), 0);
    eq("\"2\" parses as 2", dos_recovery_parse("2", 1), 2);
    eq("\"3\\r\\n\" parses as 3", dos_recovery_parse("3\r\n", 3), 3);
    eq("\" 7 \" parses as 7 (leading blanks skipped)", dos_recovery_parse(" 7 ", 3), 7);
    eq("empty parses as 0", dos_recovery_parse("", 0), 0);
    eq("NULL parses as 0", dos_recovery_parse(NULL, 0), 0);
    eq("\"garbage\" parses as 0, NOT as a failure",
       dos_recovery_parse("garbage", 7), 0);
    eq("\"x3\" parses as 0 (leading junk, not a 3)", dos_recovery_parse("x3", 2), 0);
    /* An absurd count is far more likely to be a corrupt file than a machine
       that really failed 99999 times, and it must not read as "uninstall". */
    eq("\"99999\" parses as 0 (absurd -> corrupt)",
       dos_recovery_parse("99999", 5), 0);
    eq("\"12\" still parses as 12", dos_recovery_parse("12", 2), 12);

    /* The end-to-end shape: a corrupt file leaves us running normally. */
    eq("corrupt counter -> NORMAL, never uninstall",
       dos_recovery_decide(dos_recovery_parse("????", 4)), DOS_START_NORMAL);

    printf("== %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
