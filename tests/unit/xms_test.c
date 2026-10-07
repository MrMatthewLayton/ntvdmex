/* xms_test.c -- off-VM unit battery for the XMS core (src/dos/dos_xms.h).
 *
 * Layer-1 test for M4 slice 1, in the same style as mcb_test.c: the XMS
 * allocator + Move logic run against host memory with malloc/free backing
 * hooks and a plain buffer standing in for the guest's conventional window --
 * no VM, no Windows. Exercises version/free-query, alloc/free with pool
 * accounting + handle exhaustion, realloc (grow/shrink/content-preserve),
 * lock/unlock (incl. free-while-locked), and the Move function across all
 * four endpoint combinations (conv<->EMB, EMB<->EMB) plus error paths.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dos_xms.h"

/* The pool the checks build, and the blocks they allocate in it, in KB. */
#define XMS_TEST_POOL_KB              1024     /* 1 MB                                  */
#define XMS_TEST_FIRST_BLOCK_KB       64
#define XMS_TEST_SECOND_BLOCK_KB      128
#define XMS_TEST_OVERSIZED_KB         2048
#define XMS_TEST_GROWN_KB             128
#define XMS_TEST_SHRUNK_KB            16
#define XMS_TEST_SECOND_HANDLE        2
#define XMS_TEST_THIRD_HANDLE         3
#define XMS_TEST_UNKNOWN_HANDLE       99
#define XMS_TEST_EXTRA_ATTEMPTS       4        /* tries past the handle limit          */

/* The Move checks: a conventional window, the far pointers into it, and what moves. */
/* ⚠ It was 128KB, and 2000:0000 below is linear 20000h -- exactly 128KB -- so the
   round trip wrote 16 bytes past the window (AddressSanitizer: global-buffer-overflow).
   Harmless only while whatever followed the array was unused. */
#define XMS_TEST_CONVENTIONAL_SIZE    0x30000  /* 192KB conventional window            */
#define XMS_TEST_SOURCE_SEGMENT       0x1000u  /* conv far ptr 1000:0010               */
#define XMS_TEST_SOURCE_OFFSET        0x0010u
#define XMS_TEST_DESTINATION_SEGMENT  0x2000u  /* conv far ptr 2000:0000               */
#define XMS_TEST_DESTINATION_OFFSET   0x0000u
#define XMS_TEST_MESSAGE              "XMS round trip!"
#define XMS_TEST_MESSAGE_SIZE         16       /* the message and its terminator       */
#define XMS_TEST_EMB_MIDDLE           1024     /* into the middle of the second block  */
#define XMS_TEST_ODD_LENGTH           15
#define XMS_TEST_WRAP_LENGTH          0x2000
#define XMS_TEST_WRAPPING_OFFSET      0xFFFFF000u  /* near 4 GB: wraps past the bounds check */
#define XMS_TEST_LAST_FAR_SEGMENT     0xFFFF0000u  /* FFFF:FFF0                         */
#define XMS_TEST_LAST_FAR_OFFSET      0xFFF0u
#define XMS_TEST_PAST_HMA_LENGTH      0x20
#define XMS_TEST_FIRST_MARK           0xAB
#define XMS_TEST_LAST_MARK            0xCD

static INT g_Checks = 0, g_Failures = 0;

static VOID XmsTestCheck(BOOL passed, PCSTR description)
{
    g_Checks++;
    if (passed) { printf("  PASS  %s\n", description); }
    else { printf("  FAIL  %s\n", description); g_Failures++; }
}

/* Backing hooks: real host heap, KB-sized. */
static PVOID XmsTestAllocate(PVOID context, DWORD kilobytes)
{
    (VOID)context;
    return calloc((SIZE_T)kilobytes, DOS_XMS_BYTES_PER_KB);
}

static VOID XmsTestFree(PVOID context, PVOID memory, DWORD kilobytes)
{
    (VOID)context; (VOID)kilobytes;
    free(memory);
}

/* A far pointer as a Move offset: segment in the high word, offset in the low. */
static DWORD XmsTestFarPointer(DWORD segment, DWORD offset)
{
    return (segment << DOS_XMS_FAR_SEGMENT_SHIFT) | offset;
}

