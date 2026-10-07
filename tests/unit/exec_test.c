/* exec_test.c -- EXEC's two header questions, pinned off-VM.  GH #255.
 *
 *   1. DosExeKind: is this a DOS program, or a Windows one EXEC must hand to
 *      Windows? Built from synthetic headers -- including the three that must NOT be
 *      taken for Windows (an LE-bound DOS/4GW game, a BIND'ed OS/2 family program,
 *      an old .EXE with junk at 3Ch).
 *   2. DosExecSize: how much memory the header buys. The expectations are the
 *      rows p_exmem.asm measured on MS-DOS 6.22, PCem and DOSBox-X, quoted by name;
 *      the children are p_exmemc.asm's header, rebuilt here (one 512-byte page, a
 *      32-byte header, 130h bytes of image -- which DOS counts as 1Eh paragraphs).
 *
 *   cc -std=c99 -I src -I src/dos -o exec_test tests/unit/exec_test.c && ./exec_test
 */
#include <stdio.h>
#include <string.h>
#include "dos_loader.h"

/* The synthetic files. */
#define EXEC_TEST_FILE_SIZE          0x400
#define EXEC_TEST_NEW_HEADER         0x80     /* where e_lfanew points                    */
#define EXEC_TEST_LOW_NEW_HEADER     0x20     /* an e_lfanew below 40h                    */
#define EXEC_TEST_SHORT_READ         0x81     /* stops one byte into the new header       */
#define EXEC_TEST_BROKEN_PE_NUL      0x83     /* the second of "PE\0\0"'s NULs            */
#define EXEC_TEST_STUB_HEADER_PARAS  4        /* 64-byte header                           */
#define EXEC_TEST_STUB_RELOCATIONS   0x40     /* e_lfarlc                                 */
#define EXEC_TEST_PE_GUI             2
#define EXEC_TEST_PE_CONSOLE         3
#define EXEC_TEST_NE_OS2             1
#define EXEC_TEST_NO_EXTRA           0

/* p_exmemc.asm's child. */
#define EXEC_TEST_CHILD_HEADER_BYTES 0x20
#define EXEC_TEST_CHILD_IMAGE_BYTES  0x130
#define EXEC_TEST_CHILD_HEADER_PARAS 2
#define EXEC_TEST_CHILD_RELOCATIONS  0x1C
#define EXEC_TEST_CHILD_INITIAL_SP   0x130
#define EXEC_TEST_CHILD_IMAGE_PARAS  0x1E     /* one page less the header                 */

/* The memory each row measured, in paragraphs. */
#define EXEC_TEST_LARGEST            0x8000
#define EXEC_TEST_SMALL_BLOCK        0x100
#define EXEC_TEST_EXACT_BLOCK        0x12E    /* 10h + 1Eh + 100h                         */
#define EXEC_TEST_SHORT_BLOCK        0x12D
#define EXEC_TEST_MIN_100            0x100
#define EXEC_TEST_MAX_200            0x200
#define EXEC_TEST_MIN_F000           0xF000
#define EXEC_TEST_MAX_FFFF           0xFFFF
#define EXEC_TEST_MAX_10             0x10
#define EXEC_TEST_MIN_40             0x40
#define EXEC_TEST_ALLOC_A            0x22E    /* 10h + 1Eh + 200h                         */
#define EXEC_TEST_ALLOC_E            0x3E     /* 10h + 1Eh + 10h                          */
#define EXEC_TEST_ALLOC_MIN_WINS     0x6E     /* 10h + 1Eh + 40h                          */
#define EXEC_TEST_COM_FIRST_BYTE     0xB4
#define EXEC_TEST_COM_SIZE           0x100

static INT g_Checks, g_Failures;

static VOID ExecTestExpect(PCSTR description, LONG actual, LONG expected)
{
    ++g_Checks;
    if (actual == expected) return;
    ++g_Failures;
    printf("  FAIL %-58s got 0x%lX, want 0x%lX\n", description, (long)actual, (long)expected);
}

static BYTE g_File[EXEC_TEST_FILE_SIZE];

static VOID ExecTestPutWord(UINT offset, UINT value)
{
    g_File[offset] = (BYTE)value; g_File[offset + 1] = (BYTE)(value >> DOS_HIGH_BYTE_SHIFT);
}

/* an MZ file with e_lfanew = newHeader and `signature` there */
static VOID ExecTestMakeStub(UINT newHeader, PCSTR signature, UINT extraOffset, UINT extraValue)
{
    memset(g_File, 0, sizeof g_File);
    g_File[0] = 'M'; g_File[1] = 'Z';
    ExecTestPutWord(DOS_MZ_HEADER_PARAGRAPHS, EXEC_TEST_STUB_HEADER_PARAS);    /* 64-byte header */
    ExecTestPutWord(DOS_MZ_RELOCATION_TABLE, EXEC_TEST_STUB_RELOCATIONS);      /* e_lfarlc */
    ExecTestPutWord(DOS_MZ_NEW_HEADER, newHeader);
    if (signature) memcpy(g_File + newHeader, signature, strlen(signature));
    if (extraOffset) ExecTestPutWord(newHeader + extraOffset, extraValue);
}

