/* types_test.c -- src/ntvdmex_types.h gives the Windows types the Windows widths (#333).
 *
 * The header already refuses to compile on a wrong width; this test states the same facts at
 * run time, adds signedness (which a size check cannot see), and pins the byte/word macros the
 * device models use on every register access.
 */
#include <stdio.h>
#include "ntvdmex_types.h"

#define TYPES_TEST_SIGNED_MINUS_ONE (-1)
#define TYPES_TEST_WORD_VALUE       0x1234
#define TYPES_TEST_DWORD_VALUE      0x89ABCDEFu

static INT g_Total = 0, g_Failures = 0;

static VOID TypesTestCheck(BOOL condition, PCSTR description)
{
    g_Total++;
    if (condition) {
        printf("  PASS  %s\n", description);
    } else {
        printf("  FAIL  %s\n", description);
        g_Failures++;
    }
}

INT main(VOID)
{
    INT8  signedByte  = (INT8)TYPES_TEST_SIGNED_MINUS_ONE;
    INT32 signedDword = (INT32)TYPES_TEST_SIGNED_MINUS_ONE;
    LONG  signedLong  = (LONG)TYPES_TEST_SIGNED_MINUS_ONE;

    printf("== ntvdmex_types.h: the Windows widths ==\n");
    TypesTestCheck(sizeof(BYTE) == 1 && sizeof(WORD) == 2 && sizeof(DWORD) == 4,
                   "BYTE / WORD / DWORD are 8 / 16 / 32 bits");
    TypesTestCheck(sizeof(INT8) == 1 && sizeof(INT16) == 2 && sizeof(INT32) == 4
                   && sizeof(INT64) == 8 && sizeof(UINT64) == 8,
                   "INT8..INT64 and UINT64 are 8..64 bits");
    TypesTestCheck(sizeof(LONG) == 4 && sizeof(ULONG) == 4,
                   "LONG / ULONG are 32 bits -- not unsigned long's 64 on LP64");
    TypesTestCheck(sizeof(BOOL) == 4 && sizeof(INT) == 4 && sizeof(UINT) == 4,
                   "BOOL / INT / UINT are 32 bits");
    TypesTestCheck(sizeof(PVOID) == sizeof(SIZE_T), "SIZE_T spans a pointer");

    printf("== signedness ==\n");
    TypesTestCheck(signedByte < 0, "INT8 is signed (CHAR's signedness is the compiler's)");
    TypesTestCheck(signedDword < 0 && signedLong < 0, "INT32 and LONG are signed");
    TypesTestCheck((DWORD)TYPES_TEST_SIGNED_MINUS_ONE > 0, "DWORD is unsigned");

    printf("== byte and word macros ==\n");
    TypesTestCheck(LOBYTE(TYPES_TEST_WORD_VALUE) == 0x34 && HIBYTE(TYPES_TEST_WORD_VALUE) == 0x12,
                   "LOBYTE / HIBYTE split 1234h into 34h / 12h");
    TypesTestCheck(LOWORD(TYPES_TEST_DWORD_VALUE) == 0xCDEF && HIWORD(TYPES_TEST_DWORD_VALUE) == 0x89AB,
                   "LOWORD / HIWORD split 89ABCDEFh into CDEFh / 89ABh");
    TypesTestCheck(MAKEWORD(0x34, 0x12) == TYPES_TEST_WORD_VALUE, "MAKEWORD(34h, 12h) = 1234h");
    TypesTestCheck((DWORD)MAKELONG(0xCDEF, 0x89AB) == TYPES_TEST_DWORD_VALUE,
                   "MAKELONG(CDEFh, 89ABh) = 89ABCDEFh");

    printf("\n%d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
