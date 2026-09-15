/* err_test.c -- INT 21h AH=59h's class/action/locus table, pinned off-VM.  GH #34.
 *
 * Every expectation is a CASE= line from tools/dostest/p_err.asm run on the
 * genuine MS-DOS 6.22 oracle, quoted in the check's name. Nothing here is
 * written from memory of what DOS returns -- that is the cardinal rule of epic
 * #24, and this table is precisely the kind of value it exists to protect:
 * plausible-looking and wrong is indistinguishable from right until a program
 * branches on it.
 *
 *   cc -std=c99 -I src/dos -o err_test tools/dostest/err_test.c && ./err_test
 */
#include <stdio.h>
#include "dos_err.h"

static int checks, fails;

static void eq(const char *what, long got, long want)
{
    ++checks;
    if (got == want) return;
    ++fails;
    printf("  FAIL %-56s got 0x%04lX, want 0x%04lX\n", what, got, want);
}

/* One measured row: code -> BX (class:action) and CH (locus). */
static void row(const char *what, unsigned code, unsigned bx, unsigned ch)
{
    unsigned short gotbx; unsigned char gotch;
    char label[128];
    int ok = dos_err_classify((unsigned short)code, &gotbx, &gotch);

    snprintf(label, sizeof(label), "%s -> BX", what);
    ++checks;
    if (!ok) { ++fails; printf("  FAIL %-56s reported UNMEASURED\n", label); return; }
    --checks;                                   /* eq() below counts it */
    eq(label, gotbx, bx);
    snprintf(label, sizeof(label), "%s -> CH", what);
    eq(label, gotch, ch);
}

