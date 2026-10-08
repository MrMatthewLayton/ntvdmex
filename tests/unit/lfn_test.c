/* lfn_test.c -- INT 21h AH=71h (the long-filename API), the pure half, pinned off-VM. (#210)
 *
 * What this pins is ARITHMETIC AND LAYOUT, not stock NTVDM's behaviour: the FILETIME
 * expectations are computed independently (Python's calendar.timegm + the 1601 epoch
 * offset, 11644473600 s), the record offsets are WIN32_FIND_DATAA's, and the action
 * table is RBIL's. Where stock could answer differently (the short-name tail, the
 * DOS-format high dword, ...) dos_lfn.h says UNMEASURED and tests/probes/dos/p_lfn.asm
 * asks it -- a green run here is not a claim of parity.
 *
 *   cc -std=c99 -I src/dos -o lfn_test tests/unit/lfn_test.c && ./lfn_test
 */
#include <stdio.h>
#include <string.h>
#include "dos_lfn.h"

/* FILETIMEs, from calendar.timegm (above). */
#define LFN_TEST_FT_1980_01_01           119600064000000000ull  /* the DOS epoch         */
#define LFN_TEST_FT_1979_12_31_235959    119600063990000000ull
#define LFN_TEST_FT_2000_02_29_060708    125962780280000000ull
#define LFN_TEST_FT_2001_09_17_123456    126452036960000000ull
#define LFN_TEST_FT_2001_09_17_123457_25 126452036972500000ull  /* ...57.250            */
#define LFN_TEST_FT_2107_12_31_235958    159992927980000000ull  /* the last DOS second  */
#define LFN_TEST_FT_2108_01_01           159992928000000000ull
#define LFN_TEST_FT_2026_10_04_120000    0x01DD53F7E1B8E000ull  /* p_lfn's value        */

/* The same instants as DOS date (DX), time (CX) and BH. */
#define LFN_TEST_YEAR_SHIFT              9
#define LFN_TEST_MONTH_SHIFT             5
#define LFN_TEST_HOUR_SHIFT              11
#define LFN_TEST_MINUTE_SHIFT            5
#define LFN_TEST_DOS_DATE(yearsSince1980, month, day) \
    (((yearsSince1980) << LFN_TEST_YEAR_SHIFT) | ((month) << LFN_TEST_MONTH_SHIFT) | (day))
#define LFN_TEST_DOS_TIME(hour, minute, halfSeconds) \
    (((hour) << LFN_TEST_HOUR_SHIFT) | ((minute) << LFN_TEST_MINUTE_SHIFT) | (halfSeconds))
#define LFN_TEST_DATE_1980_01_01         0x0021
#define LFN_TEST_TIME_MIDNIGHT           0x0000
#define LFN_TEST_DATE_2001_09_17         0x2B31
#define LFN_TEST_TIME_12_34_56           0x645C
#define LFN_TEST_DATE_2026_10_04         0x5D44
#define LFN_TEST_TIME_12_00_00           0x6000
#define LFN_TEST_BH_ODD_SECOND_250MS     125     /* 100 + 25                              */
#define LFN_TEST_BH_TOO_BIG              200
#define LFN_TEST_BH_NONE                 0

/* Poison, written before a call that must overwrite it. */
#define LFN_TEST_POISON_WORD             0xEEEE
#define LFN_TEST_POISON_BYTE             0xEE
#define LFN_TEST_POISON_FILETIME         0xEEEEEEEEull
#define LFN_TEST_POISON_CHAR             'Z'

