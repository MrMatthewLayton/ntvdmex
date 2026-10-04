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

        ++checks;
        d = 0;
        if (!dos_err_from_win32(W32_FILE_EXISTS, &d)) { ++fails;
            printf("  FAIL %-56s unmapped\n", "win32 FILE_EXISTS"); }
        eq("6Ch exists+fail -> 0x50 (p_file int21.6C.exists)", d, 0x50);

        /* An unmapped code must NOT invent an answer: it keeps the old 2 and
           reports 0 so the handler can log UNMAPPED -- the same refusal
           dos_err_classify() makes for an unmeasured class. */
        ++checks;
        d = 0xDEAD;
        if (dos_err_from_win32(0x4D2, &d)) { ++fails;
            printf("  FAIL %-56s claimed to know it\n", "win32=1234 is unmapped"); }
        eq("unmapped keeps the historical 2 (no invention)", d, 0x02);
    }

    /* #34: the INT 24h contract, from tools/dostest/p_crit.asm (6.22 + PCem) */
    {
        unsigned short bx; unsigned char ch;
        int ok = dos_err_classify(0x53, &bx, &ch);
        eq("crit.4e.fail.59 is a measured row", ok, 1);
        eq("crit.4e.fail.59 BX=0D04", bx, 0x0D04);
        eq("crit.4e.fail.59 CH=01", ch, 0x01);
        eq("crit.4e.fail.int24 AH=1A (find-first)", dos_crit_ah(0x4E), 0x1A);
        eq("crit.3c.fail.int24 AH=1A (create: a READ of the FAT)", dos_crit_ah(0x3C), 0x1A);
        eq("ignore is NOT allowed on a path call (bit 5)", dos_crit_ah(0x3D) & 0x20, 0);
        eq("crit.4e.fail.call AX=0003", dos_crit_fail_ax(0x4E, 2), 0x0003);
        eq("crit.3d.fail.call AX=0003 (PCem)", dos_crit_fail_ax(0x3D, 2), 0x0003);
        eq("21 (not ready) is a hardware error", dos_crit_is_hw(21), 1);
        eq("18 (no more files) is not", dos_crit_is_hw(18), 0);
        {   unsigned short d = 0;
            eq("win32 21 -> DOS 21 (identity)", dos_err_from_win32(21, &d), 1);
            eq("...value", d, 21); }
    }

    /* #275: 3Fh/40h on an open handle. ⚠ SPEC-DERIVED (MS-DOS 4.0 kernel source, see
       dos_err.h), NOT YET MEASURED: p_crit2.asm asks 6.22 and PCem. When it has, the
       expectations below are replaced by its rows, not the other way round. */
    {
        unsigned char r = dos_crit_ah(0x3F), w = dos_crit_ah(0x40);
        eq("3Fh AH=3E (data area, read, F+R+I)", r, 0x3E);
        eq("40h AH=3F (data area, WRITE, F+R+I)", w, 0x3F);
        eq("3Fh read bit (0) clear", r & 1, 0);
        eq("40h write bit (0) set", w & 1, 1);
        eq("3Fh area (bits 1-2) = 3, data", (r >> 1) & 3, 3);
        eq("40h area (bits 1-2) = 3, data", (w >> 1) & 3, 3);
        eq("3Fh IGNORE allowed (bit 5)", r & DOS_CRIT_ALLOW_IGNORE, 0x20);
        eq("40h RETRY allowed (bit 4)", w & DOS_CRIT_ALLOW_RETRY, 0x10);
        eq("40h FAIL allowed (bit 3)", w & DOS_CRIT_ALLOW_FAIL, 0x08);
        eq("a disk, not a character device (bit 7)", (r | w) & 0x80, 0);
        eq("path calls unchanged by #275 (4Eh AH=1A)", dos_crit_ah(0x4E), 0x1A);
        eq("3Fh FAIL -> AX=0005 (SET_ACC_ERR)", dos_crit_fail_ax(0x3F, 2), 0x0005);
        eq("40h FAIL -> AX=0005", dos_crit_fail_ax(0x40, 0), 0x0005);
        eq("path FAIL still AX=0003", dos_crit_fail_ax(0x3C, 2), 0x0003);
        eq("31 (general failure) is a hardware error", dos_crit_is_hw(31), 1);
        eq("32 (sharing violation) is NOT", dos_crit_is_hw(32), 0);
        eq("5 (access denied) is NOT", dos_crit_is_hw(5), 0);
        /* IGNORE: the call reports what was asked; a read stops at end of file */
        eq("ignore write: the request", dos_crit_ignore_count(0x40, 0x200, 0, 0, 1), 0x200);
        eq("ignore read mid-file: the request", dos_crit_ignore_count(0x3F, 0x200, 0, 0x1000, 1), 0x200);
        eq("ignore read near EOF: what is left", dos_crit_ignore_count(0x3F, 0x200, 0xF80, 0x1000, 1), 0x80);
        eq("ignore read at/after EOF: 0", dos_crit_ignore_count(0x3F, 0x200, 0x1200, 0x1000, 1), 0);
        eq("ignore read, size unknown: the request", dos_crit_ignore_count(0x3F, 0x200, 0, 0, 0), 0x200);
        /* AL: the open file's drive, from NT names */
        {
            const char *dev[26] = { 0 };
            dev[0] = "\\Device\\Floppy0";
            dev[2] = "\\Device\\HarddiskVolume1";
            dev[3] = "\\Device\\HarddiskVolume10";
            dev[4] = "\\Device\\CdRom0";
            eq("floppy file -> A:", dos_crit_drive_from_ntname("\\Device\\Floppy0\\X.TXT", dev), 0);
            eq("case-insensitive", dos_crit_drive_from_ntname("\\DEVICE\\floppy0\\X.TXT", dev), 0);
            eq("HarddiskVolume1 is not a prefix of ...Volume10 (whole component)",
               dos_crit_drive_from_ntname("\\Device\\HarddiskVolume10\\A\\B", dev), 3);
            eq("HarddiskVolume1 file -> C:", dos_crit_drive_from_ntname("\\Device\\HarddiskVolume1\\A", dev), 2);
            eq("CD root -> E:", dos_crit_drive_from_ntname("\\Device\\CdRom0", dev), 4);
            eq("network name: no match (-1)", dos_crit_drive_from_ntname("\\Device\\LanmanRedirector\\srv\\x", dev), -1);
            eq("NULL name: -1", dos_crit_drive_from_ntname(NULL, dev), -1);
        }
    }

    printf("== %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