/* p_exmemc.asm's child: 32-byte header, 130h-byte image, min/max as given */
static DWORD ExecTestMakeChild(UINT minimumAlloc, UINT maximumAlloc)
{
    DWORD length = EXEC_TEST_CHILD_HEADER_BYTES + EXEC_TEST_CHILD_IMAGE_BYTES;
    memset(g_File, 0, sizeof g_File);
    g_File[0] = 'M'; g_File[1] = 'Z';
    ExecTestPutWord(DOS_MZ_LAST_PAGE_BYTES, length % DOS_MZ_PAGE_BYTES);
    ExecTestPutWord(DOS_MZ_PAGE_COUNT, (length + DOS_MZ_PAGE_BYTES - 1) / DOS_MZ_PAGE_BYTES);
    ExecTestPutWord(DOS_MZ_HEADER_PARAGRAPHS, EXEC_TEST_CHILD_HEADER_PARAS);
    ExecTestPutWord(DOS_MZ_MIN_ALLOC, minimumAlloc); ExecTestPutWord(DOS_MZ_MAX_ALLOC, maximumAlloc);
    ExecTestPutWord(DOS_MZ_INITIAL_SP, EXEC_TEST_CHILD_INITIAL_SP);
    ExecTestPutWord(DOS_MZ_RELOCATION_TABLE, EXEC_TEST_CHILD_RELOCATIONS);
    return length;
}