/* The find record, by WIN32_FIND_DATAA's offsets. */
#define LFN_TEST_RECORD_SIZE             318
#define LFN_TEST_GUARD_BYTES             4
#define LFN_TEST_GUARD                   0xCC
#define LFN_TEST_ATTRIBUTES              0x00
#define LFN_TEST_CREATION_LOW            0x04
#define LFN_TEST_CREATION_HIGH           0x08
#define LFN_TEST_ACCESS_LOW              0x0C
#define LFN_TEST_ACCESS_HIGH             0x10
#define LFN_TEST_WRITE_LOW               0x14
#define LFN_TEST_WRITE_HIGH              0x18
#define LFN_TEST_SIZE_HIGH               0x1C
#define LFN_TEST_SIZE_LOW                0x20
#define LFN_TEST_RESERVED_0              0x24
#define LFN_TEST_RESERVED_1              0x28
#define LFN_TEST_LONG_NAME               0x2C
#define LFN_TEST_SHORT_NAME              0x130
#define LFN_TEST_LONG_NAME_WITH_NUL      21      /* "A long file name.txt" and its NUL     */
#define LFN_TEST_LONG_NAME_MAX           259
#define LFN_TEST_FILE_SIZE_HIGH          0x11223344
#define LFN_TEST_FILE_SIZE_LOW           0x55667788
#define LFN_TEST_DATE_SHIFT              16      /* SI=1: the date in the high word        */
#define LFN_TEST_FILETIME_FORMAT         FALSE   /* SI=0                                   */
#define LFN_TEST_DOS_FORMAT              TRUE    /* SI=1                                   */
#define LFN_TEST_OVERLONG_BUFFER         400
#define LFN_TEST_OVERLONG_LENGTH         300
#define LFN_TEST_OVERLONG_CHAR           'x'

/* A little-endian DWORD's bytes. */
#define LFN_TEST_BYTE1                   1
#define LFN_TEST_BYTE2                   2
#define LFN_TEST_BYTE3                   3
#define LFN_TEST_BYTE1_SHIFT             8
#define LFN_TEST_BYTE2_SHIFT             16
#define LFN_TEST_BYTE3_SHIFT             24

/* Attribute bits, and CL / CH. */
#define LFN_TEST_NONE                    0x00
#define LFN_TEST_READ_ONLY               0x01
#define LFN_TEST_HIDDEN                  0x02
#define LFN_TEST_SYSTEM                  0x04
#define LFN_TEST_DIRECTORY               0x10
#define LFN_TEST_ARCHIVE                 0x20
#define LFN_TEST_NORMAL                  0x80

/* 6Ch / 716Ch: the action word, what CX reports, and BX's access mode. */
#define LFN_TEST_ACTION_MEANINGLESS      0x00
#define LFN_TEST_ACTION_OPEN_FAIL        0x01
#define LFN_TEST_ACTION_TRUNCATE_FAIL    0x02
#define LFN_TEST_ACTION_FAIL_CREATE      0x10
#define LFN_TEST_ACTION_OPEN_CREATE      0x11
#define LFN_TEST_ACTION_TRUNCATE_CREATE  0x12
#define LFN_TEST_TAKEN_OPENED            1
#define LFN_TEST_TAKEN_CREATED           2
#define LFN_TEST_TAKEN_REPLACED          3
#define LFN_TEST_EXISTED                 TRUE
#define LFN_TEST_NEW                     FALSE
#define LFN_TEST_LFN_CALL                TRUE    /* 716Ch                                  */
#define LFN_TEST_DOS_CALL                FALSE   /* 6Ch                                    */
#define LFN_TEST_MODE_READ               0
#define LFN_TEST_MODE_WRITE              1
#define LFN_TEST_MODE_READ_WRITE         2
#define LFN_TEST_MODE_SHARED_READ_WRITE  0x2042
#define LFN_TEST_GENERIC_READ            0x80000000ul
#define LFN_TEST_GENERIC_WRITE           0x40000000ul
#define LFN_TEST_GENERIC_READ_WRITE      0xC0000000ul

/* Win32 errors in, DOS errors out. */
#define LFN_TEST_WIN32_FILE_NOT_FOUND    2
#define LFN_TEST_WIN32_INVALID_HANDLE    6
#define LFN_TEST_WIN32_NO_MORE_FILES     18
#define LFN_TEST_WIN32_INVALID_PARAMETER 87
#define LFN_TEST_WIN32_DIR_NOT_EMPTY     145
#define LFN_TEST_WIN32_ALREADY_EXISTS    183
#define LFN_TEST_DOS_FILE_NOT_FOUND      2
#define LFN_TEST_DOS_ACCESS_DENIED       5
#define LFN_TEST_DOS_INVALID_HANDLE      6
#define LFN_TEST_DOS_NO_MORE_FILES       18
#define LFN_TEST_DOS_FILE_EXISTS         0x50

#define LFN_TEST_LABEL_SIZE              160

static INT g_Checks, g_Failures;

