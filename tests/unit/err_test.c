/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * INT 21h AH=59h's class/action/locus table, pinned off-VM.  GH #34.
 *
 * Every expectation is a CASE= line from tests/probes/dos/p_err.asm run on the
 * genuine MS-DOS 6.22 oracle, quoted in the check's name. Nothing here is
 * written from memory of what DOS returns -- that is the cardinal rule of epic
 * #24, and this table is precisely the kind of value it exists to protect:
 * plausible-looking and wrong is indistinguishable from right until a program
 * branches on it.
 *
 *   cc -std=c99 -I src/dos -o err_test tests/unit/err_test.c && ./err_test
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include "dos_err.h"

/* The DOS error codes the checks provoke (AX). */
#define ERR_TEST_FILE_NOT_FOUND         0x02
#define ERR_TEST_PATH_NOT_FOUND         0x03
#define ERR_TEST_ACCESS_DENIED          0x05
#define ERR_TEST_INVALID_HANDLE         0x06
#define ERR_TEST_INVALID_DRIVE          0x0F
#define ERR_TEST_NO_MORE_FILES          0x12
#define ERR_TEST_NOT_READY              21
#define ERR_TEST_GENERAL_FAILURE        31
#define ERR_TEST_SHARING_VIOLATION      32
#define ERR_TEST_FILE_EXISTS            0x50
#define ERR_TEST_FAIL_I24               0x53
#define ERR_TEST_UNMEASURED_CODE        0x21    /* Lock violation: real DOS, never provoked */
#define ERR_TEST_UNMAPPED_WIN32         0x4D2   /* 1234 */

/* What the oracle answered: BX = class:action, CH = the locus. */
#define ERR_TEST_BX_NOT_FOUND           0x0803
#define ERR_TEST_BX_INVALID_HANDLE      0x0704
#define ERR_TEST_BX_ACCESS_DENIED       0x0303
#define ERR_TEST_BX_FILE_EXISTS         0x0C03
#define ERR_TEST_BX_FAIL_I24            0x0D04
#define ERR_TEST_CH_BLOCK_DEVICE        0x02
#define ERR_TEST_CH_UNKNOWN             0x01

/* Poison: what the out-parameters hold before a call that must overwrite them. */
#define ERR_TEST_POISON_WORD            0xDEAD
#define ERR_TEST_POISON_BYTE            0xEE

/* INT 21h functions, and the INT 24h AH each one gets. */
#define ERR_TEST_CREATE                 0x3C
#define ERR_TEST_OPEN                   0x3D
#define ERR_TEST_READ                   0x3F
#define ERR_TEST_WRITE                  0x40
#define ERR_TEST_FIND_FIRST             0x4E
#define ERR_TEST_AH_PATH_CALL           0x1A
#define ERR_TEST_AH_READ                0x3E
#define ERR_TEST_AH_WRITE               0x3F
#define ERR_TEST_AH_WRITE_BIT           1
#define ERR_TEST_AH_AREA_SHIFT          1
#define ERR_TEST_AH_AREA_MASK           3
#define ERR_TEST_AH_AREA_DATA           3
#define ERR_TEST_AH_ALLOW_FAIL          0x08
#define ERR_TEST_AH_ALLOW_RETRY         0x10
#define ERR_TEST_AH_ALLOW_IGNORE        0x20
#define ERR_TEST_AH_CHARACTER_DEVICE    0x80
#define ERR_TEST_NOT_READY_INDEX        2       /* DI's low byte: code - 19 */
#define ERR_TEST_AX_PATH_NOT_FOUND      0x0003
#define ERR_TEST_AX_ACCESS_DENIED       0x0005

/* An IGNOREd transfer: the request, a file, and where in it. */
#define ERR_TEST_REQUEST                0x200
#define ERR_TEST_FILE_SIZE              0x1000
#define ERR_TEST_NEAR_EOF               0xF80
#define ERR_TEST_LEFT_NEAR_EOF          0x80
#define ERR_TEST_PAST_EOF               0x1200