INT main(VOID)
{
    UINT subsystem;
    WORD allocation; BOOL loadHigh; INT status;
    DWORD length;

    /* ── 1. what kind of program ── */
    ExecTestMakeStub(EXEC_TEST_NEW_HEADER, "PE\0\0", DOS_PE_SUBSYSTEM, EXEC_TEST_PE_GUI);
    ExecTestExpect("PE, GUI subsystem -> PE",        DosExeKind(g_File, sizeof g_File, &subsystem), DOS_EXE_PE);
    ExecTestExpect("PE, GUI subsystem -> subsys 2",  subsystem, EXEC_TEST_PE_GUI);
    ExecTestMakeStub(EXEC_TEST_NEW_HEADER, "PE\0\0", DOS_PE_SUBSYSTEM, EXEC_TEST_PE_CONSOLE);
    DosExeKind(g_File, sizeof g_File, &subsystem);
    ExecTestExpect("PE, console subsystem -> subsys 3", subsystem, EXEC_TEST_PE_CONSOLE);
    ExecTestMakeStub(EXEC_TEST_NEW_HEADER, "NE", DOS_NE_TARGET_OS, DOS_NE_OS_WINDOWS);
    ExecTestExpect("NE, target OS 2 (Windows) -> WOW", DosExeKind(g_File, sizeof g_File, &subsystem), DOS_EXE_NE);
    ExecTestMakeStub(EXEC_TEST_NEW_HEADER, "NE", DOS_NE_TARGET_OS, DOS_NE_OS_UNSPECIFIED);
    ExecTestExpect("NE, target OS 0 (Windows 1/2) -> WOW", DosExeKind(g_File, sizeof g_File, &subsystem), DOS_EXE_NE);
    ExecTestMakeStub(EXEC_TEST_NEW_HEADER, "NE", DOS_NE_TARGET_OS, EXEC_TEST_NE_OS2);
    ExecTestExpect("NE, target OS 1 (OS/2, BIND'ed) -> DOS: the stub IS the program",
                   DosExeKind(g_File, sizeof g_File, &subsystem), DOS_EXE_DOS);
    ExecTestMakeStub(EXEC_TEST_NEW_HEADER, "LE", EXEC_TEST_NO_EXTRA, 0);
    ExecTestExpect("LE (DOS/4GW-bound game) -> DOS", DosExeKind(g_File, sizeof g_File, &subsystem), DOS_EXE_DOS);
    ExecTestMakeStub(EXEC_TEST_NEW_HEADER, "LX", EXEC_TEST_NO_EXTRA, 0);
    ExecTestExpect("LX -> DOS", DosExeKind(g_File, sizeof g_File, &subsystem), DOS_EXE_DOS);
    ExecTestMakeStub(EXEC_TEST_LOW_NEW_HEADER, "PE\0\0", EXEC_TEST_NO_EXTRA, 0);
    ExecTestExpect("e_lfanew below 40h -> DOS (old .EXE, junk at 3Ch)",
                   DosExeKind(g_File, sizeof g_File, &subsystem), DOS_EXE_DOS);
    ExecTestMakeStub(EXEC_TEST_NEW_HEADER, "PE\0\0", EXEC_TEST_NO_EXTRA, 0);
    ExecTestExpect("e_lfanew past what was read -> DOS", DosExeKind(g_File, EXEC_TEST_SHORT_READ, &subsystem), DOS_EXE_DOS);
    ExecTestMakeStub(EXEC_TEST_NEW_HEADER, "PE\0\0", EXEC_TEST_NO_EXTRA, 0); g_File[EXEC_TEST_BROKEN_PE_NUL] = 'X';
    ExecTestExpect("\"PE\" without its two NULs -> DOS", DosExeKind(g_File, sizeof g_File, &subsystem), DOS_EXE_DOS);
    ExecTestMakeStub(EXEC_TEST_NEW_HEADER, "PE\0\0", EXEC_TEST_NO_EXTRA, 0); g_File[0] = 'Z'; g_File[1] = 'M';
    ExecTestExpect("not MZ -> DOS (a .COM)", DosExeKind(g_File, sizeof g_File, &subsystem), DOS_EXE_DOS);

    /* ── 2. how much memory (largest free block 8000h paragraphs) ── */
    length = ExecTestMakeChild(EXEC_TEST_MIN_100, EXEC_TEST_MAX_200);
    ExecTestExpect("image paras of p_exmemc's child: one page less the header", DosImageParagraphs(g_File, length), EXEC_TEST_CHILD_IMAGE_PARAS);
    status = DosExecSize(g_File, length, EXEC_TEST_LARGEST, &allocation, &loadHigh);
    ExecTestExpect("exmem.a.exec min 100h max 200h -> ok", status, DOS_MCB_SUCCESS);
    ExecTestExpect("exmem.a.child BX=022E: 10h + 1Eh + 200h", allocation, EXEC_TEST_ALLOC_A);
    ExecTestExpect("exmem.a not loaded high", loadHigh, FALSE);

    length = ExecTestMakeChild(EXEC_TEST_MIN_F000, EXEC_TEST_MAX_FFFF);
    status = DosExecSize(g_File, length, EXEC_TEST_LARGEST, &allocation, &loadHigh);
    ExecTestExpect("exmem.b.exec min F000h > largest -> 8", status, DOS_MCB_ERROR_INSUFFICIENT_MEMORY);

    length = ExecTestMakeChild(0, 0);
    status = DosExecSize(g_File, length, EXEC_TEST_LARGEST, &allocation, &loadHigh);
    ExecTestExpect("exmem.c.exec min = max = 0 -> ok", status, DOS_MCB_SUCCESS);
    ExecTestExpect("exmem.c load HIGH", loadHigh, TRUE);
    ExecTestExpect("exmem.c takes the whole block", allocation, EXEC_TEST_LARGEST);

    length = ExecTestMakeChild(0, EXEC_TEST_MAX_FFFF);
    status = DosExecSize(g_File, length, EXEC_TEST_LARGEST, &allocation, &loadHigh);
    ExecTestExpect("exmem.d max FFFFh -> the whole block", allocation, EXEC_TEST_LARGEST);
    ExecTestExpect("exmem.d not high", loadHigh, FALSE);

    length = ExecTestMakeChild(0, EXEC_TEST_MAX_10);
    status = DosExecSize(g_File, length, EXEC_TEST_LARGEST, &allocation, &loadHigh);
    ExecTestExpect("exmem.e.child BX=003E: 10h + 1Eh + 10h", allocation, EXEC_TEST_ALLOC_E);

    length = ExecTestMakeChild(EXEC_TEST_MIN_40, EXEC_TEST_MAX_10);
    DosExecSize(g_File, length, EXEC_TEST_LARGEST, &allocation, &loadHigh);
    ExecTestExpect("max below min -> min wins (10h + 1Eh + 40h)", allocation, EXEC_TEST_ALLOC_MIN_WINS);

    length = ExecTestMakeChild(0, EXEC_TEST_MAX_200);
    status = DosExecSize(g_File, length, EXEC_TEST_SMALL_BLOCK, &allocation, &loadHigh);
    ExecTestExpect("max beyond the block -> capped at the block", allocation, EXEC_TEST_SMALL_BLOCK);
    ExecTestExpect("...and that is not an error", status, DOS_MCB_SUCCESS);

    length = ExecTestMakeChild(EXEC_TEST_MIN_100, EXEC_TEST_MAX_200);
    status = DosExecSize(g_File, length, EXEC_TEST_EXACT_BLOCK, &allocation, &loadHigh);
    ExecTestExpect("min exactly fits (10h + 1Eh + 100h = 12Eh) -> ok", status, DOS_MCB_SUCCESS);
    status = DosExecSize(g_File, length, EXEC_TEST_SHORT_BLOCK, &allocation, &loadHigh);
    ExecTestExpect("one paragraph short -> 8", status, DOS_MCB_ERROR_INSUFFICIENT_MEMORY);

    g_File[0] = EXEC_TEST_COM_FIRST_BYTE;                                   /* a .COM */
    status = DosExecSize(g_File, EXEC_TEST_COM_SIZE, EXEC_TEST_LARGEST, &allocation, &loadHigh);
    ExecTestExpect(".COM takes the largest block", allocation, EXEC_TEST_LARGEST);

    printf("== %d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}