static VOID LfnTestExpect(PCSTR description, UINT64 actual, UINT64 expected)
{
    ++g_Checks;
    if (actual == expected) return;
    ++g_Failures;
    printf("  FAIL %-58s got 0x%llX, want 0x%llX\n", description, actual, expected);
}

static VOID LfnTestExpectString(PCSTR description, PCSTR actual, PCSTR expected)
{
    ++g_Checks;
    if (!strcmp(actual, expected)) return;
    ++g_Failures;
    printf("  FAIL %-58s got \"%s\", want \"%s\"\n", description, actual, expected);
}

static VOID LfnTestFileTimeToDos(PCSTR description, UINT64 fileTime, BOOL shouldConvert,
                                 UINT expectedDate, UINT expectedTime, UINT expectedTenMs)
{
    WORD dosDate = LFN_TEST_POISON_WORD, dosTime = LFN_TEST_POISON_WORD;
    BYTE tenMs = LFN_TEST_POISON_BYTE;
    CHAR label[LFN_TEST_LABEL_SIZE];
    BOOL didConvert = DosLfnFileTimeToDos(fileTime, &dosDate, &dosTime, &tenMs);
    snprintf(label, sizeof label, "%s: converts", description);
    LfnTestExpect(label, didConvert, shouldConvert);
    if (!shouldConvert || !didConvert) return;
    snprintf(label, sizeof label, "%s: DX date", description);
    LfnTestExpect(label, dosDate, expectedDate);
    snprintf(label, sizeof label, "%s: CX time", description);
    LfnTestExpect(label, dosTime, expectedTime);
    snprintf(label, sizeof label, "%s: BH 10ms", description);
    LfnTestExpect(label, tenMs, expectedTenMs);
}

static VOID LfnTestDosToFileTime(PCSTR description, UINT dosDate, UINT dosTime, UINT tenMs,
                                 BOOL shouldConvert, UINT64 expectedFileTime)
{
    UINT64 fileTime = LFN_TEST_POISON_FILETIME;
    CHAR label[LFN_TEST_LABEL_SIZE];
    BOOL didConvert = DosLfnDosToFileTime((WORD)dosDate, (WORD)dosTime, (BYTE)tenMs, &fileTime);
    snprintf(label, sizeof label, "%s: converts", description);
    LfnTestExpect(label, didConvert, shouldConvert);
    if (shouldConvert && didConvert) {
        snprintf(label, sizeof label, "%s: FILETIME", description);
        LfnTestExpect(label, fileTime, expectedFileTime);
    }
}

static VOID LfnTestShortName(PCSTR longName, PCSTR expectedShortName, PCSTR expectedFcbName)
{
    CHAR shortName[DOS_SHORT_NAME_SIZE], fcbName[DOS_FCB_NAME_SIZE + 1];
    CHAR label[LFN_TEST_LABEL_SIZE];
    memset(shortName, LFN_TEST_POISON_CHAR, sizeof shortName);
    DosLfnShortName(longName, shortName, fcbName);
    fcbName[DOS_FCB_NAME_SIZE] = 0;
    snprintf(label, sizeof label, "71A8 DH=1 \"%s\"", longName);
    LfnTestExpectString(label, shortName, expectedShortName);
    snprintf(label, sizeof label, "71A8 DH=0 \"%s\"", longName);
    LfnTestExpectString(label, fcbName, expectedFcbName);
}

static DWORD LfnTestReadDword(PCBYTE bytes)
{
    return bytes[0] | (bytes[LFN_TEST_BYTE1] << LFN_TEST_BYTE1_SHIFT)
         | (bytes[LFN_TEST_BYTE2] << LFN_TEST_BYTE2_SHIFT)
         | ((DWORD)bytes[LFN_TEST_BYTE3] << LFN_TEST_BYTE3_SHIFT);
}