/* Drive letters, as DosCritDriveFromNtName counts them. */
#define ERR_TEST_DRIVE_A                0
#define ERR_TEST_DRIVE_C                2
#define ERR_TEST_DRIVE_D                3
#define ERR_TEST_DRIVE_E                4

#define ERR_TEST_LABEL_SIZE             128

static INT g_Checks;
static INT g_Failures;

static VOID ErrTestExpect(PCSTR description, INT32 actual, INT32 expected)
{
    ++g_Checks;

    if (actual == expected)
        return;

    ++g_Failures;
    printf("  FAIL %-56s got 0x%04lX, want 0x%04lX\n", description, (long)actual, (long)expected);
}

/* One measured row: code -> BX (class:action) and CH (locus). */
static VOID ErrTestRow(PCSTR description, UINT code, UINT expectedBx, UINT expectedCh)
{
    WORD classAndAction;
    BYTE locus;
    CHAR label[ERR_TEST_LABEL_SIZE];
    BOOL isMeasured = DosErrClassify((WORD)code, &classAndAction, &locus);

    snprintf(label, sizeof(label), "%s -> BX", description);
    ++g_Checks;

    if (!isMeasured)
    {
        ++g_Failures;
        printf("  FAIL %-56s reported UNMEASURED\n", label);
        return;
    }

    --g_Checks;                                 /* ErrTestExpect() below counts it */
    ErrTestExpect(label, classAndAction, expectedBx);
    snprintf(label, sizeof(label), "%s -> CH", description);
    ErrTestExpect(label, locus, expectedCh);
}