int main(void)
{
    unsigned short bx; unsigned char ch;

    printf("== INT 21h AH=59h error classification (dos_err.h), measured on 6.22\n");

    /* ── THE NOT-FOUND FAMILY. Three different codes, one classification.
       CASE=err.after.3D.missing AX=0002 BX=0803 CX=02C1
       CASE=err.after.4E.nopath  AX=0003 BX=0803 CX=02C1
       CASE=err.after.4E.nofile  AX=0012 BX=0803 CX=02C1 */
    row("code 2  file not found  [3D on a missing file]", 0x02, 0x0803, 0x02);
    row("code 3  path not found  [4E into a missing dir]", 0x03, 0x0803, 0x02);
    row("code 18 no more files   [4E matching nothing]", 0x12, 0x0803, 0x02);

    /* CASE=err.after.3F.badhandle AX=0006 BX=0704 CX=01C1
       The odd one out on BOTH fields, which is why it is worth a check of its
       own: class 07 (bad request), action 04 (abort), locus 01 (block device)
       where the not-found family is 08/03 locus 02. */
    row("code 6  invalid handle  [3Fh on handle 20]", 0x06, 0x0704, 0x01);

    /* ── MEASURED IN SESSION 52. Both of these returned zeroes and logged
       UNMEASURED before p_err.asm learned to provoke them.
       CASE=err.after.3D.readonly AX=0005 BX=0303 CX=02C1
       CASE=err.after.5B.exists   AX=0050 BX=0C03 CX=02C1
       ★ Note code 5's action is 03 (retry after USER intervention -- take the
       read-only flag off), not 04 (abort). A guess would very likely have said
       abort, and a program that aborts where DOS says "ask the user" is exactly
       the silent wrong branch this table exists to prevent. */
    row("code 5  access denied   [3D write on a read-only file]", 0x05, 0x0303, 0x02);
    row("code 80 file exists     [5Bh over an existing file]", 0x50, 0x0C03, 0x02);
    /* CASE=err.after.47.baddrive AX=000F BX=0803 CX=02C1
       ⚠ Provoked through AH=47h, NOT through an open: "Y:\..." to 3Dh returns 3
       (path not found). A code can need a particular door, and picking the wrong
       one is how it stays "unprovokable" and unmeasured. */
    row("code 15 invalid drive    [47h on a drive with nothing behind it]",
        0x0F, 0x0803, 0x02);

    /* ── CODE 0 IS NOT AN ERROR. 59h after a successful call reports AX=0 with
       class and locus zero, so it must be classified (return 1), not reported
       as an unmeasured gap. */
    ++checks;
    if (!dos_err_classify(0, &bx, &ch)) {
        ++fails; printf("  FAIL %-56s reported UNMEASURED\n", "code 0 is not an error");
    }
    eq("code 0 -> BX is zero", bx, 0);
    eq("code 0 -> CH is zero", ch, 0);

    /* ── AN UNMEASURED CODE MUST SAY SO AND ZERO THE FIELDS. This is the check
       that keeps the table honest: the moment it starts inventing a plausible
       class for anything it has not seen, it stops being evidence. Code 0x21
       (lock violation) is real DOS but has never been provoked here. */
    ++checks;
    bx = 0xDEAD; ch = 0xEE;
    if (dos_err_classify(0x21, &bx, &ch)) {
        ++fails;
        printf("  FAIL %-56s claimed to know it\n", "code 0x21 is UNMEASURED");
    }
    eq("unmeasured code zeroes BX (never a guess)", bx, 0);
    eq("unmeasured code zeroes CH (never a guess)", ch, 0);

    /* Every row must carry the oracle case it came from -- an evidence string is
       not decoration here, it is how the next person re-runs the measurement. */
    {
        unsigned i;
        for (i = 0; i < DOS_ERR_ROWS; ++i) {
            ++checks;
            if (dos_err_table[i].evidence && dos_err_table[i].evidence[0]) continue;
            ++fails;
            printf("  FAIL row %u (code 0x%02X) has no oracle evidence string\n",
                   i, dos_err_table[i].code);
        }
    }

    /* ── WIN32 -> DOS, the mapping AH=3Dh used to skip entirely. (s72) ────────
         Every expectation below is a pair of measured lines: the oracle's answer
         for the situation, and the `win32=0x..` the rig's own handler logged for
         the same probe case. Before this existed AH=3Dh answered 2 for every
         cause, so a read-only file read as "not found". */
    {
        unsigned short d;
        printf("== INT 21h AH=3Dh: Win32 failure -> DOS code (dos_err_from_win32)\n");

        ++checks;
        if (!dos_err_from_win32(W32_FILE_NOT_FOUND, &d)) { ++fails;
            printf("  FAIL %-56s not mapped\n", "win32=2 is a measured row"); }
        eq("win32=2  -> 2   [err.after.3D.missing  AX=0002]", d, 0x02);

        ++checks;
        if (!dos_err_from_win32(W32_PATH_NOT_FOUND, &d)) { ++fails;
            printf("  FAIL %-56s not mapped\n", "win32=3 is a measured row"); }
        eq("win32=3  -> 3   [err.after.3D.baddrive AX=0003]", d, 0x03);

        ++checks;
        if (!dos_err_from_win32(W32_ACCESS_DENIED, &d)) { ++fails;
            printf("  FAIL %-56s not mapped\n", "win32=5 is a measured row"); }
        eq("win32=5  -> 5   [err.after.3D.readonly AX=0005]", d, 0x05);

        /* ⚠ THE POINT OF THE WHOLE EXERCISE: these three must be DISTINCT. The
             bug was not a wrong constant, it was three causes collapsing to one
             answer, and a table that mapped them all to 5 would pass any test
             that only checked "not 2". */
        {
            unsigned short a, b, c;
            dos_err_from_win32(W32_FILE_NOT_FOUND, &a);
            dos_err_from_win32(W32_PATH_NOT_FOUND, &b);
            dos_err_from_win32(W32_ACCESS_DENIED,  &c);
            ++checks;
            if (a == b || b == c || a == c) { ++fails;
                printf("  FAIL %-56s %u/%u/%u\n",
                       "not-found / path / denied must stay distinct", a, b, c); }
        }

        /* An unmapped code must NOT invent an answer: it keeps the old 2 and
           reports 0 so the handler can log UNMAPPED -- the same refusal
           dos_err_classify() makes for an unmeasured class. */
        ++checks;
        d = 0xDEAD;
        if (dos_err_from_win32(0x4D2, &d)) { ++fails;
            printf("  FAIL %-56s claimed to know it\n", "win32=1234 is unmapped"); }
        eq("unmapped keeps the historical 2 (no invention)", d, 0x02);
    }

    printf("== %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