INT main(VOID)
{
    printf("== INT 21h AH=71h long-filename API, pure half (dos_lfn.h)\n");

    /* ── 71A7h BL=0: FILETIME -> DOS. Constants from calendar.timegm. ──────────── */
    LfnTestFileTimeToDos("1980-01-01 00:00:00 (DOS epoch)", LFN_TEST_FT_1980_01_01, TRUE,
                         LFN_TEST_DATE_1980_01_01, LFN_TEST_TIME_MIDNIGHT, LFN_TEST_BH_NONE);
    /* The stamp p_file sets with 5701h: 12:34:56 2001-09-17. */
    LfnTestFileTimeToDos("2001-09-17 12:34:56", LFN_TEST_FT_2001_09_17_123456, TRUE,
                         LFN_TEST_DATE_2001_09_17, LFN_TEST_TIME_12_34_56, LFN_TEST_BH_NONE);
    /* An ODD second and 250 ms: the DOS time keeps 56 (28*2); BH = 100 + 25. */
    LfnTestFileTimeToDos("2001-09-17 12:34:57.250", LFN_TEST_FT_2001_09_17_123457_25, TRUE,
                         LFN_TEST_DATE_2001_09_17, LFN_TEST_TIME_12_34_56,
                         LFN_TEST_BH_ODD_SECOND_250MS);
    LfnTestFileTimeToDos("2107-12-31 23:59:58 (last DOS second)", LFN_TEST_FT_2107_12_31_235958,
                         TRUE, LFN_TEST_DOS_DATE(127u, 12u, 31u), LFN_TEST_DOS_TIME(23u, 59u, 29u),
                         LFN_TEST_BH_NONE);
    LfnTestFileTimeToDos("2108-01-01 (past DOS's range)", LFN_TEST_FT_2108_01_01, FALSE, 0, 0, 0);
    LfnTestFileTimeToDos("1979-12-31 23:59:59 (before it)", LFN_TEST_FT_1979_12_31_235959, FALSE,
                         0, 0, 0);
    LfnTestFileTimeToDos("2000-02-29 06:07:08 (leap day)", LFN_TEST_FT_2000_02_29_060708, TRUE,
                         LFN_TEST_DOS_DATE(20u, 2u, 29u), LFN_TEST_DOS_TIME(6u, 7u, 4u),
                         LFN_TEST_BH_NONE);
    /* The probe's own 71A7h case (p_lfn lfn.71A7.ft2dos): 2026-10-04 12:00:00. */
    LfnTestFileTimeToDos("2026-10-04 12:00:00 (p_lfn's value)", LFN_TEST_FT_2026_10_04_120000,
                         TRUE, LFN_TEST_DATE_2026_10_04, LFN_TEST_TIME_12_00_00, LFN_TEST_BH_NONE);

    /* ── 71A7h BL=1: DOS -> FILETIME, and the round trip. ──────────────────────── */
    LfnTestDosToFileTime("DOS epoch", LFN_TEST_DATE_1980_01_01, LFN_TEST_TIME_MIDNIGHT,
                         LFN_TEST_BH_NONE, TRUE, LFN_TEST_FT_1980_01_01);
    LfnTestDosToFileTime("2001-09-17 12:34:56", LFN_TEST_DATE_2001_09_17, LFN_TEST_TIME_12_34_56,
                         LFN_TEST_BH_NONE, TRUE, LFN_TEST_FT_2001_09_17_123456);
    LfnTestDosToFileTime("2001-09-17 12:34:57.250 via BH=125", LFN_TEST_DATE_2001_09_17,
                         LFN_TEST_TIME_12_34_56, LFN_TEST_BH_ODD_SECOND_250MS, TRUE,
                         LFN_TEST_FT_2001_09_17_123457_25);
    LfnTestDosToFileTime("leap day 2000-02-29", LFN_TEST_DOS_DATE(20u, 2u, 29u),
                         LFN_TEST_DOS_TIME(6u, 7u, 4u), LFN_TEST_BH_NONE, TRUE,
                         LFN_TEST_FT_2000_02_29_060708);
    LfnTestDosToFileTime("2001-02-29 (no such day)", LFN_TEST_DOS_DATE(21u, 2u, 29u), 0, 0, FALSE,
                         0);
    LfnTestDosToFileTime("month 13", LFN_TEST_DOS_DATE(21u, 13u, 1u), 0, 0, FALSE, 0);
    LfnTestDosToFileTime("day 0", LFN_TEST_DOS_DATE(21u, 1u, 0u), 0, 0, FALSE, 0);
    LfnTestDosToFileTime("hour 24", LFN_TEST_DATE_2001_09_17, LFN_TEST_DOS_TIME(24u, 0u, 0u), 0,
                         FALSE, 0);
    LfnTestDosToFileTime("seconds/2 = 30", LFN_TEST_DATE_2001_09_17, LFN_TEST_DOS_TIME(0u, 0u, 30u),
                         0, FALSE, 0);
    LfnTestDosToFileTime("BH = 200", LFN_TEST_DATE_2001_09_17, LFN_TEST_TIME_12_34_56,
                         LFN_TEST_BH_TOO_BIG, FALSE, 0);
    {   WORD dosDate, dosTime; BYTE tenMs; UINT64 roundTrip = 0;
        DosLfnFileTimeToDos(LFN_TEST_FT_2001_09_17_123457_25, &dosDate, &dosTime, &tenMs);
        DosLfnDosToFileTime(dosDate, dosTime, tenMs, &roundTrip);
        LfnTestExpect("round trip FILETIME -> DOS -> FILETIME (10 ms exact)", roundTrip,
                      LFN_TEST_FT_2001_09_17_123457_25); }

    /* ── 714Eh/714Fh: the 318-byte record. ─────────────────────────────────────── */
    {   BYTE record[DOS_LFN_FIND_RECORD_SIZE + LFN_TEST_GUARD_BYTES];
        DOS_LFN_FIND_ENTRY entry;
        INT byteIndex, strayBits = 0;
        memset(record, LFN_TEST_GUARD, sizeof record);
        entry.Attributes = LFN_TEST_ARCHIVE;
        entry.SizeHigh = LFN_TEST_FILE_SIZE_HIGH; entry.SizeLow = LFN_TEST_FILE_SIZE_LOW;
        entry.CreationTime = LFN_TEST_FT_1980_01_01; entry.LastAccessTime = 0;
        entry.LastWriteTime = LFN_TEST_FT_2001_09_17_123457_25;
        entry.LongName = "A long file name.txt"; entry.ShortName = "ALONGF~1.TXT";
        DosLfnFindPack(record, &entry, LFN_TEST_FILETIME_FORMAT);
        LfnTestExpect("record length is 318 (13Eh)", DOS_LFN_FIND_RECORD_SIZE,
                      LFN_TEST_RECORD_SIZE);
        LfnTestExpect("bytes past the record untouched", record[DOS_LFN_FIND_RECORD_SIZE],
                      LFN_TEST_GUARD);
        LfnTestExpect("00h attributes", LfnTestReadDword(record + LFN_TEST_ATTRIBUTES),
                      LFN_TEST_ARCHIVE);
        LfnTestExpect("04h creation low",  LfnTestReadDword(record + LFN_TEST_CREATION_LOW),
                      (DWORD)LFN_TEST_FT_1980_01_01);
        LfnTestExpect("08h creation high", LfnTestReadDword(record + LFN_TEST_CREATION_HIGH),
                      (DWORD)(LFN_TEST_FT_1980_01_01 >> DWORD_SHIFT));
        LfnTestExpect("0Ch last access (0)", LfnTestReadDword(record + LFN_TEST_ACCESS_LOW)
                                             | LfnTestReadDword(record + LFN_TEST_ACCESS_HIGH), 0);
        LfnTestExpect("14h last write low",  LfnTestReadDword(record + LFN_TEST_WRITE_LOW),
                      (DWORD)LFN_TEST_FT_2001_09_17_123457_25);
        LfnTestExpect("18h last write high", LfnTestReadDword(record + LFN_TEST_WRITE_HIGH),
                      (DWORD)(LFN_TEST_FT_2001_09_17_123457_25 >> DWORD_SHIFT));
        LfnTestExpect("1Ch size HIGH first", LfnTestReadDword(record + LFN_TEST_SIZE_HIGH),
                      LFN_TEST_FILE_SIZE_HIGH);
        LfnTestExpect("20h size low",        LfnTestReadDword(record + LFN_TEST_SIZE_LOW),
                      LFN_TEST_FILE_SIZE_LOW);
        LfnTestExpect("24h reserved zeroed", LfnTestReadDword(record + LFN_TEST_RESERVED_0)
                                             | LfnTestReadDword(record + LFN_TEST_RESERVED_1), 0);
        LfnTestExpectString("2Ch long name", (PCSTR)record + LFN_TEST_LONG_NAME,
                            "A long file name.txt");
        LfnTestExpectString("130h short name", (PCSTR)record + LFN_TEST_SHORT_NAME, "ALONGF~1.TXT");
        for (byteIndex = LFN_TEST_LONG_NAME + LFN_TEST_LONG_NAME_WITH_NUL;
             byteIndex < LFN_TEST_SHORT_NAME; ++byteIndex)
            strayBits |= record[byteIndex];
        LfnTestExpect("long name field zero past its NUL", strayBits, 0);
        /* SI=1: DOS date in the high word, time in the low word; high dword 0. */
        DosLfnFindPack(record, &entry, LFN_TEST_DOS_FORMAT);
        LfnTestExpect("SI=1 write time = date<<16|time",
                      LfnTestReadDword(record + LFN_TEST_WRITE_LOW),
                      ((DWORD)LFN_TEST_DATE_2001_09_17 << LFN_TEST_DATE_SHIFT)
                      | LFN_TEST_TIME_12_34_56);
        LfnTestExpect("SI=1 write time high dword 0",
                      LfnTestReadDword(record + LFN_TEST_WRITE_HIGH), 0);
        LfnTestExpect("SI=1 creation = DOS epoch", LfnTestReadDword(record + LFN_TEST_CREATION_LOW),
                      ((DWORD)LFN_TEST_DATE_1980_01_01 << LFN_TEST_DATE_SHIFT));
        LfnTestExpect("SI=1 a zero FILETIME stays 0",
                      LfnTestReadDword(record + LFN_TEST_ACCESS_LOW), 0);
        /* A 259-character long name fills the field and keeps its NUL. */
        {   CHAR overlongName[LFN_TEST_OVERLONG_BUFFER];
            memset(overlongName, LFN_TEST_OVERLONG_CHAR, LFN_TEST_OVERLONG_LENGTH);
            overlongName[LFN_TEST_OVERLONG_LENGTH] = 0;
            entry.LongName = overlongName;
            DosLfnFindPack(record, &entry, LFN_TEST_FILETIME_FORMAT);
            LfnTestExpect("an over-long name is cut at 259 + NUL",
                          record[LFN_TEST_LONG_NAME + LFN_TEST_LONG_NAME_MAX], 0);
            LfnTestExpect("...and the short name field is intact", record[LFN_TEST_SHORT_NAME],
                          'A'); }
    }

    /* ── CL allowed / CH required. ─────────────────────────────────────────────── */
    LfnTestExpect("plain file, CL=0",
                  DosLfnAttributesOk(LFN_TEST_ARCHIVE, LFN_TEST_NONE, LFN_TEST_NONE), TRUE);
    LfnTestExpect("directory, CL=0 -> hidden",
                  DosLfnAttributesOk(LFN_TEST_DIRECTORY, LFN_TEST_NONE, LFN_TEST_NONE), FALSE);
    LfnTestExpect("directory, CL=10h",
                  DosLfnAttributesOk(LFN_TEST_DIRECTORY, LFN_TEST_DIRECTORY, LFN_TEST_NONE), TRUE);
    LfnTestExpect("hidden file needs CL bit 1",
                  DosLfnAttributesOk(LFN_TEST_HIDDEN | LFN_TEST_ARCHIVE, LFN_TEST_NONE,
                                     LFN_TEST_NONE), FALSE);
    LfnTestExpect("system file with CL=16h",
                  DosLfnAttributesOk(LFN_TEST_SYSTEM | LFN_TEST_ARCHIVE,
                                     LFN_TEST_HIDDEN | LFN_TEST_SYSTEM | LFN_TEST_DIRECTORY,
                                     LFN_TEST_NONE), TRUE);
    LfnTestExpect("CH=10h: a file is not a dir",
                  DosLfnAttributesOk(LFN_TEST_ARCHIVE, LFN_TEST_DIRECTORY, LFN_TEST_DIRECTORY),
                  FALSE);
    LfnTestExpect("CH=10h: a dir is",
                  DosLfnAttributesOk(LFN_TEST_DIRECTORY, LFN_TEST_DIRECTORY, LFN_TEST_DIRECTORY),
                  TRUE);
    LfnTestExpect("read-only never excludes",
                  DosLfnAttributesOk(LFN_TEST_READ_ONLY | LFN_TEST_ARCHIVE, LFN_TEST_NONE,
                                     LFN_TEST_NONE), TRUE);
    LfnTestExpect("Win32 NORMAL (80h) is a file",
                  DosLfnAttributesOk(LFN_TEST_NORMAL, LFN_TEST_NONE, LFN_TEST_NONE), TRUE);

    /* ── 71A8h. ────────────────────────────────────────────────────────────────── */
    LfnTestShortName("A long file name.txt", "ALONGF~1.TXT", "ALONGF~1TXT");
    LfnTestShortName("Long Directory Name",  "LONGDI~1",     "LONGDI~1   ");
    LfnTestShortName("readme.txt",           "README.TXT",   "README  TXT");   /* already 8.3 */
    LfnTestShortName("COMMAND.COM",          "COMMAND.COM",  "COMMAND COM");
    LfnTestShortName("a.b.c.txt",            "ABC~1.TXT",    "ABC~1   TXT");   /* dots dropped */
    LfnTestShortName("my+file=1.html",       "MY_FIL~1.HTM", "MY_FIL~1HTM");   /* + = -> _ */
    LfnTestShortName("x y.z",                "XY~1.Z",       "XY~1    Z  ");
    LfnTestShortName("C:\\dir\\Some Name.doc", "SOMENA~1.DOC", "SOMENA~1DOC"); /* last component */
    /* a bare dot is not 8.3 */
    LfnTestShortName("NAME.",                "NAME~1",       "NAME~1     ");
    LfnTestShortName("toolongname.txt",      "TOOLON~1.TXT", "TOOLON~1TXT");

    /* ── 6Ch / 716Ch action word. ─────────────────────────────────────────────── */
    LfnTestExpect("action 01h open|fail", DosExtOpenDisposition(LFN_TEST_ACTION_OPEN_FAIL),
                  DOS_EXT_OPEN_OPEN_EXISTING);
    LfnTestExpect("action 10h fail|create", DosExtOpenDisposition(LFN_TEST_ACTION_FAIL_CREATE),
                  DOS_EXT_OPEN_CREATE_NEW);
    LfnTestExpect("action 11h open|create", DosExtOpenDisposition(LFN_TEST_ACTION_OPEN_CREATE),
                  DOS_EXT_OPEN_OPEN_ALWAYS);
    LfnTestExpect("action 12h trunc|create",
                  DosExtOpenDisposition(LFN_TEST_ACTION_TRUNCATE_CREATE),
                  DOS_EXT_OPEN_CREATE_ALWAYS);
    LfnTestExpect("action 02h trunc|fail", DosExtOpenDisposition(LFN_TEST_ACTION_TRUNCATE_FAIL),
                  DOS_EXT_OPEN_TRUNCATE_EXISTING);
    LfnTestExpect("action 00h (meaningless) -> open, as 6Ch always did",
                  DosExtOpenDisposition(LFN_TEST_ACTION_MEANINGLESS), DOS_EXT_OPEN_OPEN_EXISTING);
    LfnTestExpect("taken: open existing      -> 1",
                  DosExtOpenActionTaken(DOS_EXT_OPEN_OPEN_EXISTING, LFN_TEST_EXISTED,
                                        LFN_TEST_DOS_CALL), LFN_TEST_TAKEN_OPENED);
    LfnTestExpect("taken: create new         -> 2",
                  DosExtOpenActionTaken(DOS_EXT_OPEN_CREATE_NEW, LFN_TEST_NEW, LFN_TEST_DOS_CALL),
                  LFN_TEST_TAKEN_CREATED);
    LfnTestExpect("taken: truncate existing  -> 3",
                  DosExtOpenActionTaken(DOS_EXT_OPEN_TRUNCATE_EXISTING, LFN_TEST_EXISTED,
                                        LFN_TEST_DOS_CALL), LFN_TEST_TAKEN_REPLACED);
    LfnTestExpect("taken: open-always, there -> 1",
                  DosExtOpenActionTaken(DOS_EXT_OPEN_OPEN_ALWAYS, LFN_TEST_EXISTED,
                                        LFN_TEST_DOS_CALL), LFN_TEST_TAKEN_OPENED);
    LfnTestExpect("taken: open-always, new   -> 2",
                  DosExtOpenActionTaken(DOS_EXT_OPEN_OPEN_ALWAYS, LFN_TEST_NEW, LFN_TEST_DOS_CALL),
                  LFN_TEST_TAKEN_CREATED);
    LfnTestExpect("taken: create-always, there -> 3 (replaced)",
                  DosExtOpenActionTaken(DOS_EXT_OPEN_CREATE_ALWAYS, LFN_TEST_EXISTED,
                                        LFN_TEST_DOS_CALL), LFN_TEST_TAKEN_REPLACED);
    LfnTestExpect("taken: create-always, new   -> 3 (stock 6Ch, p_lfn)",
                  DosExtOpenActionTaken(DOS_EXT_OPEN_CREATE_ALWAYS, LFN_TEST_NEW,
                                        LFN_TEST_DOS_CALL), LFN_TEST_TAKEN_REPLACED);
    LfnTestExpect("taken: create-always, new, 716Ch -> 2 (stock, p_lfn)",
                  DosExtOpenActionTaken(DOS_EXT_OPEN_CREATE_ALWAYS, LFN_TEST_NEW,
                                        LFN_TEST_LFN_CALL), LFN_TEST_TAKEN_CREATED);
    LfnTestExpect("taken: create-always, there, 716Ch -> 3",
                  DosExtOpenActionTaken(DOS_EXT_OPEN_CREATE_ALWAYS, LFN_TEST_EXISTED,
                                        LFN_TEST_LFN_CALL), LFN_TEST_TAKEN_REPLACED);
    LfnTestExpect("BX mode 0 -> GENERIC_READ", DosExtOpenAccess(LFN_TEST_MODE_READ),
                  LFN_TEST_GENERIC_READ);
    LfnTestExpect("BX mode 1 -> GENERIC_WRITE", DosExtOpenAccess(LFN_TEST_MODE_WRITE),
                  LFN_TEST_GENERIC_WRITE);
    LfnTestExpect("BX mode 2 -> read|write", DosExtOpenAccess(LFN_TEST_MODE_READ_WRITE),
                  LFN_TEST_GENERIC_READ_WRITE);
    LfnTestExpect("BX 0x2042 (share bits) -> r|w",
                  DosExtOpenAccess(LFN_TEST_MODE_SHARED_READ_WRITE), LFN_TEST_GENERIC_READ_WRITE);

    /* ── errors. ────────────────────────────────────────────────────────────────── */
    {   WORD dosError = 0;
        LfnTestExpect("w32 2 -> 2 (measured row)",
                      DosLfnErrFromWin32(LFN_TEST_WIN32_FILE_NOT_FOUND, &dosError)
                      && dosError == LFN_TEST_DOS_FILE_NOT_FOUND, TRUE);
        LfnTestExpect("w32 18 -> 18 no more files",
                      DosLfnErrFromWin32(LFN_TEST_WIN32_NO_MORE_FILES, &dosError)
                      && dosError == LFN_TEST_DOS_NO_MORE_FILES, TRUE);
        LfnTestExpect("w32 6 -> 6 invalid handle",
                      DosLfnErrFromWin32(LFN_TEST_WIN32_INVALID_HANDLE, &dosError)
                      && dosError == LFN_TEST_DOS_INVALID_HANDLE, TRUE);
        LfnTestExpect("w32 145 dir not empty -> 5",
                      DosLfnErrFromWin32(LFN_TEST_WIN32_DIR_NOT_EMPTY, &dosError)
                      && dosError == LFN_TEST_DOS_ACCESS_DENIED, TRUE);
        LfnTestExpect("w32 183 already exists -> 80",
                      DosLfnErrFromWin32(LFN_TEST_WIN32_ALREADY_EXISTS, &dosError)
                      && dosError == LFN_TEST_DOS_FILE_EXISTS, TRUE);
        LfnTestExpect("w32 87 unmapped -> 0, AX 2",
                      DosLfnErrFromWin32(LFN_TEST_WIN32_INVALID_PARAMETER, &dosError) == FALSE
                      && dosError == LFN_TEST_DOS_FILE_NOT_FOUND, TRUE); }

    printf("== %d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}