INT main(VOID)
{
    WORD classAndAction;
    BYTE locus;

    printf("== INT 21h AH=59h error classification (dos_err.h), measured on 6.22\n");

    /* THE NOT-FOUND FAMILY. Three different codes, one classification.
     * CASE=err.after.3D.missing AX=0002 BX=0803 CX=02C1
     * CASE=err.after.4E.nopath  AX=0003 BX=0803 CX=02C1
     * CASE=err.after.4E.nofile  AX=0012 BX=0803 CX=02C1
     */
    ErrTestRow("code 2  file not found  [3D on a missing file]", ERR_TEST_FILE_NOT_FOUND,
               ERR_TEST_BX_NOT_FOUND, ERR_TEST_CH_BLOCK_DEVICE);
    ErrTestRow("code 3  path not found  [4E into a missing dir]", ERR_TEST_PATH_NOT_FOUND,
               ERR_TEST_BX_NOT_FOUND, ERR_TEST_CH_BLOCK_DEVICE);
    ErrTestRow("code 18 no more files   [4E matching nothing]", ERR_TEST_NO_MORE_FILES,
               ERR_TEST_BX_NOT_FOUND, ERR_TEST_CH_BLOCK_DEVICE);

    /* CASE=err.after.3F.badhandle AX=0006 BX=0704 CX=01C1
     * The odd one out on BOTH fields, which is why it is worth a check of its
     * own: class 07 (bad request), action 04 (abort), locus 01 (unknown)
     * where the not-found family is 08/03 locus 02.
     */
    ErrTestRow("code 6  invalid handle  [3Fh on handle 20]", ERR_TEST_INVALID_HANDLE,
               ERR_TEST_BX_INVALID_HANDLE, ERR_TEST_CH_UNKNOWN);

    /* MEASURED IN SESSION 52. Both of these returned zeroes and logged
     * UNMEASURED before p_err.asm learned to provoke them.
     * CASE=err.after.3D.readonly AX=0005 BX=0303 CX=02C1
     * CASE=err.after.5B.exists   AX=0050 BX=0C03 CX=02C1
     *
     * [INFO]: Note code 5's action is 03 (retry after USER intervention -- take the
     * read-only flag off), not 04 (abort). A guess would very likely have said
     * abort, and a program that aborts where DOS says "ask the user" is exactly
     * the silent wrong branch this table exists to prevent.
     */
    ErrTestRow("code 5  access denied   [3D write on a read-only file]", ERR_TEST_ACCESS_DENIED,
               ERR_TEST_BX_ACCESS_DENIED, ERR_TEST_CH_BLOCK_DEVICE);
    ErrTestRow("code 80 file exists     [5Bh over an existing file]", ERR_TEST_FILE_EXISTS,
               ERR_TEST_BX_FILE_EXISTS, ERR_TEST_CH_BLOCK_DEVICE);
    /* CASE=err.after.47.baddrive AX=000F BX=0803 CX=02C1
     *
     * [CAUTION]: Provoked through AH=47h, NOT through an open: "Y:\..." to 3Dh returns 3
     * (path not found). A code can need a particular door, and picking the wrong
     * one is how it stays "unprovokable" and unmeasured.
     */
    ErrTestRow("code 15 invalid drive    [47h on a drive with nothing behind it]",
               ERR_TEST_INVALID_DRIVE, ERR_TEST_BX_NOT_FOUND, ERR_TEST_CH_BLOCK_DEVICE);

    /* CODE 0 IS NOT AN ERROR. 59h after a successful call reports AX=0 with
     * class and locus zero, so it must be classified (return TRUE), not reported
     * as an unmeasured gap.
     */
    ++g_Checks;

    if (!DosErrClassify(DOS_ERR_NONE, &classAndAction, &locus))
    {
        ++g_Failures;
        printf("  FAIL %-56s reported UNMEASURED\n", "code 0 is not an error");
    }

    ErrTestExpect("code 0 -> BX is zero", classAndAction, 0);
    ErrTestExpect("code 0 -> CH is zero", locus, 0);

    /* AN UNMEASURED CODE MUST SAY SO AND ZERO THE FIELDS. This is the check
     * that keeps the table honest: the moment it starts inventing a plausible
     * class for anything it has not seen, it stops being evidence. Code 0x21
     * (lock violation) is real DOS but has never been provoked here.
     */
    ++g_Checks;
    classAndAction = ERR_TEST_POISON_WORD;
    locus = ERR_TEST_POISON_BYTE;

    if (DosErrClassify(ERR_TEST_UNMEASURED_CODE, &classAndAction, &locus))
    {
        ++g_Failures;
        printf("  FAIL %-56s claimed to know it\n", "code 0x21 is UNMEASURED");
    }

    ErrTestExpect("unmeasured code zeroes BX (never a guess)", classAndAction, 0);
    ErrTestExpect("unmeasured code zeroes CH (never a guess)", locus, 0);

    /* Every row must carry the oracle case it came from -- an evidence string is
     * not decoration here, it is how the next person re-runs the measurement.
     */
    {
        UINT rowIndex;

        for (rowIndex = 0; rowIndex < DOS_ERR_ROWS; ++rowIndex)
        {
            ++g_Checks;

            if (g_DosErrTable[rowIndex].Evidence && g_DosErrTable[rowIndex].Evidence[0])
                continue;

            ++g_Failures;
            printf("  FAIL row %u (code 0x%02X) has no oracle evidence string\n",
                   rowIndex, g_DosErrTable[rowIndex].Code);
        }
    }

    /* WIN32 -> DOS, the mapping AH=3Dh used to skip entirely. (s72):
     * Every expectation below is a pair of measured lines: the oracle's answer
     * for the situation, and the `win32=0x..` the test machine's own handler logged for
     * the same probe case. Before this existed AH=3Dh answered 2 for every
     * cause, so a read-only file read as "not found".
     */
    {
        WORD dosError;
        printf("== INT 21h AH=3Dh: Win32 failure -> DOS code (dos_err_from_win32)\n");

        ++g_Checks;

        if (!DosErrFromWin32(DOS_ERR_WIN32_FILE_NOT_FOUND, &dosError)) { ++g_Failures;
            printf("  FAIL %-56s not mapped\n", "win32=2 is a measured row"); }

        ErrTestExpect("win32=2  -> 2   [err.after.3D.missing  AX=0002]", dosError,
                      ERR_TEST_FILE_NOT_FOUND);

        ++g_Checks;

        if (!DosErrFromWin32(DOS_ERR_WIN32_PATH_NOT_FOUND, &dosError)) { ++g_Failures;
            printf("  FAIL %-56s not mapped\n", "win32=3 is a measured row"); }

        ErrTestExpect("win32=3  -> 3   [err.after.3D.baddrive AX=0003]", dosError,
                      ERR_TEST_PATH_NOT_FOUND);

        ++g_Checks;

        if (!DosErrFromWin32(DOS_ERR_WIN32_ACCESS_DENIED, &dosError)) { ++g_Failures;
            printf("  FAIL %-56s not mapped\n", "win32=5 is a measured row"); }

        ErrTestExpect("win32=5  -> 5   [err.after.3D.readonly AX=0005]", dosError,
                      ERR_TEST_ACCESS_DENIED);

        /* [CAUTION]: THE POINT OF THE WHOLE EXERCISE: these three must be DISTINCT. The
         * bug was not a wrong constant, it was three causes collapsing to one
         * answer, and a table that mapped them all to 5 would pass any test
         * that only checked "not 2".
         */
        {
            WORD fileNotFound;
            WORD pathNotFound;
            WORD accessDenied;
            DosErrFromWin32(DOS_ERR_WIN32_FILE_NOT_FOUND, &fileNotFound);
            DosErrFromWin32(DOS_ERR_WIN32_PATH_NOT_FOUND, &pathNotFound);
            DosErrFromWin32(DOS_ERR_WIN32_ACCESS_DENIED,  &accessDenied);
            ++g_Checks;

            if (fileNotFound == pathNotFound || pathNotFound == accessDenied
                || fileNotFound == accessDenied) { ++g_Failures;
                printf("  FAIL %-56s %u/%u/%u\n",
                       "not-found / path / denied must stay distinct",
                       fileNotFound, pathNotFound, accessDenied); }
        }

        ++g_Checks;
        dosError = 0;

        if (!DosErrFromWin32(DOS_ERR_WIN32_FILE_EXISTS, &dosError)) { ++g_Failures;
            printf("  FAIL %-56s unmapped\n", "win32 FILE_EXISTS"); }

        ErrTestExpect("6Ch exists+fail -> 0x50 (p_file int21.6C.exists)", dosError,
                      ERR_TEST_FILE_EXISTS);

        /* An unmapped code must NOT invent an answer: it keeps the old 2 and
         * reports FALSE so the handler can log UNMAPPED -- the same refusal
         * DosErrClassify() makes for an unmeasured class.
         */
        ++g_Checks;
        dosError = ERR_TEST_POISON_WORD;

        if (DosErrFromWin32(ERR_TEST_UNMAPPED_WIN32, &dosError)) { ++g_Failures;
            printf("  FAIL %-56s claimed to know it\n", "win32=1234 is unmapped"); }

        ErrTestExpect("unmapped keeps the historical 2 (no invention)", dosError,
                      ERR_TEST_FILE_NOT_FOUND);
    }

    /* #34: the INT 24h contract, from tests/probes/dos/p_crit.asm (6.22 + PCem) */
    {
        WORD classAndAction;
        BYTE locus;
        BOOL isMeasured = DosErrClassify(ERR_TEST_FAIL_I24, &classAndAction, &locus);
        ErrTestExpect("crit.4e.fail.59 is a measured row", isMeasured, TRUE);
        ErrTestExpect("crit.4e.fail.59 BX=0D04", classAndAction, ERR_TEST_BX_FAIL_I24);
        ErrTestExpect("crit.4e.fail.59 CH=01", locus, ERR_TEST_CH_UNKNOWN);
        ErrTestExpect("crit.4e.fail.int24 AH=1A (find-first)",
                      DosCritInt24Ah(ERR_TEST_FIND_FIRST), ERR_TEST_AH_PATH_CALL);
        ErrTestExpect("crit.3c.fail.int24 AH=1A (create: a READ of the FAT)",
                      DosCritInt24Ah(ERR_TEST_CREATE), ERR_TEST_AH_PATH_CALL);
        ErrTestExpect("ignore is NOT allowed on a path call (bit 5)",
                      DosCritInt24Ah(ERR_TEST_OPEN) & ERR_TEST_AH_ALLOW_IGNORE, 0);
        ErrTestExpect("crit.4e.fail.call AX=0003",
                      DosCritFailAx(ERR_TEST_FIND_FIRST, ERR_TEST_NOT_READY_INDEX),
                      ERR_TEST_AX_PATH_NOT_FOUND);
        ErrTestExpect("crit.3d.fail.call AX=0003 (PCem)",
                      DosCritFailAx(ERR_TEST_OPEN, ERR_TEST_NOT_READY_INDEX),
                      ERR_TEST_AX_PATH_NOT_FOUND);
        ErrTestExpect("21 (not ready) is a hardware error",
                      DosCritIsHardwareError(ERR_TEST_NOT_READY), TRUE);
        ErrTestExpect("18 (no more files) is not",
                      DosCritIsHardwareError(ERR_TEST_NO_MORE_FILES), FALSE);
        {   WORD dosError = 0;
            ErrTestExpect("win32 21 -> DOS 21 (identity)",
                          DosErrFromWin32(ERR_TEST_NOT_READY, &dosError), TRUE);
            ErrTestExpect("...value", dosError, ERR_TEST_NOT_READY); }
    }

    /* #275: 3Fh/40h on an open handle. [CAUTION] SPEC-DERIVED (MS-DOS 4.0 kernel source, see
     * dos_err.h), NOT YET MEASURED: p_crit2.asm asks 6.22 and PCem. When it has, the
     * expectations below are replaced by its rows, not the other way round.
     */
    {
        BYTE readAh = DosCritInt24Ah(ERR_TEST_READ);
        BYTE writeAh = DosCritInt24Ah(ERR_TEST_WRITE);
        ErrTestExpect("3Fh AH=3E (data area, read, F+R+I)", readAh, ERR_TEST_AH_READ);
        ErrTestExpect("40h AH=3F (data area, WRITE, F+R+I)", writeAh, ERR_TEST_AH_WRITE);
        ErrTestExpect("3Fh read bit (0) clear", readAh & ERR_TEST_AH_WRITE_BIT, 0);
        ErrTestExpect("40h write bit (0) set", writeAh & ERR_TEST_AH_WRITE_BIT,
                      ERR_TEST_AH_WRITE_BIT);
        ErrTestExpect("3Fh area (bits 1-2) = 3, data",
                      (readAh >> ERR_TEST_AH_AREA_SHIFT) & ERR_TEST_AH_AREA_MASK,
                      ERR_TEST_AH_AREA_DATA);
        ErrTestExpect("40h area (bits 1-2) = 3, data",
                      (writeAh >> ERR_TEST_AH_AREA_SHIFT) & ERR_TEST_AH_AREA_MASK,
                      ERR_TEST_AH_AREA_DATA);
        ErrTestExpect("3Fh IGNORE allowed (bit 5)", readAh & DOS_CRIT_ALLOW_IGNORE,
                      ERR_TEST_AH_ALLOW_IGNORE);
        ErrTestExpect("40h RETRY allowed (bit 4)", writeAh & DOS_CRIT_ALLOW_RETRY,
                      ERR_TEST_AH_ALLOW_RETRY);
        ErrTestExpect("40h FAIL allowed (bit 3)", writeAh & DOS_CRIT_ALLOW_FAIL,
                      ERR_TEST_AH_ALLOW_FAIL);
        ErrTestExpect("a disk, not a character device (bit 7)",
                      (readAh | writeAh) & ERR_TEST_AH_CHARACTER_DEVICE, 0);
        ErrTestExpect("path calls unchanged by #275 (4Eh AH=1A)",
                      DosCritInt24Ah(ERR_TEST_FIND_FIRST), ERR_TEST_AH_PATH_CALL);
        ErrTestExpect("3Fh FAIL -> AX=0005 (SET_ACC_ERR)",
                      DosCritFailAx(ERR_TEST_READ, ERR_TEST_NOT_READY_INDEX),
                      ERR_TEST_AX_ACCESS_DENIED);
        ErrTestExpect("40h FAIL -> AX=0005", DosCritFailAx(ERR_TEST_WRITE, 0),
                      ERR_TEST_AX_ACCESS_DENIED);
        ErrTestExpect("path FAIL still AX=0003",
                      DosCritFailAx(ERR_TEST_CREATE, ERR_TEST_NOT_READY_INDEX),
                      ERR_TEST_AX_PATH_NOT_FOUND);
        ErrTestExpect("31 (general failure) is a hardware error",
                      DosCritIsHardwareError(ERR_TEST_GENERAL_FAILURE), TRUE);
        ErrTestExpect("32 (sharing violation) is NOT",
                      DosCritIsHardwareError(ERR_TEST_SHARING_VIOLATION), FALSE);
        ErrTestExpect("5 (access denied) is NOT",
                      DosCritIsHardwareError(ERR_TEST_ACCESS_DENIED), FALSE);
        /* IGNORE: the call reports what was asked; a read stops at end of file */
        ErrTestExpect("ignore write: the request",
                      DosCritIgnoreCount(ERR_TEST_WRITE, ERR_TEST_REQUEST, 0, 0, TRUE),
                      ERR_TEST_REQUEST);
        ErrTestExpect("ignore read mid-file: the request",
                      DosCritIgnoreCount(ERR_TEST_READ, ERR_TEST_REQUEST, 0, ERR_TEST_FILE_SIZE,
                                         TRUE),
                      ERR_TEST_REQUEST);
        ErrTestExpect("ignore read near EOF: what is left",
                      DosCritIgnoreCount(ERR_TEST_READ, ERR_TEST_REQUEST, ERR_TEST_NEAR_EOF,
                                         ERR_TEST_FILE_SIZE, TRUE),
                      ERR_TEST_LEFT_NEAR_EOF);
        ErrTestExpect("ignore read at/after EOF: 0",
                      DosCritIgnoreCount(ERR_TEST_READ, ERR_TEST_REQUEST, ERR_TEST_PAST_EOF,
                                         ERR_TEST_FILE_SIZE, TRUE),
                      0);
        ErrTestExpect("ignore read, size unknown: the request",
                      DosCritIgnoreCount(ERR_TEST_READ, ERR_TEST_REQUEST, 0, 0, FALSE),
                      ERR_TEST_REQUEST);
        /* AL: the open file's drive, from NT names */
        {
            PCSTR devices[DOS_CRIT_DRIVE_COUNT] = { 0 };
            devices[ERR_TEST_DRIVE_A] = "\\Device\\Floppy0";
            devices[ERR_TEST_DRIVE_C] = "\\Device\\HarddiskVolume1";
            devices[ERR_TEST_DRIVE_D] = "\\Device\\HarddiskVolume10";
            devices[ERR_TEST_DRIVE_E] = "\\Device\\CdRom0";
            ErrTestExpect("floppy file -> A:",
                          DosCritDriveFromNtName("\\Device\\Floppy0\\X.TXT", devices),
                          ERR_TEST_DRIVE_A);
            ErrTestExpect("case-insensitive",
                          DosCritDriveFromNtName("\\DEVICE\\floppy0\\X.TXT", devices),
                          ERR_TEST_DRIVE_A);
            ErrTestExpect("HarddiskVolume1 is not a prefix of ...Volume10 (whole component)",
                          DosCritDriveFromNtName("\\Device\\HarddiskVolume10\\A\\B", devices),
                          ERR_TEST_DRIVE_D);
            ErrTestExpect("HarddiskVolume1 file -> C:",
                          DosCritDriveFromNtName("\\Device\\HarddiskVolume1\\A", devices),
                          ERR_TEST_DRIVE_C);
            ErrTestExpect("CD root -> E:", DosCritDriveFromNtName("\\Device\\CdRom0", devices),
                          ERR_TEST_DRIVE_E);
            ErrTestExpect("network name: no match (-1)",
                          DosCritDriveFromNtName("\\Device\\LanmanRedirector\\srv\\x", devices),
                          DOS_CRIT_NO_DRIVE);
            ErrTestExpect("NULL name: -1", DosCritDriveFromNtName(NULL, devices),
                          DOS_CRIT_NO_DRIVE);
        }
    }

    printf("== %d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}