INT main(VOID)
{
    DOS_XMS_STATE state;
    WORD firstHandle, secondHandle, thirdHandle;
    BYTE errorCode, lockCount, freeHandles;
    DWORD largestKb, totalFreeKb, sizeKb, firstLinear, secondLinear;
    INT attempt;
    BOOL succeeded;

    printf("== M4 XMS core battery ==\n");

    /* A 1 MB pool (1024 KB). */
    DosXmsInitialize(&state, XMS_TEST_POOL_KB, XmsTestAllocate, XmsTestFree, NULL);

    /* T1: fresh state ----------------------------------------------------- */
    /* ── A20 STARTS OPEN, AND THIS CHECK USED TO PIN THE LIE. ─────────────────
         It asserted IsA20Enabled == 0, i.e. AH=07h answers "A20 disabled" until
         some guest happens to call AH=03h. An NT VDM does not wrap at 1 MB, so the
         line is effectively always open, and extended memory is unreachable
         while it is reported masked.
       Oracle, tests/probes/dos/p_xms.asm on 6.22 with HIMEM.SYS:
         CASE=xms.07.query.a20 SIG=AX,BX AX=0001    (enabled)
       Ours answered AX=0000. (GH #47) */
    XmsTestCheck(state.IsA20Enabled == TRUE && state.IsHmaAllocated == FALSE,
                 "init: A20 open (as the VDM really is), HMA free");
    DosXmsQueryFreeMemory(&state, &largestKb, &totalFreeKb);
    XmsTestCheck(largestKb == XMS_TEST_POOL_KB && totalFreeKb == XMS_TEST_POOL_KB,
                 "fn08: whole pool free initially");

    /* T2: allocate 64 KB -------------------------------------------------- */
    errorCode = 0;
    succeeded = DosXmsAllocate(&state, XMS_TEST_FIRST_BLOCK_KB, &firstHandle, &errorCode);
    XmsTestCheck(succeeded && firstHandle == DOS_XMS_FIRST_HANDLE, "fn09: alloc 64KB -> handle 1");
    DosXmsQueryFreeMemory(&state, &largestKb, &totalFreeKb);
    XmsTestCheck(largestKb == XMS_TEST_POOL_KB - XMS_TEST_FIRST_BLOCK_KB
                 && totalFreeKb == XMS_TEST_POOL_KB - XMS_TEST_FIRST_BLOCK_KB,
                 "fn08: 960KB free after 64KB alloc");

    /* T3: handle info ----------------------------------------------------- */
    succeeded = DosXmsGetHandleInformation(&state, firstHandle, &lockCount, &freeHandles, &sizeKb,
                                           &errorCode);
    XmsTestCheck(succeeded && sizeKb == XMS_TEST_FIRST_BLOCK_KB && lockCount == 0,
                 "fn0E: handle 1 is 64KB, unlocked");
    XmsTestCheck(freeHandles == DOS_XMS_MAX_HANDLES - 1,
                 "fn0E: free-handle count reflects 1 in use");

    /* T4: out-of-pool allocation fails with A0 ---------------------------- */
    succeeded = DosXmsAllocate(&state, XMS_TEST_OVERSIZED_KB, &secondHandle, &errorCode);
    XmsTestCheck(!succeeded && errorCode == DOS_XMS_ERROR_OUT_OF_MEMORY,
                 "fn09: over-pool alloc fails (A0)");

    /* T5: a second, smaller block, plus a 0-KB block ---------------------- */
    succeeded = DosXmsAllocate(&state, XMS_TEST_SECOND_BLOCK_KB, &secondHandle, &errorCode);
    XmsTestCheck(succeeded && secondHandle == XMS_TEST_SECOND_HANDLE,
                 "fn09: alloc 128KB -> handle 2");
    succeeded = DosXmsAllocate(&state, 0, &thirdHandle, &errorCode);
    XmsTestCheck(succeeded && thirdHandle == XMS_TEST_THIRD_HANDLE,
                 "fn09: alloc 0KB -> legal empty handle 3");
    DosXmsQueryFreeMemory(&state, &largestKb, &totalFreeKb);
    XmsTestCheck(totalFreeKb
                     == XMS_TEST_POOL_KB - XMS_TEST_FIRST_BLOCK_KB - XMS_TEST_SECOND_BLOCK_KB,
                 "fn08: pool accounting after 3 allocs");

    /* T6: Move conventional -> EMB, then EMB -> conventional (round trip) -- */
    {
        static BYTE conventional[XMS_TEST_CONVENTIONAL_SIZE];  /* the conventional window */
        DOS_XMS_MOVE move;
        PCSTR message = XMS_TEST_MESSAGE;
        PBYTE sourceBytes = conventional + (XMS_TEST_SOURCE_SEGMENT << DOS_XMS_PARAGRAPH_SHIFT)
                          + XMS_TEST_SOURCE_OFFSET;
        memset(conventional, 0, sizeof conventional);
        memcpy(sourceBytes, message, XMS_TEST_MESSAGE_SIZE);

        /* conv 1000:0010  ->  handle 1 offset 0 */
        move.Length = XMS_TEST_MESSAGE_SIZE; move.SourceHandle = DOS_XMS_CONVENTIONAL_HANDLE;
        move.SourceOffset = XmsTestFarPointer(XMS_TEST_SOURCE_SEGMENT, XMS_TEST_SOURCE_OFFSET);
        move.DestinationHandle = firstHandle; move.DestinationOffset = 0;
        succeeded = DosXmsMove(&state, conventional, &move, &errorCode);
        XmsTestCheck(succeeded, "fn0B: move conv -> EMB");
        XmsTestCheck(memcmp(state.Handles[firstHandle - DOS_XMS_FIRST_HANDLE].Memory, message,
                            XMS_TEST_MESSAGE_SIZE) == 0,
                     "fn0B: EMB holds the moved bytes");

        /* handle 1 offset 0  ->  conv 2000:0000 */
        memset(conventional, 0, sizeof conventional);
        move.Length = XMS_TEST_MESSAGE_SIZE; move.SourceHandle = firstHandle; move.SourceOffset = 0;
        move.DestinationHandle = DOS_XMS_CONVENTIONAL_HANDLE;
        move.DestinationOffset = XmsTestFarPointer(XMS_TEST_DESTINATION_SEGMENT,
                                                   XMS_TEST_DESTINATION_OFFSET);
        succeeded = DosXmsMove(&state, conventional, &move, &errorCode);
        XmsTestCheck(succeeded
                     && memcmp(conventional
                                   + (XMS_TEST_DESTINATION_SEGMENT << DOS_XMS_PARAGRAPH_SHIFT),
                               message, XMS_TEST_MESSAGE_SIZE) == 0,
                     "fn0B: move EMB -> conv (round trip)");
    }

    /* T7: Move EMB -> EMB ------------------------------------------------- */
    {
        DOS_XMS_MOVE move;
        move.Length = XMS_TEST_MESSAGE_SIZE; move.SourceHandle = firstHandle; move.SourceOffset = 0;
        move.DestinationHandle = secondHandle;
        move.DestinationOffset = XMS_TEST_EMB_MIDDLE;   /* into the middle of secondHandle */
        succeeded = DosXmsMove(&state, NULL, &move, &errorCode);
        XmsTestCheck(succeeded
                     && memcmp((PBYTE)state.Handles[secondHandle - DOS_XMS_FIRST_HANDLE].Memory
                                   + XMS_TEST_EMB_MIDDLE,
                               state.Handles[firstHandle - DOS_XMS_FIRST_HANDLE].Memory,
                               XMS_TEST_MESSAGE_SIZE) == 0,
                     "fn0B: move EMB -> EMB");
    }

    /* T8: Move error paths ------------------------------------------------ */
    {
        DOS_XMS_MOVE move;
        /* s84: an offset near 4 GB used to WRAP past the bounds check and a conventional
           endpoint had no ceiling -- both reached host memory outside the guest's. */
        move.Length = XMS_TEST_WRAP_LENGTH; move.SourceHandle = firstHandle;
        move.SourceOffset = XMS_TEST_WRAPPING_OFFSET;
        move.DestinationHandle = secondHandle; move.DestinationOffset = 0;
        XmsTestCheck(!DosXmsMove(&state, NULL, &move, &errorCode)
                     && errorCode == DOS_XMS_ERROR_INVALID_SOURCE_OFFSET,
                     "fn0B: 32-bit wrap of src offset refused (A4)");
        move.SourceOffset = 0; move.DestinationOffset = XMS_TEST_WRAPPING_OFFSET;
        XmsTestCheck(!DosXmsMove(&state, NULL, &move, &errorCode)
                     && errorCode == DOS_XMS_ERROR_INVALID_DESTINATION_OFFSET,
                     "fn0B: 32-bit wrap of dst offset refused (A6)");
        move.Length = XMS_TEST_PAST_HMA_LENGTH; move.SourceHandle = DOS_XMS_CONVENTIONAL_HANDLE;
        move.SourceOffset = XMS_TEST_LAST_FAR_SEGMENT | XMS_TEST_LAST_FAR_OFFSET;  /* FFFF:FFF0 */
        move.DestinationHandle = secondHandle; move.DestinationOffset = 0;
        XmsTestCheck(!DosXmsMove(&state, NULL, &move, &errorCode)
                     && errorCode == DOS_XMS_ERROR_INVALID_SOURCE_OFFSET,
                     "fn0B: conventional source past FFFF:FFFF refused");
        move.Length = XMS_TEST_ODD_LENGTH; move.SourceHandle = firstHandle; move.SourceOffset = 0;
        move.DestinationHandle = secondHandle; move.DestinationOffset = 0;
        XmsTestCheck(!DosXmsMove(&state, NULL, &move, &errorCode)
                     && errorCode == DOS_XMS_ERROR_INVALID_LENGTH,
                     "fn0B: odd length rejected (A7)");
        move.Length = XMS_TEST_MESSAGE_SIZE; move.SourceHandle = XMS_TEST_UNKNOWN_HANDLE;
        XmsTestCheck(!DosXmsMove(&state, NULL, &move, &errorCode)
                     && errorCode == DOS_XMS_ERROR_INVALID_SOURCE_HANDLE,
                     "fn0B: bad source handle (A3)");
        move.SourceHandle = firstHandle;
        /* past end of 64KB block */
        move.SourceOffset = XMS_TEST_FIRST_BLOCK_KB * DOS_XMS_BYTES_PER_KB;
        XmsTestCheck(!DosXmsMove(&state, NULL, &move, &errorCode)
                     && errorCode == DOS_XMS_ERROR_INVALID_SOURCE_OFFSET,
                     "fn0B: source offset past end (A4)");
        move.Length = 0; move.SourceHandle = firstHandle; move.SourceOffset = 0;
        XmsTestCheck(DosXmsMove(&state, NULL, &move, &errorCode),
                     "fn0B: zero-length move is a no-op (ok)");
    }

    /* T9: lock / unlock + free-while-locked ------------------------------- */
    succeeded = DosXmsLock(&state, firstHandle, &firstLinear, &errorCode);
    XmsTestCheck(succeeded && firstLinear != 0, "fn0C: lock handle 1 -> nonzero linear addr");
    succeeded = DosXmsLock(&state, secondHandle, &secondLinear, &errorCode);
    XmsTestCheck(succeeded && secondLinear != firstLinear,
                 "fn0C: distinct blocks lock to distinct addrs");
    XmsTestCheck(!DosXmsFree(&state, firstHandle, &errorCode) && errorCode == DOS_XMS_ERROR_LOCKED,
                 "fn0A: free-while-locked rejected (AB)");
    succeeded = DosXmsGetHandleInformation(&state, firstHandle, &lockCount, &freeHandles, &sizeKb,
                                           &errorCode);
    XmsTestCheck(succeeded && lockCount == 1, "fn0E: lock count == 1");
    XmsTestCheck(DosXmsUnlock(&state, firstHandle, &errorCode), "fn0D: unlock handle 1");
    XmsTestCheck(!DosXmsUnlock(&state, firstHandle, &errorCode)
                 && errorCode == DOS_XMS_ERROR_NOT_LOCKED, "fn0D: over-unlock rejected (AA)");

    /* T10: realloc grow (preserve) + shrink ------------------------------- */
    {
        DWORD lastByte = XMS_TEST_FIRST_BLOCK_KB * DOS_XMS_BYTES_PER_KB - 1;
        PBYTE memory = (PBYTE)state.Handles[firstHandle - DOS_XMS_FIRST_HANDLE].Memory;
        PDOS_XMS_HANDLE firstEntry = &state.Handles[firstHandle - DOS_XMS_FIRST_HANDLE];
        /* mark first+last byte, then grow 64 -> 128 KB */
        memory[0] = XMS_TEST_FIRST_MARK; memory[lastByte] = XMS_TEST_LAST_MARK;
        succeeded = DosXmsReallocate(&state, firstHandle, XMS_TEST_GROWN_KB, &errorCode);
        XmsTestCheck(succeeded && firstEntry->SizeKb == XMS_TEST_GROWN_KB,
                     "fn0F: grow 64->128KB");
        memory = (PBYTE)state.Handles[firstHandle - DOS_XMS_FIRST_HANDLE].Memory;
        XmsTestCheck(memory[0] == XMS_TEST_FIRST_MARK && memory[lastByte] == XMS_TEST_LAST_MARK,
                     "fn0F: grow preserves old content");
        /* shrink to 16 KB */
        succeeded = DosXmsReallocate(&state, firstHandle, XMS_TEST_SHRUNK_KB, &errorCode);
        XmsTestCheck(succeeded && firstEntry->SizeKb == XMS_TEST_SHRUNK_KB,
                     "fn0F: shrink 128->16KB");
        DosXmsQueryFreeMemory(&state, &largestKb, &totalFreeKb);
        XmsTestCheck(totalFreeKb
                         == XMS_TEST_POOL_KB - XMS_TEST_SHRUNK_KB - XMS_TEST_SECOND_BLOCK_KB,
                     "fn0F: pool accounting after realloc");
    }

    /* T11: free both, pool fully restored --------------------------------- */
    XmsTestCheck(DosXmsUnlock(&state, secondHandle, &errorCode),
                 "fn0D: unlock handle 2 (locked back in T9)");
    XmsTestCheck(DosXmsFree(&state, firstHandle, &errorCode), "fn0A: free handle 1");
    XmsTestCheck(DosXmsFree(&state, secondHandle, &errorCode), "fn0A: free handle 2");
    XmsTestCheck(DosXmsFree(&state, thirdHandle, &errorCode),
                 "fn0A: free handle 3 (the 0KB block)");
    XmsTestCheck(!DosXmsFree(&state, firstHandle, &errorCode)
                 && errorCode == DOS_XMS_ERROR_INVALID_HANDLE, "fn0A: double-free rejected (A2)");
    DosXmsQueryFreeMemory(&state, &largestKb, &totalFreeKb);
    XmsTestCheck(totalFreeKb == XMS_TEST_POOL_KB, "fn08: whole pool free again after frees");

    /* T12: handle exhaustion --------------------------------------------- */
    {
        INT allocatedCount = 0;
        for (attempt = 0; attempt < DOS_XMS_MAX_HANDLES + XMS_TEST_EXTRA_ATTEMPTS; ++attempt) {
            WORD handle;
            if (DosXmsAllocate(&state, 0, &handle, &errorCode)) ++allocatedCount;
            else {
                XmsTestCheck(errorCode == DOS_XMS_ERROR_NO_HANDLES,
                             "fn09: exhausting handles fails (A1)");
                break;
            }
        }
        XmsTestCheck(allocatedCount == DOS_XMS_MAX_HANDLES,
                     "fn09: exactly XMS_MAX_HANDLES 0KB blocks allocatable");
    }

    printf("\n%d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}
